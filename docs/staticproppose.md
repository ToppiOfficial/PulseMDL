# $staticproppose

Bakes a custom skeletal pose (and optionally flex shapes) into a model's geometry,
then compiles it as a static prop. Combines the pose-baking step of `$staticproppose`
with the full static prop pipeline (`$staticprop` flags, skeleton collapse, animation
stripping, default rotation, attachment recalc).

Cannot be combined with `$staticprop`.

## Syntax

```
$staticproppose {
    pose <file> <frame>
    flex <name> [<value>]
    flex <name> [<value>]
    ...
}
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

## Notes

- `$staticproppose` must appear after all `$body` / `$bodygroup` / `$model` lines since
  it triggers the full compile pipeline immediately when parsed.

- The pose source does not need to be referenced by any `$sequence` or `$animation`.
  It is loaded only to sample bone transforms for the bake.

- Because the pose source is used only for its bone transforms, any special DME
  elements it carries - jigglebones and bone constraints (twist / point / orient /
  aim / parent) - are ignored and do not contribute to the compiled model.

- The skeleton is collapsed into a single `static_prop` bone, so special DME elements
  that depend on the original bones are stripped: hitboxes, jigglebones, and any
  attachments that came from the source DMX/SMD (e.g. weapon-bone slots). Attachments
  you define in the QC - including `$attachmentbyverts` and the `placementOrigin` from
  `$autocenter` - are kept. (Plain `$staticprop` keeps its attachments unchanged.)

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
