#pragma once
#include <string>
#include <vector>
#include <map>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/FString.hpp> 
#include <Unreal/NameTypes.hpp> 

struct MatReplace {
    std::string index;
    std::wstring matPath;
};

struct MorphTarget {
    std::wstring target;
    double setVal = -1000.0;
    double minVal = 0.0;
    double maxVal = 1.0;
    std::wstring type = L"None";
};

struct SwapConfig {
    std::wstring PackName = L"Default Pack";
    std::wstring CharacterID;
    std::wstring SkelMeshPath;
    std::wstring Gender = L"None";
    std::wstring SkinName = L"";
    int MinLevel = 1;
    int MaxLevel = 999;
    int MinTrust = 0;        
    int MaxTrust = 999999;   
    int MinRank = 0;         
    int MaxRank = 5;        
    std::wstring IsRarePal = L"";
    std::vector<std::wstring> ReqTrait;
    std::vector<std::wstring> PrefTrait;
    std::vector<MatReplace> MatReplaceList;
    std::vector<MorphTarget> MorphTargetList;
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

struct AltrSoftObjectPtr {
    AltrWeakObjectPtr WeakPtr;
    int32_t TagAtLastTest = 0;
    int32_t Padding = 0;
    AltrSoftObjectPath ObjectID;
};

struct FVector_UE5 {
    double X, Y, Z;
};