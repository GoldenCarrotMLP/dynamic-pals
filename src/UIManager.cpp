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

    static std::map<std::wstring, int> GDropdownMapping;

    struct FAnchors { double MinimumX; double MinimumY; double MaximumX; double MaximumY; }; 
    struct FMargin { float Left; float Top; float Right; float Bottom; };
    struct FLinearColor { float R; float G; float B; float A; };
    
    struct FSlateColor_UE5 {
        FLinearColor SpecifiedColor;
        uint8_t ColorUseRule; 
        uint8_t Pad[3];       
    };

    void UIManager::ToggleMenu() {
        bIsMenuOpen = !bIsMenuOpen;
        if (bIsMenuOpen) {
            DP_LOG(Default, "Alt+N pressed. Scanning for closest Pal...");
            UpdateTarget();
            
            if (TargetPal) {
                DP_LOG(Default, "Found '{}' . Building UI.",TargetPal->GetName());
                BuildWidget();
                LockInput(true);
            } else {
                DP_LOG(Normal, "No valid Pal found within radius! Menu cancelled.");
                bIsMenuOpen = false;
            }
        } else {
            DP_LOG(Default, "Closing Menu.");
            DestroyWidget();
            LockInput(false);
        }
    }

    void UIManager::LockInput(bool bLock) {
    if (!CurrentPlayerController) return;

    if (bLock) {
        // 1. Show the mouse cursor and enable click events 
        Utils::SetPropertyValue<bool>(CurrentPlayerController, STR("bShowMouseCursor"), true);
        Utils::SetPropertyValue<bool>(CurrentPlayerController, STR("bEnableClickEvents"), true);
        Utils::SetPropertyValue<bool>(CurrentPlayerController, STR("bEnableMouseOverEvents"), true);

        // 2. Ignore camera movement
        // Note: In Unreal Engine, this increments an internal "Ignore" counter!
        struct { bool bNewLookInput; } LookParams{ true };
        Utils::CallFunction(CurrentPlayerController, STR("SetIgnoreLookInput"), &LookParams);

        // 3. Ignore player/mount movement 
        struct { bool bNewMoveInput; } MoveParams{ true };
        Utils::CallFunction(CurrentPlayerController, STR("SetIgnoreMoveInput"), &MoveParams);

    } else {
        // 1. Hide the mouse cursor AND disable click events! 
        // (If not disabled, the PlayerController will intercept clicks meant for the game viewport)
        Utils::SetPropertyValue<bool>(CurrentPlayerController, STR("bShowMouseCursor"), false);
        Utils::SetPropertyValue<bool>(CurrentPlayerController, STR("bEnableClickEvents"), false);
        Utils::SetPropertyValue<bool>(CurrentPlayerController, STR("bEnableMouseOverEvents"), false);

        // 2. Restore camera & movement
        // CRITICAL FIX: Use Reset() instead of Set(false). 
        // Because rebuilding the UI calls LockInput(true) multiple times, the ignore counters stack. 
        // Resetting guarantees the counters drop to 0, completely unblocking the player.
        Utils::CallFunction(CurrentPlayerController, STR("ResetIgnoreLookInput"));
        Utils::CallFunction(CurrentPlayerController, STR("ResetIgnoreMoveInput"));
        
        // 3. Force focus back to game viewport
        UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        if (WBL) {
            Utils::CallFunction(WBL, STR("SetFocusToGameViewport"));
            
            // 4. Safely return Input Mode to Game Only
            UFunction* InputModeFunc = WBL->GetFunctionByNameInChain(STR("SetInputMode_GameOnly"));
            if (InputModeFunc) {
                struct { UObject* PC; bool bConsume; } InputModeParams{ CurrentPlayerController, false };
                WBL->ProcessEvent(InputModeFunc, &InputModeParams);
            }
        }
    }
}

    void UIManager::UpdateTarget() {
        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";

        // Always find the active PlayerController to prevent Use-After-Free crashes!
        CurrentPlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        

        if (!CurrentPlayerController) {
            DP_LOG(Warning, "UpdateTarget cancelled: CurrentPlayerController is NULL!");
            return;
        }

        UObject* PlayerPawn = nullptr;
        Utils::CallFunction(CurrentPlayerController, STR("K2_GetPawn"), &PlayerPawn);
        if (!PlayerPawn) {
            DP_LOG(Warning, "UpdateTarget cancelled: PlayerPawn is NULL!");
            return;
        }

        // 1. Get the actual 3D camera location and rotation natively (resolves third-person camera offset)
        struct FRotator_UE5 { double Pitch, Yaw, Roll; };
        struct { FVector_UE5 Location; FRotator_UE5 Rotation; } ViewPointParams;
        Utils::CallFunction(CurrentPlayerController, STR("GetPlayerViewPoint"), &ViewPointParams);
        
        FVector_UE5 CameraLoc = ViewPointParams.Location;
        FRotator_UE5 CameraRot = ViewPointParams.Rotation;

        // 2. Convert Camera Rotation to a 3D Forward Unit Vector
        double PitchRad = CameraRot.Pitch * 0.01745329251; // Degrees to Radians
        double YawRad = CameraRot.Yaw * 0.01745329251;
        double CosPitch = std::cos(PitchRad);

        FVector_UE5 CameraForward;
        CameraForward.X = std::cos(YawRad) * CosPitch;
        CameraForward.Y = std::sin(YawRad) * CosPitch;
        CameraForward.Z = std::sin(PitchRad);

        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

        UObject* aimedPal = nullptr;
        double highestDot = -1.0;

        UObject* closestPal = nullptr;
        double closestDistSq = 999999999.0;

        //DP_LOG(Default, "=== DYN_PALS TARGET SCAN START (Origin: Camera Viewpoint) ===");

        for (UObject* Pal : AllPals) {
            if (Pal == PlayerPawn || !Pal) continue;

            struct { FVector_UE5 RetVal; } PalLocParams;
            Utils::CallFunction(Pal, STR("K2_GetActorLocation"), &PalLocParams);
            FVector_UE5 PalLoc = PalLocParams.RetVal;

            FVector_UE5 Dir;
            Dir.X = PalLoc.X - CameraLoc.X;
            Dir.Y = PalLoc.Y - CameraLoc.Y;
            Dir.Z = PalLoc.Z - CameraLoc.Z;

            double distSq = (Dir.X * Dir.X) + (Dir.Y * Dir.Y) + (Dir.Z * Dir.Z);
            double dist = std::sqrt(distSq);
            double distMeters = dist / 100.0; // Convert Unreal units (cm) to meters

            // Ignore Pals outside our 50m scanning radius
            if (dist > 5000.0) {
                continue;
            }

            // Calculate alignment dot product (1.0 = perfect center-screen crosshair alignment)
            FVector_UE5 DirNorm = { Dir.X / dist, Dir.Y / dist, Dir.Z / dist };
            double dot = CameraForward.X * DirNorm.X + CameraForward.Y * DirNorm.Y + CameraForward.Z * DirNorm.Z;

            // Verbose diagnostics: print details for every nearby candidate being scanned
            //DP_LOG(Default, "[AIM SCAN] Candidate: '{}' | Dist: {:.1f}m | Crosshair Alignment (Dot): {:.4f} (Cone req: >= 0.9700)\n", Pal->GetName(), distMeters, dot);

            // If the Pal is inside our 14-degree crosshair aiming cone (dot >= 0.97)
            if (dot >= 0.97) {
                if (dot > highestDot) {
                    highestDot = dot;
                    aimedPal = Pal;
                }
            }

            // Tracker for the closest fallback Pal
            if (distSq < closestDistSq) {
                closestDistSq = distSq;
                closestPal = Pal;
            }
        }

        // 3. Selection Resolution
        UObject* selectedPal = nullptr;
        if (aimedPal) {
            selectedPal = aimedPal;
            //DP_LOG(Default, "===> SELECTED AIMED TARGET: '{}' (Crosshair Alignment: {:.4f})\n", selectedPal->GetName(), highestDot);
        } else if (closestPal) {
            selectedPal = closestPal;
            //DP_LOG(Default, "===> NO PAL AIMED AT. FALLING BACK TO CLOSEST: '{}' (Distance: {:.1f}m)\n", selectedPal->GetName(), std::sqrt(closestDistSq) / 100.0);
        }

        //DP_LOG(Default, "=== DYN_PALS TARGET SCAN END ===");

        if (selectedPal) {
            TargetPal = selectedPal;

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

            UObject* PalUtil1 = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            struct { UObject* Char; FName RetVal; } CharIDParams1{TargetPal, FName()};
            if (PalUtil1) Utils::CallFunction(PalUtil1, STR("GetCharacterIDFromCharacter"), &CharIDParams1);
            TargetCharID = PalProcessor::Get().StripCharacterPrefix(CharIDParams1.RetVal.ToString());
        } else {
            DP_LOG(Warning, "Target Scan completed: No valid PalCharacters were found in 50-meter range.");
        }
    }
    void UIManager::BuildWidget() {
        if (!CurrentPlayerController) return;

        UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        UClass* WidgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (!WBL || !WidgetClass) return;

        struct { UObject* WorldContext; UClass* WidgetType; UObject* OwningPlayer; UObject* ReturnValue; } CreateParams{
            CurrentPlayerController, WidgetClass, CurrentPlayerController, nullptr
        };
        
        UFunction* CreateFunc = WBL->GetFunctionByNameInChain(STR("Create"));
        if (!CreateFunc) return;
        WBL->ProcessEvent(CreateFunc, &CreateParams);
        
        MyWidget = CreateParams.ReturnValue;
        if (!MyWidget) return;

        auto* FocusProp = Utils::GetProperty(MyWidget, STR("bIsFocusable"));
        if (FocusProp) {
            bool* pFocus = FocusProp->ContainerPtrToValuePtr<bool>(MyWidget);
            if (pFocus) *pFocus = true;
        }

        UObject* WidgetTree = nullptr;
        if (!Utils::GetPropertyValue(MyWidget, STR("WidgetTree"), WidgetTree) || !WidgetTree) return;

        auto ConstructElement = [&](UClass* ElementClass) -> UObject* {
            if (!ElementClass) return nullptr;
            FStaticConstructObjectParameters Params{ElementClass, MyWidget};
            Params.Name = FName(); // Leave empty for safe engine generation
            return UObjectGlobals::StaticConstructObject(Params);
        };

        UClass* CanvasPanelClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
        UClass* BorderClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Border"));
        UClass* ScrollBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.ScrollBox"));
        UClass* VerticalBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.VerticalBox"));
        UClass* HorizontalBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.HorizontalBox"));
        UClass* ComboBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.ComboBoxString"));
        UClass* CheckBoxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CheckBox"));
        UClass* SliderClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Slider"));
        UClass* TextBlockClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.TextBlock"));
        UClass* SpacerClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Spacer"));
        UClass* ButtonClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Button"));

        UObject* Canvas = ConstructElement(CanvasPanelClass);
        UObject* BackgroundBorder = ConstructElement(BorderClass);
        UObject* ScrollBox = ConstructElement(ScrollBoxClass);
        UObject* VBox = ConstructElement(VerticalBoxClass);

        auto* RootProp = Utils::GetProperty(WidgetTree, STR("RootWidget"));
        if (RootProp) {
            UObject** pRoot = RootProp->ContainerPtrToValuePtr<UObject*>(WidgetTree);
            if (pRoot) *pRoot = Canvas;
        }

        struct { UObject* Content; UObject* ReturnValue; } AddBorderParams{BackgroundBorder, nullptr};
        Utils::CallFunction(Canvas, STR("AddChild"), &AddBorderParams);
        UObject* CanvasSlot = AddBorderParams.ReturnValue;

        FAnchors Anchors{0.02, 0.10, 0.32, 0.90}; 
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
            UObject* Spacer = ConstructElement(SpacerClass);
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
        const FLinearColor PalBakerOrange  = {0.960f, 0.620f, 0.043f, 1.0f}; 
        const FLinearColor PalBakerRed     = {0.850f, 0.150f, 0.150f, 1.0f}; 
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

        auto SetAutoWrap = [&](UObject* Widget, bool bWrap) {
            if (!Widget) return;
            auto* WrapProp = Utils::GetProperty(Widget, STR("AutoWrapText"));
            if (WrapProp) {
                bool* pWrap = WrapProp->ContainerPtrToValuePtr<bool>(Widget);
                if (pWrap) *pWrap = bWrap;
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

        UObject* TitleText = ConstructElement(TextBlockClass);
        if (TitleText) {
            FText titleTextVal = ConvStringToText(L"DynPals Settings:\n" + TargetCharID);
            struct { FText InText; } SetTitleParams{titleTextVal};
            Utils::CallFunction(TitleText, STR("SetText"), &SetTitleParams);
            SetTextColor(TitleText, PalBakerCyan); 
            
            struct { UObject* Content; UObject* ReturnValue; } AddTitleParams{TitleText, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddTitleParams);
        }

        AddSpacer(15.0);

        UObject* SkinLabel = ConstructElement(TextBlockClass);
        if (SkinLabel) {
            FText labelVal = ConvStringToText(L"Current Swap:");
            struct { FText InText; } SetLabelParams{labelVal};
            Utils::CallFunction(SkinLabel, STR("SetText"), &SetLabelParams);
            SetTextColor(SkinLabel, PalBakerEmerald); 

            struct { UObject* Content; UObject* ReturnValue; } AddLabelParams{SkinLabel, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddLabelParams);
        }

        AddSpacer(5.0);

        // Fetch Live Attributes for Evaluation
        bool IsRare = false, IsWild = false;
        std::wstring GenderStr = L"None", SkinName = L"";
        int LevelNum = 1, RankNum = 0, FriendshipNum = 0;
        std::vector<std::wstring> Traits;

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue(TargetPal, STR("CharacterParameterComponent"), ParamComp);
        if (ParamComp) {
            UObject* IndivParam = nullptr;
            Utils::GetPropertyValue(ParamComp, STR("IndividualParameter"), IndivParam);
            if (IndivParam) {
                struct { bool RetVal; } RareParams{false};
                Utils::CallFunction(IndivParam, STR("IsRarePal"), &RareParams);
                IsRare = RareParams.RetVal;

                struct { uint8_t RetVal; } GenderParams{0};
                Utils::CallFunction(IndivParam, STR("GetGenderType"), &GenderParams);
                GenderStr = (GenderParams.RetVal == 1) ? L"Male" : ((GenderParams.RetVal == 2) ? L"Female" : L"None");

                struct { int32_t RetVal; } LevelParams{1};
                Utils::CallFunction(IndivParam, STR("GetLevel"), &LevelParams);
                LevelNum = LevelParams.RetVal;

                struct { int32_t RetVal; } RankParams{0};
                Utils::CallFunction(IndivParam, STR("GetRank"), &RankParams);
                RankNum = RankParams.RetVal;

                struct { int32_t RetVal; } FriendshipParams{0};
                Utils::CallFunction(IndivParam, STR("GetFriendshipPoint"), &FriendshipParams);
                FriendshipNum = FriendshipParams.RetVal;

                struct { FName RetVal; } SkinParams{FName()};
                Utils::CallFunction(IndivParam, STR("GetSkinName"), &SkinParams);
                SkinName = SkinParams.RetVal.ToString();
                if (SkinName == L"None") SkinName = L"";

                struct { TArray<FName> RetVal; } TraitsParams;
                Utils::CallFunction(IndivParam, STR("GetPassiveSkillList"), &TraitsParams);
                for (int32_t i = 0; i < TraitsParams.RetVal.Num(); ++i) {
                    Traits.push_back(TraitsParams.RetVal[i].ToString());
                }
            }
        }

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (PalUtil) {
            struct { UObject* Actor; bool RetVal; } WildParams{TargetPal, false};
            Utils::CallFunction(PalUtil, STR("IsWildNPC"), &WildParams);
            IsWild = WildParams.RetVal;
        }

        auto evaluations = ConfigManager::Get().EvaluateAllSwaps(TargetCharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild);
        
        int bestScore = 999999;
        int tieCount = 0;
        for (const auto& eval : evaluations) {
            if (!eval.IsValid) continue;
            if (eval.Score < bestScore) {
                bestScore = eval.Score;
                tieCount = 1;
            } else if (eval.Score == bestScore) {
                tieCount++;
            }
        }

        int totalTiedWeight = 0;
        for (const auto& eval : evaluations) {
            if (eval.IsValid && eval.Score == bestScore) {
                totalTiedWeight += ConfigManager::Get().GetConfigs()[eval.ConfigIndex].SpawnWeight;
            }
        }
        // 1. First, gather all valid evals per PackName
        std::map<std::wstring, std::vector<SwapEvaluation>> rawPacks;
        for (const auto& eval : evaluations) {
            if (bHideInvalidSwaps && !eval.IsValid) continue;
            rawPacks[ConfigManager::Get().GetConfigs()[eval.ConfigIndex].PackName].push_back(eval);
        }

        // 2. Process each pack to find groups and generate display names
        std::map<std::wstring, std::vector<std::pair<int, std::wstring>>> uiGroupedPacks;

        for (auto& [packName, evals] : rawPacks) {
            struct ParsedSkin {
                SwapEvaluation eval;
                std::wstring cleanName;
                std::vector<std::wstring> tokens;
                bool isManualLabel = false;
            };
            
            std::vector<ParsedSkin> parsedSkins;
            std::map<std::wstring, int> prefixCounts; 

            for (auto& eval : evals) {
                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                ParsedSkin ps;
                ps.eval = eval;
                
                std::wstring labelName = cfg.SwapLabel;
                if (labelName.empty()) labelName = cfg.SkinName;
                if (labelName.empty()) {
                    std::wstring filename = cfg.SkelMeshPath;
                    size_t lastSlash = filename.find_last_of(L'/');
                    if (lastSlash != std::wstring::npos) filename = filename.substr(lastSlash + 1);
                    size_t dotPos = filename.find(L'.');
                    if (dotPos != std::wstring::npos) filename = filename.substr(0, dotPos);
                    labelName = filename;
                }
                
                // If it looks like a raw filename (contains _ or starts with SK_)
                if (labelName.rfind(L"SK_", 0) == 0 || labelName.rfind(L"sk_", 0) == 0 || labelName.find(L'_') != std::wstring::npos) {
                    ps.isManualLabel = false;
                    std::wstring clean = labelName;
                    
                    if (clean.rfind(L"SK_", 0) == 0 || clean.rfind(L"sk_", 0) == 0) {
                        clean = clean.substr(3);
                    }
                    
                    // Case-insensitive strip of TargetCharID
                    std::wstring lowerClean = clean;
                    std::transform(lowerClean.begin(), lowerClean.end(), lowerClean.begin(), ::towlower);
                    std::wstring lowerCharID = TargetCharID;
                    std::transform(lowerCharID.begin(), lowerCharID.end(), lowerCharID.begin(), ::towlower);
                    
                    if (lowerClean.rfind(lowerCharID + L"_", 0) == 0) {
                        clean = clean.substr(TargetCharID.length() + 1);
                    } else if (lowerClean.rfind(lowerCharID, 0) == 0) {
                        clean = clean.substr(TargetCharID.length());
                        if (!clean.empty() && clean[0] == L'_') clean = clean.substr(1);
                    }
                    if (clean.empty()) {
                        clean = L"(Vanilla Mesh)";
                        ps.isManualLabel = true;
                    }
                    ps.cleanName = clean;
                    
                    size_t start = 0, end;
                    while ((end = clean.find(L'_', start)) != std::wstring::npos) {
                        if (end != start) ps.tokens.push_back(clean.substr(start, end - start));
                        start = end + 1;
                    }
                    if (start < clean.length()) ps.tokens.push_back(clean.substr(start));
                    
                    if (ps.tokens.size() >= 3) prefixCounts[ps.tokens[0] + L"_" + ps.tokens[1]]++;
                    if (ps.tokens.size() >= 2) prefixCounts[ps.tokens[0]]++;
                } else {
                    ps.isManualLabel = true;
                    ps.cleanName = labelName.empty() ? L"(Vanilla Mesh)" : labelName;
                }
                parsedSkins.push_back(ps);
            }
            
            for (auto& ps : parsedSkins) {
                std::wstring header = L"[ " + packName + L" ]";
                std::wstring display = ps.cleanName;
                
                if (!ps.isManualLabel) {
                    std::wstring bestPrefix = L"";
                    int prefixTokens = 0;
                    
                    if (ps.tokens.size() >= 3) {
                        std::wstring cand = ps.tokens[0] + L"_" + ps.tokens[1];
                        if (prefixCounts[cand] >= 2) {
                            bestPrefix = cand;
                            prefixTokens = 2;
                        }
                    }
                    if (bestPrefix.empty() && ps.tokens.size() >= 2) {
                        std::wstring cand = ps.tokens[0];
                        if (prefixCounts[cand] >= 2) {
                            bestPrefix = cand;
                            prefixTokens = 1;
                        }
                    }
                    
                    if (!bestPrefix.empty()) {
                        // Clean up the sub-category prefix and replace underscores with spaces
                        std::wstring cleanPrefix = bestPrefix;
                        std::replace(cleanPrefix.begin(), cleanPrefix.end(), L'_', L' ');
                        
                        // Split CamelCase inside the prefix if present
                        std::wstring splitPrefix = L"";
                        if (!cleanPrefix.empty()) {
                            splitPrefix.push_back(cleanPrefix[0]);
                            for (size_t i = 1; i < cleanPrefix.size(); ++i) {
                                if ((cleanPrefix[i - 1] >= L'a' && cleanPrefix[i - 1] <= L'z') && 
                                    (cleanPrefix[i] >= L'A' && cleanPrefix[i] <= L'Z')) {
                                    splitPrefix.push_back(L' ');
                                }
                                splitPrefix.push_back(cleanPrefix[i]);
                            }
                        }
                        
                        header += L" - " + splitPrefix;
                        
                        display = L"";
                        for(size_t i = prefixTokens; i < ps.tokens.size(); ++i) {
                            display += ps.tokens[i];
                            if (i < ps.tokens.size() - 1) display += L" ";
                        }
                    } else {
                        std::replace(display.begin(), display.end(), L'_', L' ');
                    }
                    
                    // CamelCase / PascalCase Splitting: Insert a space if a lowercase letter is followed by an uppercase letter
                    std::wstring splitDisplay = L"";
                    if (!display.empty()) {
                        splitDisplay.push_back(display[0]);
                        for (size_t i = 1; i < display.size(); ++i) {
                            if ((display[i - 1] >= L'a' && display[i - 1] <= L'z') && 
                                (display[i] >= L'A' && display[i] <= L'Z')) {
                                splitDisplay.push_back(L' ');
                            }
                            splitDisplay.push_back(display[i]);
                        }
                        display = splitDisplay;
                    }
                }
                
                uiGroupedPacks[header].push_back({ps.eval.ConfigIndex, display});
            }
        }

        // --- COMBO BOX (Smart Grouped Names) ---
        ComboBoxWidget = ConstructElement(ComboBoxClass);
        if (ComboBoxWidget) {
            GDropdownMapping.clear();
            LastSelectedOption = L"";
            StyleComboBox(ComboBoxWidget);

            for (auto& [headerStr, items] : uiGroupedPacks) {
                struct { FString Option; } AddHeaderP{ FString(headerStr.c_str()) };
                Utils::CallFunction(ComboBoxWidget, STR("AddOption"), &AddHeaderP);
                GDropdownMapping[headerStr] = -1; 

                for (const auto& item : items) {
                    int evalConfigIndex = item.first;
                    std::wstring displayLabel = item.second;

                    std::wstring optName = L"   " + displayLabel; 
                    while (GDropdownMapping.find(optName) != GDropdownMapping.end()) optName += L" "; 

                    struct { FString Option; } AddOptP{ FString(optName.c_str()) };
                    Utils::CallFunction(ComboBoxWidget, STR("AddOption"), &AddOptP);
                    GDropdownMapping[optName] = evalConfigIndex;

                    PalPersistData* persist = SaveManager::Get().GetPersistData(TargetInstanceID);
                    
                    int persistConfigIndex = persist ? ConfigManager::Get().FindConfigIndex(persist->PackName, persist->SkinName, persist->SwapLabel, persist->SkelMeshPath) : -1;
                    if (persistConfigIndex == evalConfigIndex) {
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

        AddSpacer(15.0);

        // --- FILTER CHECKBOX ---
        UObject* FilterHBox = ConstructElement(HorizontalBoxClass);
        if (FilterHBox && CheckBoxClass) {
            CheckBoxWidget = ConstructElement(CheckBoxClass);
            if (CheckBoxWidget) {
                struct { bool bInIsChecked; } CheckParams{bHideInvalidSwaps};
                Utils::CallFunction(CheckBoxWidget, STR("SetIsChecked"), &CheckParams);
                
                struct { UObject* Content; UObject* ReturnValue; } AddCheckParams{CheckBoxWidget, nullptr};
                Utils::CallFunction(FilterHBox, STR("AddChild"), &AddCheckParams);
            }

            UObject* CheckLabel = ConstructElement(TextBlockClass);
          if (CheckLabel) {
                FText labelVal = ConvStringToText(L" Hide Invalid Matches");
                struct { FText InText; } SetLabelParams{labelVal};
                Utils::CallFunction(CheckLabel, STR("SetText"), &SetLabelParams);
                SetTextColor(CheckLabel, OffWhite);
                
                // FIX: Pass the actual label widget as the content!
                struct { UObject* Content; UObject* ReturnValue; } AddLabelParams{CheckLabel, nullptr}; 
                Utils::CallFunction(FilterHBox, STR("AddChild"), &AddLabelParams);
            }

            struct { UObject* Content; UObject* ReturnValue; } AddHBoxParams{FilterHBox, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddHBoxParams);
            AddSpacer(15.0);
        }

        // --- RANDOMIZE BUTTON --- 
        RandomizeButtonWidget = ConstructElement(ButtonClass);
        if (RandomizeButtonWidget && TextBlockClass) {
            UObject* ButtonText = ConstructElement(TextBlockClass);
            if (ButtonText) {
                FText btnTextVal = ConvStringToText(L"Reroll");
                struct { FText InText; } SetTextParams{btnTextVal};
                Utils::CallFunction(ButtonText, STR("SetText"), &SetTextParams);
                SetTextColor(ButtonText, {0.012f, 0.078f, 0.153f, 1.0f}); // Dark blue text
                
                struct { UObject* Content; UObject* ReturnValue; } AddTextParams{ButtonText, nullptr};
                Utils::CallFunction(RandomizeButtonWidget, STR("AddChild"), &AddTextParams);
            }

            // Style with cyan theme
            struct { FLinearColor InColor; } ColorParams{PalBakerCyan};
            Utils::CallFunction(RandomizeButtonWidget, STR("SetBackgroundColor"), &ColorParams);

            struct { UObject* Content; UObject* ReturnValue; } AddBtnParams{RandomizeButtonWidget, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddBtnParams);
            
            AddSpacer(15.0);
        }

        // --- MATCHMAKER EVALUATION LOG ---
        UObject* EvalLogTitle = ConstructElement(TextBlockClass);
        if (EvalLogTitle) {
            FText titleVal = ConvStringToText(L"Evaluation Log:");
            struct { FText InText; } SetTitleParams{titleVal};
            Utils::CallFunction(EvalLogTitle, STR("SetText"), &SetTitleParams);
            SetTextColor(EvalLogTitle, PalBakerCyan);
            struct { UObject* Content; UObject* ReturnValue; } AddParams{EvalLogTitle, nullptr};
            Utils::CallFunction(VBox, STR("AddChild"), &AddParams);
        }
        AddSpacer(5.0);

        if (evaluations.empty()) {
            UObject* NoEvalText = ConstructElement(TextBlockClass);
            if (NoEvalText) {
                FText textVal = ConvStringToText(L"No swaps configured for this Pal.");
                struct { FText InText; } SetTextParams{textVal};
                Utils::CallFunction(NoEvalText, STR("SetText"), &SetTextParams);
                SetTextColor(NoEvalText, OffWhite);
                struct { UObject* Content; UObject* ReturnValue; } AddParams{NoEvalText, nullptr};
                Utils::CallFunction(VBox, STR("AddChild"), &AddParams);
            }
        } else {
            for (const auto& eval : evaluations) {
                if (bHideInvalidSwaps && !eval.IsValid) continue;

                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                
                UObject* PackText = ConstructElement(TextBlockClass);
                if (PackText) {
                    FText pTextVal = ConvStringToText(cfg.PackName);
                    struct { FText InText; } SetTextParams{pTextVal};
                    Utils::CallFunction(PackText, STR("SetText"), &SetTextParams);
                    SetTextColor(PackText, OffWhite);
                    struct { UObject* Content; UObject* ReturnValue; } AddParams{PackText, nullptr};
                    Utils::CallFunction(VBox, STR("AddChild"), &AddParams);
                }

                std::wstring processedFilename = cfg.SkelMeshPath;
                size_t lastSlash = processedFilename.find_last_of(L'/');
                if (lastSlash != std::wstring::npos) processedFilename = processedFilename.substr(lastSlash + 1);
                
                size_t dotPos = processedFilename.find(L'.');
                if (dotPos != std::wstring::npos) processedFilename = processedFilename.substr(0, dotPos);

                if (processedFilename.rfind(L"SK_", 0) == 0 || processedFilename.rfind(L"sk_", 0) == 0) processedFilename = processedFilename.substr(3);
                for (wchar_t& c : processedFilename) { if (c == L'_') c = L' '; }

                int pct = 0;
                if (eval.IsValid && eval.Score == bestScore && totalTiedWeight > 0) {
                    pct = (cfg.SpawnWeight * 100) / totalTiedWeight;
                }

                FLinearColor textColor;
                if (!eval.IsValid) textColor = PalBakerRed;
                else if (eval.Score < 0) textColor = PalBakerCyan;
                else if (eval.Score == 0) textColor = PalBakerEmerald;
                else textColor = PalBakerOrange;

                std::wstring logStr = L"    " + std::to_wstring(pct) + L"% : " + processedFilename;

                UObject* LogText = ConstructElement(TextBlockClass);
                if (LogText) {
                    FText textVal = ConvStringToText(logStr);
                    struct { FText InText; } SetTextParams{textVal};
                    Utils::CallFunction(LogText, STR("SetText"), &SetTextParams);
                    SetTextColor(LogText, textColor);
                    SetAutoWrap(LogText, true);
                    struct { UObject* Content; UObject* ReturnValue; } AddParams{LogText, nullptr};
                    Utils::CallFunction(VBox, STR("AddChild"), &AddParams);
                }
                
                AddSpacer(8.0);
            }
        }
        
        AddSpacer(15.0);

        // --- SLIDERS ---
        ActiveSliders.clear();
        PalPersistData* persist = SaveManager::Get().GetPersistData(TargetInstanceID);
        int persistConfigIndex = persist ? ConfigManager::Get().FindConfigIndex(persist->PackName, persist->SkinName, persist->SwapLabel, persist->SkelMeshPath) : -1;

        if (persist && persistConfigIndex != -1) {
            auto& activeCfg = ConfigManager::Get().GetConfigs()[persistConfigIndex];

            
            if (!activeCfg.MorphTargetList.empty()) {
                UObject* MorphLabel = ConstructElement(TextBlockClass);
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
                    UObject* Label = ConstructElement(TextBlockClass);
                    if (Label) {
                        FText labelVal = ConvStringToText(morph.target);
                        struct { FText InText; } SetLabelParams{labelVal};
                        Utils::CallFunction(Label, STR("SetText"), &SetLabelParams);
                        SetTextColor(Label, OffWhite); 

                        struct { UObject* Content; UObject* ReturnValue; } AddLabelParams{Label, nullptr};
                        Utils::CallFunction(VBox, STR("AddChild"), &AddLabelParams);
                    }

                    UObject* Slider = ConstructElement(SliderClass);
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

    void UIManager::TickUI(UObject* LocalPlayerController) {
        CurrentPlayerController = LocalPlayerController; // Cache natively bound player

        // Process cross-thread toggle requests safely (such as Alt+N)
        if (bToggleRequested) {
            bToggleRequested = false;
            ToggleMenu();
        }

        if (!bIsMenuOpen || !MyWidget) return;

        // Close the menu when Escape is pressed
        static bool bWasEscapeDown = false;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            if (!bWasEscapeDown) {
                bWasEscapeDown = true;
                ToggleMenu(); 
                return; 
            }
        } else {
            bWasEscapeDown = false;
        }

        if (CheckBoxWidget) {
            struct { bool ReturnValue; } IsCheckedParams{false};
            Utils::CallFunction(CheckBoxWidget, STR("IsChecked"), &IsCheckedParams);
            if (IsCheckedParams.ReturnValue != bHideInvalidSwaps) {
                bHideInvalidSwaps = IsCheckedParams.ReturnValue;
                DestroyWidget();
                BuildWidget();
                LockInput(true);
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
                        LockInput(true); 
                        return; 
                    }
                }
            }
        }

        // --- BUTTON POLLING FOR RANDOMIZE ---
        if (RandomizeButtonWidget) {
            struct { bool ReturnValue; } IsPressedParams{false};
            Utils::CallFunction(RandomizeButtonWidget, STR("IsPressed"), &IsPressedParams);
            
            if (IsPressedParams.ReturnValue) {
                if (!bWasRandomizePressed) {
                    bWasRandomizePressed = true;
                    
                    DP_LOG(Default, "Randomize button clicked. Rerolling swap and morphs...");
                    PalProcessor::Get().ProcessPal(TargetPal, true);
                    
                    DestroyWidget();
                    BuildWidget();
                    LockInput(true);
                    return; 
                }
            } else {
                bWasRandomizePressed = false;
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
                SaveManager::Get().SetPersistData(TargetInstanceID, *persist, true); 
            }
        }
    }

    void UIManager::DestroyWidget() {
        if (MyWidget) {
            Utils::CallFunction(MyWidget, STR("RemoveFromParent"));
            MyWidget = nullptr;
        }
        ComboBoxWidget = nullptr;
        CheckBoxWidget = nullptr;
        RandomizeButtonWidget = nullptr;
        bWasRandomizePressed = false;
        ActiveSliders.clear();
    }
}