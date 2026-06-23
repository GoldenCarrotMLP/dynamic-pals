#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional> 
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/FString.hpp> 
#include <Unreal/NameTypes.hpp> 
#include <Unreal/FText.hpp> // <--- FIX: Resolves undefined RC::Unreal::FText
#include <DynamicOutput/DynamicOutput.hpp>
#include <fmt/format.h> 

namespace DynPals {
    // Forward declaration to break circular dependency
    void EnqueueUIToast(const std::wstring& Message, uint8_t PriorityType, uint8_t ToneType); // <--- FIXED

    struct DynPalsGuid {
        uint32_t A, B, C, D;
        
        // INSTANT ZERO-CHECK (No String Conversion Needed!)
        bool IsValid() const {
            return A != 0 || B != 0 || C != 0 || D != 0;
        }
    };
}

// HIJACKED DP_LOG: Compile-time optimized. Branches are discarded at compile-time for Normal/Debug logs.
#define DP_LOG(Level, Format, ...) \
    do { \
        RC::Output::send<RC::LogLevel::Level>(STR("[DynPals] " Format) __VA_OPT__(,) __VA_ARGS__); \
        if constexpr (RC::LogLevel::Level == RC::LogLevel::Error || RC::LogLevel::Level == RC::LogLevel::Warning) { \
            uint8_t priority = (RC::LogLevel::Level == RC::LogLevel::Error) ? 2 : 1; /* 3 = VeryImportant, 2 = Important */ \
            uint8_t tone = 1; /* 1 = Negative (Red/Warning color) */ \
            DynPals::EnqueueUIToast( \
                fmt::format(STR("[DynPals] " Format) __VA_OPT__(,) __VA_ARGS__), \
                priority, \
                tone \
            ); \
        } \
    } while(0)

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
    std::wstring PackName;
    std::wstring SkinName;
    std::wstring SkelMeshPath;
    std::map<std::wstring, double> MorphSet;

    // Helper to check if a swap has been assigned
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

// ============================================================================
// NATIVE PALWORLD TOAST UI MEMORY STRUCTS
// ============================================================================
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

    // CRITICAL: Perfectly byte-aligned memory layout to match Palworld's 0x38 FPalLogAdditionalData size
    #pragma pack(push, 1)
    struct FPalLogAdditionalData {
        RC::Unreal::TArray<void*> SoftTextures; // 16 bytes (0x00)
        uint8_t LogToneType;                    // 1 byte  (0x10)
        uint8_t Pad1[3];                        // 3 bytes (0x11)
        RC::Unreal::FName DefaultFontStyleName; // 8 bytes (0x14)
        uint8_t Pad2[4];                        // 4 bytes (0x1C)
        RC::Unreal::UClass* OverrideWidgetClass;// 8 bytes (0x20)
        FPalStaticItemIdAndNum ItemIDAndNum;    // 12 bytes(0x28)
        uint8_t Pad3[4];                        // 4 bytes (0x34) -> Totals 0x38 bytes!
    };
    #pragma pack(pop)

    // Parameter block for UPalLogManager::AddLog
    struct FPalAddLogParams {
        uint8_t Priority;                       // 0x00
        uint8_t Pad1[7];                        // Align FText
        RC::Unreal::FText Text;                 // 0x08
        FPalLogAdditionalData AdditionalData;   // 0x20
        DynPalsGuid ReturnValue;                // 0x58
    };
}