# Auto-generated collision ($generate / $generatejoint)

These sub-tokens let the compiler auto-generate convex collision from render
geometry using convex decomposition (VHACD), instead of requiring a hand-authored
collision mesh. They are useful for extra geometry - e.g. a backpack added to a
character - that was never modeled in your 3d editor.

The geometry to decompose is supplied through a [`$rendermesh`](rendermesh.md)
definition. A `$rendermesh` is **required**: you cannot generate from the
`$collisionmodel` / `$collisionjoints` source itself, because that source is
treated as an already-finished (premade) collision hull.

Generated convex pieces are fed into the same `minicollision` pipeline as normal
collision, so Source's collision budget and `.phy` format apply unchanged.

## Where they go

`$generate` and `$generatejoint` are sub-tokens used **inside the `{ }` block** of
`$collisionmodel` or `$collisionjoints`. They sit alongside `$mass`, `$concave`,
`$jointconstrain`, etc.

```
$collisionjoints "ragdoll.smd" {
    $mass 80
    $generatejoint "spine" "backpack" concavity 0.05 hull 4
}
```

If you want to generate collision without any premade source mesh, pass `"blank"`
(or `""`) as the source:

```
$collisionmodel "blank" {
    $generate "backpack" concavity 0.05 hull 4
    $mass 5
}
```

## Syntax

All options after the required positional argument(s) are `keyword value` pairs.
They are optional, may appear in any order, and fall back to defaults if omitted.

Single rigid body (in a `$collisionmodel` block):

```
$generate <rendermesh> [concavity <f>] [hull <i>] [maxverts <i>]
```

Per joint (in a `$collisionjoints` block, may repeat for different bones):

```
$generatejoint <bone> <rendermesh> [concavity <f>] [hull <i>] [maxverts <i>] [cullweight <f>]
```

Example:

```
$generatejoint "Hair_L01" "haircollide" concavity 0.02 hull 1 cullweight 0.5
```

If the target bone already has collision from the `$collisionjoints` source mesh,
the generated collision replaces it. This lets you share a single base collision
SMD across characters and regenerate just the bones that deviate:

```
$collisionjoints "base_ragdoll.smd" {
    $generatejoint "Hair_L01" "haircollide"
}
```

## Parameters

**`<bone>`** (`$generatejoint` only)

Name of the bone the generated collision should attach to. The render mesh must
contain geometry weighted to this bone, and the bone must exist in the compiled
model. The generated hulls are placed in that bone's local space so they track the
bone at runtime. If the bone already has a collision solid (from the
`$collisionjoints` source), the generated hulls are added to it; otherwise a new
solid is created for the bone.

**`<rendermesh>`**

Name of a `$rendermesh` definition declared earlier in the QC. Its filtered
geometry (DmeMesh isolate/exclude, `removematerial`, `nofacial`) is what gets
decomposed.

**`concavity <f>`** (float `0.0` - `1.0`, default `0.04`)

Quality knob. **Lower** values produce a tighter fit with **more** convex pieces;
**higher** values produce a coarser approximation with **fewer** pieces. Internally
this maps to the decomposer's allowed volume error (about 0.1% at `0.0` up to ~10%
at `1.0`). A value around `0.02` - `0.1` is a reasonable starting point.

**`hull <i>`** (int, default `1`)

Hard cap on the number of convex pieces produced for this request. Source has a
strict per-solid collision budget; keep this modest. `hulls` is accepted as an
alias. If a request exceeds the collision block's `$maxconvexpieces` limit, a
`COSTLY GENERATED COLLISION` warning is printed (the pieces are still written).

**`maxverts <i>`** (int, default `16`)

Maximum number of vertices in each generated convex hull - this is the **detail /
polygon count** knob. **Lower** values produce simpler, lower-poly hulls (and are
easier for the physics packer to seal); **higher** values follow the source shape
more closely. `verts` is accepted as an alias. Note this is separate from `hull`,
which controls the *number* of pieces, not the detail of each piece. Minimum `4`.

**`cullweight <f>`** (float `0.0` - `1.0`, default `0.42`, `$generatejoint` only)

Minimum bone weight a vertex must have to be included in this joint's hull. A
vertex whose weight to `<bone>` is **below** this value is skipped, and triangles
are kept only when **all three** of their vertices pass. `cull` is accepted as an
alias.

This tightens each joint's hull to geometry that is genuinely driven by that bone
and stops neighbouring joints (e.g. `Hair_L01` and `Hair_L02`) from both claiming
the soft skin-blend region between them, which otherwise makes their hulls overlap
and collide. The default `0.42` keeps vertices the bone is roughly the dominant
influence on while still tolerating typical 3-bone blends. Set `0.0` to include any
vertex that touches the bone at all; raise toward `1.0` for tighter, more separated
hulls (at the risk of leaving gaps where a strand blends across many bones).

## Overriding source collision

If a `$generatejoint` targets a bone that already has collision from the
`$collisionjoints` source mesh, the generated collision **replaces** it (the old
collision for that bone is discarded). This lets you share one base collision SMD
across characters and regenerate only the bones that deviate - e.g. reuse
`base_ragdoll.smd` for the body and regenerate the hair bones per character. Bones
not targeted by a `$generatejoint` keep their source collision unchanged.

## Rejected pieces

Every generated convex piece must become a valid sealed convex element for the
physics packer (per Source's collision rules: convex, sealed, single-bone). A
piece the packer cannot turn into a clean closed manifold is rejected; the
compiler first retries it with a decimated vertex set, and only then drops it,
printing:

```
collision autogen: N of M generated convex piece(s) rejected by the physics packer
```

If **every** piece for a request is rejected (or the request otherwise produces no
usable geometry), compilation **halts with an error** rather than shipping a broken
collision model - an empty solid would write a malformed `.phy` that crashes the
engine on load. Fix the request and recompile:

- simplify or thicken the source render mesh,
- raise `concavity` for a coarser (more easily sealed) decomposition,
- lower `maxverts` for simpler hulls,
- or (for `$generatejoint`) lower `cullweight` if too much geometry was culled.

The same halt-on-error applies to setup mistakes: an unknown `$rendermesh`, a bone
not found in the mesh, or a bone not present in the compiled model.

## Notes and limitations

- A `$rendermesh` is mandatory; there is no generation from the collision source.
- Generation reads geometry only - it never touches flex / vertex animation data.
- Decomposition quality depends on reasonably clean input geometry. Render meshes
  are usually fine; very thin or near-flat geometry is the most likely to be
  rejected by the packer.
- Generated hulls are capped at 32 vertices each to keep them simple enough for
  the packer to seal reliably.
- VHACD is vendored under `libs/vhacd` (single-header, BSD-3). Backend types are
  isolated inside `studiomdl/convexdecompose.cpp`.
