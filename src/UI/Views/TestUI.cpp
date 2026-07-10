#define NOMINMAX 
#include <Windows.h>

#include "UI/Views/TestUI.hpp"
#include "UI/Components/WindowFrame.hpp"
#include "UI/WidgetBuilder.hpp"
#include "UI/IconLibrary.hpp"
#include "Utils.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    static bool IsWidgetPressed(RC::Unreal::UObject* Widget) {
        if (!Widget) return false;
        RC::Unreal::UObject* TargetBtn = Widget;
        RC::Unreal::UObject* Temp = nullptr;

        if (Utils::GetPropertyValue(TargetBtn, STR("WBP_PalCommonButton"), Temp) && Temp) TargetBtn = Temp;
        if (Utils::GetPropertyValue(TargetBtn, STR("WBP_PalInvisibleButton"), Temp) && Temp) TargetBtn = Temp;

        struct { bool RetVal; } Params{false};
        Utils::CallFunction(TargetBtn, STR("IsPressed"), &Params);
        return Params.RetVal;
    }

    void TestUI::BuildWidget() {
        if (!CurrentPlayerController) return;

        DP_LOG(Default, "[TestUI Debug] Executing BuildWidget... (ActiveTab = {})", ActiveTab);

        TabBtn1 = nullptr;
        TabBtn2 = nullptr;
        TestSlider = nullptr;
        TestSwitch = nullptr;
        TestLR = nullptr;
        HighlightButton = nullptr;
        TextBlocks.clear();
        RowIcons.clear();

        if (DropdownOptions.empty()) {
            DropdownOptions = {
                L"Option A", L"Option B", L"Option C", L"Option D", L"Option E",
                L"Option F", L"Option G", L"Option H", L"Option I", L"Option J",
                L"Option K", L"Option L", L"Option M", L"Option N", L"Option O",
                L"Option P", L"Option Q", L"Option R", L"Option S", L"Option T",
                L"Option U", L"Option V", L"Option W", L"Option X", L"Option Y",
                L"Option Z"
            };
        }

        UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        UClass* WidgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (!WBL || !WidgetClass) return;

        struct { UObject* WorldContext; UClass* WidgetType; UObject* OwningPlayer; UObject* ReturnValue; } CreateParams{
            CurrentPlayerController, WidgetClass, CurrentPlayerController, nullptr
        };
        Utils::CallFunction(WBL, STR("Create"), &CreateParams);
        MyWidget = CreateParams.ReturnValue;

        UObject* PalFont = Utils::LoadAssetSafely(UI::Assets::Fonts::PalDefault);
        const FLinearColor_UE5 PalBlue = {0.78f, 0.96f, 1.0f, 1.0f}; 
        const FLinearColor_UE5 White   = {1.0f, 1.0f, 1.0f, 1.0f};

        auto HeaderBox = UI::HorizontalBox(MyWidget)
            .AddToHorizontalBox(UI::Image(MyWidget).ImageFromAsset(UI::Assets::Common::NoticeMark).ImageColor(PalBlue).ImageSize(24, 24), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); })
            .AddToHorizontalBox(UI::Text(MyWidget).Text(L"DYN PALS SYSTEM TEST").Font(PalFont, L"Bold", 24).TextOutline(2, {0.0f, 0.0f, 0.0f, 1.0f}).TextColor(PalBlue));

        auto TabLayout = UI::HorizontalBox(MyWidget);
        auto Tab1Builder = UI::OptionTab(MyWidget).SetupTab(L"Settings", 0).SetTabActive(ActiveTab == 0);
        auto Tab2Builder = UI::OptionTab(MyWidget).SetupTab(L"Visuals", 1).SetTabActive(ActiveTab == 1);

        UObject* Tab1Widget = Tab1Builder.Build();
        UObject* Tab2Widget = Tab2Builder.Build();

        if (ActiveTab == 0) Utils::CallFunction(Tab1Widget, STR("SetTabActive"), &bHighlight); 
        else Utils::CallFunction(Tab2Widget, STR("SetTabActive"), &bHighlight);

        TabBtn1 = std::make_unique<UI::Button>(Tab1Widget);
        TabBtn1->OnClicked([this]() {
            if (ActiveTab != 0) { ActiveTab = 0; if (TestDropdown) TestDropdown->ClosePopup(); RequestRebuild(); }
        });

        TabBtn2 = std::make_unique<UI::Button>(Tab2Widget);
        TabBtn2->OnClicked([this]() {
            if (ActiveTab != 1) { ActiveTab = 1; RequestRebuild(); }
        });

        TabLayout.AddToHorizontalBox(Tab1Builder, [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0); }).AddToHorizontalBox(Tab2Builder);

        auto ContentContainer = UI::VerticalBox(MyWidget);

        if (ActiveTab == 0) {
            auto ButtonBuilder = WidgetBuilder(UI::Assets::Blueprints::CommonButton, MyWidget)
                .Text(L"Highlight every second element")
                .DesiredSizeOverride(400.0f, 45.0f)
                .UnlockButtonSize(400.0f); // Unlocks internal truncation limits
            
            UObject* ButtonRoot = ButtonBuilder.Build();

            HighlightButton = std::make_unique<UI::Button>(ButtonRoot);
            HighlightButton->OnClicked([this]() {
                bHighlight = !bHighlight; 

                const FLinearColor_UE5 PalBlue = {0.78f, 0.96f, 1.0f, 1.0f};
                const FLinearColor_UE5 White   = {1.0f, 1.0f, 1.0f, 1.0f};
                const FLinearColor_UE5 DarkOut = {0.0f, 0.0f, 0.0f, 0.8f};
                const FLinearColor_UE5 SoftOut = {0.0f, 0.0f, 0.0f, 0.5f};

                for (size_t i = 0; i < TextBlocks.size(); ++i) {
                    UObject* TextBlock = TextBlocks[i];
                    UObject* RowIcon   = RowIcons[i];
                    
                    if (TextBlock && RowIcon) {
                        bool bCondition = bHighlight && ((i + 1) % 2 == 0);
                        UI::SetTextColor(TextBlock, bCondition ? PalBlue : White);
                        UI::SetFontData(TextBlock, bCondition ? 32 : 22, bCondition ? DarkOut : SoftOut);
                        UI::SetImageColor(RowIcon, bCondition ? PalBlue : White);
                    }
                }
            });

            auto ListBoxBuilder = UI::VerticalBox(MyWidget);
            std::vector<std::wstring> labels = { L"A", L"B", L"C", L"D", L"E", L"F" };
            for (size_t i = 0; i < labels.size(); ++i) {
                auto RowIcon = UI::Image(MyWidget).ImageFromAsset(UI::Assets::Common::StatusArrow).ImageColor(White).ImageSize(16, 16);
                auto TextWidget = UI::Text(MyWidget).Text(L"this is text " + labels[i]).Font(PalFont, L"Medium", 20).TextOutline(1, {0.0f, 0.0f, 0.0f, 0.5f}).TextColor(White);
                TextBlocks.push_back(TextWidget.Build());
                RowIcons.push_back(RowIcon.Build());

                auto RowLayout = UI::HorizontalBox(MyWidget)
                    .AddToHorizontalBox(RowIcon, [](BoxSlotBuilder& Slot) { Slot.Padding(0.0f, 0.0f, 12.0f, 0.0f).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); })
                    .AddToHorizontalBox(TextWidget, [](BoxSlotBuilder& Slot) { Slot.VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); });
                ListBoxBuilder.AddToVerticalBox(RowLayout, [](BoxSlotBuilder& Slot) { Slot.Padding(0.0f, 6.0f, 0.0f, 6.0f); });
            }

            ContentContainer
                .AddToVerticalBox(ButtonBuilder, [](BoxSlotBuilder& Slot) { Slot.Padding(0.0f, 0.0f, 0.0f, 20.0f).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); })
                .AddToVerticalBox(ListBoxBuilder);

        } else if (ActiveTab == 1) {
            TestSlider = std::make_unique<UI::Slider>(MyWidget, 0.0, 100.0, 50.0);
            TestSwitch = std::make_unique<UI::Switch>(MyWidget, true);
            TestLR = std::make_unique<UI::Selector>(MyWidget, std::vector<std::wstring>{L"Low", L"Medium", L"High", L"Epic"}, 2);

            RC::Unreal::UObject* WidgetTrashBin = UI::VerticalBox(MyWidget).Build();
            struct { uint8_t InVisibility; } VisParams{ 1 };
            Utils::CallFunction(WidgetTrashBin, STR("SetVisibility"), &VisParams);

            if (!TestDropdown) {
                int initialIdx = 0;
                for (size_t i = 0; i < DropdownOptions.size(); ++i) {
                    if (DropdownOptions[i] == CurrentDropdownChoice) {
                        initialIdx = static_cast<int>(i);
                        break;
                    }
                }
                TestDropdown = std::make_unique<UI::Dropdown>(DropdownOptions, initialIdx);
                TestDropdown->OnChanged([this](int Index, std::wstring Choice) {
                    CurrentDropdownChoice = Choice;
                });
            }
            TestDropdown->SetTrashBin(WidgetTrashBin);

            UObject* NativeDropdownWidget = TestDropdown->Build(MyWidget, CurrentPlayerController);

            ContentContainer
                .AddToVerticalBox(UI::Text(MyWidget).Text(L"Option Slider").Font(PalFont, L"Medium", 18))
                .AddToVerticalBox(WidgetBuilder(TestSlider->GetWidget()), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 5, 0, 20); })
                .AddToVerticalBox(UI::Text(MyWidget).Text(L"Option Switch").Font(PalFont, L"Medium", 18))
                .AddToVerticalBox(WidgetBuilder(TestSwitch->GetWidget()), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 5, 0, 20); })
                .AddToVerticalBox(UI::Text(MyWidget).Text(L"Left/Right Selector").Font(PalFont, L"Medium", 18))
                .AddToVerticalBox(WidgetBuilder(TestLR->GetWidget()), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 5, 0, 20); })
                .AddToVerticalBox(UI::Text(MyWidget).Text(L"Native Overlay Dropdown").Font(PalFont, L"Medium", 18))
                .AddToVerticalBox(WidgetBuilder(L"/Script/UMG.SizeBox", MyWidget).AddChild(WidgetBuilder(NativeDropdownWidget)), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 5, 0, 20); });
        }

        UObject* Canvas = UI::WindowFrame(MyWidget, 600.0f)
            .SetHeader(HeaderBox)
            .AddAutoScaledRow(TabLayout) 
            .AddContent(ContentContainer)
            .SetFooter(UI::ActionBar(MyWidget))
            .Build();

        UObject* WidgetTree = nullptr;
        if (Utils::GetPropertyValue(MyWidget, STR("WidgetTree"), WidgetTree) && WidgetTree) {
            FProperty* RootProp = Utils::GetProperty(WidgetTree, STR("RootWidget"));
            if (RootProp) *RootProp->ContainerPtrToValuePtr<UObject*>(WidgetTree) = Canvas;
        }

        //Utils::CallFunction(MyWidget, STR("Initialize")); //Redundant
        struct { int32_t ZOrder; } ViewportParams{9999};
        Utils::CallFunction(MyWidget, STR("AddToViewport"), &ViewportParams);
    }

    void TestUI::OnTickUI() {
        if (TabBtn1) TabBtn1->Tick();
        if (TabBtn2) TabBtn2->Tick();

        if (ActiveTab == 0) {
            if (HighlightButton) HighlightButton->Tick();
        } else if (ActiveTab == 1) {
            if (TestSlider)   TestSlider->Tick();
            if (TestSwitch)   TestSwitch->Tick();
            if (TestLR)       TestLR->Tick();
            if (TestDropdown) TestDropdown->Tick();
        }
    }
}