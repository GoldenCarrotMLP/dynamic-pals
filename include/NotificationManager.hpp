#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {

    struct PendingToast {
        std::wstring Message;
        EPalLogPriority Priority;       // Added Priority
        EPalLogContentToneType Tone;    // Keep Tone
    };

    class NotificationManager {
    public:
        static NotificationManager& Get() {
            static NotificationManager instance;
            return instance;
        }

        // Now accepts both Priority and Tone
        void EnqueueToast(const std::wstring& Message, EPalLogPriority Priority = EPalLogPriority::Normal, EPalLogContentToneType Tone = EPalLogContentToneType::Normal);
        void ProcessToasts(RC::Unreal::UObject* WorldContext);

    private:
        NotificationManager() = default;
        NotificationManager(const NotificationManager&) = delete;
        NotificationManager& operator=(const NotificationManager&) = delete;

        std::mutex ToastMutex;
        std::vector<PendingToast> ToastQueue;
    };
}