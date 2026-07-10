#pragma once
#include <Unreal/Core/Templates/Function.hpp>
#include <string>

namespace DynPals {
    enum class ENamedThreads : int64_t {
        GameThread = 2,
        AnyThread = 255
    };

    class AsyncHelper {
    public:
        // Scans RAM for the native function on startup
        static void Initialize();

        // Queues a lambda to run securely on the designated thread
        static void AsyncTask(ENamedThreads Thread, const RC::Unreal::TUniqueFunction<void()>& Function);
        
        // Expose FindPattern publicly
        static void* FindPattern(const std::string& patternStr);
        
    private:
        static inline void* AsyncTaskPtr = nullptr;
    };
}