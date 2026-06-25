#define NOMINMAX 
#include "HooksManager.hpp"
#include "PalProcessor.hpp"
#include "SaveManager.hpp"
#include "UIManager.hpp"
#include "NotificationManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "Updater.hpp"
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
    static bool bIsAtMenu = false; // Tracks whether the game state is currently sitting in the main menu

    void HooksManager::OnPalSpawnedReady(UnrealScriptFunctionCallableContext& Context, void*) {
        if (!bCompletedInitReady) return;

        UObject* PalNPC = Context.Context;
        if (PalNPC) {
            PalProcessor::Get().ProcessPal(PalNPC, false);
        }
    }

    // Helper function to safely read version.txt and format it like v0.0.56
static std::wstring GetFormattedVersionString() {
    HMODULE hModule = NULL;
    // Get the handle of the module containing this function [1]
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&GetFormattedVersionString, &hModule);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(hModule, path, MAX_PATH);
    std::wstring currentDllPath(path);
    std::wstring dllDir = currentDllPath.substr(0, currentDllPath.find_last_of(L"\\/") + 1);
    std::wstring versionTxtPath = dllDir + L"version.txt";

    std::ifstream file(versionTxtPath);
    if (!file.is_open()) {
        return L"v0.0.56"; // Default/Fallback if the file doesn't exist yet [1]
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    
    // Trim whitespaces and newlines
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
        // Decimal-coded version mapping: 56 -> v0.0.56, 102 -> v0.1.02, 1234 -> v1.2.34
        int major = versionNum / 1000;
        int minor = (versionNum / 100) % 10;
        int patch = versionNum % 100;
        wchar_t buf[64];
        swprintf(buf, 64, L"v%d.%d.%02d", major, minor, patch);
        return std::wstring(buf);
    } catch (...) {
        // Safe string fallback in case of abnormal formatting in version.txt
        std::wstring rawVersion;
        rawVersion.assign(content.begin(), content.end());
        return L"v" + rawVersion;
    }
}

    // --- LIVE STAT CHANGE DETECTOR ---
    static void OnPalStatChanged(UnrealScriptFunctionCallableContext& Context, void*) {
        if (!bCompletedInitReady) return; 

        UObject* IndivParam = Context.Context;
        if (!IndivParam) return;

        UObject* PalActor = nullptr;
        
        struct { UObject* ReturnValue; } GetActorParams{nullptr};
        Utils::CallFunction(IndivParam, STR("GetIndividualActor"), &GetActorParams);
        PalActor = GetActorParams.ReturnValue;

        if (!PalActor) {
            Utils::GetPropertyValue<UObject*>(IndivParam, STR("IndividualActor"), PalActor);
        }

        if (PalActor) {
            PalProcessor::Get().ProcessPal(PalActor, false);
        }
    }

    static void OnStartedWorldAutoSave(UnrealScriptFunctionCallableContext&, void*) {
        DP_LOG(Default, "Auto-Save triggered! Synchronizing world persistence...\n");
        SaveManager::Get().SaveWorldData();
    }

    // Ticks synchronously on the main Game Thread via Actor:K2_GetActorRotation
    static void OnGameThreadTick(UnrealScriptFunctionCallableContext& Context, void*) {
        static bool bIsReentrant = false;
        if (bIsReentrant) return;
        bIsReentrant = true;

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

    // Main Menu Detector - Fires exactly when a UI is rendered to the screen
    static void OnWidgetAddedToViewport(UnrealScriptFunctionCallableContext& Context, void*) {
        if (bIsAtMenu) return; // Fast exit if we have already handled returning to the main menu

        UObject* Widget = Context.Context;
        if (!Widget) return;

        UClass* WidgetClass = Widget->GetClassPrivate();
        if (!WidgetClass) return;

        std::wstring WidgetName = WidgetClass->GetName();
        
        // Detect the Main Menu or Login Screen natively
        if (WidgetName.find(L"WBP_Title") != std::wstring::npos || 
            WidgetName.find(L"WBP_Login") != std::wstring::npos) 
        {
            bIsAtMenu = true;
            bCompletedInitReady = false;
            
            SaveManager::Get().Reset();
            NotificationManager::Get().SetReady(false); 
            PalProcessor::Get().ClearAllSwappedStatus();
            
            DP_LOG(Default, "Transitioned to Main Menu (Detected via %S). Mod entering standby mode...\n", WidgetName.c_str());

            // Execute the auto-updater safely on a background thread
            std::thread([]() {
                Updater::CheckForUpdates();
            }).detach();
        }
    }

    static void OnOpenLevel(UnrealScriptFunctionCallableContext& Context, void*) {
        // Any time a map transition starts (e.g. clicking Return to Title), we reset our Menu tracker
        // so that OnWidgetAddedToViewport will successfully trigger the updater again when the map finishes loading.
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
                    // Reset the menu tracking state since we are loading into a live session
                    bIsAtMenu = false; 
                    bCompletedInitReady = false;
                    
                    NotificationManager::Get().SetReady(false);
                    SaveManager::Get().Reset();
                    PalProcessor::Get().ClearAllSwappedStatus();

                    DP_LOG(Default, "New Session Detected (Map: '{}'). Spawning surge quarantine active. Pausing swaps for 5 seconds...\n", MapName);

                    // Fire and forget the 5-second settle period on a background thread
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

                            bCompletedInitReady = true; // Safe to process new spawns now!
                            DP_LOG(Default, "Reconciliation complete. Mod entering zero-overhead standby.\n");

                            // Grab the parsed version string and broadcast it to the player as a warning-level toast [1, 2]
                            std::wstring verStr = GetFormattedVersionString();
                            DP_LOG(Normal, "welcome to dynamic pals {}", verStr);

                            // Flush any deferred startup errors or syntax issues safely [1, 2]
                            NotificationManager::Get().FlushQueuedToasts();
                        });
                    }).detach();
                }
            }
        }
    }

    void HooksManager::RegisterHooks() {
        // 1. Hook the Event-Driven Spawn Pipeline
        UFunction* InitFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalNPC:OnCompletedInitParam"));
        if (InitFunc) {
            InitFunc->RegisterPostHook(OnPalSpawnedReady, nullptr);
            DP_LOG(Default, "Successfully hooked OnCompletedInitParam (Native Pipeline Active!)\n");
        } else {
            DP_LOG(Error, "CRITICAL: Failed to hook PalNPC initialization!\n");
        }

        // 2. Map transition & Quarantine trigger on ClientRestart
        UFunction* RestartFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.PlayerController:ClientRestart"));
        if (RestartFunc) {
            RestartFunc->RegisterPostHook(OnClientRestart, nullptr);
            DP_LOG(Default, "Successfully hooked ClientRestart for map transitions.\n");
        } else {
            DP_LOG(Error, "CRITICAL: Failed to hook ClientRestart!\n");
        }

        // 3. Primary Game Thread Tick Hook
        UFunction* ActorRotFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Actor:K2_GetActorRotation"));
        if (ActorRotFunc) {
            ActorRotFunc->RegisterPreHook(OnGameThreadTick, nullptr);
            DP_LOG(Default, "Successfully hooked K2_GetActorRotation on the Game Thread.\n");
        } else {
            DP_LOG(Error, "CRITICAL: Failed to hook K2_GetActorRotation!\n");
        }

        // 4. Auto-Save Hook
        UFunction* SaveFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalSaveGameManager:StartWorldDataAutoSave"));
        if (SaveFunc) {
            SaveFunc->RegisterPostHook(OnStartedWorldAutoSave, nullptr);
            DP_LOG(Default, "Successfully hooked StartWorldDataAutoSave for auto-saving.\n");
        } else {
            DP_LOG(Warning, "WARNING: Failed to hook StartWorldDataAutoSave. Auto-saves may not trigger.\n");
        }

        // 5. LIVE STAT UPDATE HOOKS 
        const wchar_t* StatHooks[] = {
            STR("/Script/Pal.PalIndividualCharacterParameter:UpdateLevelDelegate"),
            STR("/Script/Pal.PalIndividualCharacterParameter:UpdateRankDelegate"),
            STR("/Script/Pal.PalIndividualCharacterParameter:UpdateFriendshipPointDelegate"),
            STR("/Script/Pal.PalIndividualCharacterParameter:OnPassiveSkillUpdateDelegate"),
            STR("/Script/Pal.PalIndividualCharacterParameter:AddPassiveSkill"),
            STR("/Script/Pal.PalIndividualCharacterParameter:RemovePassiveSkill")
        };

        for (const wchar_t* HookName : StatHooks) {
            UFunction* TargetFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, HookName);
            if (TargetFunc) {
                TargetFunc->RegisterPostHook(OnPalStatChanged, nullptr);
            }
        }

        // 6. Hook AddToViewport to detect the Title Screen widget
        UFunction* AddToViewportFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToViewport"));
        if (AddToViewportFunc) {
            AddToViewportFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
            DP_LOG(Default, "Successfully hooked UserWidget:AddToViewport for menu detection.\n");
        }
        
        // 6B. Fallback for AddToPlayerScreen 
        UFunction* AddToPlayerScreenFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToPlayerScreen"));
        if (AddToPlayerScreenFunc) {
            AddToPlayerScreenFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
        }

        // 7. Hook OpenLevel to reset the Title Screen detection flag on map change
        UFunction* OpenLevelFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics:OpenLevel"));
        if (OpenLevelFunc) {
            OpenLevelFunc->RegisterPreHook(OnOpenLevel, nullptr);
            DP_LOG(Default, "Successfully hooked GameplayStatics:OpenLevel.\n");
        }
    }
}