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
        std::unique_ptr<UI::Slider> SliderCtrl;
    };

    class UIManager : public UIBase {
    public:
        static UIManager& Get() {
            static UIManager instance;
            return instance;
        }

        void ToggleMenu() { RequestToggle(); }
        bool IsMenuOpen() const { return IsOpen(); }

    protected:
        virtual bool OnSetup() override;
        virtual void OnClose() override;
        virtual void BuildWidget() override;
        virtual void OnTickUI() override;

    private:
        UIManager() = default;
        
        void UpdateTarget();

        // State Tracking
        bool bHideInvalidSwaps = true; 
        float LastScrollOffset = 0.0f;
        
        // Target Context
        RC::Unreal::UObject* TargetPal = nullptr;
        std::wstring TargetInstanceID = L"";
        std::wstring TargetCharID = L"";

        // Smart Controllers
        std::unique_ptr<UI::Dropdown> SkinDropdown;
        std::unique_ptr<UI::Switch> HideInvalidSwitch;
        std::unique_ptr<UI::Button> RerollButton;
        std::vector<ActiveMorphSlider> MorphSliders;

        // Native Widget Pointers (For direct queries)
        RC::Unreal::UObject* MainScrollBoxObj = nullptr;
        RC::Unreal::UFunction* GetScrollOffsetFunc = nullptr; // Cached Pointer

        // Data Models
        std::vector<std::wstring> DropdownOptions;
        std::vector<int> DropdownConfigIndices;
    };
}