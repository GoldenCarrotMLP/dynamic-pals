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

    class PalProcessor {
    public:
        static PalProcessor& Get() {
            static PalProcessor instance;
            return instance;
        }

        std::wstring StripCharacterPrefix(const std::wstring& InputID);
        void ProcessPal(RC::Unreal::UObject* Character, bool ForceReroll);
        
        // Updated signature to support custom delays
        void ForceSwap(RC::Unreal::UObject* Character, int SwapIndex, int DelayMs = 10);
        
        // The new Passive Scanner
        void ScanActivePals();
        
        // Added declaration to resolve C2039
        void TickDeferredSwaps();

    private:
        PalProcessor() = default;
        PalProcessor(const PalProcessor&) = delete;
        PalProcessor& operator=(const PalProcessor&) = delete;

        void ApplySwap(RC::Unreal::UObject* Character, const SwapConfig& swap, PalPersistData& persist);

        // Memory tracking to prevent reapplying meshes to the same Pal infinitely
        std::set<RC::Unreal::UObject*> ProcessedPals;
        
        // Added queue to hold deferred model swaps
        std::vector<PendingSwap> PendingSwaps;
        
        std::chrono::steady_clock::time_point LastScanTime = std::chrono::steady_clock::now();
    };
}