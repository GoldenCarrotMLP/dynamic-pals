cmake_minimum_required(VERSION 3.22)
project(DynamicPals)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(RE-UE4SS)

add_library(DynamicPals SHARED 
    "src/dllmain.cpp"
    "src/HooksManager.cpp"
    "src/ConfigManager.cpp"
    "src/SaveManager.cpp"
    "src/PalProcessor.cpp"
    "src/UIManager.cpp"
    "src/Updater.cpp"
    "src/NotificationManager.cpp" 
    "src/AsyncHelper.cpp"
)

# Standard UE4SS Include Paths + Your New include/ Directory
target_include_directories(DynamicPals PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "RE-UE4SS/UE4SS/include"
    "RE-UE4SS/deps/first/Unreal/include"
    "RE-UE4SS/deps/first/DynamicOutput/include"
    "RE-UE4SS/deps/first/Function-Pointer-Wrapper/include"
)

# Link against UE4SS
target_link_libraries(DynamicPals PUBLIC UE4SS wininet)

# FORCE SYNCHRONOUS PDB WRITES (Fixes C1041 Compiler Locks)
if(MSVC)
    target_compile_options(DynamicPals PRIVATE /FS)
    target_link_options(DynamicPals PRIVATE "/FORCE:MULTIPLE")
endif()

# =========================================================================
# AUTOMATION: Executed on post-build to copy dll and run external scripts
# =========================================================================
add_custom_command(TARGET DynamicPals POST_BUILD
    # 1. Copy the compiled DLL to the dlls folder
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DynamicPals>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main.dll"
    
    # 2. Run the PowerShell template parser (Generates V1 & V2 JSON Templates)
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake_scripts/generate_templates.ps1" -SourceDir "${CMAKE_CURRENT_SOURCE_DIR}"
    
    # 3. Query Git and write/upsert the version.txt file
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake_scripts/write_version.ps1" -SourceDir "${CMAKE_CURRENT_SOURCE_DIR}"
    
    COMMENT "Copying main.dll, generating V1 and V2 JSON templates, and updating version.txt with Git commit count."
)