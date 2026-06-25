# DynamicPals [(Guide)](./Docs/README.md)

`DynamicPals` is a native C++ runtime skeletal mesh, material, and morph target swapper for Palworld, built on top of [RE-UE4SS](https://github.com/Okaetsu/RE-UE4SS). It allows server-safe, client-side, or single-player cosmetic variations of Pals based on custom traits, levels, genders, trust, and more.

It includes an asynchronous auto-updater, a thread-safe native UI notification system (toasts), an in-game manual customization UI, and an in-memory database persistence system.


---

## Prerequisites

Before cloning and building, you must ensure your development environment has access to the Unreal Engine GitHub organization (which is required by the UE4SS dependency submodules).

1. **Unreal Engine Source Access:** 
   - Link your GitHub account to your Epic Games account by visiting [Epic Games Connections](https://accounts.epicgames.com/account/connections).
   - Accept the invitation email from GitHub to join the `@EpicGames` organization.
   - *If this step is skipped, git will fail to clone the necessary UE4SS submodules.*

---

## Developer Environment Setup

You can set up all your compiler build tools directly from your terminal using Windows Package Manager (`winget`) [2]. Open **PowerShell** as **Administrator** and run the following commands:

### 1. Install Git
Installs Git and registers it to your system path:
```powershell
winget install --id Git.Git -e --source winget
```

### 2. Install CMake
Installs CMake and registers it to your system path [4]:
```powershell
winget install --id Kitware.CMake -e --source winget
```

### 3. Install Visual Studio C++ Build Tools
Installs the lightweight MSVC compiler and necessary Windows SDKs without downloading the entire Visual Studio IDE [5]:
```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```
*(Alternatively, if you prefer the full Visual Studio 2022 Community IDE, run this instead:)* [6]
```powershell
winget install --id Microsoft.VisualStudio.2022.Community --source winget --override "--wait --passive --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"
```
*Note: Please restart your terminal/PC after these installations finish to refresh your environment variables [6].*

---

## Compilation

### 1. Clone the Repository (Recursive)
Because this project utilizes nested submodules (such as UE4SS and its first-party dependencies), you **must** clone the repository recursively [7]:

```bash
git clone --recursive https://github.com/GoldenCarrotMLP/dynamic-pals.git
cd dynamic-pals
```

*If you already cloned the repository without `--recursive`, run this command inside the directory:*
```bash
git submodule update --init --recursive
```

### 2. Configure the Build Directories
Generate the build system configuration [8]:

```bash
cmake -B build -S .
```

### 3. Build the DLL
Compile the project using the native target configuration required by the game executable:

```bash
cmake --build build --config Game__Shipping__Win64
```

Upon successful compilation, the compiled `main.dll` and the dynamically updated `version.txt` (reflecting your current git commit count) will be automatically written to your local `dlls/` folder.

---

## How to Install the Mod

1. Ensure [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) is installed in your Palworld directory (`Pal/Binaries/Win64/`).
2. Create a folder named `DynamicPals` in your `Mods` directory: 
   `Pal/Binaries/Win64/Mods/DynamicPals/`
3. Copy your compiled `main.dll` and `version.txt` into:
   `Pal/Binaries/Win64/Mods/DynamicPals/dlls/`
4. Add `DynamicPals : 1` to your `mods.txt` file located in `Pal/Binaries/Win64/Mods/mods.txt`.

---

## Controls

* **`Alt + N`:** Toggles the manual customization menu when standing near a Pal.
* **`Escape` (while menu is open):** Safely closes the menu, re-hiding the mouse cursor and returning input capture to the game camera.

---

## Creating Skin Configuration Packs

Configurations are stored as JSON files inside your project's content directory at:
`Paks/~mods/SwapJSON/`

---
## Credits

This mod is possible and stable thanks to these amazing people:

- MonoDragon
- Z3NlTH-Dr4G0N
- ⌞ℤ𝕖𝕥𝕥𝕖𝕣𝟛𝔻⌝
- MCorgano
- Okaetsu
- T-Box
- DefaultUsername80
- Reaper
- Spam
- Raeil

## License

This project is licensed under the MIT License - see the LICENSE file for details.
