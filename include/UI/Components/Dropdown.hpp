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
        struct PooledHeader {
            RC::Unreal::UObject* RootWidget;
            RC::Unreal::UObject* TextWidget;
        };
        struct PooledButton {
            RC::Unreal::UObject* RootWidget;
            RC::Unreal::UObject* TextWidget;
            class DynPals::UI::Button* BtnCtrl; 
        };

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

            MaxWidth = 250.0f;
            for (const auto& opt : Options) {
                float w = static_cast<float>(opt.length()) * 12.0f + 120.0f;
                if (w > MaxWidth) MaxWidth = w;
            }
            if (MaxWidth > 600.0f) MaxWidth = 600.0f;
        }
        
        void SetTrashBin(RC::Unreal::UObject* Bin) {
            DropdownTrashBin = Bin;
        }

        void PreloadPool(RC::Unreal::UObject* Outer, int btnCount, int headerCount = 10) {
            OuterContext = Outer;
            
            RC::Unreal::UObject* PalFont = Utils::LoadAssetSafely(DynPals::UI::Assets::Fonts::PalDefault);
            for (int i = 0; i < headerCount; i++) {
                auto TitleTxt = DynPals::UI::Text(OuterContext).Text(L"").Font(PalFont, L"Bold", 16).TextColor({0.063f, 0.725f, 0.506f, 1.0f}); 
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

            for (int i = 0; i < btnCount; i++) {
                DynPals::WidgetBuilder Item(DynPals::UI::Assets::Blueprints::CommonButton, OuterContext);
                Item.DesiredSizeOverride(600.0f - 40.0f, 45.0f);
                Item.UnlockButtonSize(600.0f - 60.0f); 
                
                RC::Unreal::UObject* RootObj = Item.Build();
                if (!RootObj) continue;
                RC::Unreal::UObject* TextObj = nullptr;
                Utils::GetPropertyValue(RootObj, STR("Text_Main"), TextObj, true);

                auto Ctrl = std::make_unique<class DynPals::UI::Button>(RootObj);
                class DynPals::UI::Button* RawCtrl = Ctrl.get();
                AllButtonCtrls.push_back(std::move(Ctrl));
                
                ButtonPool.push_back(PooledButton{RootObj, TextObj, RawCtrl});
            }
        }

        RC::Unreal::UObject* Build(RC::Unreal::UObject* Outer, RC::Unreal::UObject* PC) {
            if (OuterContext != Outer) {
                ClosePopup();
                PopupOverlay = nullptr;
                ScrollBoxList = nullptr;
                bNeedsListRebuild = true;
                OuterContext = Outer;
            }
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
                if (m_bIsBuildingListAsync) {
                    ProcessSingleBuildStep();
                }

                for (auto& btnCtrl : AllButtonCtrls) {
                    if (btnCtrl) btnCtrl->Tick();
                }
            }
        }

        void ClosePopup() {
            if (PopupOverlay) {
                struct { uint8_t InVisibility; } Params{ 1 }; // 1 = Collapsed
                Utils::CallFunction(PopupOverlay, STR("SetVisibility"), &Params);
                bIsPopupOpen = false;
                
                if (m_bIsBuildingListAsync) {
                    m_bIsBuildingListAsync = false;
                    CleanupUnusedPoolWidgets(); // Safely stash unused widgets to trash bin if closed early
                }
            }
        }

        ~Dropdown() { 
            ClosePopup();
            if (PopupOverlay) {
                Utils::CallFunction(PopupOverlay, STR("RemoveFromParent"));
            }
        }

        const std::vector<PooledHeader>& GetHeaderPool() const { return HeaderPool; }
        const std::vector<PooledButton>& GetButtonPool() const { return ButtonPool; }

    private:
        std::vector<std::wstring> Options;
        int SelectedIndex = 0;
        float MaxWidth = 400.0f;
        bool bNeedsListRebuild = true;
        bool bIsPopupOpen = false; 
        std::function<void(int, std::wstring)> OnSelectionChanged;

        // Async Time-slicing state
        bool m_bIsBuildingListAsync = false;
        size_t m_BuildIndex = 0;
        int m_HeaderUsedIndex = 0;
        int m_ButtonUsedIndex = 0;

        RC::Unreal::UObject* OuterContext = nullptr;
        RC::Unreal::UObject* PlayerController = nullptr;

        std::unique_ptr<class DynPals::UI::Button> MainButtonCtrl = nullptr;
        RC::Unreal::UObject* PopupOverlay = nullptr;
        RC::Unreal::UObject* ScrollBoxList = nullptr;
        RC::Unreal::UObject* TargetWidget = nullptr;
        RC::Unreal::UObject* DropdownTrashBin = nullptr;

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

            Utils::CallFunction(ScrollBoxList, STR("ClearChildren"));

            m_BuildIndex = 0;
            m_HeaderUsedIndex = 0;
            m_ButtonUsedIndex = 0;
            TargetWidget = nullptr;
            
            m_bIsBuildingListAsync = true;
            // NOTE: bNeedsListRebuild is NOT set to false here anymore!
            // It will be cleared in ProcessSingleBuildStep() ONLY when construction finishes.
        }

        void ProcessSingleBuildStep() {
            if (!ScrollBoxList || m_BuildIndex >= Options.size()) {
                m_bIsBuildingListAsync = false; // Building finished!
                bNeedsListRebuild = false;      // Mark rebuild complete
                CleanupUnusedPoolWidgets();

                if (TargetWidget) {
                    struct {
                        RC::Unreal::UObject* WidgetToFind;
                        bool bAnimateScroll;
                        uint8_t ScrollDestination;
                        float Padding;
                    } ScrollParams{TargetWidget, false, 2, 0.0f};
                    Utils::CallFunction(ScrollBoxList, STR("ScrollWidgetIntoView"), &ScrollParams);
                }
                return;
            }

            RC::Unreal::UObject* PalFont = Utils::LoadAssetSafely(DynPals::UI::Assets::Fonts::PalDefault);
            RC::Unreal::UObject* KTL = DynPals::Utils::GetKTL();
            RC::Unreal::UFunction* ConvFunc = DynPals::Utils::GetKTLFunction(STR("Conv_StringToText"));

            const auto& opt = Options[m_BuildIndex];
            bool isHeader = (opt.rfind(L"[", 0) == 0); 
            
            if (isHeader) {
                std::wstring cleanHeader = opt;
                if (cleanHeader.front() == L'[' && cleanHeader.back() == L']') {
                    cleanHeader = cleanHeader.substr(2, cleanHeader.length() - 4);
                }

                // VALIDITY CHECK: Ensure pooled widget hasn't been GC'd
                bool bNeedsNewHeader = (m_HeaderUsedIndex >= static_cast<int>(HeaderPool.size())) || 
                                       !Utils::IsObjectValid(HeaderPool[m_HeaderUsedIndex].RootWidget);

                if (bNeedsNewHeader) {
                    auto TitleTxt = DynPals::UI::Text(OuterContext).Text(cleanHeader).Font(PalFont, L"Bold", 16).TextColor({0.063f, 0.725f, 0.506f, 1.0f}); 
                    RC::Unreal::UObject* TextObj = TitleTxt.Build();

                    auto HeaderVBox = DynPals::UI::VerticalBox(OuterContext);
                    HeaderVBox.AddToVerticalBox(DynPals::WidgetBuilder(TextObj), [](DynPals::BoxSlotBuilder& Slot) {
                        Slot.Padding(10.0f, 14.0f, 10.0f, 4.0f);
                    });

                    auto DividerLine = DynPals::UI::SizeBox(OuterContext).HeightOverride(1.0f).AddChild(DynPals::UI::Border(OuterContext).BrushColor({0.063f, 0.725f, 0.506f, 0.3f}));
                    HeaderVBox.AddToVerticalBox(DividerLine, [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(10.0f, 0.0f, 10.0f, 6.0f); });

                    RC::Unreal::UObject* RootObj = HeaderVBox.Build();
                    
                    if (m_HeaderUsedIndex < static_cast<int>(HeaderPool.size())) {
                        HeaderPool[m_HeaderUsedIndex] = PooledHeader{RootObj, TextObj};
                    } else {
                        HeaderPool.push_back(PooledHeader{RootObj, TextObj});
                    }
                }

                PooledHeader& ph = HeaderPool[m_HeaderUsedIndex++];
                
                if (ph.TextWidget && KTL && ConvFunc) {
                    struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } P1{ RC::Unreal::FString(cleanHeader.c_str()), RC::Unreal::FText() };
                    KTL->ProcessEvent(ConvFunc, &P1);
                    struct { RC::Unreal::FText InText; } P2{P1.ReturnValue};
                    Utils::CallFunction(ph.TextWidget, STR("SetText"), &P2, true);
                }

                struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{ph.RootWidget, nullptr};
                Utils::CallFunction(ScrollBoxList, STR("AddChild"), &AddParams);

            } else {
                // VALIDITY CHECK: Ensure pooled button hasn't been GC'd
                bool bNeedsNewButton = (m_ButtonUsedIndex >= static_cast<int>(ButtonPool.size())) || 
                                       !Utils::IsObjectValid(ButtonPool[m_ButtonUsedIndex].RootWidget);

                if (bNeedsNewButton) {
                    DynPals::WidgetBuilder Item(DynPals::UI::Assets::Blueprints::CommonButton, OuterContext);
                    Item.DesiredSizeOverride(MaxWidth - 40.0f, 45.0f);
                    Item.UnlockButtonSize(MaxWidth - 60.0f); 
                    
                    RC::Unreal::UObject* RootObj = Item.Build();
                    if (RootObj) {
                        RC::Unreal::UObject* TextObj = nullptr;
                        Utils::GetPropertyValue(RootObj, STR("Text_Main"), TextObj, true);

                        auto Ctrl = std::make_unique<class DynPals::UI::Button>(RootObj);
                        class DynPals::UI::Button* RawCtrl = Ctrl.get();
                        AllButtonCtrls.push_back(std::move(Ctrl));
                        
                        if (m_ButtonUsedIndex < static_cast<int>(ButtonPool.size())) {
                            ButtonPool[m_ButtonUsedIndex] = PooledButton{RootObj, TextObj, RawCtrl};
                        } else {
                            ButtonPool.push_back(PooledButton{RootObj, TextObj, RawCtrl});
                        }
                    }
                }

                if (m_ButtonUsedIndex < static_cast<int>(ButtonPool.size())) {
                    PooledButton& pb = ButtonPool[m_ButtonUsedIndex++];
                    
                    if (pb.TextWidget && KTL && ConvFunc) {
                        struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } P1{ RC::Unreal::FString(opt.c_str()), RC::Unreal::FText() };
                        KTL->ProcessEvent(ConvFunc, &P1);
                        struct { RC::Unreal::FText InText; } P2{P1.ReturnValue};
                        Utils::CallFunction(pb.TextWidget, STR("SetText"), &P2, true);
                    }

                    int itemIndex = static_cast<int>(m_BuildIndex);
                    if (itemIndex == SelectedIndex) {
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

            m_BuildIndex++;
        }

        void CleanupUnusedPoolWidgets() {
            for (size_t i = m_HeaderUsedIndex; i < HeaderPool.size(); ++i) {
                if (Utils::IsObjectValid(HeaderPool[i].RootWidget) && DropdownTrashBin) {
                    Utils::CallFunction(HeaderPool[i].RootWidget, STR("RemoveFromParent"));
                    struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{HeaderPool[i].RootWidget, nullptr};
                    Utils::CallFunction(DropdownTrashBin, STR("AddChild"), &AddParams);
                }
            }
            for (size_t i = m_ButtonUsedIndex; i < ButtonPool.size(); ++i) {
                if (Utils::IsObjectValid(ButtonPool[i].RootWidget) && DropdownTrashBin) {
                    Utils::CallFunction(ButtonPool[i].RootWidget, STR("RemoveFromParent"));
                    struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{ButtonPool[i].RootWidget, nullptr};
                    Utils::CallFunction(DropdownTrashBin, STR("AddChild"), &AddParams);
                }
            }
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
        }
    };
}