#include "UIParser.hpp"

using namespace RC::Unreal;

namespace DynPals {

    // Internal structs matching your UIManager
    struct FAnchors { double MinimumX; double MinimumY; double MaximumX; double MaximumY; }; 
    struct FMargin { float Left; float Top; float Right; float Bottom; };
    
    struct FSlateColor_UE5 {
        FLinearColor_UE5 SpecifiedColor;
        uint8_t ColorUseRule; 
        uint8_t Pad[3];       
    };

    UObject* UIParser::Parse(const nlohmann::json& layoutJson, UObject* OuterContext, UObject* PlayerContext) {
        ElementsByID.clear();
        KismetTextLibrary = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
        
        return BuildNodeRecursive(layoutJson, OuterContext, OuterContext);
    }

    UClass* UIParser::GetUMGClass(const std::string& ClassName) {
        if (ClassCache.find(ClassName) != ClassCache.end()) return ClassCache[ClassName];
        
        std::wstring Path = L"/Script/UMG." + Utils::StringToWString(ClassName);
        UClass* Cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, Path.c_str());
        ClassCache[ClassName] = Cls;
        return Cls;
    }

    UObject* UIParser::BuildNodeRecursive(const nlohmann::json& node, UObject* ParentWidget, UObject* OuterContext) {
        if (!node.contains("Type")) return nullptr;

        std::string TypeName = node["Type"].get<std::string>();
        UClass* WidgetClass = GetUMGClass(TypeName);
        if (!WidgetClass) {
            DP_LOG(Warning, "[UIParser] Could not find UMG class: {}", Utils::StringToWString(TypeName));
            return nullptr;
        }

        // 1. Construct Widget
        FStaticConstructObjectParameters Params{WidgetClass, OuterContext};
        Params.Name = FName(); 
        UObject* NewWidget = UObjectGlobals::StaticConstructObject(Params);

        // 2. Cache by ID if present
        if (node.contains("ID")) {
            ElementsByID[node["ID"].get<std::string>()] = NewWidget;
        }

        // 3. Apply Styling
        if (node.contains("Style")) {
            ApplyStyle(NewWidget, node["Style"]);
        }

        // 4. Handle Children
        if (node.contains("Children") && node["Children"].is_array()) {
            for (const auto& childNode : node["Children"]) {
                UObject* ChildWidget = BuildNodeRecursive(childNode, NewWidget, OuterContext);
                
                if (ChildWidget) {
                    struct { UObject* Content; UObject* ReturnValue; } AddParams{ChildWidget, nullptr};
                    Utils::CallFunction(NewWidget, STR("AddChild"), &AddParams);
                    UObject* Slot = AddParams.ReturnValue;

                    // If it was added to a CanvasPanel, apply Slot rules (Anchors/Offsets)
                    if (Slot && childNode.contains("Slot")) {
                        ApplySlotProperties(Slot, childNode["Slot"]);
                    }
                }
            }
        }

        return NewWidget;
    }

    void UIParser::ApplySlotProperties(UObject* Slot, const nlohmann::json& slotNode) {
        if (!Slot) return;

        if (slotNode.contains("Anchors")) {
            auto& a = slotNode["Anchors"];
            FAnchors Anchors{ a.value("MinX", 0.0), a.value("MinY", 0.0), a.value("MaxX", 1.0), a.value("MaxY", 1.0) };
            struct { FAnchors InAnchors; } AnchorsParams{Anchors};
            Utils::CallFunction(Slot, STR("SetAnchors"), &AnchorsParams);
        }
        if (slotNode.contains("Offsets")) {
            auto& o = slotNode["Offsets"];
            FMargin Offsets{ o.value("Left", 0.0f), o.value("Top", 0.0f), o.value("Right", 0.0f), o.value("Bottom", 0.0f) };
            struct { FMargin InOffsets; } OffsetsParams{Offsets};
            Utils::CallFunction(Slot, STR("SetOffsets"), &OffsetsParams);
        }
    }

    void UIParser::ApplyStyle(UObject* Widget, const nlohmann::json& styleNode) {
        std::wstring className = Widget->GetClassPrivate()->GetName();

        if (styleNode.contains("Text") && KismetTextLibrary) {
            UFunction* ConvStringFunc = KismetTextLibrary->GetFunctionByNameInChain(STR("Conv_StringToText"));
            struct { FString InString; FText ReturnValue; } Params{ FString(Utils::StringToWString(styleNode["Text"].get<std::string>()).c_str()), FText() };
            KismetTextLibrary->ProcessEvent(ConvStringFunc, &Params);

            struct { FText InText; } SetTextParams{Params.ReturnValue};
            Utils::CallFunction(Widget, STR("SetText"), &SetTextParams);
        }

        if (styleNode.contains("Color")) {
            auto arr = styleNode["Color"];
            FLinearColor_UE5 color = { arr[0], arr[1], arr[2], arr.size() > 3 ? arr[3].get<float>() : 1.0f };
            struct { FSlateColor_UE5 InColorAndOpacity; } Params{ {color, 0, {0,0,0}} };
            
            if (className.find(L"TextBlock") != std::wstring::npos) {
                Utils::CallFunction(Widget, STR("SetColorAndOpacity"), &Params);
            }
        }

        if (styleNode.contains("BackgroundColor")) {
            auto arr = styleNode["BackgroundColor"];
            FLinearColor_UE5 color = { arr[0], arr[1], arr[2], arr.size() > 3 ? arr[3].get<float>() : 1.0f };
            struct { FLinearColor_UE5 InColor; } ColorParams{color};
            Utils::CallFunction(Widget, STR("SetBackgroundColor"), &ColorParams);
        }

        if (styleNode.contains("BrushColor")) {
            auto arr = styleNode["BrushColor"];
            FLinearColor_UE5 color = { arr[0], arr[1], arr[2], arr.size() > 3 ? arr[3].get<float>() : 1.0f };
            struct { FLinearColor_UE5 InColor; } BrushParams{color};
            Utils::CallFunction(Widget, STR("SetBrushColor"), &BrushParams);
        }

        if (styleNode.contains("Padding")) {
            auto& p = styleNode["Padding"];
            FMargin Pad{ p.value("Left", 0.0f), p.value("Top", 0.0f), p.value("Right", 0.0f), p.value("Bottom", 0.0f) };
            struct { FMargin InPadding; } PaddingParams{Pad};
            Utils::CallFunction(Widget, STR("SetPadding"), &PaddingParams);
        }

        if (styleNode.contains("SizeY")) {
            struct FVector2D { double X, Y; };
            struct { FVector2D InSize; } SizeParams{{1.0, styleNode["SizeY"].get<double>()}};
            Utils::CallFunction(Widget, STR("SetSize"), &SizeParams);
        }
    }
}