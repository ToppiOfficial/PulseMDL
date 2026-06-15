# KitsuneMDL

> **Experimental**

A modified standalone fork of Valve's StudioMDL compiler, focused on advanced character modding workflows (facial flexes, procedural bones, bodygroup variants, proportion rigs) while remaining fully compatible with general studiomdl use cases. Based on [REDxEYE/studiomdl_v2](https://github.com/REDxEYE/studiomdl_v2).

Primarily tested with `DMX model 18`. Older versions may still work.

## Known Issues

> [!CAUTION]
> **Mesh splitting is unreliable.** Splitting meshes that contain flex data may produce corrupted or completely broken results. Use with caution, especially on models with shape keys or flex controllers. Results can range from minor vertex errors to fully unusable output.

> [!WARNING]
> **Collision compile accuracy may vary.** The standalone `$collisionmodel` and `$collisionjoints` pipeline (which no longer requires vphysics.dll) is built from a combination of open-source references and reverse engineering. References used: [VPhysics-Jolt](https://github.com/misyltoad/VPhysics-Jolt), [Gmod-vphysics](https://github.com/DrChat/Gmod-vphysics), and [Valve Developer Wiki - VPhysics](https://developer.valvesoftware.com/wiki/VPhysics). Its output may not be fully consistent with what official Valve studiomdl produces. If you encounter incorrect physics shapes, unexpected behavior, or compilation differences, please report them as issues.

> [!NOTE]
> **"Skipped N flex vertex deltas with no matching model vertex" is normal on complex flex/morph characters.** When a model uses `$lod` decimation, verts removed at lower LODs no longer have anywhere for their flex/morph deltas to go, so those deltas are skipped during compile. This is expected and harmless - the affected verts no longer exist. It generally does not occur on simpler models such as props. Only worth investigating if a specific morph visibly loses movement in-game.


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
- Multiple engine branch support; use `-vtxformat 1` for the Alien Swarm/CS:GO engine branch VTX format (default 0 = TF2/L4D2; minimum supported branch is likely TF2 or SourceSDK2013)
- Variables can be defined outside of QC via `-defvar <name> <value>` (repeatable)
- `$include` supports a fallback search directory via `-includedir <dir>` (repeatable)
- DirectX 8 compile support, opt-out with `-nodx80` (similar to StudioMDL++)
- `$addsearchdir` now correctly applies to SMD/DMX source file lookup
- New `ignorescale` parameter for `$animation` and `$sequence`
- New `$driverbone` and `driverlookat` commands to define procedural bones without VRD files
- New `$rendermesh` to filter source geometry by DmeMesh object name or by material name; DmeMesh filtering requires DMX, `removematerial` works on any source format
- New `$if`, `$elif`, `$else`, and `$switch` conditional commands
- New `$staticproppose <animation_file> <frame>` to bake a custom pose into a `static_prop`'s geometry skeleton. Cannot be combined with `$staticprop`. **(Work in progress - does not behave as expected)**
- New `$return <optional message>` to halt compilation with an optional message (similar to `$qcassert`)
- New `$print <message>` for compile-time output
- New `$deltaproportions` to generate `a_reference` and `a_proportions`. See `docs/deltaproportions.md`
- New `$include` inline options:
  - `iffileexist` - silently skips the include if the file does not exist
  - `nofallbackdir` - disables the `-includedir` fallback for this include
- `$model` can now be used inside `$bodygroup` blocks, enabling named variants with per-variant sub-options (eyeball, flex, etc.). See `docs/bodygroup-model.md`
- Recreated features from StudioMDL++ and NekoMDL:
  - `-cullanims` flag to strip unreferenced `$animation` blocks
  - Bone weight cull threshold reduced from 5% to 0.01%
  - `$scale` now affects eyeball, eyelid, dmxeyelid, forceboneposrot, procedural bones, and VTA flex deltas
  - `$renamebone` now propagates to the collision model
  - Fixed crash with blank bodygroup combined with `$staticprop`

## TODO

- Fix mesh splitting with flex data - currently produces corrupted or broken output on models with shape keys or flex controllers
- Fully test `$collisionmodel` and `$collisionjoints` pipeline against official Valve studiomdl output to validate accuracy
- Linux native support

## Credits

- Valve - Source Engine and SDK
- [REDxEYE](https://github.com/REDxEYE) - base fork
- [ficool2](https://github.com/ficool2) - StudioMDL++ (Ideas based on the features their studiomdl version provided)
- [Starfelll](https://github.com/Starfelll) - NekoMDL
