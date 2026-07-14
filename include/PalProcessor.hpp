#pragma once
#include <string>
#include <vector>
#include <set>
#include <map>
#include <chrono>
#include <deque>
#include <mutex>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {

    struct QueuedSwap {
        RC::Unreal::UObject* Character;
        bool ForceReroll;
        int ExplicitSwapIndex;
        bool IsCompanionSync;
    };

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
        void ProcessPal(RC::Unreal::UObject* Character, bool ForceReroll, int ExplicitSwapIndex = -1, bool IsCompanionSync = false);
        void CheckAndTriggerUpdate(RC::Unreal::UObject* Character);

        void ForceSwap(RC::Unreal::UObject* Character, int SwapIndex, int DelayMs = 10);
        void DelayedSwap(RC::Unreal::UObject* Character, int SwapIndex, const std::wstring& CompName);
        void DelayedReroll(RC::Unreal::UObject* Character, const std::wstring& CompName);
        void ScanActivePals();
        void ProcessPlayerParty(RC::Unreal::UObject* WorldContext);
        void Tick();

        void ClearAllSwappedStatus();
        void ClearSwappedStatus(const std::wstring& InstanceID, RC::Unreal::UObject* Character);

    private:
        PalProcessor() = default;
        PalProcessor(const PalProcessor&) = delete;
        PalProcessor& operator=(const PalProcessor&) = delete;

        int EvaluateIdealSwapIndex(RC::Unreal::UObject* Character, std::wstring& OutInstanceID);
        void ApplySwap(RC::Unreal::UObject* Character, const SwapConfig& swap, PalPersistData& persist);
        
        bool ExecuteSwap(RC::Unreal::UObject* Character, bool ForceReroll, int ExplicitSwapIndex = -1, bool IsCompanionSync = false);

        std::map<RC::Unreal::UObject*, std::wstring> SwappedInstances;

        std::map<std::wstring, std::set<RC::Unreal::UObject*>> ActivePalsByInstanceID;
        std::map<std::wstring, PalRuntimeStats> RuntimeStatsCache;


        std::set<RC::Unreal::UObject*> ProcessedPals; 
        std::vector<QueuedPal> ProcessingQueue; 
        std::chrono::steady_clock::time_point LastScanTime = std::chrono::steady_clock::now();

        std::deque<QueuedSwap> SwapQueue;
        std::mutex QueueMutex;
    };
}