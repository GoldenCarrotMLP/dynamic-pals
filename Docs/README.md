# DynamicPals (DynPals) — Runtime Replacer Framework

DynamicPals is a high-performance, native C++ runtime mesh and material swapper framework for Palworld built on UE4SS. 

Unlike traditional `.pak` replacements which statically overwrite game-wide assets, DynamicPals intercepts overworld spawns dynamically at runtime, applying randomized or highly specific cosmetic variations (meshes, materials, and blendshape morphs) to individual Pals of the same species.

---

## Features
* **Native Execution:** Written completely in C++ for maximum performance and minimal frame-time overhead.
* **Two JSON Formats Supported:** Seamlessly parses both Version 1 (Altermatic) and Version 2 (PalMagic) configuration schemas.
* **Matchmaking Engine:** Evaluates Pals' traits, gender, level, and rarity against configured weights to assign the most specific skin. (thanks to spam for helping polish it)
* **UI Integration:** Utilize the `SwapLabel` property to give your replacement skins clean, readable names inside supported in-game menus.
* **Dynamic Morph Target Engine:** Randomized or absolute morph targets (blendshapes) applied cleanly per-instance.
* **Advanced Material Instancing:** Swap textures and materials dynamically per-slot without duplicating mesh files. Now features wildcard targeting (`/*`) for rapid material folder overrides! (thanks to Raeil for the idea)
* **Dynamic Hue Shifting:** Generate vibrant, randomized HSV colors at runtime per material slot using `"RandomHue": true`. The mod automatically handles creating Dynamic Material Instances (MIDs) on-the-fly and applies a unique, persisted color to the `"Hue"` vector parameter (safely ignored by materials that do not support it).
* **Overworld Settle Quarantine:** Pauses swaps for 5 seconds during initial level loads to prevent spawning performance spikes.
* **Custon Names:** Depending on how you set your JSON, the pal can appear ingame with a custom name of your choosing.

---

## Installation

1. Install the latest stable version of [UE4SS](https://github.com/Okaetsu/RE-UE4SS).
2. Drop the compiled [`DynamicPals.zip`](https://github.com/GoldenCarrotMLP/dynamic-pals/releases/latest) folder (or the offline release) into your `ue4ss/Mods/` directory.
3. Place your JSON configuration files in their respective folders:
   * **Version 1 (Array-based) files:** `Mods/DynamicPals/Paks/~mods/SwapJSON/`
   * **Version 2 (Map-based) files:** `Mods/DynamicPals/Paks/~mods/ModelJSON/`
4. Launch Palworld.

---

## Reference Guides
* [Version 1 JSON Schema Specification (Array-based)](./FORMAT_V1.md)
* [Version 2 JSON Schema Specification (Grouped Map-based)](./FORMAT_V2.md)
* [Internal Pal IDs List](./PAL_IDS.json)
* [Internal Trait (Passive Skill) IDs List](./TRAIT_IDS.json)

---

## The "Best Match" Matchmaking Algorithm

When a Pal spawns, DynamicPals evaluates every loaded config entry targeting that `CharacterID`. The engine uses a **Matchmaking Point Penalty System** (where a lower score is more specific, and therefore wins the match). 

If two or more configurations tie for the lowest score, the engine resolves the tie using a weighted random distribution based on each config's `SpawnWeight`.

### Scoring Rules:
| Condition | Match Outcome | Score Adjustment / Action |
| :--- | :--- | :--- |
| **Required Swap Missing** | Fail | Skip Swap (Invalidated) |
| **Required Trait Missing** | Fail | Skip Swap (Invalidated) |
| **Banned Trait (`SkipTrait`) Present** | Fail | Skip Swap (Invalidated) |
| **IsRarePal / IsWildPal Misaligned** | Fail | Skip Swap (Invalidated) |
| **Level / Rank / Trust Out of Range** | Fail | Skip Swap (Invalidated) |
| **Gender Misaligned (Explicit)** | Fail | Skip Swap (Invalidated) |
| **Gender Matched (Explicit, Neutral, or SCake)** | Pass | **0** (Valid match, score is unaffected) |
| **IsRarePal Matched** | Hit | **-50** Bonus |
| **IsWildPal Matched** | Hit | **-50** Bonus |
| **Explicit SkinName Matched** | Hit | **-50** Bonus |
| **Required Trait Matched** | Hit | **-20** Bonus (per trait) |
| **Preferred Trait Matched** | Hit | **-10** Bonus |
| **Preferred Trait Missed** | Miss | **+10** Penalty |
| **Specific Level Range Defined** | Hit | **-10** Bonus |
| **Specific Rank Range Defined** | Hit | **-10** Bonus |
| **Specific Trust Range Defined** | Hit | **-10** Bonus |

### Blacklist-Style Gender Filtering
Gender is treated as a strict filter rather than a scoring metric. 
* A gender-neutral swap (`"None"` or `"Any"`) and a gender-matching swap both incur **0 score adjustments**, putting them into the exact same pool if all other traits/limits align. Selection is resolved entirely by their configured `SpawnWeight`.
* If a configuration specifies a gender (e.g., `"Female"`), and the spawned Pal is not female (or does not match an allowable SCake fallback like `"Andro"`), the swap is **invalidated** and skipped.

*Note: If a skin's configured SpawnWeight is over double the weight of other tied candidates, the mod will print a yellow warning trace in your console to prevent unintentional variety dilution.*