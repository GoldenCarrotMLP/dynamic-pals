#pragma once
#include <Unreal/UObjectGlobals.hpp>
#include <string>
#include <vector>
#include <atomic> // For thread-safe flags

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
        
        // Safe cross-thread toggle request
        void RequestMenuToggle() { bToggleRequested = true; }

    private:
        UIManager() = default;
        UIManager(const UIManager&) = delete;
        UIManager& operator=(const UIManager&) = delete;

        RC::Unreal::UObject* GetLocalPlayerController();
        void BuildWidget();
        void DestroyWidget();
        void UpdateTarget();
        void LockInput(bool bLock);

        std::atomic<bool> bToggleRequested{false};
        bool bIsMenuOpen = false;
        bool bHideInvalidSwaps = true; 
        
        RC::Unreal::UObject* MyWidget = nullptr;
        RC::Unreal::UObject* ComboBoxWidget = nullptr;
        RC::Unreal::UObject* CheckBoxWidget = nullptr; 
        RC::Unreal::UObject* RandomizeButtonWidget = nullptr; // Track the Randomize Button
        RC::Unreal::UObject* TargetPal = nullptr;

        std::wstring TargetInstanceID = L"";
        std::wstring TargetCharID = L"";
        std::wstring LastSelectedOption = L"";
        std::vector<ActiveSlider> ActiveSliders;

        bool bWasRandomizePressed = false; // Debounce state to prevent multi-triggering
    };
}