# $driverbone

Creates one or more QUATINTERP procedural bones. Each helper bone is driven
by the rotation of a single "driver" bone: when the driver reaches a known
rotation (a "trigger"), the helper snaps/blends toward the matching pose.

Typical use: bicep-twist correction, forearm-roll, etc.

## Syntax

```
$driverbone <driver_bone> {
    triggerpose <file>

    trigger <tolerance_deg> <frame>
    ...

    [unlockbones]
    [noculltriggers]
    [autotrigger <tolerance_deg> [<min_frame> <max_frame>]]

    <helper_bone>
    ...
}
```

## Parameters

**`<driver_bone>`**

Name of the bone whose local rotation is read at runtime. This bone
must exist in the compiled skeleton AND in the triggerpose file.

**`triggerpose <file>`**

SMD or DMX animation file that defines what each trigger state looks like.
Every frame is sampled as follows:
- driver bone local rotation -> trigger quaternion (when to fire)
- helper bone local pos/quat -> target pose (what to snap to)

The file must contain at least as many frames as the highest frame
index referenced by `trigger` or `autotrigger`.

**`trigger <tolerance_deg> <frame>`**

Adds one trigger at the given frame of the triggerpose file.
- `tolerance_deg` - angular radius (degrees) within which this trigger is active. Smaller = sharper blend boundary.
- `frame` - zero-based frame index in the triggerpose animation.

Up to 32 triggers total (across `trigger` and `autotrigger` combined).
Explicit trigger entries always take priority over `autotrigger` for the same frame.

**`unlockbones`** (optional flag)

Remaps the trigger poses onto the target skeleton's actual bind pose
instead of treating the triggerpose values as absolute transforms.
Works like applying a new rest pose in Blender: the animation deltas
(relative to frame 0 of the triggerpose) are preserved, but they are
re-expressed on top of whatever bind pose the target skeleton actually
has (from `$definebone`, `$realignbones`, mesh bind pose, etc.).
Both position AND rotation are remapped this way.
Use this when sharing one triggerpose file across multiple rigs that
have different bone lengths, proportions, or bind orientations.

**`noculltriggers`** (optional flag)

By default the compiler checks all triggers for near-duplicates: if
two trigger quaternions (the driver bone's rotation at each frame) are
within 1 degree of each other, the later one is removed and a message
is printed. This prevents over-saturation of the QUATINTERP table,
which is common when using `autotrigger` on a dense animation.
`noculltriggers` disables this cleanup and keeps every trigger as-is.

**`autotrigger <tolerance_deg> [<min_frame> <max_frame>]`** (optional)

Auto-generates one trigger entry for every frame in the range
`[min_frame, max_frame]` (inclusive). If min/max are omitted the full
animation is used. All auto-generated triggers share the same
`tolerance_deg`. Any frame that already has an explicit trigger entry
is skipped. Useful when every frame is a valid pose and writing out individual
trigger lines would be repetitive, but one or two special frames
need a different tolerance.

**`<helper_bone>`** (one or more, unquoted bone names)

Each listed bone becomes an independent QUATINTERP entry pointing at
the same driver bone and the same trigger table. List as many as
needed; each must appear in both the compiled skeleton and the
triggerpose file.

## Examples

Forearm twist with two helper bones:

```
$driverbone "forearm_L" {
    triggerpose "reference/forearm_twist_poses.smd"
    trigger 20.0 0      // 0 deg twist  (rest)
    trigger 20.0 15     // 90 deg twist
    trigger 20.0 30     // 180 deg twist
    unlockbones
    "twist_upper_L"
    "twist_lower_L"
}
```

Finger curl with `autotrigger`:

```
$driverbone "finger1_L" {
    triggerpose "reference/finger_poses.smd"
    autotrigger 10.0 0 20
    trigger 5.0 10      // knuckle snap frame gets a tighter tolerance
    "finger1_helper_L"
}
```

---

# $driverlookat

Creates one or more AIMATBONE procedural bones. Each helper bone continuously
rotates to aim one of its local axes at a target bone or attachment point.
The parent bone is resolved automatically from the skeleton hierarchy.

Typical use: eye tracking, head following, dynamic aim correction.

## Syntax

```
$driverlookat <target> {
    [aimvector <x> <y> <z>]
    [upvector  <x> <y> <z>]
    [origin    <x> <y> <z>]

    <helper_bone>
    ...
}
```

## Parameters

**`<target>`**

Name of the bone or attachment the helper bones aim at. The compiler
checks attachments first; if no attachment is found by that name it
falls back to a bone lookup.

**`aimvector <x> <y> <z>`** (optional, default: `0 0 1`)

The local axis of the helper bone that points toward the target.
Does not need to be unit-length; the compiler normalizes it.

**`upvector <x> <y> <z>`** (optional, default: `1 0 0`)

The local axis of the helper bone used to resolve roll (up direction).
Does not need to be unit-length; the compiler normalizes it.

**`origin <x> <y> <z>`** (optional, default: `0 0 0`)

World-space offset applied to the target position before aiming.
Useful for nudging the look-at point without moving the target bone.
Affected by `$scale`.

**`<helper_bone>`** (one or more, unquoted bone names)

Each listed bone becomes an independent AIMATBONE entry, all aiming
at the same target with the same vectors.

## Examples

Two eye bones tracking a look-at attachment:

```
$driverlookat "eyes_target" {
    aimvector 0 1 0
    upvector  0 0 1
    "eye_L"
    "eye_R"
}
```

Head bone tracking a bone with a slight upward offset:

```
$driverlookat "lookat_bone" {
    aimvector 1 0 0
    upvector  0 0 1
    origin    0 0 2.0
    "head_aim"
}
```

---

# Notes

- Both commands are affected by `$scale` (position/offset values are scaled).
- `$driverbone` uses the first animation track in the triggerpose file.
  For DMX files with multiple tracks, only track 0 is sampled.
- Trigger count limit: 32 per helper bone (`$driverbone`).
- Helper bones that are optimised out of the skeleton by the compiler are
  silently skipped (a warning is printed unless `$quiet` is set).
