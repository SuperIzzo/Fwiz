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
11. [Built-in Functions](#11-built-in-functions)
12. [Built-in Constants](#12-built-in-constants)
13. [Special Values](#13-special-values)
14. [Standard Library](#14-standard-library)

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
- Scientific: `1e9`, `6.022e23`, `1.5e-3`

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
| `[ ]` | Section header brackets |
| `->` | Section return-variable arrow |
| `@` | Prefix for directives (currently only `@extern`) |
| `#` | Line comment |

### Keywords

`if`, `iff`, `undefined`. Plus the directive `@extern`. These cannot be used as variable names.

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

When `box.fw` calls `rectangle(...)`:

1. If `box.fw` defines a section `[rectangle(...) -> ...]`, use that.
2. Otherwise, fwiz looks for `rectangle.fw` in the **current working directory** (where you ran `fwiz`, not where `box.fw` lives).
3. If not found, the call fails with `No equation found for 'rectangle'`.

Because resolution is CWD-relative, invoke fwiz from the root of your project so relative paths work consistently:

```bash
$ cd my-project/
$ fwiz box(volume=?, width=2, height=3, depth=4)
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

In `--derive` mode these are preserved symbolically:

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

### 14.2 `stdlib/builtin.fw` — reference for C++ built-ins

The `.fw` representation of the C++-backed built-ins (`sin`, `cos`, `sqrt`, `log`, etc.). Each section shows the `@extern` directive and the inverse equation used for reverse solving:

```
[sin(x) -> result] @extern sin
x = asin(result)
```

This file is **reference documentation** — the actual built-ins are compiled into the fwiz binary. Read it when you want to define your own `@extern`-backed function in C++: copy the pattern, register via `register_function()`, and your new function behaves like a native built-in.
