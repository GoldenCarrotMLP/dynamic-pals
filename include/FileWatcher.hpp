#pragma once
#include <string>

namespace DynPals {
    class FileWatcher {
    public:
        // Starts a background thread to watch the directory for config file changes
        static void Start(const std::wstring& DirectoryPath);
    };
}