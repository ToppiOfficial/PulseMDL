# $deltaproportions

Generates two implicit single-frame animations (`a_reference` and
`a_proportions`) at compile time from a reference skeleton pose and an
optional proportion skeleton pose.

These two animations are the inputs for the proportion trick
([guide](https://steamcommunity.com/sharedfiles/filedetails/?id=2308084980)),
which allows one character model compiled with a shared skeleton to
stretch or compress its bones at runtime to match a character with different
limb lengths or body proportions.

## Syntax

```
$deltaproportions {
    referencepose  <file>  <frame>
    [proportionpose <file> <frame>]

    ["<bone_name>"  [ignore]
                    [offsetpos <x> <y> <z>]
                    [offsetangle <x> <y> <z>]
                    [ignorepos <x> <y> <z>]
                    [ignoreangle <x> <y> <z>]]

    [appendreference {
        <animation options>
    }]

    [appendproportions {
        <animation options>
    }]
}
```

## Parameters

**`referencepose <file> <frame>`** (required)

SMD or DMX animation file whose frame `<frame>` contains the REST POSE
of the BASE body model - the "normal" proportions that other sequences
and models are authored against. `<frame>` is zero-based.

**`proportionpose <file> <frame>`** (optional)

SMD or DMX animation file whose frame `<frame>` contains the REST POSE
of the PROPORTION model being compiled - i.e. the resized/reshaped
version of the skeleton. `<frame>` is zero-based.

If omitted, the bind pose of the first `$body`/`$model` source already
loaded in this QC is used. This is the common case: you are compiling
the proportion model itself, so its own skeleton IS the proportion pose.
`$body` or `$model` must therefore appear before `$deltaproportions` in the
QC when `proportionpose` is omitted.

**`"<bone_name>" [params...]`** (optional, repeatable)

Per-bone configuration. The bone name must match the reference skeleton
exactly (case-sensitive). Multiple params may appear on the same line.

- `ignore` - Exclude this bone from both output animations. The bone's bind
  pose values from the reference source are written into both
  `a_reference` and `a_proportions`, so the bone contributes no delta
  to the proportion sequence at runtime.

- `offsetpos <x> <y> <z>` - Translate the bone's local position in its own local bone axes by
  the given amount. Applied only to `a_proportions`. Useful for nudging
  a specific bone's resting position in the proportion model.

- `offsetangle <x> <y> <z>` - Rotate the bone's local orientation by the given euler angles
  (degrees, applied in ZYX order) in its own local frame. Applied
  only to `a_proportions`. Useful for correcting a bone's rest rotation.

- `ignorepos <x> <y> <z>` - Per-axis mask for world-space position blending.
  Each component is 0 or 1: `1` = keep the world position from the reference hierarchy,
  `0` = take the world position from the proportion pose.
  All zeros (or omitting this param) = full proportion position.
  All ones = same as `ignore` for position only.

- `ignoreangle <x> <y> <z>` - Per-axis mask for world-space rotation blending.
  Each component is 0 or 1: `1` = keep the world euler angle from the reference skeleton,
  `0` = take the world euler angle from the proportion pose.
  All zeros (or omitting this param) = full proportion rotation.

**`appendreference { ... }`** (optional)

Any valid `$animation` sub-parameters to apply to the generated
`a_reference` animation (fps, loop, delta, frame, ikrule, etc.).

**`appendproportions { ... }`** (optional)

Same as `appendreference` but for `a_proportions`.

## Algorithm

The two output animations are computed in two passes over the reference skeleton:

**Pass 1 - builds `a_reference`:**
For each bone that exists in both skeletons the world-space rotation
is taken from the proportion pose. Position is derived by walking down
the reference hierarchy with the new rotations (so children follow
their parent's new orientation). Bones absent from the proportion
skeleton keep their original rotation propagated through the new
parent chain.

**Pass 2 - builds `a_proportions`:**
Rotations are the same as pass 1. For each matched bone the
world-space position is now also taken from the proportion pose.
Unmatched bones derive their position by following their parent's
pass-2 transform with the original local offset, so they move
naturally with the re-proportioned hierarchy.

Both results are converted back to local space before being stored.

## Output animations

**`a_reference`** - The reference skeleton expressed with proportion-pose rotations.
Used as the blend-from pose in the proportion sequence.

**`a_proportions`** - The proportion skeleton with both rotations and positions from the
proportion pose. Used as the blend-to pose in the proportion sequence.

Both animations are registered as if written with `$animation` in the QC
and can be referenced by name in any `$sequence` block.

## Examples

Proportion model with explicit pose files:

```
$body "body" "meshes/body_tall.dmx"

$deltaproportions {
    referencepose  "anims/ref_body.dmx"   0
    proportionpose "anims/tall_body.dmx"  0
}

$sequence "proportions" {
    "a_proportions"
    delta
    autoplay
    subtract "a_reference" 0
    hidden
}
```

Proportion model using its own bind pose (most common):

```
// $body is loaded first so $deltaproportions can read its bind pose.
$body "body" "meshes/body_tall.dmx"

$deltaproportions {
    referencepose "anims/ref_body.dmx" 0
    // proportionpose omitted - uses body_tall.dmx bind pose
}

$sequence "proportions" {
    "a_proportions"
    delta
    autoplay
    subtract "a_reference" 0
    hidden
}
```

With `appendreference` / `appendproportions`:

```
$deltaproportions {
    referencepose "anims/ref_body.dmx" 0

    appendreference {
        fps 30
    }

    appendproportions {
        fps 30
        nummframes 1
    }
}
```

Per-bone overrides:

```
$deltaproportions {
    referencepose  "anims/ref_body.dmx"  0
    proportionpose "anims/tall_body.dmx" 0

    // lift the pelvis an extra 2.5 units in proportion space
    "ValveBiped.Bip01_Pelvis"  offsetpos 0 0 2.5

    // keep the head's reference rotation on the Z axis only
    "ValveBiped.Bip01_Head1"   ignoreangle 0 0 1

    // skip the weapon bone entirely
    "ValveBiped.weapon_bone"   ignore
}
```

## Notes

- Bone matching between the reference and proportion skeletons is by name
  (case-sensitive). Bones present only in the reference skeleton are carried
  through the hierarchy without modification.
- `$deltaproportions` must appear after any `$body`/`$model` it relies on for the
  implicit `proportionpose` fallback.
- The generated animations count toward the MAXSTUDIOANIMS limit (3000).
- The animation names `a_reference` and `a_proportions` are fixed.
