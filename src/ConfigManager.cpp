#include "ConfigManager.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include "json.hpp"
#include <random>
#include <algorithm>
#include <filesystem>
#include <set> 
#include <sstream>

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

        // O(1) Case-insensitive dictionary lookup to bypass repeated allocations in loop
        struct JsonDict {
            std::map<std::string, const nlohmann::json*> data;
            
            JsonDict(const nlohmann::json& node) {
                if (node.is_object()) {
                    for (auto it = node.begin(); it != node.end(); ++it) {
                        data[ToLower(it.key())] = &(*it);
                    }
                }
            }
            
            bool contains(const std::string& key) const {
                return data.find(ToLower(key)) != data.end();
            }
            
            const nlohmann::json& get(const std::string& key) const {
                static const nlohmann::json empty;
                auto it = data.find(ToLower(key));
                return it != data.end() ? *(it->second) : empty;
            }
        };

        // --- SINGLE UNIFIED HELPER: Splits comma-separated strings into multiple independent override instructions! ---
        void ParseMaterialIndices(const std::string& rawIndex, const std::wstring& matPath, bool bRandomHue, SwapConfig& sc) {
            std::stringstream ss(rawIndex);
            std::string token;
            while (std::getline(ss, token, ',')) {
                size_t first = token.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) continue;
                size_t last = token.find_last_not_of(" \t\r\n");
                token = token.substr(first, last - first + 1);
                
                if (!token.empty()) {
                    sc.MatReplaceList.push_back({token, matPath, bRandomHue});
                }
            }
        }

        double SafeGetDouble(const JsonDict& dict, const std::string& key, double defaultValue = 0.0) {
            if (!dict.contains(key)) return defaultValue;
            const auto& node = dict.get(key);
            if (node.is_number()) return node.get<double>();
            else if (node.is_string()) {
                try { return std::stod(node.get<std::string>()); } catch (...) { return defaultValue; }
            }
            return defaultValue;
        }

        int32_t SafeGetInt(const JsonDict& dict, const std::string& key, int32_t defaultValue = 0) {
            if (!dict.contains(key)) return defaultValue;
            const auto& node = dict.get(key);
            if (node.is_number()) return node.get<int32_t>();
            else if (node.is_string()) {
                try { return std::stoi(node.get<std::string>()); } catch (...) { return defaultValue; }
            }
            return defaultValue;
        }

        std::optional<bool> SafeGetOptionalBool(const JsonDict& dict, const std::string& key) {
            if (!dict.contains(key)) return std::nullopt;
            const auto& node = dict.get(key);
            if (node.is_boolean()) return node.get<bool>();
            else if (node.is_string()) {
                std::string s = ToLower(node.get<std::string>());
                return (s == "true" || s == "1");
            } else if (node.is_number()) {
                return node.get<int>() != 0;
            }
            return std::nullopt;
        }

        std::string SafeGetIndexString(const JsonDict& dict, const std::string& key) {
            if (!dict.contains(key)) return "0";
            const auto& val = dict.get(key);
            if (val.is_string()) return val.get<std::string>();
            else if (val.is_number()) return std::to_string(val.get<int>());
            return "0";
        }

        void ValidateGender(const std::wstring& genderStr, const std::wstring& packName, const std::wstring& swapLabel) {
            std::wstring lg = ToLower(genderStr);
            if (!lg.empty() && lg != L"none" && lg != L"male" && lg != L"female" &&
                lg != L"any" && lg != L"all" && lg != L"both" && lg != L"futa" && lg != L"fullfuta" && 
                lg != L"andro" && lg != L"neutered" && lg != L"fullneutered") {
                
                DP_LOG(Error, "JSON ERROR in Pack '{}': Swap '{}' has an invalid Gender '{}'. Valid options: None, Male, Female.", packName, swapLabel, genderStr);
            }
        }

        void ParseCommonSwapProperties(SwapConfig& sc, const JsonDict& dict) {
            if (dict.contains("AnimTarget")) sc.AnimTarget = Utils::StringToWString(dict.get("AnimTarget").get<std::string>());
            if (dict.contains("Gender")) sc.Gender = Utils::StringToWString(dict.get("Gender").get<std::string>());
            if (dict.contains("SkinName")) sc.SkinName = Utils::StringToWString(dict.get("SkinName").get<std::string>());
            if (dict.contains("SetNickname")) sc.SetNickname = Utils::StringToWString(dict.get("SetNickname").get<std::string>());
            
            sc.MinLevel = SafeGetInt(dict, "MinLevel", 1);
            sc.MaxLevel = SafeGetInt(dict, "MaxLevel", 999);
            sc.MinTrust = SafeGetInt(dict, "MinTrust", 0);
            sc.MaxTrust = SafeGetInt(dict, "MaxTrust", 999999);
            sc.MinRank = SafeGetInt(dict, "MinRank", 0);
            sc.MaxRank = SafeGetInt(dict, "MaxRank", 5);
            
            if (dict.contains("SpawnWeight")) {
                double w = SafeGetDouble(dict, "SpawnWeight", 1.0);
                sc.SpawnWeight = w > 0.0 ? w : 1.0;
            }
            
            if (dict.contains("ReqSwap")) {
                for (auto& req : dict.get("ReqSwap")) sc.ReqSwap.push_back(Utils::StringToWString(req.get<std::string>()));
            }
            if (dict.contains("SkipTrait")) {
                for (auto& trait : dict.get("SkipTrait")) sc.SkipTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }
            
            if (dict.contains("Extra")) {
                const auto& extraNode = dict.get("Extra");
                if (extraNode.is_object()) sc.Extra = Utils::StringToWString(extraNode.dump());
                else if (extraNode.is_string()) sc.Extra = Utils::StringToWString(extraNode.get<std::string>());
            }
            
            if (dict.contains("LuckyStarReq")) sc.IsRarePal = SafeGetOptionalBool(dict, "LuckyStarReq");
            else if (dict.contains("IsRarePal")) sc.IsRarePal = SafeGetOptionalBool(dict, "IsRarePal");
            
            sc.IsWildPal = SafeGetOptionalBool(dict, "IsWildPal");

            if (dict.contains("PassiveSkills")) {
                for (auto& trait : dict.get("PassiveSkills")) sc.ReqTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            } else if (dict.contains("ReqTrait")) {
                for (auto& trait : dict.get("ReqTrait")) sc.ReqTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }

            if (dict.contains("PrefTrait")) {
                for (auto& trait : dict.get("PrefTrait")) sc.PrefTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }

            if (dict.contains("SpecialMaterial")) {
                for (auto& mat : dict.get("SpecialMaterial")) {
                    JsonDict matDict(mat);
                    std::string rawIndex = SafeGetIndexString(matDict, "Index");
                    bool bRandomHue = SafeGetOptionalBool(matDict, "RandomHue").value_or(false);
                    std::wstring matPath;
                    if (matDict.contains("MaterialAsset")) matPath = Utils::StringToWString(matDict.get("MaterialAsset").get<std::string>());
                    else if (matDict.contains("MatPath")) matPath = Utils::StringToWString(matDict.get("MatPath").get<std::string>());
                    ParseMaterialIndices(rawIndex, matPath, bRandomHue, sc);
                }
            } else if (dict.contains("MatReplace")) {
                for (auto& mat : dict.get("MatReplace")) {
                    JsonDict matDict(mat);
                    std::string rawIndex = SafeGetIndexString(matDict, "Index");
                    bool bRandomHue = SafeGetOptionalBool(matDict, "RandomHue").value_or(false);
                    std::wstring matPath = Utils::StringToWString(matDict.get("MatPath").get<std::string>());
                    ParseMaterialIndices(rawIndex, matPath, bRandomHue, sc);
                }
            }

            if (dict.contains("ShapeKeys")) {
                for (auto& morph : dict.get("ShapeKeys")) {
                    JsonDict morphDict(morph);
                    MorphTarget mt;
                    if (morphDict.contains("Name")) mt.target = Utils::StringToWString(morphDict.get("Name").get<std::string>());
                    else if (morphDict.contains("Target")) mt.target = Utils::StringToWString(morphDict.get("Target").get<std::string>());
                    
                    mt.setVal = SafeGetDouble(morphDict, "Set", -1000.0);
                    mt.minVal = SafeGetDouble(morphDict, "Min", 0.0);
                    mt.maxVal = SafeGetDouble(morphDict, "Max", 1.0);
                    
                    if (morphDict.contains("Mode")) {
                        std::string modeStr = morphDict.get("Mode").get<std::string>();
                        if (ToLower(modeStr) == "restrictive" || ToLower(modeStr) == "restrict") mt.type = L"Restrict";
                        else mt.type = Utils::StringToWString(modeStr);
                    } else if (morphDict.contains("Type")) {
                        mt.type = Utils::StringToWString(morphDict.get("Type").get<std::string>());
                    }
                    sc.MorphTargetList.push_back(mt);
                }
            } else if (dict.contains("MorphTarget")) {
                for (auto& morph : dict.get("MorphTarget")) {
                    JsonDict morphDict(morph);
                    MorphTarget mt;
                    mt.target = Utils::StringToWString(morphDict.get("Target").get<std::string>());
                    mt.setVal = SafeGetDouble(morphDict, "Set", -1000.0);
                    mt.minVal = SafeGetDouble(morphDict, "Min", 0.0);
                    mt.maxVal = SafeGetDouble(morphDict, "Max", 1.0);
                    if (morphDict.contains("Type")) mt.type = Utils::StringToWString(morphDict.get("Type").get<std::string>());
                    sc.MorphTargetList.push_back(mt);
                }
            }
        }
    }

    void ConfigManager::Initialize(const std::wstring& BasePath) {
        ConfigPath = BasePath + L"Paks/~mods/";
        LoadConfigJSONs();
    }

    void ConfigManager::LoadConfigJSONs() {
        Configs.clear();
        WarnedCharIDs.clear();
        
        std::wstring PathV1 = ConfigPath + L"SwapJSON/";
        std::wstring PathV2 = ConfigPath + L"ModelJSON/";

        if (!std::filesystem::exists(PathV1)) {
            std::filesystem::create_directories(PathV1);
        }
        if (!std::filesystem::exists(PathV2)) {
            std::filesystem::create_directories(PathV2);
        }

        int loadedPacksCount = 0;
        DP_LOG(Default, "ConfigManager: Scanning recursively for Swap/Model JSONs...");

        std::vector<std::wstring> targetPaths = { PathV1, PathV2 };

        for (const auto& targetPath : targetPaths) {
            if (!std::filesystem::exists(targetPath)) continue;

            for (const auto& entry : fs::recursive_directory_iterator(targetPath)) {
                if (entry.is_regular_file()) {
                    std::wstring filepath = entry.path().wstring();
                    std::wstring filename = entry.path().filename().wstring();

                    if (ToLower(entry.path().extension().wstring()) == L".json") {
                        if (filename.rfind(L"_", 0) == 0 || filename.find(L"Template") != std::wstring::npos) {
                            continue;
                        }

                        std::string fileContent = Utils::ReadFileToString(filepath);
                        if (fileContent.empty()) continue;

                        try {
                            std::vector<std::set<std::string>> keysAtDepth;
                            std::wstring currentFilename = filename;

                            nlohmann::json::parser_callback_t cb = [&keysAtDepth, currentFilename](int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
                                if (depth + 1 >= (int)keysAtDepth.size()) {
                                    keysAtDepth.resize(depth + 2);
                                }

                                if (event == nlohmann::json::parse_event_t::object_start) {
                                    keysAtDepth[depth + 1].clear();
                                }
                                else if (event == nlohmann::json::parse_event_t::key) {
                                    if (parsed.is_string()) {
                                        std::string keyName = parsed.get<std::string>();
                                        if (keysAtDepth[depth].count(keyName)) {
                                            std::wstring wKey = Utils::StringToWString(keyName);
                                            DP_LOG(Error, "JSON DUPLICATE KEY ERROR in '{}': Key '{}' appears multiple times in the same block! The parser will OVERWRITE the first one. Please ensure all your skin labels/keys are uniquely named.", currentFilename, wKey);
                                        } else {
                                            keysAtDepth[depth].insert(keyName);
                                        }
                                    }
                                }
                                return true;
                            };

                            nlohmann::json configData = nlohmann::json::parse(fileContent, cb, true, true);
                            
                            if (!configData.is_object()) {
                                DP_LOG(Warning, "Skipping invalid config (not a JSON Object): '{}'\n", filename);
                                continue;
                            }

                            std::wstring packName = L"Default Pack";
                            if (configData.contains("PackName") && configData.at("PackName").is_string()) {
                                packName = Utils::StringToWString(configData.at("PackName").get<std::string>());
                            } else if (configData.contains("ModelPack") && configData.at("ModelPack").is_string()) {
                                packName = Utils::StringToWString(configData.at("ModelPack").get<std::string>());
                            } else {
                                packName = entry.path().stem().wstring();
                            }

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
        }

        std::map<std::wstring, std::vector<size_t>> collisionMap;
        for (size_t i = 0; i < Configs.size(); ++i) {
            auto& cfg = Configs[i];
            
            if (cfg.SwapLabel.empty() && (cfg.SkinName.empty() || cfg.SkinName == L"None")) {
                std::wstring key = cfg.PackName + L"|" + ToLower(cfg.CharacterID) + L"|" + cfg.SkelMeshPath;
                collisionMap[key].push_back(i);
            }
        }

        for (const auto& [key, indices] : collisionMap) {
            if (indices.size() > 1) { 
                auto& cfg = Configs[indices[0]];
                
                std::wstring meshName = cfg.SkelMeshPath;
                size_t lastSlash = meshName.find_last_of(L'/');
                if (lastSlash != std::wstring::npos) {
                    meshName = meshName.substr(lastSlash + 1);
                }
                size_t lastDot = meshName.find_last_of(L'.');
                if (lastDot != std::wstring::npos) {
                    meshName = meshName.substr(0, lastDot);
                }
                
                DP_LOG(Error, "JSON ERROR in Pack '{}': Found {} variants for '{}'. Please add a 'SkinLabel' property to each or they won't be able to load properly!", 
                    cfg.PackName, indices.size(), meshName);
            }
        }

        DP_LOG(Default, "Successfully loaded {} skin packs dynamically.\n", loadedPacksCount);
        DP_LOG(Default, "Complete matchmaking table compiled with {} swaps.\n", Configs.size());
    }

    void ConfigManager::ParseSwaps(const std::wstring& PackName, const nlohmann::json& swapArray) {
        for (auto& swapJson : swapArray) {
            JsonDict dict(swapJson);
            SwapConfig sc;
            sc.PackName = PackName; 
            
            if (dict.contains("CharacterID")) sc.CharacterID = Utils::StringToWString(dict.get("CharacterID").get<std::string>());
            if (dict.contains("SkelMeshPath")) sc.SkelMeshPath = Utils::StringToWString(dict.get("SkelMeshPath").get<std::string>());
            
            if (dict.contains("SkinLabel")) sc.SwapLabel = Utils::StringToWString(dict.get("SkinLabel").get<std::string>());
            else if (dict.contains("SwapLabel")) sc.SwapLabel = Utils::StringToWString(dict.get("SwapLabel").get<std::string>());
            else if (dict.contains("SwapName")) sc.SwapLabel = Utils::StringToWString(dict.get("SwapName").get<std::string>());

            ParseCommonSwapProperties(sc, dict);

            if (sc.SwapLabel.empty()) {
                sc.SwapLabel = Utils::GenerateFallbackLabel(sc.SkelMeshPath, sc.MatReplaceList, sc.MorphTargetList);
            }
            
            ValidateGender(sc.Gender, sc.PackName, sc.SwapLabel);
            Configs.push_back(sc);
        }
    }
    
    void ConfigManager::ParseSwapsV2(const std::wstring& PackName, const nlohmann::json& skinListObj) {
        for (auto& [charIdStr, skinsObj] : skinListObj.items()) {
            if (!skinsObj.is_object()) continue;
            std::wstring charID = Utils::StringToWString(charIdStr);
            
            for (auto& [skinLabelStr, swapJson] : skinsObj.items()) {
                if (!swapJson.is_object()) continue;
                JsonDict dict(swapJson);
                SwapConfig sc;
                sc.PackName = PackName;
                sc.CharacterID = charID;
                
                sc.SwapLabel = Utils::StringToWString(skinLabelStr); 

                if (dict.contains("SkinLabel")) sc.SwapLabel = Utils::StringToWString(dict.get("SkinLabel").get<std::string>());
                else if (dict.contains("SwapLabel")) sc.SwapLabel = Utils::StringToWString(dict.get("SwapLabel").get<std::string>());
                else if (dict.contains("SwapName")) sc.SwapLabel = Utils::StringToWString(dict.get("SwapName").get<std::string>());

                if (dict.contains("SkinPath")) sc.SkelMeshPath = Utils::StringToWString(dict.get("SkinPath").get<std::string>());

                ParseCommonSwapProperties(sc, dict);
                
                if (sc.SwapLabel.empty()) {
                    sc.SwapLabel = Utils::GenerateFallbackLabel(sc.SkelMeshPath, sc.MatReplaceList, sc.MorphTargetList);
                }
                
                ValidateGender(sc.Gender, sc.PackName, sc.SwapLabel);
                Configs.push_back(sc);
            }
        }
    }
    
    std::vector<SwapEvaluation> ConfigManager::EvaluateAllSwaps(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName, int Rank, int Trust, bool IsWild, const std::wstring& CurrentSwapLabel) const {
        std::vector<SwapEvaluation> results;

        for (size_t i = 0; i < Configs.size(); i++) {
            auto& swap = Configs[i];
            if (ToLower(swap.CharacterID) != ToLower(CharID)) continue;

            SwapEvaluation eval;
            eval.ConfigIndex = (int)i;
            eval.Score = 0; 
            eval.IsValid = true;

            if (Level < swap.MinLevel || Level > swap.MaxLevel) {
                eval.IsValid = false;
            } else if (swap.MinLevel > 1 || swap.MaxLevel < 999) {
                eval.Score -= 10; 
                // EVOLUTION FIX: Higher MinLevel requirements inherently score better!
                // This cleanly breaks ties between previous and next evolutions (e.g., Braixen vs Delphox)
                if (swap.MinLevel > 1) eval.Score -= swap.MinLevel;
            }

            if (eval.IsValid && (Rank < swap.MinRank || Rank > swap.MaxRank)) {
                eval.IsValid = false;
            } else if (swap.MinRank > 0 || swap.MaxRank < 5) {
                eval.Score -= 10; 
                // Higher Condensation Ranks score better (Multiplied by 5 since ranks are only 1-5)
                if (swap.MinRank > 0) eval.Score -= (swap.MinRank * 5); 
            }

            if (eval.IsValid && (Trust < swap.MinTrust || Trust > swap.MaxTrust)) {
                eval.IsValid = false;
            } else if (swap.MinTrust > 0 || swap.MaxTrust < 999999) {
                eval.Score -= 10; 
            }

            std::wstring swapGender = ToLower(swap.Gender);
            std::wstring charGender = ToLower(GenderStr);

            if (swapGender == L"any" || swapGender == L"all" || swapGender == L"both" || swapGender.empty()) {
                swapGender = L"none";
            }
            if (charGender == L"any" || charGender == L"all" || charGender == L"both" || charGender.empty()) {
                charGender = L"none";
            }

            if (eval.IsValid && swapGender != L"none") {
                if (swapGender != charGender) {
                    bool fallbackMatched = false;
                    
                    if (swapGender == L"male" && (charGender == L"futa" || charGender == L"fullfuta")) {
                        fallbackMatched = true;
                    } else if (swapGender == L"female" && (charGender == L"andro" || charGender == L"neutered" || charGender == L"fullneutered")) {
                        fallbackMatched = true;
                    }
                    
                    if (!fallbackMatched) {
                        eval.IsValid = false;
                    }
                }
            }

            if (eval.IsValid && !swap.SkinName.empty()) {
                if (ToLower(SkinName) != ToLower(swap.SkinName)) eval.IsValid = false;
                else eval.Score -= 50; 
            }

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

            if (eval.IsValid && !swap.ReqSwap.empty()) {
                bool hasReqSwap = false;

                if (!swap.SwapLabel.empty() && ToLower(swap.SwapLabel) == ToLower(CurrentSwapLabel)) {
                    hasReqSwap = true;
                } else {
                    for (const auto& req : swap.ReqSwap) {
                        if (ToLower(req) == ToLower(CurrentSwapLabel)) {
                            hasReqSwap = true;
                            break;
                        }
                    }
                }

                if (!hasReqSwap) {
                    eval.IsValid = false;
                } else {
                    eval.Score -= 30;
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

    int ConfigManager::FindConfigIndex(const std::wstring& PackName, const std::wstring& SkinName, const std::wstring& SwapLabel, const std::wstring& SkelMeshPath, const std::wstring& CharID) const {
        if (!SwapLabel.empty()) {
            for (size_t i = 0; i < Configs.size(); ++i) {
                if (Configs[i].PackName == PackName && Configs[i].SwapLabel == SwapLabel) return (int)i;
            }
        }
        
        if (!SkinName.empty() && SkinName != L"None") {
            for (size_t i = 0; i < Configs.size(); ++i) {
                if (Configs[i].PackName == PackName && Configs[i].SkinName == SkinName) return (int)i;
            }
        }
        
        if (!CharID.empty()) {
            for (size_t i = 0; i < Configs.size(); ++i) {
                if (Configs[i].PackName == PackName && 
                    Configs[i].SkelMeshPath == SkelMeshPath && 
                    ToLower(Configs[i].CharacterID) == ToLower(CharID)) return (int)i;
            }
        }

        for (size_t i = 0; i < Configs.size(); ++i) {
            if (Configs[i].PackName == PackName && Configs[i].SkelMeshPath == SkelMeshPath) return (int)i;
        }

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
            
            if (eval.Score < bestScore) {
                bestScore = eval.Score;
                bestMatches = { eval.ConfigIndex };
            } else if (eval.Score == bestScore) {
                bestMatches.push_back(eval.ConfigIndex);
            }
        }

        if (!bestMatches.empty()) {
            if (bestMatches.size() >= 4) {
                double maxWeight = -1.0;
                int maxWeightIdx = -1;
                double secondMaxWeight = -1.0;
                int secondMaxWeightIdx = -1;

                for (int idx : bestMatches) {
                    double w = Configs[idx].SpawnWeight;
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

                if (secondMaxWeight > 0.0 && maxWeight > (2.0 * secondMaxWeight)) {
                    auto& maxConfig = Configs[maxWeightIdx];
                    std::wstring charID = maxConfig.CharacterID;

                    if (WarnedCharIDs.find(charID) == WarnedCharIDs.end()) {
                        WarnedCharIDs.insert(charID);

                        std::wstring maxName = maxConfig.SkinName.empty() ? L"Anonymous Mesh" : maxConfig.SkinName;
                        std::wstring secondName = L"Other Candidates";
                        if (secondMaxWeightIdx != -1) {
                            secondName = Configs[secondMaxWeightIdx].SkinName.empty() ? L"Anonymous Mesh" : Configs[secondMaxWeightIdx].SkinName;
                        }

                        DP_LOG(Warning, "Skin '{}' in Pack '{}' has a highly biased spawn weight ({:.2f}) compared to '{}' with {:.2f}.", 
                               maxName, maxConfig.PackName, maxWeight, secondName, secondMaxWeight);
                    }
                }
            }

            double totalWeight = 0.0;
            for (int idx : bestMatches) {
                totalWeight += Configs[idx].SpawnWeight;
            }

            if (totalWeight > 0.0) {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_real_distribution<double> dis(0.0, totalWeight);
                double randomValue = dis(gen);

                double cumulativeWeight = 0.0;
                for (int idx : bestMatches) {
                    cumulativeWeight += Configs[idx].SpawnWeight;
                    if (randomValue <= cumulativeWeight) {
                        return idx;
                    }
                }
            }
            return bestMatches[0];
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