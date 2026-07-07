// --- START OF FILE include/UI/Components/WindowFrame.hpp ---
#pragma once
#include <functional>
#include <Unreal/UObjectGlobals.hpp>
#include "UI/WidgetBuilder.hpp" // <-- FIXED INCLUDE PATH
#include "Utils.hpp"

namespace DynPals::UI {

    class WindowFrame {
    public:
        WindowFrame(RC::Unreal::UObject* Outer, float DefaultWidth = 600.0f) 
            : MyOuter(Outer), Width(DefaultWidth), 
              FrameSizeBox(DynPals::UI::SizeBox(Outer)), 
              ContentBox(DynPals::UI::VerticalBox(Outer)) 
        {
            FrameSizeBox.WidthOverride(Width);
            FrameSizeBox.AddChild(ContentBox);
        }

        WindowFrame& SetHeader(const DynPals::WidgetBuilder& Header) {
            ContentBox.AddToVerticalBox(Header, [](DynPals::BoxSlotBuilder& Slot) {
                Slot.Padding(0.0f, 0.0f, 0.0f, 24.0f);
            });
            return *this;
        }

        WindowFrame& AddAutoScaledRow(const DynPals::WidgetBuilder& Row, float PaddingBottom = 15.0f) {
            auto ScaledRow = DynPals::UI::ScaleBox(MyOuter).Stretch(DynPals::EBuilderStretch::ScaleToFit).AddChild(Row);
            ContentBox.AddToVerticalBox(ScaledRow, [PaddingBottom](DynPals::BoxSlotBuilder& Slot) {
                Slot.Padding(0.0f, 0.0f, 0.0f, PaddingBottom);
            });
            return *this;
        }

        WindowFrame& AddContent(const DynPals::WidgetBuilder& Content, float PaddingBottom = 24.0f) {
            ContentBox.AddToVerticalBox(Content, [PaddingBottom](DynPals::BoxSlotBuilder& Slot) {
                Slot.Padding(20.0f, 0.0f, 20.0f, PaddingBottom);
            });
            return *this;
        }

        WindowFrame& SetFooter(const DynPals::WidgetBuilder& Footer) {
            ContentBox.AddToVerticalBox(Footer, [](DynPals::BoxSlotBuilder& Slot) {
                Slot.Size(DynPals::EBuilderSlateSizeRule::Automatic)
                    .HorizontalAlignment(DynPals::EBuilderHorizontalAlignment::HAlign_Fill)
                    .VerticalAlignment(DynPals::EBuilderVerticalAlignment::VAlign_Bottom);
            });
            return *this;
        }

        RC::Unreal::UObject* Build(double AnchorMinX = 0.5, double AnchorMinY = 0.5, double AnchorMaxX = 0.5, double AnchorMaxY = 0.5, double AlignX = 0.5, double AlignY = 0.5) {
            auto RootCanvas = DynPals::UI::Canvas(MyOuter)
                .AddToCanvas(
                    DynPals::UI::Window(MyOuter).PackWindowContent(FrameSizeBox),
                    [=](DynPals::CanvasSlotBuilder& Slot) {
                        Slot.Anchors(AnchorMinX, AnchorMinY, AnchorMaxX, AnchorMaxY)
                            .Alignment(AlignX, AlignY)
                            .AutoSize(true);
                    }
                );
            return RootCanvas.Build();
        }

    private:
        RC::Unreal::UObject* MyOuter;
        float Width;
        DynPals::WidgetBuilder FrameSizeBox;
        DynPals::WidgetBuilder ContentBox;
    };
}
// --- END OF FILE include/UI/Components/WindowFrame.hpp ---