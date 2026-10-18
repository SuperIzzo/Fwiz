#!/usr/bin/env python3
"""Offline tests for run.py using a mock LLM backend (no budget spent)."""
import os
import tempfile
import unittest

import run


def args(backend, **kw):
    ns = run.argparse.Namespace(dry_run=False, model="mock", max_runs=3, max_turns=4, backend=backend)
    ns.__dict__.update(kw)
    return ns


class RunnerTests(unittest.TestCase):
    problem = run.bench.load_problems("alg-linear-system")[0]

    def test_parse_reply(self):
        files, cmds = run.parse_reply("x\n```fw a.fw\ns = x + y\n```\n```fwiz 'a.fw(x=?)'\n```\n")
        self.assertEqual(files, {"a.fw": "s = x + y\n"})
        self.assertEqual(cmds, ["'a.fw(x=?)'"])

    def test_mock_recovers_after_error(self):
        replies = iter([
            "```fw p.fw\ns = x + y\nd = x - y\n```\n```fwiz 'p.fw(x=?)'\n```",
            "```fwiz 'p.fw(x=?, s=10, d=2)'\n```",
        ])
        r = run.run_problem(self.problem, args(lambda *a: next(replies)), "")
        self.assertTrue(r["pass"], r)
        self.assertEqual((r["turns"], r["fwiz_runs"]), (2, 2))

    def test_wrong_answer_fails(self):
        reply = "```fw p.fw\nx = 7\n```\n```fwiz 'p.fw(x=?)'\n```"
        r = run.run_problem(self.problem, args(lambda *a: reply), "")
        self.assertFalse(r["pass"])
        self.assertEqual(r["fwiz_runs"], 3)  # a wrong printed value must not stop the run

    def test_wrong_then_right_recovers(self):
        replies = iter([
            "```fw p.fw\nx = 7\n```\n```fwiz 'p.fw(x=?)'\n```",
            "```fw p.fw\ns = x + y\nd = x - y\n```\n```fwiz 'p.fw(x=?, s=10, d=2)'\n```",
        ])
        r = run.run_problem(self.problem, args(lambda *a: next(replies)), "")
        self.assertTrue(r["pass"], r)
        self.assertEqual(r["turns"], 2)

    def test_backend_error_recorded(self):
        def boom(*a):
            raise RuntimeError("down")
        r = run.run_problem(self.problem, args(boom), "")
        self.assertFalse(r["pass"])
        self.assertIn("down", r["error"])

    def test_dry_run_all_pass(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(run.main(["--dry-run", "--out", out]), 0)
            self.assertTrue(os.path.exists(os.path.join(out, "results.json")))
            self.assertTrue(os.path.exists(os.path.join(out, "summary.md")))


if __name__ == "__main__":
    unittest.main()
