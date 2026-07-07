// --- START OF FILE include/UI/Components/Button.hpp ---
#pragma once
#include <functional>
#include <string>
#include <Unreal/UObjectGlobals.hpp>
#include "UI/WidgetBuilder.hpp"
#include "Utils.hpp"

namespace DynPals::UI {

    class Button {
    public:
        // Construct a standard button with an initial text label
        Button(RC::Unreal::UObject* Outer, const std::wstring& Text) {
            Widget = UI::Button(Outer).Text(Text).Build();
        }

        // Support wrapping tab buttons or pre-existing native menu items
        Button(RC::Unreal::UObject* ExistingWidget) : Widget(ExistingWidget) {}

        RC::Unreal::UObject* GetWidget() const { return Widget; }

        Button& OnClicked(std::function<void()> Callback) {
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
        RC::Unreal::UObject* Widget = nullptr;
        bool bWasPressed = false;
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
}
// --- END OF FILE include/UI/Components/Button.hpp ---