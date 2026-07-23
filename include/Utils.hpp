// --- START OF FILE include/Utils.hpp ---
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
#include <set>
#include <fstream>
#include <shared_mutex>
#include <chrono> 
#include "../include/NativeAsyncLoader.hpp"

namespace DynPals::Utils {
    using namespace RC::Unreal;

    inline UObject* GetKismetSystemLibrary();
    inline UFunction* GetKismetFunction(const wchar_t* FunctionName);

    inline void SafeProcessEvent(RC::Unreal::UObject* Obj, RC::Unreal::UFunction* Func, void* Params) {
        if (!Obj || !Func) return;
        __try {
            Obj->ProcessEvent(Func, Params);
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
    }

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
        
        // --- NEW: Tracks automatically discovered asset folders ---
        inline std::set<std::wstring> ScannedFolders;
        inline std::shared_mutex ScannedFoldersMutex;

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
            std::unique_lock<std::shared_mutex> lock9(ScannedFoldersMutex);

            PropCache.clear(); FuncCache.clear(); LibraryCache.clear();
            LibFuncCache.clear(); ClassCache.clear(); KismetFuncCache.clear(); FolderCache.clear();
            ScannedFolders.clear();
            CachedKSL = nullptr; CachedIsValidFunc = nullptr;
        }
    }

    inline UObject* GetLibrary(const wchar_t* LibraryPath) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::LibraryMutex);
            if (Caches::LibraryCache.count(LibraryPath)) return Caches::LibraryCache[LibraryPath];
        }
        UObject* Lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, LibraryPath);
        std::unique_lock<std::shared_mutex> write_lock(Caches::LibraryMutex);
        return Caches::LibraryCache[LibraryPath] = Lib;
    }

    inline UFunction* GetLibraryFunction(const wchar_t* LibraryPath, const wchar_t* FunctionName) {
        Caches::Key k{ LibraryPath, FunctionName };
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::LibFuncMutex);
            if (Caches::LibFuncCache.count(k)) return Caches::LibFuncCache[k];
        }
        UObject* Lib = GetLibrary(LibraryPath);
        UFunction* Func = Lib ? Lib->GetFunctionByNameInChain(FunctionName) : nullptr;
        std::unique_lock<std::shared_mutex> write_lock(Caches::LibFuncMutex);
        return Caches::LibFuncCache[k] = Func;
    }

    inline UClass* GetClassCached(const wchar_t* ClassPath, bool bSilenceLogs = false) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::ClassMutex);
            if (Caches::ClassCache.count(ClassPath)) return Caches::ClassCache[ClassPath];
        }
        UClass* Cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, ClassPath);
        std::unique_lock<std::shared_mutex> write_lock(Caches::ClassMutex);
        return Caches::ClassCache[ClassPath] = Cls;
    }

    inline UObject* GetKismetSystemLibrary() { return GetLibrary(STR("/Script/Engine.Default__KismetSystemLibrary")); }

    inline UFunction* GetKismetFunction(const wchar_t* FunctionName) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::KismetFuncMutex);
            if (Caches::KismetFuncCache.count(FunctionName)) return Caches::KismetFuncCache[FunctionName];
        }
        UObject* KSL = GetKismetSystemLibrary();
        UFunction* Func = KSL ? KSL->GetFunctionByNameInChain(FunctionName) : nullptr;
        std::unique_lock<std::shared_mutex> write_lock(Caches::KismetFuncMutex);
        return Caches::KismetFuncCache[FunctionName] = Func;
    }

    inline bool IsMemoryReadable(const void* ptr, size_t size) {
        if (!ptr) return false;
        __try {
            volatile const char* p = reinterpret_cast<volatile const char*>(ptr);
            char dummy1 = p[0]; char dummy2 = p[size - 1];
            (void)dummy1; (void)dummy2; 
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            return false;
        }
    }

    inline bool IsObjectValid(UObject* Obj) {
        if (!Obj) return false;
        uintptr_t addr = reinterpret_cast<uintptr_t>(Obj);
        if (addr < 0x10000ULL || (addr % 8) != 0) return false;
        if (!IsMemoryReadable(Obj, 0x18)) return false;
        void* vtable = *reinterpret_cast<void**>(Obj);
        if (!IsMemoryReadable(vtable, 8)) return false;

        if (!Caches::CachedKSL || !Caches::CachedIsValidFunc) {
            Caches::CachedKSL = GetKismetSystemLibrary();
            Caches::CachedIsValidFunc = GetKismetFunction(STR("IsValid"));
        }
        if (!Caches::CachedKSL || !Caches::CachedIsValidFunc) return false; 

        struct { UObject* Object; bool ReturnValue; } Params{ Obj, false };
        SafeProcessEvent(Caches::CachedKSL, Caches::CachedIsValidFunc, &Params);
        return Params.ReturnValue;
    }

    inline bool IsObjectTracked(UObject* TargetObj) {
        if (!TargetObj) return false;
        uintptr_t addr = reinterpret_cast<uintptr_t>(TargetObj);
        if (addr < 0x100000000ULL || (addr % 8) != 0) return false;
        bool bFound = false;
        UObjectGlobals::ForEachUObject([&](UObject* Obj, int32_t Index, int32_t SerialNumber) -> RC::LoopAction {
            if (Obj == TargetObj) { bFound = true; return RC::LoopAction::Break; }
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
        if (lastSlash != std::wstring::npos) meshName = meshName.substr(lastSlash + 1);
        size_t lastDot = meshName.find_last_of(L'.');
        if (lastDot != std::wstring::npos) meshName = meshName.substr(0, lastDot);

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

    inline std::wstring StringToWString(const std::string& str) { return std::wstring(str.begin(), str.end()); }
    inline std::string WStringToString(const std::wstring& wstr) { return std::string(wstr.begin(), wstr.end()); }
    inline std::wstring FStringToWString(const FString& fstr) { const TCHAR* data = fstr.GetCharArray().GetData(); return data ? std::wstring(data) : L""; }
    inline std::wstring GuidToWString(const DynPalsGuid& Guid) { wchar_t buf[64]; swprintf(buf, 64, L"%08X%08X%08X%08X", Guid.A, Guid.B, Guid.C, Guid.D); return std::wstring(buf); }

    inline FProperty* GetProperty(UObject* Object, const wchar_t* PropertyName, bool bSilenceLogs = false) {
        if (!Object || !IsObjectValid(Object)) return nullptr;
        auto* Class = Object->GetClassPrivate();
        if (!Class || !IsObjectValid(Class)) return nullptr;

        Caches::CacheKey key{Class, std::wstring_view(PropertyName)};
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::PropMutex);
            if (Caches::PropCache.count(key)) return Caches::PropCache[key];
        }
        FProperty* Prop = Class->GetPropertyByNameInChain(PropertyName);
        std::unique_lock<std::shared_mutex> write_lock(Caches::PropMutex);
        return Caches::PropCache[key] = Prop;
    }

    template<typename T>
    inline bool GetPropertyValue(UObject* Object, const wchar_t* PropertyName, T& OutValue, bool bSilenceLogs = false) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property) {
            T* Ptr = Property->ContainerPtrToValuePtr<T>(Object);
            if (Ptr) { OutValue = *Ptr; return true; }
        }
        return false;
    }

    template<>
    inline bool GetPropertyValue<bool>(UObject* Object, const wchar_t* PropertyName, bool& OutValue, bool bSilenceLogs) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property && Property->GetClass().GetName() == L"BoolProperty") {
            FBoolProperty* BoolProp = static_cast<FBoolProperty*>(Property);
            void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Object);
            if (Ptr) { OutValue = BoolProp->GetPropertyValue(Ptr); return true; }
        }
        return false;
    }

    template<typename T>
    inline bool SetPropertyValue(UObject* Object, const wchar_t* PropertyName, const T& Value, bool bSilenceLogs = false) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property) {
            T* Ptr = Property->ContainerPtrToValuePtr<T>(Object);
            if (Ptr) { *Ptr = Value; return true; }
        }
        return false;
    }

    template<>
    inline bool SetPropertyValue<bool>(UObject* Object, const wchar_t* PropertyName, const bool& Value, bool bSilenceLogs) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property && Property->GetClass().GetName() == L"BoolProperty") {
            FBoolProperty* BoolProp = static_cast<FBoolProperty*>(Property);
            void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Object);
            if (Ptr) { BoolProp->SetPropertyValue(Ptr, Value); return true; }
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
            if (Caches::FuncCache.count(key)) {
                Function = Caches::FuncCache[key];
                if (Function && IsObjectValid(Function)) SafeProcessEvent(Object, Function, Params);
                return;
            }
        }
        Function = Object->GetFunctionByNameInChain(FunctionName);
        {
            std::unique_lock<std::shared_mutex> write_lock(Caches::FuncMutex);
            Caches::FuncCache[key] = Function;
        }
        if (Function && IsObjectValid(Function)) SafeProcessEvent(Object, Function, Params);
    }

    inline std::wstring FormatAssetPath(const std::wstring& Path) {
        if (Path.empty() || Path.find(L'.') != std::wstring::npos) return Path;
        size_t lastSlash = Path.find_last_of(L'/');
        if (lastSlash != std::wstring::npos) return Path + L"." + Path.substr(lastSlash + 1);
        return Path;
    }

    inline std::string ReadFileToString(const std::wstring& path) {
        std::ifstream file(path);
        if (!file.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    inline UClass* GetUserWidgetClass() { return GetClassCached(STR("/Script/UMG.UserWidget")); }
    inline UObject* GetWBL() { return GetLibrary(STR("/Script/UMG.Default__WidgetBlueprintLibrary")); }
    inline UFunction* GetWBLFunction(const wchar_t* FunctionName) { return GetLibraryFunction(STR("/Script/UMG.Default__WidgetBlueprintLibrary"), FunctionName); }
    inline UObject* GetKTL() { return GetLibrary(STR("/Script/Engine.Default__KismetTextLibrary")); }
    inline UFunction* GetKTLFunction(const wchar_t* FunctionName) { return GetLibraryFunction(STR("/Script/Engine.Default__KismetTextLibrary"), FunctionName); }
    inline UObject* GetKML() { return GetLibrary(STR("/Script/Engine.Default__KismetMaterialLibrary")); }
    inline UFunction* GetKMLFunction(const wchar_t* FunctionName) { return GetLibraryFunction(STR("/Script/Engine.Default__KismetMaterialLibrary"), FunctionName); }
    inline UObject* GetKSL() { return GetLibrary(STR("/Script/Engine.Default__KismetSystemLibrary")); }
    inline UFunction* GetKSLFunction(const wchar_t* FunctionName) { return GetLibraryFunction(STR("/Script/Engine.Default__KismetSystemLibrary"), FunctionName); }


    // ==========================================
    // ZERO-STUTTER ASSET MEMORY CHECKS
    // ==========================================

    // --- THE FIX: JIT FOLDER REGISTRATION ---
    // This forces the Engine to discover exact-path materials hidden inside unmounted Mod .pak files!
    inline void RegisterAssetFolder(const std::wstring& AssetPath) {
        if (AssetPath.empty()) return;
        size_t lastSlash = AssetPath.find_last_of(L'/');
        if (lastSlash == std::wstring::npos) return;
        std::wstring FolderPath = AssetPath.substr(0, lastSlash);

        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::ScannedFoldersMutex);
            if (Caches::ScannedFolders.count(FolderPath)) return; // Already scanned
        }

        UObject* ARH = GetLibrary(STR("/Script/AssetRegistry.Default__AssetRegistryHelpers"));
        if (!ARH) return;

        struct { UObject* ReturnValue; } GetARParams{nullptr};
        CallFunction(ARH, STR("GetAssetRegistry"), &GetARParams);
        UObject* AssetRegistry = GetARParams.ReturnValue;
        if (!AssetRegistry) return;

        UFunction* ScanFunc = AssetRegistry->GetFunctionByNameInChain(STR("ScanPathsSynchronous"));
        if (ScanFunc) {
            alignas(8) uint8_t ScanBuffer[512] = {0}; 
            FString Src(FolderPath.c_str()); 
            TArray<FString> LocalPaths;
            LocalPaths.Add(Src); 
            
            FArrayProperty* InPathsProp = CastField<FArrayProperty>(ScanFunc->GetPropertyByNameInChain(STR("InPaths")));
            if (InPathsProp) {
                void* Dest = InPathsProp->ContainerPtrToValuePtr<void>(ScanBuffer);
                if (Dest) memcpy(Dest, &LocalPaths, sizeof(TArray<FString>));
            }
            
            FBoolProperty* ForceRescanProp = CastField<FBoolProperty>(ScanFunc->GetPropertyByNameInChain(STR("bForceRescan")));
            if (ForceRescanProp) ForceRescanProp->SetPropertyValue(ForceRescanProp->ContainerPtrToValuePtr<void>(ScanBuffer), true);
            
            DP_LOG(Default, "[Asset Scanner] Forcing synchronous rescan of folder to discover exact path asset: '{}'", FolderPath);
            SafeProcessEvent(AssetRegistry, ScanFunc, ScanBuffer);
        }

        std::unique_lock<std::shared_mutex> write_lock(Caches::ScannedFoldersMutex);
        Caches::ScannedFolders.insert(FolderPath);
    }

    // Core O(1) Fetch Function
    inline UObject* LoadAssetInternal(const std::wstring& AssetPath, bool bAllowBlocking = true) {
        if (AssetPath.empty()) return nullptr;

        // TIER 1: Check Requester-Specific Cache (0.001 ms)
        UObject* DirectPtr = NativeAsyncLoader::GetLoadedPointer(AssetPath);
        if (DirectPtr && IsObjectValid(DirectPtr)) {
            return DirectPtr;
        }

        // TIER 2: Check Global C++ Cache with Pointer Recycling Verification (0.001 ms)
        UObject* GlobalPtr = NativeAsyncLoader::GetGlobalPointer(AssetPath);
        if (GlobalPtr && IsObjectValid(GlobalPtr)) {
            return GlobalPtr;
        }

        // TIER 3: Check Blueprint Master Array (1.0 ms Failsafe)
        UObject* BPArrayPtr = NativeAsyncLoader::FetchFromBPMasterArray(AssetPath);
        if (BPArrayPtr && IsObjectValid(BPArrayPtr)) {
            return BPArrayPtr;
        }

        // TIER 4: Slow Native Engine Search (47.0 ms Last Resort)
        std::wstring resolvedPath = NativeAsyncLoader::ResolveCasing(AssetPath);
        std::wstring formatted = FormatAssetPath(resolvedPath); 
        
        UObject* ExistingObj = nullptr;
        if (formatted.rfind(L"/", 0) == 0) {
            ExistingObj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, formatted.c_str());
        } else {
            UObject* ANY_PACKAGE = reinterpret_cast<UObject*>(-1);
            ExistingObj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, ANY_PACKAGE, formatted.c_str());
        }

        if (ExistingObj && IsObjectValid(ExistingObj)) {
            std::wstring ClassName = ExistingObj->GetClassPrivate()->GetName();
            
            if (ClassName != L"Package") {
                if (ClassName == L"ObjectRedirector") {
                    UObject* Dest = nullptr;
                    if (GetPropertyValue<UObject*>(ExistingObj, STR("DestinationObject"), Dest)) {
                        NativeAsyncLoader::RegisterGlobalPointer(AssetPath, Dest);
                        return Dest;
                    }
                }
                DP_LOG(Default, "[Utils] Asset '{}' resolved via SLOW StaticFindObject. Re-caching globally.", AssetPath);
                NativeAsyncLoader::RegisterGlobalPointer(AssetPath, ExistingObj);
                return ExistingObj;
            }
        }

        // TIER 5: Blocking Disk Load (Only if permitted)
        if (!bAllowBlocking) return nullptr;

        DP_LOG(Default, "[Utils] Executing SLOW BLOCKING LOAD for '{}' (This causes a game hitch!)", AssetPath);

        std::wstring package, asset;
        size_t dot = formatted.find(L'.');
        if (dot != std::wstring::npos) {
            package = formatted.substr(0, dot);
            asset = formatted.substr(dot + 1);
        } else return nullptr;

        AltrSoftObjectPtr SoftPtr;
        SoftPtr.ObjectID.PackageName = FName(package.c_str(), FNAME_Add);
        SoftPtr.ObjectID.AssetName = FName(asset.c_str(), FNAME_Add);

        UObject* KismetLib = GetKismetSystemLibrary();
        UFunction* LoadFunc = KismetLib ? KismetLib->GetFunctionByNameInChain(STR("LoadAsset_Blocking")) : nullptr;
        if (!LoadFunc) return nullptr;

        alignas(8) uint8_t LoadParams[256] = {0};
        memcpy(LoadParams, &SoftPtr, sizeof(AltrSoftObjectPtr));

        SafeProcessEvent(KismetLib, LoadFunc, LoadParams);
        
        UObject* LoadedObj = nullptr;
        FProperty* RetProp = LoadFunc->GetPropertyByNameInChain(STR("ReturnValue"));
        if (RetProp) {
            UObject** RetPtr = RetProp->ContainerPtrToValuePtr<UObject*>(LoadParams);
            if (RetPtr) LoadedObj = *RetPtr;
        }

        if (LoadedObj && IsObjectValid(LoadedObj)) {
            std::wstring ClassName = LoadedObj->GetClassPrivate()->GetName();
            if (ClassName == L"ObjectRedirector") {
                UObject* Dest = nullptr;
                if (GetPropertyValue<UObject*>(LoadedObj, STR("DestinationObject"), Dest)) {
                    NativeAsyncLoader::RegisterGlobalPointer(AssetPath, Dest);
                    return Dest;
                }
            }
            NativeAsyncLoader::RegisterGlobalPointer(AssetPath, LoadedObj);
            return LoadedObj;
        }
        
        return nullptr;
    }
    inline bool IsAssetLoaded(const std::wstring& AssetPath) {
        if (AssetPath.empty()) return true;
        return LoadAssetInternal(AssetPath, false) != nullptr;
    }

    inline bool IsSkeletalMeshLoaded(const std::wstring& AssetPath) {
        if (IsAssetLoaded(AssetPath)) return true;

        if (AssetPath.find(L"/Mods/") != std::wstring::npos) {
            size_t lastSlash = AssetPath.find_last_of(L'/');
            if (lastSlash != std::wstring::npos) {
                std::wstring directory = AssetPath.substr(0, lastSlash + 1);
                std::wstring filename = AssetPath.substr(lastSlash + 1);

                if (filename.rfind(L"SK_", 0) != 0 && filename.rfind(L"sk_", 0) != 0) {
                    if (IsAssetLoaded(directory + L"SK_" + filename)) return true;
                    if (IsAssetLoaded(directory + L"sk_" + filename)) return true;
                }
            }
        }
        return false;
    }

    inline UObject* LoadAssetSafely(const std::wstring& AssetPath) {
        return LoadAssetInternal(AssetPath, true);
    }

    inline UObject* LoadSkeletalMeshSafely(const std::wstring& AssetPath) {
        UObject* Loaded = LoadAssetInternal(AssetPath, false);
        if (Loaded) return Loaded;

        std::wstring fallbackSK;
        std::wstring fallbacksk;

        if (AssetPath.find(L"/Mods/") != std::wstring::npos) {
            size_t lastSlash = AssetPath.find_last_of(L'/');
            if (lastSlash != std::wstring::npos) {
                std::wstring directory = AssetPath.substr(0, lastSlash + 1);
                std::wstring filename = AssetPath.substr(lastSlash + 1);

                if (filename.rfind(L"SK_", 0) != 0 && filename.rfind(L"sk_", 0) != 0) {
                    fallbackSK = directory + L"SK_" + filename;
                    Loaded = LoadAssetInternal(fallbackSK, false);
                    if (Loaded) return Loaded;

                    fallbacksk = directory + L"sk_" + filename;
                    Loaded = LoadAssetInternal(fallbacksk, false);
                    if (Loaded) return Loaded;
                }
            }
        }

        Loaded = LoadAssetInternal(AssetPath, true);
        if (Loaded) return Loaded;

        if (!fallbackSK.empty()) {
            Loaded = LoadAssetInternal(fallbackSK, true);
            if (Loaded) return Loaded;
            
            Loaded = LoadAssetInternal(fallbacksk, true);
            if (Loaded) return Loaded;
        }

        return nullptr;
    }

    inline std::vector<std::wstring> GetAssetsInVirtualFolder(const std::wstring& FolderPath) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::FolderMutex);
            if (Caches::FolderCache.count(FolderPath)) return Caches::FolderCache[FolderPath];
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
        if (!AssetRegistry) return Results;

        UFunction* ScanFunc = AssetRegistry->GetFunctionByNameInChain(STR("ScanPathsSynchronous"));
        if (ScanFunc) {
            alignas(8) uint8_t ScanBuffer[512] = {0}; 
            FString Src(FolderPath.c_str()); 
            TArray<FString> LocalPaths;
            LocalPaths.Add(Src); 
            
            FArrayProperty* InPathsProp = CastField<FArrayProperty>(ScanFunc->GetPropertyByNameInChain(STR("InPaths")));
            if (InPathsProp) {
                void* Dest = InPathsProp->ContainerPtrToValuePtr<void>(ScanBuffer);
                if (Dest) memcpy(Dest, &LocalPaths, sizeof(TArray<FString>));
            }
            
            FBoolProperty* ForceRescanProp = CastField<FBoolProperty>(ScanFunc->GetPropertyByNameInChain(STR("bForceRescan")));
            if (ForceRescanProp) ForceRescanProp->SetPropertyValue(ForceRescanProp->ContainerPtrToValuePtr<void>(ScanBuffer), true);
            
            SafeProcessEvent(AssetRegistry, ScanFunc, ScanBuffer);
        }

        UFunction* GetAssetsFunc = AssetRegistry->GetFunctionByNameInChain(STR("GetAssetsByPath"));
        if (!GetAssetsFunc) return Results;

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
                                    }
                                } else {
                                    Results.push_back(FullName);
                                }
                            }
                        }
                    }
                }
            } 
        }

        std::unique_lock<std::shared_mutex> write_lock(Caches::FolderMutex);
        
        // --- ADD TO SCANNED FOLDERS CACHE AUTOMATICALLY ---
        {
            std::unique_lock<std::shared_mutex> scan_lock(Caches::ScannedFoldersMutex);
            Caches::ScannedFolders.insert(FolderPath);
        }
        
        return Caches::FolderCache[FolderPath] = Results;
    }

}
// --- END OF FILE include/Utils.hpp ---