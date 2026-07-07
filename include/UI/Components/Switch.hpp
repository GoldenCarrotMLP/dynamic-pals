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
            InitializeCachedPointers();
        }

        RC::Unreal::UObject* GetWidget() const { return Widget; }

        Switch& OnChanged(std::function<void(bool)> Callback) {
            OnChangeCallback = Callback;
            return *this;
        }

        void Tick() {
            if (!Widget || !CurrentIsOnProp) return;
            bool currentIsOn = LastState;

            bool* Ptr = CurrentIsOnProp->ContainerPtrToValuePtr<bool>(Widget);
            if (Ptr) {
                currentIsOn = *Ptr;
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
        RC::Unreal::FProperty* CurrentIsOnProp = nullptr;
        bool LastState = false;
        std::function<void(bool)> OnChangeCallback;

        void InitializeCachedPointers() {
            if (!Widget) return;
            CurrentIsOnProp = Utils::GetProperty(Widget, STR("CurrentIsOn"));
        }
    };
}