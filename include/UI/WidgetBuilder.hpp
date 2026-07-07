#pragma once
#include <string>
#include <functional>
#include <vector>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/FString.hpp>
#include "Utils.hpp"
#include "DataTypes.hpp"
#include "UI/IconLibrary.hpp"

namespace DynPals {

    struct FVector2D_Builder { double X; double Y; };
    struct FBuilderMargin { float Left; float Top; float Right; float Bottom; };
    struct FBuilderAnchors { FVector2D_Builder Minimum; FVector2D_Builder Maximum; };
    struct FBuilderSlateColor { FLinearColor_UE5 SpecifiedColor; uint8_t ColorUseRule; uint8_t Pad[3]; };
    enum class EBuilderSlateSizeRule : uint8_t { Automatic = 0, Fill = 1 };
    struct FBuilderSlateChildSize { float Value; EBuilderSlateSizeRule SizeRule; };
    enum class EBuilderHorizontalAlignment : uint8_t { HAlign_Fill, HAlign_Left, HAlign_Center, HAlign_Right };
    enum class EBuilderVerticalAlignment : uint8_t { VAlign_Fill, VAlign_Top, VAlign_Center, VAlign_Bottom };
    enum class EBuilderStretch : uint8_t { None, Fill, ScaleToFit, ScaleToFitX, ScaleToFitY, ScaleToFill, ScaleBySafeZone, UserSpecified };

    // ==========================================
    // SLOT BUILDERS
    // ==========================================
    class CanvasSlotBuilder {
        RC::Unreal::UObject* Slot;
    public:
        CanvasSlotBuilder(RC::Unreal::UObject* InSlot) : Slot(InSlot) {}
        CanvasSlotBuilder& Anchors(double MinX, double MinY, double MaxX, double MaxY) {
            FBuilderAnchors A{{MinX, MinY}, {MaxX, MaxY}};
            struct { FBuilderAnchors InAnchors; } Params{A};
            Utils::CallFunction(Slot, STR("SetAnchors"), &Params);
            return *this;
        }
        CanvasSlotBuilder& Offsets(float Left, float Top, float Right, float Bottom) {
            FBuilderMargin M{Left, Top, Right, Bottom};
            struct { FBuilderMargin InOffsets; } Params{M};
            Utils::CallFunction(Slot, STR("SetOffsets"), &Params);
            return *this;
        }
        CanvasSlotBuilder& Alignment(double X, double Y) {
            FVector2D_Builder Align{X, Y};
            struct { FVector2D_Builder InAlignment; } Params{Align};
            Utils::CallFunction(Slot, STR("SetAlignment"), &Params);
            return *this;
        }
        CanvasSlotBuilder& AutoSize(bool bAutoSize) {
            struct { bool bInAutoSize; } Params{bAutoSize};
            Utils::CallFunction(Slot, STR("SetAutoSize"), &Params);
            return *this;
        }
    };

    class BoxSlotBuilder {
        RC::Unreal::UObject* Slot;
    public:
        BoxSlotBuilder(RC::Unreal::UObject* InSlot) : Slot(InSlot) {}
        BoxSlotBuilder& Padding(float Left, float Top, float Right, float Bottom) {
            FBuilderMargin M{Left, Top, Right, Bottom};
            struct { FBuilderMargin InPadding; } Params{M};
            Utils::CallFunction(Slot, STR("SetPadding"), &Params);
            return *this;
        }
        BoxSlotBuilder& Size(EBuilderSlateSizeRule Rule, float Value = 1.0f) {
            FBuilderSlateChildSize S{Value, Rule};
            struct { FBuilderSlateChildSize InSize; } Params{S};
            Utils::CallFunction(Slot, STR("SetSize"), &Params);
            return *this;
        }
        BoxSlotBuilder& HorizontalAlignment(EBuilderHorizontalAlignment Align) {
            struct { EBuilderHorizontalAlignment InAlign; } Params{Align};
            Utils::CallFunction(Slot, STR("SetHorizontalAlignment"), &Params);
            return *this;
        }
        BoxSlotBuilder& VerticalAlignment(EBuilderVerticalAlignment Align) {
            struct { EBuilderVerticalAlignment InAlign; } Params{Align};
            Utils::CallFunction(Slot, STR("SetVerticalAlignment"), &Params);
            return *this;
        }
    };

    // ==========================================
    // WIDGET BUILDER
    // ==========================================
    class WidgetBuilder {
    private:
        RC::Unreal::UObject* Widget;
        RC::Unreal::UObject* KTL;

        RC::Unreal::UClass* GetClass(const wchar_t* Path) {
            return RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, Path);
        }

    public:
        WidgetBuilder(RC::Unreal::UObject* ExistingWidget) : Widget(ExistingWidget) {
            KTL = DynPals::Utils::GetKTL();
        }

        WidgetBuilder(const wchar_t* ClassPath, RC::Unreal::UObject* Outer) {
            RC::Unreal::UClass* Cls = GetClass(ClassPath);
            if (!Cls) {
                Cls = static_cast<RC::Unreal::UClass*>(Utils::LoadAssetSafely(ClassPath));
            }
            if (!Cls) return;

            // Clean cached retrievals
            KTL = DynPals::Utils::GetKTL();
            RC::Unreal::UClass* UserWidgetClass = DynPals::Utils::GetUserWidgetClass();
            
            if (UserWidgetClass && Cls->IsChildOf(UserWidgetClass)) {
                RC::Unreal::UObject* WBL = DynPals::Utils::GetWBL();
                RC::Unreal::UFunction* CreateFunc = DynPals::Utils::GetWBLFunction(STR("Create"));

                if (WBL && CreateFunc) {
                    RC::Unreal::UObject* PC = RC::Unreal::UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
                    struct { RC::Unreal::UObject* WorldContext; RC::Unreal::UClass* WidgetType; RC::Unreal::UObject* OwningPlayer; RC::Unreal::UObject* ReturnValue; } CreateParams{
                        PC, Cls, PC, nullptr
                    };
                    WBL->ProcessEvent(CreateFunc, &CreateParams);
                    Widget = CreateParams.ReturnValue;
                }
            } else {
                RC::Unreal::FStaticConstructObjectParameters Params{Cls, Outer};
                Params.Name = RC::Unreal::FName(); 
                Widget = RC::Unreal::UObjectGlobals::StaticConstructObject(Params);

            }
        }

        RC::Unreal::UObject* Build() const { return Widget; }

        WidgetBuilder& AddToCanvas(const WidgetBuilder& Child, std::function<void(CanvasSlotBuilder&)> SlotConfig = nullptr) {
            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } Params{Child.Build(), nullptr};
            Utils::CallFunction(Widget, STR("AddChildToCanvas"), &Params);
            if (SlotConfig && Params.ReturnValue) { CanvasSlotBuilder Sb(Params.ReturnValue); SlotConfig(Sb); }
            return *this;
        }

        WidgetBuilder& AddToHorizontalBox(const WidgetBuilder& Child, std::function<void(BoxSlotBuilder&)> SlotConfig = nullptr) {
            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } Params{Child.Build(), nullptr};
            Utils::CallFunction(Widget, STR("AddChildToHorizontalBox"), &Params);
            if (SlotConfig && Params.ReturnValue) { BoxSlotBuilder Sb(Params.ReturnValue); SlotConfig(Sb); }
            return *this;
        }

        WidgetBuilder& AddToVerticalBox(const WidgetBuilder& Child, std::function<void(BoxSlotBuilder&)> SlotConfig = nullptr) {
            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } Params{Child.Build(), nullptr};
            Utils::CallFunction(Widget, STR("AddChildToVerticalBox"), &Params);
            if (SlotConfig && Params.ReturnValue) { BoxSlotBuilder Sb(Params.ReturnValue); SlotConfig(Sb); }
            return *this;
        }

        WidgetBuilder& AddChild(const WidgetBuilder& Child) {
            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } Params{Child.Build(), nullptr};
            Utils::CallFunction(Widget, STR("AddChild"), &Params);
            return *this;
        }

        WidgetBuilder& Opacity(float Alpha) {
            struct { float InOpacity; } Params{Alpha};
            Utils::CallFunction(Widget, STR("SetRenderOpacity"), &Params);
            return *this;
        }

        WidgetBuilder& DesiredSizeOverride(double Width, double Height) {
            if (!Widget) return *this;

            // 1. First choice: If it's a SizeBox, modify width and height overrides
            RC::Unreal::UFunction* SetWidthFunc = Widget->GetFunctionByNameInChain(STR("SetWidthOverride"));
            RC::Unreal::UFunction* SetHeightFunc = Widget->GetFunctionByNameInChain(STR("SetHeightOverride"));
            if (SetWidthFunc && SetHeightFunc) {
                struct { float InWidth; } WParams{ static_cast<float>(Width) };
                struct { float InHeight; } HParams{ static_cast<float>(Height) };
                Widget->ProcessEvent(SetWidthFunc, &WParams);
                Widget->ProcessEvent(SetHeightFunc, &HParams);
                return *this;
            }

            // 2. Second choice: Try standard SetDesiredSizeOverride
            RC::Unreal::UFunction* SetDesiredSizeFunc = Widget->GetFunctionByNameInChain(STR("SetDesiredSizeOverride"));
            if (SetDesiredSizeFunc) {
                struct { FVector2D_Builder InSize; } Params{ {Width, Height} };
                Widget->ProcessEvent(SetDesiredSizeFunc, &Params);
                return *this;
            }

            // 3. Third choice: If it's a wrapper like WBP_CommonButton_C, unwrap it and size the inner PalInvisibleButton
            RC::Unreal::UObject* InnerBtn = nullptr;
            if (Utils::GetPropertyValue<RC::Unreal::UObject*>(Widget, STR("WBP_PalInvisibleButton"), InnerBtn, true) && InnerBtn) {
                RC::Unreal::UFunction* SetMinDimFunc = InnerBtn->GetFunctionByNameInChain(STR("SetMinDimensions"));
                if (SetMinDimFunc) {
                    struct { int32_t MinW; int32_t MinH; } DimParams{ static_cast<int32_t>(Width), static_cast<int32_t>(Height) };
                    InnerBtn->ProcessEvent(SetMinDimFunc, &DimParams);
                    return *this;
                }
            }

            // 4. Fourth choice: If the widget IS the CommonButtonBase natively
            RC::Unreal::UFunction* SetMinDimFunc = Widget->GetFunctionByNameInChain(STR("SetMinDimensions"));
            if (SetMinDimFunc) {
                struct { int32_t MinW; int32_t MinH; } DimParams{ static_cast<int32_t>(Width), static_cast<int32_t>(Height) };
                Widget->ProcessEvent(SetMinDimFunc, &DimParams);
                return *this;
            }

            // 5. Critical Warning: Only print if all sizing paths failed
            DP_LOG(Warning, "[WidgetBuilder] DesiredSizeOverride FAILED: No sizing function ('SetMinDimensions', 'SetWidth/HeightOverride', 'SetDesiredSizeOverride') found on object '{}'", Widget->GetName());
            return *this;
        }


        WidgetBuilder& UnlockButtonSize(float TargetWidth) {
            if (!Widget) return *this;

            bool bFoundText = false;
            bool bFoundBtn = false;

            // 1. Target the text block directly (exposed because bIsVariable = true)
            RC::Unreal::UObject* TextMain = nullptr;
            if (Utils::GetPropertyValue<RC::Unreal::UObject*>(Widget, STR("Text_Main"), TextMain, true) && TextMain) {
                Utils::SetPropertyValue<float>(TextMain, STR("MaxWidth"), TargetWidth);
                Utils::SetPropertyValue<bool>(TextMain, STR("AutoWrapText"), false);
                Utils::SetPropertyValue<bool>(TextMain, STR("IsAutoAdjustScale"), false);
                bFoundText = true;
            }

            // 2. Expand the native clickable hit-box of the inner button
            RC::Unreal::UObject* InnerBtn = nullptr;
            if (Utils::GetPropertyValue<RC::Unreal::UObject*>(Widget, STR("WBP_PalInvisibleButton"), InnerBtn, true) && InnerBtn) {
                RC::Unreal::UFunction* SetMinDimFunc = InnerBtn->GetFunctionByNameInChain(STR("SetMinDimensions"));
                if (SetMinDimFunc) {
                    struct { int32_t MinW; int32_t MinH; } DimParams{ static_cast<int32_t>(TargetWidth), 45 };
                    InnerBtn->ProcessEvent(SetMinDimFunc, &DimParams);
                }
                bFoundBtn = true;
            }

            // 3. Diagnostic Fallback: If both fail, inject a searchable Tooltip for UE4SS Live View
            if (!bFoundText && !bFoundBtn) {
                std::wstring className = Widget->GetClassPrivate()->GetName();
                std::wstring objName = Widget->GetName();
                
                wchar_t addressBuf[32];
                swprintf(addressBuf, 32, L"0x%p", (void*)Widget);
                std::wstring uniqueDebugTag = L"DYNPALS_DEBUG_FAILED_WIDGET_ADDR_" + std::wstring(addressBuf);

                DP_LOG(Warning, "[WidgetBuilder] UnlockButtonSize: Mismatch! Class: '{}' | Name: '{}' | Addr: {}. Injecting tag into ToolTipText...", 
                       className, objName, addressBuf);

                // Construct an FText with the unique address tag
                RC::Unreal::UObject* KTL = Utils::GetKTL();
                RC::Unreal::UFunction* ConvFunc = Utils::GetKTLFunction(STR("Conv_StringToText"));
                if (KTL && ConvFunc) {
                    struct { RC::Unreal::FString InStr; RC::Unreal::FText OutText; } ConvParams{ RC::Unreal::FString(uniqueDebugTag.c_str()), RC::Unreal::FText() };
                    KTL->ProcessEvent(ConvFunc, &ConvParams);
                    // Write to ToolTipText so it can be dumped via Live View query
                    Utils::SetPropertyValue<RC::Unreal::FText>(Widget, STR("ToolTipText"), ConvParams.OutText, true);
                }
            }

            return *this;
        }


        WidgetBuilder& Text(const std::wstring& Str) {
            // Fully qualify UFunction with its namespace
            RC::Unreal::UFunction* ConvFunc = DynPals::Utils::GetKTLFunction(STR("Conv_StringToText"));
            if (!KTL || !ConvFunc) return *this;

            struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } P1{ RC::Unreal::FString(Str.c_str()), RC::Unreal::FText() };
            KTL->ProcessEvent(ConvFunc, &P1);
            
            struct { RC::Unreal::FText InText; } P2{P1.ReturnValue};
            Utils::CallFunction(Widget, STR("SetText"), &P2);
            return *this;
        }


        WidgetBuilder& TextColor(const FLinearColor_UE5& Col) {
            FBuilderSlateColor SlateCol{Col, 0, {0,0,0}};
            struct { FBuilderSlateColor InColorAndOpacity; } Params{SlateCol};
            Utils::CallFunction(Widget, STR("SetColorAndOpacity"), &Params);
            return *this;
        }

        WidgetBuilder& Font(RC::Unreal::UObject* FontAsset, const wchar_t* Typeface, int32_t Size) {
            RC::Unreal::FProperty* FontProp = Utils::GetProperty(Widget, STR("Font"));
            if (FontProp) {
                void* FontPtr = FontProp->ContainerPtrToValuePtr<void>(Widget);
                if (FontPtr) {
                    RC::Unreal::FStructProperty* StructProp = static_cast<RC::Unreal::FStructProperty*>(FontProp);
                    if (StructProp && StructProp->GetStruct()) {
                        RC::Unreal::UStruct* FontStruct = StructProp->GetStruct();
                        
                        if (FontAsset) {
                            RC::Unreal::FProperty* ObjProp = FontStruct->GetPropertyByNameInChain(STR("FontObject"));
                            if (ObjProp) {
                                RC::Unreal::UObject** Ptr = ObjProp->ContainerPtrToValuePtr<RC::Unreal::UObject*>(FontPtr);
                                if (Ptr) *Ptr = FontAsset;
                            }
                        }
                        
                        RC::Unreal::FProperty* NameProp = FontStruct->GetPropertyByNameInChain(STR("TypefaceFontName"));
                        if (NameProp) {
                            RC::Unreal::FName* NamePtr = NameProp->ContainerPtrToValuePtr<RC::Unreal::FName>(FontPtr);
                            if (NamePtr) *NamePtr = RC::Unreal::FName(Typeface, RC::Unreal::FNAME_Add);
                        }
                        
                        RC::Unreal::FProperty* SizeProp = FontStruct->GetPropertyByNameInChain(STR("Size"));
                        if (SizeProp) {
                            int32_t* SizePtr = SizeProp->ContainerPtrToValuePtr<int32_t>(FontPtr);
                            if (SizePtr) *SizePtr = Size;
                        }
                    }
                }
                Utils::CallFunction(Widget, STR("SetFont"), FontPtr);
            }
            return *this;
        }

        WidgetBuilder& TextOutline(int32_t OutlineSize, const FLinearColor_UE5& Color) {
            RC::Unreal::FProperty* FontProp = Utils::GetProperty(Widget, STR("Font"));
            if (FontProp) {
                void* FontPtr = FontProp->ContainerPtrToValuePtr<void>(Widget);
                if (FontPtr) {
                    RC::Unreal::FStructProperty* StructProp = static_cast<RC::Unreal::FStructProperty*>(FontProp);
                    if (StructProp && StructProp->GetStruct()) {
                        RC::Unreal::FProperty* OutlineProp = StructProp->GetStruct()->GetPropertyByNameInChain(STR("OutlineSettings"));
                        if (OutlineProp) {
                            RC::Unreal::FStructProperty* OutlineStructProp = static_cast<RC::Unreal::FStructProperty*>(OutlineProp);
                            if (OutlineStructProp && OutlineStructProp->GetStruct()) {
                                void* OutlinePtr = OutlineStructProp->ContainerPtrToValuePtr<void>(FontPtr);
                                if (OutlinePtr) {
                                    RC::Unreal::FProperty* SizeProp = OutlineStructProp->GetStruct()->GetPropertyByNameInChain(STR("OutlineSize"));
                                    if (SizeProp) {
                                        int32_t* SizePtr = SizeProp->ContainerPtrToValuePtr<int32_t>(OutlinePtr);
                                        if (SizePtr) *SizePtr = OutlineSize;
                                    }

                                    RC::Unreal::FProperty* ColorProp = OutlineStructProp->GetStruct()->GetPropertyByNameInChain(STR("OutlineColor"));
                                    if (ColorProp) {
                                        FLinearColor_UE5* ColorPtr = ColorProp->ContainerPtrToValuePtr<FLinearColor_UE5>(OutlinePtr);
                                        if (ColorPtr) *ColorPtr = Color;
                                    }
                                }
                            }
                        }
                    }
                }
                Utils::CallFunction(Widget, STR("SetFont"), FontPtr);
            }
            return *this;
        }

        WidgetBuilder& BackgroundColor(const FLinearColor_UE5& Col) {
            if (!Widget) return *this;
            struct { FLinearColor_UE5 Color; } Params{Col};

            // 1. First choice: Use SetColorAndOpacity if it's a UserWidget/TextBlock
            RC::Unreal::UFunction* SetColorFunc = Widget->GetFunctionByNameInChain(STR("SetColorAndOpacity"));
            if (SetColorFunc) {
                struct FBuilderSlateColor { FLinearColor_UE5 SpecifiedColor; uint8_t ColorUseRule; uint8_t Pad[3]; };
                struct { FBuilderSlateColor InColorAndOpacity; } SCParams{ {Col, 0, {0,0,0}} };
                Widget->ProcessEvent(SetColorFunc, &SCParams);
                return *this;
            }

            // 2. Second choice: Try SetBackgroundColor (standard for UButtons)
            RC::Unreal::UFunction* SetBgColorFunc = Widget->GetFunctionByNameInChain(STR("SetBackgroundColor"));
            if (SetBgColorFunc) {
                Widget->ProcessEvent(SetBgColorFunc, &Params);
                return *this;
            }

            // 3. Third choice: Try SetBrushColor (standard for UBorders)
            RC::Unreal::UFunction* SetBrushColorFunc = Widget->GetFunctionByNameInChain(STR("SetBrushColor"));
            if (SetBrushColorFunc) {
                Widget->ProcessEvent(SetBrushColorFunc, &Params);
                return *this;
            }

            // 4. Critical Warning: Only print if all coloring paths failed
            DP_LOG(Warning, "[WidgetBuilder] BackgroundColor FAILED: No coloring function ('SetColorAndOpacity', 'SetBackgroundColor', 'SetBrushColor') found on object '{}'", Widget->GetName());
            return *this;
        }



        
        WidgetBuilder& BrushColor(const FLinearColor_UE5& Col) {
            struct { FLinearColor_UE5 InColor; } Params{Col};
            Utils::CallFunction(Widget, STR("SetBrushColor"), &Params);
            return *this;
        }
        
        WidgetBuilder& Padding(float Left, float Top, float Right, float Bottom) {
            FBuilderMargin M{Left, Top, Right, Bottom};
            struct { FBuilderMargin InPadding; } Params{M};
            Utils::CallFunction(Widget, STR("SetPadding"), &Params);
            return *this;
        }

        WidgetBuilder& ImageFromAsset(const wchar_t* AssetPath) {
            RC::Unreal::UObject* Tex = Utils::LoadAssetSafely(AssetPath);
            if (Tex) {
                std::wstring className = Widget->GetClassPrivate()->GetName();
                if (className.find(L"Image") != std::wstring::npos) {
                    struct { RC::Unreal::UObject* Texture; bool bMatchSize; } Params{Tex, false};
                    Utils::CallFunction(Widget, STR("SetBrushFromTexture"), &Params);
                } else if (className.find(L"Border") != std::wstring::npos) {
                    struct { RC::Unreal::UObject* Texture; } Params{Tex};
                    Utils::CallFunction(Widget, STR("SetBrushFromTexture"), &Params);
                }
            }
            return *this;
        }

        WidgetBuilder& ImageSize(double Width, double Height) {
            FVector2D_Builder Size{Width, Height};
            struct { FVector2D_Builder InSize; } Params{Size};
            Utils::CallFunction(Widget, STR("SetDesiredSizeOverride"), &Params);
            return *this;
        }

        WidgetBuilder& ImageColor(const FLinearColor_UE5& Col) {
            struct { FLinearColor_UE5 InColorAndOpacity; } Params{Col};
            Utils::CallFunction(Widget, STR("SetColorAndOpacity"), &Params);
            return *this;
        }

        WidgetBuilder& PackWindowContent(const WidgetBuilder& ContentLayout) {
            RC::Unreal::UObject* NamedSlot = nullptr;
            if (Utils::GetPropertyValue<RC::Unreal::UObject*>(Widget, STR("NamedSlot_91"), NamedSlot) && NamedSlot) {
                struct { RC::Unreal::UObject* Content; } Params{ContentLayout.Build()};
                Utils::CallFunction(NamedSlot, STR("AddChild"), &Params);
            }
            return *this;
        }

        WidgetBuilder& WidthOverride(float Width) {
            struct { float W; } Params{Width};
            Utils::CallFunction(Widget, STR("SetWidthOverride"), &Params);
            return *this;
        }
        
        WidgetBuilder& HeightOverride(float Height) {
            struct { float H; } Params{Height};
            Utils::CallFunction(Widget, STR("SetHeightOverride"), &Params);
            return *this;
        }

        WidgetBuilder& Stretch(EBuilderStretch InStretch) {
            struct { EBuilderStretch S; } Params{InStretch};
            Utils::CallFunction(Widget, STR("SetStretch"), &Params);
            return *this;
        }

        WidgetBuilder& SetupSlider(double Value, double Min, double Max) {
            struct { double V; double M; double Mx; } Params{Value, Min, Max};
            Utils::CallFunction(Widget, STR("SetValue"), &Params);
            return *this;
        }

        WidgetBuilder& SetupSwitch(bool bIsOn) {
            struct { bool bOn; } Params{bIsOn};
            Utils::CallFunction(Widget, STR("Setup"), &Params);
            return *this;
        }

        WidgetBuilder& SetupTab(const std::wstring& Name, int32_t Index) {
            RC::Unreal::FString InString(Name.c_str());
            struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } ConvParams{ InString, RC::Unreal::FText() };
            KTL->ProcessEvent(KTL->GetFunctionByNameInChain(STR("Conv_StringToText")), &ConvParams);
            
            struct { RC::Unreal::FText N; int32_t I; } Params{ConvParams.ReturnValue, Index};
            Utils::CallFunction(Widget, STR("SetName"), &Params);
            return *this;
        }
        
        WidgetBuilder& SetTabActive(bool bIsActive) {
            struct { bool bA; } Params{bIsActive};
            Utils::CallFunction(Widget, STR("SetTabActive"), &Params);
            return *this;
        }

        WidgetBuilder& SetupLR(const std::vector<std::wstring>& Options, int32_t CurrentIndex) {
            RC::Unreal::UFunction* SetupFunc = Widget->GetFunctionByNameInChain(STR("SetupSelections"));
            if (SetupFunc) {
                alignas(8) uint8_t Params[128] = {0}; 
                
                RC::Unreal::FProperty* SelProp = SetupFunc->GetPropertyByNameInChain(STR("Selections"));
                RC::Unreal::FProperty* CurProp = SetupFunc->GetPropertyByNameInChain(STR("Current"));
                
                if (SelProp && CurProp) {
                    RC::Unreal::TArray<RC::Unreal::FString>* Arr = static_cast<RC::Unreal::TArray<RC::Unreal::FString>*>(SelProp->ContainerPtrToValuePtr<void>(Params));
                    int32_t* Cur = CurProp->ContainerPtrToValuePtr<int32_t>(Params);
                    
                    if (Arr && Cur) {
                        *Cur = CurrentIndex;
                        for (const auto& opt : Options) {
                            RC::Unreal::FString Str(opt.c_str());
                            Arr->Add(Str);
                        }
                        Widget->ProcessEvent(SetupFunc, Params);
                    }
                }
            }
            return *this;
        }
    };

    namespace UI {
        inline DynPals::WidgetBuilder Canvas(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(STR("/Script/UMG.CanvasPanel"), Outer); }
        inline DynPals::WidgetBuilder HorizontalBox(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(STR("/Script/UMG.HorizontalBox"), Outer); }
        inline DynPals::WidgetBuilder VerticalBox(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(STR("/Script/UMG.VerticalBox"), Outer); }
        inline DynPals::WidgetBuilder Border(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(STR("/Script/UMG.Border"), Outer); }
        inline DynPals::WidgetBuilder Image(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(STR("/Script/UMG.Image"), Outer); }
        inline DynPals::WidgetBuilder SizeBox(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(STR("/Script/UMG.SizeBox"), Outer); }
        inline DynPals::WidgetBuilder ScaleBox(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(STR("/Script/UMG.ScaleBox"), Outer); }
        inline DynPals::WidgetBuilder ScrollBox(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(STR("/Script/UMG.ScrollBox"), Outer); }

        inline DynPals::WidgetBuilder Window(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(DynPals::UI::Assets::Blueprints::CommonWindow, Outer); }
        inline DynPals::WidgetBuilder Button(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(DynPals::UI::Assets::Blueprints::CommonButton, Outer); }
        inline DynPals::WidgetBuilder Text(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(DynPals::UI::Assets::Blueprints::PalTextBlock, Outer); }
        inline DynPals::WidgetBuilder ActionBar(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(DynPals::UI::Assets::Blueprints::PalActionBar, Outer); }

        inline DynPals::WidgetBuilder OptionSlider(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(DynPals::UI::Assets::Blueprints::OptionSlider, Outer); }
        inline DynPals::WidgetBuilder OptionTab(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(DynPals::UI::Assets::Blueprints::OptionTab, Outer); }
        inline DynPals::WidgetBuilder OptionLR(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(DynPals::UI::Assets::Blueprints::OptionLR, Outer); }
        inline DynPals::WidgetBuilder OptionSwitch(RC::Unreal::UObject* Outer) { return DynPals::WidgetBuilder(DynPals::UI::Assets::Blueprints::OptionSwitch, Outer); }

        inline void SetFontData(RC::Unreal::UObject* Widget, int32_t Size, const FLinearColor_UE5& OutlineCol) {
            RC::Unreal::FProperty* FontProp = Utils::GetProperty(Widget, STR("Font"));
            if (FontProp) {
                void* FontPtr = FontProp->ContainerPtrToValuePtr<void>(Widget);
                if (FontPtr) {
                    RC::Unreal::FStructProperty* StructProp = static_cast<RC::Unreal::FStructProperty*>(FontProp);
                    if (StructProp && StructProp->GetStruct()) {
                        
                        RC::Unreal::FProperty* SizeProp = StructProp->GetStruct()->GetPropertyByNameInChain(STR("Size"));
                        if (SizeProp) {
                            int32_t* SizePtr = SizeProp->ContainerPtrToValuePtr<int32_t>(FontPtr);
                            if (SizePtr) *SizePtr = Size;
                        }
                    }
                }
                Utils::CallFunction(Widget, STR("SetFont"), FontPtr);
            }
        }

        inline void SetTextColor(RC::Unreal::UObject* Widget, const FLinearColor_UE5& Col) {
            FBuilderSlateColor SlateCol{Col, 0, {0,0,0}};
            struct { FBuilderSlateColor InColorAndOpacity; } Params{SlateCol};
            Utils::CallFunction(Widget, STR("SetColorAndOpacity"), &Params);
        }

        inline void SetImageColor(RC::Unreal::UObject* Widget, const FLinearColor_UE5& Col) {
            struct { FLinearColor_UE5 InColorAndOpacity; } Params{Col};
            Utils::CallFunction(Widget, STR("SetColorAndOpacity"), &Params);
        }
    }
}