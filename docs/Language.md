# fwiz Language Reference

Syntax and semantics of the `.fw` language.

For the solver and numeric mode, see [Solver.md](Solver.md).
For curve fitting, see [Fitting.md](Fitting.md).
For the CLI and error messages, see [CLI.md](CLI.md).
For a gentle introduction, see [../README.md](../README.md).

## Table of Contents

1. [File Format](#1-file-format)
2. [Lexical Grammar](#2-lexical-grammar)
3. [Statements](#3-statements)
4. [Expressions](#4-expressions)
5. [Conditions](#5-conditions)
6. [Sections (Function Definitions)](#6-sections-function-definitions)
7. [Formula Calls](#7-formula-calls)
8. [Queries and Multiple Returns](#8-queries-and-multiple-returns)
9. [Recursion](#9-recursion)
10. [Rewrite Rules](#10-rewrite-rules)
    - [10.6 Typed-Binding Predicates](#106-typed-binding-predicates)
11. [Built-in Functions](#11-built-in-functions)
12. [Built-in Constants](#12-built-in-constants)
13. [Special Values](#13-special-values)
14. [Standard Library](#14-standard-library)
15. [Vector and Matrix Literals](#15-vector-and-matrix-literals)
16. [Complex Numbers](#16-complex-numbers)
17. [Dimension Annotations](#17-dimension-annotations)
18. [Bounded Aggregation](#18-bounded-aggregation)

---

## 1. File Format

- Extension: `.fw` (the CLI adds it automatically when omitted)
- Encoding: UTF-8, with or without a BOM
- Line endings: LF, CRLF, or mixed — all accepted
- Whitespace: leading/trailing whitespace on lines is trimmed; blank lines are ignored
- Comments: lines starting with `#`, or anything after a `#` on an equation line

A file is a sequence of **statements**, separated by newlines or `;`.

---

## 2. Lexical Grammar

### Identifiers

Variable and function names are `[A-Za-z_][A-Za-z0-9_]*`. Case-sensitive.

### Numbers

- Integers: `42`, `-7`, `0`
- Decimals: `3.14`, `0.5`, `-2.718`
- Scientific notation: `1e9`, `6.022e23`, `1.5e-3`, `100e-3` → `0.1`, `1E5` → `100000`

The lexer's `read_number` (since 2026-05-13) consumes the `[eE][+-]?[0-9]+` exponent tail as part of the number token, so `1.5e3` is the single literal `1500`, not `1.5 * e3`. A bare `e` or `E` not followed by digits (e.g. `1e`) produces `Num(1)` + `IDENT("e")`; the parser desugars this to `1 * e` (Euler's constant).

### Unit Suffixes

A number immediately followed by an identifier (no space) is desugared by the parser into a multiplication (since 2026-05-13):

```
mass = 100kg          # parsed as 100 * kg
length = 9.8m         # parsed as 9.8 * m
period = 2hr          # parsed as 2 * hr
```

The identifier is an ordinary variable. Unit semantics live in stdlib `.fw` files that bind the identifier to a conversion factor:

```bash
# with stdlib/units/si-minimal.fw loaded:
$ fwiz 'stdlib/units/si-minimal.fw(mass=100kg, mass=?)'
mass = 100
```

**Function-call suffix:** `100sin(x)` desugars to `MUL(Num(100), FUNC_CALL("sin", [Var("x")]))`. No warning is emitted.

**Precedence quirk — `100m^2`:** The parser sees `(100 * m)^2` rather than `100 * (m^2)`. A parse-time warning is emitted to stderr:

```
Warning: '100m^...' uses NUMBER-IDENT desugar before '^'; did you mean '100 * m^2'?
```

Use explicit parentheses or a space: `100 * m^2`. This is a known limitation; see Future #74.

**CLI-arg unit suffixes** work end-to-end when the unit identifier resolves after file load (since 2026-05-13, cycle 2). `parse_cli_query` defers any RHS expression whose `evaluate` returns empty (because the identifier is unbound at CLI-parse time) to post-load resolution via the `synthetic_equations` channel — the same mechanism used by `diff`/`integral` CLI queries. Example:

```bash
$ fwiz 'stdlib/units/si-minimal.fw(distance=10km, time=2hr, speed=?, speed_eqn=distance/time, speed_eqn=speed)'
speed = 25/18
```

The result is in SI base (m/s). `--derive` and `--fit` retain their existing symbolic-RHS contract; the deferral path is skipped in those modes.

### Operators and Punctuation

| Token | Meaning |
|-------|---------|
| `+ - * / ^` | Arithmetic |
| `-` | Unary negation |
| `( )` | Grouping |
| `= == != < <= > >=` | Comparison (in conditions) |
| `&&` / `\|\|` | Logical AND / OR (in conditions) |
| `,` | Separator in argument lists |
| `;` | Line separator (equivalent to newline) |
| `?` | Query marker (`x=?`) |
| `!` | "Exactly one solution" modifier (`x=?!`) |
| `[ ]` | Section header brackets; also range/vector/matrix literals (`[1..6]`, `[1,2,3]`) |
| `->` | Section return-variable arrow |
| `:` | Binding-annotation separator (`var:dim = expr`); since gen-3 cycle 2. |
| `..` | Range separator in range literals (`[lo..hi]`); since gen-6 cycle 1. |
| `@` | Step separator in range literals (`[lo..hi @ step]`); prefix for directives (`@extern`) |
| `#` | Line comment |

### Keywords

`if`, `iff`, `in`, `undefined`. Plus the directive `@extern`. These cannot be used as variable names. (`in` is a keyword since cycle 3f; it can appear as a parameter name in formula-call bindings.)

---

## 3. Statements

A `.fw` file is composed of the following statement types.

### 3.1 Equations

An equation asserts a relationship between variables:

```
distance = speed * time
```

The left side is typically a single variable (sometimes a formula-call binding, see §3.5). The right side is any expression. Equations are declarative — fwiz can solve for **any** variable that appears in an equation, not just the left side.

Multiple equations can define the same variable; the solver tries them in file order.

### 3.2 Defaults

A line with a bare number on the right is a **default**:

```
g = 9.81
```

Defaults provide a fallback value when a variable isn't supplied on the command line and isn't derivable from other equations. When you query a variable that has a default, the default is **ignored** — fwiz solves from equations instead.

Defaults override built-in constants: if a file defines `e = 5`, that value is used instead of Euler's number.

### 3.3 Comments

```
# This is a comment line
force = mass * g  # This is a trailing comment
```

Comments are stripped before parsing.

### 3.4 Global Conditions

A standalone condition constrains variables globally — useful for domain restrictions:

```
side > 0
area >= 0
```

These are applied when narrowing solution ranges during numeric solving and when checking validity of algebraic solutions.

### 3.5 Formula Calls

A formula-call statement invokes another formula (in the same file, a built-in, or an external file) and binds its outputs into the current scope:

```
rectangle(area=?floor, width=width, height=depth)
```

See §7 for full semantics.

### 3.6 Sections

A `[name(args) -> return]` header starts a section. See §6.

### 3.7 Rewrite Rules

A line whose left side is not a plain variable is parsed as a rewrite rule — a pattern-to-pattern simplification rule. See §10.

```
sin(-x) = -sin(x)
x / x = 1 iff x != 0
```

---

## 4. Expressions

### 4.1 Operators and Precedence

From tightest to loosest binding:

| Precedence | Operator | Associativity | Meaning |
|------------|----------|---------------|---------|
| 1 (tightest) | `^` | right | Power |
| 2 | unary `-` | right | Negation |
| 3 | `*`, `/` | left | Multiplication, division |
| 4 | `+`, `-` | left | Addition, subtraction |
| 5 | `<`, `<=`, `>`, `>=`, `=`, `==`, `!=` | left | Comparison (conditions only) |
| 6 | `&&` | left | Logical AND (conditions only) |
| 7 (loosest) | `\|\|` | left | Logical OR (conditions only) |

Parentheses override precedence. `(a + b) * c` forces the addition first, whereas `a + b * c` multiplies first.

### 4.2 Function Calls in Expressions

`sqrt(x)`, `sin(x + 1)`, `log(x^2)`. Arguments are expressions. A single positional argument binds to the first named argument of the function's section header.

### 4.3 Division

Integer-over-integer division is preserved as a structural fraction when the result is non-integer:

```bash
$ fwiz convert(celsius=?, fahrenheit=50)
celsius = 10        # 10/1 simplifies to 10

$ fwiz convert(celsius=?, fahrenheit=72)
celsius = 200 / 9   # preserved as exact fraction
```

Division by zero returns NaN (propagates through arithmetic).

---

## 5. Conditions

A condition restricts when an equation or rewrite rule applies.

### 5.1 `if` — one-directional

Checked only during forward evaluation:

```
y = sqrt(x) if x >= 0
y = 0       if x < 0
```

### 5.2 `iff` — bidirectional

Use `iff` when the condition is part of the equation's *domain* — something that must hold whichever direction fwiz solves the equation. With `iff`, fwiz can run the condition backwards too, and this is what enables *inverse reasoning*.

The difference is clearest on a constant branch:

```
result = 1 if  x > 0     # (a) condition only gates forward
result = 1 iff x > 0     # (b) condition gates both directions
```

Now ask for `x` given `result = 1`:

```bash
$ fwiz a(x=?, result=1)      # with 'if'
Error: Cannot solve for 'x'

$ fwiz b(x=?, result=1)      # with 'iff'
x : (0, +inf)
```

The `iff` version inverts the condition into a **range** — `x` is any positive real. The `if` version can't be inverted at all: `if` only ever runs forward, so fwiz has no way to reason from `result = 1` back to `x`.

The same principle makes piecewise definitions like absolute value invertible. A top-level piecewise `myabs.fw`:

```
result = x  iff x >= 0
result = -x iff x < 0
```

```bash
$ fwiz myabs(x=?, result=3)
x = 3
x = -3
$ fwiz myabs(x=?, result=0)
x = 0
$ fwiz myabs(x=?, result=-1)
Error: Cannot solve for 'x'       # no piece's domain permits a negative result
```

Each piece is inverted and its `iff` condition is applied to the inverted value, so only the roots that lie in the original piece's domain survive. Using `if` here would block inversion entirely.

The standard library wraps this same pattern into the named function `abs` via a section (see §14).

### 5.3 Comparison Operators

`>`, `>=`, `<`, `<=`, `=`, `==`, `!=`. `=` and `==` are synonyms.

### 5.4 Compound Conditions

```
tax = income * 0.1 if income > 0 && income <= 50000
```

`&&` binds tighter than `||`. Parentheses work as expected.

### 5.5 Optional Comma

A condition can optionally be preceded by `,`:

```
y = x, if x > 0
```

This is purely cosmetic.

---

## 6. Sections (Function Definitions)

A section declares a named function with positional arguments and a return variable.

### 6.1 Header Syntax

```
[name(arg1, arg2, ...) -> return]
```

- `name` — the function name (also the section's identity)
- `arg1, arg2, ...` — positional parameter names
- `return` — the default query variable

### 6.2 Body Forms

**Single-line with `=` sugar:**
```
[square(x) -> result] = x ^ 2
```
`= expr` on the header desugars to `return = expr` (i.e. `result = x^2`).

**Multi-line body:**
```
[abs(x) -> result]
= x  iff x >= 0
= -x iff x < 0
```
Each `= expr` line is desugared to `result = expr` using the header's return variable. Regular equations (not starting with `=`) are also allowed inside sections and do not get the sugar.

**With `@extern`:**
```
[sin(x) -> result] @extern sin
x = asin(result)
```
`@extern name` wires the section's forward direction to a C++ function pointer for fast evaluation. The inverse equation (here, `x = asin(result)`) lets fwiz solve the section in reverse.

### 6.3 Section Terminators

A section runs from its header until the next section header, the end of the file, or an explicit separator. `;` works as a line separator anywhere:

```
[sin(x) -> result] @extern sin; x = asin(result)
```

### 6.4 Built-in Section Definitions

These are bundled and loaded automatically on every run:

| Function | Forward | Inverse |
|----------|---------|---------|
| `sin`    | `@extern sin`  | `x = asin(result)` |
| `cos`    | `@extern cos`  | `x = acos(result)` |
| `tan`    | `@extern tan`  | `x = atan(result)` |
| `asin`   | `@extern asin` | `x = sin(result)` |
| `acos`   | `@extern acos` | `x = cos(result)` |
| `atan`   | `@extern atan` | `x = tan(result)` |
| `log`    | `@extern log`  | `x = e ^ result` |
| `sqrt`   | `@extern sqrt` | `x = result ^ 2`, `result >= 0` |
| `abs`    | `@extern abs`  | piecewise, bidirectional |

You can redefine any of these in your own file; your definition wins.

---

## 7. Formula Calls

A formula call invokes another formula and binds its outputs into the current scope.

### 7.1 Named Bindings

```
rectangle(area=?floor, width=width, height=depth)
```

- `area=?floor` — query `area` in the callee, expose the result as `floor` in the caller
- `width=width` — pass the caller's `width` to the callee's `width`
- `height=depth` — pass the caller's `depth` to the callee's `height`

Inputs can be arbitrary expressions evaluated in the caller's scope:

```
factorial(result=?prev, n=n-1)   # n-1 evaluated in caller
```

### 7.2 Positional Arguments

When a formula call appears as an expression (e.g. inside another equation), positional arguments map to the section header's argument list, and the query target is the return variable:

```
y = square(x + 1)   # expands to: square(x=x+1, result=?), bind to y
```

### 7.3 Query Alias

A binding like `area=?floor` renames the result: the callee computes `area`, the caller sees it as `floor`. If you want the result under the same name, `area=?` is fine.

### 7.4 "Exactly One" Modifier

`area=?!floor` requires exactly one solution from the callee; if the callee returns multiple, fwiz errors out instead of picking one. Useful when calling a function whose multi-root behavior would make the parent's logic ambiguous.

### 7.5 Cross-File Calls

If the callee is not in the current file, fwiz looks for a `.fw` file matching the function name in the current directory:

```
# box.fw
rectangle(area=?bottom, width=width, height=depth)
```

This loads `rectangle.fw` as a sub-system on demand.

### 7.6 Project Structure

fwiz has no `import` or module system — you organize a project by putting `.fw` files in a directory and letting cross-file calls stitch them together.

#### Resolution rules

Since the @include migration (2026-06-23) fwiz resolves cross-file calls in **strict mode by
default** — a callee must be **explicitly declared** with `@include` (or be on the include path).
Co-location alone is no longer a resolution channel.

When `box.fw` calls `rectangle(...)`:

1. If `box.fw` defines a section `[rectangle(...) -> ...]`, use that.
2. Otherwise, if `box.fw` has `@include "rectangle.fw"` (or `rectangle.fw` is on the `-I` / `FWIZ_PATH`
   search path), load it. The callee must declare an explicit `[rectangle(args) -> ret]` section —
   a flat file of bare equations merges its definitions but is not callable by stem.
3. If neither holds, the call fails with a clear error naming the call and suggesting
   `add @include "rectangle.fw"`, listing the searched directories.

To restore the old implicit-CWD behavior (co-located callee resolved without `@include`), pass
`--legacy-implicit` — a one-release backward-compat opt-out:

```bash
$ cd my-project/
$ fwiz --legacy-implicit box(volume=?, width=2, height=3, depth=4)
```

#### Typical layout

```
my-project/
├── main.fw          # top-level formulas you invoke
├── helpers.fw       # shared definitions: [helper(x) -> y] sections
└── data.fw          # shared defaults (g = 9.81, pi overrides, etc.)
```

- **One file = one concept.** `rectangle.fw` defines rectangle arithmetic; `triangle.fw` defines triangle arithmetic; `physics.fw` collects physics formulas.
- **Files are namespaces only by filename.** There is no explicit import — if `main.fw` calls `rectangle(...)` and `rectangle.fw` exists, it's found.
- **Share defaults via a library file.** Put `g = 9.81`, `c = 299792458`, etc. in a common file and call it from consumers. Defaults in a callee override CLI bindings only if the caller doesn't pass them through.

#### When to split

Split a `.fw` file when:
- A section is reused from more than one file → move it to its own `.fw` and let cross-file resolution handle it
- The file exceeds one screen of formulas and covers multiple domains
- You want to reuse a formula across projects — put it in a shared directory and invoke fwiz from there

Do NOT split just because a file has multiple equations — fwiz is designed around multi-equation files where the equations compose naturally via shared variables.

#### The shipped stdlib

fwiz ships a small standard library in the `stdlib/` directory. See §14.

### 7.7 `@include` — explicit cross-file dependencies

```
@include "rectangle.fw"          # pull rectangle.fw's definitions into this file
@include "stdlib/units/si-minimal.fw"
```

`@include "path.fw"` (quoted form primary; unquoted tolerated) recursively loads the named file,
merging its definitions (sections, constants, rewrite rules) into the current system. It is the
explicit declaration that satisfies strict-mode resolution. Place `@include` lines anywhere in a
`.fw` file (conventionally at the top); transitive includes (A includes B includes C) and include
cycles are both handled — a cycle throws a clear error rather than looping.

**Search order** for the included path:

1. **File-relative** — the directory of the file doing the `@include` (`base_dir`).
2. **`-I <dir>`** — each `-I` directory on the command line, in order (repeatable; `-Idir` attached
   form also accepted).
3. **`FWIZ_PATH`** — directories from the `FWIZ_PATH` environment variable (split on `:` / `;`),
   searched after the `-I` dirs.

A not-found `@include` names every directory it searched.

**Strict-by-default model.** A fresh fwiz invocation resolves cross-file calls strictly: the callee
must be reachable via `@include` or the include path, AND must declare an explicit
`[name(args) -> ret]` section to be callable by stem. This makes a `.fw` file's dependencies
self-documenting. Inline builtins (`diff`, `integral`, `range`, `vec`, `mat`, `matmul`, `det`, `inv`,
`transpose`) are NOT cross-file calls — they are resolved internally and never require `@include`.

**`--legacy-implicit`** opts out: cross-file calls fall back to the pre-migration implicit base_dir
co-location probe (a co-located `rectangle.fw` resolves a `rectangle(...)` call without `@include`,
and a flat file is callable by stem). This flag is a one-release backward-compat window.

---

## 8. Queries and Multiple Returns

The CLI invokes a file with a parenthesized argument list:

```
fwiz <file>(<var>=<value>, <var>=?, <var>=?<alias>, ...)
```

Query types:

| Syntax | Meaning |
|--------|---------|
| `x=value` | Input: set `x` to `value` |
| `x=?` | Query: solve for `x`, return all solutions |
| `x=?!` | Query: solve for `x`, error if more than one solution |
| `x=?alias` | Query with rename: solve for `x`, print as `alias` |
| `x=?!alias` | Same, with "exactly one" constraint |

Multiple queries in one call are solved independently using the same input bindings:

```bash
$ fwiz geometry(area=?, perimeter=?, width=5, height=3)
area = 15
perimeter = 16
```

Input values can be expressions:

```bash
$ fwiz geometry(area=?, width=2^3, height=sqrt(9))
area = 24
```

---

## 9. Recursion

A formula that calls itself creates a recursive definition. Conditional base cases terminate the recursion:

```
result = 1                                       if n <= 0
result = n * factorial(result=?prev, n=n-1)      if n > 0
```

The solver picks the first equation whose condition holds for the current bindings. Deep recursion is bounded by `max_formula_depth` (default 1000) — exceeding it produces an error rather than a stack overflow.

Recursion gives fwiz Turing-completeness: you can express arbitrary algorithms, including non-terminating ones. Use conditions to guarantee progress toward a base case.

---

## 10. Rewrite Rules

A line whose left side is not a plain variable is parsed as a **rewrite rule** — a structural pattern that the simplifier applies during evaluation and derivation.

### 10.1 Syntax

```
pattern = replacement                     # unconditional
pattern = replacement iff condition       # guarded
```

Variables in the pattern act as **wildcards**. Built-in constants (`pi`, `e`, etc.) and literal numbers match only themselves.

### 10.2 Built-in Rules

Shipped with fwiz:

```
sin(-x) = -sin(x)
cos(-x) = cos(x)
asin(sin(x)) = x
acos(cos(x)) = x
atan(tan(x)) = x
sin(asin(x)) = x
cos(acos(x)) = x
tan(atan(x)) = x
abs(abs(x)) = abs(x)
abs(-x) = abs(x)
sqrt(x^2) = abs(x)
log(e^x) = x
e^log(x) = x
log(x^n) = n * log(x) iff x != 0
x / x = 1             iff x != 0
x / x = undefined     iff x = 0
x ^ 0 = 1
x ^ 1 = x
x ^ (1/2) = sqrt(x)
(x^a)^b = x^(a*b)
x^a / x^b = x^(a-b)  iff x != 0
abs(x) / x = sign(x)  iff x != 0
abs(x) / x = undefined iff x = 0
```

### 10.3 Exhaustiveness

A pair of guarded rules can cover the full domain — giving fwiz a complete rewrite for a pattern:

```
x / x = 1         iff x != 0
x / x = undefined iff x = 0
```

Together these cover all real `x`, so the simplifier can always reduce `x/x` to something definite.

### 10.4 Commutative Matching

The pattern matcher recognizes that `a + b` matches `y + x` (with `a -> y`, `b -> x`), and handles multi-term additive and multiplicative permutations. Write rules in any one ordering; commutativity is handled automatically.

### 10.5 User-Defined Rules

Any `.fw` file can add rewrite rules. They're picked up on load:

```
# my_rules.fw
double(x) = 2 * x
triple(x) = 3 * x
```

### 10.6 Typed-Binding Predicates

Some rule conditions test the *type* of a wildcard binding rather than comparing values. Two canonical predicates are available (since gen-5 cycle 3a, 2026-05-15):

| Predicate | Arity | True when |
|-----------|-------|-----------|
| `is_neg_num(n)` | 1 | `n` binds to a negative numeric literal |
| `is_in(v, set_name)` | 2 | binding of `v` is a member of the named set (see §17.3) |

**Infix `in` syntax** (preferred, since cycle 3f, 2026-05-16): `v in set_name` is syntax sugar for `is_in(v, set_name)`. Both forms lower to the same AST. The infix form reads as the math `v ∈ S` and is preferred in new rule writing.

**Legacy aliases** (accepted at rule-load time, rewritten to `is_in` internally):
- `is_int(n)` → `is_in(n, int)`
- `is_in_dimension(v, dim)` → `is_in(v, dim)`

```
x ^ n = 1 / x ^ (-n)  iff is_neg_num(n)
# Only fires when exponent is a known negative literal; symbolic -k is unaffected.

x + y = undefined  iff x in mass && y in time
# Dimension-rejection rule (infix form): adding mass to time is undefined.

floor(n) = n  iff n in int
# Rule using built-in named set (infix form).

x + y = undefined  iff is_in(x, mass) && is_in(y, time)
# Function-call form — equivalent, still accepted.
```

Fail-safe semantics: if the wildcard is not bound, or the binding does not satisfy the predicate, it returns false (contrast comparison clauses like `x != 0`, which default to permissive-true on unknown bindings). Predicates and comparison clauses can be combined freely in a single condition.

`is_in(v, dim_name)` where `dim_name` is a dimension section requires dimension annotations on variables — see §17.

**Precedence quirk:** `in` is detected by a string-level scan in `parse_condition` BEFORE the comparison-op loop. As a result `iff x == 5 in int` splits at ` in ` first and would attempt to parse `x == 5` as an expression (which fails — `==` is condition-level, not expression-level). Parenthesise to make intent explicit: `iff (x == 5) in int` (parses as `is_in((x == 5), int)`, also fails because `==` is not expression-level — but is the documented intent). For typical cycle-3a/3b/3d/3f usage this never matters; document for future-proofing.

**Chained `in`:** `x in y in z` is rejected at parse time with a clear "Infix 'in' does not chain" error message; use `(x in y) && (x in z)` for compound membership tests.

**Reserved-word note:** `in` is a true lexer keyword (since cycle 3f). It cannot be used as a variable name in any equation or expression. It CAN appear as a parameter name in formula-call bindings (`foo(in=value)`).

---

## 11. Built-in Functions

Defined via the built-in section mechanism (§6.4). All accept real arguments and return real values; complex results are NaN.

| Function | Description | Domain |
|----------|-------------|--------|
| `sqrt(x)` | Square root | `x >= 0` |
| `abs(x)` | Absolute value | all real |
| `sin(x)` | Sine (radians) | all real |
| `cos(x)` | Cosine (radians) | all real |
| `tan(x)` | Tangent (radians) | `x != pi/2 + k*pi` |
| `asin(x)` | Inverse sine (radians) | `-1 <= x <= 1` |
| `acos(x)` | Inverse cosine (radians) | `-1 <= x <= 1` |
| `atan(x)` | Inverse tangent (radians) | all real |
| `log(x)` | Natural logarithm | `x > 0` |
| `sign(x)` | Sign: −1, 0, or +1 | all real |
| `diff(f, x)` | Symbolic derivative of `f` with respect to `x` | `x` must be a bare variable name |
| `integral(f, x)` | Symbolic antiderivative of `f` with respect to `x` | `x` must be a bare variable name |
| `integral(f, x, a, b)` | Definite integral of `f` from `a` to `b` | `x` bare; `a`, `b` any expressions |

`diff(f, x)` is a parser-level builtin: when it appears in a `.fw` equation body, the derivative is computed symbolically at load time and the result (a simplified expression tree) is inlined in place of the call. `f` may be any expression or the name of another variable defined in the same system (the post-load pass substitutes its equation's RHS before differentiating).

`integral(f, x)` is likewise resolved at load time when used in an equation body. The indefinite form returns an antiderivative expression. The 4-arg definite form uses symbolic F(b) − F(a) as its primary path; when symbolic integration cannot close the antiderivative, it falls back to adaptive Simpson's rule numerically. When neither path succeeds, the `integral(...)` call is preserved unevaluated. `diff` and `integral` are also accepted as CLI query targets — see [CLI.md §1](CLI.md#1-syntax).

```
# sensitivity.fw
force = mass * acceleration
sensitivity = diff(force, mass)    # inlined as: sensitivity = acceleration
```

Out-of-domain inputs produce NaN at evaluation time.

Custom functions registered via C++ `register_function()` are also callable; see [Developer.md](Developer.md).

---

## 12. Built-in Constants

Available in any equation without declaration:

| Name | Value | Description |
|------|-------|-------------|
| `pi`  | 3.14159265... | Circle constant |
| `e`   | 2.71828182... | Euler's number |
| `phi` | 1.61803398... | Golden ratio |
| `i`   | (no numeric value; NaN binding) | Imaginary unit (`i^2 = -1`). Symbolic-only — forces the symbolic channel. See §16. |

In `--derive` mode `pi`, `e`, and `phi` are preserved symbolically:

```bash
$ fwiz --derive physics(circumference=?, radius=r)
circumference = 2 * pi * r
```

In solve mode they are numeric. File-local defaults override built-ins.

---

## 13. Special Values

### 13.1 `undefined`

A reserved symbolic value used to denote domain boundaries. Propagates through arithmetic:

```
x / x = undefined iff x = 0
```

An equation that evaluates to `undefined` is skipped by the solver — the next equation is tried.

### 13.2 NaN

Any expression that produces IEEE NaN at evaluation time (e.g. `sqrt(-1)`, `log(-1)`, `0/0`) makes that equation's result **invalid**. The solver skips it and tries the next candidate.

### 13.3 Infinity

`1/0` yields NaN (fwiz normalizes division-by-zero to NaN, not `+inf`). Other paths that naturally produce `inf` (large exponentials, extreme `log`) cause the solver to reject that branch and try the next.

### 13.4 Invalid Input

You cannot pass `inf` or `nan` as a CLI input:

```
Error: Infinity is not a valid value for 'y'
```

---

## 14. Standard Library

fwiz ships a small standard library in the `stdlib/` directory of the repo.

### 14.1 `stdlib/stdlib.fw` — reusable helpers

Pure-fwiz definitions of common piecewise functions. Each uses bidirectional `iff` conditions so they work in both forward and reverse solves.

| Function | Definition | Behaviour |
|----------|-----------|-----------|
| `abs(x)` | `x iff x >= 0; -x iff x < 0` | Absolute value |
| `sign(x)` | `1 iff x > 0; 0 iff x = 0; -1 iff x < 0` | Sign |
| `clamp(x, lo, hi)` | `lo iff x < lo; x iff lo <= x <= hi; hi iff x > hi` | Clamp to range |
| `max(a, b)` | `a iff a >= b; b iff b > a` | Maximum |
| `min(a, b)` | `a iff a <= b; b iff b < a` | Minimum |
| `step(x)` | `0 iff x < 0; 1 iff x >= 0` | Heaviside step |

To use these, copy `stdlib.fw` into your project directory (or invoke fwiz from a directory that has it on the resolution path — §7.6). Then call the functions directly:

```bash
$ fwiz my_formula(result=?, x=-5)   # where my_formula calls stdlib's abs()
```

Note: `abs` is *also* a C++ built-in (§11) for speed; the stdlib version demonstrates how a user could define it in pure fwiz. The C++ built-in takes precedence when both are available.

### 14.3 `stdlib/units/si-minimal.fw` — SI units (since 2026-05-13, expanded cycle 2)

Binds SI base units, common prefixes, and 5 derived units to their SI-base scalar values so that unit-suffix expressions resolve numerically:

**Base units** (all scalar 1): `m, kg, s, A, K, mol, cd`

**Length prefixes:** `km=1000`, `mm=0.001`, `um=1e-6`, `nm=1e-9`, `Mm=1e6`, `Gm=1e9`

**Mass prefixes:** `g=0.001`, `mg=1e-6` (note: `kg` is the SI base, not `g`)

**Time prefixes/multiples:** `ms=0.001`, `us=1e-6`, `ns=1e-9`, `min=60`, `hr=3600`, `day=86400`

**Derived units:** `N=1` (newton, kg·m/s²), `J=1` (joule), `W=1` (watt), `Pa=1` (pascal), `Hz=1` (hertz)

All values are defined in terms of SI base — `km = 1000 * m` and so on — so they cascade through a single base-change point.

Load it as the CLI's single file argument and write your formula inline or via the inline-source idiom:

```bash
$ fwiz 'stdlib/units/si-minimal.fw(distance=10km, time=2hr, speed=?, speed_eqn=distance/time, speed_eqn=speed)'
speed = 25 / 18
```

Note: a multi-file CLI load form (`fwiz file_a.fw file_b.fw(...)`) is **not** currently supported — the CLI accepts exactly one filename. Library files needing a units catalog must either (a) inline the necessary bindings (the `stdlib/physics/mechanics.fw` pattern, see §14.4) or (b) drive the equations via CLI synthetic bindings. Completed 2026-06-23 (M1+M2+M3). See the `@include` directive section (§7.7) for syntax and strict-mode resolution.

Dimensional analysis rejection and further catalog expansion are tracked in Future #7a and #7b (the latter blocked by Future #78 — see Future.md).

### 14.4 `stdlib/physics/mechanics.fw` — Newtonian mechanics (since 2026-05-13, cycle 3)

Bidirectional Newtonian-mechanics equations: `force = mass * accel`, weight, kinetic energy, momentum, work, power, pressure, frequency/period reciprocity. Inlines the SI-base bindings it depends on (`m, kg, s, N, Pa, J, W`) so unit-suffix CLI args (`mass=10kg`, `accel=9.81*m/s^2`) resolve end-to-end without a separate units load. Variable names are deliberately verbose (`mass`, not `m`; `displacement`, not `s`; `area`, not `A`) to avoid shadowing the SI base-unit symbols bound by `stdlib/units/si-minimal.fw`.

```bash
$ fwiz 'stdlib/physics/mechanics.fw(force=?, mass=10kg, accel=9.81)'
force = 981 / 10
$ fwiz 'stdlib/physics/mechanics.fw(mass=?, force=98.1, accel=9.81)'
mass = 10
```

### 14.2 `stdlib/builtin.fw` — reference for C++ built-ins

The `.fw` representation of the C++-backed built-ins (`sin`, `cos`, `sqrt`, `log`, etc.). Each section shows the `@extern` directive and the inverse equation used for reverse solving:

```
[sin(x) -> result] @extern sin
x = asin(result)
```

This file is **reference documentation** — the actual built-ins are compiled into the fwiz binary. Read it when you want to define your own `@extern`-backed function in C++: copy the pattern, register via `register_function()`, and your new function behaves like a native built-in.

---

## 15. Vector and Matrix Literals

fwiz supports vector and matrix literals using bracket syntax. No new `ExprType` is introduced — they are represented internally as `FUNC_CALL("vec", ...)` and `FUNC_CALL("mat", ...)` nodes, so `sizeof(Expr)` is unchanged.

### 15.1 Vector Literals

```
v = [1, 2, 3]           # row vector
w = [a, b+1, c^2]       # symbolic elements allowed
```

A bracketed, comma-separated list without `..` (which would trigger range parsing in the CLI) is a vector literal. It parses as `FUNC_CALL("vec", e1, e2, e3)`.

### 15.2 Matrix Literals

```
m = [[1, 0], [0, 1]]    # 2x2 identity matrix
```

A list of vector literals, one per row. Parses internally as `FUNC_CALL("mat", vec_row1, vec_row2, ...)`. Rows with differing column counts are rejected at parse time (since 2026-05-13): `[[1, 2], [3]]` raises `"Ragged matrix literal: row 0 has 2 columns, row 1 has 1 column"` rather than silently propagating `undefined`.

### 15.3 Auto-simplification

Element-wise add/sub between same-shape literals and scalar multiplication (scalar × vec or scalar × mat) simplify automatically. Shape mismatch (e.g. adding a 2×2 to a 2×3, or taking the dot product of vectors of different lengths) propagates `undefined`.

### 15.4 Matrix Builtins

| Builtin | Effect | Scope |
|---------|--------|-------|
| `matmul(A, B)` | Matrix product | Both arguments must be matrices; compatible inner dimensions |
| `det(M)` | Determinant | 2×2 and 3×3 only |
| `inv(M)` | Matrix inverse | 2×2 only |
| `transpose(M)` | Transpose | Any rectangular matrix |

```bash
$ fwiz --derive '(M=?) M = matmul([[1, 2], [3, 4]], [[5, 6], [7, 8]])'
M = [[19, 22], [43, 50]]
```

Matrix-vector products require lifting the vector to a single-column matrix (e.g. `matmul(R, [[3], [4]])`, not `matmul(R, [3, 4])`). Out-of-scope shapes for `det`, `inv`, or `matmul` propagate `undefined`. `evaluate()` returns empty for any `vec`/`mat` node, so matrix-valued results need `--derive` to surface; `det` returns a scalar and works in solve mode too.

---

## 16. Complex Numbers

The imaginary unit `i` is a built-in symbolic constant. Its numeric binding is NaN by design — fwiz routes complex arithmetic through the symbolic channel only.

```bash
$ fwiz '(z=?) z = i * i'
z = -1
```

The simplifier applies the builtin rewrite rules `i * i = -1` and `i^2 = -1`. Any expression containing `i` that reaches the numeric evaluator returns empty (`evaluate()` returns empty), so the numeric solver, conditions, and verify mode all reject complex operands — they never silently produce a real approximation.

**Current scope:** single-step reductions like `i^2 → -1` work. Multi-step complex arithmetic like `(1+i)*(1-i) → 2` requires distributing MUL over ADD first, which is not yet implemented. See `docs/Future.md` for the planned follow-up.

---

## 17. Dimension Annotations

Since gen-3 cycle 2 (2026-05-15), fwiz supports optional dimension annotations on variables and named dimension sections that group unit bindings. Gen-5 cycle 3a (2026-05-15) extended the annotation system with a unified named-set registry and canonical `is_in` predicate. Gen-5 cycle 3b (2026-05-16) added user-defined predicate sets.

### 17.1 Section Flavors

Three section header shapes serve distinct roles:

| Syntax | Kind | Registered as |
|--------|------|---------------|
| `[name]` (no parens, no `->`) | Dimension section | `DIM_SECTION` in `set_definitions_` |
| `[name(param)] iff ...` (parameter + body) | Predicate section | `USER_PREDICATE` in `set_definitions_` |
| `[name(args) -> ret]` (args + return) | Formula section | sub-system in `custom_function_defs_` |

**Dimension sections** declare a dimension category — all LHS names in the body are tagged with that dimension:

```
[mass]
g = 1
kg = 1000 * g
lb = 453.592 * g
```

Every variable bound inside the section (`g`, `kg`, `lb`) is automatically registered as having dimension `mass` in the system's `type_map_`. The section body uses ordinary equation and default syntax.

Access values via dot-dispatch:

```bash
$ fwiz '([mass]\ng=1\nkg=1000*g)(mass.kg=?)'
mass.kg = 1000
```

**Predicate sections** (since gen-5 cycle 3b) declare a user-defined named set with a membership condition:

```
[whole_number(n)] iff n >= 0 && is_in(n, int)
```

The header parameter (`n`) is bound to the queried value at `is_in` call time. The body is a condition; multi-line bodies join as implicit AND:

```
[non_trivial(x)]
iff x != 0
iff x != 1
```

An empty body is silently inert (no set registered). Forward references between predicate sections work — both are registered before any `is_in` dispatch evaluates them.

### 17.2 Binding Annotations

The `:` token annotates an individual binding with a type:

```
m_obj:mass = 10 * kg          # atomic: m_obj tagged as dimension 'mass'
n:(int, mass) = 5             # intersection: n gets dim=mass AND set membership int
q:(whole_number, mass) = 5    # intersection with user-defined predicate set
```

After the annotation is stripped, the line is parsed as a normal equation or default. The annotation does NOT change the variable's numeric value — only its entry in `type_map_`.

**Intersection classification:** each atom in the intersection list is looked up in the `set_definitions_` registry. `DIM_SECTION` atoms populate `BindingType.dim`; `BUILTIN_PREDICATE` and `USER_PREDICATE` atoms populate `BindingType.sets`. An unknown atom raises `BindingAnnotationError` at parse time, naming the unknown atom and listing built-in alternatives.

Operators inside intersection parentheses (`*`, `/`, `^`) raise a `BindingAnnotationError`. Only bare identifiers separated by commas are accepted.

### 17.3 Named Sets

Four built-in named sets are available in any annotation or predicate condition:

| Name | Membership |
|------|-----------|
| `int` | value is an integer (finite, `is_integer_value(v)`) |
| `real` | value is a finite real (non-NaN, non-inf) |
| `rational` | currently equivalent to `real` |
| `imaginary` | accepts NaN-sentinel (covers `i`-containing expressions); renamed from `complex` in cycle 3b |

These are the same names you use in intersection annotations (`n:(int, mass) = 5`) and in `is_in` rule predicates (`is_in(n, int)`).

**User-defined sets** registered via predicate sections (§17.1) work identically: `is_in(v, whole_number)` dispatches through the same `SetDef::Kind` switch in `check_condition`. **Design invariant (AC8):** `[my_int(n)] iff is_in(n, real) && is_in(n, int)` is functionally equivalent to the built-in `int` set — built-ins are optimized C++ fast-paths of what users can express in `.fw`.

### 17.4 Dimension-Checking Rewrite Rules

The canonical predicate is `is_in(v, set_name)`. **Preferred infix form (cycle 3f):** `v in set_name`. Both forms produce the same AST; the infix form reads as math.

```
# prevent adding mass to time (dim-mismatch rule, infix form):
x + y = undefined  iff x in mass && y in time

# integer-only rule:
floor(n) = n  iff n in int

# user-defined set in a rule:
p + q = undefined  iff p in whole_number

# function-call form — equivalent, still accepted:
x + y = undefined  iff is_in(x, mass) && is_in(y, time)
```

**Legacy aliases** (accepted at rule-load time, rewritten to `is_in` internally):
- `is_int(n)` → `is_in(n, int)`
- `is_in_dimension(v, dim)` → `is_in(v, dim)`

Both legacy forms still work — the engine normalizes them at parse time. All predicates are fail-safe: if the variable is not annotated or not bound, the predicate returns false (rule does not fire). See §10.6 for the full predicate table.

**Context requirement:** `is_in` predicates (including user-defined) can only fire from rewrite-rule conditions (complex LHS — where the pattern matcher provides wildcard bindings). Equation-level conditions (`var = expr if cond`) provide no wildcard bindings, so `is_in` always returns false there. Stdlib authors writing dimensional-rejection rules must use rewrite-rule shape.

### 17.5 Current Scope and Limitations

- Atomic annotations, intersection annotations, four built-in named sets, and user-defined predicate sets ship as of cycle 3b.
- `compute_dim` propagation through compound expressions (`MUL`, `DIV`, `POW`, `NEG`) shipped in cycle 3c (2026-06-06, Future #7b FULL DONE). `is_in(expr, mass)` works for compound expressions like `kg*2`, not only bare annotated Vars.
- Named compound-dimension aliases (`[speed] := length/time`) are not yet supported — Future #81.
- Function-section sets (`x:fibonacci` triggers existential solve) are planned for cycle 3d.
- Stdlib `.fw` files do not yet wrap SI base units in dim sections — a natural cycle-3c follow-on.

---

## 18. Bounded Aggregation

Since gen-6 cycle 1 (2026-06-22), fwiz supports bounded aggregations — reducing a body expression over a discrete integer domain.

### 18.1 Range Literals

`[lo..hi]` and `[lo..hi @ step]` are first-class expression-grammar constructs. They parse to an internal `range(lo, hi)` or `range(lo, hi, step)` representation; bounds can be any expression.

```
[1..6]          # 1, 2, 3, 4, 5, 6
[1..10 @ 2]     # 1, 3, 5, 7, 9
[0..2*pi @ pi/4]  # 0, pi/4, pi/2, ... (symbolic bounds: stays unevaluated until concrete)
```

Disambiguation: inside `[...]`, a `..` sequence triggers range parsing; a `,` gathers a vector/matrix literal (§15). The two forms cannot be mixed inside the same bracket.

### 18.2 Reducers

Six reducers fold a body over the domain:

| Syntax | Result | Empty-domain |
|--------|--------|--------------|
| `sum(body, var in domain)` | sum of body over each domain value | 0 |
| `product(body, var in domain)` | product | 1 |
| `count(var in domain)` | number of elements (no body) | 0 |
| `max(body, var in domain)` | maximum numeric value | unevaluated |
| `min(body, var in domain)` | minimum numeric value | unevaluated |
| `mean(body, var in domain)` | exact arithmetic mean (structural fraction) | unevaluated |

`mean` returns an exact structural fraction when the domain size does not divide the sum evenly: `mean(i, i in [1..4])` → `5 / 2`, not `2.5`.

### 18.3 Iterator Clause

The explicit iterator form `sum(body, var in domain)` names the variable (`var`) and its domain. Inside `body`, `var` is the loop variable:

```
sum(i, i in [1..5])         # 15
sum(i^2, i in [1..4])       # 30
product(i, i in [1..5])     # 120
count(i in [1..5 @ 2])      # 3  (1, 3, 5)
mean(i, i in [1..4])        # 5 / 2
```

### 18.4 Symbolic Bounds

When either bound of the range is symbolic (unresolved), the aggregate stays unevaluated as a `sum(...)` call. Once the bound becomes concrete (via a solve or binding), the aggregate folds automatically:

```
total = sum(i, i in [1..n])     # stays unevaluated while n is free
                                 # resolves once n is bound (e.g. n=5 → total=15)
```

### 18.5 Formula-Call Bodies

Reducers work over formula calls in the body.

**Explicit iterator with named bindings:**
```
total = sum(dmg(atk=f, def=k), f in [1..6])
```
Each domain value substitutes the iterator into the named binding (`atk=1`, `atk=2`, …). The formula call resolves per term.

**Single-literal broadcast:**
```
total = sum(combat(atk=[1..6], def=5, dmg=?))
```
The range literal appears directly in the formula-call binding. One anonymous iterator; the binding is concretized per term. `dmg=?` selects the formula's return variable.

**Lockstep:**
```
total = sum(combat(atk=[1..6], def=atk, dmg=?))
```
`def=atk` is a lockstep binding — `def` follows `atk` value-for-value.

### 18.6 Reverse-Solve

Since the static-domain aggregate unrolls into an ordinary expression at load time, the existing solver inverts body parameters without any aggregation-specific logic:

```
total = sum(dmg(atk=f, def=k), f in [1..6])
```

With `total=21`, query `k`: the solver numerically scans `k` via the system-probe path (Strategy 6) and finds `k ~ 1`.

Use `resolve_all` (the `x=?` query form) for piecewise-body aggregations — `resolve()` first-wins can return a spurious root through multi-branch formula calls (Future #102). The algebraic `=` display (instead of `~`) for linear inverses is a planned quality upgrade (Future #101).

### 18.7 Planned / Deferred

The full collection ontology is designed but only `[..]` ranges are implemented this cycle:

- `{}` ordered array/sequence literals — not yet implemented
- Boundary brackets `[`/`(`/`]`/`)` for open/closed intervals — designed, not yet parsed
- Multi-iterator cartesian and dependent ranges (`sum(f(i,j), j in [1..6], i in [j..6])`) — deferred to a later cycle
- Solve-for-the-range-bound (`sum(i in [1..n]) = 15`, solve `n`) — deferred
