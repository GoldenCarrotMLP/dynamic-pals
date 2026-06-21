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

    static bool bCompletedInitReady = false;
    static std::chrono::steady_clock::time_point WorldStartTime;
    static bool bTimerStarted = false;
    static bool bTickProcessingEnabled = false; 
    
    static UObject* LastPlayerController = nullptr;
    static UObject* LastWorld = nullptr;

    void HooksManager::OnPalSpawnedReady(UnrealScriptFunctionCallableContext& Context, void*) {
        if (!bCompletedInitReady) return;

        UObject* PalNPC = Context.Context;
        if (PalNPC) {
            PalProcessor::Get().ProcessPal(PalNPC, false);
        }
    }

    // --- LIVE STAT CHANGE DETECTOR ---
    // Fires automatically when a Pal levels up, changes rank, traits, or friendship.
    static void OnPalStatChanged(UnrealScriptFunctionCallableContext& Context, void*) {
        if (!bCompletedInitReady) return; 

        UObject* IndivParam = Context.Context;
        if (!IndivParam) return;

        UObject* PalActor = nullptr;
        
        // Extract the actor this parameter component belongs to
        struct { UObject* ReturnValue; } GetActorParams{nullptr};
        Utils::CallFunction(IndivParam, STR("GetIndividualActor"), &GetActorParams);
        PalActor = GetActorParams.ReturnValue;

        // Fallback to reading the property directly if the getter fails
        if (!PalActor) {
            Utils::GetPropertyValue<UObject*>(IndivParam, STR("IndividualActor"), PalActor);
        }

        if (PalActor) {
            // ProcessPal will intelligently check if the new stats unlocked a better tier skin!
            PalProcessor::Get().ProcessPal(PalActor, false);
        }
    }

    static void OnStartedWorldAutoSave(UnrealScriptFunctionCallableContext&, void*) {
        DP_LOG(Normal, "Auto-Save triggered! Synchronizing world persistence...\n");
        SaveManager::Get().SaveWorldData();
    }

    // Ticks synchronously on the main Game Thread via Actor:K2_GetActorRotation
    static void OnGameThreadTick(UnrealScriptFunctionCallableContext& Context, void*) {
        static bool bIsReentrant = false;
        if (bIsReentrant) return;
        bIsReentrant = true;

        // ZERO-OVERHEAD BYPASS: Suspends the tick hook completely during normal gameplay.
        // It only wakes up during level loading, when the menu is open, if a toggle is requested, or if a deferred swap is executing.
        if (!bTickProcessingEnabled && 
            !UIManager::Get().IsMenuOpen() && 
            !UIManager::Get().IsToggleRequested() && 
            !PalProcessor::Get().HasPendingSwaps()) 
        {
            bIsReentrant = false;
            return;
        }

        // Throttle heavy execution to 60 FPS (16ms) to prevent performance stutters
        static auto LastTickTime = std::chrono::steady_clock::now();
        auto Now = std::chrono::steady_clock::now();
        
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastTickTime).count() >= 16) {
            LastTickTime = Now;
            
            // FIX: Process UI inputs if the menu is active OR if the user is attempting to toggle it (Alt+N)
            if (UIManager::Get().IsMenuOpen() || UIManager::Get().IsToggleRequested()) {
                UObject* ActorContext = Context.Context;
                if (ActorContext) {
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

            // Process deferred swaps
            PalProcessor::Get().TickDeferredSwaps();

            // Handle the 5-second level load reconciliation countdown
            if (bTimerStarted && !bCompletedInitReady) {
                auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now - WorldStartTime).count();
                
                if (Elapsed >= 5) {
                    DP_LOG(Normal, "Settle period complete. Running overworld Pal reconciliation...\n");
                    
                    std::vector<UObject*> AllPals;
                    UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
                    for (UObject* Pal : AllPals) {
                        if (Pal) {
                            PalProcessor::Get().ProcessPal(Pal, false);
                        }
                    }

                    bCompletedInitReady = true;
                    bTimerStarted = false;
                    bTickProcessingEnabled = false; // Goes back to sleep!
                    DP_LOG(Normal, "Reconciliation complete. Mod entering zero-overhead standby.\n");
                }
            }
        }

        bIsReentrant = false;
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
                    bTimerStarted = false;
                    bTickProcessingEnabled = false; 
                    
                    SaveManager::Get().Reset();
                    PalProcessor::Get().ClearAllSwappedStatus();
                    
                    DP_LOG(Normal, "Transitioned to Main Menu. Mod entering standby mode...\n");
                } else {
                    bCompletedInitReady = false;
                    bTimerStarted = true;
                    bTickProcessingEnabled = true; 
                    WorldStartTime = std::chrono::steady_clock::now();
                    
                    SaveManager::Get().Reset();
                    PalProcessor::Get().ClearAllSwappedStatus();

                    DP_LOG(Normal, "New Session Detected (Map: '{}'). Spawning surge quarantine active. Pausing swaps for 5 seconds...\n", MapName);
                }
            }
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

        // 2. Map transition & Quarantine trigger on ClientRestart
        UFunction* RestartFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.PlayerController:ClientRestart"));
        if (RestartFunc) {
            RestartFunc->RegisterPostHook(OnClientRestart, nullptr);
            DP_LOG(Normal, "Successfully hooked ClientRestart for map transitions.\n");
        } else {
            DP_LOG(Error, "CRITICAL: Failed to hook ClientRestart!\n");
        }

        // 3. Primary Game Thread Tick Hook
        UFunction* ActorRotFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Actor:K2_GetActorRotation"));
        if (ActorRotFunc) {
            ActorRotFunc->RegisterPreHook(OnGameThreadTick, nullptr);
            DP_LOG(Normal, "Successfully hooked K2_GetActorRotation on the Game Thread.\n");
        } else {
            DP_LOG(Error, "CRITICAL: Failed to hook K2_GetActorRotation!\n");
        }

        // 4. Auto-Save Hook
        UFunction* SaveFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalSaveGameManager:StartWorldDataAutoSave"));
        if (SaveFunc) {
            SaveFunc->RegisterPostHook(OnStartedWorldAutoSave, nullptr);
            DP_LOG(Normal, "Successfully hooked StartWorldDataAutoSave for auto-saving.\n");
        } else {
            DP_LOG(Warning, "WARNING: Failed to hook StartWorldDataAutoSave. Auto-saves may not trigger.\n");
        }

        // 5. LIVE STAT UPDATE HOOKS (Trigger Morph Engine on Level Up / Trust Up / Trait Updates)
        const wchar_t* StatHooks[] = {
            STR("/Script/Pal.PalIndividualCharacterParameter:UpdateLevelDelegate"),
            STR("/Script/Pal.PalIndividualCharacterParameter:UpdateRankDelegate"),
            STR("/Script/Pal.PalIndividualCharacterParameter:UpdateFriendshipPointDelegate"),
            STR("/Script/Pal.PalIndividualCharacterParameter:OnPassiveSkillUpdateDelegate"),
            STR("/Script/Pal.PalIndividualCharacterParameter:AddPassiveSkill"),
            STR("/Script/Pal.PalIndividualCharacterParameter:RemovePassiveSkill")
        };

        for (const wchar_t* HookTarget : StatHooks) {
            UFunction* Func = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, HookTarget);
            if (Func) {
                Func->RegisterPostHook(OnPalStatChanged, nullptr);
                DP_LOG(Normal, "Live Stat Hook Registered: {}\n", HookTarget);
            }
        }
    }
}