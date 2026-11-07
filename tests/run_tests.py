#!/usr/bin/env python3
"""Runs the C+ language tests.

Usage: python3 tests/run_tests.py <path to cplus> [name filter]

Every tests/cases/*.cplus and examples/features/*.cplus file is compiled and checked
against directives written in its comments. A test can use these, one per line, anywhere
in the file:

  // expect: <text>    the next line the program must print (in order)
  // error: <text>     compiling must fail, and the errors must contain <text>
  // not-error: <text> the errors must not contain <text>, for a test with error directives
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
  // race: <text>       the program has a race on purpose: it is only built with --race, and
                       must stop, with this in its report
  // no-race: <why>     the program has `spawn`, but is not built with --race

A program with `spawn` is also built with --race, without and with -O, which must find
nothing wrong, as a wrong answer from a race can take a very long time to show up. This
is left out, with a note, if the C compiler has no ThreadSanitizer.

A test that has `import http;` gets a local server, tests/http_server.py, whose addresses
come as environment variables, so no test needs the network. These tests are left out,
with a note, if the compiler was built without libcurl.

A program that runs is built four ways, plain and as a debug build, each without and
with -O, and must behave the same every time. It runs with CPLUS_CHECK_LEAKS=1, so a
class object that is never freed, or one used after it was freed, fails the test. A
debug build runs without that setting, since it reports leaks by itself. Debug builds
need the sanitizer libraries, such as libclang-rt-18-dev on Debian or Ubuntu. Programs
also get CPLUS_TEST_VARIABLE=1, for tests that read an environment variable.

Tests run at the same time, one per processor, and print in order.

A test of errors must exit with code 1, as anything else is a crash, and every error
must come with a help and a spec line.

The runner also checks that every spec anchor in compiler/SpecRefs.h is a real
heading in docs/SPEC.md, so diagnostics never point at a missing section, that each section
of the spec has an example in examples/features, that `cplus explain` finds every section, that
every program in examples/ compiles without warnings, that very deeply nested code is an
error instead of a crash, that no way of copying a unique reference compiles, that a
crash whose report cannot flush the program's output still says what happened, that lines
printed by threads stay together, that a program links what it uses and no more, that mistakes in
the command line are reported as other errors are, and that the code in the spec compiles. The check of
copies builds each case by itself, since one test file with many errors would pass with one
of them missing.
"""
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parent.parent
CASES = ROOT / "tests" / "cases"
FEATURES = ROOT / "examples" / "features"

# Sections with nothing to run: a heading over others, or what is about the tools and not the language.
NO_EXAMPLE = {"c-language-specification", "lexical-structure", "types", "declarations", "statements", "expressions", "source-files",
              "no-preprocessor", "keywords", "nesting-limits", "debug-builds", "the-cplus-command", "grammar", "implementation-status"}


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


def spec_sections():
    in_code = False
    sections = []
    for line in (ROOT / "docs" / "SPEC.md").read_text().splitlines():
        if line.startswith("```"):
            in_code = not in_code
        elif not in_code and re.match(r"#+ ", line):
            sections.append(slugify(line.split(" ", 1)[1]))
    return sections


def check_example_for_each_section():
    ok = True
    sections = spec_sections()
    for section in sections:
        if section not in NO_EXAMPLE and not (FEATURES / f"{section}.cplus").exists():
            print(f"FAIL the spec section '{section}' has no example in examples/features")
            ok = False
    for path in FEATURES.glob("*.cplus"):
        if path.stem not in sections:
            print(f"FAIL {path.relative_to(ROOT)} is not named after a section of the spec")
            ok = False
    return ok


def check_explain(cplus):
    """`cplus explain` has every section, and the words people use lead to them."""
    def explain(*topic):
        return subprocess.run([cplus, "explain", *topic], capture_output=True, text=True)
    ok = True
    for section in spec_sections():
        if section == "c-language-specification":
            continue
        run = explain(section)
        if run.returncode != 0 or not run.stdout.startswith("#"):
            print(f"FAIL cplus explain {section}: exit code {run.returncode}\n  " + run.stderr[:300])
            ok = False
    checks = [
        (explain("--check").returncode == 0, "every word and example leads to a section"),
        (explain("hashmap").stdout.startswith("### Hash maps"), "a word leads to its section"),
        ("examples/features/hash-maps.cplus" in explain("hash-maps").stdout, "a section shows its example"),
        ("cplus explain integer-types" in explain("types").stdout, "a section lists what is under it"),
        ("generics" in explain().stdout and explain().returncode == 0, "no topic lists them all"),
        (explain("cheatsheet").stdout == (ROOT / "docs" / "CHEATSHEET.md").read_text(), "the cheat sheet is there, byte for byte"),
        (all(path.read_text() in explain(path.stem).stdout for path in FEATURES.glob("*.cplus")), "each example is there, byte for byte"),
        (explain("no-such-topic-at-all").returncode == 1, "an unknown topic is an error"),
        ("cplus explain" in explain("hashmapp").stderr, "an unknown topic says what it may have meant"),
    ]
    for passed, what in checks:
        if not passed:
            print(f"FAIL cplus explain: {what}")
            ok = False
    return ok


# Blocks of the spec that are not meant to compile as they are: each shows an error, or is a sketch. What else is not there to
# compile only lacks what the text around it has defined, which is all right. Anything new has to be looked at.
SPEC_SKETCHES = (
    "long big = 5;", "User? find(vector<User> users", "class Box : public Named", "interface Speaker {", "json::Value doc = try",
    "http::WebSocket ws = try", "int* a, b;", "constexpr int square(int n)", "int x;", "import json;          // a standard module",
    "if (count) ...", "template<Comparable T>", "template<typename T>",
)


def spec_code_problems(cplus, text, workdir):
    declaration = re.compile(r"^\s*(import\b|struct\b|class\b|interface\b|template\b|extern\b|constexpr\b|\[\[allow\(new_delete\)\]\];|const\s+\w+\s+[A-Z_]+\s*=)")
    function = re.compile(r"^\s*(?:[\w:<>,\s\*\?]+?)\s+\**\w+\s*\([^;]*\)\s*(const\s*)?(throws\s*)?\{")
    needs_context = re.compile(r"is not declared|unknown type|there is no function named|there is no class named|has no field named|has no method named")
    source = workdir / "spec_block.cplus"
    problems = []
    for block in re.finditer(r"^```cpp\n(.*?)^```", text, re.S | re.M):
        code = block.group(1)
        if code.startswith(SPEC_SKETCHES):
            continue
        if not re.search(r"\bint main\b", code):
            declarations, statements = [], []
            lines = code.splitlines()
            i = 0
            while i < len(lines):
                if declaration.match(lines[i]) or function.match(lines[i]):
                    depth = 0
                    while i < len(lines):
                        bare = re.sub(r'"(\\.|[^"\\])*"|//.*$', "", lines[i])
                        depth += bare.count("{") - bare.count("}")
                        declarations.append(lines[i])
                        i += 1
                        if depth <= 0 and declarations[-1].rstrip().endswith((";", "}")) or depth <= 0 and re.search(r";\s*//", declarations[-1]):
                            break
                else:
                    statements.append(lines[i])
                    i += 1
            code = "\n".join(declarations) + "\nint main() throws {\n" + "\n".join(statements) + "\nreturn 0;\n}\n"
        source.write_text(code)
        run = subprocess.run([cplus, "check", str(source)], capture_output=True, text=True)
        if run.returncode not in (0, 1):
            problems.append(f"the compiler crashed on this block:\n{code}")
        real = [line for line in run.stderr.splitlines()
                if line.startswith("error:") and "could not compile" not in line and not needs_context.search(line)]
        if real:
            problems.append(f"line {text.count(chr(10), 0, block.start()) + 2}: {real[0]}")
    return problems


def check_spec_code(cplus, workdir):
    """The code in the documents goes on compiling, so that they cannot drift away from the compiler without a test failing."""
    ok = True
    for name in ("SPEC.md", "CHEATSHEET.md"):
        for problem in spec_code_problems(cplus, (ROOT / "docs" / name).read_text(), workdir):
            print(f"FAIL docs/{name}: {problem}")
            ok = False
    return ok


def errors_without_help(stderr):
    """Every error says how to fix it, and where the rule is."""
    problems = []
    blocks = re.split(r"^(?=(?:error|warning|note): )", stderr, flags=re.M)
    for block in blocks:
        if not block.startswith("error: ") or block.startswith("error: could not compile"):
            continue
        first = block.splitlines()[0]
        if "= help:" not in block:
            problems.append("no help: " + first)
        if "= spec:" not in block:
            problems.append("no spec: " + first)
    return problems


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
        if (run.returncode != 1 or run.stderr.count("nested too deeply") != 1 or "no main" in run.stderr
                or "docs/SPEC.md#nesting-limits" not in run.stderr):
            print(f"FAIL nesting limit with {what}: exit code {run.returncode}\n  " + run.stderr[:500])
            ok = False
    for what, text in close.items():
        source.write_text(text)
        run = subprocess.run([cplus, "run", str(source)], capture_output=True, text=True)
        if run.returncode != 0 or run.stdout != "7\n":
            print(f"FAIL code near the nesting limit with {what}: exit code {run.returncode}\n  " + run.stderr[:500])
            ok = False
    return ok


def check_unique_copies(cplus, workdir):
    """Each of these would give an object a second owner, so each must be an error by itself."""
    before = """
class Node { public: int n = 1; };
class Dog : public Node {};
struct Holds { unique<Node> u; };
struct Family { vector<Family> kids; unique<Node>? u; };
class Owner { public: unique<Node> held; Owner(unique<Node> h) : held(move(h)) {} };
unique<Node> make() { return unique<Node>(); }
unique<Node>? none() { return null; }
void take(unique<Node> u) {}
template<typename T> T same(T x) { return x; }
template<typename T> void noReturn(T x) {}
unique<Node> mayFail(unique<Node> u) throws { return u; }
int main() {
    unique<Node> a = unique<Node>();
    unique<Node> a2 = unique<Node>();
    unique<Node>? o = null;
    vector<unique<Node>> v;
    vector<Holds> hs;
    array<unique<Node>, 1> arr = {unique<Node>()};
    Holds h = {unique<Node>()};
    Holds? ho = null;
    Owner own = Owner(unique<Node>());
    bool yes = true;
"""
    copies = [
        "unique<Node> b = a;", "auto b = a;", "auto b = (a);", "unique<Node> b = yes ? a : make();",
        "unique<Node> b = yes ? make() : a;", "unique<Node>? b = a;", "Node b = a;", "take(a);", "v.push_back(a);",
        "vector<unique<Node>> w = {a};", "array<unique<Node>, 1> w = {a};", "Holds x = {a};", "Holds x = Holds{a};",
        "Holds x = Holds(a);", "auto p = new unique<Node>(a);", "unique<Node>* p = &a; unique<Node> b = *p;", "o = a;",
        "a2 = a;", "Owner x = Owner(a);", "auto t = static_cast<unique<Node>>(a);", "unique<Node> b = unique<Node>(a);",
        "auto b = h.u;", "auto b = v[0];", "auto b = own.held;", "auto b = hs[0];", "auto b = hs[0].u;", "auto b = ho?.u;",
        "auto w = v;", "vector<unique<Node>> w = vector<unique<Node>>(v);", "auto b = arr;",
        "auto w = array<unique<Node>, 1>(arr);", "auto x = h;", "Holds x = Holds(h);", "same(a);", "noReturn(a);",
        "noReturn(h);", "noReturn(v);", "for (unique<Node> x : v) {}", "for (auto x : arr) {}", "if (auto x = o) {}",
        "if (unique<Node> x = o) {}", "while (auto x = o) {}", "auto b = o;", "auto b = o ?? make();",
        "int y = (o ?? a).n;", "auto b = o.value();", "auto b = o.value_or(make());", "auto b = v.front();",
        "auto b = v.back();", "auto b = arr.front();", "try { auto b = mayFail(a); } catch (Error e) {}",
        "unique<Node> b = (a2 = a);", "unique<Node> b = (a2 = make());", "Node n = yes ? a : a2;",
        "unique<Node>* p = &a; take(*p);", "weak<Node> w = a;", "unique<Dog> d = unique<Dog>(); unique<Node> up = d;",
        "unique<Node> b = none() ?? a;", "auto b = none() ?? a;", "Family f; Family g = f;", "Family f; auto k = f.kids;",
        "hashmap<int, unique<Node>> m; auto c = m;", "hashmap<int, unique<Node>> m; auto b = m[1];",
        "hashmap<int, unique<Node>> m; auto b = m.at(1);", "hashmap<int, unique<Node>> m; unique<Node> b = m[1];",
        "hashmap<int, unique<Node>> m; take(m[1]);", "hashmap<int, unique<Node>> m; take(m.at(1));",
        "hashmap<int, unique<Node>> m; m[1] = a;", "hashmap<int, unique<Node>> m; m.insert({1, a});",
        "hashmap<int, unique<Node>> m; m[2] = m[1];", "hashmap<int, unique<Node>> m; auto x = m.get(1);",
        "hashmap<int, unique<Node>> m; auto x = m.items();", "hashmap<int, unique<Node>> m; auto x = m.values();",
        "hashmap<int, unique<Node>> m; for (auto e : m) {}", "hashmap<int, unique<Node>> m; noReturn(m);",
        "hashmap<int, unique<Node>> m; hashmap<int, unique<Node>> c = hashmap<int, unique<Node>>(m);",
    ]
    ok = True
    source = workdir / "copies.cplus"
    for copy in copies:
        source.write_text(before + "    " + copy + "\n    return 0;\n}\n")
        run = subprocess.run([cplus, "check", str(source)], capture_output=True, text=True)
        if run.returncode != 1:
            print(f"FAIL this copies a unique reference, but it compiled: {copy}\n  " + run.stderr[:300])
            ok = False
    return ok


def check_crash_in_output(cplus, workdir):
    """Flushing the output can crash while reporting a crash. The report must still come out."""
    if not sys.platform.startswith("linux"):
        return True  # tests/c/crash.c builds its output with fopencookie
    problem = build_c_library(ROOT / "tests" / "c" / "crash.c", workdir)
    if problem:
        print("FAIL a crash while flushing: " + problem)
        return False
    ok = True
    source = workdir / "crash_in_output.cplus"
    exe = workdir / "crash_in_output"
    for what, by_abort in (("a crash", 0), ("an abort", 1)):
        source.write_text('extern "C" void break_output(int by_abort);\nextern "C" void crash();\n'
                          f'int main() {{ break_output({by_abort}); println("lost"); crash(); return 0; }}\n')
        build = subprocess.run([cplus, "build", str(source), "-o", str(exe), "-L", str(workdir), "-lcrash"],
                               capture_output=True, text=True)
        if build.returncode != 0:
            print(f"FAIL {what} while flushing did not compile:\n  " + build.stderr[:500])
            ok = False
            continue
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        if run.returncode != 134 or "the program used memory it does not own" not in run.stderr:
            print(f"FAIL {what} while flushing: exit code {run.returncode}\n  " + run.stderr[:500])
            ok = False
    return ok


def check_crash_paths(cplus, workdir):
    """A fatal error says so once, however many threads stop at once, and whatever another thread is stuck in."""
    if not sys.platform.startswith("linux"):
        return True  # tests/c/crash.c builds its output with fopencookie
    problem = build_c_library(ROOT / "tests" / "c" / "crash.c", workdir)
    if problem:
        print("FAIL crash paths: " + problem)
        return False
    ok = True
    source = workdir / "crash_paths.cplus"
    exe = workdir / "crash_paths"
    builds = [[]] + ([["--race"], ["--race", "-O"]] if HAS_RACE[0] else [])
    def build_and_run(what, text, flags, times, expect_code, expect_text):
        nonlocal ok
        source.write_text(text)
        build = subprocess.run([cplus, "build", str(source), "-o", str(exe), "-L", str(workdir), "-lcrash"] + flags, capture_output=True, text=True)
        if build.returncode != 0:
            print(f"FAIL {what} did not compile:\n  " + build.stderr[:500])
            ok = False
            return
        for _ in range(times):
            try:
                run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, errors="replace")
            except subprocess.TimeoutExpired:
                print(f"FAIL {what}, built with {flags}: it never ended")
                ok = False
                return
            reports = len(re.findall(r"^(?:fatal|uncaught) error", run.stderr, re.M))
            if run.returncode != expect_code or expect_text not in run.stderr or reports != 1:
                print(f"FAIL {what}, built with {flags}: exit code {run.returncode}, {reports} reports\n  " + run.stderr[:600])
                ok = False
                return
    stopping = {
        "the same check fails": ("n + 2147483647", "does not fit in 'int'"),
        "a crash": ("crashes()", "the program used memory it does not own"),
    }
    # Threads held back, and let go together.
    for what, (expression, message) in stopping.items():
        text = ('extern "C" void crash();\nextern "C" int usleep(int microseconds);\n'
                'int crashes() { crash(); return 0; }\n'
                f'int stops(Mutex gate, int n) {{ gate.lock(); gate.unlock(); return {expression}; }}\n'
                'int main() {\n    Mutex gate = Mutex();\n    gate.lock();\n    vector<Task<int>> tasks;\n'
                '    for (int i = 1; i <= 16; i++) tasks.push_back(spawn stops(gate, i));\n'
                '    usleep(20000);\n    gate.unlock();\n    for (auto t : tasks) println("{}", t.wait());\n    return 0;\n}\n')
        for flags in builds:
            build_and_run(f"threads at once when {what}", text, flags, 25, 134, message)
    # A thread stuck in the output, holding its lock.
    endings = {
        "a crash": ("crash();", 134, "the program used memory it does not own"),
        "a failed check": ("println(\"{}\", big() + 1);", 134, "does not fit in 'int'"),
        "a stop": ("Mutex m = Mutex();\n    m.lock();\n    m.lock();", 134, "it would wait for itself"),
    }
    for what, (statement, code, message) in endings.items():
        text = ('extern "C" void stick_output();\nextern "C" void crash();\nextern "C" int usleep(int microseconds);\n'
                'int big() { return 2147483647; }\nvoid prints() { println("{}", string(1000000, \'x\')); }\n'
                'int main() throws {\n    stick_output();\n    spawn prints();\n    usleep(100000);\n'
                f'    {statement}\n    return 0;\n}}\n')
        for flags in builds:
            build_and_run(f"{what} while a thread is stuck in the output", text, flags, 2, code, message)
    # An error out of main is reported, and then the program waits for its threads, as it does when main returns.
    text = ('extern "C" void stick_output();\nextern "C" int usleep(int microseconds);\n'
            'void prints() { println("{}", string(1000000, \'x\')); }\n'
            'int main() throws {\n    stick_output();\n    spawn prints();\n    usleep(100000);\n    throw Error("from main");\n}\n')
    for flags in builds:
        source.write_text(text)
        build = subprocess.run([cplus, "build", str(source), "-o", str(exe), "-L", str(workdir), "-lcrash"] + flags, capture_output=True, text=True)
        if build.returncode != 0:
            print("FAIL an error from main while a thread is stuck did not compile:\n  " + build.stderr[:500])
            ok = False
            continue
        program = subprocess.Popen([str(exe)], stderr=subprocess.PIPE, text=True, errors="replace")
        watchdog = threading.Timer(30, program.kill)
        watchdog.start()
        line = program.stderr.readline()
        watchdog.cancel()
        program.kill()
        program.communicate()
        if not line.startswith("uncaught error") or "from main" not in line:
            print(f"FAIL an error from main while a thread is stuck in the output, built with {flags}: [{line[:200]}]")
            ok = False
    return ok


def check_print_lines(cplus, workdir):
    """What one println prints stays together, whichever thread gets there when. The lines can come in any order."""
    source = workdir / "print_lines.cplus"
    exe = workdir / "print_lines"
    source.write_text("""
void printMany(int who) {
    for (int i = 0; i < 2000; i++) println("thread {} line {} of {} {}", who, i, 2000, "end");
}
int main() {
    vector<Task<void>> tasks;
    for (int i = 0; i < 8; i++) tasks.push_back(spawn printMany(i));
    for (auto t : tasks) t.wait();
    return 0;
}
""")
    build = subprocess.run([cplus, "build", str(source), "-o", str(exe), "-O"], capture_output=True, text=True)
    if build.returncode != 0:
        print("FAIL lines printed by threads did not compile:\n  " + build.stderr[:500])
        return False
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    lines = run.stdout.splitlines()
    wrong = [line for line in lines if not re.fullmatch(r"thread \d line \d+ of 2000 end", line)]
    if run.returncode != 0 or len(lines) != 16000 or wrong:
        print(f"FAIL lines printed by threads: exit code {run.returncode}, {len(lines)} lines, {len(wrong)} broken up, such as {wrong[:2]}")
        return False
    return True


def check_command_line(cplus, workdir):
    """What is wrong with the command, and not the program, is said as any other error is."""
    good = workdir / "good.cplus"
    good.write_text("int main() { return 0; }\n")
    other = workdir / "other.cplus"
    other.write_text("int main() { return 0; }\n")
    text = workdir / "notes.txt"
    text.write_text("notes\n")
    missing_dir = workdir / "no" / "such" / "folder"
    g = str(good)
    cases = [
        ([], 2, "no command given"), (["frobnicate"], 2, "unknown command 'frobnicate'"), (["check"], 2, "no source file given"),
        (["check", g, str(other)], 2, "only one source file can be given"), (["check", g, "--nope"], 2, "unknown option '--nope'"),
        (["check", g, "-o", "x"], 2, "-o is only used with `cplus build`"), (["build", g, "-o"], 2, "-o needs an output path"),
        (["run", g, "--emit-llvm"], 2, "--emit-llvm is only used with `cplus build`"), (["build", g, "--", "a"], 2, "'--' is only used with `cplus run`"),
        (["build", g, "-l"], 2, "-l needs a library name"), (["build", g, "-L"], 2, "-L needs a directory"),
        (["build", g, "-g"], 2, "'-g' is not an option of cplus"), (["build", g, "-fsanitize=thread"], 2, "is not an option of cplus"),
        (["build", g, "--debug", "--race"], 2, "--debug and --race cannot be used together"), (["explain", "a", "b"], 2, "explain takes one topic"),
        (["check", str(text)], 2, "is not a C+ source file"), (["check", str(workdir / "gone.cplus")], 1, "cannot read"),
        (["build", g, "-o", str(missing_dir / "out")], 1, "cannot write"), (["build", g, "-o", str(workdir / "out"), "-lno_such_library_here"], 1, "linking failed"),
    ]
    ok = True
    for args, code, message in cases:
        run = subprocess.run([cplus, *args], capture_output=True, text=True)
        lacking = errors_without_help(run.stderr)
        if run.returncode != code or message not in run.stderr or lacking:
            print(f"FAIL cplus {' '.join(args)}: exit code {run.returncode}, expected {code}, and '{message}'. {lacking}\n  " + run.stderr[:300])
            ok = False
    run = subprocess.run([cplus, "run", g], capture_output=True, text=True, env=dict(os.environ, TMPDIR=str(missing_dir)))
    if run.returncode != 1 or "cannot find a folder for temporary files" not in run.stderr or errors_without_help(run.stderr):
        print(f"FAIL cplus run with no folder for temporary files: exit code {run.returncode}\n  " + run.stderr[:300])
        ok = False
    return ok


def check_what_is_linked(cplus, workdir):
    """What a program uses of the runtime decides what it links and builds, and nothing else does."""
    source = workdir / "linked.cplus"
    exe = workdir / "linked"
    ok = True
    def link_line(text):
        source.write_text(text)
        run = subprocess.run([cplus, "build", str(source), "-o", str(exe)], capture_output=True, text=True,
                             env=dict(os.environ, CPLUS_SHOW_LINK="1"))
        return run.stderr
    threads = {"a Mutex": "int main() { Mutex m = Mutex(); m.lock(); m.unlock(); return 0; }\n",
               "a LockGuard": "int main() { Mutex m = Mutex(); LockGuard g = LockGuard(m); return 0; }\n",
               "spawn": "void f() {}\nint main() { spawn f(); return 0; }\n"}
    for what, text in threads.items():
        if "-lpthread" not in link_line(text):
            print(f"FAIL a program with {what} is written on threads, but does not link them")
            ok = False
    if "-lpthread" in link_line("int main() { return 0; }\n"):
        print("FAIL a program with no threads links them")
        ok = False
    # Reflection only looks at types, so what makes values for JSON is not there for it.
    source.write_text("import reflect;\nclass N { public: int x = 0; };\nstruct S { hashmap<string, N> m; };\nint main() { println(\"{}\", reflect::type<S>().name()); return 0; }\n")
    run = subprocess.run([cplus, "build", str(source), "--emit-llvm"], capture_output=True, text=True)
    made = [name for name in ("#make", "#put") if name in run.stdout]
    if run.returncode != 0 or made:
        print(f"FAIL a program that only imports reflect has what JSON needs: {made}\n  " + run.stderr[:300])
        ok = False
    # json::of makes its value directly, as text between the two would cost twice as much.
    source.write_text("import json;\nint main() { println(\"{}\", json::of(1).whole()); return 0; }\n")
    run = subprocess.run([cplus, "build", str(source), "--emit-llvm"], capture_output=True, text=True)
    body = re.search(r'define [^\n]*"cplus\.json::of<int>"[^\n]*\n(.*?)^}', run.stdout, re.S | re.M)
    calls = re.findall(r'call [^@\n]*@"?([^"(\s]+)', body.group(1)) if body else []
    through_text = [name for name in calls if "json_write" in name or "json_read" in name or "stringify" in name or "parse" in name]
    if run.returncode != 0 or through_text or "cplus_json_build" not in calls:
        print(f"FAIL json::of goes through text: it calls {calls}\n  " + run.stderr[:300])
        ok = False
    return ok


def check_source_rules(root=ROOT):
    """What the review of the code found twice over, and which no behavior would show: one of a thing, in one place."""
    def text(name):
        return (root / name).read_text()
    runtime = "".join(path.read_text() for path in sorted((root / "runtime").glob("*.c")))
    codegen = text("compiler/Codegen.cpp")
    parser = text("compiler/Parser.cpp")
    sema = text("compiler/Sema.cpp")
    builtins = text("compiler/Builtins.h")
    broken = []
    def rule(holds, what):
        if not holds:
            broken.append(what)
    # Running out of memory says one thing, from one place.
    rule("static void* memory(" not in runtime, "runtime: memory() written again instead of cplus_malloc")
    rule('cplus_stop("out of memory"' not in runtime, "runtime: its own out of memory message instead of cplus_malloc")
    # How a call is made, and how a struct's fields are retained and released, are each written once.
    rule(len(re.findall(r"\w+\.insert\(\w+\.begin\(\), result\)", codegen)) == 1, "Codegen: spawn writes its own way of making a call")
    rule(codegen.count("for (auto [f, which] : {std::pair{info.retain") == 1, "Codegen: a second copy of what a struct's fields do on retain and release")
    # What a lookup in a hashmap passes around is a borrowed key.
    rule("__find(K key" not in builtins, "Builtins: __find copies its key")
    # The list of modules is in one place.
    rule('imp.path != "json"' not in sema, "Sema: its own list of the modules")
    rule("kBuiltinModules[]" not in parser and "kBuiltinModules[]" in text("compiler/AST.h"), "Parser: its own list of the modules")
    # A declaration in Builtins.h and its definition in the runtime agree.
    rule("int32_t cplus_json_read" not in runtime, "json.c: cplus_json_read is not the bool that Builtins.h says")
    # A URL is cleaned once, where an error is made.
    rule("http::WebSocket(socket, http::__urlForError" not in builtins, "Builtins: a URL cleaned twice")
    # What comes with the language says what it is, and the compiler finds its fields by name.
    rule('sd.templateName == "Task"' not in codegen, "Codegen: knows a Task by its name")
    rule("sd.fields[0].type->elem" not in codegen and "self, 0, \"entries\"" not in codegen, "Codegen: knows the fields of a hashmap by their places")
    rule("fields[0].type->elem" not in sema, "Sema: knows the fields of a hashmap by their places")
    # What is not code that is true any more.
    rule(text("compiler/Parser.h").startswith("#pragma once"), "Parser.h: something before #pragma once")
    rule("What `throw` takes" not in parser, "Parser: a comment for something that moved")
    for what in broken:
        print(f"FAIL {what}")
    return not broken


def build_c_library(source, workdir):
    obj = workdir / (source.stem + ".o")
    cc = os.environ.get("CC", "cc")
    for command in ([cc, "-std=c11", "-O1", "-c", str(source), "-o", str(obj)],
                    ["ar", "rcs", str(workdir / f"lib{source.stem}.a"), str(obj)]):
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            return f"could not build {source.name}:\n" + result.stderr
    return None


def imports_http(path):
    return re.search(r"^import http;", path.read_text(), re.M) is not None


def compiler_has_http(cplus, workdir):
    source = workdir / "has_http.cplus"
    source.write_text("import http;\nint main() { return 0; }\n")
    run = subprocess.run([cplus, "check", str(source)], capture_output=True, text=True)
    return "built without libcurl" not in run.stderr


def compiler_has_race(cplus, workdir):
    source = workdir / "has_race.cplus"
    source.write_text("int main() { return 0; }\n")
    run = subprocess.run([cplus, "build", str(source), "-o", str(workdir / "has_race"), "--race"], capture_output=True, text=True)
    return run.returncode == 0


HAS_RACE = [True]  # filled in by main


def start_http_server():
    """It ends when this program does, when its input ends."""
    server = subprocess.Popen([sys.executable, str(ROOT / "tests" / "http_server.py")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    os.environ.update(json.loads(server.stdout.readline()))
    os.environ["no_proxy"] = "*"  # whatever proxy this machine has, these connections do not go to it
    return server


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
        is_usage = build.returncode == 2 and "usage:" in build.stderr
        if build.returncode != 1 and not is_usage:
            return f"a compile error must exit with 1, but this was {build.returncode}, which is a crash\n--- stderr ---\n" + build.stderr
        missing = [e for e in errors if e not in build.stderr]
        if missing:
            return "compile errors did not contain: " + "; ".join(missing) + "\n--- stderr ---\n" + build.stderr
        wrong = [e for e in re.findall(r"//[ \t]*not-error:[ \t]*(.*?)[ \t]*$", text, re.M) if e in build.stderr]
        if wrong:
            return "compile errors should not contain: " + "; ".join(wrong) + "\n--- stderr ---\n" + build.stderr
        lacking = errors_without_help(build.stderr)
        if lacking:
            return "; ".join(lacking)
        return None
    races = re.findall(r"//[ \t]*race:[ \t]*(.*?)[ \t]*$", text, re.M)
    builds = [[], ["-O"], ["--debug"], ["--debug", "-O"]]
    if races:
        if not HAS_RACE[0]:
            return None
        builds = [["--race"], ["--race", "-O"]]
        traps = races
    elif HAS_RACE[0] and re.search(r"^[^/\n]*\bspawn\b", text, re.M) and "// no-race:" not in text:
        builds += [["--race"], ["--race", "-O"]]
    for flags in builds:
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
    # A build with --race runs many times slower.
    limit = 60 if "--race" in flags else 10
    try:
        run = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=limit, env=env)
    except subprocess.TimeoutExpired:
        return f"timed out after {limit} seconds"
    if traps:
        if run.returncode == 0:
            return "expected a runtime error, but the program exited normally"
        # 134 is a runtime error, and 1 an AddressSanitizer report, which only a debug build has. A --race build's is 134 too.
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
        ok = check_example_for_each_section() and ok
        ok = check_explain(cplus) and ok
    passed = failed = 0
    paths = [p for p in [*sorted(CASES.glob("*.cplus")), *sorted(FEATURES.glob("*.cplus"))] if name_filter in p.name]
    server = None
    with tempfile.TemporaryDirectory(prefix="cplus-tests-") as tmp:
        HAS_RACE[0] = compiler_has_race(cplus, pathlib.Path(tmp))
        if not HAS_RACE[0]:
            print("note: leaving out --race, since the C compiler has no ThreadSanitizer")
        if any(imports_http(p) for p in paths):
            if compiler_has_http(cplus, pathlib.Path(tmp)):
                server = start_http_server()
                if "CPLUS_TEST_WS6" not in os.environ:
                    print("note: leaving out the tests of IPv6, since this machine has no address for it")
                    paths = [p for p in paths if "ipv6" not in p.name]
            else:
                print("note: leaving out the tests of http, since this compiler was built without libcurl")
                paths = [p for p in paths if not imports_http(p)]
        if not name_filter:
            ok = check_nesting_limit(cplus, pathlib.Path(tmp)) and ok
            ok = check_unique_copies(cplus, pathlib.Path(tmp)) and ok
            ok = check_crash_in_output(cplus, pathlib.Path(tmp)) and ok
            ok = check_crash_paths(cplus, pathlib.Path(tmp)) and ok
            ok = check_print_lines(cplus, pathlib.Path(tmp)) and ok
            ok = check_what_is_linked(cplus, pathlib.Path(tmp)) and ok
            ok = check_source_rules() and ok
            ok = check_command_line(cplus, pathlib.Path(tmp)) and ok
            ok = check_spec_code(cplus, pathlib.Path(tmp)) and ok

        def run_in_own_folder(path):
            workdir = pathlib.Path(tmp) / f"{path.parent.name}-{path.stem}"
            workdir.mkdir()
            return run_case(cplus, path, workdir)

        with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
            for path, problem in zip(paths, pool.map(run_in_own_folder, paths)):
                if problem:
                    failed += 1
                    print(f"FAIL {path.relative_to(ROOT)}\n  " + problem.replace("\n", "\n  "))
                else:
                    passed += 1
    if server:
        server.stdin.close()
        server.wait()
    print(f"{passed} passed, {failed} failed")
    return 0 if ok and failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
