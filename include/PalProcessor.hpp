#pragma once
#include <string>
#include <set>
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
        
        void ClearAllSwappedStatus() {
            SwappedInstances.clear();
        }

        void ClearSwappedStatus(const std::wstring& InstanceID) {
            SwappedInstances.erase(InstanceID);
        }

    private:
        PalProcessor() = default;
        PalProcessor(const PalProcessor&) = delete;
        PalProcessor& operator=(const PalProcessor&) = delete;

        void ApplySwap(RC::Unreal::UObject* Character, const SwapConfig& swap, PalPersistData& persist);

        std::set<std::wstring> SwappedInstances;
    };
}