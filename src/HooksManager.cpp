// --- START OF FILE src/HooksManager.cpp ---
#define NOMINMAX
#include <Windows.h>

#include <Zydis/Zydis.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <chrono>
#include <fstream>
#include <safetyhook.hpp>
#include <thread>

#include "AsyncHelper.hpp"
#include "HooksManager.hpp"
#include "NotificationManager.hpp"
#include "PalProcessor.hpp"
#include "SaveManager.hpp"
#include "UI/UIRegistry.hpp"
#include "UI/Views/UIManager.hpp"
#include "Updater.hpp"
#include "Utils.hpp"
#include "VFXManager.hpp"
#include "../include/NativeAsyncLoader.hpp" 

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

static bool bCompletedInitReady = false;

static UObject* LastPlayerController = nullptr;

static UObject* LastWorld = nullptr;

static bool bIsAtMenu = false;

static SafetyHookInline Hook_MasterWazaUpdate;

static SafetyHookInline Hook_OnUpdateCharacterRank;

static SafetyHookInline Hook_AddFriendship;

static void* ResolveNativeFromThunk(void* ThunkAddress) {
    if (!ThunkAddress) return nullptr;

    ZyanStatus status;
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        return nullptr;
    }

    ZyanUSize offset = 0;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    std::vector<void*> calls;

    const uintptr_t thunkStart = reinterpret_cast<uintptr_t>(ThunkAddress);
    const uintptr_t thunkEnd = thunkStart + 500; 

    while (offset < 500) {
        status = ZydisDecoderDecodeFull(&decoder, reinterpret_cast<uint8_t*>(ThunkAddress) + offset, 500 - offset, &instruction, operands);
        if (!ZYAN_SUCCESS(status)) break;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_RET) break;

        if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
            if (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uintptr_t rip = thunkStart + offset + instruction.length;
                uintptr_t target = rip + operands[0].imm.value.s;
                calls.push_back(reinterpret_cast<void*>(target));
            }
        }
        else if (instruction.mnemonic == ZYDIS_MNEMONIC_JMP) {
            if (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uintptr_t rip = thunkStart + offset + instruction.length;
                uintptr_t target = rip + operands[0].imm.value.s;

                if (target < thunkStart || target > thunkEnd) {
                    calls.push_back(reinterpret_cast<void*>(target));
                    break; 
                }
            }
        }

        offset += instruction.length;
    }

    if (calls.empty()) return nullptr;

    void* lastCallTarget = calls.back();

    status = ZydisDecoderDecodeFull(&decoder, lastCallTarget, 16, &instruction, operands);
    if (ZYAN_SUCCESS(status)) {
        if (instruction.mnemonic == ZYDIS_MNEMONIC_CMP && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_RCX) {
            if (calls.size() > 1) {
                return calls[calls.size() - 2];
            }
        }
    }

    if (calls.size() > 1 && calls.front() == lastCallTarget) {
        return nullptr; 
    }

    return lastCallTarget;
}


static void* GetNativeAddress(const wchar_t* FunctionPath) {
  UFunction* FuncObj = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, FunctionPath);

  if (!FuncObj) return nullptr;

  void* ThunkAddr =
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(FuncObj) + 0xD8);

  void* NativeAddr = ResolveNativeFromThunk(ThunkAddr);

  if (NativeAddr) {
    return NativeAddr;
  }

  return ThunkAddr;
}

void __fastcall NativeMasterWazaUpdate_Hook(UObject* This, int32_t AddLevel,
                                            int32_t NowLevel) {
  Hook_MasterWazaUpdate.call<void, UObject*, int32_t, int32_t>(This, AddLevel,
                                                               NowLevel);

  if (This) {
    std::wstring actorName = This->GetName();

    DP_LOG(Default,
           "[Native Hook] Pal {} Leveled Up to {}! Checking evolution...",
           actorName, NowLevel);

    PalProcessor::Get().ProcessPal(This, false);
  }
}

void __fastcall NativeOnUpdateCharacterRank_Hook(UObject* This, int32_t NewRank,
                                                 int32_t OldRank) {
  Hook_OnUpdateCharacterRank.call<void, UObject*, int32_t, int32_t>(
      This, NewRank, OldRank);

  if (This) {
    UObject* PalActor = This->GetOuterPrivate();

    if (PalActor) {
      std::wstring actorName = PalActor->GetName();

      DP_LOG(Default,
             "[Native Hook] Pal {} Condensation Rank Up to {}! Checking "
             "evolution...",
             actorName, NewRank);

      PalProcessor::Get().ProcessPal(PalActor, false);
    }
  }
}

void __fastcall NativeAddFriendship_Hook(UObject* This, int32_t Value,
                                         bool bApplyPassiveSkill) {
  Hook_AddFriendship.call<void, UObject*, int32_t, bool>(This, Value,
                                                         bApplyPassiveSkill);

  if (This) {
    UObject* PalActor = nullptr;

    Utils::GetPropertyValue<UObject*>(This, STR("IndividualActor"), PalActor);

    if (PalActor) {
      std::wstring actorName = PalActor->GetName();

      DP_LOG(Default,
             "[Native Hook] Pal {} Friendship updated! Checking evolution...",
             actorName);

      PalProcessor::Get().ProcessPal(PalActor, false);
    }
  }
}

static void OnStartedWorldAutoSave(UnrealScriptFunctionCallableContext&,
                                   void*) {
  DP_LOG(Default, "Auto-Save triggered! Synchronizing world persistence...\n");

  SaveManager::Get().SaveWorldData();
}

static void OnGameThreadTick(UnrealScriptFunctionCallableContext& Context,
                             void*) {
  static bool bIsReentrant = false;

  if (bIsReentrant) return;

  bIsReentrant = true;

  VFXManager::Get().Tick();

  NativeAsyncLoader::Tick(); // Triggers Watchdog

  // Spaced-out queue processing restored!
  static auto LastSwapTime = std::chrono::steady_clock::now();
  static int VirtualFrameCount = 0; // Restored variable
  auto Now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastSwapTime).count() >= 16) {
      LastSwapTime = Now;
      VirtualFrameCount++;
      if (VirtualFrameCount >= 6) { // ADJUST THIS TO CONTROL SPACING (e.g. 16, 30, etc.)
          VirtualFrameCount = 0;
          PalProcessor::Get().Tick();
      }
  }

  if (!UIRegistry::Get().RequiresTick()) {
    bIsReentrant = false;
    return;
  }

  static auto LastUITickTime = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastUITickTime).count() >= 16) {
    LastUITickTime = Now;


    UObject* ActorContext = Context.Context;

    if (ActorContext) {
      UObject* Level = ActorContext->GetOuterPrivate();

      UObject* World = Level ? Level->GetOuterPrivate() : nullptr;

      if (World && World == LastWorld) {
        UObject* PlayerController = nullptr;

        UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));

        if (GameplayStatics) {
          struct {
            UObject* WorldContextObject;
            int32_t PlayerIndex;
            UObject* ReturnValue;
          } GSParams{ActorContext, 0, nullptr};

          Utils::CallFunction(GameplayStatics, STR("GetPlayerController"),
                              &GSParams);

          PlayerController = GSParams.ReturnValue;
        }

        if (PlayerController) {
          UIRegistry::Get().TickAll(PlayerController);
        }
      }
    }
  }

  bIsReentrant = false;
}

static void OnWidgetAddedToViewport(
    UnrealScriptFunctionCallableContext& Context, void*) {
  if (bIsAtMenu) return;

  UObject* Widget = Context.Context;

  if (!Widget) return;

  UClass* WidgetClass = Widget->GetClassPrivate();

  if (!WidgetClass) return;

  std::wstring WidgetName = WidgetClass->GetName();

  if (WidgetName.find(L"WBP_Title") != std::wstring::npos ||

      WidgetName.find(L"WBP_Login") != std::wstring::npos)

  {
    bIsAtMenu = true;

    bCompletedInitReady = false;

    SaveManager::Get().Reset();

    NotificationManager::Get().SetReady(false);

    PalProcessor::Get().ClearAllSwappedStatus();

    UIRegistry::Get().InvalidateAllUIs();

    DP_LOG(Default,
           "Transitioned to Main Menu (Detected via '{}'). Mod entering "
           "standby mode...\n",
           WidgetName);

    std::thread([]() {
      Updater::CheckForUpdates();
    }).detach();
  }
}



static void OnOpenLevel(UnrealScriptFunctionCallableContext& Context, void*) {
  bIsAtMenu = false;
  bCompletedInitReady = false;

  NotificationManager::Get().SetReady(false);
  UIRegistry::Get().InvalidateAllUIs();
  Utils::Caches::ClearAll(); 
  NativeAsyncLoader::ClearCache(); 
}


static std::wstring GetFormattedVersionString() {
  HMODULE hModule = NULL;

  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,

                     (LPCWSTR)&GetFormattedVersionString, &hModule);

  wchar_t path[MAX_PATH];

  GetModuleFileNameW(hModule, path, MAX_PATH);

  std::wstring currentDllPath(path);

  std::wstring dllDir =
      currentDllPath.substr(0, currentDllPath.find_last_of(L"\\/") + 1);

  std::wstring versionTxtPath = dllDir + L"version.txt";

  std::ifstream file(versionTxtPath);

  if (!file.is_open()) {
    return L"v0.0.56";
  }

  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

  content.erase(0, content.find_first_not_of(" \t\r\n"));

  size_t last = content.find_last_not_of(" \t\r\n");

  if (last != std::string::npos) {
    content.erase(last + 1);
  }

  if (content.empty()) {
    return L"v0.0.56";
  }

  try {
    int versionNum = std::stoi(content);

    int major = versionNum / 1000;

    int minor = (versionNum / 100) % 10;

    int patch = versionNum % 100;

    wchar_t buf[64];

    swprintf(buf, 64, L"v%d.%d.%02d", major, minor, patch);

    return std::wstring(buf);

  } catch (...) {
    std::wstring rawVersion;

    rawVersion.assign(content.begin(), content.end());

    return L"v" + rawVersion;
  }
}

void HooksManager::OnPalSpawnedReady(
    UnrealScriptFunctionCallableContext& Context, void*) {
  UObject* PalNPC = Context.Context;
  std::wstring palName = PalNPC ? PalNPC->GetName() : L"NULL";

  auto start = std::chrono::high_resolution_clock::now();

  //DP_LOG(Default, "[Hook Monitor] OnPalSpawnedReady fired for {}", palName);

  if (!bCompletedInitReady) {
    DP_LOG(Default, "  -> Aborted: Mod is still in startup standby.");
    return;
  }

  if (PalNPC) {
    PalProcessor::Get().ProcessPal(PalNPC, false);
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

static void OnClientRestart(UnrealScriptFunctionCallableContext& Context,
                            void*) {
  UObject* PlayerController = Context.Context;

  if (!PlayerController) return;

  UObject* Level = PlayerController->GetOuterPrivate();

  UObject* CurrentWorld = Level ? Level->GetOuterPrivate() : nullptr;

  if (PlayerController != LastPlayerController || CurrentWorld != LastWorld) {
    LastPlayerController = PlayerController;

    LastWorld = CurrentWorld;

    UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));

    if (GameplayStatics) {
      struct {
        UObject* WorldContextObject;
        bool bRemovePrefixString;
        FString ReturnValue;
      } Params{PlayerController, true, FString()};

      Utils::CallFunction(GameplayStatics, STR("GetCurrentLevelName"), &Params);

      std::wstring MapName = Utils::FStringToWString(Params.ReturnValue);

      bool bIsMenu = (MapName.find(L"Title") != std::wstring::npos ||

                      MapName.find(L"Login") != std::wstring::npos ||

                      MapName.empty());

      if (bIsMenu) {
            bCompletedInitReady = false;
            NotificationManager::Get().SetReady(false);
            SaveManager::Get().Reset();
            PalProcessor::Get().ClearAllSwappedStatus();
            Utils::Caches::ClearAll(); 
            NativeAsyncLoader::ClearCache();

          } else {
            bIsAtMenu = false;

            bCompletedInitReady = false;

            NotificationManager::Get().SetReady(false);
            SaveManager::Get().Reset();
            PalProcessor::Get().ClearAllSwappedStatus();
            Utils::Caches::ClearAll(); 
            NativeAsyncLoader::ClearCache();

            DP_LOG(Default,
                   "New Session Detected (Map: '{}'). Standby active. Waiting 8 seconds for level load...\n",
                   MapName);


        std::thread([]() {
          std::this_thread::sleep_for(std::chrono::seconds(8)); 

          AsyncHelper::AsyncTask(ENamedThreads::GameThread, []() {
            DP_LOG(Default, "Settle period complete. Safely resolving player active party...\n");

            // --- INITIALIZE THE ASYNC LOADER SAFELY NOW ---
            NativeAsyncLoader::Initialize();

            UObject* PlayerControllerObj = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
            if (PlayerControllerObj) {
                PalProcessor::Get().ProcessPlayerParty(PlayerControllerObj);
                
                UIManager::Get().PreloadUI(PlayerControllerObj);
            }

            bCompletedInitReady = true;

            std::wstring verStr = GetFormattedVersionString();
            DP_LOG(Normal, "Welcome to dynamic pals {} - Experimental", verStr);

            NotificationManager::Get().SetReady(true);
            NotificationManager::Get().FlushQueuedToasts();
          });
        }).detach();

      }
    }
  }
}

void HooksManager::RegisterHooks() {
  UFunction* InitFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/Pal.PalNPC:OnCompletedInitParam"));

  if (InitFunc) {
    InitFunc->RegisterPostHook(OnPalSpawnedReady, nullptr);

    DP_LOG(
        Default,
        "Successfully hooked OnCompletedInitParam (Native Pipeline Active!)\n");
  }

  UFunction* RestartFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/Engine.PlayerController:ClientRestart"));

  if (RestartFunc) {
    RestartFunc->RegisterPostHook(OnClientRestart, nullptr);

    DP_LOG(Default, "Successfully hooked ClientRestart for map transitions.\n");
  }

  UFunction* ActorRotFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/Engine.Actor:K2_GetActorRotation"));

  if (ActorRotFunc) {
    ActorRotFunc->RegisterPreHook(OnGameThreadTick, nullptr);

    DP_LOG(Default,
           "Successfully hooked K2_GetActorRotation on the Game Thread.\n");
  }

  UFunction* SaveFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr,
      STR("/Script/Pal.PalSaveGameManager:StartWorldDataAutoSave"));

  if (SaveFunc) {
    SaveFunc->RegisterPostHook(OnStartedWorldAutoSave, nullptr);
  }

  uintptr_t BaseAddr = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));

  void* MasterWazaUpdateAddr = GetNativeAddress(STR("/Script/Pal.PalNPC:MasterWazaUpdateWhenLevelUp"));
        if (MasterWazaUpdateAddr) {
            Hook_MasterWazaUpdate = safetyhook::create_inline(MasterWazaUpdateAddr, NativeMasterWazaUpdate_Hook);
            DP_LOG(Default, "[Native Hook] Detoured MasterWazaUpdateWhenLevelUp dynamically!");
        } else {
            DP_LOG(Error, "Failed to dynamically resolve Native MasterWazaUpdateWhenLevelUp!");
        }


        void* SetRankAddr = AsyncHelper::FindPattern("40 53 48 83 EC 20 48 8B D9 48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 75 ?? 48 8B D0 48 8B CB 48 83 C4 20 5B");
        if (SetRankAddr) {
            Hook_OnUpdateCharacterRank = safetyhook::create_inline(SetRankAddr, NativeOnUpdateCharacterRank_Hook);
            DP_LOG(Default, "[Native Hook] Detoured OnUpdateCharacterRank via AOB!");
        } else {
            DP_LOG(Error, "Failed to resolve AOB for OnUpdateCharacterRank!");
        }

        void* FriendshipAddr = GetNativeAddress(STR("/Script/Pal.PalIndividualCharacterParameter:AddFriendShip"));
        if (FriendshipAddr) {
            DP_LOG(Default, "[Native Hook] Detoured AddFriendShip successfully!");
        } else {
            DP_LOG(Error, "Failed to resolve Native AddFriendShip!");

  }

  UFunction* AddToViewportFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToViewport"));

  if (AddToViewportFunc) {
    AddToViewportFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
  }

  UFunction* AddToPlayerScreenFunc =
      UObjectGlobals::StaticFindObject<UFunction*>(
          nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToPlayerScreen"));

  if (AddToPlayerScreenFunc) {
    AddToPlayerScreenFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
  }

  UFunction* OpenLevelFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/Engine.GameplayStatics:OpenLevel"));

  if (OpenLevelFunc) {
    OpenLevelFunc->RegisterPreHook(OnOpenLevel, nullptr);
  }

  // --- NATIVE BLUEPRINT CALLBACK HOOK (Actor:SetOwner) ---
  UFunction* SetOwnerFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/Engine.Actor:SetOwner"));

  if (SetOwnerFunc) {
    SetOwnerFunc->RegisterPreHook([](UnrealScriptFunctionCallableContext& Context, void*) {
        if (Context.Context && Context.Context->GetClassPrivate()->GetName() == L"ModActor_C") {
            UFunction* Func = Context.TheStack.Node();
            if (!Func) return;

            UObject* Requester = nullptr;
            FProperty* OwnerProp = Func->GetPropertyByNameInChain(STR("NewOwner"));
            if (OwnerProp) {
                UObject** Ptr = OwnerProp->ContainerPtrToValuePtr<UObject*>(Context.TheStack.Locals());
                if (Ptr) Requester = *Ptr;
            }

            if (Requester && Utils::IsObjectValid(Requester)) {
                // Route the completed callback into our state machine!
                NativeAsyncLoader::OnAsyncLoadComplete(Context.Context, Requester);
            }
        }
    }, nullptr);
    DP_LOG(Default, "Successfully registered native callback hook on Actor:SetOwner.");
  }
}

}  // namespace DynPals
// --- END OF FILE src/HooksManager.cpp ---