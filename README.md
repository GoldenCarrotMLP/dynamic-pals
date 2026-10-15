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

1. Ensure [UE4SS](https://github.com/Okaetsu/RE-UE4SS) is installed in your Palworld directory (`Pal/Binaries/Win64/`).
2. Create a folder named `DynamicPals` in your UE4SS `Mods` directory: 
   `Pal/Binaries/Win64/ue4ss/Mods/DynamicPals/`
3. Copy your compiled `main.dll`, `version.txt` and `DynamicPals.pak` into:
   `Pal/Binaries/Win64/ue4ss/Mods/DynamicPals/dlls/`
4. Copy `DynamicPals.pak` into `Pal/Content/Paks/LogicMods/` as well (the auto-update build does this for you on first launch; restart the game once afterwards).
5. Enable the mod with an `enabled.txt` file in the `DynamicPals` folder, or add `DynamicPals : 1` to `Pal/Binaries/Win64/ue4ss/Mods/mods.txt`.

The release zips produced by the build (`DynamicPals_AutoUpdate.zip` / `DynamicPals.zip`) already contain this folder layout.

---

## Controls

* **`Alt + N`:** Toggles the manual customization menu when looking at a Pal.
* **`Escape` (while menu is open):** Safely closes the menu, re-hiding the mouse cursor and returning input capture to the game camera.
* **Reroll Pal:** Unbound by default. Assign a key in the menu's settings tab to reroll the Pal in your crosshair instantly.

All hotkeys can be rebound from the settings tab of the `Alt + N` menu.

---

## Creating Skin Configuration Packs

Configurations are stored as JSON files inside the game's content directory at:
`Pal/Content/Paks/~mods/SwapJSON/` for classic Altermatic (V1) jsons
`Pal/Content/Paks/~mods/ModelJSON/` for newer PalMagic (V2) jsons

See [Docs/FORMAT_V1.md](./Docs/FORMAT_V1.md) and [Docs/FORMAT_V2.md](./Docs/FORMAT_V2.md) for the full schemas.

---
## Credits

This mod is possible and stable thanks to these amazing people:

- MonoDragon
- Z3NlTH-Dr4G0N
- ⌞ℤ𝕖𝕥𝕥𝕖𝕣𝟛𝔻⌝
- [MCorgano](https://github.com/Corgano)
- [Okaetsu](https://github.com/Okaetsu)
- T-Box
- DefaultUsername80
- Reaper
- Spam
- Raeil
- Jyanon 
- Faust0151 
- Hevensdragon
- [Isovel](https://github.com/isovel)
- [Syncriix](https://github.com/Syncriix)
  
## License

This project is licensed under the MIT License