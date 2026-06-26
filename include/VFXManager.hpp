#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <Unreal/UObjectGlobals.hpp>

namespace DynPals {
    class VFXManager {
    public:
        static VFXManager& Get() {
            static VFXManager instance;
            return instance;
        }

        void Initialize();
        
        // Cycles the previewer forward/backward
        void CycleNext();
        void CyclePrevious();
        
        // Ticks the 10-second auto-kill timer
        void Tick();

        // Groundwork for JSON Pal attachments (Future-Proofing)
        RC::Unreal::UObject* AttachVFXToPal(RC::Unreal::UObject* PalActor, const std::wstring& VfxPath, const std::wstring& SocketName = L"None");

    private:
        VFXManager() = default;
        VFXManager(const VFXManager&) = delete;
        VFXManager& operator=(const VFXManager&) = delete;

        void SpawnCurrentPreview();
        void KillCurrentPreview();

        std::vector<std::wstring> VFXList;
        int CurrentIndex = 0;
        
        RC::Unreal::UObject* ActivePreviewComponent = nullptr;
        std::chrono::steady_clock::time_point SpawnTime;
    };
}