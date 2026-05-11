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
$ fwiz physics(force=?, mass=10, acceleration=3)
force = 30

$ fwiz physics(acceleration=?, force=30, mass=10)
acceleration = 3

$ fwiz physics(mass=?, force=30, acceleration=3)
mass = 10
```

## 4. Defaults

Give a variable a fallback value by putting a bare number on the right:

```
# physics.fw
g = 10                  # handy round number; Earth gravity is 9.81 m/s²
force = mass * g
```

Now `g` defaults to that value:

```bash
$ fwiz physics(force=?, mass=10)
force = 100
```

If you *do* provide `g`, yours wins:

```bash
$ fwiz physics(force=?, mass=5, g=3)
force = 15
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

## 10. Recent Features

fwiz keeps growing. Here are six features added in the most recent cycles, with one runnable example each.

### Symbolic integration

Ask for an antiderivative via the CLI `integral(...)=?` query target — parallel to `diff(...)=?`. Create a small file:

```
# demo.fw
f = x^2
```

Then query the indefinite integral:

```bash
$ fwiz 'demo.fw(integral(f, x)=?antideriv)'
antideriv = x^3 / 3
```

Definite integrals work inline because the result is a number:

```bash
$ fwiz '(area=?) area = integral(x^2, x, 0, 3)'
area = 9
```

Integration uses three tiers: closed-form patterns, u-substitution, and integration by parts (LIATE heuristic). Unrecognised forms preserve the `integral(...)` call unevaluated. See [Solver.md §6.5](Solver.md#65-symbolic-integration).

### Inverse-solve through an integral

Because `integral` is part of the algebra, fwiz can solve for an integration bound. Given `area.fw`:

```
A = integral(x^2, x, 0, b)
```

```bash
$ fwiz 'area(A=9, b=?)'
b = 3
```

### Batch sweeps — `--table`

Evaluate a query across a range of inputs and get a TSV table:

```bash
$ fwiz --table 'examples/triangle(C=?, a=[1..5], b=4, c=5)'
a   C
1   180
2   108.2099569
3   90
4   77.36437491
5   66.42182152
```

The header shows the range variables (in CLI order) followed by the query aliases. Scalar inputs like `b=4, c=5` apply per row but don't appear as columns. Ranges support custom steps (`[0..2*pi @ pi/4]`) and compound concatenation (`[1..3, 7..9]`). Add `--zip` to pair inputs element-wise instead of taking the cartesian product. See [CLI.md §6](CLI.md#6-batch--table-mode).

### Vector and matrix literals

Write vectors and matrices directly:

```bash
$ fwiz --derive '(v=?) v = [1, 2, 3] + [4, 5, 6]'
v = [5, 7, 9]

$ fwiz '(d=?) d = det([[1, 2], [3, 4]])'
d = -2

$ fwiz --derive '(M=?) M = matmul([[1, 2], [3, 4]], [[5, 6], [7, 8]])'
M = [[19, 22], [43, 50]]
```

Element-wise add/sub and scalar multiplication simplify automatically. Builtins: `matmul`, `det` (2×2 and 3×3), `inv` (2×2), `transpose`. Matrix-valued results need `--derive` because the numeric solver doesn't reduce matrices to scalars; `det` returns a scalar and works in either mode. See [Language.md §15](Language.md#15-vector-and-matrix-literals).

### Complex numbers

The imaginary unit `i` is a built-in symbolic constant:

```bash
$ fwiz '(z=?) z = i * i'
z = -1
```

Complex arithmetic is symbolic-only — fwiz routes it through the simplifier rather than the numeric evaluator. See [Language.md §16](Language.md#16-complex-numbers).

### Periodic solutions

Trig equations with infinitely many roots return a parametric family. Use `--no-numeric` to suppress single-root probing and ask for the symbolic family instead:

```bash
$ fwiz --no-numeric '(x=?, result=1/2) result = sin(x)'
x = 1 / 6 * pi + k * 2 * pi  # k in Z
x = 5 / 6 * pi + k * 2 * pi  # k in Z
```

Each line is one branch of the family; the `# k in Z` comment marks `k` as an integer parameter. Pick any integer to get a concrete root.

---

## 11. Where Next?

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
