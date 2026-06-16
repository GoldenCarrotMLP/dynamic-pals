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

inline UObject* LoadAssetSafely(const std::wstring& AssetPath) {
    UObject* KismetLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
    if (!KismetLib) return nullptr;

    UFunction* MakePathFunc = KismetLib->GetFunctionByNameInChain(STR("MakeSoftObjectPath"));
    if (!MakePathFunc) return nullptr;

    struct {
        FString Path;
        uint8_t ReturnValue[64];
    } MakePathParams{FString(AssetPath.c_str()), {0}};
    KismetLib->ProcessEvent(MakePathFunc, &MakePathParams);

    UFunction* ConvFunc = KismetLib->GetFunctionByNameInChain(STR("Conv_SoftObjPathToSoftObjRef"));
    if (!ConvFunc) return nullptr;

    struct {
        uint8_t SoftObjectPath[64];
        uint8_t ReturnValue[64];
    } ConvParams{{0}, {0}};
    memcpy(ConvParams.SoftObjectPath, MakePathParams.ReturnValue, 64);
    KismetLib->ProcessEvent(ConvFunc, &ConvParams);

    UFunction* LoadFunc = KismetLib->GetFunctionByNameInChain(STR("LoadAsset_Blocking"));
    if (!LoadFunc) return nullptr;

    struct {
        uint8_t Asset[64];
        UObject* ReturnValue;
    } LoadParams{{0}, nullptr};
    memcpy(LoadParams.Asset, ConvParams.ReturnValue, 64);
    KismetLib->ProcessEvent(LoadFunc, &LoadParams);

    return LoadParams.ReturnValue;
}