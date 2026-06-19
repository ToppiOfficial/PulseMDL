# $modelbudget

`$modelbudget` is a single block command that declares the model's intended
resource **budget**. Each parameter sets a *soft* limit: if the compiled model
exceeds it, compilation fails with a clear error naming the parameter. This lets
a modder lock a model to a self-imposed ceiling instead of silently growing up
to the compiler's hard maximum.

It replaces the older `$maxverts` and `$maxbones` commands, which have been
removed. Using either of them now produces an "unknown command" error.

## Soft limits vs. hard caps

Every parameter has two ceilings:

- **default** - the value used when the parameter is omitted from the block.
- **hard cap** - the compiler's absolute maximum (a compile-time `#define`,
  usually the size of an internal array). A value passed to `$modelbudget` is
  clamped to `[1, hard cap]`; you cannot raise a limit above its hard cap from
  a QC. To change a hard cap you must edit the corresponding `#define` (see the
  "BUDGET HARD CAPS" block in `include/studiomdl/studiomdl.h`) and rebuild.

If `$modelbudget` is not present at all, every limit keeps its default value, so
behavior matches the old defaults (256 bones, `MAXSTUDIOVERTS/3` split verts).

## Syntax

```
$modelbudget {
    totalverts       <int>
    bodyverts        <limit> [clamp]
    bones            <int>
    materials        <int>
    flexcontroller   <int>
    flexmorph        <int>
    flexmorphverts   <int>
    flexrules        <int>
    poseparam        <int>
    boneconstraints  <int>
    sequence         <int>
    animation        <int>
}
```

Any subset of parameters may be listed, in any order. An unknown parameter name
is a compile error.

## Parameters

| Param | Meaning | Default | Hard cap |
|---|---|---|---|
| `totalverts <int>` | Total render vertices across the whole model (all bodygroups and LOD0). | `MAXSTUDIOVERTS` | `MAXSTUDIOVERTS` |
| `bodyverts <limit> [clamp]` | Per-mesh vertex count before a `$body`/`$bodygroup studio`/`$model` mesh is split into multiple models. First value is the split trigger; optional second value is the per-chunk clamp (defaults to `min(limit, MAXSTUDIOVERTS/2)`). | `MAXSTUDIOVERTS/3` each | `MAXSTUDIOVERTS/2` |
| `bones <int>` | Maximum total bones after collapse. | 256 | `MAXSTUDIOBONES` (1024) |
| `materials <int>` | Maximum distinct materials/textures. | 32 | `MAXSTUDIOSKINS` (128) |
| `flexcontroller <int>` | Maximum flex controllers (input sliders). | 96 | `MAXSTUDIOFLEXCTRL` (256) |
| `flexmorph <int>` | Maximum flex descriptors (morph targets). Also bounds flex keys (= half of `MAXSTUDIOFLEXDESC`). | 1024 | `MAXSTUDIOFLEXDESC` (4096) |
| `flexmorphverts <int>` | Maximum vertices moved by flexes (per mesh). | 32768 | `MAXSTUDIOFLEXVERTS` (65536) |
| `flexrules <int>` | Maximum flex rules. | 1024 | `MAXSTUDIOFLEXRULES` (4096) |
| `poseparam <int>` | Maximum pose parameters. | 24 | `MAXSTUDIOPOSEPARAM` (64) |
| `boneconstraints <int>` | Maximum bone constraints (point / orient / aim / parent). Excludes jigglebones, twist bones and `$driverlookat` aimat bones, which are separate procedural-bone types. | 64 | `MAXSTUDIOBONECONSTRAINTS` (256) |
| `sequence <int>` | Maximum `$sequence` count. | 1524 | `MAXSTUDIOSEQUENCES` (4096) |
| `animation <int>` | Maximum `$animation` count. | 3000 | `MAXSTUDIOANIMS` (8192) |

`animframes` (frames per animation) is intentionally **not** budgetable and
stays fixed at `MAXSTUDIOANIMFRAMES` (5000).

## Example

```
$modelbudget {
    bones          80
    materials      8
    flexmorph      256
    sequence       64
}
```

A model exceeding any listed limit fails the build, e.g.:

```
ERROR: $modelbudget materials exceeded: model uses 12, budget 8
```

## Implementation notes

- The handler `Cmd_ModelBudget()` lives in `studiomdl/studiomdl_commands.cpp`
  and stores values into `g_StudioMdlContext` budget fields.
- `bodyverts`/`bones` reuse the existing enforcement paths
  (`ClampMaxVerticesPerModel` in `studiomdl_app.cpp`, the bone check in
  `simplify.cpp`).
- `flexmorphverts` is checked inline where flexed verts are counted
  (`simplify.cpp`).
- All other limits are validated once at the end of `SimplifyModel()` by
  `ValidateModelBudget()`, after all counts are final.
