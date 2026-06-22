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
# DYNAMIC TEMPLATE GENERATOR: Extracts C++ struct members dynamically
# =========================================================================
set(PS_SCRIPT "
\$file = Get-Content -Raw \"${CMAKE_CURRENT_SOURCE_DIR}/include/DataTypes.hpp\"

# Phase 1: Parse out TypeScript-style alias type constraints
\$aliases = @{}
foreach (\$line in (\$file -split '\\r?\\n')) {
    if (\$line -match 'using\\\\s+([a-zA-Z0-9_]+)\\\\s*=\\\\s*[^;]+;\\\\s*//\\\\s*OPTIONS:\\\\s*(.*)\$') {
        \$aliases[\$Matches[1].Trim()] = \$Matches[2].Trim()
    }
}

# Phase 2: Parse struct variables and map schemas
if (\$file -match '(?s)struct\\s+SwapConfig\\s*\\{(.*?)\\};') {
    \$body = \$Matches[1]
    \$json = [ordered]@{}
    foreach (\$line in (\$body -split '\\r?\\n')) {
        \$line = \$line -replace '//.*\$', ''
        if (\$line -match '^\\s*([a-zA-Z0-9_:<>]+)\\s+([a-zA-Z0-9_]+)(?:\\s*=\\s*([^;]+))?;') {
            \$type = \$Matches[1].Trim()
            \$name = \$Matches[2].Trim()
            \$val = if (\$Matches[3]) { \$Matches[3].Trim() } else { \$null }
            
            # Skip PackName inside the array, as it's parsed on the root level instead
            if (\$name -eq 'PackName') { continue }
            
            # Match schema constraints based on type aliases
            \$optionVal = if (\$aliases.ContainsKey(\$type)) { \$aliases[\$type] = \$aliases[\$type] } else { \$null }
            
            if (\$type -like '*std::vector*') {
                if (\$type -like '*MatReplace*') {
                    \$json[\$name] = ,@{'Index'='0'; 'MatPath'='/Game/MaterialPath'}
                } elseif (\$type -like '*MorphTarget*') {
                    # Dynamically inject parsed MorphType options into the morph sub-structure
                    \$morphTypeVal = if (\$aliases.ContainsKey('MorphType')) { \$aliases['MorphType'] } else { 'Free' }
                    \$json[\$name] = ,@{'Target'='MorphName'; 'Set'=1.0; 'Min'=0.0; 'Max'=1.0; 'Type'=\$morphTypeVal}
                } else {
                    \$json[\$name] = ,('TraitExample')
                }
            } elseif (\$type -like '*std::optional*') {
                if (\$type -like '*bool*') {
                    \$json[\$name] = \$false 
                } else {
                    \$json[\$name] = \$null
                }
            } elseif (\$type -eq 'int') {
                \$json[\$name] = if (\$val) { [int]\$val } else { 0 }
            } elseif (\$type -eq 'double') {
                \$json[\$name] = if (\$val) { [double]\$val } else { 0.0 }
            } else {
                # Fall back to using the custom type alias options, or the parsed default value
                if (\$optionVal) {
                    \$json[\$name] = \$optionVal
                } else {
                    \$json[\$name] = if (\$val) { \$val.Replace('L\"', '').Replace('\"', '') } else { '' }
                }
            }
        }
    }
    
    # Wrap it nicely inside the root structure matching palbaker-Ronin.json
    \$rootLayout = [ordered]@{
        'PackName' = 'Your Replacer Pack Name'
        'SkelMeshSwap' = ,\$json
    }
    \$final = \$rootLayout | ConvertTo-Json -Depth 5
    New-Item -ItemType Directory -Force -Path \"${CMAKE_CURRENT_SOURCE_DIR}/dlls\" | Out-Null
    Set-Content -Path \"${CMAKE_CURRENT_SOURCE_DIR}/dlls/_Template_SwapConfig.json\" -Value \$final
}
")


# Write the generator script out safely (CMake expands ${CMAKE_CURRENT_SOURCE_DIR})
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generate_template.ps1" "${PS_SCRIPT}")


# =========================================================================
# AUTOMATION: Executed on post-build to copy dll and run the parser
# =========================================================================
# =========================================================================
# VERSION GENERATOR: Dynamically queries Git commit count at build time
# =========================================================================
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/write_version.cmake" "
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND \${GIT_EXECUTABLE} rev-list --count HEAD
            WORKING_DIRECTORY \"${CMAKE_CURRENT_SOURCE_DIR}\"
            OUTPUT_VARIABLE GIT_COMMIT_COUNT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(GIT_COMMIT_COUNT)
            file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/dlls/version.txt\" \"\${GIT_COMMIT_COUNT}\")
        else()
            file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/dlls/version.txt\" \"0\")
        endif()
    else()
        # Fallback if git is not installed or not in PATH
        file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/dlls/version.txt\" \"0\")
    endif()
")
# =========================================================================
# AUTOMATION: Executed on post-build to copy dll and run the parser
# =========================================================================
add_custom_command(TARGET DynamicPals POST_BUILD
    # 1. Copy the compiled DLL to the dlls folder
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DynamicPals>" "${CMAKE_CURRENT_SOURCE_DIR}/dlls/main.dll"
    
    # 2. Run the PowerShell template parser
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_BINARY_DIR}/generate_template.ps1"
    
    # 3. Query Git and write/upsert the version.txt file
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/write_version.cmake"
    
    COMMENT "Copying main.dll, generating SwapConfig templates, and updating version.txt with Git commit count."
)