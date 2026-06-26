#pragma once
#include <Unreal/UObjectGlobals.hpp>
#include <string>
#include <vector>
#include <atomic>

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
        void TickUI(RC::Unreal::UObject* LocalPlayerController); 
        
        void RequestMenuToggle() { bToggleRequested = true; }

        bool IsToggleRequested() const { return bToggleRequested.load(); }
        bool IsMenuOpen() const { return bIsMenuOpen; }

    private:
        UIManager() = default;
        UIManager(const UIManager&) = delete;
        UIManager& operator=(const UIManager&) = delete;

        void BuildWidget();
        void DestroyWidget();
        void UpdateTarget();
        void LockInput(bool bLock);
        void RefreshSliders(); // <-- ADD THIS

        std::atomic<bool> bToggleRequested{false};
        bool bIsMenuOpen = false;
        bool bHideInvalidSwaps = true; 
        
        RC::Unreal::UObject* CurrentPlayerController = nullptr; 
        RC::Unreal::UObject* MyWidget = nullptr;
        RC::Unreal::UObject* ComboBoxWidget = nullptr;
        RC::Unreal::UObject* CheckBoxWidget = nullptr; 
        RC::Unreal::UObject* RandomizeButtonWidget = nullptr; 
        RC::Unreal::UObject* ScrollBoxWidget = nullptr;
        RC::Unreal::UObject* SlidersVBox = nullptr; // <-- ADD THIS
        RC::Unreal::UObject* TargetPal = nullptr;

        std::wstring TargetInstanceID = L"";
        std::wstring TargetCharID = L"";
        std::wstring LastSelectedOption = L"";
        std::vector<ActiveSlider> ActiveSliders;

        bool bWasRandomizePressed = false; 
    };
}