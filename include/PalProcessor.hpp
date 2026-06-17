#pragma once
#include <string>
#include <vector>
#include <set>
#include <chrono>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {
    class PalProcessor {
    public:
        static PalProcessor& Get() {
            static PalProcessor instance;
            return instance;
        }

        std::wstring StripCharacterPrefix(const std::wstring& InputID);
        void ProcessPal(RC::Unreal::UObject* Character, bool ForceReroll);
        void ForceSwap(RC::Unreal::UObject* Character, int SwapIndex);
        
        // The new Passive Scanner
        void ScanActivePals();

    private:
        PalProcessor() = default;
        PalProcessor(const PalProcessor&) = delete;
        PalProcessor& operator=(const PalProcessor&) = delete;

        void ApplySwap(RC::Unreal::UObject* Character, const SwapConfig& swap, PalPersistData& persist);

        // Memory tracking to prevent reapplying meshes to the same Pal infinitely
        std::set<RC::Unreal::UObject*> ProcessedPals;
        std::chrono::steady_clock::time_point LastScanTime = std::chrono::steady_clock::now();
    };
}