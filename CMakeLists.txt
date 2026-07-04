cmake_minimum_required(VERSION 3.22)
project(DynamicPals)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(RE-UE4SS)

# Common files shared by both the online and offline builds
set(COMMON_SRC_FILES 
    "src/dllmain.cpp"
    "src/HooksManager.cpp"
    "src/ConfigManager.cpp"
    "src/SaveManager.cpp"
    "src/PalProcessor.cpp"
    "src/UIManager.cpp"
    "src/NotificationManager.cpp" 
    "src/AsyncHelper.cpp"
    "src/VFXManager.cpp"
    "src/safetyhook.cpp" # Compiling your manually added safetyhook file!
)

# -------------------------------------------------------------------------
# TARGET 1: Standard Build (Compiles and Links WinINet with Updater.cpp)
# -------------------------------------------------------------------------
add_library(DynamicPals SHARED ${COMMON_SRC_FILES} "src/Updater.cpp")
target_include_directories(DynamicPals PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "RE-UE4SS/UE4SS/include"
    "RE-UE4SS/deps/first/Unreal/include"
    "RE-UE4SS/deps/first/DynamicOutput/include"
    "RE-UE4SS/deps/first/Function-Pointer-Wrapper/include"
    "RE-UE4SS/deps/first/zydis/include" # Zydis headers required by safetyhook.cpp
    "RE-UE4SS/deps/first/zydis/dependencies/zycore/include" # Zycore headers required by Zydis
)
# Linking Zydis which is compiled as part of the RE-UE4SS subdirectory
target_link_libraries(DynamicPals PUBLIC UE4SS Zydis wininet)

# -------------------------------------------------------------------------
# TARGET 2: Offline Build (Compiles dummy Updater_Offline.cpp - No WinINet)
# -------------------------------------------------------------------------
add_library(DynamicPals_Offline SHARED ${COMMON_SRC_FILES} "src/Updater_Offline.cpp")
target_include_directories(DynamicPals_Offline PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "RE-UE4SS/UE4SS/include"
    "RE-UE4SS/deps/first/Unreal/include"
    "RE-UE4SS/deps/first/DynamicOutput/include"
    "RE-UE4SS/deps/first/Function-Pointer-Wrapper/include"
    "RE-UE4SS/deps/first/zydis/include" # Zydis headers required by safetyhook.cpp
    "RE-UE4SS/deps/first/zydis/dependencies/zycore/include" # Zycore headers required by Zydis
)
# Linking Zydis which is compiled as part of the RE-UE4SS subdirectory
target_link_libraries(DynamicPals_Offline PUBLIC UE4SS Zydis)

# =========================================================================
# BUILD ORDER DEPENDENCIES (Prevents Parallel Compilation Race Conditions)
# =========================================================================
# Force the Zydis target to finish compiling before either mod target tries to link
add_dependencies(DynamicPals Zydis)
add_dependencies(DynamicPals_Offline Zydis)

# Force offline build to compile before the Standard target's automation script copies them
add_dependencies(DynamicPals DynamicPals_Offline)

# FORCE SYNCHRONOUS PDB WRITES (Fixes C1041 Compiler Locks during parallel builds)
if(MSVC)
    target_compile_options(DynamicPals PRIVATE /FS)
    target_link_options(DynamicPals PRIVATE "/FORCE:MULTIPLE")
    
    target_compile_options(DynamicPals_Offline PRIVATE /FS)
    target_link_options(DynamicPals_Offline PRIVATE "/FORCE:MULTIPLE")
endif()

# =========================================================================
# AUTOMATION: Executed on post-build to copy dll, run scripts, and zip releases
# =========================================================================
add_custom_command(TARGET DynamicPals POST_BUILD
    # 1. Copy the standard DLL
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DynamicPals>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main.dll"
    
    # 2. Copy the offline-safe DLL
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DynamicPals_Offline>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main_offline.dll"
    
    # 3. Run the PowerShell template parser (Generates V1 & V2 JSON Templates)
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake_scripts/generate_templates.ps1" -SourceDir "${CMAKE_CURRENT_SOURCE_DIR}"
    
    # 4. Query Git and write/upsert the version.txt file
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake_scripts/write_version.ps1" -SourceDir "${CMAKE_CURRENT_SOURCE_DIR}"
    
    # 5. Pack and stage everything into production-ready ZIP releases in the root directory
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake_scripts/package_release.ps1" -SourceDir "${CMAKE_CURRENT_SOURCE_DIR}"
    
    COMMENT "Copying DLLs, generating V1 and V2 JSON templates, updating version.txt, and packaging ZIP releases."
)