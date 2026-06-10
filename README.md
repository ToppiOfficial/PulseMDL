# KitsuneMDL (studiomdl_v2)

> **Experimental**

A standalone fork of Valve's StudioMDL compiler based on [REDxEYE/studiomdl_v2](https://github.com/REDxEYE/studiomdl_v2).

## Requirements

`$collisionmodel` and `$collisionjoints` require the following DLLs placed alongside the executable, sourced from TF2's `bin/x64/` folder: (Not sure if other 64-bit source engine game can work)

- `vphysics.dll`
- `tier0.dll`
- `vstdlib.dll`


`DMX model 18` but lower version can maybe still work

## TODO

- Remove the dependency for vphysics.dll (but still able to compile collisionjoints and collisionmodel) and dmxconvert.exe
- Explore the issue stated by REDxEYE about mesh split corrupting flex vertices.
- Further clean and remove "unused/dead code"
- Improve the loading and fix some crashes when dealing with large SMD model

## Features

- 64 Bit
- Increased some limit
  - Bone limit 256 -> 1024
  - Texture limit 32 -> 64
  - Flex vertices limit 10000 -> 32768
  - Mesh vertices limit 21845 -> 174762 (Overall model mesh limit: 65536 -> 524288)
- Replaced nvstrip with meshoptimizer
- Minor code changes for faster compile
- Change bone collapsing optimization behavior, Bone will not collapse if it is within this criteria:
  - If bone is Jigglebone
  - If bone is procedural bone/ driver bone
  - If bone is $definebone
  - If bone is $bonemerge
  - If bone is BoneFlexDriver
- `$definevariable` now work inside quotation but `$definemacro` does not.
- Multiple engine branch support but requires additional launch parameter `-newvtx` for Alien Swarm to CS:GO Engine Branch. (The minium is likely Team Fortress 2 or SourceSDK2013)
- variables can now be defined outside of qc through `-definevariable <var name> <value>` launch parameter. (can be repeated)
- `$include` now has a fallback directory if it is not found on the specified path using the new launch parameter `-includedir <dir>`. (can be repeated)
- Can compile for DirectX8 and can be opted out with `-nodx80` similar to StudioMDL++
- `$addsearchdir` now works correctly for SMD/DMX source file lookup
- New `ignorescale` parameter for `$animation` and `$sequence`
- New `$driverbone` and `driverlookat` to create procedural bone without the need for VRD files
- New `$rendermesh` for DMX models containing multiple DMEMesh elements
- New `$if $elif $else` and `$switch` conditional commands.
- New `$staticproppose <animation_file> <frame>` to bake a custom pose into the geometry skeleton to a single `static_prop`. Cannot be used together with `$staticprop`. (Doesn't work as expected!) (WORK-IN-PROGRESS)
- New `$return <optional message>` and `$print <message>`.  `$return` stops the compile with optional message similar to `$qcassert`.
- New `$deltaproportions` to generate the `a_reference` and `a_proportions`. See `docs/deltaproportions.txt`.
- New `$include` optional inline parameters:
  - `iffileexist` checks if file exist, if it doesn't then silently pass and ingore.  Does not stop the compile.
  - `nofallbackdir` doesn't use `-includedir` fallback
- Recreated some features from StudioMDL++ and NekoMDL:
  - `-cullanims` flag to strip unreferenced `$animation` blocks
  - Bone weight cull threshold reduced from 5% to 0.01%
  - `$scale` now affects eyeball, eyelid, dmxeyelid, forceboneposrot, procedural bones, and VTA flex deltas
  - `$renamebone` now propagates to the collision model
  - Fixed crash with blank bodygroup + `$staticprop` (TODO: Test)

## Credits

- Valve — Source Engine and SDK
- [REDxEYE](https://github.com/REDxEYE) — base fork
- [ficool2](https://github.com/ficool2) — studiomdl++ fixes ideas
- [Starfelll](https://github.com/Starfelll) — NekoMDL fixes ideas
