#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional> 
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/FString.hpp> 
#include <Unreal/NameTypes.hpp> 
#include <DynamicOutput/DynamicOutput.hpp>

// OKAETSU FIX: Safely handles empty arguments using __VA_OPT__
#define DP_LOG(Level, Format, ...) RC::Output::send<RC::LogLevel::Level>(STR("[DynPals] " Format) __VA_OPT__(,) __VA_ARGS__)

namespace DynPals {
    struct DynPalsGuid {
        uint32_t A, B, C, D;
        
        // INSTANT ZERO-CHECK (No String Conversion Needed!)
        bool IsValid() const {
            return A != 0 || B != 0 || C != 0 || D != 0;
        }
    };
}

using GenderType = std::wstring; // OPTIONS: Male, Female, None, Futa, FullFuta, Andro, Neutered, FullNeutered
using MorphType = std::wstring;  // OPTIONS: Restrict, Free, None

struct MatReplace {
    std::string index;
    std::wstring matPath;
};

struct MorphTarget {
    std::wstring target;
    double setVal = -1000.0;
    double minVal = 0.0;
    double maxVal = 1.0;
    MorphType type = L"None"; 
};

struct SwapConfig {
    std::wstring PackName = L"Default Pack";
    std::wstring CharacterID;
    std::wstring SkelMeshPath;
    std::wstring AnimTarget = L"";
    GenderType Gender = L"None"; 
    std::wstring SkinName = L"";
    int MinLevel = 1;
    int MaxLevel = 999;
    int MinTrust = 0;        
    int MaxTrust = 999999;   
    int MinRank = 0;         
    int MaxRank = 5;         
    int SpawnWeight = 1;          
    std::optional<bool> IsRarePal; 
    std::optional<bool> IsWildPal; 
    std::vector<std::wstring> ReqTrait;
    std::vector<std::wstring> PrefTrait;
    std::vector<std::wstring> SkipTrait;
    std::vector<MatReplace> MatReplaceList;
    std::vector<MorphTarget> MorphTargetList;
    std::wstring Extra = L"{}";  
};

struct SwapEvaluation {
    int ConfigIndex;
    bool IsValid;
    int Score;
};

struct PalPersistData {
    std::wstring InstanceID;
    int SwapIndex = -1;
    std::map<std::wstring, double> MorphSet;
};

struct AltrSoftObjectPath {
    RC::Unreal::FName PackageName;
    RC::Unreal::FName AssetName;
    RC::Unreal::FString SubPathString;
};

struct AltrWeakObjectPtr {
    int32_t ObjectIndex = 0;
    int32_t ObjectSerialNumber = 0;
};

// Alignment and packing safety 
#pragma pack(push, 1)
struct AltrSoftObjectPtr {
    AltrWeakObjectPtr WeakPtr;
    int32_t TagAtLastTest = 0;
    int32_t Padding = 0;
    AltrSoftObjectPath ObjectID;
};
#pragma pack(pop)

struct FVector_UE5 {
    double X, Y, Z;
};