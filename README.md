# PulseMDL

![status](https://img.shields.io/badge/status-beta-yellow) ![platform](https://img.shields.io/badge/platform-Windows%20x64-lightgray)

> **Experimental**

A modified standalone fork of Valve's StudioMDL compiler, focused on advanced character modding workflows (facial flexes, procedural bones, bodygroup variants, proportion rigs).

Primarily tested with `DMX model 18`. Older versions may still work.

> [!IMPORTANT]
> This project is still a work in progress. The codebase inherits a large amount of Valve's original StudioMDL source and still requires extensive cleanup of unused/dead code, and large portions are likely to be rewritten. Expect instability and breaking changes.

## Tested Source Engine Branch
 
| Game | Test Result |
|---|---|
| Garry's Mod | ✅ Fully Supported |
| Left 4 Dead 2 | ✅ Fully Supported |
| Half-Life 2 | ✅ Fully Supported |
| Counter-Strike: Source | 🟡 Partially tested |
| Team Fortress 2 | ❌ Untested |
| Source Filmmaker | ✅ Fully Supported |
| Portal 1 & 2 | ❌ Untested |

## Features & Changes

A full, categorized list of everything that differs from Valve's original StudioMDL - **New** features, **Changes** to existing behavior, and **Known Issues** - lives in the wiki:

- **[Changes from stock StudioMDL](../../wiki/Changes)** (New / Changes / Known Issues)
- **[Wiki home](../../wiki)** for per-command documentation

> **Note:** New QC commands that are not yet documented on the wiki are not finalized. Their syntax, behavior, and naming are subject to change or removal in future versions.


## Credits

- Valve - Source Engine and SDK
- [REDxEYE](https://github.com/REDxEYE) - base fork [REDxEYE/studiomdl_v2](https://github.com/REDxEYE/studiomdl_v2)
- [ficool2](https://github.com/ficool2) - StudioMDL++ (Ideas based on the features their studiomdl version provided)
- [Starfelll](https://github.com/Starfelll) - NekoMDL (Ideas based on the features their studiomdl version provided)
