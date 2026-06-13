# $staticproppose

Bakes a custom skeletal pose (and optionally flex shapes) into a model's geometry,
then compiles it as a static prop. Combines the pose-baking step of `$staticproppose`
with the full static prop pipeline (`$staticprop` flags, skeleton collapse, animation
stripping, default rotation, attachment recalc).

Cannot be combined with `$staticprop`.

## Syntax

Block form (recommended):

```
$staticproppose {
    pose <file> <frame>
    flex <name> [<value>]
    flex <name> [<value>]
    ...
}
```

Inline form (legacy, no flex support):

```
$staticproppose <file> <frame>
```

## Subcommands

**`pose <file> <frame>`**

Required in block form. Loads `<file>` (`.smd` or `.dmx`) as the animation source
and samples bone transforms at `<frame>` (zero-based). Those transforms are
skinned onto all body source vertices, baking the pose into the geometry
before the skeleton is collapsed.

The bone names in `<file>` must match the bone names in the body sources. Any
bone present in the body source but absent in `<file>` keeps its bind pose.

**`flex <name> [<value>]`**

Optional, repeatable. Blends the named delta state (flex shape) into the base
mesh before flex data is stripped. `<name>` is the delta state name as it appears
in the DMX / source file (same name shown in a `$flexcontroller` block).

`<value>` is a float in `[0.0, 1.0]`. Omitting it defaults to `1.0` (full activation).
Values are clamped to `[0.0, 1.0]`.

Both position and normal deltas are applied. Use this to bake face poses or
corrective shapes into the final static geometry.

## Examples

Basic pose bake - frame 30 of an SMD:

```
$staticproppose {
    pose "animations/my_pose.smd" 30
}
```

Pose bake with a half-open mouth and raised brows:

```
$staticproppose {
    pose "animations/face_pose.dmx" 0
    flex "mouth_open" 0.5
    flex "brow_raise"
}
```

Legacy inline form (pose only, no flex):

```
$staticproppose "animations/my_pose.smd" 30
```

## Notes

- `$staticproppose` must appear after all `$body` / `$bodygroup` / `$model` lines since
  it triggers the full compile pipeline immediately when parsed.

- The pose source does not need to be referenced by any `$sequence` or `$animation`.
  It is loaded only to sample bone transforms for the bake.

- `$nosequence` can be used together with `$staticproppose`. A dummy BindPose sequence
  is injected automatically so no `$sequence` line is needed.

- Flex names are matched against source animation names (delta state names), not
  flex controller names. For stereo shapes the left/right split names are used
  (e.g. `"smile_L"`, `"smile_R"`), not the controller name (`"smile"`).

- If a flex name is not found in any body source a warning is printed and that
  entry is skipped; compilation continues normally.

- The transform fix: bone positions from the pose source are evaluated at the
  same scale as the mesh vertices (`$scale` / `g_currentscale`). Models compiled
  without a custom `$scale` are unaffected.
