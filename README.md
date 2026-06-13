# KitsuneMDL

> **Experimental**

A standalone fork of Valve's StudioMDL compiler based on [REDxEYE/studiomdl_v2](https://github.com/REDxEYE/studiomdl_v2).

Primarily tested with `DMX model 18`. Older versions may still work.

## Known Issues

> [!CAUTION]
> **Mesh splitting is unreliable.** Splitting meshes that contain flex data may produce corrupted or completely broken results. Use with caution, especially on models with shape keys or flex controllers. Results can range from minor vertex errors to fully unusable output.

> [!WARNING]
> **Collision compile accuracy may vary.** The standalone `$collisionmodel` and `$collisionjoints` pipeline (which no longer requires vphysics.dll) is built from a combination of open-source references and reverse engineering. References used: [VPhysics-Jolt](https://github.com/misyltoad/VPhysics-Jolt), [Gmod-vphysics](https://github.com/DrChat/Gmod-vphysics), and [Valve Developer Wiki - VPhysics](https://developer.valvesoftware.com/wiki/VPhysics). Its output may not be fully consistent with what official Valve studiomdl produces. If you encounter incorrect physics shapes, unexpected behavior, or compilation differences, please report them as issues.


## Features

- 64-bit, no external dependencies
- Increased limits:
  - Bone limit: 256 -> 1024
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
- Multiple engine branch support; use `-newvtx` for the Alien Swarm to CS:GO engine branch (minimum supported branch is likely TF2 or SourceSDK2013)
- Variables can be defined outside of QC via `-defvar <name> <value>` (repeatable)
- `$include` supports a fallback search directory via `-includedir <dir>` (repeatable)
- DirectX 8 compile support, opt-out with `-nodx80` (similar to StudioMDL++)
- `$addsearchdir` now correctly applies to SMD/DMX source file lookup
- New `ignorescale` parameter for `$animation` and `$sequence`
- New `$driverbone` and `driverlookat` commands to define procedural bones without VRD files
- New `$rendermesh` for DMX models containing multiple DMEMesh elements
- New `$if`, `$elif`, `$else`, and `$switch` conditional commands
- New `$staticproppose <animation_file> <frame>` to bake a custom pose into a `static_prop`'s geometry skeleton. Cannot be combined with `$staticprop`. **(Work in progress - does not behave as expected)**
- New `$return <optional message>` to halt compilation with an optional message (similar to `$qcassert`)
- New `$print <message>` for compile-time output
- New `$deltaproportions` to generate `a_reference` and `a_proportions`. See `docs/deltaproportions.txt`
- New `$include` inline options:
  - `iffileexist` - silently skips the include if the file does not exist
  - `nofallbackdir` - disables the `-includedir` fallback for this include
- `$model` can now be used inside `$bodygroup` blocks, enabling named variants with per-variant sub-options (eyeball, flex, etc.). See `docs/bodygroup-model.txt`
- Recreated features from StudioMDL++ and NekoMDL:
  - `-cullanims` flag to strip unreferenced `$animation` blocks
  - Bone weight cull threshold reduced from 5% to 0.01%
  - `$scale` now affects eyeball, eyelid, dmxeyelid, forceboneposrot, procedural bones, and VTA flex deltas
  - `$renamebone` now propagates to the collision model
  - Fixed crash with blank bodygroup combined with `$staticprop`

## Credits

- Valve - Source Engine and SDK
- [REDxEYE](https://github.com/REDxEYE) - base fork
- [ficool2](https://github.com/ficool2) - StudioMDL++ (Ideas based on the features their studiomdl version provided)
- [Starfelll](https://github.com/Starfelll) - NekoMDL
