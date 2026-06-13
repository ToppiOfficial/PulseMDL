# $rendermesh

Defines a named mesh filter that can be used in place of a DMX filename in
`$body`, `$bodygroup` (`studio`), and `$model`. Each definition selects a subset of
the DmeMesh objects inside a DMX file without modifying the source file or
duplicating it on disk.

Only DMX sources are supported. SMD, OBJ, and VRM sources are skipped with
a warning.

## Syntax

```
$rendermesh <name> <file> <defaultState> {
    "<meshName>"
    ...
}
```

## Parameters

**`<name>`**

Identifier used to reference this definition from `$body`, `$bodygroup`,
or `$model`. Must be unique within the QC file.

**`<file>`**

Path to the DMX source file, relative to the current `$cd` directory.
Same rules as a normal `$body` filename.

**`<defaultState>`** (`0` or `1`)

Controls what happens to meshes not listed in the block:
- `0` - all meshes are hidden by default; only listed meshes are shown.
- `1` - all meshes are shown by default; only listed meshes are hidden.

**`"<meshName>"`** (one or more)

Name of a DmeMesh object inside the DMX file. Each listed mesh takes
the state opposite to `defaultState`:
- `defaultState 0` - listed mesh is shown (isolate)
- `defaultState 1` - listed mesh is hidden (exclude)

The compiler errors if a listed name does not exist in the DMX.

## Usage in $body / $bodygroup / $model

Anywhere a DMX filename would normally appear, write the `$rendermesh` name
instead. The compiler resolves it, loads the underlying DMX (using the
normal source cache), clones the geometry, and applies the filter before
the model is processed.

```
$body  <partName>  <rendermeshName>
studio <rendermeshName>
$model <partName>  <rendermeshName>
```

Multiple body parts or `studio` entries can reference the same `$rendermesh`
name or the same DMX file through different `$rendermesh` definitions.
Each gets its own independent copy of the geometry.

## Examples

Two body parts from one DMX, each showing a different mesh subset:

```
$rendermesh hair_mesh "thirdperson_model.dmx" 0 {
    "c_hair_lod0"
    "c_hair_lod0_edgeline"
}

$rendermesh body_mesh "thirdperson_model.dmx" 1 {
    "c_hair_lod0"
    "c_hair_lod0_edgeline"
}

$body hair hair_mesh
$body body body_mesh
```

Bodygroup with two filtered variants of the same file:

```
$rendermesh outfit_A "character.dmx" 0 {
    "outfit_A_mesh"
}

$rendermesh outfit_B "character.dmx" 0 {
    "outfit_B_mesh"
}

$bodygroup outfit {
    studio outfit_A
    studio outfit_B
    blank
}
```

## Notes

- `$rendermesh` is a definition only; it does not load the DMX at parse time.
  Loading happens when the name is first encountered in `$body`/`$bodygroup`/`$model`.
- The underlying DMX file is still loaded through the normal source cache.
  The filtered copy is separate and does not affect other references to the
  same file.
- Vertex animations (blend shapes) are remapped to match the filtered vertex
  set automatically.
- `$rendermesh` has no effect on `$sequence`, `$animation`, or any other command.
