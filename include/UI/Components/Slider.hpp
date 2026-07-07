// --- START OF FILE include/UI/Components/Slider.hpp ---
#pragma once
#include <functional>
#include <cmath>
#include <Unreal/UObjectGlobals.hpp>
#include "UI/WidgetBuilder.hpp"
#include "Utils.hpp"

namespace DynPals::UI {

    class Slider {
    public:
        Slider(RC::Unreal::UObject* Outer, double Min = 0.0, double Max = 100.0, double Initial = 50.0) {
            Widget = UI::OptionSlider(Outer).SetupSlider(Initial, Min, Max).Build();
            LastValue = Initial;
        }

        RC::Unreal::UObject* GetWidget() const { return Widget; }

        Slider& OnChanged(std::function<void(double)> Callback) {
            OnChangeCallback = Callback;
            return *this;
        }

        void Tick() {
            if (!Widget) return;
            double currentVal = LastValue;
            if (Utils::GetPropertyValue(Widget, STR("CurrentValue"), currentVal)) {
                if (std::abs(currentVal - LastValue) > 0.001) {
                    LastValue = currentVal;
                    if (OnChangeCallback) {
                        OnChangeCallback(currentVal);
                    }
                }
            }
        }

    private:
        RC::Unreal::UObject* Widget = nullptr;
        double LastValue = 50.0;
        std::function<void(double)> OnChangeCallback;
    };
}
// --- END OF FILE include/UI/Components/Slider.hpp ---