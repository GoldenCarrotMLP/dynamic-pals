#pragma once
#include "UI/UIBase.hpp"
#include "UI/Components/Button.hpp"
#include "UI/Components/Dropdown.hpp"
#include "UI/Components/Slider.hpp"
#include "UI/Components/Switch.hpp"
#include <string>
#include <vector>
#include <memory>
#include <Unreal/UObjectGlobals.hpp>

namespace DynPals {

    struct ActiveMorphSlider {
        std::wstring MorphTargetName;
        std::unique_ptr<class DynPals::UI::Slider> SliderCtrl;
    };

    class UIManager : public UIBase {
    public:
        static UIManager& Get() {
            static UIManager instance;
            return instance;
        }

        void ToggleMenu() { RequestToggle(); }
        bool IsMenuOpen() const { return IsOpen(); }

        void PreloadUI(RC::Unreal::UObject* PC);
        virtual RC::Unreal::UObject* GetDesiredFocusTarget() const override;

    protected:
        virtual bool OnSetup() override;
        virtual void OnOpen() override;
        virtual void OnClose() override;
        virtual void OnInvalidate() override;
        virtual void BuildWidget() override;
        virtual void OnTickUI() override;

    private:
        UIManager() { bCloseOnEscape = true; }
        
        RC::Unreal::UObject* PreloadContainer = nullptr;
        
        void UpdateTarget();
        void RefreshUI();
        
        // --- Camera Helper Methods ---
        void EnablePalCamera();
        void DisablePalCamera();
        void UpdatePalCameraRotation(double Yaw);

        // State Tracking
        bool bHideInvalidSwaps = true; 
        float LastScrollOffset = 0.0f;
        bool bNeedsRefresh = false;
        
        // Target Context
        RC::Unreal::UObject* TargetPal = nullptr;
        std::wstring TargetInstanceID = L"";
        std::wstring TargetCharID = L"";

        // Smart Controllers
        std::unique_ptr<class DynPals::UI::Dropdown> SkinDropdown;
        std::unique_ptr<class DynPals::UI::Switch> HideInvalidSwitch;
        std::unique_ptr<class DynPals::UI::Button> RerollButton;
        std::vector<std::unique_ptr<class DynPals::UI::Slider>> MorphSliderPool;
        int ActiveMorphSlidersCount = 0;
        
        // UI Elements
        std::unique_ptr<class DynPals::UI::Switch> FocusPalSwitch;
        std::unique_ptr<class DynPals::UI::Slider> CameraRotationSlider;

        // Native Widget Persistent Containers
        RC::Unreal::UObject* MainScrollBoxObj = nullptr;
        RC::Unreal::UFunction* GetScrollOffsetFunc = nullptr; 
        
        RC::Unreal::UObject* DynamicMorphBox = nullptr;
        RC::Unreal::UObject* DynamicLogBox = nullptr;
        RC::Unreal::UObject* CameraRotationContainer = nullptr;
        RC::Unreal::UObject* PalFontCache = nullptr;
        
        RC::Unreal::UObject* HeaderTextObj = nullptr;
        
        RC::Unreal::UObject* WidgetTrashBin = nullptr; // Hidden container to prevent GC
        // Data Models
        std::vector<std::wstring> DropdownOptions;
        std::vector<int> DropdownConfigIndices;

        // Text Widget Pool for Logging
        std::vector<RC::Unreal::UObject*> LogTextPool;

        // Camera Members
        RC::Unreal::UObject* OriginalViewTarget = nullptr; 
        bool bIsPalCameraActive = false;                   
    };
}