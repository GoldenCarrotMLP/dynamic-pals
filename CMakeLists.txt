cmake_minimum_required(VERSION 3.22)
project(DynamicPals)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to UE4SS's required custom config if none is specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Game__Shipping__Win64" CACHE STRING "Choose the type of build." FORCE)
endif()

add_subdirectory(RE-UE4SS)

# Common files shared by both the online and offline builds
set(COMMON_SRC_FILES
    "src/dllmain.cpp"
    "src/HooksManager.cpp"
    "src/ConfigManager.cpp"
    "src/SaveManager.cpp"
    "src/PalProcessor.cpp"
    "src/NotificationManager.cpp" 
    "src/AsyncHelper.cpp"
    "src/VFXManager.cpp"
    "src/FileWatcher.cpp"
    "src/safetyhook.cpp" 
    
    "src/UI/UIBase.cpp"       
    "src/UI/UIRegistry.cpp"   
    "src/UI/views/TestUI.cpp"      
    "src/UI/views/UIManager.cpp"  
    "src/NativeAsyncLoader.cpp"  
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
    "RE-UE4SS/deps/first/zydis/include" 
    "RE-UE4SS/deps/first/zydis/dependencies/zycore/include" 
)
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
    "RE-UE4SS/deps/first/zydis/include" 
    "RE-UE4SS/deps/first/zydis/dependencies/zycore/include" 
)
target_link_libraries(DynamicPals_Offline PUBLIC UE4SS Zydis)

# =========================================================================
# BUILD ORDER DEPENDENCIES (Prevents Parallel Compilation Race Conditions)
# =========================================================================
add_dependencies(DynamicPals Zydis)
add_dependencies(DynamicPals_Offline Zydis)
add_dependencies(DynamicPals DynamicPals_Offline)

# =========================================================================
# COMPILER FLAGS: Force PDB generation even in Shipping configurations
# =========================================================================
if(MSVC)
    # /FS (Force Synchronous PDB Writes), /Zi (Generate Debug Information)
    target_compile_options(DynamicPals PRIVATE /FS /Zi)
    target_compile_options(DynamicPals_Offline PRIVATE /FS /Zi)
    
    # /FORCE:MULTIPLE (Ignore multiple definition warnings), /DEBUG (Tell linker to emit PDB)
    target_link_options(DynamicPals PRIVATE "/FORCE:MULTIPLE" "/DEBUG")
    target_link_options(DynamicPals_Offline PRIVATE "/FORCE:MULTIPLE" "/DEBUG")
endif()

# =========================================================================
# AUTOMATION: Executed on post-build to copy dll, run scripts, and zip releases
# =========================================================================
add_custom_command(TARGET DynamicPals POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DynamicPals>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DynamicPals_Offline>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main_offline.dll"
    
    # Copy generated PDBs to the dll folder for the Visual Studio Debugger
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_PDB_FILE:DynamicPals>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main.pdb"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_PDB_FILE:DynamicPals_Offline>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main_offline.pdb"
    
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake_scripts/generate_templates.ps1" -SourceDir "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake_scripts/write_version.ps1" -SourceDir "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake_scripts/package_release.ps1" -SourceDir "${CMAKE_CURRENT_SOURCE_DIR}"
    
    COMMENT "Copying DLLs and PDBs, generating JSON templates, and packaging ZIP releases."
)