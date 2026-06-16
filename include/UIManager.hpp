#pragma once
#include <Unreal/UObjectGlobals.hpp>
#include <string>
#include <vector>

namespace DynPals {

    struct ActiveSlider {
        std::wstring MorphTargetName;
        RC::Unreal::UObject* SliderWidget;
        float LastValue;
    };

    class UIManager {
    public:
        static UIManager& Get() {
            static UIManager instance;
            return instance;
        }

        void ToggleMenu();
        void TickUI();

    private:
        UIManager() = default;
        UIManager(const UIManager&) = delete;
        UIManager& operator=(const UIManager&) = delete;

        RC::Unreal::UObject* GetLocalPlayerController();
        void BuildWidget();
        void DestroyWidget();
        void UpdateTarget();
        void LockInput(bool bLock);

        bool bIsMenuOpen = false;
        
        RC::Unreal::UObject* MyWidget = nullptr;
        RC::Unreal::UObject* ComboBoxWidget = nullptr;
        RC::Unreal::UObject* TargetPal = nullptr;

        std::wstring TargetInstanceID = L"";
        std::wstring TargetCharID = L"";
        std::wstring LastSelectedOption = L"";
        std::vector<ActiveSlider> ActiveSliders;
    };
}