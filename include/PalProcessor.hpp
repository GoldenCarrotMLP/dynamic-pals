#pragma once
#include <string>
#include <vector>
#include <set>
#include <chrono>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {
    struct PendingSwap {
        RC::Unreal::UObject* Character;
        int SwapIndex;
        std::chrono::steady_clock::time_point ScheduledTime;
    };

    struct QueuedPal {
        RC::Unreal::UObject* Character;
        bool ForceReroll;
        int State; // 0 = Waiting in Pool, 1 = Teleported & Assembling
        std::chrono::steady_clock::time_point AssemblyEndTime;
    };

    class PalProcessor {
    public:
        static PalProcessor& Get() {
            static PalProcessor instance;
            return instance;
        }

        std::wstring StripCharacterPrefix(const std::wstring& InputID);
        void ProcessPal(RC::Unreal::UObject* Character, bool ForceReroll);
        
        void ForceSwap(RC::Unreal::UObject* Character, int SwapIndex, int DelayMs = 10);
        void ScanActivePals();
        void TickDeferredSwaps();

    private:
        PalProcessor() = default;
        PalProcessor(const PalProcessor&) = delete;
        PalProcessor& operator=(const PalProcessor&) = delete;

        void ApplySwap(RC::Unreal::UObject* Character, const SwapConfig& swap, PalPersistData& persist);
        void ExecuteSwap(RC::Unreal::UObject* Character, bool ForceReroll);

        std::set<RC::Unreal::UObject*> ProcessedPals;
        
        std::vector<PendingSwap> PendingSwaps; 
        std::vector<QueuedPal> ProcessingQueue; 
        
        std::chrono::steady_clock::time_point LastScanTime = std::chrono::steady_clock::now();
    };
}