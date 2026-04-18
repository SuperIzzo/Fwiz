# Getting Started with fwiz

This guide walks you from zero to solving your first formulas in about five minutes.

For the complete language reference, see [Language.md](Language.md).
For every CLI flag, see [CLI.md](CLI.md).

## 1. Install

### Build from source

fwiz is a single header-only C++17 project with no external dependencies.

```bash
git clone https://github.com/<your-org>/fwiz.git
cd fwiz
make
```

You need GCC 7+ or Clang 5+. The binary lands in `bin/fwiz`.

Add it to your `PATH` (optional):

```bash
export PATH="$PWD/bin:$PATH"
```

### Verify

```bash
fwiz --help 2>/dev/null || echo "ok, run fwiz with a .fw file"
```

## 2. Your First Formula

Create a file called `convert.fw`:

```
fahrenheit = celsius * 9 / 5 + 32
```

That's it. One line. It says: *these three variables are related by this equation.*

Now solve it — **forwards**:

```bash
$ fwiz convert(fahrenheit=?, celsius=100)
fahrenheit = 212
```

And **backwards**:

```bash
$ fwiz convert(celsius=?, fahrenheit=72)
celsius = 200 / 9
```

You wrote the formula once. fwiz inverted it for you. It prefers exact fractions over decimal approximations — here `200 / 9` is the exact answer, not `22.2222…`.

### How to read the CLI

```
fwiz convert(fahrenheit=?, celsius=100)
     ^^^^^^^ ^^^^^^^^^^^^^ ^^^^^^^^^^^
     file    query          input
```

- `celsius=100` — "I'm telling you `celsius` is `100`"
- `fahrenheit=?` — "Please solve for `fahrenheit`"

The `.fw` extension on the file is added automatically.

## 3. Multi-Variable Formulas

Make a new file `physics.fw`:

```
force = mass * acceleration
```

Now you can ask for any one of the three given the other two:

```bash
$ fwiz physics(force=?, mass=10, acceleration=9.81)
force = 98.1

$ fwiz physics(acceleration=?, force=98.1, mass=10)
acceleration = 9.81

$ fwiz physics(mass=?, force=98.1, acceleration=9.81)
mass = 10
```

## 4. Defaults

Give a variable a fallback value by putting a bare number on the right:

```
# physics.fw
g = 9.81
force = mass * g
```

Now `g` defaults to Earth gravity:

```bash
$ fwiz physics(force=?, mass=10)
force = 98.1
```

If you *do* provide `g`, yours wins:

```bash
$ fwiz physics(force=?, mass=5, g=1.62)    # moon
force = 8.1
```

## 5. Combining Equations

fwiz composes equations that share variables. Create `navigation.fw`:

```
distance = speed * time
distance = sqrt((x1 - x2)^2 + (y1 - y2)^2)
```

Now you can solve for travel time directly from coordinates:

```bash
$ fwiz navigation(time=?, speed=60, x1=0, y1=0, x2=30, y2=40)
time = 5 / 6
```

fwiz saw that `distance` appears in both equations, substituted one into the other, and solved for `time`. You didn't have to tell it how.

## 6. Multiple Queries, One Call

Solve for several variables at once:

```bash
$ fwiz geometry(area=?, perimeter=?, width=5, height=3)
area = 15
perimeter = 16
```

Or rename outputs with an alias (`=?name`):

```bash
$ fwiz geometry(area=?a, perimeter=?p, width=5, height=3)
a = 15
p = 16
```

## 7. When Algebra Isn't Enough

Some equations can't be solved symbolically — but fwiz falls back to numerics automatically:

```bash
$ fwiz formula(x=?, y=9)        # y = x^2
x = -3
x = 3

$ fwiz formula(x=?, y=1)        # y = x + sin(x)
x ~ 0.5109734294
```

Exact results use `=`, approximate results use `~`.

## 8. See the Work

When something confuses you, add `--steps` to watch fwiz reason:

```bash
$ fwiz --steps convert(celsius=?, fahrenheit=72)
loading convert.fw
  equation: fahrenheit = celsius * 9 / 5 + 32

solving for: celsius
  given:
    fahrenheit = 72
  result: celsius = 22.22222222
celsius = 200 / 9
```

Trace output goes to stderr, so you can still pipe the result.

## 9. Projects with Multiple Files

Once you have more than one formula, split them across files. When `box.fw` calls `rectangle(...)` and you don't define `rectangle` inside `box.fw`, fwiz looks for a `rectangle.fw` file in the current directory:

```
my-project/
├── rectangle.fw      # [rectangle(width, height) -> area] = width * height
└── box.fw            # rectangle(area=?bottom, width=w, height=d)
```

```bash
$ cd my-project/
$ fwiz box(volume=?, w=2, d=3, h=4)
```

**Key rule**: file resolution is relative to your *current working directory*, not to the calling file. Invoke fwiz from the root of your project.

You can also share a small standard library. fwiz ships one in `stdlib/`:

- `stdlib/stdlib.fw` — `abs`, `sign`, `clamp`, `max`, `min`, `step`
- `stdlib/builtin.fw` — reference for how the C++-backed built-ins (`sin`, `cos`, `sqrt`, `log`) are wired up

Copy or symlink `stdlib/stdlib.fw` into your project directory to use the helpers. For the full story — resolution rules, when to split, typical layouts — see [Language.md §7.6](Language.md#76-project-structure).

## 10. Where Next?

You now know the essentials. When you're ready for more:

- **[Language.md](Language.md)** — full syntax: conditions (`if` / `iff`), sections (`[name(args) -> return]`), formula calls, recursion, rewrite rules
- **[Solver.md](Solver.md)** — how fwiz decides which strategy to try; symbolic `--derive`, `--verify`, `--explore`
- **[Fitting.md](Fitting.md)** — `--fit` discovers closed-form approximations (polynomials, trig, exponentials, compositions)
- **[CLI.md](CLI.md)** — every flag, every error message
- **[../examples/](../examples/)** — physics, finance, geometry, triangle solver, recursive factorial

### Try the examples

```bash
# Complete triangle solver: angles from three sides (law of cosines)
$ fwiz --explore examples/triangle(A=?, B=?, C=?, a=3, b=4, c=5)

# Recursive factorial (Turing-complete via recursion)
$ fwiz examples/factorial(result=?, n=7)

# Curve fitting
$ fwiz --fit examples/convert(fahrenheit=?, celsius=x)
```
