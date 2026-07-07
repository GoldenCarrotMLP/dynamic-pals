#pragma once
#include <functional>
#include <vector>
#include <string>
#include <Unreal/UObjectGlobals.hpp>
#include "UI/WidgetBuilder.hpp"
#include "Utils.hpp"

namespace DynPals::UI {

    class Selector {
    public:
        Selector(RC::Unreal::UObject* Outer, const std::vector<std::wstring>& Options, int32_t InitialIndex = 0) {
            Widget = UI::OptionLR(Outer).SetupLR(Options, InitialIndex).Build();
            LastIndex = InitialIndex;
            InitializeCachedPointers();
        }

        RC::Unreal::UObject* GetWidget() const { return Widget; }

        Selector& OnChanged(std::function<void(int32_t)> Callback) {
            OnChangeCallback = Callback;
            return *this;
        }

        void Tick() {
            if (!Widget || !CurrentProp) return;
            int32_t currentIndex = LastIndex;

            int32_t* Ptr = CurrentProp->ContainerPtrToValuePtr<int32_t>(Widget);
            if (Ptr) {
                currentIndex = *Ptr;
                if (currentIndex != LastIndex) {
                    LastIndex = currentIndex;
                    if (OnChangeCallback) {
                        OnChangeCallback(currentIndex);
                    }
                }
            }
        }

    private:
        RC::Unreal::UObject* Widget = nullptr;
        RC::Unreal::FProperty* CurrentProp = nullptr;
        int32_t LastIndex = 0;
        std::function<void(int32_t)> OnChangeCallback;

        void InitializeCachedPointers() {
            if (!Widget) return;
            CurrentProp = Utils::GetProperty(Widget, STR("Current"));
        }
    };
}