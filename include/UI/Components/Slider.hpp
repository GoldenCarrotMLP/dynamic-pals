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
            InitializeCachedPointers();
        }

        RC::Unreal::UObject* GetWidget() const { return Widget; }

        Slider& OnChanged(std::function<void(double)> Callback) {
            OnChangeCallback = Callback;
            return *this;
        }

        void Tick() {
            if (!Widget || !CurrentValueProp) return;
            double currentVal = LastValue;
            
            double* Ptr = CurrentValueProp->ContainerPtrToValuePtr<double>(Widget);
            if (Ptr) {
                currentVal = *Ptr;
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
        RC::Unreal::FProperty* CurrentValueProp = nullptr;
        double LastValue = 50.0;
        std::function<void(double)> OnChangeCallback;

        void InitializeCachedPointers() {
            if (!Widget) return;
            CurrentValueProp = Utils::GetProperty(Widget, STR("CurrentValue"));
        }
    };
}