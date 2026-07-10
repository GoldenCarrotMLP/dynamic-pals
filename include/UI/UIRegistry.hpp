#pragma once
#include <vector>
#include <Unreal/UObjectGlobals.hpp>

namespace DynPals {
    class UIBase;

    class UIRegistry {
    public:
        static UIRegistry& Get() {
            static UIRegistry instance;
            return instance;
        }

        void RegisterUI(UIBase* UI);
        void UnregisterUI(UIBase* UI);
        
        // Ticks all registered UIs and handles global input locking
        void TickAll(RC::Unreal::UObject* PlayerController);
        void UpdateInputState(RC::Unreal::UObject* PlayerController);

        // Fast non-reflected check to see if any UI needs a tick
        inline bool RequiresTick() const { return bRequiresTick; }
        
        // Evaluates UI states to recalculate tick requirements
        void UpdateTickState();

        // Invalidates all registered widgets
        void InvalidateAllUIs();

    private:
        UIRegistry() = default;
        std::vector<UIBase*> RegisteredUIs;
        bool bIsInputLocked = false;
        bool bRequiresTick = false;
    };
}