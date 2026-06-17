#pragma once
#include <string>
#include <vector>
#include "json.hpp"
#include "DataTypes.hpp"

namespace DynPals {
    class ConfigManager {
    public:
        static ConfigManager& Get() {
            static ConfigManager instance;
            return instance;
        }

        std::vector<int> GetConfigsForCharID(const std::wstring& CharID) const;
        void ParseSwaps(const std::wstring& PackName, const nlohmann::json& swapArray);

        void Initialize(const std::wstring& BasePath);
        void LoadConfigJSONs();
        int FindBestSwap(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName, int Rank, int Trust);

        const std::vector<SwapConfig>& GetConfigs() const { return Configs; }

    private:
        ConfigManager() = default;
        ConfigManager(const ConfigManager&) = delete;
        ConfigManager& operator=(const ConfigManager&) = delete;

        std::wstring ConfigPath;
        std::vector<SwapConfig> Configs;
    };
}