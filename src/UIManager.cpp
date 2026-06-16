#define NOMINMAX 
#include <Windows.h>

#include "UIManager.hpp"
#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "PalProcessor.hpp"
#include "Utils.hpp"

#include <Unreal/UObject.hpp>
#include <Unreal/FString.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <cmath>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    struct FAnchors { double MinimumX; double MinimumY; double MaximumX; double MaximumY; }; 
    struct FMargin { float Left; float Top; float Right; float Bottom; };
    struct FLinearColor { float R; float G; float B; float A; };

    UObject* UIManager::GetLocalPlayerController() {
        std::vector<UObject*> PCs;
        UObjectGlobals::FindAllOf(STR("PalPlayerController"), PCs);
        
        for (UObject* PC : PCs) {
            if (!PC || PC->GetName().rfind(L"Default__", 0) == 0) continue;
            
            struct { bool ReturnValue; } IsLocalParams{false};
            Utils::CallFunction(PC, STR("IsLocalPlayerController"), &IsLocalParams);
            
            if (IsLocalParams.ReturnValue) return PC;
        }
        return nullptr;
    }

    void UIManager::ToggleMenu() {
        bIsMenuOpen = !bIsMenuOpen;
        Output::send<LogLevel::Normal>(STR("[DynPals] UI Toggle called. Open: {}\n"), bIsMenuOpen ? STR("TRUE") : STR("FALSE"));

        if (bIsMenuOpen) {
            UpdateTarget();
            if (TargetPal) {
                BuildWidget();
                LockInput(true);
            } else {
                bIsMenuOpen = false;
                Output::send<LogLevel::Normal>(STR("[DynPals] UI scan aborted: No targetable Pal found within 30 meters.\n"));
            }
        } else {
            DestroyWidget();
            LockInput(false);
        }
    }

    void UIManager::LockInput(bool bLock) {
        UObject* PlayerController = GetLocalPlayerController();
        if (PlayerController) {
            auto* prop = Utils::GetProperty(PlayerController, STR("bShowMouseCursor"));
            if (prop) {
                bool* pVal = prop->ContainerPtrToValuePtr<bool>(PlayerController);
                if (pVal) *pVal = bLock;
            }
            struct { bool bNewLookInput; } Params{ bLock };
            Utils::CallFunction(PlayerController, STR("SetIgnoreLookInput"), &Params);
        }
    }

    void UIManager::UpdateTarget() {
        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";

        UObject* PlayerController = GetLocalPlayerController();
        if (!PlayerController) return;

        UObject* PlayerPawn = nullptr;
        Utils::CallFunction(PlayerController, STR("K2_GetPawn"), &PlayerPawn);
        if (!PlayerPawn) return;

        struct { FVector_UE5 RetVal; } LocParams;
        Utils::CallFunction(PlayerPawn, STR("K2_GetActorLocation"), &LocParams);
        FVector_UE5 PlayerLoc = LocParams.RetVal;

        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

        double closestDist = 999999999.0;
        UObject* closestPal = nullptr;

        for (UObject* Pal : AllPals) {
            if (Pal == PlayerPawn || !Pal) continue;

            struct { FVector_UE5 RetVal; } PalLocParams;
            Utils::CallFunction(Pal, STR("K2_GetActorLocation"), &PalLocParams);
            FVector_UE5 PalLoc = PalLocParams.RetVal;

            double dx = PalLoc.X - PlayerLoc.X;
            double dy = PalLoc.Y - PlayerLoc.Y;
            double dz = PalLoc.Z - PlayerLoc.Z;
            double distSq = (dx*dx) + (dy*dy) + (dz*dz);

            if (distSq < closestDist) {
                closestDist = distSq;
                closestPal = Pal;
            }
        }

        if (closestPal && closestDist < (3000.0 * 3000.0)) {
            TargetPal = closestPal;

            UObject* ParamComp = nullptr;
            Utils::GetPropertyValue(TargetPal, STR("CharacterParameterComponent"), ParamComp);
            if (!ParamComp) return;

            UObject* IndivParam = nullptr;
            Utils::GetPropertyValue(ParamComp, STR("IndividualParameter"), IndivParam);
            if (!IndivParam) return;

            struct FPalInstanceID { DynPalsGuid PlayerUId; DynPalsGuid InstanceId; } IDStruct;
            if (Utils::GetPropertyValue(IndivParam, STR("IndividualId"), IDStruct)) {
                TargetInstanceID = Utils::GuidToWString(IDStruct.InstanceId);
            }

            UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            struct { UObject* Char; FName RetVal; } CharIDParams{TargetPal, FName()};
            if (PalUtil) Utils::CallFunction(PalUtil, STR("GetCharacterIDFromCharacter"), &CharIDParams);
            TargetCharID = PalProcessor::Get().StripCharacterPrefix(CharIDParams.RetVal.ToString());
            
            Output::send<LogLevel::Normal>(STR("[DynPals] Target Scanner: Locked onto closest Pal: {} (Distance: {} units)\n"), TargetCharID, std::sqrt(closestDist));
        }
    }

    void UIManager::BuildWidget() {
        UObject* PlayerController = GetLocalPlayerController();
        if (!PlayerController) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: Active local PlayerController not found!\n"));
            return;
        }

        UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        UClass* WidgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (!WBL || !WidgetClass) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: WidgetBlueprintLibrary or UserWidget Class not found!\n"));
            return;
        }

        struct { UObject* WorldContext; UClass* WidgetType; UObject* OwningPlayer; UObject* ReturnValue; } CreateParams{
            PlayerController, WidgetClass, PlayerController, nullptr
        };
        
        UFunction* CreateFunc = WBL->GetFunctionByNameInChain(STR("Create"));
        if (!CreateFunc) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: Create function not found!\n"));
            return;
        }
        WBL->ProcessEvent(CreateFunc, &CreateParams);
        
        MyWidget = CreateParams.ReturnValue;
        if (!MyWidget) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: Failed to instantiate MyWidget!\n"));
            return;
        }

        UObject* WidgetTree = nullptr;
        if (!Utils::GetPropertyValue(MyWidget, STR("WidgetTree"), WidgetTree) || !WidgetTree) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: WidgetTree property not found on UserWidget!\n"));
            return;
        }

        auto ConstructElement = [&](UClass* ElementClass, const wchar_t* Name) -> UObject* {
            if (!ElementClass) return nullptr;
            FStaticConstructObjectParameters Params{ElementClass, MyWidget};
            Params.Name = FName(Name, FNAME_Add);
            return UObjectGlobals::StaticConstructObject(Params);
        };

        // Get UMG Native classes
        UClass* CanvasPanelClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
        UClass* BorderClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Border"));
        UClass* ScrollBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.ScrollBox"));
        UClass* VerticalBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.VerticalBox"));
        UClass* ComboBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.ComboBoxString"));
        UClass* SliderClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Slider"));
        UClass* TextBlockClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.TextBlock"));
        UClass* SpacerClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Spacer"));

        if (!CanvasPanelClass || !BorderClass || !ScrollBoxClass || !VerticalBoxClass || !ComboBoxClass || !SliderClass || !TextBlockClass) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: One or more native UMG classes could not be loaded!\n"));
            return;
        }

        UObject* Canvas = ConstructElement(CanvasPanelClass, STR("Canvas"));
        UObject* BackgroundBorder = ConstructElement(BorderClass, STR("BackgroundBorder"));
        UObject* ScrollBox = ConstructElement(ScrollBoxClass, STR("ScrollBox"));
        UObject* VBox = ConstructElement(VerticalBoxClass, STR("VBox"));

        if (!Canvas || !BackgroundBorder || !ScrollBox || !VBox) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: Failed to construct core layout elements!\n"));
            return;
        }

        auto* RootProp = Utils::GetProperty(WidgetTree, STR("RootWidget"));
        if (RootProp) {
            UObject** pRoot = RootProp->ContainerPtrToValuePtr<UObject*>(WidgetTree);
            if (pRoot) *pRoot = Canvas;
        } else {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: RootWidget property not found on WidgetTree!\n"));
            return;
        }

        // Parent Border inside the Canvas
        struct { UObject* Content; UObject* ReturnValue; } AddBorderParams{BackgroundBorder, nullptr};
        Utils::CallFunction(Canvas, STR("AddChild"), &AddBorderParams);
        UObject* CanvasSlot = AddBorderParams.ReturnValue;
        if (!CanvasSlot) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: Failed to parent Border to Canvas!\n"));
            return;
        }

        // Floating Middle-Left Panel (2% margin left, 15% top to 85% bottom)
        FAnchors Anchors{0.02, 0.15, 0.22, 0.85}; 
        struct { FAnchors InAnchors; } AnchorsParams{Anchors};
        Utils::CallFunction(CanvasSlot, STR("SetAnchors"), &AnchorsParams);

        // Zero out offsets so it stretches exactly to the anchors
        FMargin Offsets{0.0f, 0.0f, 0.0f, 0.0f};
        struct { FMargin InOffsets; } OffsetsParams{Offsets};
        Utils::CallFunction(CanvasSlot, STR("SetOffsets"), &OffsetsParams);

        // Styling the Border Box
        struct { FLinearColor InBrushColor; } BrushColorParams{{0.015f, 0.015f, 0.02f, 0.85f}}; // Dark bluish-grey, transparent
        Utils::CallFunction(BackgroundBorder, STR("SetBrushColor"), &BrushColorParams);

        struct { FMargin InPadding; } PaddingParams{{15.0f, 15.0f, 15.0f, 15.0f}};
        Utils::CallFunction(BackgroundBorder, STR("SetPadding"), &PaddingParams);

        // Add ScrollBox > VBox chain
        struct { UObject* Content; UObject* ReturnValue; } AddScrollParams{ScrollBox, nullptr};
        Utils::CallFunction(BackgroundBorder, STR("AddChild"), &AddScrollParams);

        struct { UObject* Content; UObject* ReturnValue; } AddVBoxParams{VBox, nullptr};
        Utils::CallFunction(ScrollBox, STR("AddChild"), &AddVBoxParams);

        // Text Utilities
        UObject* KTL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
        if (!KTL) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: KismetTextLibrary not found!\n"));
            return;
        }
        UFunction* ConvStringFunc = KTL->GetFunctionByNameInChain(STR("Conv_StringToText"));
        if (!ConvStringFunc) {
            Output::send<LogLevel::Error>(STR("[DynPals] BuildWidget: Conv_StringToText function not found!\n"));
            return;
        }

        auto ConvStringToText = [&](const std::wstring& str) -> FText {
            struct { FString InString; FText ReturnValue; } Params{ FString(str.c_str()), FText() };
            KTL->ProcessEvent(ConvStringFunc, &Params);
            return Params.ReturnValue;
        };

        // Layout Utilities
        auto AddSpacer = [&](double SizeY) {
            if (SpacerClass) {
                UObject* Spacer = ConstructElement(SpacerClass, STR("Spacer"));
                if (Spacer) {
                    struct FVector2D { double X, Y; };
                    struct { FVector2D InSize; } SizeParams{{1.0, SizeY}};
                    Utils::CallFunction(Spacer, STR("SetSize"), &SizeParams);
                    struct { UObject* Content; UObject* ReturnValue; } AddSpacerParams{Spacer, nullptr};
                    Utils::CallFunction(VBox, STR("AddChild"), &AddSpacerParams);
                }
            }
        };

        UObject* TitleText = ConstructElement(TextBlockClass, STR("TitleText"));
        if (TitleText) {
            FText titleTextVal = ConvStringToText(L"DynPals Settings:\n" + TargetCharID);
            struct { FText InText; } SetTitleParams{titleTextVal};
            Utils::CallFunction(TitleText, STR("SetText"), &SetTitleParams);
            
            struct { UObject* Content; UObject* ReturnValue; } AddTitleParams{TitleText, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddTitleParams);
        }

        AddSpacer(15.0);

        UObject* SkinLabel = ConstructElement(TextBlockClass, STR("SkinLabel"));
        if (SkinLabel) {
            FText labelVal = ConvStringToText(L"Current Swap:");
            struct { FText InText; } SetLabelParams{labelVal};
            Utils::CallFunction(SkinLabel, STR("SetText"), &SetLabelParams);

            struct { UObject* Content; UObject* ReturnValue; } AddLabelParams{SkinLabel, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddLabelParams);
        }

        AddSpacer(5.0);

        ComboBoxWidget = ConstructElement(ComboBoxClass, STR("SkinCombo"));
        if (ComboBoxWidget) {
            LastSelectedOption = L"";

            auto configs = ConfigManager::Get().GetConfigsForCharID(TargetCharID);
            for (int idx : configs) {
                auto& cfg = ConfigManager::Get().GetConfigs()[idx];
                
                // fallback name processing to use SkelMesh filename if SkinName is empty in JSON
                std::wstring labelName = cfg.SkinName;
                if (labelName.empty()) {
                    size_t lastSlash = cfg.SkelMeshPath.find_last_of(L'/');
                    if (lastSlash != std::wstring::npos) {
                        std::wstring filename = cfg.SkelMeshPath.substr(lastSlash + 1);
                        if (filename.rfind(L"SK_", 0) == 0 || filename.rfind(L"sk_", 0) == 0) {
                            filename = filename.substr(3);
                        }
                        labelName = filename;
                    } else {
                        labelName = L"Default";
                    }
                }

                std::wstring optName = cfg.PackName + L" / " + labelName;
                
                struct { FString Option; } AddOptParams{ FString(optName.c_str()) };
                Utils::CallFunction(ComboBoxWidget, STR("AddOption"), &AddOptParams);

                PalPersistData* persist = SaveManager::Get().GetPersistData(TargetInstanceID);
                if (persist && persist->SwapIndex == idx) {
                    LastSelectedOption = optName;
                    struct { FString Option; } SetSelParams{ FString(optName.c_str()) };
                    Utils::CallFunction(ComboBoxWidget, STR("SetSelectedOption"), &SetSelParams);
                }
            }

            struct { UObject* Content; UObject* ReturnValue; } AddComboParams{ComboBoxWidget, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddComboParams);
        }

        AddSpacer(20.0);

        ActiveSliders.clear();
        PalPersistData* persist = SaveManager::Get().GetPersistData(TargetInstanceID);
        if (persist && persist->SwapIndex != -1) {
            auto& activeCfg = ConfigManager::Get().GetConfigs()[persist->SwapIndex];
            
            if (!activeCfg.MorphTargetList.empty()) {
                UObject* MorphLabel = ConstructElement(TextBlockClass, STR("MorphTitleLabel"));
                if (MorphLabel) {
                    FText labelVal = ConvStringToText(L"Morph Targets");
                    struct { FText InText; } SetLabelParams{labelVal};
                    Utils::CallFunction(MorphLabel, STR("SetText"), &SetLabelParams);

                    struct { UObject* Content; UObject* ReturnValue; } AddLabelParams{MorphLabel, nullptr};
                    Utils::CallFunction(VBox, STR("AddChild"), &AddLabelParams);
                }

                AddSpacer(10.0);
            }

            for (auto& morph : activeCfg.MorphTargetList) {
                if (morph.type != L"Restrict" && morph.minVal < morph.maxVal) {
                    UObject* Label = ConstructElement(TextBlockClass, (morph.target + L"_Label").c_str());
                    if (Label) {
                        FText labelVal = ConvStringToText(morph.target);
                        struct { FText InText; } SetLabelParams{labelVal};
                        Utils::CallFunction(Label, STR("SetText"), &SetLabelParams);

                        struct { UObject* Content; UObject* ReturnValue; } AddLabelParams{Label, nullptr};
                        Utils::CallFunction(VBox, STR("AddChild"), &AddLabelParams);
                    }

                    UObject* Slider = ConstructElement(SliderClass, (morph.target + L"_Slider").c_str());
                    if (Slider) {
                        struct { float InValue; } SetMinParams{(float)morph.minVal};
                        Utils::CallFunction(Slider, STR("SetMinValue"), &SetMinParams);
                        struct { float InValue; } SetMaxParams{(float)morph.maxVal};
                        Utils::CallFunction(Slider, STR("SetMaxValue"), &SetMaxParams);

                        float currentVal = (float)persist->MorphSet[morph.target];
                        struct { float InValue; } SetValParams{currentVal};
                        Utils::CallFunction(Slider, STR("SetValue"), &SetValParams);

                        struct { UObject* Content; UObject* ReturnValue; } AddSliderParams{Slider, nullptr};
                        Utils::CallFunction(VBox, STR("AddChild"), &AddSliderParams);

                        ActiveSliders.push_back({morph.target, Slider, currentVal});

                        AddSpacer(10.0);
                    }
                }
            }
        }

        struct { uint8_t InVisibility; } VisParams{0}; 
        Utils::CallFunction(MyWidget, STR("SetVisibility"), &VisParams);
        Utils::CallFunction(Canvas, STR("SetVisibility"), &VisParams);
        Utils::CallFunction(BackgroundBorder, STR("SetVisibility"), &VisParams);
        Utils::CallFunction(ScrollBox, STR("SetVisibility"), &VisParams);
        Utils::CallFunction(VBox, STR("SetVisibility"), &VisParams);

        Utils::CallFunction(MyWidget, STR("Initialize"));

        struct { int32_t ZOrder; } ViewportParams{9999};
        Utils::CallFunction(MyWidget, STR("AddToViewport"), &ViewportParams);
        
        struct { bool RetVal; } InViewportParams{false};
        Utils::CallFunction(MyWidget, STR("IsInViewport"), &InViewportParams);

        struct { bool RetVal; } IsVisibleParams{false};
        Utils::CallFunction(MyWidget, STR("IsVisible"), &IsVisibleParams);

        struct { uint8_t RetVal; } GetVisParams{0};
        Utils::CallFunction(MyWidget, STR("GetVisibility"), &GetVisParams);

        Output::send<LogLevel::Normal>(STR("[DynPals] BuildWidget: UMG UI Diagnostic -> IsInViewport: {}, IsVisible: {}, VisibilityEnum: {}\n"), 
            InViewportParams.RetVal ? STR("YES") : STR("NO"), 
            IsVisibleParams.RetVal ? STR("YES") : STR("NO"), 
            (int)GetVisParams.RetVal);
    }

    void UIManager::TickUI() {
        if (!bIsMenuOpen || !MyWidget) return;

        if (!TargetPal) {
            ToggleMenu();
            return;
        }

        if (ComboBoxWidget) {
            FString SelectedOpt;
            Utils::CallFunction(ComboBoxWidget, STR("GetSelectedOption"), &SelectedOpt);
            std::wstring selectedStr = Utils::FStringToWString(SelectedOpt);

            if (selectedStr != LastSelectedOption) {
                LastSelectedOption = selectedStr;
                
                auto configs = ConfigManager::Get().GetConfigsForCharID(TargetCharID);
                for (int idx : configs) {
                    auto& cfg = ConfigManager::Get().GetConfigs()[idx];
                    
                    // Re-generate the identical display name for match check
                    std::wstring labelName = cfg.SkinName;
                    if (labelName.empty()) {
                        size_t lastSlash = cfg.SkelMeshPath.find_last_of(L'/');
                        if (lastSlash != std::wstring::npos) {
                            std::wstring filename = cfg.SkelMeshPath.substr(lastSlash + 1);
                            if (filename.rfind(L"SK_", 0) == 0 || filename.rfind(L"sk_", 0) == 0) {
                                filename = filename.substr(3);
                            }
                            labelName = filename;
                        } else {
                            labelName = L"Default";
                        }
                    }

                    std::wstring optName = cfg.PackName + L" / " + labelName;

                    if (optName == selectedStr) {
                        PalProcessor::Get().ForceSwap(TargetPal, idx);
                        
                        DestroyWidget();
                        BuildWidget();
                        break;
                    }
                }
            }
        }

        PalPersistData* persist = SaveManager::Get().GetPersistData(TargetInstanceID);
        if (persist) {
            bool bChanged = false;
            for (auto& as : ActiveSliders) {
                float currentVal = 0.0f;
                Utils::CallFunction(as.SliderWidget, STR("GetValue"), &currentVal);

                if (std::abs(currentVal - as.LastValue) > 0.001f) {
                    as.LastValue = currentVal;
                    persist->MorphSet[as.MorphTargetName] = currentVal;
                    bChanged = true;

                    UObject* MeshComp = nullptr;
                    Utils::CallFunction(TargetPal, STR("GetMainMesh"), &MeshComp);
                    if (MeshComp) {
                        struct { FName MorphTargetName; float Value; bool bRemoveZeroWeight; } MorphParams{
                            FName(as.MorphTargetName.c_str(), FNAME_Add), currentVal, false
                        };
                        Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
                    }
                }
            }

            if (bChanged) {
                SaveManager::Get().SetPersistData(TargetInstanceID, *persist); 
            }
        }
    }

    void UIManager::DestroyWidget() {
        if (MyWidget) {
            Utils::CallFunction(MyWidget, STR("RemoveFromParent"));
            MyWidget = nullptr;
        }
        ComboBoxWidget = nullptr;
        ActiveSliders.clear();
    }
}