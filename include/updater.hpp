#pragma once

namespace DynPals {
    class Updater {
    public:
        // Runs on a background thread
        static void CheckForUpdates();
    };
}