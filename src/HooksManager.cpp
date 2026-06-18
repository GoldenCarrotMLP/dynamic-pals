#define NOMINMAX // Disable Windows min/max macros to prevent Unreal template corruption
#include "HooksManager.hpp"
#include "PalProcessor.hpp"
#include "SaveManager.hpp"
#include "UIManager.hpp"
#include "Utils.hpp"

#include <Unreal/CoreUObject/UObject/Class.hpp> 
#include <Unreal/UObjectGlobals.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    void HooksManager::OnPalInit(UnrealScriptFunctionCallableContext& Context, void*) {
        UObject* ParamComp = Context.Context;
        if (ParamComp && ParamComp->GetOuterPrivate()) {
            PalProcessor::Get().ProcessPal(ParamComp->GetOuterPrivate(), false);
        }
    }

    // A universal Game Thread tick callback
    static void OnGameThreadTick(UnrealScriptFunctionCallableContext& Context, void*) {
        // Safe reentrancy guard to prevent recursive loop execution
        static bool bIsReentrant = false;
        if (bIsReentrant) return;
        bIsReentrant = true;

        static bool bHasTickedOnce = false;
        if (!bHasTickedOnce) {
            bHasTickedOnce = true;
            Output::send<LogLevel::Normal>(STR("[DynPals] Game Thread core loop is ALIVE and ticking!\n"));
        }

        static auto LastTickTime = std::chrono::steady_clock::now();
        auto Now = std::chrono::steady_clock::now();
        
        // Throttle to 60 FPS (16ms)
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastTickTime).count() >= 16) {
            LastTickTime = Now;
            SaveManager::Get().TickSave();
            UIManager::Get().TickUI(); 
            PalProcessor::Get().TickDeferredSwaps();
            PalProcessor::Get().ScanActivePals(); 
        }

        bIsReentrant = false;
    }

    void HooksManager::RegisterHooks() {
        // 1. Pal Initializer Hook
        UFunction* InitFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalCharacterParameterComponent:OnInitialize_AfterSetIndividualParameter"));
        if (InitFunc) {
            InitFunc->RegisterPreHook(OnPalInit, nullptr);
            Output::send<LogLevel::Normal>(STR("[DynPals] Registered OnInitialize PreHook successfully.\n"));
        }

        // 2. Native C++ Locally-Evaluated Ticks (Guaranteed to execute constantly in single-player & multiplayer)
        std::vector<const wchar_t*> TickCandidates = {
            STR("/Script/Engine.Actor:K2_GetActorRotation"),
            STR("/Script/Engine.PlayerController:IsLocalPlayerController")
        };

        int HooksRegistered = 0;
        for (const wchar_t* FuncName : TickCandidates) {
            UFunction* Func = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, FuncName);
            if (Func) {
                Func->RegisterPreHook(OnGameThreadTick, nullptr);
                HooksRegistered++;
                Output::send<LogLevel::Normal>(STR("[DynPals] Hooked Game Thread UFunction: {}\n"), FuncName);
            }
        }

        if (HooksRegistered == 0) {
            Output::send<LogLevel::Error>(STR("[DynPals] CRITICAL: Failed to hook any Game Thread Tick functions!\n"));
        }
    }
}