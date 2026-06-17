#define NOMINMAX 
#include <Windows.h>

#include "UIManager.hpp"
#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "PalProcessor.hpp"
#include "Utils.hpp"

#include <Unreal/UObject.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> 
#include <DynamicOutput/DynamicOutput.hpp>
#include <chrono>
#include <cmath>
#include <map>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    // Global memory map to translate ComboBox display strings to Config Indices
    static std::map<std::wstring, int> GDropdownMapping;

    // UE5 Struct Layouts
    struct FAnchors { double MinimumX; double MinimumY; double MaximumX; double MaximumY; }; 
    struct FMargin { float Left; float Top; float Right; float Bottom; };
    struct FLinearColor { float R; float G; float B; float A; };
    
    struct FSlateColor_UE5 {
        FLinearColor SpecifiedColor;
        uint8_t ColorUseRule; // ESlateColorStylingMode (0 = UseColor_Specified)
        uint8_t Pad[3];       // Memory alignment padding
    };

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
        if (!PlayerController) return;

        UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        if (!WBL) return;

        if (bLock) {
            // Force cursor visibility via APlayerController
            auto* prop = Utils::GetProperty(PlayerController, STR("bShowMouseCursor"));
            if (prop) {
                bool* pVal = prop->ContainerPtrToValuePtr<bool>(PlayerController);
                if (pVal) *pVal = true;
            }

            // APlayerController::SetIgnoreLookInput(true)
            struct { bool bNewLookInput; } LookParams{ true };
            Utils::CallFunction(PlayerController, STR("SetIgnoreLookInput"), &LookParams);

            // UWidgetBlueprintLibrary::SetInputMode_UIOnlyEx
            // Params: PlayerController, InWidgetToFocus, InMouseLockMode (EMouseLockMode::DoNotLock = 0), bFlushInput
            struct {
                UObject* PlayerController;
                UObject* InWidgetToFocus;
                uint8_t InMouseLockMode; 
                bool bFlushInput;
            } SetInputParams{ PlayerController, MyWidget, 0, false };

            Utils::CallFunction(WBL, STR("SetInputMode_UIOnlyEx"), &SetInputParams);
        } else {
            // Restore standard cursor visibility
            auto* prop = Utils::GetProperty(PlayerController, STR("bShowMouseCursor"));
            if (prop) {
                bool* pVal = prop->ContainerPtrToValuePtr<bool>(PlayerController);
                if (pVal) *pVal = false;
            }

            // APlayerController::SetIgnoreLookInput(false)
            struct { bool bNewLookInput; } LookParams{ false };
            Utils::CallFunction(PlayerController, STR("SetIgnoreLookInput"), &LookParams);

            // UWidgetBlueprintLibrary::SetInputMode_GameOnly
            // Params: PlayerController, bFlushInput
            struct {
                UObject* PlayerController;
                bool bFlushInput;
            } SetInputParams{ PlayerController, false };

            Utils::CallFunction(WBL, STR("SetInputMode_GameOnly"), &SetInputParams);
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
        if (!PlayerController) return;

        UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        UClass* WidgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (!WBL || !WidgetClass) return;

        struct { UObject* WorldContext; UClass* WidgetType; UObject* OwningPlayer; UObject* ReturnValue; } CreateParams{
            PlayerController, WidgetClass, PlayerController, nullptr
        };
        
        UFunction* CreateFunc = WBL->GetFunctionByNameInChain(STR("Create"));
        if (!CreateFunc) return;
        WBL->ProcessEvent(CreateFunc, &CreateParams);
        
        MyWidget = CreateParams.ReturnValue;
        if (!MyWidget) return;

        // Force root widget to be focusable to prevent Slate input routing crashes in UI-Only mode
        auto* FocusProp = Utils::GetProperty(MyWidget, STR("bIsFocusable"));
        if (FocusProp) {
            bool* pFocus = FocusProp->ContainerPtrToValuePtr<bool>(MyWidget);
            if (pFocus) *pFocus = true;
        }


        UObject* WidgetTree = nullptr;
        if (!Utils::GetPropertyValue(MyWidget, STR("WidgetTree"), WidgetTree) || !WidgetTree) return;

        auto ConstructElement = [&](UClass* ElementClass, const wchar_t* Name) -> UObject* {
            if (!ElementClass) return nullptr;
            FStaticConstructObjectParameters Params{ElementClass, MyWidget};
            Params.Name = FName(Name, FNAME_Add);
            return UObjectGlobals::StaticConstructObject(Params);
        };

        UClass* CanvasPanelClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
        UClass* BorderClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Border"));
        UClass* ScrollBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.ScrollBox"));
        UClass* VerticalBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.VerticalBox"));
        UClass* ComboBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.ComboBoxString"));
        UClass* SliderClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Slider"));
        UClass* TextBlockClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.TextBlock"));
        UClass* SpacerClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Spacer"));

        UObject* Canvas = ConstructElement(CanvasPanelClass, STR("Canvas"));
        UObject* BackgroundBorder = ConstructElement(BorderClass, STR("BackgroundBorder"));
        UObject* ScrollBox = ConstructElement(ScrollBoxClass, STR("ScrollBox"));
        UObject* VBox = ConstructElement(VerticalBoxClass, STR("VBox"));

        auto* RootProp = Utils::GetProperty(WidgetTree, STR("RootWidget"));
        if (RootProp) {
            UObject** pRoot = RootProp->ContainerPtrToValuePtr<UObject*>(WidgetTree);
            if (pRoot) *pRoot = Canvas;
        }

        struct { UObject* Content; UObject* ReturnValue; } AddBorderParams{BackgroundBorder, nullptr};
        Utils::CallFunction(Canvas, STR("AddChild"), &AddBorderParams);
        UObject* CanvasSlot = AddBorderParams.ReturnValue;

        FAnchors Anchors{0.02, 0.15, 0.22, 0.85}; 
        struct { FAnchors InAnchors; } AnchorsParams{Anchors};
        Utils::CallFunction(CanvasSlot, STR("SetAnchors"), &AnchorsParams);

        FMargin Offsets{0.0f, 0.0f, 0.0f, 0.0f};
        struct { FMargin InOffsets; } OffsetsParams{Offsets};
        Utils::CallFunction(CanvasSlot, STR("SetOffsets"), &OffsetsParams);

        struct { FLinearColor InBrushColor; } BrushColorParams{{0.012f, 0.078f, 0.153f, 0.96f}}; 
        Utils::CallFunction(BackgroundBorder, STR("SetBrushColor"), &BrushColorParams);

        struct { FMargin InPadding; } PaddingParams{{15.0f, 15.0f, 15.0f, 15.0f}};
        Utils::CallFunction(BackgroundBorder, STR("SetPadding"), &PaddingParams);

        struct { UObject* Content; UObject* ReturnValue; } AddScrollParams{ScrollBox, nullptr};
        Utils::CallFunction(BackgroundBorder, STR("AddChild"), &AddScrollParams);

        struct { UObject* Content; UObject* ReturnValue; } AddVBoxParams{VBox, nullptr};
        Utils::CallFunction(ScrollBox, STR("AddChild"), &AddVBoxParams);

        UObject* KTL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
        UFunction* ConvStringFunc = KTL ? KTL->GetFunctionByNameInChain(STR("Conv_StringToText")) : nullptr;

        auto ConvStringToText = [&](const std::wstring& str) -> FText {
            struct { FString InString; FText ReturnValue; } Params{ FString(str.c_str()), FText() };
            KTL->ProcessEvent(ConvStringFunc, &Params);
            return Params.ReturnValue;
        };

        auto AddSpacer = [&](double SizeY) {
            UObject* Spacer = ConstructElement(SpacerClass, STR("Spacer"));
            if (Spacer) {
                struct FVector2D { double X, Y; };
                struct { FVector2D InSize; } SizeParams{{1.0, SizeY}};
                Utils::CallFunction(Spacer, STR("SetSize"), &SizeParams);
                struct { UObject* Content; UObject* ReturnValue; } AddSpacerP{Spacer, nullptr};
                Utils::CallFunction(VBox, STR("AddChild"), &AddSpacerP);
            }
        };

        const FLinearColor PalBakerCyan    = {0.024f, 0.714f, 0.831f, 1.0f}; 
        const FLinearColor PalBakerEmerald = {0.063f, 0.725f, 0.506f, 1.0f}; 
        const FLinearColor OffWhite        = {0.930f, 0.930f, 0.930f, 1.0f}; 

        auto SetTextColor = [&](UObject* Widget, FLinearColor Color) {
            if (!Widget) return;
            struct { FSlateColor_UE5 InColorAndOpacity; } Params{ {Color, 0, {0,0,0}} };
            
            std::wstring className = Widget->GetClassPrivate()->GetName();
            if (className.find(L"TextBlock") != std::wstring::npos) {
                Utils::CallFunction(Widget, STR("SetColorAndOpacity"), &Params);
            } else if (className.find(L"ComboBox") != std::wstring::npos) {
                Utils::CallFunction(Widget, STR("SetForegroundColor"), &Params);
            }
        };

        auto StyleComboBox = [&](UObject* Combo) {
            if (!Combo) return;
            
            auto* ForegroundProp = Utils::GetProperty(Combo, STR("ForegroundColor"));
            if (ForegroundProp) {
                FSlateColor_UE5* pColor = ForegroundProp->ContainerPtrToValuePtr<FSlateColor_UE5>(Combo);
                if (pColor) {
                    pColor->SpecifiedColor = OffWhite;
                    pColor->ColorUseRule = 0; 
                }
            }

            auto* ItemStyleProp = Utils::GetProperty(Combo, STR("ItemStyle"));
            if (ItemStyleProp) {
                void* ItemStylePtr = ItemStyleProp->ContainerPtrToValuePtr<void>(Combo);
                if (ItemStylePtr) {
                    FStructProperty* StructProp = static_cast<FStructProperty*>(ItemStyleProp);
                    UStruct* TableRowStyleStruct = StructProp->GetStruct();
                    if (TableRowStyleStruct) {
                        
                        FProperty* TextColorProp = TableRowStyleStruct->GetPropertyByNameInChain(STR("TextColor"));
                        if (TextColorProp) {
                            FSlateColor_UE5* pColor = TextColorProp->ContainerPtrToValuePtr<FSlateColor_UE5>(ItemStylePtr);
                            if (pColor) {
                                pColor->SpecifiedColor = OffWhite;
                                pColor->ColorUseRule = 0;
                            }
                        }
                        
                        FProperty* SelectedTextColorProp = TableRowStyleStruct->GetPropertyByNameInChain(STR("SelectedTextColor"));
                        if (SelectedTextColorProp) {
                            FSlateColor_UE5* pColor = SelectedTextColorProp->ContainerPtrToValuePtr<FSlateColor_UE5>(ItemStylePtr);
                            if (pColor) {
                                pColor->SpecifiedColor = PalBakerCyan;
                                pColor->ColorUseRule = 0;
                            }
                        }
                    }
                }
            }
        };

        UObject* TitleText = ConstructElement(TextBlockClass, STR("TitleText"));
        if (TitleText) {
            FText titleTextVal = ConvStringToText(L"DynPals Settings:\n" + TargetCharID);
            struct { FText InText; } SetTitleParams{titleTextVal};
            Utils::CallFunction(TitleText, STR("SetText"), &SetTitleParams);
            SetTextColor(TitleText, PalBakerCyan); 
            
            struct { UObject* Content; UObject* ReturnValue; } AddTitleParams{TitleText, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddTitleParams);
        }

        AddSpacer(15.0);

        UObject* SkinLabel = ConstructElement(TextBlockClass, STR("SkinLabel"));
        if (SkinLabel) {
            FText labelVal = ConvStringToText(L"Current Swap");
            struct { FText InText; } SetLabelParams{labelVal};
            Utils::CallFunction(SkinLabel, STR("SetText"), &SetLabelParams);
            SetTextColor(SkinLabel, PalBakerEmerald); 

            struct { UObject* Content; UObject* ReturnValue; } AddLabelParams{SkinLabel, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddLabelParams);
        }

        AddSpacer(5.0);

        ComboBoxWidget = ConstructElement(ComboBoxClass, STR("SkinCombo"));
        if (ComboBoxWidget) {
            GDropdownMapping.clear();
            LastSelectedOption = L"";

            StyleComboBox(ComboBoxWidget);

            std::map<std::wstring, std::vector<int>> groupedPacks;
            auto validConfigs = ConfigManager::Get().GetConfigsForCharID(TargetCharID);
            for (int idx : validConfigs) {
                groupedPacks[ConfigManager::Get().GetConfigs()[idx].PackName].push_back(idx);
            }

            for (auto& [packName, indices] : groupedPacks) {
                std::wstring headerStr = L"[ " + packName + L" ]";
                
                struct { FString Option; } AddHeaderP{ FString(headerStr.c_str()) };
                Utils::CallFunction(ComboBoxWidget, STR("AddOption"), &AddHeaderP);
                GDropdownMapping[headerStr] = -1; 

                for (int idx : indices) {
                    auto& cfg = ConfigManager::Get().GetConfigs()[idx];
                    
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

                    std::wstring optName = L"   " + labelName; 

                    while (GDropdownMapping.find(optName) != GDropdownMapping.end()) {
                        optName += L" "; 
                    }

                    struct { FString Option; } AddOptP{ FString(optName.c_str()) };
                    Utils::CallFunction(ComboBoxWidget, STR("AddOption"), &AddOptP);
                    GDropdownMapping[optName] = idx;

                    PalPersistData* persist = SaveManager::Get().GetPersistData(TargetInstanceID);
                    if (persist && persist->SwapIndex == idx) {
                        LastSelectedOption = optName;
                    }
                }
            }

            if (!LastSelectedOption.empty()) {
                struct { FString Option; } SetSelParams{ FString(LastSelectedOption.c_str()) };
                Utils::CallFunction(ComboBoxWidget, STR("SetSelectedOption"), &SetSelParams);
            }

            Utils::CallFunction(ComboBoxWidget, STR("RefreshOptions"));

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
                    SetTextColor(MorphLabel, PalBakerEmerald); 

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
                        SetTextColor(Label, OffWhite); 

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
    }

    void UIManager::TickUI() {
        if (!bIsMenuOpen || !MyWidget) return;

        // Throttled Safety Check: Validate TargetPal only once per second to save memory/CPU
        static auto LastCheckTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - LastCheckTime > std::chrono::seconds(1)) {
            LastCheckTime = now;
            
            std::vector<UObject*> AllPals;
            UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
            
            bool bIsTargetValid = false;
            for (UObject* Pal : AllPals) {
                if (Pal == TargetPal) {
                    bIsTargetValid = true;
                    break;
                }
            }

            if (!bIsTargetValid) {
                ToggleMenu(); // Safely closes the menu if the Pal dies/despawns
                return;
            }
        }

        if (ComboBoxWidget) {
            FString SelectedOpt;
            Utils::CallFunction(ComboBoxWidget, STR("GetSelectedOption"), &SelectedOpt);
            std::wstring selectedStr = Utils::FStringToWString(SelectedOpt);

            if (!selectedStr.empty() && selectedStr != LastSelectedOption) {
                
                auto it = GDropdownMapping.find(selectedStr);
                if (it != GDropdownMapping.end()) {
                    
                    if (it->second == -1) {
                        if (!LastSelectedOption.empty()) {
                            struct { FString Option; } SetSelParams{ FString(LastSelectedOption.c_str()) };
                            Utils::CallFunction(ComboBoxWidget, STR("SetSelectedOption"), &SetSelParams);
                        }
                    } else {
                        LastSelectedOption = selectedStr;
                        PalProcessor::Get().ForceSwap(TargetPal, it->second);
                        
                        DestroyWidget();
                        BuildWidget();
                        LockInput(true); // CRITICAL FIX: Re-tether Input Mode to the newly created Widget!
                        return; // Halt Tick immediately to prevent accessing dead memory
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