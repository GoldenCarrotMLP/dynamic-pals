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
#include <fmt/xchar.h>

struct SwapConfig;

// --- ADD: SwapIdentity Helper & FPalCharacterIconDataRow ---
namespace DynPals {

    struct SwapIdentity {
        static constexpr const wchar_t* Prefix = L"DynamicPals:";
        static constexpr const wchar_t* Delimiter = L"::";

        // Builds: "DynamicPals:<PackName>::<CharID>::<SwapLabel>::<ModelPath>"
        static std::wstring MakeID(const std::wstring& PackName, const std::wstring& CharID, const std::wstring& SwapLabel, const std::wstring& ModelPath) {
            return std::wstring(Prefix) + PackName + Delimiter + CharID + Delimiter + SwapLabel + Delimiter + ModelPath;
        }

        static std::wstring MakeID(const ::SwapConfig& Config);

        // Checks if a string begins with "DynamicPals:"
        static bool IsDynamicSkin(const std::wstring& SkinIdentifier) {
            return SkinIdentifier.rfind(Prefix, 0) == 0;
        }

        // Deconstructs the 4-part ID back into its source components
        static bool ParseID(const std::wstring& FullID, std::wstring& OutPackName, std::wstring& OutCharID, std::wstring& OutSwapLabel, std::wstring& OutModelPath) {
            if (!IsDynamicSkin(FullID)) return false;

            std::wstring body = FullID.substr(std::wcslen(Prefix));
            const size_t delimLen = std::wcslen(Delimiter);

            size_t d1 = body.find(Delimiter);
            if (d1 == std::wstring::npos) return false;

            size_t d2 = body.find(Delimiter, d1 + delimLen);
            if (d2 == std::wstring::npos) return false;

            size_t d3 = body.find(Delimiter, d2 + delimLen);
            if (d3 == std::wstring::npos) return false;

            OutPackName  = body.substr(0, d1);
            OutCharID    = body.substr(d1 + delimLen, d2 - (d1 + delimLen));
            OutSwapLabel = body.substr(d2 + delimLen, d3 - (d2 + delimLen));
            OutModelPath = body.substr(d3 + delimLen);
            return true;
        }
    };

    // Matches FPalCharacterIconDataRow (0x38 bytes: 8-byte FTableRowBase + 48-byte TSoftObjectPtr)
    struct FPalCharacterIconDataRow {
        uint8_t Pad_TableRowBase[0x08]; // 0x00 - FTableRowBase
        int32_t WeakPtr_ObjectIndex;    // 0x08 - FWeakObjectPtr
        int32_t WeakPtr_SerialNumber;   // 0x0C
        int32_t TagAtLastTest;          // 0x10
        int32_t Pad_0014;               // 0x14
        RC::Unreal::FName PackageName;  // 0x18 - FTopLevelAssetPath::PackageName
        RC::Unreal::FName AssetName;    // 0x20 - FTopLevelAssetPath::AssetName
        RC::Unreal::FString SubPath;    // 0x28 - FSoftObjectPath::SubPathString
    }; // Total size: 0x38

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
    std::wstring type = L"None"; 
};

struct SwapConfig {
    std::wstring PackName = L"Default Pack";
    std::wstring CharacterID;
    std::wstring SkelMeshPath;
    std::wstring AnimTarget = L"";
    std::wstring Gender = L"None"; 
    std::wstring SkinName = L"";   
    std::wstring SwapLabel = L"";  // UI Display Name
    std::wstring SetNickname = L"";
    std::wstring IconAssetPath = L"";
    int MinLevel = 1;
    int MaxLevel = 999;
    int MinTrust = 0;        
    int MaxTrust = 999999;   
    int MinRank = 0;         
    int MaxRank = 999; // <--- Changed from 5 to 999 for modded rank compatibility
    double SpawnWeight = 1;          
    std::optional<bool> IsRarePal; 
    std::optional<bool> IsWildPal; 
    double MinSizeMultiplier = 1.0;
    double MaxSizeMultiplier = 1.0;
    std::vector<std::wstring> ReqSwap;
    std::vector<std::wstring> ReqTrait;
    std::vector<std::wstring> PrefTrait;
    std::vector<std::wstring> SkipTrait;
    std::vector<MatReplace> MatReplaceList;
    std::vector<MorphTarget> MorphTargetList;
    std::wstring Extra = L"{}";  
};

namespace DynPals {
    using SwapConfig = ::SwapConfig;
}

inline std::wstring DynPals::SwapIdentity::MakeID(const ::SwapConfig& Config) {
    return MakeID(Config.PackName, Config.CharacterID, Config.SwapLabel, Config.SkelMeshPath);
}

struct SwapEvaluation {
    int ConfigIndex;
    bool IsValid;
    int Score;
    std::wstring RejectionReason = L""; // <--- ADD THIS
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
    std::wstring OriginalSkinName; 
    std::map<std::wstring, double> MorphSet;
    std::map<std::string, std::wstring> MatSet;
    std::map<std::string, FLinearColor_UE5> MatColorSet;
    bool bIsManuallyLocked = false;
    double SizeMultiplier = -1.0;
    
    // Checks the actual composition of swap data components
    bool HasSavedSwap() const {
        return !SkelMeshPath.empty() 
            || !SkinName.empty() 
            || !MatSet.empty() 
            || !MatColorSet.empty() 
            || !MorphSet.empty();
    }

    // Returns true if explicitly locked to Vanilla
    bool IsVanillaLocked() const {
        return bIsManuallyLocked && !HasSavedSwap();
    }

    // Determines if this record should be written to disk
    bool ShouldSave() const {
        return HasSavedSwap() || bIsManuallyLocked || (SizeMultiplier > 0.0 && std::abs(SizeMultiplier - 1.0) > 0.005);
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
struct FRotator_UE5 {
    double Pitch;
    double Yaw;
    double Roll;
};
namespace DynPals {
    enum class EPalLogPriority : uint8_t { 
        None = 0, Normal = 1, Important = 2, VeryImportant = 3 
    };

    enum class EPalLogContentToneType : uint8_t { 
        Normal = 0, Negative = 1, Positive = 2 
    };

    

    

    
}