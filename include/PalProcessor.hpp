#pragma once
#include <string>
#include <vector>
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

    private:
        PalProcessor() = default;
        PalProcessor(const PalProcessor&) = delete;
        PalProcessor& operator=(const PalProcessor&) = delete;

        // Updated signature: Now accepts persistence reference for tracking morph states
        void ApplySwap(RC::Unreal::UObject* Character, const SwapConfig& swap, PalPersistData& persist);
    };
}