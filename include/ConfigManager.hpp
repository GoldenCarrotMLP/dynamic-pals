#pragma once
#include <string>
#include <vector>
#include "json.hpp" // Updated to look for json.hpp inside include/
#include "DataTypes.hpp"

namespace DynPals {
    class ConfigManager {
    public:
        static ConfigManager& Get() {
            static ConfigManager instance;
            return instance;
        }

        void Initialize(const std::wstring& BasePath);
        void LoadConfigJSONs();
        int FindBestSwap(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName);

        const std::vector<SwapConfig>& GetConfigs() const { return Configs; }

    private:
        ConfigManager() = default;
        ConfigManager(const ConfigManager&) = delete;
        ConfigManager& operator=(const ConfigManager&) = delete;

        void ParseSwaps(const nlohmann::json& swapArray);

        std::wstring ConfigPath;
        std::vector<SwapConfig> Configs;
    };
}