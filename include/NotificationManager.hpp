#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {

    struct PendingToast {
        std::wstring Message;
        EPalLogPriority Priority;
        EPalLogContentToneType Tone;
    };

    class NotificationManager {
    public:
        static NotificationManager& Get() {
            static NotificationManager instance;
            return instance;
        }

        void EnqueueToast(const std::wstring& Message, EPalLogPriority Priority = EPalLogPriority::Normal, EPalLogContentToneType Tone = EPalLogContentToneType::Normal);
        
        // Flushes startup/menu warnings once in-game
        void FlushQueuedToasts(); 

        // Instantly clears active on-screen logs from WBP_PalLogWidget
        void ClearInGameLogs();

        // Controls whether toasts queue up or fire immediately
        void SetReady(bool bReady) { bIsReadyForToasts = bReady; }

    private:
        NotificationManager() = default;
        NotificationManager(const NotificationManager&) = delete;
        NotificationManager& operator=(const NotificationManager&) = delete;

        std::mutex ToastMutex;
        std::vector<PendingToast> ToastQueue;
        bool bIsReadyForToasts = false; // Locked to false by default!
    };
}