# DynamicPals (DynPals) — Runtime Replacer Framework

DynamicPals is a high-performance, native C++ runtime mesh and material swapper framework for Palworld built on UE4SS. 

Unlike traditional `.pak` replacements which statically overwrite game-wide assets, DynamicPals intercepts overworld spawns dynamically at runtime, applying randomized or highly specific cosmetic variations (meshes, materials, and blendshape morphs) to individual Pals of the same species.

---

## Features
* **Native Execution:** Written completely in C++ for maximum performance and minimal frame-time overhead.
* **Two JSON Formats Supported:** Seamlessly parses both Version 1 (Altermatic) and Version 2 (PalMagic) configuration schemas.
* **Matchmaking Engine:** Evaluates Pals' traits, gender, level, and rarity against configured weights to assign the most specific skin.
* **Dynamic Morph Target Engine:** Randomized or absolute morph targets (blendshapes) applied cleanly per-instance.
* **Material Instancing:** Swap textures and materials dynamically per-slot without duplicating mesh files.
* **Overworld Settle Quarantine:** Pauses swaps for 5 seconds during initial level loads to prevent spawning performance spikes.

---

## Installation

1. Install the latest stable version of [UE4SS](https://github.com/Okaetsu/RE-UE4SS).
3. Drop the compiled [`DynamicPals.zip`](https://github.com/GoldenCarrotMLP/dynamic-pals/releases/latest) folder into `ue4ss/Mods/`.
4. Place your JSON configuration files inside `Mods/DynamicPals/Paks/~mods/SwapJSON/`.
5. Launch Palworld.

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
| **Required Trait Missing** | Fail | Skip Swap (Invalidated) |
| **Banned Trait (`SkipTrait`) Present** | Fail | Skip Swap (Invalidated) |
| **IsRarePal / IsWildPal Misaligned** | Fail | Skip Swap (Invalidated) |
| **Level / Rank / Trust Out of Range** | Fail | Skip Swap (Invalidated) |
| **Gender Misaligned (Explicit)** | Fail | Skip Swap (Invalidated) |
| **Gender Generic Fallback (`None`)** | Miss | **+500,000** Penalty (Direct gender matches always win) |
| **Gender Extended Fallback (SCake)** | Miss | **+50,000** Penalty (e.g. Futa fallback on Male) |
| **IsRarePal Matched** | Hit | **-50** Bonus |
| **IsWildPal Matched** | Hit | **-50** Bonus |
| **Explicit SkinName Matched** | Hit | **-50** Bonus |
| **Required Trait Matched** | Hit | **-20** Bonus (per trait) |
| **Preferred Trait Matched** | Hit | **-10** Bonus |
| **Preferred Trait Missed** | Miss | **+10** Penalty |
| **Specific Level Range Defined** | Hit | **-10** Bonus |
| **Specific Rank Range Defined** | Hit | **-10** Bonus |
| **Specific Trust Range Defined** | Hit | **-10** Bonus |
| **Gender Matched (Explicit)** | Hit | **-100** Bonus |

*Note: If a skin's configured SpawnWeight is over double the weight of other tied candidates, the mod will print a yellow warning trace in your console to prevent unintentional variety dilution.*
