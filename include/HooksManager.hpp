#pragma once
#include <Unreal/Hooks.hpp>

namespace DynPals {
    class HooksManager {
    public:
        static void RegisterHooks();

    private:
        static void OnPalInit(RC::Unreal::UnrealScriptFunctionCallableContext& Context, void*);
    };
}