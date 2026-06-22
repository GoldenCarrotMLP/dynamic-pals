#include "NotificationManager.hpp"
#include "Utils.hpp"

using namespace RC::Unreal;

namespace DynPals {

    // Helper wrapper to process both mapped parameters
    void EnqueueUIToast(const std::wstring& Message, uint8_t PriorityType, uint8_t ToneType) {
        NotificationManager::Get().EnqueueToast(
            Message, 
            static_cast<EPalLogPriority>(PriorityType), 
            static_cast<EPalLogContentToneType>(ToneType)
        );
    }

    void NotificationManager::EnqueueToast(const std::wstring& Message, EPalLogPriority Priority, EPalLogContentToneType Tone) {
        std::lock_guard<std::mutex> lock(ToastMutex);
        ToastQueue.push_back({ Message, Priority, Tone });
    }

    void NotificationManager::ProcessToasts(UObject* WorldContext) {
        std::lock_guard<std::mutex> lock(ToastMutex);
        if (ToastQueue.empty() || !WorldContext) return;

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        UObject* KTL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
        
        if (!PalUtil || !KTL) {
            ToastQueue.clear();
            return;
        }

        // Grab the local LogManager instance
        struct { UObject* WorldCtx; UObject* LogMgr; } GetLogParams{WorldContext, nullptr};
        UFunction* GetLogFunc = PalUtil->GetFunctionByNameInChain(STR("GetLogManager"));
        if (GetLogFunc) PalUtil->ProcessEvent(GetLogFunc, &GetLogParams);

        UObject* LogManager = GetLogParams.LogMgr;
        if (!LogManager) return;

        UFunction* AddLogFunc = LogManager->GetFunctionByNameInChain(STR("AddLog"));
        UFunction* ConvFunc = KTL->GetFunctionByNameInChain(STR("Conv_StringToText"));
        if (!AddLogFunc || !ConvFunc) return;

        // Process all pending UI toasts
        for (const auto& Toast : ToastQueue) {
            
            // Convert C++ String to Unreal FText
            struct { FString InStr; FText OutText; } ConvParams{ FString(Toast.Message.c_str()), FText() };
            KTL->ProcessEvent(ConvFunc, &ConvParams);

            // Setup the required additional data with our mapped Tone!
            FPalLogAdditionalData AddData{};
            AddData.LogToneType = static_cast<uint8_t>(Toast.Tone);
            AddData.DefaultFontStyleName = FName(); 
            AddData.OverrideWidgetClass = nullptr;

            // Package and Fire with our mapped Priority!
            FPalAddLogParams LogParams{};
            LogParams.Priority = static_cast<uint8_t>(Toast.Priority); 
            LogParams.Text = ConvParams.OutText;
            LogParams.AdditionalData = AddData;

            LogManager->ProcessEvent(AddLogFunc, &LogParams);
        }

        ToastQueue.clear();
    }
}