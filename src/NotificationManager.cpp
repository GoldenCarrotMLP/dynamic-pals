#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

#include "NotificationManager.hpp"
#include "UI/UIBase.hpp"
#include "UI/WidgetBuilder.hpp"
#include "UI/Components/Button.hpp"
#include "AsyncHelper.hpp"
#include "Utils.hpp"
#include <Unreal/FText.hpp>
#include <Unreal/FString.hpp>

using namespace RC::Unreal;

namespace DynPals {

    static UObject* GetActiveLogManager() {
        std::vector<UObject*> worlds;
        UObjectGlobals::FindAllOf(STR("World"), worlds);
        if (worlds.empty()) return nullptr;

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (!PalUtil) return nullptr;

        UFunction* GetLogFunc = PalUtil->GetFunctionByNameInChain(STR("GetLogManager"));
        if (!GetLogFunc) return nullptr;

        for (UObject* World : worlds) {
            if (!World) continue;

            UClass* Class = World->GetClassPrivate();
            if (Class && Class->GetName() == L"World" && World->GetName().find(L"Default__") == std::wstring::npos) {
                struct { UObject* WorldCtx; UObject* LogMgr; } GetLogParams{World, nullptr};
                PalUtil->ProcessEvent(GetLogFunc, &GetLogParams);
                
                if (GetLogParams.LogMgr) {
                    return GetLogParams.LogMgr;
                }
            }
        }
        return nullptr;
    }

    static void ShowToastDirectWithLogManager(UObject* LogManager, const std::wstring& Message, EPalLogPriority Priority, EPalLogContentToneType Tone) {
        UObject* KTL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
        if (!KTL) return;

        UFunction* AddLogFunc = LogManager->GetFunctionByNameInChain(STR("AddLog"));
        UFunction* ConvFunc = KTL->GetFunctionByNameInChain(STR("Conv_StringToText"));
        if (!AddLogFunc || !ConvFunc) return;

        struct { FString InStr; FText OutText; } ConvParams{ FString(Message.c_str()), FText() };
        KTL->ProcessEvent(ConvFunc, &ConvParams);

        FPalLogAdditionalData AddData{};
        AddData.LogToneType = static_cast<uint8_t>(Tone);
        AddData.DefaultFontStyleName = FName(); 
        AddData.OverrideWidgetClass = nullptr;

        FPalAddLogParams LogParams{};
        LogParams.Priority = static_cast<uint8_t>(Priority); 
        LogParams.Text = ConvParams.OutText;
        LogParams.AdditionalData = AddData;

        LogManager->ProcessEvent(AddLogFunc, &LogParams);
    }

    // Helper: Sets text directly on WBP_CommonButton's internal Text_Main component
    static void SetCommonButtonText(UObject* CommonButtonObj, const std::wstring& TextStr) {
        if (!CommonButtonObj || !Utils::IsObjectValid(CommonButtonObj)) return;

        UObject* KTL = Utils::GetKTL();
        UFunction* ConvFunc = Utils::GetKTLFunction(STR("Conv_StringToText"));
        if (!KTL || !ConvFunc) return;

        struct { FString InStr; FText OutText; } ConvParams{ FString(TextStr.c_str()), FText() };
        KTL->ProcessEvent(ConvFunc, &ConvParams);

        // Target the inner Text_Main text block proved by FModel JSON dump!
        UObject* TextMainObj = nullptr;
        if (Utils::GetPropertyValue<UObject*>(CommonButtonObj, STR("Text_Main"), TextMainObj, true) && TextMainObj) {
            struct { FText InText; } TextParams{ ConvParams.OutText };
            Utils::CallFunction(TextMainObj, STR("SetText"), &TextParams);
        } else {
            struct { FText InText; } TextParams{ ConvParams.OutText };
            Utils::CallFunction(CommonButtonObj, STR("SetText"), &TextParams);
        }
    }

    void EnqueueUIToast(const std::wstring& Message, uint8_t PriorityType, uint8_t ToneType) {
        NotificationManager::Get().EnqueueToast(
            Message, 
            static_cast<EPalLogPriority>(PriorityType), 
            static_cast<EPalLogContentToneType>(ToneType)
        );
    }

    void NotificationManager::EnqueueToast(const std::wstring& Message, EPalLogPriority Priority, EPalLogContentToneType Tone) {
        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [this, Message, Priority, Tone]() {
            if (!bIsReadyForToasts) {
                std::lock_guard<std::mutex> lock(ToastMutex);
                ToastQueue.push_back({Message, Priority, Tone});
                return;
            }

            UObject* LogManager = GetActiveLogManager();
            if (LogManager) {
                ShowToastDirectWithLogManager(LogManager, Message, Priority, Tone);
            }
        });
    }

    void NotificationManager::FlushQueuedToasts() {
        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [this]() {
            UObject* LogManager = GetActiveLogManager();
            if (!LogManager) {
                DP_LOG(Verbose, "Failed to flush queued toasts: LogManager is still null.\n");
                return;
            }

            std::vector<PendingToast> ToastsToFlush;
            {
                std::lock_guard<std::mutex> lock(ToastMutex);
                bIsReadyForToasts = true;
                ToastsToFlush = std::move(ToastQueue);
                ToastQueue.clear();
            }

            for (const auto& toast : ToastsToFlush) {
                ShowToastDirectWithLogManager(LogManager, toast.Message, toast.Priority, toast.Tone);
            }

            DP_LOG(Verbose, "Dispatched {} queued messages.", ToastsToFlush.size());
        });
    }

    void NotificationManager::ClearInGameLogs() {
        std::vector<UObject*> logWidgets;
        UObjectGlobals::FindAllOf(STR("WBP_PalLogWidget_C"), logWidgets);

        for (UObject* LogWidget : logWidgets) {
            if (!LogWidget || !Utils::IsObjectValid(LogWidget)) continue;

            UObject* NormalScrollBox = nullptr;
            if (Utils::GetPropertyValue<UObject*>(LogWidget, STR("ScrollBox_NormalLog"), NormalScrollBox) && NormalScrollBox) {
                Utils::CallFunction(NormalScrollBox, STR("ClearChildren"));
            }

            UObject* ImportantBorder = nullptr;
            if (Utils::GetPropertyValue<UObject*>(LogWidget, STR("ImportantBorder"), ImportantBorder) && ImportantBorder) {
                Utils::CallFunction(ImportantBorder, STR("ClearChildren"));
            }

            UObject* VeryImportantBorder = nullptr;
            if (Utils::GetPropertyValue<UObject*>(LogWidget, STR("VeryImportantBorder"), VeryImportantBorder) && VeryImportantBorder) {
                Utils::CallFunction(VeryImportantBorder, STR("ClearChildren"));
            }

            FProperty* NormalListProp = Utils::GetProperty(LogWidget, STR("NormalLogList"));
            if (NormalListProp) {
                TArray<UObject*>* NormalList = NormalListProp->ContainerPtrToValuePtr<TArray<UObject*>>(LogWidget);
                if (NormalList) NormalList->Empty();
            }

            FProperty* ImportantListProp = Utils::GetProperty(LogWidget, STR("ImportantLogList"));
            if (ImportantListProp) {
                TArray<UObject*>* ImportantList = ImportantListProp->ContainerPtrToValuePtr<TArray<UObject*>>(LogWidget);
                if (ImportantList) ImportantList->Empty();
            }

            FProperty* VeryImpIDArrayProp = Utils::GetProperty(LogWidget, STR("veryImportantLogIDArray"));
            if (VeryImpIDArrayProp) {
                TArray<DynPalsGuid>* VeryImpIDArray = VeryImpIDArrayProp->ContainerPtrToValuePtr<TArray<DynPalsGuid>>(LogWidget);
                if (VeryImpIDArray) VeryImpIDArray->Empty();
            }
        }
    }

    void NotificationManager::ShowModalDialog(const std::wstring& Message) {
        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Message]() {
            UObject* PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
            if (!PlayerController || !Utils::IsObjectValid(PlayerController)) return;

            UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            UObject* KTL = Utils::GetKTL();
            if (!PalUtil || !KTL) return;

            UFunction* AlertFunc = PalUtil->GetFunctionByNameInChain(STR("Alert"));
            UFunction* ConvFunc = KTL->GetFunctionByNameInChain(STR("Conv_StringToText"));
            if (!AlertFunc || !ConvFunc) return;

            struct { FString InStr; FText OutText; } ConvParams{ FString(Message.c_str()), FText() };
            KTL->ProcessEvent(ConvFunc, &ConvParams);

            struct { UObject* WorldContext; FText Message; } AlertParams{ PlayerController, ConvParams.OutText };
            PalUtil->ProcessEvent(AlertFunc, &AlertParams);

            DP_LOG(Default, "[NotificationManager] Displayed native modal alert dialog.");
        });
    }

    // 2-Button Hijacked Modal Controller
    class TwoButtonModalUI : public UIBase {
    public:
        TwoButtonModalUI(
            const std::wstring& InMessage,
            const std::wstring& InRightText, std::function<void()> InRightClick,
            const std::wstring& InLeftText, std::function<void()> InLeftClick
        ) : Message(InMessage), 
            RightText(InRightText), OnRightClick(InRightClick),
            LeftText(InLeftText), OnLeftClick(InLeftClick)
        {
            bCloseOnEscape = true;
            bRequiresInputLock = true;
        }

    protected:
        virtual void BuildWidget() override {
            if (!CurrentPlayerController) return;

            UClass* DialogClass = Utils::GetClassCached(STR("/Game/Pal/Blueprint/UI/Dialog/WBP_PalDialog.WBP_PalDialog_C"));
            if (!DialogClass) return;

            UObject* WBL = Utils::GetWBL();
            UFunction* CreateFunc = Utils::GetWBLFunction(STR("Create"));
            if (!WBL || !CreateFunc) return;

            struct { UObject* WorldContext; UClass* WidgetType; UObject* OwningPlayer; UObject* ReturnValue; } CreateParams{
                CurrentPlayerController, DialogClass, CurrentPlayerController, nullptr
            };
            WBL->ProcessEvent(CreateFunc, &CreateParams);
            MyWidget = CreateParams.ReturnValue;
            if (!MyWidget) return;

            // Add Background Blur to CanvasPanel_0
            UObject* CanvasRoot = nullptr;
            if (Utils::GetPropertyValue<UObject*>(MyWidget, STR("CanvasPanel_0"), CanvasRoot) && CanvasRoot) {
                UClass* BlurClass = Utils::GetClassCached(STR("/Script/UMG.BackgroundBlur"));
                if (BlurClass) {
                    FStaticConstructObjectParameters BlurParams{BlurClass, MyWidget};
                    BlurParams.Name = FName();
                    UObject* BlurWidget = UObjectGlobals::StaticConstructObject(BlurParams);
                    if (BlurWidget) {
                        Utils::SetPropertyValue<float>(BlurWidget, STR("BlurStrength"), 4.0f);
                        struct { UObject* Content; UObject* ReturnValue; } AddParams{BlurWidget, nullptr};
                        Utils::CallFunction(CanvasRoot, STR("AddChild"), &AddParams);
                        if (AddParams.ReturnValue) {
                            struct { int32_t ZOrder; } ZParams{-1};
                            Utils::CallFunction(AddParams.ReturnValue, STR("SetZOrder"), &ZParams);
                        }
                    }
                }
            }

            // Call SetupUI with DialogType = 1 (YesNo mode - forces 2 native buttons)
            UObject* KTL = Utils::GetKTL();
            UFunction* ConvFunc = Utils::GetKTLFunction(STR("Conv_StringToText"));
            if (KTL && ConvFunc) {
                struct { FString InStr; FText OutText; } ConvParams{ FString(Message.c_str()), FText() };
                KTL->ProcessEvent(ConvFunc, &ConvParams);

                struct { uint8_t DialogType; uint8_t Pad[7]; FText Msg; } SetupParams{ 1, {0}, ConvParams.OutText };
                Utils::CallFunction(MyWidget, STR("SetupUI"), &SetupParams);
            }

            // Fetch inner WBP_CommonPopupWindow
            UObject* PopupWindow = nullptr;
            Utils::GetPropertyValue<UObject*>(MyWidget, STR("WBP_CommonPopupWindow"), PopupWindow);

            if (PopupWindow) {
                // Exact Property Names from FModel Dump: WBP_CommonButton_L and WBP_CommonButton_R
                UObject* LeftBtnObj = nullptr;
                UObject* RightBtnObj = nullptr;

                Utils::GetPropertyValue<UObject*>(PopupWindow, STR("WBP_CommonButton_L"), LeftBtnObj, true);
                Utils::GetPropertyValue<UObject*>(PopupWindow, STR("WBP_CommonButton_R"), RightBtnObj, true);

                // Fallback via functions
                if (!LeftBtnObj) {
                    struct { UObject* ReturnValue; } LeftRes{ nullptr };
                    Utils::CallFunction(PopupWindow, STR("GetLeftButton"), &LeftRes);
                    LeftBtnObj = LeftRes.ReturnValue;
                }
                if (!RightBtnObj) {
                    struct { UObject* ReturnValue; } RightRes{ nullptr };
                    Utils::CallFunction(PopupWindow, STR("GetRightButton"), &RightRes);
                    RightBtnObj = RightRes.ReturnValue;
                }

                // Hijack Right Button ("Yes" / Confirm)
                if (RightBtnObj) {
                    SetCommonButtonText(RightBtnObj, RightText);
                    
                    RightBtnCtrl = std::make_unique<UI::Button>(RightBtnObj);
                    auto action = OnRightClick;
                    RightBtnCtrl->OnClicked([this, action]() {
                        if (action) action();
                        RequestToggle();
                    });
                }

                // Hijack Left Button ("No" / Cancel)
                if (LeftBtnObj) {
                    SetCommonButtonText(LeftBtnObj, LeftText);

                    LeftBtnCtrl = std::make_unique<UI::Button>(LeftBtnObj);
                    auto action = OnLeftClick;
                    LeftBtnCtrl->OnClicked([this, action]() {
                        if (action) action();
                        RequestToggle();
                    });
                }
            }

            struct { int32_t ZOrder; } ViewportParams{99999};
            Utils::CallFunction(MyWidget, STR("AddToViewport"), &ViewportParams);
        }

        virtual void OnTickUI() override {
            if (RightBtnCtrl) RightBtnCtrl->Tick();
            if (LeftBtnCtrl) LeftBtnCtrl->Tick();
        }

    private:
        std::wstring Message;
        std::wstring RightText;
        std::function<void()> OnRightClick;
        std::wstring LeftText;
        std::function<void()> OnLeftClick;

        std::unique_ptr<UI::Button> RightBtnCtrl;
        std::unique_ptr<UI::Button> LeftBtnCtrl;
    };

    void NotificationManager::ShowTwoButtonModal(
        const std::wstring& Message,
        const std::wstring& RightBtnText, std::function<void()> OnRightClick,
        const std::wstring& LeftBtnText, std::function<void()> OnLeftClick
    ) {
        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Message, RightBtnText, OnRightClick, LeftBtnText, OnLeftClick]() {
            auto Modal = new TwoButtonModalUI(Message, RightBtnText, OnRightClick, LeftBtnText, OnLeftClick);
            Modal->RequestToggle();
        });
    }
}