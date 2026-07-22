#include "NotificationManager.hpp"
#include "AsyncHelper.hpp"
#include "Utils.hpp"

using namespace RC::Unreal;

namespace DynPals {

    // Safely fetches the active native LogManager subsystem by scanning all active World instances
    static UObject* GetActiveLogManager() {
        std::vector<UObject*> worlds;
        UObjectGlobals::FindAllOf(STR("World"), worlds);
        if (worlds.empty()) return nullptr;

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (!PalUtil) return nullptr;

        UFunction* GetLogFunc = PalUtil->GetFunctionByNameInChain(STR("GetLogManager"));
        if (!GetLogFunc) return nullptr;

        // Iterate through all loaded World instances to find the live, active gameplay world
        for (UObject* World : worlds) {
            if (!World) continue;

            UClass* Class = World->GetClassPrivate();
            if (Class && Class->GetName() == L"World" && World->GetName().find(L"Default__") == std::wstring::npos) {
                struct { UObject* WorldCtx; UObject* LogMgr; } GetLogParams{World, nullptr};
                PalUtil->ProcessEvent(GetLogFunc, &GetLogParams);
                
                if (GetLogParams.LogMgr) {
                    return GetLogParams.LogMgr; // Found the active live game World!
                }
            }
        }
        return nullptr;
    }

    // Direct caller utilizing an already verified active LogManager
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

    void EnqueueUIToast(const std::wstring& Message, uint8_t PriorityType, uint8_t ToneType) {
        NotificationManager::Get().EnqueueToast(
            Message, 
            static_cast<EPalLogPriority>(PriorityType), 
            static_cast<EPalLogContentToneType>(ToneType)
        );
    }

    void NotificationManager::EnqueueToast(const std::wstring& Message, EPalLogPriority Priority, EPalLogContentToneType Tone) {
        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [this, Message, Priority, Tone]() {
            // STRICT CHECK: Are we fully loaded in the overworld?
            if (!bIsReadyForToasts) {
                std::lock_guard<std::mutex> lock(ToastMutex);
                ToastQueue.push_back({Message, Priority, Tone});
                return;
            }

            // We are fully loaded, safely grab the log manager and print instantly
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
                bIsReadyForToasts = true; // UNLOCK the queue! We are officially in-game.
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
    // 1. Force-remove ALL active log toast widgets (Normal, Important, Error) from screen directly
    std::vector<UObject*> logItems;
    UObjectGlobals::FindAllOf(STR("PalLogWidgetBase"), logItems);
    for (UObject* Item : logItems) {
        if (Item && Utils::IsObjectValid(Item)) {
            Utils::CallFunction(Item, STR("RemoveFromParent"));
        }
    }

    // 2. Clear WBP_PalLogWidget_C UI containers and tracking arrays
    std::vector<UObject*> logWidgets;
    UObjectGlobals::FindAllOf(STR("WBP_PalLogWidget_C"), logWidgets);

    for (UObject* LogWidget : logWidgets) {
        if (!LogWidget || !Utils::IsObjectValid(LogWidget)) continue;

        // A. Clear visual UI containers for Normal, Important, and Very Important
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

        // B. Empty internal tracking arrays on WBP_PalLogWidget
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
}