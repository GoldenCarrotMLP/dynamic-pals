#define NOMINMAX 
#include "HooksManager.hpp"
#include "PalProcessor.hpp"
#include "SaveManager.hpp"
#include "UIManager.hpp"
#include "NotificationManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "Updater.hpp"
#include "VFXManager.hpp"
#include <fstream>

#include <Unreal/CoreUObject/UObject/Class.hpp> 
#include <Unreal/UObjectGlobals.hpp>
#include <chrono>
#include <thread>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    static bool bCompletedInitReady = false;
    static UObject* LastPlayerController = nullptr;
    static UObject* LastWorld = nullptr;
    static bool bIsAtMenu = false; 

    void HooksManager::OnPalSpawnedReady(UnrealScriptFunctionCallableContext& Context, void*) {
        if (!bCompletedInitReady) return;

        UObject* PalNPC = Context.Context;
        if (PalNPC) {
            PalProcessor::Get().ProcessPal(PalNPC, false);
        }
    }

    static std::wstring GetFormattedVersionString() {
        HMODULE hModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&GetFormattedVersionString, &hModule);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(hModule, path, MAX_PATH);
        std::wstring currentDllPath(path);
        std::wstring dllDir = currentDllPath.substr(0, currentDllPath.find_last_of(L"\\/") + 1);
        std::wstring versionTxtPath = dllDir + L"version.txt";

        std::ifstream file(versionTxtPath);
        if (!file.is_open()) {
            return L"v0.0.56"; 
        }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        
        content.erase(0, content.find_first_not_of(" \t\r\n"));
        size_t last = content.find_last_not_of(" \t\r\n");
        if (last != std::string::npos) {
            content.erase(last + 1);
        }

        if (content.empty()) {
            return L"v0.0.56";
        }

        try {
            int versionNum = std::stoi(content);
            int major = versionNum / 1000;
            int minor = (versionNum / 100) % 10;
            int patch = versionNum % 100;
            wchar_t buf[64];
            swprintf(buf, 64, L"v%d.%d.%02d", major, minor, patch);
            return std::wstring(buf);
        } catch (...) {
            std::wstring rawVersion;
            rawVersion.assign(content.begin(), content.end());
            return L"v" + rawVersion;
        }
    }

    // --- LIVE STAT CHANGE DETECTOR ---
    static void OnPalStatChanged(UnrealScriptFunctionCallableContext& Context, void*) {
        if (!bCompletedInitReady) return; 

        UObject* ContextObj = Context.Context;
        if (!ContextObj) return;

        UObject* PalActor = nullptr;
        std::wstring ClassName = ContextObj->GetClassPrivate()->GetName();

        // 1. If context is a Pal Character
        if (ClassName.find(L"PalCharacter") != std::wstring::npos || ClassName.find(L"Monster") != std::wstring::npos || ClassName.find(L"Player") != std::wstring::npos) {
            PalActor = ContextObj;
        }
        // 2. O(1) Reverse-lookup up the Actor ownership hierarchy!
        else if (ClassName == L"PalIndividualCharacterParameter") {
            UObject* OuterComp = ContextObj->GetOuterPrivate();
            if (OuterComp && OuterComp->GetClassPrivate()->GetName().find(L"PalCharacterParameterComponent") != std::wstring::npos) {
                UObject* Actor = OuterComp->GetOuterPrivate();
                if (Actor && Actor->GetClassPrivate()->GetName().find(L"PalCharacter") != std::wstring::npos) {
                    PalActor = Actor; // Success!
                } else {
                    DP_LOG(Warning, "O(1) Lookup Failed: Outer of PalCharacterParameterComponent is not a PalCharacter!\n");
                }
            } else {
                DP_LOG(Warning, "O(1) Lookup Failed: Outer of PalIndividualCharacterParameter is not a PalCharacterParameterComponent!\n");
            }

            // 3. FALLBACK: If the O(1) lookup failed due to unexpected hierarchy, log and use the safe O(N) method
            if (!PalActor) {
                DP_LOG(Normal, "Falling back to slow O(N) iteration to find PalActor for stat change...\n");
                
                std::vector<UObject*> AllPals;
                UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
                for (UObject* Pal : AllPals) {
                    if (!Pal) continue;
                    UObject* ParamComp = nullptr;
                    Utils::GetPropertyValue<UObject*>(Pal, STR("CharacterParameterComponent"), ParamComp);
                    if (ParamComp) {
                        UObject* IndivParamCheck = nullptr;
                        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParamCheck);
                        if (IndivParamCheck == ContextObj) {
                            PalActor = Pal;
                            break;
                        }
                    }
                }
            }
        }

        if (PalActor) {
            PalProcessor::Get().ProcessPal(PalActor, false);
        }
    }

    static void OnStartedWorldAutoSave(UnrealScriptFunctionCallableContext&, void*) {
        DP_LOG(Default, "Auto-Save triggered! Synchronizing world persistence...\n");
        SaveManager::Get().SaveWorldData();
    }

    static void OnGameThreadTick(UnrealScriptFunctionCallableContext& Context, void*) {
        static bool bIsReentrant = false;
        if (bIsReentrant) return;
        bIsReentrant = true;

        VFXManager::Get().Tick();

        // Let the lightweight background scanner poll periodically for missed updates!
        if (bCompletedInitReady) {
            PalProcessor::Get().ScanActivePals();
        }

        if (!UIManager::Get().IsMenuOpen()) 
        {
            bIsReentrant = false;
            return;
        }

        static auto LastTickTime = std::chrono::steady_clock::now();
        auto Now = std::chrono::steady_clock::now();
        
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastTickTime).count() >= 16) {
            LastTickTime = Now;
            
            if (UIManager::Get().IsMenuOpen()) {
                UObject* ActorContext = Context.Context;
                if (ActorContext) {
                    UObject* Level = ActorContext->GetOuterPrivate();
                    UObject* World = Level ? Level->GetOuterPrivate() : nullptr;

                    if (World && World == LastWorld) {
                        UObject* PlayerController = nullptr;
                        UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
                        if (GameplayStatics) {
                            struct { UObject* WorldContextObject; int32_t PlayerIndex; UObject* ReturnValue; } GSParams{ActorContext, 0, nullptr};
                            Utils::CallFunction(GameplayStatics, STR("GetPlayerController"), &GSParams);
                            PlayerController = GSParams.ReturnValue;
                        }
                        if (PlayerController) {
                            UIManager::Get().TickUI(PlayerController);
                        }
                    }
                }
            }
        }

        bIsReentrant = false;
    }

    static void OnWidgetAddedToViewport(UnrealScriptFunctionCallableContext& Context, void*) {
        if (bIsAtMenu) return; 

        UObject* Widget = Context.Context;
        if (!Widget) return;

        UClass* WidgetClass = Widget->GetClassPrivate();
        if (!WidgetClass) return;

        std::wstring WidgetName = WidgetClass->GetName();
        
        if (WidgetName.find(L"WBP_Title") != std::wstring::npos || 
            WidgetName.find(L"WBP_Login") != std::wstring::npos) 
        {
            bIsAtMenu = true;
            bCompletedInitReady = false;
            
            SaveManager::Get().Reset();
            NotificationManager::Get().SetReady(false); 
            PalProcessor::Get().ClearAllSwappedStatus();
            
            DP_LOG(Default, "Transitioned to Main Menu (Detected via '{}'). Mod entering standby mode...\n", WidgetName.c_str());

            std::thread([]() {
                Updater::CheckForUpdates();
            }).detach();
        }
    }

    static void OnOpenLevel(UnrealScriptFunctionCallableContext& Context, void*) {
        bIsAtMenu = false;
        bCompletedInitReady = false;
        NotificationManager::Get().SetReady(false); 
    }

    static void OnClientRestart(UnrealScriptFunctionCallableContext& Context, void*) {
        UObject* PlayerController = Context.Context;
        if (!PlayerController) return;

        UObject* Level = PlayerController->GetOuterPrivate();
        UObject* CurrentWorld = Level ? Level->GetOuterPrivate() : nullptr;

        if (PlayerController != LastPlayerController || CurrentWorld != LastWorld) {
            LastPlayerController = PlayerController;
            LastWorld = CurrentWorld;

            UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            if (GameplayStatics) {
                struct { UObject* WorldContextObject; bool bRemovePrefixString; FString ReturnValue; } Params{PlayerController, true, FString()};
                Utils::CallFunction(GameplayStatics, STR("GetCurrentLevelName"), &Params);
                std::wstring MapName = Utils::FStringToWString(Params.ReturnValue);

                bool bIsMenu = (MapName.find(L"Title") != std::wstring::npos || 
                                MapName.find(L"Login") != std::wstring::npos ||
                                MapName.empty());

                if (bIsMenu) {
                    bCompletedInitReady = false;
                    NotificationManager::Get().SetReady(false);
                    SaveManager::Get().Reset();
                    PalProcessor::Get().ClearAllSwappedStatus();
                } else {
                    bIsAtMenu = false; 
                    bCompletedInitReady = false;
                    
                    NotificationManager::Get().SetReady(false);
                    SaveManager::Get().Reset();
                    PalProcessor::Get().ClearAllSwappedStatus();

                    DP_LOG(Default, "New Session Detected (Map: '{}'). Spawning surge quarantine active. Pausing swaps for 5 seconds...\n", MapName);

                    std::thread([]() {
                        std::this_thread::sleep_for(std::chrono::seconds(8));

                        AsyncHelper::AsyncTask(ENamedThreads::GameThread, []() {
                            DP_LOG(Default, "Settle period complete. Running overworld Pal reconciliation...\n");

                            std::vector<UObject*> AllPals;
                            UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
                            for (UObject* Pal : AllPals) {
                                if (Pal) {
                                    PalProcessor::Get().ProcessPal(Pal, false);
                                }
                            }

                            bCompletedInitReady = true;
                            DP_LOG(Default, "Reconciliation complete. Mod entering zero-overhead standby.\n");

                            std::wstring verStr = GetFormattedVersionString();
                            DP_LOG(Normal, "Welcome to dynamic pals {}", verStr);

                            NotificationManager::Get().FlushQueuedToasts();
                        });
                    }).detach();
                }
            }
        }
    }

    void HooksManager::RegisterHooks() {
        UFunction* InitFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalNPC:OnCompletedInitParam"));
        if (InitFunc) {
            InitFunc->RegisterPostHook(OnPalSpawnedReady, nullptr);
            DP_LOG(Default, "Successfully hooked OnCompletedInitParam (Native Pipeline Active!)\n");
        }

        UFunction* RestartFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.PlayerController:ClientRestart"));
        if (RestartFunc) {
            RestartFunc->RegisterPostHook(OnClientRestart, nullptr);
            DP_LOG(Default, "Successfully hooked ClientRestart for map transitions.\n");
        }

        UFunction* ActorRotFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Actor:K2_GetActorRotation"));
        if (ActorRotFunc) {
            ActorRotFunc->RegisterPreHook(OnGameThreadTick, nullptr);
            DP_LOG(Default, "Successfully hooked K2_GetActorRotation on the Game Thread.\n");
        }

        UFunction* SaveFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalSaveGameManager:StartWorldDataAutoSave"));
        if (SaveFunc) {
            SaveFunc->RegisterPostHook(OnStartedWorldAutoSave, nullptr);
        }

        // 5. LIVE STAT UPDATE HOOKS 
        const wchar_t* StatHooks[] = {
            // Blueprint bound events from the base monster class (Found via your JSON dump!)
            STR("/Game/Pal/Blueprint/Character/Monster/BP_MonsterBase.BP_MonsterBase_C:OnUpdateLevelDelegate_イベント_0"),
            // The native PalIndividualCharacterParameter setter functions (we provide a few common names to be robust)
            STR("/Script/Pal.PalIndividualCharacterParameter:AddPassiveSkill"),
            STR("/Script/Pal.PalIndividualCharacterParameter:RemovePassiveSkill"),
            STR("/Script/Pal.PalIndividualCharacterParameter:AddRank"),
            STR("/Script/Pal.PalIndividualCharacterParameter:SetRank"),
            STR("/Script/Pal.PalIndividualCharacterParameter:AddFriendshipPoint"),
            STR("/Script/Pal.PalIndividualCharacterParameter:SetFriendshipPoint"),
            STR("/Script/Pal.PalIndividualCharacterParameter:AddLevel"),
            STR("/Script/Pal.PalIndividualCharacterParameter:SetLevel")
        };

        for (const wchar_t* HookName : StatHooks) {
            UFunction* TargetFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, HookName);
            if (TargetFunc) {
                TargetFunc->RegisterPostHook(OnPalStatChanged, nullptr);
            }
        }

        UFunction* AddToViewportFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToViewport"));
        if (AddToViewportFunc) {
            AddToViewportFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
        }
        
        UFunction* AddToPlayerScreenFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToPlayerScreen"));
        if (AddToPlayerScreenFunc) {
            AddToPlayerScreenFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
        }

        UFunction* OpenLevelFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics:OpenLevel"));
        if (OpenLevelFunc) {
            OpenLevelFunc->RegisterPreHook(OnOpenLevel, nullptr);
        }
    }
}