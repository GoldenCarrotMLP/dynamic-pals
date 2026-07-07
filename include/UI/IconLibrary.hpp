#pragma once

namespace DynPals::UI::Assets {

    namespace Blueprints {
        constexpr const wchar_t* CommonWindow       = L"/Game/Pal/Blueprint/UI/UserInterface/Common/WBP_PalCommonWindow.WBP_PalCommonWindow_C";
        constexpr const wchar_t* CommonButton       = L"/Game/Pal/Blueprint/UI/UserInterface/Common/WBP_CommonButton.WBP_CommonButton_C";
        constexpr const wchar_t* PalTextBlock       = L"/Game/Pal/Blueprint/UI/PalTextBlock/BP_PalTextBlock.BP_PalTextBlock_C";
        constexpr const wchar_t* LoadingIcon        = L"/Game/Pal/Blueprint/UI/UserInterface/Common/WBP_Common_LoadingIcon.WBP_Common_LoadingIcon_C";
        constexpr const wchar_t* ClipboardMsg       = L"/Game/Pal/Blueprint/UI/UserInterface/Common/WBP_Common_Menu_Msg_Small.WBP_Common_Menu_Msg_Small_C";
        constexpr const wchar_t* PalActionBar       = L"/Game/Pal/Blueprint/UI/CommonWidget/ActionBar/WBP_PalActionBar.WBP_PalActionBar_C";
        constexpr const wchar_t* PopupWindow        = L"/Game/Pal/Blueprint/UI/UserInterface/Common/WBP_CommonPopupWindow.WBP_CommonPopupWindow_C";

        // Native Options Blueprints
        constexpr const wchar_t* OptionSlider       = L"/Game/Pal/Blueprint/UI/UserInterface/MainMenu/Option/WBP_OptionSettings_ListContentSlider.WBP_OptionSettings_ListContentSlider_C";
        constexpr const wchar_t* OptionTab          = L"/Game/Pal/Blueprint/UI/UserInterface/MainMenu/Option/WBP_OptionSettings_TabButton.WBP_OptionSettings_TabButton_C";
        constexpr const wchar_t* OptionLR           = L"/Game/Pal/Blueprint/UI/UserInterface/MainMenu/Option/WBP_OptionSettings_ListContentLR.WBP_OptionSettings_ListContentLR_C";
        constexpr const wchar_t* OptionSwitch       = L"/Game/Pal/Blueprint/UI/UserInterface/MainMenu/Option/WBP_OptionSettings_ListContentSwitch.WBP_OptionSettings_ListContentSwitch_C";
    
        constexpr const wchar_t* CommonSelectList   = L"/Game/Pal/Blueprint/UI/UserInterface/Common/WBP_CommonSelectList.WBP_CommonSelectList_C";
    }
    

    namespace Fonts {
        constexpr const wchar_t* PalDefault         = L"/Game/Pal/Font/Ft_PalDefaultFont.Ft_PalDefaultFont";
    }

    namespace Common {
        constexpr const wchar_t* TalkNextArrow      = L"/Game/Pal/Texture/UI/Common/T_prt_talk_next_arrow";
        constexpr const wchar_t* LoadingCircle1     = L"/Game/Pal/Texture/UI/Common/T_icon_loading_1";
        constexpr const wchar_t* LoadingCircle2     = L"/Game/Pal/Texture/UI/Common/T_icon_loading_2";
        constexpr const wchar_t* Lock               = L"/Game/Pal/Texture/UI/Common/T_icon_pal_lock_0";
        constexpr const wchar_t* Invisible          = L"/Game/Pal/Texture/UI/Common/T_prt_Invisible";
        constexpr const wchar_t* NoticeMark         = L"/Game/Pal/Texture/UI/Common/T_prt_NoticeMark";
        constexpr const wchar_t* StatusArrow        = L"/Game/Pal/Texture/UI/Common/T_prt_status_arrow";
        constexpr const wchar_t* TalkBalloonArrow   = L"/Game/Pal/Texture/UI/Common/T_prt_TalkBalloon_Arrow";
    }

    namespace Elements {
        constexpr const wchar_t* Neutral            = L"/Game/Pal/Texture/UI/Main_Menu/T_Icon_element_00";
        constexpr const wchar_t* Water              = L"/Game/Pal/Texture/UI/Main_Menu/T_Icon_element_01";
        constexpr const wchar_t* Fire               = L"/Game/Pal/Texture/UI/Main_Menu/T_Icon_element_02";
        constexpr const wchar_t* Grass              = L"/Game/Pal/Texture/UI/Main_Menu/T_Icon_element_03";
        constexpr const wchar_t* Electric           = L"/Game/Pal/Texture/UI/Main_Menu/T_Icon_element_04";
        constexpr const wchar_t* Ice                = L"/Game/Pal/Texture/UI/Main_Menu/T_Icon_element_05";
        constexpr const wchar_t* Earth              = L"/Game/Pal/Texture/UI/Main_Menu/T_Icon_element_06";
        constexpr const wchar_t* Dark               = L"/Game/Pal/Texture/UI/Main_Menu/T_Icon_element_07";
        constexpr const wchar_t* Dragon             = L"/Game/Pal/Texture/UI/Main_Menu/T_Icon_element_08";
    }

    namespace Borders {
        constexpr const wchar_t* Frame1px           = L"/Game/Pal/Texture/UI/IngameMenu/T_prt_frame_1px";
        constexpr const wchar_t* Frame2px           = L"/Game/Pal/Texture/UI/InGame/T_prt_frame_2px";
        constexpr const wchar_t* FrameBracket0      = L"/Game/Pal/Texture/UI/IngameMenu/T_prt_frame_bracket_0";
        constexpr const wchar_t* FrameTB            = L"/Game/Pal/Texture/UI/InGame/T_prt_frame_tb";
        constexpr const wchar_t* Stripe             = L"/Game/Pal/Texture/UI/Main_Menu/T_tex_stripe";
        constexpr const wchar_t* WhiteSolid         = L"/Game/Pal/Texture/UI/Main_Menu/T_tex_white";
        constexpr const wchar_t* MenuBgGrdV         = L"/Game/Pal/Texture/UI/Main_Menu/T_prt_menu_bggrd_v";
    }
}