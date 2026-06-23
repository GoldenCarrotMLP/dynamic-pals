#include "ConfigManager.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include "json.hpp"
#include <random>
#include <algorithm>
#include <filesystem> // <--- ADD THIS INCLUDE

using namespace RC;
using namespace RC::Unreal;
namespace fs = std::filesystem;

namespace DynPals {

    namespace {
        std::string ToLower(std::string str) {
            std::transform(str.begin(), str.end(), str.begin(), ::tolower);
            return str;
        }

        std::wstring ToLower(std::wstring str) {
            std::transform(str.begin(), str.end(), str.begin(), ::towlower);
            return str;
        }

        bool ContainsKey(const nlohmann::json& parent, const std::string& key) {
            if (!parent.is_object()) return false;
            std::string target = ToLower(key);
            for (auto it = parent.begin(); it != parent.end(); ++it) {
                if (ToLower(it.key()) == target) return true;
            }
            return false;
        }

        const nlohmann::json& GetValue(const nlohmann::json& parent, const std::string& key) {
            if (!parent.is_object()) {
                static const nlohmann::json empty;
                return empty;
            }
            std::string target = ToLower(key);
            for (auto it = parent.begin(); it != parent.end(); ++it) {
                if (ToLower(it.key()) == target) return *it;
            }
            static const nlohmann::json empty;
            return empty;
        }

        double SafeGetDouble(const nlohmann::json& parent, const std::string& key, double defaultValue = 0.0) {
            if (!ContainsKey(parent, key)) return defaultValue;
            const auto& node = GetValue(parent, key);
            if (node.is_number()) return node.get<double>();
            else if (node.is_string()) {
                try { return std::stod(node.get<std::string>()); } catch (...) { return defaultValue; }
            }
            return defaultValue;
        }

        int32_t SafeGetInt(const nlohmann::json& parent, const std::string& key, int32_t defaultValue = 0) {
            if (!ContainsKey(parent, key)) return defaultValue;
            const auto& node = GetValue(parent, key);
            if (node.is_number()) return node.get<int32_t>();
            else if (node.is_string()) {
                try { return std::stoi(node.get<std::string>()); } catch (...) { return defaultValue; }
            }
            return defaultValue;
        }

        std::optional<bool> SafeGetOptionalBool(const nlohmann::json& parent, const std::string& key) {
            if (!ContainsKey(parent, key)) return std::nullopt;
            const auto& node = GetValue(parent, key);
            if (node.is_boolean()) return node.get<bool>();
            else if (node.is_string()) {
                std::string s = ToLower(node.get<std::string>());
                return (s == "true" || s == "1");
            } else if (node.is_number()) {
                return node.get<int>() != 0;
            }
            return std::nullopt;
        }

        std::string SafeGetIndexString(const nlohmann::json& node, const std::string& key) {
            if (!ContainsKey(node, key)) return "0";
            const auto& val = GetValue(node, key);
            if (val.is_string()) return val.get<std::string>();
            else if (val.is_number()) return std::to_string(val.get<int>());
            return "0";
        }
    }

    

    void ConfigManager::Initialize(const std::wstring& BasePath) {
        ConfigPath = BasePath + L"Paks/~mods/SwapJSON/";
        LoadConfigJSONs();
    }

    void ConfigManager::LoadConfigJSONs() {
        Configs.clear();
        
        // Ensure the directory exists before scanning
        if (!std::filesystem::exists(ConfigPath)) {
            Output::send<LogLevel::Error>(STR("[DynPals] SwapJSON directory missing! Created empty folder at: {}\n"), ConfigPath);
            std::filesystem::create_directories(ConfigPath);
            return;
        }

        int loadedPacksCount = 0;
        DP_LOG(Normal, "ConfigManager: Scanning recursively for Swap JSONs...");

        // Recursive directory iterator to scan the entire folder tree
         for (const auto& entry : fs::recursive_directory_iterator(ConfigPath)) {
            if (entry.is_regular_file()) {
                std::wstring filepath = entry.path().wstring();
                std::wstring filename = entry.path().filename().wstring();

                // Only load JSON files, and skip configuration template / backup files
                if (entry.path().extension() == L".json") {
                    if (filename.rfind(L"_", 0) == 0 || filename.find(L"Template") != std::wstring::npos) {
                        continue; // Skip metadata files like '_LoadList.json' or templates
                    }

                    std::string fileContent = Utils::ReadFileToString(filepath);
                    if (fileContent.empty()) continue;

                    try {
                        nlohmann::json configData = nlohmann::json::parse(fileContent, nullptr, true, true);
                        
                        // 1. Structural Validation: Must be a JSON Object
                        if (!configData.is_object()) {
                            DP_LOG(Warning, "Skipping invalid config (not a JSON Object): '{}'\n", filename);
                            continue;
                        }

                        // Determine the Pack Name
                        std::wstring packName = L"Default Pack";
                        if (configData.contains("PackName") && configData.at("PackName").is_string()) {
                            packName = Utils::StringToWString(configData.at("PackName").get<std::string>());
                        } else {
                            // Fallback to file name without extension if "PackName" key is missing
                            packName = entry.path().stem().wstring();
                        }

                        // 2. Data Validation: Must contain 'SkelMeshSwap' array
                        if (configData.contains("SkelMeshSwap") && configData.at("SkelMeshSwap").is_array()) {
                            ParseSwaps(packName, configData.at("SkelMeshSwap"));
                            loadedPacksCount++;
                        } else {
                            DP_LOG(Warning, "Skipping config (missing 'SkelMeshSwap' array): '{}'\n", filename);
                        }

                    } catch (const std::exception& e) {
                        // Safe exception catch: Keeps the mod from crashing if a user has a syntax typo in their JSON!
                        DP_LOG(Error, "Failed to parse JSON file '{}': {}\n", filename, Utils::StringToWString(e.what()));
                    }
                }
            }
        }

        DP_LOG(Normal, "Successfully loaded {} skin packs dynamically.\n", loadedPacksCount);
        DP_LOG(Normal, "Complete matchmaking table compiled with {} swaps.\n", Configs.size());
    }




    void ConfigManager::ParseSwaps(const std::wstring& PackName, const nlohmann::json& swapArray) {
        for (auto& swapJson : swapArray) {
            SwapConfig sc;
            sc.PackName = PackName; 
            
            sc.CharacterID = Utils::StringToWString(GetValue(swapJson, "CharacterID").get<std::string>());
            if (ContainsKey(swapJson, "SkelMeshPath")) sc.SkelMeshPath = Utils::StringToWString(GetValue(swapJson, "SkelMeshPath").get<std::string>());
            if (ContainsKey(swapJson, "AnimTarget")) { sc.AnimTarget = Utils::StringToWString(GetValue(swapJson, "AnimTarget").get<std::string>()); }
            if (ContainsKey(swapJson, "Gender")) sc.Gender = Utils::StringToWString(GetValue(swapJson, "Gender").get<std::string>());
            if (ContainsKey(swapJson, "SkinName")) sc.SkinName = Utils::StringToWString(GetValue(swapJson, "SkinName").get<std::string>());
            
            sc.MinLevel = SafeGetInt(swapJson, "MinLevel", 1);
            sc.MaxLevel = SafeGetInt(swapJson, "MaxLevel", 999);
            sc.MinTrust = SafeGetInt(swapJson, "MinTrust", 0);
            sc.MaxTrust = SafeGetInt(swapJson, "MaxTrust", 999999);
            sc.MinRank = SafeGetInt(swapJson, "MinRank", 0);
            sc.MaxRank = SafeGetInt(swapJson, "MaxRank", 5);
            
            if (ContainsKey(swapJson, "SpawnWeight")) {
                int w = SafeGetInt(swapJson, "SpawnWeight", 1);
                sc.SpawnWeight = w > 0 ? w : 1;
            }
            
            if (ContainsKey(swapJson, "SkipTrait")) {
                for (auto& trait : GetValue(swapJson, "SkipTrait")) {
                    sc.SkipTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
                }
            }
            if (ContainsKey(swapJson, "Extra") && GetValue(swapJson, "Extra").is_object()) {
                sc.Extra = Utils::StringToWString(GetValue(swapJson, "Extra").dump());
            }
            
            sc.IsRarePal = SafeGetOptionalBool(swapJson, "IsRarePal");
            sc.IsWildPal = SafeGetOptionalBool(swapJson, "IsWildPal");

            if (ContainsKey(swapJson, "ReqTrait")) {
                for (auto& trait : GetValue(swapJson, "ReqTrait")) sc.ReqTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }
            if (ContainsKey(swapJson, "PrefTrait")) {
                for (auto& trait : GetValue(swapJson, "PrefTrait")) sc.PrefTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }
            
            if (ContainsKey(swapJson, "MatReplace")) {
                for (auto& mat : GetValue(swapJson, "MatReplace")) {
                    MatReplace mr;
                    mr.index = SafeGetIndexString(mat, "Index");
                    mr.matPath = Utils::StringToWString(GetValue(mat, "MatPath").get<std::string>());
                    sc.MatReplaceList.push_back(mr);
                }
            }

            if (ContainsKey(swapJson, "MorphTarget")) {
                for (auto& morph : GetValue(swapJson, "MorphTarget")) {
                    MorphTarget mt;
                    mt.target = Utils::StringToWString(GetValue(morph, "Target").get<std::string>());
                    
                    mt.setVal = SafeGetDouble(morph, "Set", -1000.0);
                    mt.minVal = SafeGetDouble(morph, "Min", 0.0);
                    mt.maxVal = SafeGetDouble(morph, "Max", 1.0);
                    
                    if (ContainsKey(morph, "Type")) mt.type = Utils::StringToWString(GetValue(morph, "Type").get<std::string>());
                    sc.MorphTargetList.push_back(mt);
                }
            }
            Configs.push_back(sc);
        }
    }

    std::vector<SwapEvaluation> ConfigManager::EvaluateAllSwaps(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName, int Rank, int Trust, bool IsWild) const {
        std::vector<SwapEvaluation> results;

        for (size_t i = 0; i < Configs.size(); i++) {
            auto& swap = Configs[i];
            if (ToLower(swap.CharacterID) != ToLower(CharID)) continue;

            SwapEvaluation eval;
            eval.ConfigIndex = (int)i;
            eval.Score = 0; // In DynPals, a LOWER SCORE is fundamentally better (0 is base, negatives are highly specific overrides)
            eval.IsValid = true;

            // 1. Hard Limits & Range Specificity Bonuses
            if (Level < swap.MinLevel || Level > swap.MaxLevel) eval.IsValid = false;
            else if (swap.MinLevel > 1 || swap.MaxLevel < 999) eval.Score -= 10; // Specificity Bonus

            if (eval.IsValid && (Rank < swap.MinRank || Rank > swap.MaxRank)) eval.IsValid = false;
            else if (swap.MinRank > 0 || swap.MaxRank < 5) eval.Score -= 10; // Specificity Bonus

            if (eval.IsValid && (Trust < swap.MinTrust || Trust > swap.MaxTrust)) eval.IsValid = false;
            else if (swap.MinTrust > 0 || swap.MaxTrust < 999999) eval.Score -= 10; // Specificity Bonus

            // 2. Gender Match
            if (eval.IsValid && ToLower(swap.Gender) != L"none") {
                std::wstring swapGender = ToLower(swap.Gender);
                std::wstring charGender = ToLower(GenderStr);
                if (swapGender != charGender) {
                    bool fallbackMatched = false;
                    if (swapGender == L"male" && (charGender == L"futa" || charGender == L"fullfuta")) {
                        eval.Score += 50000; // Penalize fallback so direct Futa skins win if available
                        fallbackMatched = true;
                    } else if (swapGender == L"female" && (charGender == L"andro" || charGender == L"neutered" || charGender == L"fullneutered")) {
                        eval.Score += 50000;
                        fallbackMatched = true;
                    }
                    if (!fallbackMatched) eval.IsValid = false;
                } else {
                    eval.Score -= 100; // HUGE bonus for matching explicitly defined genders
                }
            } else if (eval.IsValid && ToLower(swap.Gender) == L"none" && ToLower(GenderStr) != L"none") {
                eval.Score += 500000; // Massive penalty if skin is generic "None" but Pal has an actual gender
            }

            // 3. Exact String Matches
            if (eval.IsValid && !swap.SkinName.empty()) {
                if (ToLower(SkinName) != ToLower(swap.SkinName)) eval.IsValid = false;
                else eval.Score -= 50; // Explicit skin name match bonus
            }

            // 4. Boolean Flags
            if (eval.IsValid && swap.IsRarePal.has_value()) {
                bool reqRare = swap.IsRarePal.value();
                if (reqRare != IsRare) eval.IsValid = false; 
                else eval.Score -= 50; // Skin explicitly asked for Rare status, beat generic skins!
            }

            if (eval.IsValid && swap.IsWildPal.has_value()) {
                bool reqWild = swap.IsWildPal.value();
                if (reqWild != IsWild) eval.IsValid = false;
                else eval.Score -= 50; // Skin explicitly asked for Wild status, beat generic skins!
            }

            // 5. Traits
            if (eval.IsValid) {
                for (const auto& req : swap.ReqTrait) {
                    bool hasTrait = false;
                    for (const auto& t : Traits) {
                        if (ToLower(t) == ToLower(req)) { hasTrait = true; break; }
                    }
                    if (!hasTrait) {
                        eval.IsValid = false;
                        break;
                    } else {
                        eval.Score -= 20; // Specificity Bonus per matched required trait!
                    }
                }
            }

            if (eval.IsValid) {
                for (const auto& pref : swap.PrefTrait) {
                    bool hasTrait = false;
                    for (const auto& t : Traits) {
                        if (ToLower(t) == ToLower(pref)) { hasTrait = true; break; }
                    }
                    if (hasTrait) eval.Score -= 10; // Bonus if preferred trait is matched
                    else eval.Score += 10;          // Minor penalty if missing
                }
            }
            if (eval.IsValid) {
                for (const auto& skip : swap.SkipTrait) {
                    bool hasBlacklistedTrait = false;
                    for (const auto& t : Traits) {
                        if (ToLower(t) == ToLower(skip)) {
                            hasBlacklistedTrait = true;
                            break;
                        }
                    }
                    if (hasBlacklistedTrait) {
                        eval.IsValid = false; // Banned trait found, skip this swap!
                        break;
                    }
                }
            }
            results.push_back(eval);
        }
        return results;
    }
int ConfigManager::FindConfigIndex(const std::wstring& PackName, const std::wstring& SkinName, const std::wstring& SkelMeshPath) const {
        // Tier 1: Exact Match (PackName + SkinName) - Only if SkinName isn't empty to avoid false positives on generic configs
        if (!SkinName.empty()) {
            for (size_t i = 0; i < Configs.size(); ++i) {
                if (Configs[i].PackName == PackName && Configs[i].SkinName == SkinName) return (int)i;
            }
        }
        
        // Tier 2: PackName + SkelMeshPath (Fallback for anonymous skins)
        for (size_t i = 0; i < Configs.size(); ++i) {
            if (Configs[i].PackName == PackName && Configs[i].SkelMeshPath == SkelMeshPath) return (int)i;
        }

        // Tier 3: SkelMeshPath only (Fallback if the user renamed the Pack folder/json file)
        for (size_t i = 0; i < Configs.size(); ++i) {
            if (Configs[i].SkelMeshPath == SkelMeshPath) return (int)i;
        }

        return -1; // Pack was uninstalled or heavily modified
    }
    int ConfigManager::PickBestSwap(const std::vector<SwapEvaluation>& evaluations) const {
        int bestScore = 999999;
        std::vector<int> bestMatches;

        for (const auto& eval : evaluations) {
            if (!eval.IsValid) continue;
            
            // Find the absolute lowest (most specific) score
            if (eval.Score < bestScore) {
                bestScore = eval.Score;
                bestMatches = { eval.ConfigIndex };
            } else if (eval.Score == bestScore) {
                // Ties go into a pool to be decided by SpawnWeight
                bestMatches.push_back(eval.ConfigIndex);
            }
        }

        if (!bestMatches.empty()) {
            int totalWeight = 0;
            for (int idx : bestMatches) {
                totalWeight += Configs[idx].SpawnWeight;
            }

            if (totalWeight > 0) {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, totalWeight - 1);
                int randomValue = dis(gen);

                int cumulativeWeight = 0;
                for (int idx : bestMatches) {
                    cumulativeWeight += Configs[idx].SpawnWeight;
                    if (randomValue < cumulativeWeight) {
                        return idx;
                    }
                }
            }
            return bestMatches[0]; // Fallback
        }
        return -1;
    }

    std::vector<int> ConfigManager::GetConfigsForCharID(const std::wstring& CharID) const {
        std::vector<int> results;
        for (size_t i = 0; i < Configs.size(); ++i) {
            if (ToLower(Configs[i].CharacterID) == ToLower(CharID)) {
                results.push_back((int)i);
            }
        }
        return results;
    }
}