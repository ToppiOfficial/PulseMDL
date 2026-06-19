# $transformbindposebone

Edit a bone's **rest/bind pose** without deforming the mesh at rest (by default)
and without changing how existing animations look - only the reference (bind) pose
moves. This is the re-rigging primitive: shift or reorient a joint's pivot in place
and let the engine deform around it during animation.

`$transformbindposebone` replaces the older `$rotatebone` and `$movebone` commands,
merging both into one command with keyword parameters. The `moveweight` clause is
now `transformweights`, and the per-animation `ignorebonemove`/`ignorebonerotate`
flags are now `ignoretransformbindpose position` / `ignoretransformbindpose angles`.

`$transformbindposebone` is **global / position-independent** in the QC: it does not
matter where it appears, it always affects geometry and animations declared both
before and after it. Edits are collected and applied once, late in compilation
(after `$body`/`$bodygroup`/`$model`, `$sequence`/`$animation`, `$deltaproportions`,
and `$definebone`/`$unlockdefinebones`). If `$realignbones` is present, the bind-pose
edits are applied **first**, so the realign is computed on the edited skeleton.

There is a combined budget of **64** edits. The 65th `$transformbindposebone` is a
hard error.

## Syntax

```
$transformbindposebone "<bonename>" [params...]
```

Parameters are order-independent and may be combined on one statement. At least one
of `angles` or `position` is required.

| Param | Meaning |
|---|---|
| `angles <rx ry rz>` | Rotate the bind orientation (Euler, same convention as `$definebone`). Pivots about the bone's own origin; the position is not displaced. |
| `position <x y z>` | Translate the bind position. |
| `worldangles` | `angles` are applied about world-aligned axes (default is the bone's **local** axes). |
| `worldposition` | `position` is along world/model axes (default is the bone's **local** axes). |
| `transformweights <residualbone> [factor]` | Re-skin: transfer `factor` of the bone's vertex weight to `<residualbone>`. See below. |
| `transformverts` | Rigidly carry the bone's rigged vertices with the bone at rest (the rest mesh deforms). Mutually exclusive with `transformweights`. |
| `ignoreanimation` | This edit does not flow into any `$sequence`/`$animation` frame. |
| `ignorehitbox` | Keep this bone's manual `$hbox` hitboxes at their original world location. |

> **Default-space change from the old commands:** the old `$movebone` defaulted to
> **world** translation and the old `$rotatebone` to **local** rotation. The unified
> command defaults **both** `angles` and `position` to **local**; use
> `worldangles`/`worldposition` to opt into world axes.

```
$transformbindposebone "ValveBiped.Bip01_L_UpperArm" angles 0 0 15
$transformbindposebone "ValveBiped.Bip01_Head1" angles 10 0 0 worldangles
$transformbindposebone "ValveBiped.Bip01_Pelvis" position 0 0 2 worldposition
$transformbindposebone "ValveBiped.Bip01_L_Hand" position 1 0 0     # local (default)

# rotate and move a joint in one statement
$transformbindposebone "ValveBiped.Bip01_Spine2" angles 0 0 -5 position 0 0 1
```

Bone names are **case-insensitive**.

## transformweights `<residualbone> [factor]`

The bind-pose edit on its own is **weight-preserving** - it relocates the pivot
without re-skinning anything, so it never changes which bones drive a vertex.
`transformweights` is the opt-in to actually **re-skin**: every vertex weighted to
the edited bone has `[factor]` of that influence **transferred to `<residualbone>`**,
and its weights are renormalized to 1.0. If the vertex already has `<residualbone>`,
the transferred share merges into it.

`[factor]` is **optional** (default **`0.55`**), clamped to `[0, 1]`:

- **`1.0`** - full transfer; the edited bone's influence moves entirely to the residual.
- **`0.5`** - half stays, half goes to the residual (a blend).
- **`0.0`** - no transfer (a no-op).

Because the rest position was already baked from the original weights, the **rest
mesh stays visually identical** - only the animation deformation of the re-skinned
fraction now follows `<residualbone>`.

```
$transformbindposebone "ValveBiped.Bip01_L_Forearm" position 0 0 -1 transformweights "ValveBiped.Bip01_L_UpperArm" 1.0
```

> `transformweights` is the experimental part of this feature. The default path (no
> `transformweights`/`transformverts`) preserves the mesh fully; validate
> `transformweights` on a flex-free test model before relying on it.

## transformverts

`transformverts` makes the edit **rigidly carry** the bone's rigged vertices at rest:
instead of relocating the pivot only (the default, where the rest mesh stays put),
the vertices weighted to the bone move/rotate **with** the bone, deforming the rest
mesh. Animations stay on the un-edited bind frame for that edit (a `transformverts`
edit is a rest-pose authoring change, not an animation re-expression).

`transformverts` and `transformweights` cannot be combined on the same statement.

```
$transformbindposebone "ValveBiped.Bip01_L_Toe0" angles 0 0 20 transformverts
```

## How it works

Each edit is expressed as a change of the bone's bind frame, delta
`D = boneToPose^-1 * newBoneToPose`. By default `D` is folded into the bone's
`srcRealign`, which is applied to **both** vertex binding
(`RemapVerticesToGlobalBones`) and every animation frame
(`ConvertAnimation`/`RemapAnimations`), so the rest mesh and all animation visuals
stay consistent automatically. Direct children are re-expressed relative to the new
parent frame, so the subtree below the edited bone stays world-fixed at rest. Delta
animations and `$deltaproportions` flow through the same path and need no special
handling.

With `transformverts`, `D` is folded into `boneToPose` but **not** `srcRealign`, so
the rest vertices follow the bone while animations stay on the un-edited frame.

## Keeping hitboxes in place: `ignorehitbox`

By default a bone edit also moves that bone's hitboxes (they are rigidly attached to
the bone). Append `ignorehitbox` to keep the bone's **manual** `$hbox` hitboxes at
their original world location - the compiler compensates the hitbox by the inverse of
the edit:

- a `position` edit shifts the hitbox `bmin`/`bmax` the opposite way;
- an `angles` edit writes the inverse rotation into the hitbox orientation.

```
$transformbindposebone "ValveBiped.Bip01_L_Foot" position 0 0 -2 worldposition transformweights "ValveBiped.Bip01_L_Calf" 1.0 ignorehitbox
$transformbindposebone "ValveBiped.Bip01_Head1" angles 0 0 15 ignorehitbox
```

> `ignorehitbox` only affects **manually authored** `$hbox` hitboxes. Auto-generated
> hitboxes are recomputed from the edited skeleton and already stay wrapped around the
> (unmoved) mesh, so the keyword is a no-op for them.

## Opting specific animations out: `ignoretransformbindpose` / `ignoreanimation`

By default these bind-pose edits flow into **every** animation (the world motion is
preserved, only re-expressed relative to the new bind frame).

To convert a specific animation as if an edit never happened, two opt-in
per-animation flags suppress the edits for that animation:

- **`ignoretransformbindpose position`** - convert this animation as if `position` edits never happened.
- **`ignoretransformbindpose angles`** - convert this animation as if `angles` edits never happened.

They are valid anywhere `$animation` options are accepted: directly on `$animation`
and inside a `$sequence` block. Both may appear to opt out of both categories.

```
$sequence "walk" {
    "anims/walk.dmx"
    ignoretransformbindpose position
    ignoretransformbindpose angles
}
```

To opt **all** animations out of an edit at once, put `ignoreanimation` on the
`$transformbindposebone` itself - the edit then affects only the rest pose, never any
animation.

## Behavior notes

- Bones are resolved after `$renamebone` and bone collapse, so a name that was
  collapsed away (or never existed) warns and is skipped - the compile continues.
- Multiple edits compose in QC order; editing a parent then a child is supported.
- A root bone (no parent) is valid.
- Affects the `-definebones` export, since edits are applied before that dump.
- Ragdoll collision (`$collisionjoints`) follows the edits: a moved/rotated bone's
  physics hull stays aligned with its mesh. Single-body `$collisionmodel` is a rigid
  model-space hull and is unaffected by per-bone edits.

## See also

- [procedural-bones.md](procedural-bones.md)
