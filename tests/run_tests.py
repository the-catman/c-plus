#!/usr/bin/env python3
"""Runs the C+ language tests.

Usage: python3 tests/run_tests.py <path to cplus> [name filter]

Every tests/cases/*.cplus file is compiled and checked against directives written
in its comments. A test can use these, one per line, anywhere in the file:

  // expect: <text>    the next line the program must print (in order)
  // error: <text>     compiling must fail, and the errors must contain <text>
  // warning: <text>   compiling must print a warning containing <text>; a test with no
                       warning directives fails if the compiler prints any warning
  // trap: <text>      the program must stop with a runtime error containing <text>,
                       and exit code 134
  // stderr: <text>    what the program prints to stderr must contain <text>, e.g. a
                       leak report, which otherwise fails the test
  // exit: <number>    the exit code the program must return (default 0)
  // flags: <flags>    options for the compiler, e.g. -Wno-new-delete
  // link-c: <file>     a C file under tests/c, built with the C compiler ($CC or cc)
                       and linked into the program
  // args: <arguments>  the command-line arguments the program runs with

A program that runs is built four ways, plain and as a debug build, each without and
with -O, and must behave the same every time. It runs with CPLUS_CHECK_LEAKS=1, so a
class object that is never freed, or one used after it was freed, fails the test. A
debug build runs without that setting, since it reports leaks by itself. Debug builds
need the sanitizer libraries, such as libclang-rt-18-dev on Debian or Ubuntu. Programs
also get CPLUS_TEST_VARIABLE=1, for tests that read an environment variable.

Tests run at the same time, one per processor, and print in order.

The runner also checks that every spec anchor in compiler/SpecRefs.h is a real
heading in docs/SPEC.md, so diagnostics never point at a missing section, that every
program in examples/ compiles without warnings, and that very deeply nested code is an
error instead of a crash.
"""
import concurrent.futures
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


def check_examples(cplus):
    ok = True
    for path in sorted((ROOT / "examples").glob("*.cplus")):
        run = subprocess.run([cplus, "check", str(path.relative_to(ROOT))], cwd=ROOT, capture_output=True, text=True)
        if run.returncode != 0 or "warning:" in run.stderr:
            print(f"FAIL {path.relative_to(ROOT)}\n  " + run.stderr.replace("\n", "\n  "))
            ok = False
    return ok


def check_nesting_limit(cplus, workdir):
    """Code nested far too deeply is an error, not a crash, and code near the limit works."""
    n = 50000
    too_deep = {
        "parentheses": "int main() { return " + "(" * n + "1" + ")" * n + "; }",
        "blocks": "int main() { " + "{" * n + "}" * n + " return 0; }",
        "unary operators": "int main() { bool b = " + "!" * n + "true; return 0; }",
        "assignments": "int main() { int a = 0; " + "a = " * n + "1; return a; }",
        "conditionals": "int main() { bool c = true; int x = " + "c ? 1 : " * n + "2; return x; }",
        "a long sum": "int main() { int a = 1; int b = a" + " + a" * n + "; return b; }",
        "a long member chain": "struct S { int x = 0; }; int main() { S s; int y = s" + ".x" * n + "; return y; }",
        "pointer types": "int main() { int" + "*" * n + " p = null; return 0; }",
        "vector types": "int main() { " + "vector<" * n + "int" + ">" * n + " v; return 0; }",
        "else-ifs": "int main() { if (true) return 0; " + "else if (true) return 1; " * n + "return 2; }",
    }
    close = {
        "parentheses": "int main() { println(\"{}\", " + "(" * 300 + "7" + ")" * 300 + "); return 0; }",
        "blocks": "int main() { " + "{" * 900 + "println(\"7\");" + "}" * 900 + " return 0; }",
        "else-ifs": "int main() { if (false) return 1; " + "else if (false) return 1; " * 900 + "println(\"7\"); return 0; }",
        "a long sum": "int main() { int a = 0; int b = 7" + " + a" * 9000 + "; println(\"{}\", b); return 0; }",
    }
    ok = True
    source = workdir / "nesting.cplus"
    for what, text in too_deep.items():
        source.write_text(text)
        run = subprocess.run([cplus, "check", str(source)], capture_output=True, text=True)
        if run.returncode != 1 or run.stderr.count("nested too deeply") != 1 or "no main" in run.stderr:
            print(f"FAIL nesting limit with {what}: exit code {run.returncode}\n  " + run.stderr[:500])
            ok = False
    for what, text in close.items():
        source.write_text(text)
        run = subprocess.run([cplus, "run", str(source)], capture_output=True, text=True)
        if run.returncode != 0 or run.stdout != "7\n":
            print(f"FAIL code near the nesting limit with {what}: exit code {run.returncode}\n  " + run.stderr[:500])
            ok = False
    return ok


def build_c_library(source, workdir):
    obj = workdir / (source.stem + ".o")
    cc = os.environ.get("CC", "cc")
    for command in ([cc, "-std=c11", "-O1", "-c", str(source), "-o", str(obj)],
                    ["ar", "rcs", str(workdir / f"lib{source.stem}.a"), str(obj)]):
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            return f"could not build {source.name}:\n" + result.stderr
    return None


def run_case(cplus, path, workdir):
    text = path.read_text()
    expect = re.findall(r"//[ \t]*expect:[ \t]?(.*)$", text, re.M)
    errors = re.findall(r"//[ \t]*error:[ \t]*(.*?)[ \t]*$", text, re.M)
    traps = re.findall(r"//[ \t]*trap:[ \t]*(.*?)[ \t]*$", text, re.M)
    warnings = re.findall(r"//[ \t]*warning:[ \t]*(.*?)[ \t]*$", text, re.M)
    stderr = re.findall(r"//[ \t]*stderr:[ \t]*(.*?)[ \t]*$", text, re.M)
    flags_m = re.search(r"//[ \t]*flags:[ \t]*(.*?)[ \t]*$", text, re.M)
    extra = flags_m.group(1).split() if flags_m else []
    for c_file in re.findall(r"//[ \t]*link-c:[ \t]*(.*?)[ \t]*$", text, re.M):
        problem = build_c_library(ROOT / c_file, workdir)
        if problem:
            return problem
        extra += ["-L", str(workdir), "-l" + (ROOT / c_file).stem]
    args_m = re.search(r"//[ \t]*args:[ \t]*(.*?)[ \t]*$", text, re.M)
    args = args_m.group(1).split() if args_m else []
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
    for flags in ([], ["-O"], ["--debug"], ["--debug", "-O"]):
        problem = run_program(cplus, rel, workdir / path.stem, [*extra, *flags], expect, traps, want_exit, warnings,
                              stderr, args)
        if problem:
            return (f"with {' '.join(flags)}: " if flags else "") + problem
    return None


def run_program(cplus, rel, exe, flags, expect, traps, want_exit, warnings, stderr, args):
    build = subprocess.run([cplus, "build", str(rel), "-o", str(exe), *flags], cwd=ROOT, capture_output=True,
                           text=True)
    if build.returncode != 0:
        return "failed to compile:\n" + build.stderr
    if not warnings and "warning:" in build.stderr:
        return "unexpected warning:\n" + build.stderr
    missing = [w for w in warnings if w not in build.stderr]
    if missing:
        return "warnings did not contain: " + "; ".join(missing) + "\n--- stderr ---\n" + build.stderr
    env = dict(os.environ, CPLUS_CHECK_LEAKS="1", CPLUS_TEST_VARIABLE="1")
    if "--debug" in flags:
        del env["CPLUS_CHECK_LEAKS"]
    try:
        run = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=10, env=env)
    except subprocess.TimeoutExpired:
        return "timed out after 10 seconds"
    if traps:
        if run.returncode == 0:
            return "expected a runtime error, but the program exited normally"
        # 134 is a runtime error, and 1 an AddressSanitizer report, which only a debug build has.
        if run.returncode != 134 and not ("--debug" in flags and run.returncode == 1):
            return f"a runtime error must exit with 134, but this was {run.returncode}\n--- stderr ---\n{run.stderr}"
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
    if not name_filter:
        ok = check_examples(cplus) and ok
    passed = failed = 0
    paths = [p for p in sorted(CASES.glob("*.cplus")) if name_filter in p.name]
    with tempfile.TemporaryDirectory(prefix="cplus-tests-") as tmp:
        if not name_filter:
            ok = check_nesting_limit(cplus, pathlib.Path(tmp)) and ok

        def run_in_own_folder(path):
            workdir = pathlib.Path(tmp) / path.stem
            workdir.mkdir()
            return run_case(cplus, path, workdir)

        with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
            for path, problem in zip(paths, pool.map(run_in_own_folder, paths)):
                if problem:
                    failed += 1
                    print(f"FAIL {path.relative_to(ROOT)}\n  " + problem.replace("\n", "\n  "))
                else:
                    passed += 1
    print(f"{passed} passed, {failed} failed")
    return 0 if ok and failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
