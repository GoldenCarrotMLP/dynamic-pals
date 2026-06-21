#define NOMINMAX 
#include "HooksManager.hpp"
#include "PalProcessor.hpp"
#include "SaveManager.hpp"
#include "UIManager.hpp"
#include "Utils.hpp"

#include <Unreal/CoreUObject/UObject/Class.hpp> 
#include <Unreal/UObjectGlobals.hpp>
#include <chrono>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    // Quarantine & Performance State Machine Globals
    static bool bCompletedInitReady = false;
    static std::chrono::steady_clock::time_point WorldStartTime;
    static bool bTimerStarted = false;
    static bool bTickProcessingEnabled = false; // Starts asleep until we enter a game map
    
    // Pointer trackers to reliably detect new sessions vs fast travels
    static UObject* LastPlayerController = nullptr;
    static UObject* LastWorld = nullptr;

    void HooksManager::OnPalSpawnedReady(UnrealScriptFunctionCallableContext& Context, void*) {
        // If the spawning-surge quarantine is still active, ignore the spawn safely
        if (!bCompletedInitReady) {
            return;
        }

        UObject* PalNPC = Context.Context;
        if (PalNPC) {
            PalProcessor::Get().ProcessPal(PalNPC, false);
        }
    }

    // Called natively by the game whenever a world auto-save is triggered
    static void OnStartedWorldAutoSave(UnrealScriptFunctionCallableContext&, void*) {
        DP_LOG(Normal, "Auto-Save triggered! Synchronizing world persistence...\n");
        SaveManager::Get().SaveWorldData();
    }

    // Ticks synchronously on the main Game Thread via Actor:K2_GetActorRotation
    static void OnGameThreadTick(UnrealScriptFunctionCallableContext& Context, void*) {
        static bool bIsReentrant = false;
        if (bIsReentrant) return;
        bIsReentrant = true;

        // ZERO-OVERHEAD BYPASS: Suspends the tick hook completely during normal gameplay and menus.
        // It only wakes up during the first 5 seconds of loading, or if you press Alt+N / have the menu open.
        if (!bTickProcessingEnabled && !UIManager::Get().IsMenuOpen() && !UIManager::Get().IsToggleRequested()) {
            bIsReentrant = false;
            return;
        }

        UObject* ActorContext = Context.Context;
        if (ActorContext) {
            UObject* PlayerController = nullptr;

            // Resolve the PlayerController using the ticking Actor as WorldContext
            UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            if (GameplayStatics) {
                struct { UObject* WorldContextObject; int32_t PlayerIndex; UObject* ReturnValue; } GSParams{ActorContext, 0, nullptr};
                Utils::CallFunction(GameplayStatics, STR("GetPlayerController"), &GSParams);
                PlayerController = GSParams.ReturnValue;
            }

            if (PlayerController) {
                struct { bool ReturnValue; } IsLocalParams{false};
                Utils::CallFunction(PlayerController, STR("IsLocalPlayerController"), &IsLocalParams);

                if (IsLocalParams.ReturnValue) {
                    static auto LastTickTime = std::chrono::steady_clock::now();
                    auto Now = std::chrono::steady_clock::now();
                    
                    // Throttle execution to ~60 FPS (16ms)
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastTickTime).count() >= 16) {
                        LastTickTime = Now;
                        
                        UIManager::Get().TickUI(PlayerController);

                        // If quarantine is active, update timer and run the reconcile after 5 seconds
                        if (bTimerStarted && !bCompletedInitReady) {
                            auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now - WorldStartTime).count();
                            
                            if (Elapsed >= 5) {
                                DP_LOG(Normal, "Settle period complete. Running one-time overworld Pal reconciliation...\n");
                                
                                std::vector<UObject*> AllPals;
                                UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
                                for (UObject* Pal : AllPals) {
                                    if (Pal) {
                                        PalProcessor::Get().ProcessPal(Pal, false);
                                    }
                                }

                                bCompletedInitReady = true;
                                bTimerStarted = false;
                                bTickProcessingEnabled = false; // Disable the tick hook for zero overhead during gameplay!
                                DP_LOG(Normal, "Reconciliation complete. Native spawn pipeline is now active!\n");
                            }
                        }
                    }
                }
            }
        }

        bIsReentrant = false;
    }

    // Triggers safely when the player finishes loading and possesses their character
    static void OnClientRestart(UnrealScriptFunctionCallableContext& Context, void*) {
        UObject* PlayerController = Context.Context;
        if (!PlayerController) return;

        // Traverse the Outer chain to find the current UWorld pointer
        UObject* Level = PlayerController->GetOuterPrivate();
        UObject* CurrentWorld = Level ? Level->GetOuterPrivate() : nullptr;

        // Check if the memory pointers have changed (indicating a new session / hard map load)
        if (PlayerController != LastPlayerController || CurrentWorld != LastWorld) {
            LastPlayerController = PlayerController;
            LastWorld = CurrentWorld;

            UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            if (GameplayStatics) {
                // Fetch the current level name safely
                struct { UObject* WorldContextObject; bool bRemovePrefixString; FString ReturnValue; } Params{PlayerController, true, FString()};
                Utils::CallFunction(GameplayStatics, STR("GetCurrentLevelName"), &Params);
                std::wstring MapName = Utils::FStringToWString(Params.ReturnValue);

                bool bIsMenu = (MapName.find(L"Title") != std::wstring::npos || 
                                MapName.find(L"Login") != std::wstring::npos ||
                                MapName.empty());

                if (bIsMenu) {
                    // Transition: Game -> Menu
                    bTimerStarted = false;
                    bTickProcessingEnabled = false; 
                    
                    SaveManager::Get().Reset();
                    PalProcessor::Get().ClearAllSwappedStatus();
                    
                    DP_LOG(Normal, "Transitioned to Main Menu. Mod entering standby mode...\n");
                } else {
                    // Transition: Menu -> Game (New Session)
                    bCompletedInitReady = false;
                    bTimerStarted = true;
                    bTickProcessingEnabled = true; // Wake up the tick hook for the countdown
                    WorldStartTime = std::chrono::steady_clock::now();
                    
                    SaveManager::Get().Reset();
                    PalProcessor::Get().ClearAllSwappedStatus();

                    DP_LOG(Normal, "New Session Detected (Map: '{}'). Spawning surge quarantine active. Pausing swaps for 5 seconds...\n", MapName);
                }
            }
        }
        // If PlayerController == LastPlayerController AND CurrentWorld == LastWorld, 
        // it's just a fast travel or respawn! We do absolutely nothing, preserving your swaps.
    }

    void HooksManager::RegisterHooks() {
        // 1. Hook the Event-Driven Spawn Pipeline
        UFunction* InitFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalNPC:OnCompletedInitParam"));
        if (InitFunc) {
            InitFunc->RegisterPostHook(OnPalSpawnedReady, nullptr);
            DP_LOG(Normal, "Successfully hooked OnCompletedInitParam (Native Pipeline Active!)\n");
        } else {
            DP_LOG(Error, "CRITICAL: Failed to hook PalNPC initialization!\n");
        }

        // 2. Map transition & Quarantine trigger on ClientRestart
        UFunction* RestartFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.PlayerController:ClientRestart"));
        if (RestartFunc) {
            RestartFunc->RegisterPostHook(OnClientRestart, nullptr);
            DP_LOG(Normal, "Successfully hooked ClientRestart for map transitions.\n");
        } else {
            DP_LOG(Error, "CRITICAL: Failed to hook ClientRestart!\n");
        }

        // 3. Primary Game Thread Tick Hook: K2_GetActorRotation (Wakes up only for countdown/menu actions)
        UFunction* ActorRotFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Actor:K2_GetActorRotation"));
        if (ActorRotFunc) {
            ActorRotFunc->RegisterPreHook(OnGameThreadTick, nullptr);
            DP_LOG(Normal, "Successfully hooked K2_GetActorRotation on the Game Thread.\n");
        } else {
            DP_LOG(Error, "CRITICAL: Failed to hook K2_GetActorRotation!\n");
        }

        // 4. Auto-Save Hook: StartWorldDataAutoSave (Syncs our persistence on native auto-saves)
        UFunction* SaveFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalSaveGameManager:StartWorldDataAutoSave"));
        if (SaveFunc) {
            SaveFunc->RegisterPostHook(OnStartedWorldAutoSave, nullptr);
            DP_LOG(Normal, "Successfully hooked StartWorldDataAutoSave for auto-saving.\n");
        } else {
            DP_LOG(Warning, "WARNING: Failed to hook StartWorldDataAutoSave. Auto-saves may not trigger.\n");
        }
    }
}