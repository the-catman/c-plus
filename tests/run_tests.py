#!/usr/bin/env python3
"""Runs the C+ language tests.

Usage: python3 tests/run_tests.py <path to cplus> [name filter]

Every tests/cases/*.cplus file is compiled and checked against directives written
in its comments. A test can use these, one per line, anywhere in the file:

  // expect: <text>    the next line the program must print (in order)
  // error: <text>     compiling must fail, and the errors must contain <text>
  // warning: <text>   compiling must print a warning containing <text>; a test with no
                       warning directives fails if the compiler prints any warning
  // trap: <text>      the program must stop with a runtime error containing <text>
  // stderr: <text>    what the program prints to stderr must contain <text>, e.g. a
                       leak report, which otherwise fails the test
  // exit: <number>    the exit code the program must return (default 0)
  // flags: <flags>    options for the compiler, e.g. -Wno-new-delete

A program that runs is built twice, without and with -O, and must behave the same
both times. It runs with CPLUS_CHECK_LEAKS=1, so a class object that is never freed,
or one used after it was freed, fails the test.

The runner also checks that every spec anchor in compiler/SpecRefs.h is a real
heading in docs/SPEC.md, so diagnostics never point at a missing section.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
CASES = ROOT / "tests" / "cases"


def slugify(heading):
    s = heading.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def check_spec_anchors():
    refs = re.findall(r'=\s*"([a-z0-9-]+)"', (ROOT / "compiler" / "SpecRefs.h").read_text())
    headings = {slugify(m) for m in re.findall(r"^#+\s+(.*)$", (ROOT / "docs" / "SPEC.md").read_text(), re.M)}
    missing = [r for r in refs if r not in headings]
    for r in missing:
        print(f"FAIL spec anchor '{r}' from compiler/SpecRefs.h has no heading in docs/SPEC.md")
    return not missing


def run_case(cplus, path, workdir):
    text = path.read_text()
    expect = re.findall(r"//[ \t]*expect:[ \t]?(.*)$", text, re.M)
    errors = re.findall(r"//[ \t]*error:[ \t]*(.*?)[ \t]*$", text, re.M)
    traps = re.findall(r"//[ \t]*trap:[ \t]*(.*?)[ \t]*$", text, re.M)
    warnings = re.findall(r"//[ \t]*warning:[ \t]*(.*?)[ \t]*$", text, re.M)
    stderr = re.findall(r"//[ \t]*stderr:[ \t]*(.*?)[ \t]*$", text, re.M)
    flags_m = re.search(r"//[ \t]*flags:[ \t]*(.*?)[ \t]*$", text, re.M)
    extra = flags_m.group(1).split() if flags_m else []
    exit_m = re.search(r"//[ \t]*exit:[ \t]*(-?\d+)", text)
    want_exit = int(exit_m.group(1)) if exit_m else 0
    rel = path.relative_to(ROOT)

    if errors:
        build = subprocess.run([cplus, "build", str(rel), "-o", str(workdir / path.stem), *extra], cwd=ROOT,
                               capture_output=True, text=True)
        if build.returncode == 0:
            return "expected a compile error, but it compiled"
        missing = [e for e in errors if e not in build.stderr]
        if missing:
            return "compile errors did not contain: " + "; ".join(missing) + "\n--- stderr ---\n" + build.stderr
        return None
    for flags in ([], ["-O"]):
        problem = run_program(cplus, rel, workdir / path.stem, [*extra, *flags], expect, traps, want_exit, warnings,
                              stderr)
        if problem:
            return (f"with {' '.join(flags)}: " if flags else "") + problem
    return None


def run_program(cplus, rel, exe, flags, expect, traps, want_exit, warnings, stderr):
    build = subprocess.run([cplus, "build", str(rel), "-o", str(exe), *flags], cwd=ROOT, capture_output=True,
                           text=True)
    if build.returncode != 0:
        return "failed to compile:\n" + build.stderr
    if not warnings and "warning:" in build.stderr:
        return "unexpected warning:\n" + build.stderr
    missing = [w for w in warnings if w not in build.stderr]
    if missing:
        return "warnings did not contain: " + "; ".join(missing) + "\n--- stderr ---\n" + build.stderr
    env = dict(os.environ, CPLUS_CHECK_LEAKS="1")
    try:
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10, env=env)
    except subprocess.TimeoutExpired:
        return "timed out after 10 seconds"
    if traps:
        if run.returncode == 0:
            return "expected a runtime error, but the program exited normally"
        missing = [t for t in traps if t not in run.stderr]
        if missing:
            return "runtime error did not contain: " + "; ".join(missing) + "\n--- stderr ---\n" + run.stderr
    elif run.returncode != want_exit:
        return f"exit code {run.returncode}, expected {want_exit}\n--- stderr ---\n{run.stderr}"
    missing = [e for e in stderr if e not in run.stderr]
    if missing:
        return "stderr did not contain: " + "; ".join(missing) + "\n--- stderr ---\n" + run.stderr
    reported = any("never freed" in e for e in stderr)
    if ("never freed" in run.stderr and not reported) or "internal error" in run.stderr:
        return "memory problem:\n" + run.stderr
    got = run.stdout.splitlines()
    if got != expect:
        lines = ["output differs", "--- expected ---", *expect, "--- got ---", *got]
        return "\n".join(lines)
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cplus = str(pathlib.Path(sys.argv[1]).resolve())
    name_filter = sys.argv[2] if len(sys.argv) > 2 else ""
    ok = check_spec_anchors()
    passed = failed = 0
    with tempfile.TemporaryDirectory(prefix="cplus-tests-") as tmp:
        for path in sorted(CASES.glob("*.cplus")):
            if name_filter not in path.name:
                continue
            problem = run_case(cplus, path, pathlib.Path(tmp))
            if problem:
                failed += 1
                print(f"FAIL {path.relative_to(ROOT)}\n  " + problem.replace("\n", "\n  "))
            else:
                passed += 1
    print(f"{passed} passed, {failed} failed")
    return 0 if ok and failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
