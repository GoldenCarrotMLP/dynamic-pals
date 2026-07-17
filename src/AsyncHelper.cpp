// --- START OF FILE src/AsyncHelper.cpp ---
#include "AsyncHelper.hpp"
#include "DataTypes.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>
#include <vector>
#include <string>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    // Standalone, ultra-safe pattern scanner. Bypasses UE4SS locks completely!
    std::vector<void*> AsyncHelper::FindMultiplePatterns(const std::string& patternStr) {
        std::vector<void*> results;
        uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(NULL));
        if (!base) return results;

        PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return results;

        PIMAGE_NT_HEADERS ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return results;

        size_t size = ntHeaders->OptionalHeader.SizeOfImage;

        // Parse the signature into bytes (-1 for wildcards)
        std::vector<int> pattern;
        for (size_t i = 0; i < patternStr.length(); ) {
            if (patternStr[i] == ' ') {
                i++;
            } else if (patternStr[i] == '?') {
                pattern.push_back(-1);
                i += (patternStr[i + 1] == '?' ? 2 : 1);
            } else {
                pattern.push_back(std::stoi(patternStr.substr(i, 2), nullptr, 16));
                i += 2;
            }
        }

        size_t patternSize = pattern.size();
        auto data = pattern.data();

        // Scan the entire executable memory range
        for (size_t i = 0; i < size - patternSize; i++) {
            bool found = true;
            for (size_t j = 0; j < patternSize; j++) {
                if (data[j] != -1 && base[i + j] != data[j]) {
                    found = false;
                    break;
                }
            }
            if (found) {
                results.push_back(base + i);
            }
        }

        return results;
    }

    void* AsyncHelper::FindPattern(const std::string& patternStr) {
        std::vector<void*> results = FindMultiplePatterns(patternStr);

        // Case 1: No match found
        if (results.empty()) {
            return nullptr;
        }

        // Case 2: Multiple matches found (Ambiguous pattern) - Log & Auto-Recover
        if (results.size() > 1) {
            DP_LOG(Warning, "[FindPattern] Warning: Multiple matches found for pattern scan! Auto-recovering using first match.");
            for (void* addr : results) {
                DP_LOG(Warning, "  -> Ambiguous match found at address: '{}'", addr);
            }
        }

        // Case 3: Exactly one unique match found (Success)
        return results[0];
    }

    void AsyncHelper::Initialize() {
        // Find the native AsyncTask function directly
        AsyncTaskPtr = FindPattern("48 8B C4 41 54 41 57 48 81 EC B8 00 00 00 48 89 58 08");

        if (AsyncTaskPtr) {
            DP_LOG(Default,"Successfully resolved native AsyncTask pipeline.\n");
        } else {
            DP_LOG(Error,"CRITICAL: Failed to find AsyncTask signature!\n");
        }
    }

    void AsyncHelper::AsyncTask(ENamedThreads Thread, const TUniqueFunction<void()>& Function) {
        if (!AsyncTaskPtr) return;
        
        using AsyncTaskSig = void(*)(ENamedThreads, const TUniqueFunction<void()>&);
        reinterpret_cast<AsyncTaskSig>(AsyncTaskPtr)(Thread, Function);
    }
}
// --- END OF FILE src/AsyncHelper.cpp ---