#pragma once
#include <string>
#include <vector>
#include <set>
#include <map>
#include <chrono>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {

    struct QueuedPal {
        RC::Unreal::UObject* Character;
        bool ForceReroll;
        int State; 
        std::chrono::steady_clock::time_point AssemblyEndTime;
    };

    struct PalRuntimeStats {
        int Level = -1;
        int Rank = -1;
        int Friendship = -1;
    };

    class PalProcessor {
    public:
        static PalProcessor& Get() {
            static PalProcessor instance;
            return instance;
        }

        std::wstring StripCharacterPrefix(const std::wstring& InputID);
        void ProcessPal(RC::Unreal::UObject* Character, bool ForceReroll);
        void CheckAndTriggerUpdate(RC::Unreal::UObject* Character);
        void ForceSwap(RC::Unreal::UObject* Character, int SwapIndex, int DelayMs = 10);
        
        void ScanActivePals();

        void ClearAllSwappedStatus();
        void ClearSwappedStatus(const std::wstring& InstanceID);

    private:
        PalProcessor() = default;
        PalProcessor(const PalProcessor&) = delete;
        PalProcessor& operator=(const PalProcessor&) = delete;

        int EvaluateIdealSwapIndex(RC::Unreal::UObject* Character, std::wstring& OutInstanceID);
        void ApplySwap(RC::Unreal::UObject* Character, const SwapConfig& swap, PalPersistData& persist);
        
        void ExecuteSwap(RC::Unreal::UObject* Character, bool ForceReroll, int ExplicitSwapIndex = -1);

        std::map<std::wstring, RC::Unreal::UObject*> SwappedInstances;
        std::map<std::wstring, PalRuntimeStats> RuntimeStatsCache;

        std::set<RC::Unreal::UObject*> ProcessedPals; 
        std::vector<QueuedPal> ProcessingQueue; 
        std::chrono::steady_clock::time_point LastScanTime = std::chrono::steady_clock::now();
    };
}