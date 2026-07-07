// --- START OF FILE include/UI/Controls.hpp ---
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cmath>
#include <Unreal/UObjectGlobals.hpp>
#include "Utils.hpp"

namespace DynPals::UI {

    // Smart controller for generic Buttons (WBP_CommonButton, PalInvisibleButton, etc.)
    class ButtonController {
    public:
        ButtonController(RC::Unreal::UObject* InWidget) : Widget(InWidget) {}

        ButtonController& OnClicked(std::function<void()> Callback) {
            OnClickCallback = Callback;
            return *this;
        }

        void Tick() {
            if (!Widget) return;
            bool isPressed = IsWidgetPressed(Widget);
            if (isPressed) {
                if (!bWasPressed) {
                    bWasPressed = true;
                    if (OnClickCallback) {
                        OnClickCallback();
                    }
                }
            } else {
                bWasPressed = false;
            }
        }

    private:
        RC::Unreal::UObject* Widget;
        bool bWasPressed = false;
        std::function<void()> OnOnClickCallback; // Renamed correctly to prevent matching issues
        std::function<void()> OnClickCallback;

        bool IsWidgetPressed(RC::Unreal::UObject* WidgetObj) const {
            if (!WidgetObj) return false;
            RC::Unreal::UObject* TargetBtn = WidgetObj;
            RC::Unreal::UObject* Temp = nullptr;

            if (Utils::GetPropertyValue(TargetBtn, STR("WBP_PalCommonButton"), Temp) && Temp) TargetBtn = Temp;
            if (Utils::GetPropertyValue(TargetBtn, STR("WBP_PalInvisibleButton"), Temp) && Temp) TargetBtn = Temp;

            struct { bool RetVal; } Params{false};
            Utils::CallFunction(TargetBtn, STR("IsPressed"), &Params);
            return Params.RetVal;
        }
    };

    // Smart controller for OptionSlider (WBP_OptionSettings_ListContentSlider_C)
    class SliderController {
    public:
        SliderController(RC::Unreal::UObject* InWidget, double InitialValue = 50.0) 
            : Widget(InWidget), LastValue(InitialValue) {}

        SliderController& OnChanged(std::function<void(double)> Callback) {
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
        RC::Unreal::UObject* Widget;
        double LastValue;
        std::function<void(double)> OnChangeCallback;
    };

    // Smart controller for OptionSwitch (WBP_OptionSettings_ListContentSwitch_C)
    class SwitchController {
    public:
        SwitchController(RC::Unreal::UObject* InWidget, bool InitialState = false) 
            : Widget(InWidget), LastState(InitialState) {}

        SwitchController& OnChanged(std::function<void(bool)> Callback) {
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
        RC::Unreal::UObject* Widget;
        bool LastState;
        std::function<void(bool)> OnChangeCallback;
    };

    // Smart controller for OptionLR (WBP_OptionSettings_ListContentLR_C)
    class SelectorController {
    public:
        SelectorController(RC::Unreal::UObject* InWidget, int32_t InitialIndex = 0) 
            : Widget(InWidget), LastIndex(InitialIndex) {}

        SelectorController& OnChanged(std::function<void(int32_t)> Callback) {
            OnChangeCallback = Callback;
            return *this;
        }

        void Tick() {
            if (!Widget) return;
            int32_t currentIndex = LastIndex;
            if (Utils::GetPropertyValue(Widget, STR("Current"), currentIndex)) {
                if (currentIndex != LastIndex) {
                    LastIndex = currentIndex;
                    if (OnChangeCallback) {
                        OnChangeCallback(currentIndex);
                    }
                }
            }
        }

    private:
        RC::Unreal::UObject* Widget;
        int32_t LastIndex;
        std::function<void(int32_t)> OnChangeCallback;
    };
}
// --- END OF FILE include/UI/Controls.hpp ---