cmake_minimum_required(VERSION 3.22)
project(AltermaticCpp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add UE4SS SDK
add_subdirectory(RE-UE4SS)

# Define your mod
add_library(AltermaticCpp SHARED "dllmain.cpp")

# Standard UE4SS Include Paths + Your Current Source Directory
target_include_directories(AltermaticCpp PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "RE-UE4SS/UE4SS/include"
    "RE-UE4SS/deps/first/Unreal/include"
    "RE-UE4SS/deps/first/DynamicOutput/include"
    "RE-UE4SS/deps/first/Function-Pointer-Wrapper/include"
)

# Link against UE4SS
target_link_libraries(AltermaticCpp PUBLIC UE4SS)

# AUTOMATION: Create /dlls directory, copy, and rename DLL to main.dll after compiling
add_custom_command(TARGET AltermaticCpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/dlls"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:AltermaticCpp>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main.dll"
    COMMENT "Auto-copying and renaming compiled DLL to Mods/AltermaticCpp/dlls/main.dll"
)