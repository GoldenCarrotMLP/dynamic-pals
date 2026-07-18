#pragma once
#define NOMINMAX
#include <Windows.h>
#include "DataTypes.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> 
#include <Unreal/FWeakObjectPtr.hpp>

#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp> 
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <fstream>
#include <shared_mutex>

namespace DynPals::Utils {
    using namespace RC::Unreal;

    inline UObject* GetKismetSystemLibrary();
    inline UFunction* GetKismetFunction(const wchar_t* FunctionName);

    // ==========================================
    // STANDALONE SEH SAFETY WRAPPER
    // ==========================================
    // Absorbs frame-perfect garbage collection crashes during fast travel
    inline void SafeProcessEvent(RC::Unreal::UObject* Obj, RC::Unreal::UFunction* Func, void* Params) {
        if (!Obj || !Func) return;
        __try {
            Obj->ProcessEvent(Func, Params);
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            // Silently drop the execution if the object became invalid mid-frame
        }
    }

    // ==========================================
    // SEAMLESS GLOBAL REFLECTION CACHE ENGINE
    // ==========================================
    namespace Caches {
        struct CacheKey {
            UClass* Cls;
            std::wstring_view Name;
            bool operator<(const CacheKey& o) const {
                if (Cls != o.Cls) return Cls < o.Cls;
                return Name < o.Name;
            }
        };

        struct Key {
            std::wstring Path;
            std::wstring Name;
            bool operator<(const Key& Other) const {
                if (Path != Other.Path) return Path < Other.Path;
                return Name < Other.Name;
            }
        };

        inline std::map<CacheKey, FProperty*> PropCache;
        inline std::shared_mutex PropMutex;

        inline std::map<CacheKey, UFunction*> FuncCache;
        inline std::shared_mutex FuncMutex;

        inline std::map<std::wstring, UObject*> LibraryCache;

        inline std::shared_mutex LibraryMutex;

        inline std::map<Key, UFunction*> LibFuncCache;
        inline std::shared_mutex LibFuncMutex;

        inline std::map<std::wstring, UClass*> ClassCache;
        inline std::shared_mutex ClassMutex;

        inline std::map<std::wstring, UFunction*> KismetFuncCache;
        inline std::shared_mutex KismetFuncMutex;

        inline std::map<std::wstring, std::vector<std::wstring>> FolderCache;
        inline std::shared_mutex FolderMutex;

        inline UObject* CachedKSL = nullptr;
        inline UFunction* CachedIsValidFunc = nullptr;

        inline void ClearAll() {
            std::unique_lock<std::shared_mutex> lock1(PropMutex);
            std::unique_lock<std::shared_mutex> lock2(FuncMutex);
            std::unique_lock<std::shared_mutex> lock4(LibraryMutex);
            std::unique_lock<std::shared_mutex> lock5(LibFuncMutex);
            std::unique_lock<std::shared_mutex> lock6(ClassMutex);
            std::unique_lock<std::shared_mutex> lock7(KismetFuncMutex);
            std::unique_lock<std::shared_mutex> lock8(FolderMutex);

            PropCache.clear();
            FuncCache.clear();
            LibraryCache.clear();
            LibFuncCache.clear();
            ClassCache.clear();
            KismetFuncCache.clear();
            FolderCache.clear();

            CachedKSL = nullptr;

            CachedIsValidFunc = nullptr;

            DP_LOG(Default, "[Cache] Cleared all global reflection, class, and asset caches successfully.");
        }
    }

    inline UObject* GetLibrary(const wchar_t* LibraryPath) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::LibraryMutex);
            auto it = Caches::LibraryCache.find(LibraryPath);
            if (it != Caches::LibraryCache.end()) return it->second;
        }
        
        UObject* Lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, LibraryPath);
        if (!Lib) {
            DP_LOG(Warning, "[Utils] GetLibrary FAILED: Could not find core library at path: '{}'", LibraryPath);
        }

        std::unique_lock<std::shared_mutex> write_lock(Caches::LibraryMutex);
        Caches::LibraryCache[LibraryPath] = Lib;
        return Lib;
    }

    inline UFunction* GetLibraryFunction(const wchar_t* LibraryPath, const wchar_t* FunctionName) {
        Caches::Key k{ LibraryPath, FunctionName };

        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::LibFuncMutex);
            auto it = Caches::LibFuncCache.find(k);
            if (it != Caches::LibFuncCache.end()) return it->second;
        }

        UObject* Lib = GetLibrary(LibraryPath);
        UFunction* Func = Lib ? Lib->GetFunctionByNameInChain(FunctionName) : nullptr;
        
        if (!Func) {
            DP_LOG(Warning, "[Utils] GetLibraryFunction FAILED: Could not find function '{}' in library '{}'", FunctionName, LibraryPath);
        }

        std::unique_lock<std::shared_mutex> write_lock(Caches::LibFuncMutex);
        Caches::LibFuncCache[k] = Func;
        return Func;
    }

    inline UClass* GetClassCached(const wchar_t* ClassPath) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::ClassMutex);
            auto it = Caches::ClassCache.find(ClassPath);
            if (it != Caches::ClassCache.end()) return it->second;
        }

        UClass* Cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, ClassPath);
        if (!Cls) {
            DP_LOG(Warning, "[Utils] GetClassCached FAILED: Could not find class at path: '{}'", ClassPath);
        }

        std::unique_lock<std::shared_mutex> write_lock(Caches::ClassMutex);
        Caches::ClassCache[ClassPath] = Cls;
        return Cls;
    }

    inline UObject* GetKismetSystemLibrary() {
        return GetLibrary(STR("/Script/Engine.Default__KismetSystemLibrary"));
    }

    inline UFunction* GetKismetFunction(const wchar_t* FunctionName) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::KismetFuncMutex);
            auto it = Caches::KismetFuncCache.find(FunctionName);
            if (it != Caches::KismetFuncCache.end()) return it->second;
        }

        UObject* KSL = GetKismetSystemLibrary();
        UFunction* Func = KSL ? KSL->GetFunctionByNameInChain(FunctionName) : nullptr;
        
        std::unique_lock<std::shared_mutex> write_lock(Caches::KismetFuncMutex);
        Caches::KismetFuncCache[FunctionName] = Func;
        return Func;
    }

// Lightweight memory page prober to verify a pointer is mapped and readable by the OS
inline bool IsMemoryReadable(const void* ptr, size_t size) {
    if (!ptr) return false;
    __try {
        volatile const char* p = reinterpret_cast<volatile const char*>(ptr);
        // Probe the first and last byte of the requested range
        char dummy1 = p[0];
        char dummy2 = p[size - 1];
        (void)dummy1; (void)dummy2; // Prevent compiler optimization
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
    inline bool IsObjectValid(UObject* Obj) {
    if (!Obj) return false;
    
    uintptr_t addr = reinterpret_cast<uintptr_t>(Obj);
    if (addr == 0 || (addr % 8) != 0) return false;

    // 1. Probe the object's header memory (offset 0 to 0x18) to verify it is mapped and readable
    if (!IsMemoryReadable(Obj, 0x18)) return false;

    // 2. Probe its virtual table pointer to ensure it isn't pointing to poisoned/dead memory
    void* vtable = *reinterpret_cast<void**>(Obj);
    if (!IsMemoryReadable(vtable, 8)) return false;

    // 3. Resolve native library functions
    if (!Caches::CachedKSL || !Caches::CachedIsValidFunc) {
        Caches::CachedKSL = GetKismetSystemLibrary();
        Caches::CachedIsValidFunc = GetKismetFunction(STR("IsValid"));
    }

    if (!Caches::CachedKSL || !Caches::CachedIsValidFunc) return false; 

    // 4. Safe invocation of the engine's internal IsValid check
    struct { UObject* Object; bool ReturnValue; } Params{ Obj, false };
    SafeProcessEvent(Caches::CachedKSL, Caches::CachedIsValidFunc, &Params);
    return Params.ReturnValue;
}
    inline bool IsObjectTracked(UObject* TargetObj) {
        if (!TargetObj) return false;

        uintptr_t addr = reinterpret_cast<uintptr_t>(TargetObj);
        if (addr == 0 || (addr % 8) != 0) return false;
        
        bool bFound = false;
        UObjectGlobals::ForEachUObject([&](UObject* Obj, int32_t Index, int32_t SerialNumber) -> RC::LoopAction {
            if (Obj == TargetObj) {
                bFound = true;
                return RC::LoopAction::Break;
            }
            return RC::LoopAction::Continue;
        });
        return bFound;
    }

    inline FField* GetNextField(FField* Field) {
        if (!Field) return nullptr;
        return *reinterpret_cast<FField**>(reinterpret_cast<uint8_t*>(Field) + 0x20);
    }

    inline std::wstring GenerateFallbackLabel(const std::wstring& SkelMeshPath, const std::vector<MatReplace>& MatReplaceList, const std::vector<MorphTarget>& MorphTargetList) {
        std::wstring meshName = SkelMeshPath;
        size_t lastSlash = meshName.find_last_of(L'/');
        if (lastSlash != std::wstring::npos) {
            meshName = meshName.substr(lastSlash + 1);
        }
        size_t lastDot = meshName.find_last_of(L'.');
        if (lastDot != std::wstring::npos) {
            meshName = meshName.substr(0, lastDot);
        }

        size_t hashVal = std::hash<std::wstring>{}(SkelMeshPath);
        for (const auto& mat : MatReplaceList) {
            hashVal ^= std::hash<std::string>{}(mat.index) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
            hashVal ^= std::hash<std::wstring>{}(mat.matPath) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
        }
        for (const auto& morph : MorphTargetList) {
            hashVal ^= std::hash<std::wstring>{}(morph.target) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
        }

        wchar_t buf[32];
        swprintf(buf, 32, L" (%08X)", static_cast<unsigned int>(hashVal & 0xFFFFFFFF));
        return meshName + buf;
    }

    inline std::wstring StringToWString(const std::string& str) {
        std::wstring wstr;
        wstr.reserve(str.size());
        for (char c : str) {
            wstr.push_back(static_cast<wchar_t>(c));
        }
        return wstr;
    }

    inline std::string WStringToString(const std::wstring& wstr) {
        std::string str;
        str.reserve(wstr.size());
        for (wchar_t wc : wstr) {
            str.push_back(static_cast<char>(wc));
        }
        return str;
    }

    inline std::wstring FStringToWString(const FString& fstr) {
        const TCHAR* data = fstr.GetCharArray().GetData();
        return data ? std::wstring(data) : L"";
    }

    inline std::wstring GuidToWString(const DynPalsGuid& Guid) {
        wchar_t buf[64];
        swprintf(buf, 64, L"%08X%08X%08X%08X", Guid.A, Guid.B, Guid.C, Guid.D);
        return std::wstring(buf);
    }

    inline FProperty* GetProperty(UObject* Object, const wchar_t* PropertyName, bool bSilenceLogs = false) {
        if (!Object || !IsObjectValid(Object)) return nullptr;
        auto* Class = Object->GetClassPrivate();
        if (!Class || !IsObjectValid(Class)) return nullptr;

        Caches::CacheKey key{Class, std::wstring_view(PropertyName)};
        
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::PropMutex);
            auto it = Caches::PropCache.find(key);
            if (it != Caches::PropCache.end()) return it->second;
        }

        FProperty* Prop = Class->GetPropertyByNameInChain(PropertyName);
        
        if (!Prop && !bSilenceLogs) {
            DP_LOG(Verbose, "[Utils] GetProperty: Could not find property '{}' on object '{}'", PropertyName, Object->GetName());
        }

        std::unique_lock<std::shared_mutex> write_lock(Caches::PropMutex);
        Caches::PropCache[key] = Prop;
        
        return Prop;
    }

    template<typename T>
    inline bool GetPropertyValue(UObject* Object, const wchar_t* PropertyName, T& OutValue, bool bSilenceLogs = false) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property) {
            T* Ptr = Property->ContainerPtrToValuePtr<T>(Object);
            if (Ptr) {
                OutValue = *Ptr;
                return true;
            }
        }
        return false;
    }

    template<>
    inline bool GetPropertyValue<bool>(UObject* Object, const wchar_t* PropertyName, bool& OutValue, bool bSilenceLogs) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property) {
            if (Property->GetClass().GetName() == L"BoolProperty") {
                FBoolProperty* BoolProp = static_cast<FBoolProperty*>(Property);
                void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Object);
                if (Ptr) {
                    OutValue = BoolProp->GetPropertyValue(Ptr);
                    return true;
                }
            }
        }
        return false;
    }

    template<typename T>
    inline bool SetPropertyValue(UObject* Object, const wchar_t* PropertyName, const T& Value, bool bSilenceLogs = false) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property) {
            T* Ptr = Property->ContainerPtrToValuePtr<T>(Object);
            if (Ptr) {
                *Ptr = Value;
                return true;
            }
        }
        return false;
    }

    template<>
    inline bool SetPropertyValue<bool>(UObject* Object, const wchar_t* PropertyName, const bool& Value, bool bSilenceLogs) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property) {
            if (Property->GetClass().GetName() == L"BoolProperty") {
                FBoolProperty* BoolProp = static_cast<FBoolProperty*>(Property);
                void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Object);
                if (Ptr) {
                    BoolProp->SetPropertyValue(Ptr, Value);
                    return true;
                }
            }
        }
        return false;
    }

    inline void CallFunction(UObject* Object, const wchar_t* FunctionName, void* Params = nullptr, bool bSilenceLogs = false) {
        if (!Object || !IsObjectValid(Object)) return;
        auto* Class = Object->GetClassPrivate();
        if (!Class || !IsObjectValid(Class)) return;

        Caches::CacheKey key{Class, std::wstring_view(FunctionName)};
        UFunction* Function = nullptr;
        
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::FuncMutex);
            auto it = Caches::FuncCache.find(key);
            if (it != Caches::FuncCache.end()) {
                Function = it->second;
                if (Function && IsObjectValid(Function)) SafeProcessEvent(Object, Function, Params);
                return;
            }
        }

        Function = Object->GetFunctionByNameInChain(FunctionName);
        
        if (!Function && !bSilenceLogs) {
            DP_LOG(Verbose, "[Utils] CallFunction FAILED: Could not find function '{}' on object '{}'", FunctionName, Object->GetName());
        }

        {
            std::unique_lock<std::shared_mutex> write_lock(Caches::FuncMutex);
            Caches::FuncCache[key] = Function;
        }

        if (Function && IsObjectValid(Function)) {
            SafeProcessEvent(Object, Function, Params);
        }
    }

    inline std::wstring FormatAssetPath(const std::wstring& Path) {
        if (Path.empty() || Path.find(L'.') != std::wstring::npos) return Path;
        size_t lastSlash = Path.find_last_of(L'/');
        if (lastSlash != std::wstring::npos) {
            return Path + L"." + Path.substr(lastSlash + 1);
        }
        return Path;
    }

    inline std::string ReadFileToString(const std::wstring& path) {
        std::ifstream file(path);
        if (!file.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    inline UClass* GetUserWidgetClass() {
        return GetClassCached(STR("/Script/UMG.UserWidget"));
    }
    inline UObject* GetWBL() {
        return GetLibrary(STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
    }
    inline UFunction* GetWBLFunction(const wchar_t* FunctionName) {
        return GetLibraryFunction(STR("/Script/UMG.Default__WidgetBlueprintLibrary"), FunctionName);
    }
    inline UObject* GetKTL() {
        return GetLibrary(STR("/Script/Engine.Default__KismetTextLibrary"));
    }
    inline UFunction* GetKTLFunction(const wchar_t* FunctionName) {
        return GetLibraryFunction(STR("/Script/Engine.Default__KismetTextLibrary"), FunctionName);
    }
    inline UObject* GetKML() {
        return GetLibrary(STR("/Script/Engine.Default__KismetMaterialLibrary"));
    }
    inline UFunction* GetKMLFunction(const wchar_t* FunctionName) {
        return GetLibraryFunction(STR("/Script/Engine.Default__KismetMaterialLibrary"), FunctionName);
    }
    inline UObject* GetKSL() {
        return GetLibrary(STR("/Script/Engine.Default__KismetSystemLibrary"));
    }
    inline UFunction* GetKSLFunction(const wchar_t* FunctionName) {
        return GetLibraryFunction(STR("/Script/Engine.Default__KismetSystemLibrary"), FunctionName);
    }

    inline UObject* LoadAssetInternal(const std::wstring& AssetPath) {
        std::wstring formatted = FormatAssetPath(AssetPath); 
        
        UObject* ExistingObj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, formatted.c_str());
        if (ExistingObj && IsObjectValid(ExistingObj)) {
            std::wstring ClassName = ExistingObj->GetClassPrivate()->GetName();
            if (ClassName == L"ObjectRedirector") {
                UObject* Destination = nullptr;
                if (GetPropertyValue<UObject*>(ExistingObj, STR("DestinationObject"), Destination)) {
                    return Destination;
                }
            }
            return ExistingObj; 
        }

        std::wstring package, asset;
        size_t dot = formatted.find(L'.');
        if (dot != std::wstring::npos) {
            package = formatted.substr(0, dot);
            asset = formatted.substr(dot + 1);
        } else {
            return nullptr;
        }

        AltrSoftObjectPtr SoftPtr;
        SoftPtr.ObjectID.PackageName = FName(package.c_str(), FNAME_Add);
        SoftPtr.ObjectID.AssetName = FName(asset.c_str(), FNAME_Add);
        SoftPtr.ObjectID.SubPathString = FString(STR(""));

        UObject* KismetLib = GetKismetSystemLibrary();
        if (!KismetLib) return nullptr;

        UFunction* LoadFunc = KismetLib->GetFunctionByNameInChain(STR("LoadAsset_Blocking"));
        if (!LoadFunc) return nullptr;

        // CRITICAL FIX: Use zeroed buffer and reflection to prevent Stack Misalignment garbage pointers
        alignas(8) uint8_t LoadParams[256] = {0};
        
        // The first parameter is the Asset (SoftObjectPtr). We copy our struct into the start of the buffer.
        memcpy(LoadParams, &SoftPtr, sizeof(AltrSoftObjectPtr));

        SafeProcessEvent(KismetLib, LoadFunc, LoadParams);
        
        UObject* LoadedObj = nullptr;
        FProperty* RetProp = LoadFunc->GetPropertyByNameInChain(STR("ReturnValue"));
        if (RetProp) {
            // Let Unreal Engine's reflection system locate the exact memory offset for the ReturnValue
            UObject** RetPtr = RetProp->ContainerPtrToValuePtr<UObject*>(LoadParams);
            if (RetPtr) LoadedObj = *RetPtr;
        }

        if (LoadedObj && IsObjectValid(LoadedObj)) {
            std::wstring ClassName = LoadedObj->GetClassPrivate()->GetName();
            if (ClassName == L"ObjectRedirector") {
                UObject* Destination = nullptr;
                if (GetPropertyValue<UObject*>(LoadedObj, STR("DestinationObject"), Destination)) {
                    return Destination;
                }
            }
        }
        
        return LoadedObj;
    }

    inline UObject* LoadAssetSafely(const std::wstring& AssetPath) {
        return LoadAssetInternal(AssetPath);
    }

    inline UObject* LoadSkeletalMeshSafely(const std::wstring& AssetPath) {
        UObject* Loaded = LoadAssetInternal(AssetPath);
        if (Loaded) return Loaded;

        if (AssetPath.find(L"/Mods/") != std::wstring::npos) {
            size_t lastSlash = AssetPath.find_last_of(L'/');
            if (lastSlash != std::wstring::npos) {
                std::wstring directory = AssetPath.substr(0, lastSlash + 1);
                std::wstring filename = AssetPath.substr(lastSlash + 1);

                if (filename.rfind(L"SK_", 0) != 0 && filename.rfind(L"sk_", 0) != 0) {
                    std::wstring fallback = directory + L"SK_" + filename;
                    Loaded = LoadAssetInternal(fallback);
                    if (Loaded) {
                        return Loaded;
                    }

                    fallback = directory + L"sk_" + filename;
                    Loaded = LoadAssetInternal(fallback);
                    if (Loaded) {
                        return Loaded;
                    }
                }
            }
        }
        return nullptr;
    }

    inline std::vector<std::wstring> GetAssetsInVirtualFolder(const std::wstring& FolderPath) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::FolderMutex);
            if (Caches::FolderCache.find(FolderPath) != Caches::FolderCache.end()) return Caches::FolderCache[FolderPath];
        }

        std::vector<std::wstring> Results;
        UObject* ARH = GetLibrary(STR("/Script/AssetRegistry.Default__AssetRegistryHelpers"));
        if (!ARH) {
            DP_LOG(Error, "[Asset Scanner] Failed: AssetRegistryHelpers CDO not found.");
            return Results;
        }

        struct { UObject* ReturnValue; } GetARParams{nullptr};
        CallFunction(ARH, STR("GetAssetRegistry"), &GetARParams);
        UObject* AssetRegistry = GetARParams.ReturnValue;
        if (!AssetRegistry) {
            DP_LOG(Error, "[Asset Scanner] Failed: AssetRegistry instance not found.");
            return Results;
        }

        DP_LOG(Default, "[Asset Scanner] Initializing search for folder: '{}'", FolderPath);

        UFunction* ScanFunc = AssetRegistry->GetFunctionByNameInChain(STR("ScanPathsSynchronous"));
        if (ScanFunc) {
            alignas(8) uint8_t ScanBuffer[512] = {0}; 
            
            // Construct the TArray safely in C++ space to avoid uninitialized memory corruption crashes
            FString Src(FolderPath.c_str()); 
            TArray<FString> LocalPaths;
            LocalPaths.Add(Src); 
            
            FArrayProperty* InPathsProp = CastField<FArrayProperty>(ScanFunc->GetPropertyByNameInChain(STR("InPaths")));
            if (InPathsProp) {
                void* Dest = InPathsProp->ContainerPtrToValuePtr<void>(ScanBuffer);
                // Safely transplant the perfectly constructed C++ TArray layout into the UE engine buffer
                if (Dest) memcpy(Dest, &LocalPaths, sizeof(TArray<FString>));
            }
            
            FBoolProperty* ForceRescanProp = CastField<FBoolProperty>(ScanFunc->GetPropertyByNameInChain(STR("bForceRescan")));
            if (ForceRescanProp) ForceRescanProp->SetPropertyValue(ForceRescanProp->ContainerPtrToValuePtr<void>(ScanBuffer), true);
            
            DP_LOG(Default, "[Asset Scanner] Forcing synchronous rescan of folder to register modded .pak assets...");
            SafeProcessEvent(AssetRegistry, ScanFunc, ScanBuffer);
            
            // Note: LocalPaths destructor will automatically clean up the array memory safely!
        } else {
            DP_LOG(Warning, "[Asset Scanner] ScanPathsSynchronous function missing. Unregistered .pak assets may not be found.");
        }

        UFunction* GetAssetsFunc = AssetRegistry->GetFunctionByNameInChain(STR("GetAssetsByPath"));
        if (!GetAssetsFunc) {
            DP_LOG(Error, "[Asset Scanner] Failed: GetAssetsByPath function not found.");
            return Results;
        }

        alignas(8) uint8_t ParamsBuffer[512] = {0}; 

        FProperty* PackagePathProp = GetAssetsFunc->GetPropertyByNameInChain(STR("PackagePath"));
        if (PackagePathProp) {
            FName* Dest = static_cast<FName*>(PackagePathProp->ContainerPtrToValuePtr<void>(ParamsBuffer));
            if (Dest) *Dest = FName(FolderPath.c_str(), FNAME_Add);
        }

        FBoolProperty* RecursiveProp = CastField<FBoolProperty>(GetAssetsFunc->GetPropertyByNameInChain(STR("bRecursive")));
        if (RecursiveProp) RecursiveProp->SetPropertyValue(RecursiveProp->ContainerPtrToValuePtr<void>(ParamsBuffer), false);

        FBoolProperty* OnDiskProp = CastField<FBoolProperty>(GetAssetsFunc->GetPropertyByNameInChain(STR("bIncludeOnlyOnDiskAssets")));
        if (OnDiskProp) OnDiskProp->SetPropertyValue(OnDiskProp->ContainerPtrToValuePtr<void>(ParamsBuffer), false);

        SafeProcessEvent(AssetRegistry, GetAssetsFunc, ParamsBuffer);

        FArrayProperty* OutAssetDataProp = CastField<FArrayProperty>(GetAssetsFunc->GetPropertyByNameInChain(STR("OutAssetData")));
        if (OutAssetDataProp) {
            FScriptArray* ScriptArray = static_cast<FScriptArray*>(OutAssetDataProp->ContainerPtrToValuePtr<void>(ParamsBuffer));
            FProperty* InnerProp = OutAssetDataProp->GetInner();

            if (ScriptArray && InnerProp) {
                int32_t NumAssets = ScriptArray->Num();
                DP_LOG(Default, "[Asset Scanner] Search complete. Discovered {} total assets in folder.", NumAssets);

                int32_t ElementSize = InnerProp->GetSize();
                uint8_t* ArrayData = static_cast<uint8_t*>(ScriptArray->GetData());

                UFunction* GetFullNameFunc = ARH->GetFunctionByNameInChain(STR("GetFullName"));
                
                for (int32_t i = 0; i < NumAssets; ++i) {
                    if (GetFullNameFunc) {
                        alignas(8) uint8_t FNParams[512] = {0}; 
                        
                        FProperty* InAssetDataProp = GetFullNameFunc->GetPropertyByNameInChain(STR("InAssetData"));
                        if (InAssetDataProp && ArrayData) {
                            void* Dest = InAssetDataProp->ContainerPtrToValuePtr<void>(FNParams);
                            if (Dest) memcpy(Dest, ArrayData + (i * ElementSize), InAssetDataProp->GetSize());
                        }

                        SafeProcessEvent(ARH, GetFullNameFunc, FNParams);

                        FProperty* ReturnProp = GetFullNameFunc->GetPropertyByNameInChain(STR("ReturnValue"));
                        if (ReturnProp) {
                            FString* RetStr = static_cast<FString*>(ReturnProp->ContainerPtrToValuePtr<void>(FNParams));
                            if (RetStr && RetStr->GetCharArray().GetData()) {
                                std::wstring FullName = RetStr->GetCharArray().GetData();
                                size_t SpacePos = FullName.find(L' ');
                                if (SpacePos != std::wstring::npos) {
                                    std::wstring ClassName = FullName.substr(0, SpacePos);
                                    std::wstring Path = FullName.substr(SpacePos + 1);

                                    if (ClassName.find(L"Material") != std::wstring::npos) {
                                        Results.push_back(Path);
                                        DP_LOG(Default, "  -> [Accepted Material] Class: '{}', Path: '{}'", ClassName, Path);
                                    } else {
                                        DP_LOG(Default, "  -> [Rejected Asset] Class: '{}', Path: '{}'", ClassName, Path);
                                    }
                                } else {
                                    Results.push_back(FullName);
                                }
                            }
                        }
                    }
                }
            } else {
                DP_LOG(Warning, "[Asset Scanner] Warning: Output array was structurally invalid.");
            }
        }

        std::unique_lock<std::shared_mutex> write_lock(Caches::FolderMutex);
        Caches::FolderCache[FolderPath] = Results;
        return Results;
    }
}