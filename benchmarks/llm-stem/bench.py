"""Shared helpers: load problems, run fwiz in a scratch dir, score output."""
import ast
import glob
import json
import math
import os
import re
import shlex
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FWIZ = os.path.join(REPO, "bin", "fwiz")
DEFAULT_REL_TOL = 1e-6
RUN_TIMEOUT = 60


def load_problems(pattern="*"):
    paths = sorted(glob.glob(os.path.join(HERE, "problems", pattern + ".json")))
    return [dict(json.load(open(p)), _path=p) for p in paths]


def write_files(workdir, files):
    """files: {name: [lines] or str}. Names are flattened to a basename."""
    for name, body in files.items():
        if isinstance(body, list):
            body = "\n".join(body) + "\n"
        with open(os.path.join(workdir, os.path.basename(name)), "w") as f:
            f.write(body)


def run_fwiz(workdir, command):
    """Run `fwiz <command>` (no shell) in workdir; stdlib is on FWIZ_PATH."""
    try:
        args = shlex.split(command)
    except ValueError as e:
        return {"command": command, "exit": -1, "stdout": "", "stderr": f"bad command: {e}"}
    env = dict(os.environ, FWIZ_PATH=REPO)
    try:
        p = subprocess.run([FWIZ] + args, cwd=workdir, env=env, capture_output=True,
                           text=True, timeout=RUN_TIMEOUT)
        return {"command": command, "exit": p.returncode, "stdout": p.stdout, "stderr": p.stderr}
    except subprocess.TimeoutExpired:
        return {"command": command, "exit": -1, "stdout": "", "stderr": f"timeout after {RUN_TIMEOUT}s"}


_NAMES = {"pi": math.pi, "e": math.e, "phi": (1 + 5 ** 0.5) / 2}
_FUNCS = {f: getattr(math, f) for f in ("sqrt", "log", "sin", "cos", "tan", "asin", "acos", "atan", "exp")}
_FUNCS["abs"] = abs


def eval_value(text):
    """Evaluate a fwiz result like `200 / 9`, `8 * e^2`, `log(10)`; None if not numeric."""
    def ev(n):
        if isinstance(n, ast.Expression):
            return ev(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, (int, float)):
            return float(n.value)
        if isinstance(n, ast.Name) and n.id in _NAMES:
            return _NAMES[n.id]
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.USub, ast.UAdd)):
            v = ev(n.operand)
            return -v if isinstance(n.op, ast.USub) else v
        if isinstance(n, ast.BinOp):
            a, b = ev(n.left), ev(n.right)
            ops = {ast.Add: lambda: a + b, ast.Sub: lambda: a - b, ast.Mult: lambda: a * b,
                   ast.Div: lambda: a / b, ast.Pow: lambda: a ** b}
            if type(n.op) in ops:
                return ops[type(n.op)]()
        if (isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id in _FUNCS
                and len(n.args) == 1 and not n.keywords):
            return _FUNCS[n.func.id](ev(n.args[0]))
        raise ValueError("unsupported")
    try:
        v = ev(ast.parse(text.replace("^", "**"), mode="eval"))
        return float(v) if isinstance(v, (int, float)) and math.isfinite(v) else None
    except (SyntaxError, ValueError, TypeError, ZeroDivisionError, OverflowError):
        return None


_LINE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_.]*)\s*([=~])\s*(.+?)\s*(?:\s\s\(.*\))?$")


def extract_values(stdout, var):
    """All values printed for `var` (`var = v` / `var ~ v`); unparseable ones are None."""
    out = []
    for line in stdout.splitlines():
        m = _LINE.match(line)
        if m and m.group(1) == var:
            out.append(eval_value(m.group(3)))
    return out


def close(a, b, rel_tol):
    return a is not None and math.isclose(a, b, rel_tol=rel_tol, abs_tol=1e-9)


def score(problem, stdout):
    """Pass iff the printed values for the answer var match the expected set exactly."""
    ans = problem["answer"]
    tol = ans.get("rel_tol", DEFAULT_REL_TOL)
    got = extract_values(stdout, ans["var"])
    want = ans["values"]
    ok = (bool(got) and len(got) == len(want)
          and all(any(close(g, w, tol) for g in got) for w in want)
          and all(any(close(g, w, tol) for w in want) for g in got))
    return {"pass": ok, "got": got, "expected": want}
