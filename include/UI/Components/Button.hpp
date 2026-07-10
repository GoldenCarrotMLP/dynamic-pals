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
            InitializeCachedPointers();
        }

        // Support wrapping tab buttons or pre-existing native menu items
        Button(RC::Unreal::UObject* ExistingWidget) : Widget(ExistingWidget) {
            InitializeCachedPointers();
        }

        RC::Unreal::UObject* GetWidget() const { return Widget; }

        Button& OnClicked(std::function<void()> Callback) {
            OnClickCallback = Callback;
            return *this;
        }

        void Tick() {
            if (!Widget) return;
            bool isPressed = IsWidgetPressed();
            if (isPressed) {
                bWasPressed = true;
            } else {
                // Trigger action strictly on Mouse-Up (Release)
                if (bWasPressed) {
                    bWasPressed = false;
                    if (OnClickCallback) {
                        OnClickCallback();
                    }
                }
            }
        }

    private:
        RC::Unreal::UObject* Widget = nullptr;
        RC::Unreal::UObject* EvaluatedTargetBtn = nullptr;
        RC::Unreal::UFunction* IsPressedFunc = nullptr;
        bool bWasPressed = false;
        std::function<void()> OnClickCallback;

        void InitializeCachedPointers() {
            if (!Widget) return;
            EvaluatedTargetBtn = Widget;
            
            RC::Unreal::UObject* Temp = nullptr;
            if (Utils::GetPropertyValue(Widget, STR("WBP_PalCommonButton"), Temp, true) && Temp) {
                EvaluatedTargetBtn = Temp;
            } else if (Utils::GetPropertyValue(Widget, STR("WBP_PalInvisibleButton"), Temp, true) && Temp) {
                EvaluatedTargetBtn = Temp;
            }

            if (EvaluatedTargetBtn) {
                IsPressedFunc = EvaluatedTargetBtn->GetFunctionByNameInChain(STR("IsPressed"));
            }
        }

        bool IsWidgetPressed() const {
            if (!EvaluatedTargetBtn || !IsPressedFunc) return false;
            struct { bool RetVal; } Params{false};
            EvaluatedTargetBtn->ProcessEvent(IsPressedFunc, &Params);
            return Params.RetVal;
        }
    };
}
// --- END OF FILE include/UI/Components/Button.hpp ---