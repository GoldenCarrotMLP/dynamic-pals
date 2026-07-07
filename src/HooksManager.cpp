// --- START OF FILE src/HooksManager.cpp ---
#define NOMINMAX 
#include "HooksManager.hpp"
#include "PalProcessor.hpp"
#include "SaveManager.hpp"
#include "UI/Views/UIManager.hpp" 
#include "NotificationManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "Updater.hpp"
#include "VFXManager.hpp"
#include "UI/UIRegistry.hpp"
#include <fstream>
#include <safetyhook.hpp> 
#include <Zydis/Zydis.h> 

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

    // --- NATIVE DETOUR STORAGE ---
    static SafetyHookInline Hook_MasterWazaUpdate;
    static SafetyHookInline Hook_OnUpdateCharacterRank;
    static SafetyHookInline Hook_AddFriendship;

    // --- DYNAMIC NATIVE THUNK DISASSEMBLER ---
    static void* ResolveNativeFromThunk(void* ThunkAddress) {
        if (!ThunkAddress) return nullptr;

        ZyanStatus status;
        ZydisDecoder decoder;
        if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
            return nullptr;
        }

        ZyanUSize offset = 0;
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        void* lastCallTarget = nullptr;

        while (offset < 150) {
            status = ZydisDecoderDecodeFull(
                &decoder, 
                reinterpret_cast<uint8_t*>(ThunkAddress) + offset, 
                150 - offset, 
                &instruction, 
                operands
            );

            if (!ZYAN_SUCCESS(status)) {
                break;
            }

            if (instruction.mnemonic == ZYDIS_MNEMONIC_RET) {
                break;
            }

            if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
                if (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    uintptr_t rip = reinterpret_cast<uintptr_t>(ThunkAddress) + offset + instruction.length;
                    uintptr_t target = rip + operands[0].imm.value.s;
                    lastCallTarget = reinterpret_cast<void*>(target);
                }
            }

            offset += instruction.length;
        }

        return lastCallTarget;
    }

    static void* GetNativeAddress(const wchar_t* FunctionPath) {
        UFunction* FuncObj = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, FunctionPath);
        if (!FuncObj) return nullptr;
        
        void* ThunkAddr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(FuncObj) + 0xD8);
        void* NativeAddr = ResolveNativeFromThunk(ThunkAddr);
        if (NativeAddr) {
            return NativeAddr; 
        }
        return ThunkAddr;
    }

    // --- DETOUR CALLBACKS ---
    void __fastcall NativeMasterWazaUpdate_Hook(UObject* This, int32_t AddLevel, int32_t NowLevel) {
        Hook_MasterWazaUpdate.call<void, UObject*, int32_t, int32_t>(This, AddLevel, NowLevel);
        if (This) {
            std::wstring actorName = This->GetName();
            DP_LOG(Default, "[Native Hook] Pal {} Leveled Up to {}! Checking evolution...", actorName, NowLevel);
            PalProcessor::Get().ProcessPal(This, false);
        }
    }

    void __fastcall NativeOnUpdateCharacterRank_Hook(UObject* This, int32_t NewRank, int32_t OldRank) {
        Hook_OnUpdateCharacterRank.call<void, UObject*, int32_t, int32_t>(This, NewRank, OldRank);
        if (This) {
            UObject* PalActor = This->GetOuterPrivate();
            if (PalActor) {
                std::wstring actorName = PalActor->GetName();
                DP_LOG(Default, "[Native Hook] Pal {} Condensation Rank Up to {}! Checking evolution...", actorName, NewRank);
                PalProcessor::Get().ProcessPal(PalActor, false);
            }
        }
    }

    void __fastcall NativeAddFriendship_Hook(UObject* This, int32_t Value) {
        Hook_AddFriendship.call<void, UObject*, int32_t>(This, Value);
        if (This) {
            UObject* PalActor = nullptr;
            Utils::GetPropertyValue<UObject*>(This, STR("IndividualActor"), PalActor);
            if (PalActor) {
                std::wstring actorName = PalActor->GetName();
                DP_LOG(Default, "[Native Hook] Pal {} Friendship updated! Checking evolution...", actorName);
                PalProcessor::Get().ProcessPal(PalActor, false);
            }
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

        // 1. Tick VFX Manager timeline events (extremely fast check)
        VFXManager::Get().Tick();

        // 2. ULTRA-PERFORMANT EXIT: Skip reflection logic entirely if no menus need to tick!
        if (!UIRegistry::Get().RequiresTick()) {
            bIsReentrant = false;
            return; 
        }

        // Only run slow reflection queries if the menu is actually active or transitioning
        static auto LastTickTime = std::chrono::steady_clock::now();
        auto Now = std::chrono::steady_clock::now();
        
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastTickTime).count() >= 16) {
            LastTickTime = Now;
            
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
                        UIRegistry::Get().TickAll(PlayerController);
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
            
            DP_LOG(Default, "Transitioned to Main Menu (Detected via '{}'). Mod entering standby mode...\n", WidgetName);

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

    void HooksManager::OnPalSpawnedReady(UnrealScriptFunctionCallableContext& Context, void*) {
        UObject* PalNPC = Context.Context;
        
        std::wstring palName = PalNPC ? PalNPC->GetName() : L"NULL";
        DP_LOG(Default, "[Hook Monitor] OnPalSpawnedReady fired for {}", palName);

        if (!bCompletedInitReady) {
            DP_LOG(Default, "  -> Aborted: Mod is still in startup standby.");
            return;
        }

        if (PalNPC) {
            PalProcessor::Get().ProcessPal(PalNPC, false);
        }
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

        // Restored: Re-hook K2_GetActorRotation to guarantee Game Thread access context!
        UFunction* ActorRotFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Actor:K2_GetActorRotation"));
        if (ActorRotFunc) {
            ActorRotFunc->RegisterPreHook(OnGameThreadTick, nullptr);
            DP_LOG(Default, "Successfully hooked K2_GetActorRotation on the Game Thread.\n");
        }

        UFunction* SaveFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalSaveGameManager:StartWorldDataAutoSave"));
        if (SaveFunc) {
            SaveFunc->RegisterPostHook(OnStartedWorldAutoSave, nullptr);
        }

        // ==================================================
        // MIXED NATIVE ASSEMBLY DETOURS
        // ==================================================
        uintptr_t BaseAddr = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));
        
        void* MasterWazaUpdateAddr = reinterpret_cast<void*>(BaseAddr + 0x2E55330); 
        if (MasterWazaUpdateAddr) {
            Hook_MasterWazaUpdate = safetyhook::create_inline(MasterWazaUpdateAddr, NativeMasterWazaUpdate_Hook);
            DP_LOG(Default, "[Native Hook] Detoured MasterWazaUpdateWhenLevelUp successfully!");
        } else {
            DP_LOG(Error, "Failed to resolve Native MasterWazaUpdateWhenLevelUp!");
        }

        void* SetRankAddr = reinterpret_cast<void*>(BaseAddr + 0x2AF9080);
        if (SetRankAddr) {
            Hook_OnUpdateCharacterRank = safetyhook::create_inline(SetRankAddr, NativeOnUpdateCharacterRank_Hook);
            DP_LOG(Default, "[Native Hook] Detoured OnUpdateCharacterRank successfully!");
        } else {
            DP_LOG(Error, "Failed to resolve Native OnUpdateCharacterRank!");
        }

        void* FriendshipAddr = GetNativeAddress(STR("/Script/Pal.PalIndividualCharacterParameter:AddFriendShip"));
        if (FriendshipAddr) {
            Hook_AddFriendship = safetyhook::create_inline(FriendshipAddr, NativeAddFriendship_Hook);
            DP_LOG(Default, "[Native Hook] Detoured AddFriendShip successfully!");
        } else {
            DP_LOG(Error, "Failed to resolve Native AddFriendShip!");
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
// --- END OF FILE src/HooksManager.cpp ---