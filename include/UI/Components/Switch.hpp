// --- START OF FILE include/UI/Components/Switch.hpp ---
#pragma once
#include <functional>
#include <Unreal/UObjectGlobals.hpp>
#include "UI/WidgetBuilder.hpp"
#include "Utils.hpp"

namespace DynPals::UI {

    class Switch {
    public:
        Switch(RC::Unreal::UObject* Outer, bool Initial = false) {
            Widget = UI::OptionSwitch(Outer).SetupSwitch(Initial).Build();
            LastState = Initial;
        }

        RC::Unreal::UObject* GetWidget() const { return Widget; }

        Switch& OnChanged(std::function<void(bool)> Callback) {
            OnChangeCallback = Callback;
            return *this;
        }

        void Tick() {
            if (!Widget) return;
            bool currentIsOn = LastState;
            if (Utils::GetPropertyValue(Widget, STR("CurrentIsOn"), currentIsOn)) {
                if (currentIsOn != LastState) {
                    LastState = currentIsOn;
                    if (OnChangeCallback) {
                        OnChangeCallback(currentIsOn);
                    }
                }
            }
        }

    private:
        RC::Unreal::UObject* Widget = nullptr;
        bool LastState = false;
        std::function<void(bool)> OnChangeCallback;
    };
}
// --- END OF FILE include/UI/Components/Switch.hpp ---