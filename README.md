# PulseMDL

> **Experimental**

A modified standalone fork of Valve's StudioMDL compiler, focused on advanced character modding workflows (facial flexes, procedural bones, bodygroup variants, proportion rigs).

Primarily tested with `DMX model 18`. Older versions may still work.

## TODO

- Further remove some compile limit even if compiling above said limit will crash the game (add a warning?)
- Check and possibly fix vertex animations
- Implement eyeball, eyelid for DMX.
- Completely re-design the mesh split.
- Refine the printing output

## Tested Source Engine Branch
 
| Game | Test Result |
|---|---|
| Garry's Mod | ✅ Fully Supported |
| Left 4 Dead 2 | ✅ Fully Supported |
| Half-Life 2 | 🟡 Partially tested |
| Counter-Strike: Source | ❌ Untested |
| Team Fortress 2 | ❌ Untested |
| Source Filmmaker | 🟡 Partially tested |
| Portal 1 & 2 | ❌ Untested |

## Known Issues

> [!WARNING]
> **Mapping workflows (e.g. Propper) are likely no longer supported.** Some VMF-related elements were removed during cleanup, so map-to-model workflows such as [Propper](https://developer.valvesoftware.com/wiki/Propper) are likely no longer possible with this fork.

> [!WARNING]
> **Collision compile accuracy may vary.** The standalone `$collisionmodel` and `$collisionjoints` pipeline (which no longer requires vphysics.dll) is built from a combination of open-source references and reverse engineering. References used: [VPhysics-Jolt](https://github.com/misyltoad/VPhysics-Jolt), [Gmod-vphysics](https://github.com/DrChat/Gmod-vphysics), and [Valve Developer Wiki - VPhysics](https://developer.valvesoftware.com/wiki/VPhysics). Its output may not be fully consistent with what official Valve studiomdl produces. If you encounter incorrect physics shapes, unexpected behavior, or compilation differences, please report them as issues.

> [!WARNING]
> **Vertex Animations** Vertex Animation is only supported for -vtxformat 1 (alien swarm engine branch and above) but there are likely unwanted vertex animation elements being compiled with -vtxformat 0, thus don't implement vertex animation on your model for games such as TF2,HL2, and L4D2

> [!WARNING]
> **Capsule Hitbox from CS:GO (legacy)** The capsule hitbox from CSGO is still being compiled along with traditional box hitbox.  I've tested box hitbox in L4D2 and Garry's Mod and it seems to still work properly even with capsule data of "0 0 0 -1" in the model.  TODO: A launch parameter to completely compile without capsule data?


## Features

> [!NOTE]
> **New QC commands that are not listed in features are not finalized.** Syntax, behavior, and naming for said commands are subject to change or removal in future versions.

- 64-bit, no external dependencies
- Increased limits:
  - Bone limit: 256 -> 1024 (Default cap is 256, see `wiki $modelbudget`)
  - Texture limit: 32 -> 96
  - Flex vertices limit: 10000 -> 32768
  - Mesh vertices limit: 21845 -> 174762 (overall model mesh limit: 65536 -> 524288)
- Replaced nvstrip with meshoptimizer
- Minor performance improvements to the compile pipeline
- Revised bone collapsing behavior - a bone is preserved if any of the following apply:
  - It is a Jigglebone
  - It is a procedural or driver bone
  - It is referenced by `$definebone`
  - It is referenced by `$bonemerge`
  - It is a BoneFlexDriver target
- `$definevariable` now works inside quotation marks (`$definemacro` does not)
- Multiple engine branch support; use `-vtxformat 1` for the Alien Swarm/CS:GO engine branch VTX format (default 0 = TF2/L4D2; minimum supported branch is likely TF2 or SourceSDK2013)
- Variables can be defined outside of QC via `-defvar <name> <value>` (repeatable)
- `$include` supports a fallback search directory via `-includedir <dir>` (repeatable)
- DirectX 8 compile support, opt-out with `-nodx80` (similar to StudioMDL++)
- `$addsearchdir` now correctly applies to SMD/DMX source file lookup
- New `ignorescale` parameter for `$animation` and `$sequence`
- New `$driverbone` and `driverlookat` commands to define procedural bones without VRD files
- New `$rendermesh` to filter source geometry by DmeMesh object name or by material name; DmeMesh filtering requires DMX, `removematerial` works on any source format
- New `$if`, `$elif`, `$else`, and `$switch` conditional commands
- New `$staticproppose`to bake a custom pose into a `static_prop`'s geometry skeleton. Cannot be used together with `$staticprop`.
- New `$return <optional message>` to halt compilation with an optional message (similar to `$qcassert`)
- New `$break` to stop reading the QC at that point while still compiling whatever was parsed so far (unlike `$return`, which aborts without compiling)
- New `$print <message>` for compile-time output
- New `$deltaproportions` to generate `a_reference` and `a_proportions`. See `wiki deltaproportions`
- New `$include` inline options:
  - `iffileexist` - silently skips the include if the file does not exist
  - `nofallbackdir` - disables the `-includedir` fallback for this include
- `$model` can now be used inside `$bodygroup` blocks, enabling named variants with per-variant sub-options (eyeball, flex, etc.). See `wiki bodygroup-model`
- `-cullmorphs` flag to strip flex morphs not driven by any flexcontroller/flexrule/eyeball/mouth (dead vertex-animation data)
- `-cullanims` flag to strip unreferenced `$animation` blocks
- Bone weight cull threshold reduced from 5% to 0.01%
- `$scale` now affects eyeball, eyelid, dmxeyelid, forceboneposrot, procedural bones, VTA flex deltas, and `$eyeposition`
- `$renamebone` now propagates to the collision model
- Fixed crash with blank bodygroup combined with `$staticprop`


## Credits

- Valve - Source Engine and SDK
- [REDxEYE](https://github.com/REDxEYE) - base fork [REDxEYE/studiomdl_v2](https://github.com/REDxEYE/studiomdl_v2)
- [ficool2](https://github.com/ficool2) - StudioMDL++ (Ideas based on the features their studiomdl version provided)
- [Starfelll](https://github.com/Starfelll) - NekoMDL (Ideas based on the features their studiomdl version provided)
