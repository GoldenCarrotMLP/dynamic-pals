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
    static bool bTickProcessingEnabled = true;

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

    // Ticks synchronously on the main Game Thread via Actor:K2_GetActorRotation
    static void OnGameThreadTick(UnrealScriptFunctionCallableContext& Context, void*) {
        static bool bIsReentrant = false;
        if (bIsReentrant) return;
        bIsReentrant = true;

        // ZERO-OVERHEAD BYPASS: Suspends the tick hook completely during normal gameplay.
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
                        
                        SaveManager::Get().TickSave();
                        UIManager::Get().TickUI(PlayerController);

                        // If quarantine is active, update timer and run the reconcile after 5 seconds
                        if (bTimerStarted && !bCompletedInitReady) {
                            auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now - WorldStartTime).count();
                            
                            if (Elapsed >= 10) {
                                DP_LOG(Normal, "[DynPals] Settle period complete. Running one-time overworld Pal reconciliation...\n");
                                
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
                                DP_LOG(Normal, "[DynPals] Reconciliation complete. Native spawn pipeline is now active!\n");
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
        if (PlayerController) {
            // Activate the spawning quarantine and enable our tick hook to run the countdown
            bCompletedInitReady = false;
            bTimerStarted = true;
            bTickProcessingEnabled = true;
            WorldStartTime = std::chrono::steady_clock::now();
            
            // Completely flush old memory, save directories, and swap states on world transition
            SaveManager::Get().Reset();
            PalProcessor::Get().ClearAllSwappedStatus();

            DP_LOG(Normal, "[DynPals] Spawning surge quarantine active. Pausing swaps for 5 seconds...\n");
        }
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

        // 2. Quarantine trigger on ClientRestart (Fires safely when the player possesses their pawn)
        UFunction* RestartFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.PlayerController:ClientRestart"));
        if (RestartFunc) {
            RestartFunc->RegisterPostHook(OnClientRestart, nullptr);
            DP_LOG(Normal, "Successfully hooked ClientRestart for dynamic tick binding.\n");
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
    }
}