#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional> 
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/FString.hpp> 
#include <Unreal/NameTypes.hpp> 
#include <Unreal/FText.hpp> 
#include <DynamicOutput/DynamicOutput.hpp>
#include <fmt/format.h> 

namespace DynPals {
    void EnqueueUIToast(const std::wstring& Message, uint8_t PriorityType, uint8_t ToneType);

    struct DynPalsGuid {
        uint32_t A, B, C, D;
        
        bool IsValid() const {
            return A != 0 || B != 0 || C != 0 || D != 0;
        }
    };
}

#define DP_LOG(Level, Format, ...) \
    do { \
        RC::Output::send<RC::LogLevel::Level>(STR("[DynPals] " Format"\n") __VA_OPT__(,) __VA_ARGS__); \
        if constexpr (RC::LogLevel::Level == RC::LogLevel::Error) { \
            DynPals::EnqueueUIToast( \
                fmt::format(STR("[DynPals] " Format"\n") __VA_OPT__(,) __VA_ARGS__), \
                3, \
                1 \
            ); \
        } else if constexpr (RC::LogLevel::Level == RC::LogLevel::Warning) { \
            DynPals::EnqueueUIToast( \
                fmt::format(STR("[DynPals] " Format"\n") __VA_OPT__(,) __VA_ARGS__), \
                2, \
                1 \
            ); \
        } else if constexpr (RC::LogLevel::Level == RC::LogLevel::Normal) { \
            DynPals::EnqueueUIToast( \
                fmt::format(STR("[DynPals] " Format"\n") __VA_OPT__(,) __VA_ARGS__), \
                1, \
                1 \
            ); \
        } \
    } while(0)

using GenderType = std::wstring; 
using MorphType = std::wstring;  

struct FLinearColor_UE5 {
    float R, G, B, A;
};
struct MatReplace {
    std::string index;
    std::wstring matPath;
    bool bRandomHue = false;
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
    std::wstring SwapLabel = L"";  // UI Display Name
    std::wstring SetNickname = L"";
    int MinLevel = 1;
    int MaxLevel = 999;
    int MinTrust = 0;        
    int MaxTrust = 999999;   
    int MinRank = 0;         
    int MaxRank = 5;         
    double SpawnWeight = 1;          
    std::optional<bool> IsRarePal; 
    std::optional<bool> IsWildPal; 
    std::vector<std::wstring> ReqSwap;
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

struct FPalInstanceID {
        DynPals::DynPalsGuid PlayerUId;
        DynPals::DynPalsGuid InstanceId;
        DynPals::DynPalsGuid DebugId; // Alignment padding to match the engine's 3-GUID structure
    };


struct PalPersistData {
    std::wstring InstanceID;
    std::wstring PackName;
    std::wstring SkinName;
    std::wstring SwapLabel;
    std::wstring SkelMeshPath;
    std::map<std::wstring, double> MorphSet;
    std::map<std::string, std::wstring> MatSet;
    std::map<std::string, FLinearColor_UE5> MatColorSet;
    bool bIsManuallyLocked = false;
    
    bool HasSavedSwap() const {
        return !SkelMeshPath.empty() || !SkinName.empty();
    }
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
namespace DynPals {
    enum class EPalLogPriority : uint8_t { 
        None = 0, Normal = 1, Important = 2, VeryImportant = 3 
    };

    enum class EPalLogContentToneType : uint8_t { 
        Normal = 0, Negative = 1, Positive = 2 
    };

    struct FPalStaticItemIdAndNum {
        RC::Unreal::FName ItemId;
        int32_t Num = 0;
    };

    #pragma pack(push, 1)
    struct FPalLogAdditionalData {
        RC::Unreal::TArray<void*> SoftTextures; 
        uint8_t LogToneType;                    
        uint8_t Pad1[3];                        
        RC::Unreal::FName DefaultFontStyleName; 
        uint8_t Pad2[4];                        
        RC::Unreal::UClass* OverrideWidgetClass;
        FPalStaticItemIdAndNum ItemIDAndNum;    
        uint8_t Pad3[4];                        
    };
    #pragma pack(pop)

    struct FPalAddLogParams {
        uint8_t Priority;                       
        uint8_t Pad1[7];                        
        RC::Unreal::FText Text;                 
        FPalLogAdditionalData AdditionalData;   
        DynPalsGuid ReturnValue;                
    };
}