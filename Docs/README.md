# DynamicPals (DynPals) — Runtime Replacer Framework

DynamicPals is a high-performance, native C++ runtime mesh and material swapper framework for Palworld built on UE4SS. 

Unlike traditional `.pak` replacements which statically overwrite game-wide assets, DynamicPals intercepts overworld spawns dynamically at runtime, applying randomized or highly specific cosmetic variations (meshes, materials, and blendshape morphs) to individual Pals of the same species.

---

## Features
* **Ingame UI:** When pressing ALT+N ingame, selects the closest pal to your crosshair and brings up a menu which allows you to chose specific swaps and tweak the blendshapes or even reset the pal to default model. 
* **Evolution Engine:** Allows you to set up evolution lines or branches to transition to them by leveling up any attribute specifed on the JSON.
* **Native Execution:** Written completely in C++ for maximum performance and minimal frame-time overhead.
* **Two JSON Formats Supported:** Seamlessly parses both Version 1 (Altermatic) and Version 2 (PalMagic) configuration schemas.
* **Matchmaking Engine:** Evaluates Pals' traits, gender, level, and rarity against configured weights to assign the most specific skin. (thanks to spam for helping polish it)
* **UI Integration:** Utilize the `SwapLabel` property to give your replacement skins clean, readable names inside supported in-game menus.
* **Dynamic Morph Target Engine:** Randomized or absolute morph targets (blendshapes) applied cleanly per-instance.
* **Advanced Material Instancing:** Swap textures and materials dynamically per-slot without duplicating mesh files. Now features wildcard targeting (`/*`) for rapid material folder overrides! (thanks to Raeil for the idea)
* **Dynamic Hue Shifting:** Generate vibrant, randomized HSV colors at runtime per material slot using `"RandomHue": true`. The mod automatically handles creating Dynamic Material Instances (MIDs) on-the-fly and applies a unique, persisted color to the `"Hue"` vector parameter (safely ignored by materials that do not support it).
* **Dynamic scale:** With MinSizeMultiplier and MaxSizeMultiplier properties on the json you can have native size variations on your pals.
* **Overworld Settle Quarantine:** Pauses swaps for 8 seconds after a world loads to prevent spawning performance spikes.
* **Custom Names:** Depending on how you set your JSON, the pal can appear ingame with a custom name of your choosing.
* **Custom Icons:** You can specify an image path in your json to allow the pal to have their own custom icon ingame.
* **Hot Reload:** Editing or adding a JSON file while the game is running reloads all packs automatically.

---

## Installation

1. Install the latest stable version of [UE4SS](https://github.com/Okaetsu/RE-UE4SS).
2. Extract [`DynamicPals_AutoUpdate.zip`](https://github.com/GoldenCarrotMLP/dynamic-pals/releases/latest) (or the offline `DynamicPals.zip`) into `Palworld/Pal/Binaries/Win64/ue4ss/Mods/`, so you end up with `ue4ss/Mods/DynamicPals/dlls/main.dll`.
3. Install the companion asset pack `DynamicPals.pak` into `Palworld/Pal/Content/Paks/LogicMods/`:
   * **Auto-update release:** done for you. On first launch the mod copies `DynamicPals/dlls/DynamicPals.pak` into `LogicMods` — **restart the game once** so it gets mounted.
   * **Offline release:** copy `DynamicPals/dlls/DynamicPals.pak` into `LogicMods` yourself.
4. Place your JSON configuration files in their respective folders (created automatically on first launch):
   * **Version 1 (Array-based) files:** `Palworld/Pal/Content/Paks/~mods/SwapJSON/`
   * **Version 2 (Map-based) files:** `Palworld/Pal/Content/Paks/~mods/ModelJSON/`
5. Launch Palworld.

> Do not run DynamicPals together with PalMagic or Altermatic paks in `LogicMods` — the mod will warn you on load if it finds them.

---

## Reference Guides
* [Version 1 JSON Schema Specification (Array-based)](./FORMAT_V1.md)
* [Version 2 JSON Schema Specification (Grouped Map-based)](./FORMAT_V2.md)
* [Internal Pal IDs List](./PAL_IDS.json)
* [Internal Trait (Passive Skill) IDs List](./TRAIT_IDS.json)

---

## The "Best Match" Matchmaking Algorithm

When a Pal spawns, DynamicPals evaluates every loaded config entry targeting that `CharacterID`. The engine uses a **Matchmaking Point Penalty System** (where a lower score is more specific, and therefore wins the match). 

If two or more configurations tie for the lowest score, the engine resolves the tie using a weighted random distribution based on each config's `SpawnWeight`. Configs with `SpawnWeight: 0` never win automatically, but can still be picked from the in-game menu.

Boss/alpha prefixes (`BOSS_`, `MiddleBoss_`, `RAID_`, `GYM_`, `PREDATOR_`) and the `_Otomo` suffix are stripped from a Pal's ID before matching, so a config for `"PinkCat"` also applies to alpha PinkCats.

### Scoring Rules:
| Condition | Match Outcome | Score Adjustment / Action |
| :--- | :--- | :--- |
| **Required Swap (`ReqSwap`) Missing** | Fail | Skip Swap (Invalidated) |
| **Required Trait Missing** | Fail | Skip Swap (Invalidated) |
| **Banned Trait (`SkipTrait`) Present** | Fail | Skip Swap (Invalidated) |
| **IsRarePal / IsWildPal Misaligned** | Fail | Skip Swap (Invalidated) |
| **Level / Rank / Trust Out of Range** | Fail | Skip Swap (Invalidated) |
| **Gender Misaligned (Explicit)** | Fail | Skip Swap (Invalidated) |
| **SkinName Misaligned** (config requires a skin the Pal doesn't wear, or the Pal wears an official skin and the config has no `SkinName`) | Fail | Skip Swap (Invalidated) |
| **Gender Matched (Explicit, Neutral, or SCake)** | Pass | **0** (Valid match, score is unaffected) |
| **IsRarePal Matched** | Hit | **-50** Bonus |
| **IsWildPal Matched** | Hit | **-50** Bonus |
| **Explicit SkinName Matched** | Hit | **-50** Bonus |
| **Required Swap (`ReqSwap`) Matched** | Hit | **-30** Bonus |
| **Required Trait Matched** | Hit | **-20** Bonus (per trait) |
| **Preferred Trait Matched** | Hit | **-10** Bonus (per trait) |
| **Preferred Trait Missed** | Miss | **+10** Penalty (per trait) |
| **Specific Level Range Defined** | Hit | **-10** Bonus, plus an extra **-1 per `MinLevel`** above 1 |
| **Specific Rank Range Defined** | Hit | **-10** Bonus, plus an extra **-5 per `MinRank`** above 0 |
| **Specific Trust Range Defined** | Hit | **-10** Bonus |

The extra `MinLevel` / `MinRank` bonus means that, in an evolution line, the stage with the highest requirement the Pal meets is preferred, all else being equal.

### Blacklist-Style Gender Filtering
Gender is treated as a strict filter rather than a scoring metric. 
* A gender-neutral swap (`"None"`, `"Any"`, `"All"` or `"Both"`) and a gender-matching swap both incur **0 score adjustments**, putting them into the exact same pool if all other traits/limits align. Selection is resolved entirely by their configured `SpawnWeight`.
* If a configuration specifies a gender (e.g., `"Female"`), and the spawned Pal is not female (or does not match an allowable SCake fallback like `"Andro"`), the swap is **invalidated** and skipped.

*Note: If 4 or more candidates tie and one skin's SpawnWeight is over double the next highest, the mod prints a one-time note in the UE4SS console to help you spot unintentional variety dilution.*
