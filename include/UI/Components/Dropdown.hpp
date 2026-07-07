// --- START OF FILE include/UI/Components/Dropdown.hpp ---
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <Unreal/UObjectGlobals.hpp>
#include "UI/WidgetBuilder.hpp" 
#include "UI/IconLibrary.hpp"   
#include "UI/Components/Button.hpp"
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
            MainButtonCtrl = std::make_unique<UI::Button>(Outer, L"Select: " + DisplayText);
            
            MainButtonCtrl->OnClicked([this]() {
                if (!PopupOverlay) OpenPopup();
                else ClosePopup();
            });
            
            return MainButtonCtrl->GetWidget();
        }

        void Tick() {
            if (MainButtonCtrl) {
                MainButtonCtrl->Tick();
            }

            if (PopupOverlay) {
                for (auto& btnCtrl : PopupItems) {
                    btnCtrl->Tick();
                }
            }
        }

        void ClosePopup() {
            if (PopupOverlay) {
                Utils::CallFunction(PopupOverlay, STR("RemoveFromParent"));
                PopupOverlay = nullptr;
                ScrollBoxList = nullptr;
                PopupItems.clear(); // Releases child button trackers cleanly
            }
        }

        ~Dropdown() { ClosePopup(); }

    private:
        std::vector<std::wstring> Options;
        int SelectedIndex = 0;
        std::function<void(int, std::wstring)> OnSelectionChanged;

        RC::Unreal::UObject* OuterContext = nullptr;
        RC::Unreal::UObject* PlayerController = nullptr;

        std::unique_ptr<UI::Button> MainButtonCtrl = nullptr;
        RC::Unreal::UObject* PopupOverlay = nullptr;
        RC::Unreal::UObject* ScrollBoxList = nullptr;
        
        std::vector<std::unique_ptr<UI::Button>> PopupItems;

        void UpdateMainButtonText() {
            if (!MainButtonCtrl || Options.empty()) return;
            std::wstring newText = L"Select: " + Options[SelectedIndex];
            
            RC::Unreal::UObject* KTL = DynPals::Utils::GetKTL();
            RC::Unreal::UFunction* ConvFunc = DynPals::Utils::GetKTLFunction(STR("Conv_StringToText"));
            if (KTL && ConvFunc) {
                struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } P1{ RC::Unreal::FString(newText.c_str()), RC::Unreal::FText() };
                KTL->ProcessEvent(ConvFunc, &P1);
                
                struct { RC::Unreal::FText InText; } P2{P1.ReturnValue};
                Utils::CallFunction(MainButtonCtrl->GetWidget(), STR("SetText"), &P2, true);
            }
        }

        void OpenPopup() {
            DP_LOG(Default, "[Dropdown Debug] --- OpenPopup Invoked ---");
            DP_LOG(Default, "[Dropdown Debug] Options count: {}", Options.size());
            PopupItems.clear();

            RC::Unreal::UObject* WidgetTree = nullptr;
            RC::Unreal::UObject* RootCanvas = nullptr;
            if (Utils::GetPropertyValue(OuterContext, STR("WidgetTree"), WidgetTree, true) && WidgetTree) {
                Utils::GetPropertyValue(WidgetTree, STR("RootWidget"), RootCanvas, true);
            }

            DP_LOG(Default, "[Dropdown Debug] Context Validation: OuterContext = 0x{:p} ('{}'), WidgetTree = 0x{:p}, RootCanvas = 0x{:p} ('{}')", 
                   (void*)OuterContext, OuterContext ? OuterContext->GetClassPrivate()->GetName() : L"NULL",
                   (void*)WidgetTree, 
                   (void*)RootCanvas, RootCanvas ? RootCanvas->GetClassPrivate()->GetName() : L"NULL");

            if (!RootCanvas) {
                DP_LOG(Warning, "[Dropdown Debug] ABORTED: RootCanvas was NULL or unreachable.");
                return;
            }

            size_t maxLen = 0;
            for (const auto& opt : Options) {
                if (opt.length() > maxLen) maxLen = opt.length();
            }

            float ComputedWidth = static_cast<float>(maxLen) * 12.0f + 120.0f;
            if (ComputedWidth < 250.0f) ComputedWidth = 250.0f;
            if (ComputedWidth > 600.0f) ComputedWidth = 600.0f;

            float ComputedHeight = static_cast<float>(Options.size()) * 51.0f + 20.0f;
            if (ComputedHeight > 450.0f) ComputedHeight = 450.0f;

            DP_LOG(Default, "[Dropdown Debug] Dimensions Calculated: Width = {:.2f}, Height = {:.2f}", ComputedWidth, ComputedHeight);

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
                            RC::Unreal::UObject* TargetBtn = ButtonObj;
                            RC::Unreal::UObject* InnerBtn = nullptr;
                            if (Utils::GetPropertyValue<RC::Unreal::UObject*>(ButtonObj, STR("WBP_PalInvisibleButton"), InnerBtn, true) && InnerBtn) {
                                TargetBtn = InnerBtn;
                            }

                            struct { bool bInSelected; bool bGiveFeedback; } SelParams{ true, false };
                            Utils::CallFunction(TargetBtn, STR("SetIsSelected"), &SelParams, true);
                        }
                    } else {
                        Item.TextColor({0.9f, 0.9f, 0.9f, 1.0f});
                    }

                    auto ItemBtnCtrl = std::make_unique<UI::Button>(ButtonObj);
                    int itemIndex = static_cast<int>(i);
                    ItemBtnCtrl->OnClicked([this, itemIndex]() {
                        SelectedIndex = itemIndex;
                        UpdateMainButtonText();
                        ClosePopup();
                        
                        if (OnSelectionChanged && itemIndex < static_cast<int32_t>(Options.size())) {
                            OnSelectionChanged(itemIndex, Options[itemIndex]);
                        }
                    });
                    PopupItems.push_back(std::move(ItemBtnCtrl));
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
            DP_LOG(Default, "[Dropdown Debug] PopupOverlay Built: 0x{:p} ('{}')", (void*)PopupOverlay, PopupOverlay ? PopupOverlay->GetClassPrivate()->GetName() : L"NULL");

            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{PopupOverlay, nullptr};
            Utils::CallFunction(RootCanvas, STR("AddChild"), &AddParams);

            DP_LOG(Default, "[Dropdown Debug] Mount Result: CanvasSlot = 0x{:p}", (void*)AddParams.ReturnValue);

            if (AddParams.ReturnValue) {
                // 1. Fetch the WidgetLayoutLibrary CDO
                RC::Unreal::UObject* WLL = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetLayoutLibrary"));
                RC::Unreal::UFunction* GetMouseFunc = WLL ? WLL->GetFunctionByNameInChain(STR("GetMousePositionOnViewport")) : nullptr;
                
                // 2. Map the parameters block matching UE5 double-precision FVector2D (double X, double Y)
                struct FVector2D_Double { double X; double Y; };
                struct { RC::Unreal::UObject* WorldContextObject; FVector2D_Double ReturnValue; } MouseParams{ PlayerController, {0.0, 0.0} };

                if (WLL && GetMouseFunc) {
                    WLL->ProcessEvent(GetMouseFunc, &MouseParams);
                }

                DP_LOG(Default, "[Dropdown Debug] GetMousePositionOnViewport: X = {:.2f}, Y = {:.2f}", 
                       MouseParams.ReturnValue.X, MouseParams.ReturnValue.Y);

                // 3. Compute final slot coordinates (DPI-scaled matching Canvas Slot requirements) [5]
                float FinalLeft = static_cast<float>(MouseParams.ReturnValue.X) - 100.0f;
                float FinalTop = static_cast<float>(MouseParams.ReturnValue.Y) - 20.0f;

                DP_LOG(Default, "[Dropdown Debug] SetOffsets Parameter Block: Left = {:.2f}, Top = {:.2f}, Width = {:.2f}, Height = {:.2f}", 
                       FinalLeft, FinalTop, ComputedWidth, ComputedHeight);

                DynPals::CanvasSlotBuilder SlotBuilder(AddParams.ReturnValue);
                SlotBuilder.Anchors(0.0, 0.0, 0.0, 0.0)
                           .Alignment(0.0, 0.0)
                           .Offsets(FinalLeft, FinalTop, ComputedWidth, ComputedHeight) 
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
                    DP_LOG(Default, "[Dropdown Debug] List Selection Auto-Scrolled.");
                }
            } else {
                DP_LOG(Warning, "[Dropdown Debug] FAILED: AddChild failed to return a valid Slot pointer!");

            }

        }
    };
}
// --- END OF FILE include/UI/Components/Dropdown.hpp ---