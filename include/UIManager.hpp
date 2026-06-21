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
        void TickUI(RC::Unreal::UObject* LocalPlayerController); // Updated Signature!
        
        void RequestMenuToggle() { bToggleRequested = true; }

        // Getters to allow safe, zero-overhead Game Thread tick suspension
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

        std::atomic<bool> bToggleRequested{false};
        bool bIsMenuOpen = false;
        bool bHideInvalidSwaps = true; 
        
        RC::Unreal::UObject* CurrentPlayerController = nullptr; // Saves the native player controller
        RC::Unreal::UObject* MyWidget = nullptr;
        RC::Unreal::UObject* ComboBoxWidget = nullptr;
        RC::Unreal::UObject* CheckBoxWidget = nullptr; 
        RC::Unreal::UObject* RandomizeButtonWidget = nullptr; 
        RC::Unreal::UObject* TargetPal = nullptr;

        std::wstring TargetInstanceID = L"";
        std::wstring TargetCharID = L"";
        std::wstring LastSelectedOption = L"";
        std::vector<ActiveSlider> ActiveSliders;

        bool bWasRandomizePressed = false; 
    };
}