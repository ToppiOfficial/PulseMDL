# $if / $elif / $else

Conditionally includes a block of QC commands based on a boolean expression.
The chosen block is injected transparently into the token stream, so every QC
command (`$bodygroup`, `$sequence`, `$driverbone`, etc.) works inside without any
special handling.

## Syntax

```
$if <condition> {
    ...
}

$if <condition> {
    ...
} $elif <condition> {
    ...
} $elif <condition> {
    ...
} $else {
    ...
}
```

## Rules

- `$elif` and `$else` must follow the closing `}` of the preceding `$if` or `$elif`
  on the same line or the next non-blank line. Placing them inside a block
  is an error.
- Up to 8 `$elif` clauses per `$if` chain.
- Maximum nesting depth: 3 levels of `$if` inside `$if`.
- The `$if` chain is fully consumed before any chosen content is executed, so
  only one branch ever runs regardless of what that branch contains.

---

# $switch / $case / $default

Selects one block from a set of named cases based on a variable's current value.

## Syntax

```
$switch <varname> {
    $case <value> {
        ...
    }
    $case <value> {
        ...
    }
    $default {
        ...
    }
}
```

## Rules

- `<varname>` is a raw variable name (no `$...$` delimiters). The variable must
  be defined; using an undefined variable is an error.
- `$case` values are plain strings. Comparison is case-sensitive.
- The first matching `$case` wins; subsequent cases are skipped.
- `$default` is required and must appear somewhere inside the block. It runs
  when no `$case` matches. Position within the block does not matter.
- Up to 24 `$case` clauses per `$switch` (not counting `$default`).
- Maximum nesting depth: 3 levels.

---

# Conditions

A condition is a boolean expression made of tokens separated by spaces.
Variables are expanded with `$varname$` syntax before evaluation.

## Operators

Lower in this list = lower precedence:

| Operator | Meaning |
|---|---|
| `==` | equal |
| `=!` | not equal |
| `>` | greater than |
| `<` | less than |
| `>=` | greater than or equal |
| `<=` | less than or equal |
| `&&` | logical and |
| `\|\|` | logical or |

## Grouping

```
( <expr> )
```

Spaces around the parentheses are required so they tokenize as separate tokens.

## Functions

**`None(<varname>)`**
- True when `<varname>` is not defined or its value is empty.
- `<varname>` is a raw name without `$...$` delimiters.
- The function name is case-insensitive.

**`Not(<expr>)`**
- Negates a single-token expression. `<expr>` must be one token with no
  spaces (e.g. `Not(None(myvar))` or `Not(1)`).

## Numeric vs string comparison

If both sides of a comparison parse as numbers, a numeric (floating-point)
comparison is used. Otherwise a plain string comparison is used.

Example: `$ver$ >= 2` compares numerically; `$name$ == hello` compares as text.

## Truthiness

For standalone values without an operator:

- **False:** empty string, `"0"`, `"false"` (case-insensitive)
- **True:** everything else

---

# Variables

Variables are declared with `$definevariable` and reassigned with
`$redefinevariable`. They are expanded by the tokenizer wherever `$varname$` appears,
including inside conditions.

```
$definevariable    myvar   2
$redefinevariable  myvar   3
```

A variable used in a condition with `$varname$` syntax must already be defined.
To safely test for existence first, use `None(myvar)`.

---

# Examples

Simple true/false:

```
$definevariable hq 1

$if $hq$ {
    $lod 0 { }
} $else {
    $lod 1 { nofacial }
}
```

Equality check with `$elif`:

```
$definevariable platform pc

$if $platform$ == console {
    $staticprop
} $elif $platform$ == pc {
    $surfaceprop "default"
} $else {
    $surfaceprop "unknown"
}
```

Numeric comparison:

```
$definevariable lod_count 3

$if $lod_count$ >= 2 {
    $lod 1 { ... }
    $lod 2 { ... }
}
```

Existence check:

```
$if None(optional_feature) {
    $surfaceprop "default"
} $else {
    $surfaceprop $optional_feature$
}
```

Compound condition with grouping:

```
$definevariable a 1
$definevariable b 0

$if ( $a$ == 1 && $b$ == 0 ) || $a$ == 2 {
    $modelname "models/variant_a.mdl"
}
```

`Not()` with `None()`:

```
$if Not(None(optional_feature)) {
    // optional_feature is defined and non-empty
}
```

Switch on a variable:

```
$definevariable rig_type biped

$switch rig_type {
    $case biped {
        $include "bones_biped.qc"
    }
    $case quadruped {
        $include "bones_quad.qc"
    }
    $default {
        $include "bones_generic.qc"
    }
}
```

Nested `$if`:

```
$definevariable platform pc
$definevariable quality high

$if $platform$ == pc {
    $if $quality$ == high {
        $lod 0 { }
    } $else {
        $lod 1 { nofacial }
    }
}
```

---

# Notes

- The entire `$if` chain (all `$elif`/`$else` branches) is parsed and all blocks are
  collected before any content executes. Preprocessor commands (`$definevariable`,
  `$include`, etc.) inside a non-taken branch are never executed.
- `$if`/`$switch` work at the tokenizer level, so they are valid anywhere a QC
  command is valid: top level, inside `$bodygroup`, inside `$sequence`, etc.
- `$elif`, `$else`, `$case`, and `$default` are not valid as standalone top-level
  commands and produce an error if used outside their construct.
- The script stack has a depth limit of 16 (shared with `$include`). Deep nesting
  combined with many `$include` directives could approach this limit.
