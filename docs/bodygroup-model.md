# $model inside $bodygroup

Allows a bodygroup variant to be declared with a logical name and optional
per-variant sub-options (eyeball, flex, etc.), the same way a standalone
`$model` works but without creating a separate bodypart.

## Syntax

```
$bodygroup "<name>"
{
    $model "<variant_name>" "<filename>" [flags]
    {
        eyeball ...
        flex ...
        flexcontroller ...
        ...
    }
    $model "<variant_name>" "<filename>"
    studio "<filename>"
    blank
}
```

The block after the filename is optional. When omitted, no braces are needed.

Token order inside `$bodygroup`:

```
$model  <variant_name>  <filename>  [inline flags]  [{ sub-options }]
```

This matches the token order of the standalone `$model` command.

## Sub-options

The optional block accepts the same sub-options as standalone `$model`:

```
eyeball         <name> <bone> <x> <y> <z> <texture> <zoffset> <yawrange> <irisscale>
eyelid          ...
dmxeyelid       ...
flex            <name> <vtafile>
flexpair        <name> <split> <vtafile>
defaultflex     <vtafile>
flexfile        <vtafile>
flexcontroller  <type> <name> <min> <max>
localvar        <name> [<name> ...]
mouth           ...
vcafile         <file>
spherenormals   <bone> <x> <y> <z> <r>
noautodmxrules
%<flexrule>     ...
```

## Difference from studio

| Syntax | Behavior |
|---|---|
| `studio "<filename>"` | loads a mesh, no name, no sub-options |
| `$model "<name>" "<filename>" { ... }` | loads a mesh with a logical variant name and supports the full set of per-model sub-options |

Both can be freely mixed with `blank` in the same bodygroup block.

## Difference from standalone $model

Standalone `$model` creates its own implicit single-model bodypart. `$model`
inside `$bodygroup` adds to the bodygroup being defined and does not create
an extra bodypart.

## Examples

Simple named variants with no sub-options:

```
$bodygroup "head"
{
    $model "head_default" "head_default.dmx"
    $model "head_hat"     "head_hat.dmx"
    blank
}
```

Per-variant eyeball definitions:

```
$bodygroup "head"
{
    $model "head_a" "head_a.dmx"
    {
        eyeball "righteye" "ValveBiped.Bip01_Head1" 1.0 2.28 0.0 "eye_r" 0 60 0.5
        eyeball "lefteye"  "ValveBiped.Bip01_Head1" -1.0 2.28 0.0 "eye_l" 0 60 0.5
    }
    $model "head_b" "head_b.dmx"
    {
        eyeball "righteye" "ValveBiped.Bip01_Head1" 1.0 2.28 0.0 "eye_r" 0 60 0.5
        eyeball "lefteye"  "ValveBiped.Bip01_Head1" -1.0 2.28 0.0 "eye_l" 0 60 0.5
    }
}
```

Mixing `$model`, `studio`, and `blank`:

```
$bodygroup "torso"
{
    $model "torso_detailed" "torso_hq.dmx"
    {
        flexcontroller wrinkle "chest_compress" range 0 1
    }
    studio "torso_lq.smd"
    blank
}
```

## Notes

- The variant name is a logical identifier stored in the compiled model. It
  does not need to match the filename.
- Bodygroup selection index is determined by declaration order, same as `studio`
  and `blank` entries.
- `$model` can appear at any position within the bodygroup block, in any
  combination with `studio` and `blank`.
