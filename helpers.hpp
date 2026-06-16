#pragma once

#include <random> // Explicitly include at the very top to prevent namespace pollution
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <filesystem>

#include <Mod/CppUserModBase.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp> 
#include <Unreal/FProperty.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp> // Required for native array parsing
#include <DynamicOutput/DynamicOutput.hpp>
#include <nlohmann/json.hpp>

using namespace RC;
using namespace RC::Unreal;

// Structural mappings matching the Lua/JSON definitions
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
    std::wstring CharacterID;
    std::wstring SkelMeshPath;
    std::wstring Gender = L"None";
    std::wstring SkinName = L"";
    int MinLevel = 1;
    int MaxLevel = 999;
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

// PalSchema technique: Custom-aligned memory structures with non-conflicting names
struct AltrSoftObjectPath {
    FName PackageName;
    FName AssetName;
    FString SubPathString;
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

// Global stable helper functions (No namespace nesting to prevent resolution errors)
inline std::wstring StringToWString(const std::string& str) {
    return std::wstring(str.begin(), str.end());
}
inline std::string WStringToString(const std::wstring& wstr) {
    return std::string(wstr.begin(), wstr.end());
}

// Convert Unreal FString to C++ std::wstring safely via character array
inline std::wstring FStringToWString(const FString& fstr) {
    const TCHAR* data = fstr.GetCharArray().GetData();
    return data ? std::wstring(data) : L"";
}

inline std::wstring GuidToWString(const FGuid& Guid) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%08X%08X%08X%08X", Guid.A, Guid.B, Guid.C, Guid.D);
    return std::wstring(buf);
}

inline FProperty* GetProperty(UObject* Object, const wchar_t* PropertyName) {
    if (!Object) return nullptr;
    auto* Class = Object->GetClassPrivate();
    if (!Class) return nullptr;
    return Class->GetPropertyByNameInChain(PropertyName);
}

template<typename T>
inline bool GetPropertyValue(UObject* Object, const wchar_t* PropertyName, T& OutValue) {
    auto* Property = GetProperty(Object, PropertyName);
    if (Property) {
        T* Ptr = Property->ContainerPtrToValuePtr<T>(Object);
        if (Ptr) {
            OutValue = *Ptr;
            return true;
        }
    }
    return false;
}

inline void CallFunction(UObject* Object, const wchar_t* FunctionName, void* Params = nullptr) {
    if (!Object) return;
    auto* Function = Object->GetFunctionByNameInChain(FunctionName);
    if (Function) {
        Object->ProcessEvent(Function, Params);
    }
}

inline std::wstring FormatAssetPath(const std::wstring& Path) {
    if (Path.empty()) return L"";
    if (Path.find(L'.') != std::wstring::npos) return Path;
    size_t lastSlash = Path.find_last_of(L'/');
    if (lastSlash != std::wstring::npos) {
        std::wstring assetName = Path.substr(lastSlash + 1);
        return Path + L"." + assetName;
    }
    return Path;
}

inline std::string ReadFileToString(const std::wstring& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

inline void ParseAssetPath(const std::wstring& Path, std::wstring& OutPackage, std::wstring& OutAsset)
{
    std::wstring formatted = FormatAssetPath(Path);
    size_t dot = formatted.find(L'.');
    if (dot != std::wstring::npos)
    {
        OutPackage = formatted.substr(0, dot);
        OutAsset = formatted.substr(dot + 1);
    }
    else
    {
        OutPackage = formatted;
        size_t lastSlash = formatted.find_last_of(L'/');
        if (lastSlash != std::wstring::npos)
        {
            OutAsset = formatted.substr(lastSlash + 1);
        }
    }
}

// Pure reflection asset loader using custom-aligned structures
inline UObject* LoadAssetSafely(const std::wstring& AssetPath)
{
    std::wstring package, asset;
    ParseAssetPath(AssetPath, package, asset);

    // Build the exact, aligned C++ memory structure for AltrSoftObjectPtr
    AltrSoftObjectPtr SoftPtr;
    SoftPtr.ObjectID.PackageName = FName(package.c_str(), FNAME_Add);
    SoftPtr.ObjectID.AssetName = FName(asset.c_str(), FNAME_Add);
    SoftPtr.ObjectID.SubPathString = FString(STR(""));

    UObject* KismetLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
    if (!KismetLib) return nullptr;

    UFunction* LoadFunc = KismetLib->GetFunctionByNameInChain(STR("LoadAsset_Blocking"));
    if (!LoadFunc) return nullptr;

    struct {
        AltrSoftObjectPtr Asset;
        UObject* ReturnValue;
    } LoadParams{SoftPtr, nullptr};
    
    KismetLib->ProcessEvent(LoadFunc, &LoadParams);

    return LoadParams.ReturnValue;
}