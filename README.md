# fwiz — The Formula Wizardry Tool

fwiz is a formula language that works more like a calculator than a programming language. You write equations in plain maths, and fwiz figures out how to solve for whatever variable you ask about — forwards or backwards.

```
# convert.fw
fahrenheit = celsius * 9 / 5 + 32
```

```bash
$ fwiz convert(fahrenheit=?, celsius=100)
fahrenheit = 212

$ fwiz convert(celsius=?, fahrenheit=72)
celsius = 200 / 9
```

You write the formula once. fwiz works out the algebra. The default output prefers exact forms — rational fractions, `pi`, `sqrt(2)` — for humans reading results. Add `--approximate` to collapse everything to a float (`celsius = 22.22222222`) when piping to another tool.

> **Status: active development.** fwiz is usable and tested (1800+ tests), but the language and CLI are not yet stable — syntax, flags, and output format may change between commits. Pin a specific commit if you depend on it, and expect occasional breakage on `main`.

---

## Why fwiz?

Pocket calculators compute one direction: inputs in, result out. Computer algebra systems like SymPy, Maxima, and Mathematica are powerful but large — they're programming libraries with a steep learning curve and a heavy install.

fwiz sits in between. It reads like a whiteboard, runs as a single CLI binary, and inverts equations for you automatically. Compared to the alternatives:

- **vs. a calculator** — fwiz solves backwards. Write `F = m*a`, ask for `m`.
- **vs. a spreadsheet** — fwiz equations are bidirectional. No circular-reference errors when two cells depend on each other.
- **vs. SymPy / Maxima / Mathematica** — fwiz is smaller (header-only C++, no deps), simpler (declarative `.fw` files, no scripting), and CLI-first. It doesn't try to be a full CAS — no symbolic calculus, no abstract algebra. It's optimised for *solving specific problems* from plain equations.
- **vs. a programming language** — fwiz equations have no direction. You don't pick which side of `=` is the "output"; fwiz picks per query.

The design target is a tiny, fast, scriptable inference core that both humans and LLMs can use to solve maths without incanting a library.

---

## Getting Started

### Build

```bash
make
```

Requires a C++17 compiler (GCC 7+ or Clang 5+). No external dependencies.

### Run

```bash
fwiz <file>(<var>=?, <var>=?<alias>, <var>=<value>, ...)
```

The `.fw` extension is added automatically if omitted. Use `=?` to query a variable, `=?name` to query and rename it.

### Test

```bash
make test       # functional tests (1800+)
make sanitize   # memory safety checks (ASan + UBSan)
make analyze    # static analysis (clang-tidy, zero warnings)
```

---

## A Quick Tour

### Equations

An equation declares a relationship. Given any combination of its variables, fwiz can solve for the one you don't supply:

```
distance = speed * time
```

```bash
$ fwiz f(distance=?, speed=60, time=2)    # forward
distance = 120
$ fwiz f(speed=?, distance=120, time=2)   # backward
speed = 60
```

### Defaults

A line with a bare number is a default — used when you don't supply the variable yourself:

```
g = 9.81
force = mass * g
```

### Conditions

Gate equations on comparisons with `if` (one-directional) or `iff` (bidirectional, invertible):

```
tax = income * 0.1 if income > 0 && income <= 50000
tax = income * 0.2 if income > 50000
tax = 0            if income <= 0
```

### Multi-Equation Systems

Equations that share variables compose automatically:

```
distance = speed * time
distance = sqrt((x1 - x2)^2 + (y1 - y2)^2)
```

```bash
$ fwiz f(time=?, speed=60, x1=0, y1=0, x2=30, y2=40)
time = 5 / 6
```

### Functions

Declare named functions with sections:

```
[square(x) -> result] = x ^ 2

[abs(x) -> result]
= x  iff x >= 0
= -x iff x < 0
```

### Cross-File Calls and Recursion

Call other `.fw` files, including recursively:

```
# factorial.fw
result = 1 if n <= 0
result = n * factorial(result=?prev, n=n-1) if n > 0
```

```bash
$ fwiz factorial(result=?, n=5)
result = 120
```

---

## Modes

fwiz supports several modes beyond plain solving:

| Mode | Purpose |
|------|---------|
| `--derive` | Output the symbolic equation instead of a number |
| `--verify` | Check that supplied values satisfy the equations |
| `--explore` | Solve everything that's solvable; show `?` for the rest |
| `--fit` | Find a closed-form approximation of a function |
| `--steps` / `--calc` | Show algebraic or numeric reasoning |
| `--approximate` | Collapse exact output (fractions, `pi`) to floating-point |

Example:

```bash
$ fwiz --derive f(time=?, distance=d, speed=s)
time = d / s

$ fwiz --fit formula(y=?, x=x)              # y = pi * x
y = pi * x
```

Numeric solving is enabled by default — nonlinear equations (quadratics, transcendentals, recursive inverses) fall back to adaptive grid scan + Newton/bisection. Exact results use `=`, approximate results use `~`.

---

## Examples

Take a look at the `examples/` directory:

- **physics.fw** — Force, kinematics, kinetic energy, circles
- **finance.fw** — Pricing with tax, profit margins
- **navigation.fw** — Travel time from coordinates (multi-equation substitution)
- **convert.fw** — Temperature, distance, weight conversions
- **geometry.fw** — Rectangle area, perimeter, diagonal (multi-return)
- **triangle.fw** — Complete triangle solver: any 3 knowns → all unknowns
- **rectangle.fw** / **box.fw** — Cross-file formula calls
- **factorial.fw** — Recursive factorial with conditional base case

---

## Standard Library

The `stdlib/` directory ships a small library of reusable `.fw` files:

- **`stdlib/stdlib.fw`** — piecewise helpers: `abs`, `sign`, `clamp`, `max`, `min`, `step`. Pure fwiz, bidirectional, so they invert cleanly.
- **`stdlib/builtin.fw`** — reference definitions for the C++-backed built-ins (`sin`, `cos`, `sqrt`, etc.). Read this if you want to write your own `@extern` function.

See [docs/Language.md §14](docs/Language.md#14-standard-library) for the full surface.

---

## Documentation

Detailed references live in [`docs/`](docs/):

| Document | What's in it |
|----------|-------------|
| [docs/Getting-Started.md](docs/Getting-Started.md) | Install, first `.fw` file, first few solves |
| [docs/Language.md](docs/Language.md) | Complete syntax and semantics reference |
| [docs/Solver.md](docs/Solver.md) | Resolution strategies, numeric mode, derive, verify, explore |
| [docs/Fitting.md](docs/Fitting.md) | Curve fitting: templates, composition, constant recognition |
| [docs/CLI.md](docs/CLI.md) | Command-line flags, exit codes, error messages |
| [docs/Developer.md](docs/Developer.md) | Implementation architecture and conventions |
| [docs/Future.md](docs/Future.md) | Roadmap and planned features |
| [docs/Known-Issues.md](docs/Known-Issues.md) | Current limitations |
| [docs/Testing.md](docs/Testing.md) | Test organization |

---

## License

MIT — see [LICENSE](LICENSE).
