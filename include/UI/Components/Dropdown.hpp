// --- START OF FILE include/UI/Components/Dropdown.hpp ---
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <Unreal/UObjectGlobals.hpp>
#include "UI/WidgetBuilder.hpp" // <-- FIXED INCLUDE PATH
#include "UI/IconLibrary.hpp"   // <-- FIXED INCLUDE PATH
#include "Utils.hpp"

namespace DynPals::UI {

    class Dropdown {
    public:
        Dropdown(const std::vector<std::wstring>& InOptions, int InitialIndex = 0)
            : Options(InOptions), SelectedIndex(InitialIndex) {
            if (SelectedIndex < 0 || SelectedIndex >= static_cast<int>(Options.size())) {
                SelectedIndex = 0;
            }
        }

        Dropdown& OnChanged(std::function<void(int Index, std::wstring Option)> Callback) {
            OnSelectionChanged = Callback;
            return *this;
        }

        RC::Unreal::UObject* Build(RC::Unreal::UObject* Outer, RC::Unreal::UObject* PC) {
            OuterContext = Outer;
            PlayerController = PC;

            std::wstring DisplayText = Options.empty() ? L"" : Options[SelectedIndex];
            auto BtnBuilder = DynPals::UI::Button(Outer).Text(L"Select: " + DisplayText);
            
            MainButton = BtnBuilder.Build();
            return MainButton;
        }

        void Tick() {
            if (!MainButton) return;

            if (IsWidgetPressed(MainButton)) {
                if (!bWasMainBtnPressed) {
                    bWasMainBtnPressed = true;
                    if (!PopupOverlay) OpenPopup();
                    else ClosePopup();
                }
            } else { bWasMainBtnPressed = false; }

            if (PopupOverlay && ScrollBoxList && !bWasMainBtnPressed) {
                struct { int32_t RetVal; } CountParams{0};
                Utils::CallFunction(ScrollBoxList, STR("GetChildrenCount"), &CountParams);
                
                RC::Unreal::UFunction* GetChildFunc = ScrollBoxList->GetFunctionByNameInChain(STR("GetChildAt"));
                if (GetChildFunc) {
                    for (int32_t i = 0; i < CountParams.RetVal; ++i) {
                        struct { int32_t Index; RC::Unreal::UObject* RetVal; } GetChildParams{i, nullptr};
                        ScrollBoxList->ProcessEvent(GetChildFunc, &GetChildParams);
                        
                        RC::Unreal::UObject* ChildWidget = GetChildParams.RetVal;
                        if (!ChildWidget) continue;

                        std::wstring ChildClass = ChildWidget->GetClassPrivate()->GetName();
                        if (ChildClass.find(L"VerticalBox") != std::wstring::npos) continue;

                        if (IsWidgetPressed(ChildWidget)) {
                            SelectedIndex = i;
                            UpdateMainButtonText();
                            ClosePopup();
                            
                            if (OnSelectionChanged && i < static_cast<int32_t>(Options.size())) {
                                OnSelectionChanged(i, Options[i]);
                            }
                            break;
                        }
                    }
                }
            }
        }

        void ClosePopup() {
            if (PopupOverlay) {
                Utils::CallFunction(PopupOverlay, STR("RemoveFromParent"));
                PopupOverlay = nullptr;
                ScrollBoxList = nullptr;
            }
        }

        ~Dropdown() { ClosePopup(); }

    private:
        std::vector<std::wstring> Options;
        int SelectedIndex = 0;
        std::function<void(int, std::wstring)> OnSelectionChanged;

        RC::Unreal::UObject* OuterContext = nullptr;
        RC::Unreal::UObject* PlayerController = nullptr;

        RC::Unreal::UObject* MainButton = nullptr;
        RC::Unreal::UObject* PopupOverlay = nullptr;
        RC::Unreal::UObject* ScrollBoxList = nullptr;

        bool bWasMainBtnPressed = false;

        bool IsWidgetPressed(RC::Unreal::UObject* WidgetObj) const {
            if (!WidgetObj) return false;
            RC::Unreal::UObject* TargetBtn = WidgetObj;
            RC::Unreal::UObject* Temp = nullptr;

            if (Utils::GetPropertyValue(TargetBtn, STR("WBP_PalCommonButton"), Temp) && Temp) TargetBtn = Temp;
            if (Utils::GetPropertyValue(TargetBtn, STR("WBP_PalInvisibleButton"), Temp) && Temp) TargetBtn = Temp;

            struct { bool RetVal; } Params{false};
            Utils::CallFunction(TargetBtn, STR("IsPressed"), &Params);
            return Params.RetVal;
        }

        void UpdateMainButtonText() {
            if (!MainButton || Options.empty()) return;
            std::wstring newText = L"Select: " + Options[SelectedIndex];
            
            RC::Unreal::UObject* KTL = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
            struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } P1{ RC::Unreal::FString(newText.c_str()), RC::Unreal::FText() };
            KTL->ProcessEvent(KTL->GetFunctionByNameInChain(STR("Conv_StringToText")), &P1);
            
            struct { RC::Unreal::FText InText; } P2{P1.ReturnValue};
            Utils::CallFunction(MainButton, STR("SetText"), &P2);
        }

        void FixCommonButtonWidth(RC::Unreal::UObject* ButtonObj, float TargetWidth) {
            if (!ButtonObj) return;
            RC::Unreal::UObject* WidgetTree = nullptr;
            if (Utils::GetPropertyValue(ButtonObj, STR("WidgetTree"), WidgetTree) && WidgetTree) {
                RC::Unreal::TArray<RC::Unreal::UObject*> AllWidgets;
                RC::Unreal::UFunction* GetWidgetsFunc = WidgetTree->GetFunctionByNameInChain(STR("GetAllWidgets"));
                if (GetWidgetsFunc) {
                    struct { RC::Unreal::TArray<RC::Unreal::UObject*> OutWidgets; } Params;
                    WidgetTree->ProcessEvent(GetWidgetsFunc, &Params);
                    AllWidgets = Params.OutWidgets;
                } else {
                    Utils::GetPropertyValue(WidgetTree, STR("AllWidgets"), AllWidgets);
                }

                for (int32_t i = 0; i < AllWidgets.Num(); ++i) {
                    RC::Unreal::UObject* Child = AllWidgets[i];
                    if (!Child) continue;
                    std::wstring ClassName = Child->GetClassPrivate()->GetName();
                    
                    if (ClassName.find(L"SizeBox") != std::wstring::npos) {
                        Utils::CallFunction(Child, STR("ClearMaxDesiredWidth"));
                        Utils::CallFunction(Child, STR("ClearMinDesiredWidth"));
                        
                        struct { float W; } SizeParams{ TargetWidth };
                        Utils::CallFunction(Child, STR("SetWidthOverride"), &SizeParams);
                    }
                    if (ClassName.find(L"TextBlock") != std::wstring::npos || ClassName.find(L"PalTextBlock") != std::wstring::npos) {
                        float WrapAt = 0.0f;
                        Utils::SetPropertyValue<float>(Child, STR("WrapTextAt"), WrapAt);
                        Utils::SetPropertyValue<bool>(Child, STR("AutoWrapText"), false);
                        Utils::SetPropertyValue<bool>(Child, STR("IsAutoAdjustScale"), false);
                        Utils::SetPropertyValue<float>(Child, STR("MaxWidth"), TargetWidth);
                    }
                }
            }
        }

        void OpenPopup() {
            RC::Unreal::UObject* WidgetTree = nullptr;
            RC::Unreal::UObject* RootCanvas = nullptr;
            if (Utils::GetPropertyValue(OuterContext, STR("WidgetTree"), WidgetTree) && WidgetTree) {
                Utils::GetPropertyValue(WidgetTree, STR("RootWidget"), RootCanvas);
            }
            if (!RootCanvas) return;

            size_t maxLen = 0;
            for (const auto& opt : Options) {
                if (opt.length() > maxLen) maxLen = opt.length();
            }

            float ComputedWidth = static_cast<float>(maxLen) * 12.0f + 120.0f;
            if (ComputedWidth < 250.0f) ComputedWidth = 250.0f;
            if (ComputedWidth > 600.0f) ComputedWidth = 600.0f;

            float ComputedHeight = static_cast<float>(Options.size()) * 51.0f + 20.0f;
            if (ComputedHeight > 450.0f) ComputedHeight = 450.0f;

            auto ScrollBoxBuilder = DynPals::UI::ScrollBox(OuterContext);
            RC::Unreal::UObject* TargetWidget = nullptr;
            RC::Unreal::UObject* PalFont = Utils::LoadAssetSafely(DynPals::UI::Assets::Fonts::PalDefault);

            for (size_t i = 0; i < Options.size(); ++i) {
                const auto& opt = Options[i];
                bool isHeader = (opt.rfind(L"[", 0) == 0); 

                if (isHeader) {
                    std::wstring cleanHeader = opt;
                    if (cleanHeader.front() == L'[' && cleanHeader.back() == L']') {
                        cleanHeader = cleanHeader.substr(2, cleanHeader.length() - 4);
                    }

                    auto HeaderVBox = DynPals::UI::VerticalBox(OuterContext);
                    
                    auto TitleTxt = DynPals::UI::Text(OuterContext)
                        .Text(cleanHeader)
                        .Font(PalFont, L"Bold", 16)
                        .TextColor({0.063f, 0.725f, 0.506f, 1.0f}); 
                    
                    HeaderVBox.AddToVerticalBox(TitleTxt, [](DynPals::BoxSlotBuilder& Slot) {
                        Slot.Padding(10.0f, 14.0f, 10.0f, 4.0f);
                    });

                    auto DividerLine = DynPals::UI::SizeBox(OuterContext).HeightOverride(1.0f).AddChild(DynPals::UI::Border(OuterContext).BrushColor({0.063f, 0.725f, 0.506f, 0.3f}));
                    HeaderVBox.AddToVerticalBox(DividerLine, [](DynPals::BoxSlotBuilder& Slot) {
                        Slot.Padding(10.0f, 0.0f, 10.0f, 6.0f);
                    });

                    RC::Unreal::UObject* HeaderObj = HeaderVBox.Build();
                    struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{HeaderObj, nullptr};
                    Utils::CallFunction(ScrollBoxBuilder.Build(), STR("AddChild"), &AddParams);
                    continue; 
                }

                DynPals::WidgetBuilder Item(DynPals::UI::Assets::Blueprints::CommonButton, OuterContext);
                Item.Text(opt);
                Item.DesiredSizeOverride(ComputedWidth - 40.0f, 45.0f);
                Item.UnlockButtonSize(ComputedWidth - 60.0f); 
                
                RC::Unreal::UObject* ButtonObj = Item.Build();
                if (ButtonObj) {
                    if (static_cast<int>(i) == SelectedIndex) {
                        TargetWidget = ButtonObj;
                        Item.TextColor({0.024f, 0.714f, 0.831f, 1.0f});

                        RC::Unreal::UFunction* SetStateFunc = ButtonObj->GetFunctionByNameInChain(STR("SetState"));
                        if (SetStateFunc) {
                            struct { bool bState; } Params{ true };
                            ButtonObj->ProcessEvent(SetStateFunc, &Params);
                        } else {
                            struct { bool bInSelected; bool bGiveFeedback; } SelParams{ true, false };
                            Utils::CallFunction(ButtonObj, STR("SetIsSelected"), &SelParams);
                        }
                    } else {
                        Item.TextColor({0.9f, 0.9f, 0.9f, 1.0f});
                    }
                }
                
                struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{ButtonObj, nullptr};
                Utils::CallFunction(ScrollBoxBuilder.Build(), STR("AddChild"), &AddParams);

                if (AddParams.ReturnValue) {
                    DynPals::BoxSlotBuilder SlotBuilder(AddParams.ReturnValue);
                    SlotBuilder.Padding(0.0f, 0.0f, 0.0f, 6.0f);
                }
            }
            
            ScrollBoxList = ScrollBoxBuilder.Build();

            auto DropdownBuilder = DynPals::UI::SizeBox(OuterContext)
                .WidthOverride(ComputedWidth)
                .HeightOverride(ComputedHeight)
                .AddChild(
                    DynPals::UI::Border(OuterContext)
                        .ImageFromAsset(DynPals::UI::Assets::Borders::Frame1px)
                        .BrushColor({1.0f, 1.0f, 1.0f, 1.0f})
                        .AddChild(
                            DynPals::UI::Border(OuterContext)
                                .ImageFromAsset(DynPals::UI::Assets::Borders::WhiteSolid)
                                .BrushColor({0.01f, 0.01f, 0.01f, 0.98f})
                                .Padding(10.0f, 10.0f, 10.0f, 10.0f)
                                .AddChild(ScrollBoxBuilder)
                        )
                );

            PopupOverlay = DropdownBuilder.Build();

            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{PopupOverlay, nullptr};
            Utils::CallFunction(RootCanvas, STR("AddChild"), &AddParams);

            if (AddParams.ReturnValue) {
                struct { float LocationX; float LocationY; } MouseParams{0.0f, 0.0f};
                Utils::CallFunction(PlayerController, STR("GetMousePosition"), &MouseParams);

                DynPals::CanvasSlotBuilder SlotBuilder(AddParams.ReturnValue);
                SlotBuilder.Anchors(0.0, 0.0, 0.0, 0.0)
                           .Alignment(0.0, 0.0)
                           .Offsets(MouseParams.LocationX - 100.0f, MouseParams.LocationY - 20.0f, ComputedWidth, ComputedHeight) 
                           .AutoSize(false);
                
                struct { int32_t ZOrder; } ZParams{99};
                Utils::CallFunction(AddParams.ReturnValue, STR("SetZOrder"), &ZParams);
                
                if (TargetWidget) {
                    struct {
                        RC::Unreal::UObject* WidgetToFind;
                        bool bAnimateScroll;
                        uint8_t ScrollDestination;
                        float Padding;
                    } ScrollParams{TargetWidget, false, 2, 0.0f};

                    Utils::CallFunction(ScrollBoxList, STR("ScrollWidgetIntoView"), &ScrollParams);
                }
            }
        }
    };
}
// --- END OF FILE include/UI/Components/Dropdown.hpp ---