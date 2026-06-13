# $lod / $shadowlod

Defines level-of-detail entries for a model. Each block is processed in the
order it appears; later blocks correspond to lower-detail LOD levels. The
compiler merges all LOD geometry into a unified vertex buffer and writes the
final VTX/VVD/MDL output.

## Syntax

```
$lod <switchValue> {
    <options>
}

$shadowlod {
    <options>
}
```

## Parameters

**`<switchValue>`**

Distance metric at which the engine switches to this LOD. Must be a
positive float. The engine compares this value against a per-model LOD
metric; when the metric exceeds the switch value the next LOD becomes active.

**`$shadowlod`**

Special LOD used for cheap render-to-texture shadows (not shadow maps). Must
appear after all `$lod` entries. Only one is permitted. Facial animation is
disabled automatically. In the output the shadow LOD is stored as the last LOD
entry with `switchPoint = -1.0`, and `STUDIOHDR_FLAGS_HASSHADOWLOD` is set in
the MDL header flags. The engine excludes it from normal distance-based selection
and uses it only for RTT shadow rendering.

## Options

Valid inside both `$lod` and `$shadowlod`:

**`replacemodel <lodName> <replacementFile>`**

Substitutes a different source mesh for this LOD level. The replacement
mesh must be fully rigged - every vertex must have at least one bone
weight. A hard error is raised if any vertex carries no weights (skipped
for `$staticprop` models).

**`decimatemodel <meshName> <factor>`**

Auto-decimates the named LOD0 mesh using meshoptimizer. `<meshName>` must
match either the `$rendermesh` name or the base filename (no path, no
extension) of the source used by the body part or model. `<factor>` is in
the range `(0, 1.0]`:

- `1.0` - no change (identical to LOD0)
- `0.5` - target 50 percent of the original triangle count
- `0.25` - target 25 percent of the original triangle count

Multiple `decimatemodel` entries may appear in the same block, each targeting
a different mesh. The decimation is applied to the LOD0 geometry; for
nested LOD levels, the most recent LOD0 geometry is used as the source.

`$staticprop` models use `meshopt_SimplifyLockBorder` to prevent boundary
vertices from moving, which avoids seam artifacts on static meshes.

Meshes with flex/vertex animation (e.g. `$model` face meshes) are supported.
The decimated copy retains the full vertex buffer; the flex delta data
remains attached to the original vertex indices.

`removemesh` entries in the same block are respected: meshes marked for
removal are skipped before decimation.

**`removemesh <materialPath>`**

Removes all faces using the specified material from this LOD level and
below. `<materialPath>` is matched against the base filename (no path, no
extension) of the material name. Commonly used to strip edgeline or
outline meshes at lower LODs.

**`replacebone <boneName> <replacementBone>`**

Remaps all weights on `<boneName>` to `<replacementBone>` at this LOD level.

**`bonetreecollapse <boneName>`**

Collapses `<boneName>` and all of its children into the parent bone.

**`replacematerial <srcMaterial> <dstMaterial>`**

Substitutes a different material for this LOD level.

**`nofacial`**

Disables facial/flex animation at this LOD level.

**`facial`**

Enables facial/flex animation at this LOD level (regular `$lod` only; not
permitted inside `$shadowlod`).

**`use_shadowlod_materials`**

(Inside `$shadowlod` only.) Sets `STUDIOHDR_FLAGS_USE_SHADOWLOD_MATERIALS` in
the MDL header, signaling the engine to use shadow-specific materials
when rendering the shadow LOD.

## decimatemodel Notes

- The mesh name lookup uses the `$rendermesh` name for `$rendermesh`-based body
  parts, or the source filename base for `$body`/`$model` sources. The match is
  case-insensitive.

- For `$model` face meshes: the source has `m_GlobalVertices` empty at LOD
  source build time (it is populated later by `RemapVerticesToGlobalBones`).
  The decimation falls back to the `vertex[]` array, which is always populated
  by `BuildIndividualMeshes`. The `mesh[]` `vertexoffset`/`numvertices` values are
  consistent with `vertex[]` so the per-mesh position slice passed to
  meshoptimizer is correct.

- Face indices stored in `s_source_t` are mesh-local (`0..numvertices-1`),
  not global. meshoptimizer receives the per-mesh vertex slice and the
  per-mesh vertex count so that index ranges match.

- Multiple `decimatemodel` entries in one block do not alias each other's vertex
  buffers. The decimated `s_source_t` copies `m_GlobalVertices` with a zeroed
  `CUtlVector` header before assignment so that each copy gets an independent
  allocation; a shared buffer would become a dangling pointer when
  `RemapVerticesToGlobalBones` grows the original.

## Example

```
$rendermesh "body" "character.dmx" 0 {
    body_mesh
    cloth_mesh
}

$rendermesh "hair" "character.dmx" 0 {
    hair_mesh
}

$body body body
$body hair hair

$model "face" "face.dmx" {
    eyeball "eye_right" ...
    eyeball "eye_left"  ...
}

$lod 30 {
    decimatemodel body .50
    decimatemodel hair .50
    removemesh characters/models/mychar/edgeline
}

$lod 60 {
    decimatemodel body      .25
    decimatemodel hair      .25
    decimatemodel face      .70
    removemesh characters/models/mychar/edgeline
}

$shadowlod {
    decimatemodel body .25
    decimatemodel hair .25
}
```
