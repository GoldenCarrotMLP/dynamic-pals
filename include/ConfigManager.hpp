#pragma once
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <random>
#include "json.hpp"
#include "DataTypes.hpp"

namespace DynPals {
    class ConfigManager {
    public:
        static ConfigManager& Get() {
            static ConfigManager instance;
            return instance;
        }

        int FindConfigIndexBySwapID(const std::wstring& SwapID) const;

        bool IsCustomIconPath(const std::wstring& Path) const {
            if (Path.empty() || Path == L"None") return false;
            std::wstring lower = Path;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

            for (const auto& cfg : Configs) {
                if (!cfg.IconAssetPath.empty()) {
                    std::wstring cfgLower = cfg.IconAssetPath;
                    std::transform(cfgLower.begin(), cfgLower.end(), cfgLower.begin(), ::towlower);
                    if (lower.find(cfgLower) != std::wstring::npos || cfgLower.find(lower) != std::wstring::npos) {
                        return true;
                    }
                }
            }
            return false;
        }

        std::vector<int> GetConfigsForCharID(const std::wstring& CharID) const;
        void ParseSwaps(const std::wstring& PackName, const nlohmann::json& swapArray);
        void ParseSwapsV2(const std::wstring& PackName, const nlohmann::json& skinListObj);

        void Initialize(const std::wstring& BasePath);
        void LoadConfigJSONs();
        
        std::vector<SwapEvaluation> EvaluateAllSwaps(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName, int Rank, int Trust, bool IsWild, const std::wstring& CurrentSwapLabel) const; 
        int PickBestSwap(const std::vector<SwapEvaluation>& evaluations) const;
        int FindConfigIndex(const std::wstring& PackName, const std::wstring& SkinName, const std::wstring& SwapLabel, const std::wstring& SkelMeshPath, const std::wstring& CharID = L"") const;

        std::wstring GetIconAssetPathForSkin(const std::wstring& SkinOrSwapLabel) const;
        std::wstring GetIconAssetPathForSwap(const std::wstring& PackName, const std::wstring& SwapLabel) const;

        const std::vector<SwapConfig>& GetConfigs() const { return Configs; }

    private:
        ConfigManager() = default;
        ConfigManager(const ConfigManager&) = delete;
        ConfigManager& operator=(const ConfigManager&) = delete;

        std::wstring ConfigPath;
        std::vector<SwapConfig> Configs;
        mutable std::set<std::wstring> WarnedCharIDs; 
    };
}