# fwiz — The Formula Wizard

fwiz is a formula language that works more like a calculator than a programming language. You write equations in plain maths, and fwiz figures out how to solve for whatever variable you ask about — forwards or backwards.

```
# convert.fw
fahrenheit = celsius * 9 / 5 + 32
```

```bash
$ fwiz convert(fahrenheit=?, celsius=100)
fahrenheit = 212

$ fwiz convert(celsius=?, fahrenheit=72)
celsius = 22.22222222
```

You write the formula once. fwiz works out the algebra.

---

## Getting Started

### Build

```bash
make
```

Requires a C++17 compiler (GCC 7+ or Clang 5+).

### Run

```bash
fwiz <file>(<var>=?, <var>=?<alias>, <var>=<value>, ...)
```

The `.fw` extension is added automatically if omitted. Use `=?` to query a variable, `=?name` to query and rename it.

### Run tests

```bash
make test       # functional tests (1300+ tests)
make sanitize   # memory safety checks (ASan + UBSan)
make analyze    # static analysis (clang-tidy, zero warnings)
```

---

## The Language

A `.fw` file contains **equations**, **defaults**, and **comments**. That's it.

### Equations

An equation defines a relationship between variables:

```
distance = speed * time
```

This isn't an assignment — it's a declaration that these three things are related. Given any two, fwiz can solve for the third:

```bash
$ fwiz f(distance=?, speed=60, time=2)
distance = 120

$ fwiz f(speed=?, distance=120, time=2)
speed = 60

$ fwiz f(time=?, distance=120, speed=60)
time = 2
```

### Defaults

A default gives a variable a fallback value:

```
g = 9.81
force = mass * g
```

If you don't provide `g`, it uses `9.81`. If you do provide it, yours wins:

```bash
$ fwiz f(force=?, mass=10)
force = 98.1

$ fwiz f(force=?, mass=10, g=10)
force = 100
```

When you query a variable that has a default, the default is ignored — fwiz solves from the equations instead:

```bash
$ fwiz f(g=?, force=100, mass=10)
g = 10
```

### Comments

Lines starting with `#` are ignored:

```
# Newton's second law
force = mass * g
```

### Operators

| Operator | Meaning        | Example          |
|----------|----------------|------------------|
| `+`      | Addition       | `x + y`          |
| `-`      | Subtraction    | `x - y`          |
| `*`      | Multiplication | `x * y`          |
| `/`      | Division       | `x / y`          |
| `^`      | Power          | `x ^ 2`          |
| `-`      | Negation       | `-x`             |
| `( )`    | Grouping       | `(x + y) * z`    |

Standard precedence: `^` binds tightest, then `* /`, then `+ -`.

### Built-in Functions

| Function  | Meaning                     |
|-----------|-----------------------------|
| `sqrt(x)` | Square root                |
| `abs(x)`  | Absolute value             |
| `sin(x)`  | Sine (radians)             |
| `cos(x)`  | Cosine (radians)           |
| `tan(x)`  | Tangent (radians)          |
| `asin(x)` | Inverse sine (radians)     |
| `acos(x)` | Inverse cosine (radians)   |
| `atan(x)` | Inverse tangent (radians)  |
| `log(x)`  | Natural logarithm          |

---

## Multi-Equation Systems

The real power is combining equations. When two equations share a variable, fwiz substitutes one into the other automatically.

```
# navigation.fw
distance = speed * time
distance = sqrt((x1 - x2)^2 + (y1 - y2)^2)
```

Now you can ask for travel time from coordinates:

```bash
$ fwiz navigation(time=?, speed=60, x1=0, y1=0, x2=30, y2=40)
time = 0.8333333333
```

fwiz sees that both equations define `distance`, substitutes one into the other, and solves for `time`:

```
speed * time = sqrt((x1 - x2)^2 + (y1 - y2)^2)
time = sqrt((0 - 30)^2 + (0 - 40)^2) / 60
time = 50 / 60
time = 0.833...
```

### Equation Chains

Equations can reference variables defined by other equations:

```
x = a + 1
y = x * 2
```

Query `y` given `a`, and fwiz resolves the chain: first finds `x` from `a`, then `y` from `x`.

```bash
$ fwiz f(y=?, a=4)
y = 10
```

This works in reverse too — provide `y` and solve for `a`:

```bash
$ fwiz f(a=?, y=10)
a = 4
```

---

## Multiple Returns

Use multiple `=?` parameters to solve for several variables at once:

```
# geometry.fw
area = width * height
perimeter = 2 * width + 2 * height
```

```bash
$ fwiz geometry(area=?, perimeter=?, width=5, height=3)
area = 15
perimeter = 16
```

Each `=?` variable is solved independently using the same bindings.

### Aliases

When calling formulas from other files (coming soon), variable names might collide. Use `=?alias` to rename the output:

```bash
$ fwiz geometry(area=?a, perimeter=?p, width=5, height=3)
a = 15
p = 16
```

The syntax `area=?a` means: "solve for `area`, call the result `a`." Bare `=?` uses the variable's own name as the output name.

---

## Debugging with `--steps` and `--calc`

Use `--steps` to see fwiz's algebraic reasoning:

```bash
$ fwiz --steps convert(celsius=?, fahrenheit=72)
loading convert.fw
  equation: fahrenheit = celsius * 9 / 5 + 32

solving for: celsius
  given:
    fahrenheit = 72
  inverting: fahrenheit = celsius * 9 / 5 + 32
    => celsius = (fahrenheit - 32) * 5 / 9
  result: celsius = 22.2222
celsius = 22.22222222
```

Use `--calc` to also see the numeric substitution and evaluation:

```bash
$ fwiz --calc convert(celsius=?, fahrenheit=72)
  ...
    substitute fahrenheit = 72
    evaluate: (72 - 32) * 5 / 9
  result: celsius = 22.2222
```

The trace goes to stderr, so it doesn't interfere with piping the result.

---

## Symbolic Derivation with `--derive`

Use `--derive` to output the symbolic equation instead of a numeric result:

```bash
$ fwiz --derive convert(celsius=?, fahrenheit=f)
celsius = (f - 32) / 1.8

$ fwiz --derive physics(time=?, distance=d, speed=v)
time = d / v
```

Derive mode unfolds formula calls — if a formula call's body is simple enough, it's inlined into the parent expression and solved algebraically:

```bash
$ fwiz --derive box(width=?, volume=V, depth=d, height=h)
width = V / h / d

$ fwiz --derive factorial(result=?, n=5)
result = 120
```

---

## Numeric Solving

Numeric solving is enabled by default. When algebraic inversion can't solve an equation (target in a power, trig function, denominator, or recursive call), fwiz falls back to numeric root-finding.

```bash
$ fwiz formula(x=?, y=9)        # y = x^2
x = -3
x = 3

$ fwiz formula(x=?, y=1)        # y = x + sin(x)
x ~ 0.5109734294
```

Results verified as exact by forward computation show `=`. Approximate results show `~`.

Use conditions to constrain the search range:

```bash
$ fwiz formula(x=?, y=9)        # y = x^2, x > 0
x = 3
```

Flags:
- `--no-numeric` — disable numeric solving (algebraic only)
- `--precision N` — set scan density (default 200, higher finds more roots)

---

## How the Solver Works

When you ask fwiz to solve for a variable, it tries these strategies in order:

1. **Direct**: Is there an equation `target = ...`? Evaluate the right side.
2. **Invert**: Does the target appear in an equation's right side? Algebraically isolate it.
3. **Forward formula call**: Is there a formula call that produces the target? Evaluate it.
4. **Substitute**: Do two equations share a left-hand variable? Set them equal and solve.
5. **Reverse formula call**: Does the target appear in a formula call's bindings? Solve backwards.
6. **Numeric**: If all algebraic strategies fail, scan the equation numerically (adaptive grid + Newton/bisection).

At each step, if a needed variable is unknown, fwiz recursively tries to solve for it from other equations.

### What fwiz can solve

**Algebraically** (exact, instant): linear equations where the solve target appears in additions, subtractions, and multiplied/divided by constants. Like terms are combined (`y + 3 * y` → `4 * y`).

**Numerically** (enabled by default): nonlinear equations where algebraic inversion fails. Uses adaptive grid scan with Newton's method and bisection refinement. Finds all roots in the search range.

```bash
$ fwiz formula(x=?, y=9)     # y = x^2 → finds both roots
x = -3
x = 3

$ fwiz formula(x=?, y=1)     # y = x + sin(x) → approximate
x ~ 0.5109734294
```

Exact results show `=`, approximate results show `~`. Use `--no-numeric` for algebraic-only solving, or `--precision N` to control scan density (default 200).

### What fwiz can't solve (yet)

- **Systems of simultaneous equations**: only substitution via shared variables, not Gaussian elimination

### How fwiz chooses between equations

When multiple equations can solve for the same variable, fwiz uses **first valid result with recursive backtracking**:

1. Try equations in file order
2. For each equation, recursively resolve any unknown variables it needs
3. If the recursion hits a dead end (circular dependency, missing data), skip and try the next equation
4. If evaluation produces NaN or infinity, skip and try the next equation
5. The first equation that produces a finite result wins

This means **file order is your priority system**. List the simplest, most general equations first. The triangle solver demonstrates this: angle-sum equations come first (simplest), then law of cosines (two sides + angle), then law of sines (fallback). The solver automatically finds the right path through the equations regardless of which variables you provide.

---

## File Format

- Extension: `.fw` (added automatically by the CLI if omitted)
- Encoding: UTF-8 (with or without BOM)
- Line endings: Unix (LF), Windows (CRLF), or mixed
- Whitespace: leading/trailing whitespace and blank lines are ignored
- Comments: lines starting with `#`

### Equation syntax

```
variable_name = expression
```

Variable names: letters, digits, and underscores. Must start with a letter or underscore.

### Default syntax

```
variable_name = number
```

A line is treated as a default (not an equation) when the right side is a bare number.

---

## Equation Order

When multiple equations can solve for the same variable, **the first matching equation wins**. If that equation produces an invalid result (NaN or infinity), fwiz automatically tries the next one.

---

## Error Messages

fwiz gives specific error messages for common problems:

| Problem | Message |
|---|---|
| Variable not in any equation | `No equation found for 'w'` |
| Missing input value | `Cannot solve for 'x': no value for 'z'` |
| All paths give NaN/infinity | `Cannot solve for 'x': all equations produced invalid results (NaN or infinity)` |
| File not found | `Cannot open file: path/to/file.fw` |
| Path is a directory | `Path is a directory, not a file: path/` |
| Invalid CLI value | `Invalid number 'abc' for variable 'y'` |
| Infinity/NaN as input | `Infinity is not a valid value for 'y'` |

Use `--steps` to see the full solve reasoning when debugging failures.

---

## Examples

See the `examples/` directory:

- **physics.fw** — Force, kinematics, kinetic energy, circles
- **finance.fw** — Pricing with tax, profit margins
- **navigation.fw** — Travel time from coordinates (multi-equation substitution)
- **convert.fw** — Temperature, distance, weight conversions
- **geometry.fw** — Rectangle area, perimeter, diagonal (multi-return)
- **triangle.fw** — Complete triangle solver: any 3 knowns → all unknowns
- **rectangle.fw** — Simple area formula (used by box.fw via formula calls)
- **box.fw** — Box surface area and volume via cross-file formula calls to rectangle.fw
- **factorial.fw** — Recursive factorial with conditional base case (demonstrates Turing completeness)
