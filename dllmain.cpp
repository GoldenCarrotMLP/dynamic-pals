#include "helpers.hpp"

class AltermaticCpp : public CppUserModBase
{
private:
    std::wstring ConfigPath;
    std::wstring PersistFileName = L"_Altermatic_Persist_";
    std::vector<SwapConfig> Configs;
    std::map<std::wstring, PalPersistData> PersistedSwaps;
    std::wstring CurrentWorldSaveID = L"";

    static inline AltermaticCpp* Instance = nullptr;

public:
    AltermaticCpp() : CppUserModBase()
    {
        Instance = this;
        ModName = STR("AltermaticCpp");
        ModVersion = STR("1.0.0");
        ModDescription = STR("Native C++ Altermatic mesh and material swapper.");
        ModAuthors = STR("Modder");

        Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Mod DLL successfully loaded by UE4SS Mod Loader!\n"));
    }

    ~AltermaticCpp() override {}

    void LoadConfigJSONs()
    {
        std::wstring loadListPath = ConfigPath + L"_LoadList.json";
        std::string content = ReadFileToString(loadListPath);
        if (content.empty())
        {
            Output::send<LogLevel::Error>(STR("[ALTR-CPP] [FATAL] _LoadList.json is missing or unreadable!\n"));
            return;
        }

        try {
            nlohmann::json loadList = nlohmann::json::parse(content, nullptr, true, true);
            if (!loadList.contains("LoadList")) return;

            Configs.clear();
            for (auto& item : loadList.at("LoadList"))
            {
                std::string filename = item.get<std::string>();
                if (filename.empty()) continue;

                std::wstring filepath = ConfigPath + StringToWString(filename) + L".json";
                std::string fileContent = ReadFileToString(filepath);
                if (fileContent.empty()) continue;

                nlohmann::json configData = nlohmann::json::parse(fileContent, nullptr, true, true);
                if (configData.contains("SkelMeshSwap"))
                {
                    Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Loaded config '{}' with {} swaps.\n"), StringToWString(filename), configData.at("SkelMeshSwap").size());
                    for (auto& swapJson : configData.at("SkelMeshSwap"))
                    {
                        SwapConfig sc;
                        sc.CharacterID = StringToWString(swapJson.at("CharacterID").get<std::string>());
                        if (swapJson.contains("SkelMeshPath")) sc.SkelMeshPath = StringToWString(swapJson.at("SkelMeshPath").get<std::string>());
                        if (swapJson.contains("Gender")) sc.Gender = StringToWString(swapJson.at("Gender").get<std::string>());
                        if (swapJson.contains("SkinName")) sc.SkinName = StringToWString(swapJson.at("SkinName").get<std::string>());
                        if (swapJson.contains("MinLevel")) sc.MinLevel = swapJson.at("MinLevel").get<int>();
                        if (swapJson.contains("MaxLevel")) sc.MaxLevel = swapJson.at("MaxLevel").get<int>();
                        if (swapJson.contains("IsRarePal")) {
                            if (swapJson.at("IsRarePal").is_boolean()) {
                                sc.IsRarePal = swapJson.at("IsRarePal").get<bool>() ? L"true" : L"false";
                            } else {
                                sc.IsRarePal = StringToWString(swapJson.at("IsRarePal").get<std::string>());
                            }
                        }
                        if (swapJson.contains("ReqTrait")) {
                            for (auto& trait : swapJson.at("ReqTrait")) sc.ReqTrait.push_back(StringToWString(trait.get<std::string>()));
                        }
                        if (swapJson.contains("PrefTrait")) {
                            for (auto& trait : swapJson.at("PrefTrait")) sc.PrefTrait.push_back(StringToWString(trait.get<std::string>()));
                        }
                        if (swapJson.contains("MatReplace")) {
                            for (auto& mat : swapJson.at("MatReplace")) {
                                MatReplace mr;
                                mr.index = mat.at("Index").is_string() ? mat.at("Index").get<std::string>() : std::to_string(mat.at("Index").get<int>());
                                mr.matPath = StringToWString(mat.at("MatPath").get<std::string>());
                                sc.MatReplaceList.push_back(mr);
                            }
                        }
                        if (swapJson.contains("MorphTarget")) {
                            for (auto& morph : swapJson.at("MorphTarget")) {
                                MorphTarget mt;
                                mt.target = StringToWString(morph.at("Target").get<std::string>());
                                if (morph.contains("Set")) mt.setVal = morph.at("Set").get<double>();
                                if (morph.contains("Min")) mt.minVal = morph.at("Min").get<double>();
                                if (morph.contains("Max")) mt.maxVal = morph.at("Max").get<double>();
                                if (morph.contains("Type")) mt.type = StringToWString(morph.at("Type").get<std::string>());
                                sc.MorphTargetList.push_back(mt);
                            }
                        }
                        Configs.push_back(sc);
                    }
                }
            }
            Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Complete matchmaking table compiled with {} swaps.\n"), Configs.size());
        } catch (const std::exception& e) {
            Output::send<LogLevel::Error>(STR("[ALTR-CPP] Error loading JSON configs: {}\n"), RC::to_wstring(e.what()));
        }
    }

    void LoadPersistData(UObject* World)
    {
        UObject* GI = nullptr;
        GetPropertyValue<UObject*>(World, STR("OwningGameInstance"), GI);
        if (!GI) return;

        FString SaveDir;
        GetPropertyValue<FString>(GI, STR("SelectedWorldSaveDirectoryName"), SaveDir);
        std::wstring WorldSaveID = FStringToWString(SaveDir);

        if (WorldSaveID.empty() || WorldSaveID == CurrentWorldSaveID) return;

        CurrentWorldSaveID = WorldSaveID;
        Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Save Directory Detected: {}\n"), CurrentWorldSaveID);

        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";
        std::string content = ReadFileToString(persistPath);
        PersistedSwaps.clear();

        if (content.empty()) {
            Output::send<LogLevel::Normal>(STR("[ALTR-CPP] No persistence file found for this world. Starting clean.\n"));
            return;
        }

        try {
            nlohmann::json data = nlohmann::json::parse(content);
            if (data.contains("PalSwap")) {
                for (auto& item : data.at("PalSwap")) {
                    PalPersistData pd;
                    pd.InstanceID = StringToWString(item.at("InstanceID").get<std::string>());
                    pd.SwapIndex = item.at("SwapIndex").get<int>() - 1;
                    if (item.contains("MorphSet")) {
                        for (auto& [morph, val] : item.at("MorphSet").items()) {
                            pd.MorphSet[StringToWString(morph)] = val.get<double>();
                        }
                    }
                    PersistedSwaps[pd.InstanceID] = pd;
                }
                Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Loaded persistence data for {} custom Pals.\n"), PersistedSwaps.size());
            }
        } catch (...) {
            Output::send<LogLevel::Error>(STR("[ALTR-CPP] Failed to parse world persistence.\n"));
        }
    }

    void SavePersistData()
    {
        if (CurrentWorldSaveID.empty()) return;
        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";

        nlohmann::json out = nlohmann::json::object();
        nlohmann::json palSwaps = nlohmann::json::array();
        for (auto& [id, data] : PersistedSwaps) {
            nlohmann::json item = nlohmann::json::object();
            item["InstanceID"] = WStringToString(data.InstanceID);
            item["SwapIndex"] = data.SwapIndex + 1;
            nlohmann::json morphs = nlohmann::json::object();
            for (auto& [morph, val] : data.MorphSet) {
                morphs[WStringToString(morph)] = val;
            }
            item["MorphSet"] = morphs;
            palSwaps.push_back(item);
        }
        out["PalSwap"] = palSwaps;

        std::ofstream file(persistPath);
        if (file.is_open()) {
            file << out.dump(4);
            Output::send<LogLevel::Normal>(STR("[ALTR-CPP] World persistence synchronized to disk.\n"));
        }
    }

    int FindBestSwap(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName)
    {
        Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Matchmaking: Evaluating rules for {} (Lvl {} | {} | Rare: {} | Skin: {})\n"), 
            CharID, Level, GenderStr, IsRare ? STR("true") : STR("false"), SkinName);

        int bestScore = 999999;
        std::vector<int> bestMatches;

        for (size_t i = 0; i < Configs.size(); i++)
        {
            auto& swap = Configs[i];
            if (swap.CharacterID == CharID)
            {
                int score = 0;
                bool isValid = true;

                // Level Evaluation
                if (Level < swap.MinLevel || Level > swap.MaxLevel) {
                    Output::send<LogLevel::Normal>(STR("   [-] Skip Index {}: Level out of bounds (Req: {}-{})\n"), i, swap.MinLevel, swap.MaxLevel);
                    isValid = false;
                }

                // Rare Evaluation
                if (isValid && !swap.IsRarePal.empty()) {
                    bool reqRare = (swap.IsRarePal == L"true");
                    if (reqRare && !IsRare) {
                        Output::send<LogLevel::Normal>(STR("   [-] Skip Index {}: Requires Rare Pal\n"), i);
                        isValid = false;
                    } else if (!reqRare && IsRare) {
                        score += 110;
                    }
                }

                // Gender Evaluation with Fallbacks
                if (isValid && swap.Gender != L"None") {
                    if (swap.Gender != GenderStr) {
                        bool fallbackMatch = false;
                        if (swap.Gender == L"Futa" || swap.Gender == L"FullFuta") {
                            fallbackMatch = (GenderStr == L"Male");
                        } else if (swap.Gender == L"Andro" || swap.Gender == L"Neutered" || swap.Gender == L"FullNeutered") {
                            fallbackMatch = (GenderStr == L"Female");
                        }
                        if (!fallbackMatch) {
                            Output::send<LogLevel::Normal>(STR("   [-] Skip Index {}: Gender mismatch (Req: {})\n"), i, swap.Gender);
                            isValid = false;
                        }
                    }
                }

                // Skin Name Evaluation
                if (isValid) {
                    if (!swap.SkinName.empty() && SkinName != swap.SkinName) {
                        Output::send<LogLevel::Normal>(STR("   [-] Skip Index {}: SkinName mismatch (Req: {}, Pal: {})\n"), i, swap.SkinName, SkinName);
                        isValid = false;
                    } else if (swap.SkinName.empty() && !SkinName.empty()) {
                        Output::send<LogLevel::Normal>(STR("   [-] Skip Index {}: Pal has skin {}, but rule specifies default\n"), i, SkinName);
                        isValid = false;
                    }
                }

                // Required Traits
                if (isValid) {
                    for (auto& req : swap.ReqTrait) {
                        bool hasTrait = false;
                        for (auto& t : Traits) {
                            if (t == req) { hasTrait = true; break; }
                        }
                        if (!hasTrait) {
                            Output::send<LogLevel::Normal>(STR("   [-] Skip Index {}: Missing required trait '{}'\n"), i, req);
                            isValid = false;
                            break;
                        }
                    }
                }

                // Preferred Traits
                if (isValid) {
                    for (auto& pref : swap.PrefTrait) {
                        bool hasTrait = false;
                        for (auto& t : Traits) {
                            if (t == pref) { hasTrait = true; break; }
                        }
                        if (hasTrait) {
                            score -= 5;
                        } else {
                            score += 5;
                        }
                    }
                }

                if (isValid) {
                    Output::send<LogLevel::Normal>(STR("   [+] Valid Match! Index {} | Score: {}\n"), i, score);
                    if (score < bestScore) {
                        bestScore = score;
                        bestMatches = { (int)i };
                    } else if (score == bestScore) {
                        bestMatches.push_back((int)i);
                    }
                }
            }
        }

        if (!bestMatches.empty()) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, (int)bestMatches.size() - 1);
            int chosen = bestMatches[dis(gen)];
            Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Matchmaking complete. Chosen Swap Index: {}\n"), chosen);
            return chosen;
        }

        Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Matchmaking complete. No matching swaps found.\n"));
        return -1;
    }

void ApplySwap(UObject* Character, const SwapConfig& swap, PalPersistData& persist)
    {
        Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Instantiating custom assets onto character mesh...\n"));

        UObject* MeshComp = nullptr;
        CallFunction(Character, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) return;

        // 1. Swap Skeletal Mesh
        UObject* NewMesh = nullptr;
        if (!swap.SkelMeshPath.empty()) {
            NewMesh = LoadAssetSafely(FormatAssetPath(swap.SkelMeshPath));
            if (NewMesh) {
                struct { UObject* InMesh; bool bReinitPose; } MeshParams{NewMesh, true};
                CallFunction(MeshComp, STR("SetSkinnedAssetAndUpdate"), &MeshParams);
                Output::send<LogLevel::Normal>(STR("   -> Skeletal Mesh swapped successfully.\n"));
            }
        }

        // 2. Clear Old Material Overrides (Fixes the "WeaselDragon texture persisting" bug)
        CallFunction(MeshComp, STR("EmptyOverrideMaterials"));

        // 3. Reset the component's materials to the new mesh's defaults (Same as Blueprint Mod)
        if (NewMesh) {
            auto* MaterialsProp = GetProperty(NewMesh, STR("Materials"));
            if (MaterialsProp) {
                auto* ArrayProp = CastField<FArrayProperty>(MaterialsProp);
                if (ArrayProp) {
                    void* ArrayData = ArrayProp->ContainerPtrToValuePtr<void>(NewMesh);
                    auto* ScriptArray = static_cast<FScriptArray*>(ArrayData);
                    
                    if (ScriptArray && ScriptArray->Num() > 0) {
                        int32_t ElementSize = ArrayProp->GetInner()->GetElementSize();
                        uint8_t* DataPtr = static_cast<uint8_t*>(ScriptArray->GetData());
                        
                        for (int32_t i = 0; i < ScriptArray->Num(); i++) {
                            void* ElementPtr = DataPtr + (i * ElementSize);
                            // First member of FSkeletalMaterial is UMaterialInterface* (offset 0)
                            UObject* MatInterface = *reinterpret_cast<UObject**>(ElementPtr);
                            if (MatInterface) {
                                struct { int32_t ElementIndex; UObject* Material; } MatParams{i, MatInterface};
                                CallFunction(MeshComp, STR("SetMaterial"), &MatParams);
                            }
                        }
                        Output::send<LogLevel::Normal>(STR("   -> Re-initialized {} material slots to mesh defaults.\n"), ScriptArray->Num());
                    }
                }
            }
        }

        // 4. Swap Materials (Custom JSON Overrides)
        for (auto& mat : swap.MatReplaceList) {
            UObject* NewMat = LoadAssetSafely(FormatAssetPath(mat.matPath));
            if (NewMat) {
                int idx = std::stoi(mat.index);
                struct { int32_t ElementIndex; UObject* Material; } MatParams{idx, NewMat};
                CallFunction(MeshComp, STR("SetMaterial"), &MatParams);
                Output::send<LogLevel::Normal>(STR("   -> Material successfully applied to Slot {}.\n"), idx);
            }
        }

        // 5. Swap Morphs
        std::random_device rd;
        std::mt19937 gen(rd());

        for (auto& morph : swap.MorphTargetList) {
            double val = 0.0;
            auto it = persist.MorphSet.find(morph.target);
            if (it == persist.MorphSet.end()) {
                if (morph.setVal != -1000.0) {
                    val = morph.setVal;
                } else {
                    if (morph.type == L"Restrict") {
                        std::uniform_int_distribution<> dis(0, 1);
                        val = dis(gen) ? morph.maxVal : morph.minVal;
                    } else {
                        std::uniform_real_distribution<> dis(morph.minVal, morph.maxVal);
                        val = dis(gen);
                    }
                }
                persist.MorphSet[morph.target] = val;
                Output::send<LogLevel::Normal>(STR("   -> Generated Morph '{}' value: {}\n"), morph.target, val);
            } else {
                val = it->second;
                Output::send<LogLevel::Normal>(STR("   -> Loaded Morph '{}' value: {}\n"), morph.target, val);
            }

            struct { FName MorphTargetName; float Value; bool bRemoveZeroWeight; } MorphParams{FName(morph.target.c_str(), FNAME_Add), (float)val, false};
            CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
        }
    }

    void ProcessPal(UObject* Character, bool ForceReroll)
    {
        if (!Character) return;

        // Native World Resolution: Outer of Actor is Level, Outer of Level is World (Same as PalSchema)
        UObject* Level = Character->GetOuterPrivate();
        UObject* World = Level ? Level->GetOuterPrivate() : nullptr;
        if (!World) return;
        LoadPersistData(World);

        // Fetch Individual ID
        UObject* ParamComp = nullptr;
        GetPropertyValue<UObject*>(Character, STR("CharacterParameterComponent"), ParamComp);
        if (!ParamComp) return;

        UObject* IndivParam = nullptr;
        GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
        if (!IndivParam) return;

        struct FPalInstanceID {
            FGuid PlayerUId;
            FGuid InstanceId;
        } InstanceIDStruct;
        GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), InstanceIDStruct);
        std::wstring InstanceID = GuidToWString(InstanceIDStruct.InstanceId);

        // Fetch save parameter structure
        // struct FPalIndividualCharacterSaveParameter SaveParameter; starts at offset 0x388
        uint8_t* SaveParamAddress = (uint8_t*)IndivParam + 0x388;
        
        FName CharIDName = *(FName*)(SaveParamAddress + 0x0000);
        std::wstring CharID = CharIDName.ToString();

        Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Processing Character (Address: {} | ID: {} | GUID: {})\n"), 
            (void*)Character, CharID, InstanceID);

        PalPersistData ExistingPersist;
        bool hasPersist = false;
        auto pit = PersistedSwaps.find(InstanceID);
        if (pit != PersistedSwaps.end()) {
            ExistingPersist = pit->second;
            hasPersist = true;
        }

        int SwapIndex = -1;

        if (hasPersist && !ForceReroll) {
            Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Persisted pal detected. Using saved SwapIndex: {}\n"), ExistingPersist.SwapIndex);
            SwapIndex = ExistingPersist.SwapIndex;
        } else {
            // Evaluates a brand new Swap Configuration
            bool IsRare = *(bool*)(SaveParamAddress + 0x0050);
            uint8_t GenderEnum = *(uint8_t*)(SaveParamAddress + 0x0010);
            std::wstring GenderStr = L"None";
            if (GenderEnum == 1) GenderStr = L"Male";
            else if (GenderEnum == 2) GenderStr = L"Female";

            int LevelNum = *(uint8_t*)(SaveParamAddress + 0x0020);

            TArray<FName>* PassiveSkillList = (TArray<FName>*)(SaveParamAddress + 0x0090);
            std::vector<std::wstring> Traits;
            int32_t numTraits = PassiveSkillList->Num();
            if (numTraits > 0 && numTraits < 100) { // Safety constraint to prevent out of bounds memory reads
                for (int32_t i = 0; i < numTraits; i++) {
                    Traits.push_back((*PassiveSkillList)[i].ToString());
                }
            }

            FName SkinNameFName = *(FName*)(SaveParamAddress + 0x024C); // Read as FName directly
            std::wstring SkinName = SkinNameFName.ToString();
            if (SkinName == L"None") SkinName = L"";

            SwapIndex = FindBestSwap(CharID, IsRare, GenderStr, Traits, LevelNum, SkinName);

            if (SwapIndex != -1) {
                if (!hasPersist) {
                    ExistingPersist = { InstanceID, SwapIndex, {} };
                    PersistedSwaps[InstanceID] = ExistingPersist;
                } else {
                    PersistedSwaps[InstanceID].SwapIndex = SwapIndex;
                }
                SavePersistData();
            }
        }

        // Safety verification: converts 0-based memory index back to 1-based config size for boundary check
        if (SwapIndex >= 0 && SwapIndex < (int)Configs.size()) {
            ApplySwap(Character, Configs[SwapIndex], PersistedSwaps[InstanceID]);
            // Save state immediately
            SavePersistData();
        }
    }

    // Direct Pre-UFunction Hook Callback
    static void OnPalInit(UnrealScriptFunctionCallableContext& Context, void* CustomData)
    {
        if (!Instance) return;

        UObject* ParamComp = Context.Context;
        if (!ParamComp) return;

        UObject* Character = ParamComp->GetOuterPrivate();
        if (Character)
        {
            UObject* Controller = nullptr;
            GetPropertyValue<UObject*>(Character, STR("Controller"), Controller);
            if (!Controller)
            {
                UObject* Owner = nullptr;
                GetPropertyValue<UObject*>(ParamComp, STR("Owner"), Owner);
                if (Owner) Character = Owner;
            }
        }

        if (!Character) return;

        // Ensure it has an AI Controller (Ignores UI, Paldeck, and Inventory phantom models)
        UObject* Controller = nullptr;
        GetPropertyValue<UObject*>(Character, STR("Controller"), Controller);
        if (!Controller) return;

        Instance->ProcessPal(Character, false);
    }

    static void OnLevelUp(UnrealScriptFunctionCallableContext& Context, void* CustomData)
    {
        if (!Instance) return;

        UObject* IndivParam = Context.Context;
        if (!IndivParam) return;

        UObject* Character = nullptr;
        GetPropertyValue<UObject*>(IndivParam, STR("IndividualActor"), Character);
        if (Character) {
            Instance->ProcessPal(Character, true);
        }
    }

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Unreal Initialized. Building configuration paths...\n"));

        // Load project paths
        UObject* KismetLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (KismetLib) {
            FString ContentDir;
            CallFunction(KismetLib, STR("GetProjectContentDirectory"), &ContentDir);
            ConfigPath = std::wstring(ContentDir.GetCharArray().GetData()) + L"Paks/~mods/SwapJSON/"; // Explicit wstring casting prevents C2110
            Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Resolved Mod Config Path: {}\n"), ConfigPath);
        }

        LoadConfigJSONs();

        // High-level C++ Hook system of UE4SS (Extremely stable & fast!)
        UFunction* TargetFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalCharacterParameterComponent:OnInitialize_AfterSetIndividualParameter"));
        if (TargetFunc)
        {
            TargetFunc->RegisterPreHook(OnPalInit, nullptr);
            Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Hooked OnInitialize_AfterSetIndividualParameter successfully!\n"));
        }

        UFunction* LevelUpFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter:UpdateLevelDelegate__DelegateSignature"));
        if (LevelUpFunc)
        {
            LevelUpFunc->RegisterPreHook(OnLevelUp, nullptr);
            Output::send<LogLevel::Normal>(STR("[ALTR-CPP] Hooked UpdateLevelDelegate__DelegateSignature successfully!\n"));
        }
    }
};

#define ALTR_CPP_MOD_API __declspec(dllexport)
extern "C"
{
    ALTR_CPP_MOD_API CppUserModBase* start_mod()
    {
        return new AltermaticCpp();
    }

    ALTR_CPP_MOD_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}