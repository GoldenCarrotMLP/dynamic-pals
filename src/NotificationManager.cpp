#include "NotificationManager.hpp"
#include "AsyncHelper.hpp"
#include "Utils.hpp"

using namespace RC::Unreal;

namespace DynPals {

    void EnqueueUIToast(const std::wstring& Message, uint8_t PriorityType, uint8_t ToneType) {
        NotificationManager::Get().EnqueueToast(
            Message, 
            static_cast<EPalLogPriority>(PriorityType), 
            static_cast<EPalLogContentToneType>(ToneType)
        );
    }

    void NotificationManager::EnqueueToast(const std::wstring& Message, EPalLogPriority Priority, EPalLogContentToneType Tone) {
        
        // Use our new native AsyncTask to push this immediately to the Game Thread!
        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Message, Priority, Tone]() {
            
            // We are now safely on the GameThread! We can find a WorldContext natively.
            std::vector<UObject*> worlds;
            UObjectGlobals::FindAllOf(STR("World"), worlds);
            if (worlds.empty()) return;

            UObject* WorldContext = worlds[0];

            UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            UObject* KTL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
            
            if (!PalUtil || !KTL) return;

            struct { UObject* WorldCtx; UObject* LogMgr; } GetLogParams{WorldContext, nullptr};
            UFunction* GetLogFunc = PalUtil->GetFunctionByNameInChain(STR("GetLogManager"));
            if (GetLogFunc) PalUtil->ProcessEvent(GetLogFunc, &GetLogParams);

            UObject* LogManager = GetLogParams.LogMgr;
            if (!LogManager) return;

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
        });
    }

    // We can delete the ProcessToasts function completely!
    void NotificationManager::ProcessToasts(UObject* WorldContext) {
        // Intentionally empty. No more queues!
    }
}