# $attachmentbyverts

Auto-generates an attachment from the geometry of a model, so you don't have to
hand-tune the `rotate` angles and position of an attachment. It is the single
replacement for the old `$aligneyes` and `$alignmouth` commands.

It collects a set of vertices using one or more **selectors** (combined as a
union - a vertex is collected if it matches *any* selector), then:

- computes the attachment **origin** as the centroid of the collected vertices
  (plus an optional `offset`),
- computes its **forward direction** either from a literal world-space vector you
  supply, or from the averaged normals of the collected vertices, and
- picks the **parent bone** as either an explicit `bone` you name, or the bone the
  collected vertices are weighted to most.

The result is computed in world space and stored as an absolute attachment, so it
is correct regardless of how the parent bone happens to be oriented.

This replaces having to write something like:

```
$attachment "eyes" "ValveBiped.Bip01_Head1" 2.04 -3.5 0 rotate 0 -80.1 -90
```

## Syntax

```
$attachmentbyverts "<name>" {
    morph "<flex morph name>"       // repeatable
    flexgroup "<flexgroup/type>"    // repeatable
    materials "<material name>"     // repeatable
    boneweight "<bone>" <min>       // repeatable
    bone "<bone name>"              // optional - explicit parent bone override
    forward <x y z>                 // optional
    offset  <x y z>                 // optional
}
```

- **`<name>`** - the name of the generated attachment (e.g. `"eyes"`).
- **`morph "<name>"`** - collect the vertices deformed by the named flex morph
  (matched against the flex descriptor / FACS name). Repeatable.
- **`flexgroup "<name>"`** - collect the vertices deformed by any flex driven by a
  flex controller whose `type` (group) matches `<name>`. The driven flexes are
  resolved by walking the flex rules. Repeatable.
- **`materials "<name>"`** - collect the vertices assigned to the named material
  (matched by basename, extension-insensitive). Repeatable. This is how you do the
  old `$aligneyes` behaviour: point it at the eye material.
- **`boneweight "<bone>" <min>`** - collect the vertices whose skin weight to
  `<bone>` is at least `<min>`. Repeatable.
- **`bone "<name>"`** *(optional)* - force the generated attachment's parent bone.
  If omitted, the parent is the bone the collected vertices are weighted to most.
- **`forward <x y z>`** *(optional)* - a literal **world-space** direction the
  attachment points along. Source models are typically X-forward / Z-up, so
  `1 0 0` aims it down world +X. If omitted or `0 0 0`, the direction is taken
  from the **averaged normals** of the collected vertices.
- **`offset <x y z>`** *(optional, default `0 0 0`)* - a **world-aligned** offset
  added to the computed centroid.

At least one selector (`morph`/`flexgroup`/`materials`/`boneweight`) is required.

## Migration from `$aligneyes` / `$alignmouth`

| Old | New |
|---|---|
| `$aligneyes "eyes" { eyeball "eye_l" ... }` | `$attachmentbyverts "eyes" { materials "eye_material" }` |
| `$alignmouth "mouth" { flexgroup "mouth" }` | `$attachmentbyverts "mouth" { flexgroup "mouth" }` |
| `$alignmouth "mouth" { flexcontroller "jaw_drop" }` | `$attachmentbyverts "mouth" { morph "jaw_drop" }` |

The dedicated `eyeball` selector is gone - an eyeball is just a material region,
so use `materials` with the eye's material name.

## Examples

```
// eyes: centered on the eye material, aimed forward
$attachmentbyverts "eyes" {
    materials "eyeball_l"
    materials "eyeball_r"
    forward 0 1 0
}

// mouth: centered on the verts driven by the "mouth" flex group
$attachmentbyverts "mouth" {
    flexgroup "mouth"
}

// precise: verts in a material that are also weighted to a bone, explicit parent
$attachmentbyverts "muzzle" {
    materials "head"
    boneweight "jaw" 0.5
    bone "jaw"
}
```

## Ordering

The attachment slot is reserved at the point `$attachmentbyverts` appears in the
QC, so the generated attachment keeps its declaration order relative to
surrounding `$attachment` commands.

## Notes / limitations

- At least one selector must be specified.
- Referencing a morph, flexgroup, material, or bone name that doesn't exist is a
  hard error, as is a selector set that collects no vertices.
- When relying on averaged normals (`forward` omitted), degenerate/cancelling
  normals will error - supply an explicit `forward` in that case.
- `morph`/`flexgroup` read from the post-simplify LOD vertex pool; `materials`/
  `boneweight` read from the source meshes. Both feed the same centroid.
