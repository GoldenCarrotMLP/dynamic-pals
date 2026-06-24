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
        ConfigPath = BasePath + L"Paks/~mods/";
        LoadConfigJSONs();
    }

    void ConfigManager::LoadConfigJSONs() {
        Configs.clear();
        
        std::wstring PathV1 = ConfigPath + L"SwapJSON/";
        std::wstring PathV2 = ConfigPath + L"ModelJSON/";

        // 1. Ensure both directories exist safely
        if (!std::filesystem::exists(PathV1)) {
            std::filesystem::create_directories(PathV1);
        }
        if (!std::filesystem::exists(PathV2)) {
            std::filesystem::create_directories(PathV2);
        }

        int loadedPacksCount = 0;
        DP_LOG(Normal, "ConfigManager: Scanning recursively for Swap/Model JSONs...");


         for (const auto& entry : fs::recursive_directory_iterator(ConfigPath)) {
            if (entry.is_regular_file()) {
                std::wstring filepath = entry.path().wstring();
                std::wstring filename = entry.path().filename().wstring();

                if (entry.path().extension() == L".json") {
                    if (filename.rfind(L"_", 0) == 0 || filename.find(L"Template") != std::wstring::npos) {
                        continue;
                    }

                    std::string fileContent = Utils::ReadFileToString(filepath);
                    if (fileContent.empty()) continue;

                    try {
                        nlohmann::json configData = nlohmann::json::parse(fileContent, nullptr, true, true);
                        
                        if (!configData.is_object()) {
                            DP_LOG(Warning, "Skipping invalid config (not a JSON Object): '{}'\n", filename);
                            continue;
                        }

                        // Support both PackName (v1) and ModelPack (v2) naming conventions
                        std::wstring packName = L"Default Pack";
                        if (configData.contains("PackName") && configData.at("PackName").is_string()) {
                            packName = Utils::StringToWString(configData.at("PackName").get<std::string>());
                        } else if (configData.contains("ModelPack") && configData.at("ModelPack").is_string()) {
                            packName = Utils::StringToWString(configData.at("ModelPack").get<std::string>());
                        } else {
                            packName = entry.path().stem().wstring();
                        }

                        // Smart format detection
                        if (configData.contains("SkelMeshSwap") && configData.at("SkelMeshSwap").is_array()) {
                            ParseSwaps(packName, configData.at("SkelMeshSwap"));
                            loadedPacksCount++;
                        } else if (configData.contains("SkinList") && configData.at("SkinList").is_object()) {
                            ParseSwapsV2(packName, configData.at("SkinList"));
                            loadedPacksCount++;
                        } else {
                            DP_LOG(Warning, "Skipping config (missing 'SkelMeshSwap' or 'SkinList' root): '{}'\n", filename);
                        }

                    } catch (const std::exception& e) {
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
            
            if (ContainsKey(swapJson, "CharacterID")) sc.CharacterID = Utils::StringToWString(GetValue(swapJson, "CharacterID").get<std::string>());
            if (ContainsKey(swapJson, "SkelMeshPath")) sc.SkelMeshPath = Utils::StringToWString(GetValue(swapJson, "SkelMeshPath").get<std::string>());
            if (ContainsKey(swapJson, "AnimTarget")) sc.AnimTarget = Utils::StringToWString(GetValue(swapJson, "AnimTarget").get<std::string>());
            if (ContainsKey(swapJson, "Gender")) sc.Gender = Utils::StringToWString(GetValue(swapJson, "Gender").get<std::string>());
            if (ContainsKey(swapJson, "SkinName")) sc.SkinName = Utils::StringToWString(GetValue(swapJson, "SkinName").get<std::string>());
            if (ContainsKey(swapJson, "SkinName")) sc.SkinName = Utils::StringToWString(GetValue(swapJson, "SkinName").get<std::string>());
            
            // Parse explicit UI friendly display labels into SwapLabel
            if (ContainsKey(swapJson, "SkinLabel")) sc.SwapLabel = Utils::StringToWString(GetValue(swapJson, "SkinLabel").get<std::string>());
            else if (ContainsKey(swapJson, "SwapLabel")) sc.SwapLabel = Utils::StringToWString(GetValue(swapJson, "SwapLabel").get<std::string>());
            else if (ContainsKey(swapJson, "SwapName")) sc.SwapLabel = Utils::StringToWString(GetValue(swapJson, "SwapName").get<std::string>());
            
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

            // Aligned: Robust Extra metadata parsing (supports both JSON object and stringified JSON string)
            if (ContainsKey(swapJson, "Extra")) {
                const auto& extraNode = GetValue(swapJson, "Extra");
                if (extraNode.is_object()) {
                    sc.Extra = Utils::StringToWString(extraNode.dump());
                } else if (extraNode.is_string()) {
                    sc.Extra = Utils::StringToWString(extraNode.get<std::string>());
                }
            }
            
            // Aligned: LuckyStarReq alias fallback for V1
            if (ContainsKey(swapJson, "LuckyStarReq")) {
                sc.IsRarePal = SafeGetOptionalBool(swapJson, "LuckyStarReq");
            } else {
                sc.IsRarePal = SafeGetOptionalBool(swapJson, "IsRarePal");
            }
            
            sc.IsWildPal = SafeGetOptionalBool(swapJson, "IsWildPal");

            // Aligned: PassiveSkills alias fallback for V1
            if (ContainsKey(swapJson, "PassiveSkills")) {
                for (auto& trait : GetValue(swapJson, "PassiveSkills")) sc.ReqTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            } else if (ContainsKey(swapJson, "ReqTrait")) {
                for (auto& trait : GetValue(swapJson, "ReqTrait")) sc.ReqTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }

            if (ContainsKey(swapJson, "PrefTrait")) {
                for (auto& trait : GetValue(swapJson, "PrefTrait")) sc.PrefTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }
            
            // Aligned: SpecialMaterial / MatReplace dual support for V1
            if (ContainsKey(swapJson, "SpecialMaterial")) {
                for (auto& mat : GetValue(swapJson, "SpecialMaterial")) {
                    MatReplace mr;
                    mr.index = SafeGetIndexString(mat, "Index");
                    if (ContainsKey(mat, "MaterialAsset")) {
                        mr.matPath = Utils::StringToWString(GetValue(mat, "MaterialAsset").get<std::string>());
                    } else if (ContainsKey(mat, "MatPath")) {
                        mr.matPath = Utils::StringToWString(GetValue(mat, "MatPath").get<std::string>());
                    }
                    sc.MatReplaceList.push_back(mr);
                }
            } else if (ContainsKey(swapJson, "MatReplace")) {
                for (auto& mat : GetValue(swapJson, "MatReplace")) {
                    MatReplace mr;
                    mr.index = SafeGetIndexString(mat, "Index");
                    mr.matPath = Utils::StringToWString(GetValue(mat, "MatPath").get<std::string>());
                    sc.MatReplaceList.push_back(mr);
                }
            }

            // Aligned: ShapeKeys / MorphTarget dual support for V1
            if (ContainsKey(swapJson, "ShapeKeys")) {
                for (auto& morph : GetValue(swapJson, "ShapeKeys")) {
                    MorphTarget mt;
                    if (ContainsKey(morph, "Name")) {
                        mt.target = Utils::StringToWString(GetValue(morph, "Name").get<std::string>());
                    } else if (ContainsKey(morph, "Target")) {
                        mt.target = Utils::StringToWString(GetValue(morph, "Target").get<std::string>());
                    }
                    
                    mt.setVal = SafeGetDouble(morph, "Set", -1000.0);
                    mt.minVal = SafeGetDouble(morph, "Min", 0.0);
                    mt.maxVal = SafeGetDouble(morph, "Max", 1.0);
                    
                    if (ContainsKey(morph, "Mode")) {
                        std::string modeStr = GetValue(morph, "Mode").get<std::string>();
                        if (ToLower(modeStr) == "restrictive" || ToLower(modeStr) == "restrict") mt.type = L"Restrict";
                        else mt.type = Utils::StringToWString(modeStr);
                    } else if (ContainsKey(morph, "Type")) {
                        mt.type = Utils::StringToWString(GetValue(morph, "Type").get<std::string>());
                    }
                    sc.MorphTargetList.push_back(mt);
                }
            } else if (ContainsKey(swapJson, "MorphTarget")) {
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
    
    
    void ConfigManager::ParseSwapsV2(const std::wstring& PackName, const nlohmann::json& skinListObj) {
        for (auto& [charIdStr, skinsObj] : skinListObj.items()) {
            if (!skinsObj.is_object()) continue;
            std::wstring charID = Utils::StringToWString(charIdStr);
            
            for (auto& [skinLabelStr, swapJson] : skinsObj.items()) {
                if (!swapJson.is_object()) continue;

                SwapConfig sc;
                sc.PackName = PackName;
                sc.CharacterID = charID;
                sc.SkinName = Utils::StringToWString(skinLabelStr); 
                sc.SwapLabel = Utils::StringToWString(skinLabelStr); 

                      
                
                // Core Translations
                if (ContainsKey(swapJson, "SkinPath")) sc.SkelMeshPath = Utils::StringToWString(GetValue(swapJson, "SkinPath").get<std::string>());
                if (ContainsKey(swapJson, "AnimTarget")) sc.AnimTarget = Utils::StringToWString(GetValue(swapJson, "AnimTarget").get<std::string>());
                if (ContainsKey(swapJson, "Gender")) sc.Gender = Utils::StringToWString(GetValue(swapJson, "Gender").get<std::string>());
                
                // Limits mapping
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
                
                // Wild Pal Flag
                if (ContainsKey(swapJson, "IsWildPal")) {
                    sc.IsWildPal = SafeGetOptionalBool(swapJson, "IsWildPal");
                }

                // Lucky Star / Rare Pal Flag
                if (ContainsKey(swapJson, "LuckyStarReq")) {
                    sc.IsRarePal = SafeGetOptionalBool(swapJson, "LuckyStarReq");
                } else if (ContainsKey(swapJson, "IsRarePal")) {
                    sc.IsRarePal = SafeGetOptionalBool(swapJson, "IsRarePal");
                }

                // Trait handling
                if (ContainsKey(swapJson, "PassiveSkills")) {
                    for (auto& trait : GetValue(swapJson, "PassiveSkills")) sc.ReqTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
                } else if (ContainsKey(swapJson, "ReqTrait")) {
                    for (auto& trait : GetValue(swapJson, "ReqTrait")) sc.ReqTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
                }

                if (ContainsKey(swapJson, "PrefTrait")) {
                    for (auto& trait : GetValue(swapJson, "PrefTrait")) sc.PrefTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
                }
                
                if (ContainsKey(swapJson, "SkipTrait")) {
                    for (auto& trait : GetValue(swapJson, "SkipTrait")) sc.SkipTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
                }
                
                // Materials
                if (ContainsKey(swapJson, "SpecialMaterial")) {
                    for (auto& mat : GetValue(swapJson, "SpecialMaterial")) {
                        MatReplace mr;
                        mr.index = SafeGetIndexString(mat, "Index");
                        if (ContainsKey(mat, "MaterialAsset")) {
                            mr.matPath = Utils::StringToWString(GetValue(mat, "MaterialAsset").get<std::string>());
                        } else if (ContainsKey(mat, "MatPath")) {
                            mr.matPath = Utils::StringToWString(GetValue(mat, "MatPath").get<std::string>());
                        }
                        sc.MatReplaceList.push_back(mr);
                    }
                }

                // Morphs
                if (ContainsKey(swapJson, "ShapeKeys")) {
                    for (auto& morph : GetValue(swapJson, "ShapeKeys")) {
                        MorphTarget mt;
                        if (ContainsKey(morph, "Name")) {
                            mt.target = Utils::StringToWString(GetValue(morph, "Name").get<std::string>());
                        } else if (ContainsKey(morph, "Target")) {
                            mt.target = Utils::StringToWString(GetValue(morph, "Target").get<std::string>());
                        }
                        
                        mt.setVal = SafeGetDouble(morph, "Set", -1000.0);
                        mt.minVal = SafeGetDouble(morph, "Min", 0.0);
                        mt.maxVal = SafeGetDouble(morph, "Max", 1.0);
                        
                        if (ContainsKey(morph, "Mode")) {
                            std::string modeStr = GetValue(morph, "Mode").get<std::string>();
                            if (ToLower(modeStr) == "restrictive" || ToLower(modeStr) == "restrict") mt.type = L"Restrict";
                            else mt.type = Utils::StringToWString(modeStr);
                        } else if (ContainsKey(morph, "Type")) {
                            mt.type = Utils::StringToWString(GetValue(morph, "Type").get<std::string>());
                        }
                        sc.MorphTargetList.push_back(mt);
                    }
                }

                // Metadata Extra parsing (supports either nested JSON object or stringified JSON string safely)
                if (ContainsKey(swapJson, "Extra")) {
                    const auto& extraNode = GetValue(swapJson, "Extra");
                    if (extraNode.is_object()) {
                        sc.Extra = Utils::StringToWString(extraNode.dump());
                    } else if (extraNode.is_string()) {
                        sc.Extra = Utils::StringToWString(extraNode.get<std::string>());
                    }
                }
                
                Configs.push_back(sc);
            }
        }
    }
    std::vector<SwapEvaluation> ConfigManager::EvaluateAllSwaps(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName, int Rank, int Trust, bool IsWild) const {
        std::vector<SwapEvaluation> results;

        for (size_t i = 0; i < Configs.size(); i++) {
            auto& swap = Configs[i];
            if (ToLower(swap.CharacterID) != ToLower(CharID)) continue;

            SwapEvaluation eval;
            eval.ConfigIndex = (int)i;
            eval.Score = 0; 
            eval.IsValid = true;

            // 1. Hard Limits & Range Specificity Bonuses
            if (Level < swap.MinLevel || Level > swap.MaxLevel) eval.IsValid = false;
            else if (swap.MinLevel > 1 || swap.MaxLevel < 999) eval.Score -= 10; 

            if (eval.IsValid && (Rank < swap.MinRank || Rank > swap.MaxRank)) eval.IsValid = false;
            else if (swap.MinRank > 0 || swap.MaxRank < 5) eval.Score -= 10; 

            if (eval.IsValid && (Trust < swap.MinTrust || Trust > swap.MaxTrust)) eval.IsValid = false;
            else if (swap.MinTrust > 0 || swap.MaxTrust < 999999) eval.Score -= 10; 

            // 2. Gender Match (Blacklist style - No score adjustments, just validation checks)
            std::wstring swapGender = ToLower(swap.Gender);
            std::wstring charGender = ToLower(GenderStr);

            // Normalize "any", "all", or missing/empty strings to "none"
            if (swapGender == L"any" || swapGender == L"all" || swapGender.empty()) {
                swapGender = L"none";
            }
            if (charGender == L"any" || charGender == L"all" || charGender.empty()) {
                charGender = L"none";
            }

            // If the skin specifies a gender, it acts as a strict blacklist
            if (eval.IsValid && swapGender != L"none") {
                if (swapGender != charGender) {
                    bool fallbackMatched = false;
                    
                    // Support for SCake extended genders (treated as valid matches, but no score bonus)
                    if (swapGender == L"male" && (charGender == L"futa" || charGender == L"fullfuta")) {
                        fallbackMatched = true;
                    } else if (swapGender == L"female" && (charGender == L"andro" || charGender == L"neutered" || charGender == L"fullneutered")) {
                        fallbackMatched = true;
                    }
                    
                    // If it entirely misses the gender and fallbacks, invalidate it completely
                    if (!fallbackMatched) {
                        eval.IsValid = false;
                    }
                }
            }

            // 3. Exact String Matches
            if (eval.IsValid && !swap.SkinName.empty()) {
                if (ToLower(SkinName) != ToLower(swap.SkinName)) eval.IsValid = false;
                else eval.Score -= 50; 
            }

            // 4. Boolean Flags
            if (eval.IsValid && swap.IsRarePal.has_value()) {
                bool reqRare = swap.IsRarePal.value();
                if (reqRare != IsRare) eval.IsValid = false; 
                else eval.Score -= 50; 
            }

            if (eval.IsValid && swap.IsWildPal.has_value()) {
                bool reqWild = swap.IsWildPal.value();
                if (reqWild != IsWild) eval.IsValid = false;
                else eval.Score -= 50; 
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
                        eval.Score -= 20; 
                    }
                }
            }

            if (eval.IsValid) {
                for (const auto& pref : swap.PrefTrait) {
                    bool hasTrait = false;
                    for (const auto& t : Traits) {
                        if (ToLower(t) == ToLower(pref)) { hasTrait = true; break; }
                    }
                    if (hasTrait) eval.Score -= 10; 
                    else eval.Score += 10;          
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
                        eval.IsValid = false; 
                        break;
                    }
                }
            }
            
            results.push_back(eval);
        }
        return results;
    }

    int ConfigManager::FindConfigIndex(const std::wstring& PackName, const std::wstring& SkinName, const std::wstring& SwapLabel, const std::wstring& SkelMeshPath) const {
        
        // Tier 1: Exact Match by PackName + SwapLabel (Safely resolves identical meshes with different materials)
        if (!SwapLabel.empty()) {
            for (size_t i = 0; i < Configs.size(); ++i) {
                if (Configs[i].PackName == PackName && Configs[i].SwapLabel == SwapLabel) return (int)i;
            }
        }
        
        // Tier 2: Exact Match by PackName + Game-native SkinName
        if (!SkinName.empty() && SkinName != L"None") {
            for (size_t i = 0; i < Configs.size(); ++i) {
                if (Configs[i].PackName == PackName && Configs[i].SkinName == SkinName) return (int)i;
            }
        }
        
        // Tier 3: Exact Match by PackName + unique SkelMeshPath
        for (size_t i = 0; i < Configs.size(); ++i) {
            if (Configs[i].PackName == PackName && Configs[i].SkelMeshPath == SkelMeshPath) return (int)i;
        }

        // Tier 4: Path fallback (if pack folder or json was renamed by the user)
        for (size_t i = 0; i < Configs.size(); ++i) {
            if (Configs[i].SkelMeshPath == SkelMeshPath) return (int)i;
        }

        return -1; 
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
            // 1. BIAS DETECTION: Only evaluate if we have a tie (more than 1 candidate)
            if (bestMatches.size() > 1) {
                int maxWeight = -1;
                int maxWeightIdx = -1;
                int secondMaxWeight = -1;
                int secondMaxWeightIdx = -1;

                // Find the highest and second-highest weights in the tie-breaker pool
                for (int idx : bestMatches) {
                    int w = Configs[idx].SpawnWeight;
                    if (w > maxWeight) {
                        secondMaxWeight = maxWeight;
                        secondMaxWeightIdx = maxWeightIdx;
                        maxWeight = w;
                        maxWeightIdx = idx;
                    } else if (w > secondMaxWeight) {
                        secondMaxWeight = w;
                        secondMaxWeightIdx = idx;
                    }
                }

                // If the top candidate's weight is over double the next best candidate's weight [1]
                if (secondMaxWeight > 0 && maxWeight > (2 * secondMaxWeight)) {
                    auto& maxConfig = Configs[maxWeightIdx];
                    std::wstring maxName = maxConfig.SkinName.empty() ? L"Anonymous Mesh" : maxConfig.SkinName;
                    
                    std::wstring secondName = L"Other Candidates";
                    if (secondMaxWeightIdx != -1) {
                        secondName = Configs[secondMaxWeightIdx].SkinName.empty() ? L"Anonymous Mesh" : Configs[secondMaxWeightIdx].SkinName;
                    }

                    // Fires a yellow warning toast directly on the player's screen! [2]
                    DP_LOG(Warning, "Skin '{}' in Pack '{}' has a biased spawn weight ({}) which is over double the weight of other candidates (like '{}' with {}). You can adjust this in the JSON file for more variety!", 
                           maxName, maxConfig.PackName, maxWeight, secondName, secondMaxWeight);
                }
            }

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