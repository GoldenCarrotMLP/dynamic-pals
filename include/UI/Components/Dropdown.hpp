// --- START OF FILE include/UI/Components/Dropdown.hpp ---
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <Unreal/UObjectGlobals.hpp>
#include "UI/WidgetBuilder.hpp" 
#include "UI/IconLibrary.hpp"   
#include "UI/Components/Button.hpp"
#include "Utils.hpp"

namespace DynPals::UI {

    class Dropdown {
    public:
        Dropdown(const std::vector<std::wstring>& InOptions, int InitialIndex = 0) {
            SetOptions(InOptions, InitialIndex);
        }

        Dropdown& OnChanged(std::function<void(int Index, std::wstring Option)> Callback) {
            OnSelectionChanged = Callback;
            return *this;
        }

        void SetOptions(const std::vector<std::wstring>& InOptions, int NewIndex) {
            Options = InOptions;
            SelectedIndex = NewIndex;
            if (SelectedIndex < 0 || SelectedIndex >= static_cast<int>(Options.size())) SelectedIndex = 0;
            
            bNeedsListRebuild = true;
            UpdateMainButtonText();

            // Calculate Max Width for pooling bounds
            MaxWidth = 250.0f;
            for (const auto& opt : Options) {
                float w = static_cast<float>(opt.length()) * 12.0f + 120.0f;
                if (w > MaxWidth) MaxWidth = w;
            }
            if (MaxWidth > 600.0f) MaxWidth = 600.0f;
        }

        RC::Unreal::UObject* Build(RC::Unreal::UObject* Outer, RC::Unreal::UObject* PC) {
            OuterContext = Outer;
            PlayerController = PC;

            std::wstring DisplayText = Options.empty() ? L"" : Options[SelectedIndex];
            MainButtonCtrl = std::make_unique<class DynPals::UI::Button>(Outer, L"Select: " + DisplayText);
            
            MainButtonCtrl->OnClicked([this]() {
                if (bIsPopupOpen) {
                    ClosePopup();
                } else {
                    OpenPopup();
                }
            });
            
            return MainButtonCtrl->GetWidget();
        }

        void Tick() {
            if (MainButtonCtrl) MainButtonCtrl->Tick();

            if (bIsPopupOpen) {
                for (auto& btnCtrl : AllButtonCtrls) {
                    btnCtrl->Tick();
                }
            }
        }

        void ClosePopup() {
            if (PopupOverlay) {
                struct { uint8_t InVisibility; } Params{ 1 }; // 1 = Collapsed
                Utils::CallFunction(PopupOverlay, STR("SetVisibility"), &Params);
                bIsPopupOpen = false;
            }
        }

        ~Dropdown() { 
            ClosePopup();
            if (PopupOverlay) {
                Utils::CallFunction(PopupOverlay, STR("RemoveFromParent"));
            }
        }

    private:
        std::vector<std::wstring> Options;
        int SelectedIndex = 0;
        float MaxWidth = 400.0f;
        bool bNeedsListRebuild = true;
        bool bIsPopupOpen = false; 
        std::function<void(int, std::wstring)> OnSelectionChanged;

        RC::Unreal::UObject* OuterContext = nullptr;
        RC::Unreal::UObject* PlayerController = nullptr;

        std::unique_ptr<class DynPals::UI::Button> MainButtonCtrl = nullptr;
        RC::Unreal::UObject* PopupOverlay = nullptr;
        RC::Unreal::UObject* ScrollBoxList = nullptr;
        RC::Unreal::UObject* TargetWidget = nullptr;

        // WIDGET POOLING STRUCTURES
        struct PooledHeader {
            RC::Unreal::UObject* RootWidget;
            RC::Unreal::UObject* TextWidget;
        };
        struct PooledButton {
            RC::Unreal::UObject* RootWidget;
            RC::Unreal::UObject* TextWidget;
            class DynPals::UI::Button* BtnCtrl; 
        };

        std::vector<PooledHeader> HeaderPool;
        std::vector<PooledButton> ButtonPool;
        std::vector<std::unique_ptr<class DynPals::UI::Button>> AllButtonCtrls;

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

        void RebuildList() {
            if (!ScrollBoxList) return;

            auto start = std::chrono::high_resolution_clock::now();

            Utils::CallFunction(ScrollBoxList, STR("ClearChildren"));

            int headerUsed = 0;
            int buttonUsed = 0;
            TargetWidget = nullptr;
            RC::Unreal::UObject* PalFont = Utils::LoadAssetSafely(DynPals::UI::Assets::Fonts::PalDefault);

            RC::Unreal::UObject* KTL = DynPals::Utils::GetKTL();
            RC::Unreal::UFunction* ConvFunc = DynPals::Utils::GetKTLFunction(STR("Conv_StringToText"));

            for (size_t i = 0; i < Options.size(); ++i) {
                const auto& opt = Options[i];
                bool isHeader = (opt.rfind(L"[", 0) == 0); 
                
                if (isHeader) {
                    std::wstring cleanHeader = opt;
                    if (cleanHeader.front() == L'[' && cleanHeader.back() == L']') {
                        cleanHeader = cleanHeader.substr(2, cleanHeader.length() - 4);
                    }

                    if (headerUsed >= HeaderPool.size()) {
                        auto TitleTxt = DynPals::UI::Text(OuterContext).Text(cleanHeader).Font(PalFont, L"Bold", 16).TextColor({0.063f, 0.725f, 0.506f, 1.0f}); 
                        RC::Unreal::UObject* TextObj = TitleTxt.Build();

                        auto HeaderVBox = DynPals::UI::VerticalBox(OuterContext);
                        HeaderVBox.AddToVerticalBox(DynPals::WidgetBuilder(TextObj), [](DynPals::BoxSlotBuilder& Slot) {
                            Slot.Padding(10.0f, 14.0f, 10.0f, 4.0f);
                        });

                        auto DividerLine = DynPals::UI::SizeBox(OuterContext).HeightOverride(1.0f).AddChild(DynPals::UI::Border(OuterContext).BrushColor({0.063f, 0.725f, 0.506f, 0.3f}));
                        HeaderVBox.AddToVerticalBox(DividerLine, [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(10.0f, 0.0f, 10.0f, 6.0f); });

                        RC::Unreal::UObject* RootObj = HeaderVBox.Build();
                        HeaderPool.push_back(PooledHeader{RootObj, TextObj});
                    }

                    PooledHeader& ph = HeaderPool[headerUsed++];
                    
                    if (ph.TextWidget && KTL && ConvFunc) {
                        struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } P1{ RC::Unreal::FString(cleanHeader.c_str()), RC::Unreal::FText() };
                        KTL->ProcessEvent(ConvFunc, &P1);
                        struct { RC::Unreal::FText InText; } P2{P1.ReturnValue};
                        Utils::CallFunction(ph.TextWidget, STR("SetText"), &P2, true);
                    }

                    struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{ph.RootWidget, nullptr};
                    Utils::CallFunction(ScrollBoxList, STR("AddChild"), &AddParams);

                } else {
                    if (buttonUsed >= ButtonPool.size()) {
                        DynPals::WidgetBuilder Item(DynPals::UI::Assets::Blueprints::CommonButton, OuterContext);
                        Item.DesiredSizeOverride(MaxWidth - 40.0f, 45.0f);
                        Item.UnlockButtonSize(MaxWidth - 60.0f); 
                        
                        RC::Unreal::UObject* RootObj = Item.Build();
                        RC::Unreal::UObject* TextObj = nullptr;
                        Utils::GetPropertyValue(RootObj, STR("Text_Main"), TextObj, true);

                        auto Ctrl = std::make_unique<class DynPals::UI::Button>(RootObj);
                        class DynPals::UI::Button* RawCtrl = Ctrl.get();
                        AllButtonCtrls.push_back(std::move(Ctrl));
                        
                        ButtonPool.push_back(PooledButton{RootObj, TextObj, RawCtrl});
                    }

                    PooledButton& pb = ButtonPool[buttonUsed++];
                    
                    if (pb.TextWidget && KTL && ConvFunc) {
                        struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } P1{ RC::Unreal::FString(opt.c_str()), RC::Unreal::FText() };
                        KTL->ProcessEvent(ConvFunc, &P1);
                        struct { RC::Unreal::FText InText; } P2{P1.ReturnValue};
                        Utils::CallFunction(pb.TextWidget, STR("SetText"), &P2, true);
                    }

                    if (static_cast<int>(i) == SelectedIndex) {
                        TargetWidget = pb.RootWidget;
                        DynPals::UI::SetTextColor(pb.TextWidget, {0.024f, 0.714f, 0.831f, 1.0f});

                        RC::Unreal::UFunction* SetStateFunc = pb.RootWidget->GetFunctionByNameInChain(STR("SetState"));
                        if (SetStateFunc) {
                            struct { bool bState; } Params{ true };
                            pb.RootWidget->ProcessEvent(SetStateFunc, &Params);
                        } else {
                            RC::Unreal::UObject* TargetBtn = pb.RootWidget;
                            RC::Unreal::UObject* InnerBtn = nullptr;
                            if (Utils::GetPropertyValue<RC::Unreal::UObject*>(pb.RootWidget, STR("WBP_PalInvisibleButton"), InnerBtn, true) && InnerBtn) TargetBtn = InnerBtn;
                            struct { bool bInSelected; bool bGiveFeedback; } SelParams{ true, false };
                            Utils::CallFunction(TargetBtn, STR("SetIsSelected"), &SelParams, true);
                        }
                    } else {
                        DynPals::UI::SetTextColor(pb.TextWidget, {0.9f, 0.9f, 0.9f, 1.0f});
                        RC::Unreal::UFunction* SetStateFunc = pb.RootWidget->GetFunctionByNameInChain(STR("SetState"));
                        if (SetStateFunc) {
                            struct { bool bState; } Params{ false };
                            pb.RootWidget->ProcessEvent(SetStateFunc, &Params);
                        } else {
                            RC::Unreal::UObject* TargetBtn = pb.RootWidget;
                            RC::Unreal::UObject* InnerBtn = nullptr;
                            if (Utils::GetPropertyValue<RC::Unreal::UObject*>(pb.RootWidget, STR("WBP_PalInvisibleButton"), InnerBtn, true) && InnerBtn) TargetBtn = InnerBtn;
                            struct { bool bInSelected; bool bGiveFeedback; } SelParams{ false, false };
                            Utils::CallFunction(TargetBtn, STR("SetIsSelected"), &SelParams, true);
                        }
                    }

                    int itemIndex = static_cast<int>(i);
                    pb.BtnCtrl->OnClicked([this, itemIndex]() {
                        SelectedIndex = itemIndex;
                        UpdateMainButtonText();
                        ClosePopup();
                        if (OnSelectionChanged && itemIndex < static_cast<int>(Options.size())) {
                            OnSelectionChanged(itemIndex, Options[itemIndex]);
                        }
                    });

                    struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{pb.RootWidget, nullptr};
                    Utils::CallFunction(ScrollBoxList, STR("AddChild"), &AddParams);

                    if (AddParams.ReturnValue) {
                        DynPals::BoxSlotBuilder SlotBuilder(AddParams.ReturnValue);
                        SlotBuilder.Padding(0.0f, 0.0f, 0.0f, 6.0f);
                    }
                }
            }

            bNeedsListRebuild = false;

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            DP_LOG(Default, "[Profile] Dropdown::RebuildList recycled {} options in {} us ({:.3f} ms)", 
                   Options.size(), duration, duration / 1000.0f);
        }

        void OpenPopup() {
            if (!PopupOverlay) {
                float ComputedHeight = 450.0f;
                
                auto ScrollBoxBuilder = DynPals::UI::ScrollBox(OuterContext);
                ScrollBoxList = ScrollBoxBuilder.Build();

                auto DropdownBuilder = DynPals::UI::SizeBox(OuterContext)
                    .WidthOverride(MaxWidth)
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
                                    .AddChild(DynPals::WidgetBuilder(ScrollBoxList))
                            )
                    );

                PopupOverlay = DropdownBuilder.Build();

                RC::Unreal::UObject* WidgetTree = nullptr;
                RC::Unreal::UObject* RootCanvas = nullptr;
                if (Utils::GetPropertyValue(OuterContext, STR("WidgetTree"), WidgetTree, true) && WidgetTree) {
                    Utils::GetPropertyValue(WidgetTree, STR("RootWidget"), RootCanvas, true);
                }

                if (RootCanvas) {
                    struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{PopupOverlay, nullptr};
                    Utils::CallFunction(RootCanvas, STR("AddChild"), &AddParams);

                    if (AddParams.ReturnValue) {
                        RC::Unreal::UObject* WLL = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetLayoutLibrary"));
                        RC::Unreal::UFunction* GetMouseFunc = WLL ? WLL->GetFunctionByNameInChain(STR("GetMousePositionOnViewport")) : nullptr;
                        
                        struct FVector2D_Double { double X; double Y; };
                        struct { RC::Unreal::UObject* WorldContextObject; FVector2D_Double ReturnValue; } MouseParams{ PlayerController, {0.0, 0.0} };

                        if (WLL && GetMouseFunc) {
                            WLL->ProcessEvent(GetMouseFunc, &MouseParams);
                        }

                        float FinalLeft = static_cast<float>(MouseParams.ReturnValue.X) - 100.0f;
                        float FinalTop = static_cast<float>(MouseParams.ReturnValue.Y) - 20.0f;

                        DynPals::CanvasSlotBuilder SlotBuilder(AddParams.ReturnValue);
                        SlotBuilder.Anchors(0.0, 0.0, 0.0, 0.0)
                                   .Alignment(0.0, 0.0)
                                   .Offsets(FinalLeft, FinalTop, MaxWidth, ComputedHeight) 
                                   .AutoSize(false);
                        
                        struct { int32_t ZOrder; } ZParams{99};
                        Utils::CallFunction(AddParams.ReturnValue, STR("SetZOrder"), &ZParams);
                    }
                }
            } else {
                struct { float W; } Params{MaxWidth};
                Utils::CallFunction(PopupOverlay, STR("SetWidthOverride"), &Params);
            }

            if (bNeedsListRebuild) RebuildList();
            
            struct { uint8_t InVisibility; } VisParams{ 0 }; // 0 = Visible
            Utils::CallFunction(PopupOverlay, STR("SetVisibility"), &VisParams);
            bIsPopupOpen = true;

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
    };
}
// --- END OF FILE include/UI/Components/Dropdown.hpp ---