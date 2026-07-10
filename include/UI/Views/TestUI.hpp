#pragma once
#include "UI/UIBase.hpp"
#include "UI/Components/Button.hpp"
#include "UI/Components/Dropdown.hpp"
#include "UI/Components/Slider.hpp"
#include "UI/Components/Switch.hpp"
#include "UI/Components/Selector.hpp"
#include <vector>
#include <string>
#include <memory>

namespace DynPals {

    class TestUI : public UIBase {
    public:
        static TestUI& Get() {
            static TestUI instance;
            return instance;
        }

    protected:
        virtual void BuildWidget() override;
        virtual void OnTickUI() override;

    private:
        TestUI() { bCloseOnEscape = true; }


        // Tab State Tracking
        int32_t ActiveTab = 0;
        std::unique_ptr<UI::Button> TabBtn1;
        std::unique_ptr<UI::Button> TabBtn2;

        // Tab 0: Vanilla Controls Elements
        std::unique_ptr<UI::Button> HighlightButton;
        std::vector<RC::Unreal::UObject*> TextBlocks;
        std::vector<RC::Unreal::UObject*> RowIcons;
        bool bHighlight = false;

        // Tab 1: Native Options Elements
        std::unique_ptr<UI::Slider> TestSlider;
        std::unique_ptr<UI::Switch> TestSwitch;
        std::unique_ptr<UI::Selector> TestLR;

        // Tab 1: Native Select List Popup
        std::unique_ptr<UI::Dropdown> TestDropdown;
        std::wstring CurrentDropdownChoice = L"Option A";
        std::vector<std::wstring> DropdownOptions;
    };
}