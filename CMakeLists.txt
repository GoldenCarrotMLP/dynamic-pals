cmake_minimum_required(VERSION 3.22)
project(DynamicPals)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add UE4SS SDK
add_subdirectory(RE-UE4SS)

# Define your mod and point to the refactored source files
add_library(DynamicPals SHARED 
    "src/dllmain.cpp"
    "src/HooksManager.cpp"
    "src/ConfigManager.cpp"
    "src/SaveManager.cpp"
    "src/PalProcessor.cpp"
    "src/UIManager.cpp" 
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
target_link_libraries(DynamicPals PUBLIC UE4SS)

# AUTOMATION: Create /dlls directory, copy, and rename DLL to main.dll after compiling
add_custom_command(TARGET DynamicPals POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/dlls"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DynamicPals>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main.dll"
    COMMENT "Auto-copying and renaming compiled DLL to Mods/DynamicPals/dlls/main.dll"
)