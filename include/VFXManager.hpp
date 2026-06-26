#pragma once
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <functional>
#include <Unreal/UObjectGlobals.hpp>

namespace DynPals {

    struct VFXTimelineEvent {
        float TriggerTime; 
        std::function<void()> Action;
        bool bHasTriggered = false;
    };

    struct VFXComposerTask {
        RC::Unreal::UObject* TargetPal;
        std::vector<VFXTimelineEvent> Events;
        std::chrono::steady_clock::time_point StartTime;
        bool bIsComplete = false;
    };

    struct VFXEventTemplate {
        float Time;
        std::wstring VfxPath;
        float Duration = -1.0f;
        std::wstring Socket = L"None";
        float Scale = 1.0f;
        float ZOffset = 0.0f; // 0.0 = Center, -1.0 = Feet, 1.0 = Head
    };

    struct VFXComposition {
        float SwapTime = 0.0f;
        std::vector<VFXEventTemplate> Events;
    };

    class VFXManager {
    public:
        static VFXManager& Get() {
            static VFXManager instance;
            return instance;
        }

        // Reverted to 0 parameters (calculates paths internally!)
        void Initialize();
        void LoadCompositions(bool bForceReload = false);
        void CycleNext();
        void CyclePrevious();
        void Tick();

        float PlayAnimMontage(RC::Unreal::UObject* Character, RC::Unreal::UObject* MontageAsset, float PlayRate = 1.0f);
        void AddComposerTask(RC::Unreal::UObject* PalActor, const std::vector<VFXTimelineEvent>& Events);
        // Returns the swapTime so PalProcessor knows how long to wait
        float PlayComposition(RC::Unreal::UObject* PalActor, const std::wstring& CompName);

        void PlaySwapEffect(RC::Unreal::UObject* PalActor, const std::wstring& VfxPath, float ZOffset = -1.0f);
        RC::Unreal::UObject* AttachVFXToPal(RC::Unreal::UObject* PalActor, const std::wstring& VfxPath, const std::wstring& SocketName = L"None", float ScaleMult = 1.0f, float ZOffsetMult = 0.0f);

    private:
        VFXManager() = default;
        VFXManager(const VFXManager&) = delete;
        VFXManager& operator=(const VFXManager&) = delete;

        void SpawnCurrentPreview();
        void KillCurrentPreview();

        std::wstring ModsPath;
        std::vector<std::wstring> VFXList;
        int CurrentIndex = 0;
        
        RC::Unreal::UObject* ActivePreviewComponent = nullptr;
        std::chrono::steady_clock::time_point SpawnTime;

        std::vector<VFXComposerTask> ActiveTasks;
        bool bHotReloadCompositions = false;
        
        std::map<std::wstring, VFXComposition> Compositions;
    };
}