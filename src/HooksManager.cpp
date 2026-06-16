#define NOMINMAX // Disable Windows min/max macros to prevent Unreal template corruption
#include "HooksManager.hpp"
#include "PalProcessor.hpp"
#include "SaveManager.hpp"
#include "Utils.hpp"

#include <Unreal/UFunction.hpp>
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

    void HooksManager::RegisterHooks() {
        UFunction* InitFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalCharacterParameterComponent:OnInitialize_AfterSetIndividualParameter"));
        if (InitFunc) {
            InitFunc->RegisterPreHook(OnPalInit, nullptr);
            Output::send<LogLevel::Normal>(STR("[DynPals] Registered OnInitialize PreHook successfully.\n"));
        } else {
            Output::send<LogLevel::Error>(STR("[DynPals] Failed to find OnInitialize function.\n"));
        }
    }
}