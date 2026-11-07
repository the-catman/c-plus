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
import resource
import signal
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


HELP_VERBS = ("write|use|remove|add|declare|give|make|call|put|move|rename|pass|mark|leave|drop|replace|store|split|set|define|install|check|convert|turn|unwrap|read|reach"
              "|compare|assign|name|say|delete|keep|take|return|pick|build|derive|list|cast|choose|free|start|end|go|loop|copy|create|hold|print|override|run|stop|handle"
              "|change|type|allocate|look|subtract|ask|let|do|first")
HELP_SAYS_WHAT_TO_DO = re.compile(r"(?:^|[.;:?!]\s+|\bor\s+|\band\s+|,\s+)(?:" + HELP_VERBS + r")\b", re.I)


def errors_without_help(stderr):
    """Every error says how to fix it, and where the rule is. Whether advice is good takes reading, but this much a machine can tell:
    it gives code to write, a name it may have meant, or a verb that says what to do, rather than only saying how things are."""
    problems = []
    blocks = re.split(r"^(?=(?:error|warning|note): )", stderr, flags=re.M)
    for block in blocks:
        if not block.startswith("error: ") or block.startswith("error: could not compile"):
            continue
        first = block.splitlines()[0]
        if "= help:" not in block or "the part of the spec named below explains the rule" in block:
            problems.append("no help: " + first)
        if "= spec:" not in block:
            problems.append("no spec: " + first)
        for advice in re.findall(r"= help: (.*)", block):
            if "`" not in advice and "did you mean" not in advice and not HELP_SAYS_WHAT_TO_DO.search(advice):
                problems.append(f"help that gives nothing to do: {first}: {advice}")
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


def check_leak_report(cplus, workdir):
    """A debug build says what each object that was never freed is, and where it was made."""
    source = workdir / "leaks.cplus"
    source.write_text("""class Node {
public:
    Node? next;
};
class Other : public Node {
public:
    int x = 0;
};
void makes() {
    Node a = Node();
    a.next = a;
}
int main() {
    makes();
    Other o = Other();
    o.next = o;
    return 0;
}
""")
    exe = workdir / "leaks"
    build = subprocess.run([cplus, "build", str(source), "-o", str(exe), "--debug"], capture_output=True, text=True)
    if build.returncode != 0:
        if "needs AddressSanitizer" in build.stderr:
            return True
        print("FAIL a leak report did not compile:\n  " + build.stderr[:500])
        return False
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    want = ["cplus: 2 objects were never freed", "Node, made at " + str(source) + ":10:", ", in makes", "Other, made at " + str(source) + ":15:", ", in main", "weak<T>"]
    missing = [text for text in want if text not in run.stderr]
    if missing:
        print(f"FAIL a leak report does not say {missing}:\n  " + run.stderr[:800])
        return False
    return True


M = "int main() {\n%s\nreturn 0;\n}\n"

# What the spec's status table and the cheat sheet's table of planned things name, each as a program that asks for it.
PLANNED_FEATURES = [
    ("more than one file", 'import other;\n' + M % ""),
    ("your own modules", 'import net.server;\n' + M % ""),
    ("constexpr methods", 'struct P { int x = 0; constexpr int f() { return 1; } };\n' + M % ""),
    ("constexpr structs", 'constexpr struct P { int x = 0; };\n' + M % ""),
    ("constexpr of a struct", 'struct P { int x = 0; };\nconstexpr P ORIGIN = {1};\n' + M % ""),
    ("a generic constexpr function in an array's size", 'template<typename T> constexpr int size(T x) { return 2; }\n' + M % "array<int, size<int>(1)> a;"),
    ("variables outside functions", 'int count = 0;\n' + M % ""),
    ("constants of other types", 'struct P { int x = 0; };\nconst P ORIGIN = {1};\n' + M % ""),
    ("enums", 'enum Color { Red, Blue };\n' + M % ""),
    ("enum classes", 'enum class Color { Red, Blue };\n' + M % ""),
    ("switch", M % "int n = 1; switch (n) { case 1: break; }"),
    ("lambdas", M % "auto f = [](int x) { return x; };"),
    ("default arguments", 'int f(int a = 1) { return a; }\n' + M % ""),
    ("auto parameters", 'int f(auto a) { return 1; }\n' + M % ""),
    ("auto return types", 'auto f() { return 1; }\n' + M % ""),
    ("const fields", 'struct P { const int x = 0; };\n' + M % ""),
    ("nested types", 'struct P { struct Q { int y; }; int x; };\n' + M % ""),
    ("types in a function", M % "struct P { int x; };"),
    ("brace initializers", M % "int x{5};"),
    ("constructor initializers", M % "int x(5);"),
    ("brace initializers for fields", 'struct P { int x{5}; };\n' + M % ""),
    ("short", M % "short n = 1;"),
    ("8-bit integers", M % "int8_t n = 1;"),
    ("16-bit integers", M % "int16_t n = 1;"),
    ("arrays of arrays from new", M % "auto p = new int[2][3];"),
    ("initial values for new[]", M % "auto p = new int[2]{1, 2};"),
    ("numbered placeholders", M % 'println("{0}", 1);'),
    ("width and alignment", M % 'println("{:>8}", 1);'),
    ("hexadecimal format", M % 'println("{:x}", 1);'),
    ("math constants", M % "double d = M_PI;"),
    ("C functions with variable arguments", 'extern "C" int printf(string f, ...);\n' + M % ""),
    ("vectors passed to C", 'extern "C" void f(vector<int> v);\n' + M % ""),
    ("unsigned types", M % "unsigned x = 10;"),
    ("unsigned literals", M % "int x = 10u;"),
    ("overloading", 'int f(int a) { return a; }\nint f(long a) { return 1; }\n' + M % ""),
    ("string append", M % 'string s; s.append("x");'),
    ("string insert", M % 'string s; s.insert(0, "x");'),
    ("string erase", M % 'string s; s.erase(0, 1);'),
    ("string replace", M % 'string s; s.replace(0, 1, "x");'),
    ("string rfind", M % 'string s; auto n = s.rfind("x");'),
    ("string compare", M % 'string s; auto n = s.compare("x");'),
    ("parsing numbers", M % 'int n = stoi("1");'),
    ("vector insert", M % "vector<int> v; v.insert(v.begin(), 1);"),
    ("vector erase", M % "vector<int> v; v.erase(0);"),
    ("vector iterators", M % "vector<int> v; auto b = v.begin();"),
    ("find on a hashmap", M % "hashmap<int, int> m; auto x = m.find(1);"),
    ("sorted maps", M % "map<int, int> m;"),
    ("sets", M % "set<int> s;"),
    ("deque", M % "deque<int> d;"),
    ("list", M % "list<int> d;"),
    ("stack", M % "stack<int> d;"),
    ("queue", M % "queue<int> d;"),
    ("pair", M % "pair<int, int> d;"),
    ("tuple", M % "tuple<int, int> d;"),
    ("reference parameters", 'void f(int& n) {}\n' + M % ""),
    ("references in loops", M % "vector<int> v; for (auto& x : v) {}"),
    ("reference variables", M % "int a = 1; int& b = a;"),
    ("bindings in loops", M % 'hashmap<string, int> m; for (auto [k, v] : m) {}'),
    ("bindings", 'struct P { int x; int y; };\n' + M % "auto [a, b] = P{1, 2};"),
    ("final classes", 'class A final { public: int x = 0; };\n' + M % ""),
    ("final methods", 'class A { public: virtual void f() final {} };\n' + M % ""),
    ("covariant return types", 'class A { public: virtual A f() { return A(); } };\nclass B : public A { public: B f() override { return B(); } };\n' + M % ""),
    ("private base classes", 'class A { public: int x = 0; };\nclass B : private A {};\n' + M % ""),
    ("protected base classes", 'class A { public: int x = 0; };\nclass B : protected A {};\n' + M % ""),
    ("operator overloading", 'struct P { int x = 0; P operator+(P o) { return o; } };\n' + M % ""),
    ("struct destructors", 'struct P { int x = 0; ~P() {} };\n' + M % ""),
    ("static members", 'struct P { static int count; };\n' + M % ""),
    ("constructors that can fail", 'class A { public: A() throws {} };\n' + M % ""),
    ("default template arguments", 'template<typename T = int> T f(T x) { return x; }\n' + M % ""),
    ("requires clauses", 'template<typename T> requires Number<T> T f(T x) { return x; }\n' + M % ""),
    ("generic methods", 'struct S { template<typename T> T f(T x) { return x; } };\n' + M % ""),
    ("working out a generic type's arguments", 'template<typename T> class Stack { public: T v; Stack(T a) : v(a) {} };\n' + M % "auto s = Stack(1);"),
    ("pointers into vectors", M % "vector<int> v = {1}; int* p = &v[0];"),
    ("pointers into strings", M % 'string s = "a"; char* p = &s[0];'),
    ("C callbacks", 'extern "C" void qsort(void* base, long n, long size, int (*cmp)(void*, void*));\n' + M % ""),
    ("C variables", 'extern "C" int errno;\n' + M % ""),
    ("16-bit integers in C structs", 'extern "C" { struct S { short a; }; }\n' + M % ""),
    ("unions in C structs", 'extern "C" { struct S { union { int a; long b; }; }; }\n' + M % ""),
    ("unions", 'union U { int a; long b; };\n' + M % ""),
    ("http::fetch", 'import http;\n' + M % 'auto r = http::fetch("x");'),
    ("async", 'async int f() { return 1; }\n' + M % ""),
    ("await", 'int f() { return 1; }\n' + M % "int x = await f();"),
    ("atomic values", M % "atomic<int> n;"),
    ("condition variables", M % "condition_variable c;"),
    ("channels", M % "channel<int> c;"),
    ("waiting with a time limit", 'void f() {}\n' + M % "auto t = spawn f(); t.wait_for(1);"),
    ("reading a field through reflect", 'import reflect;\nstruct P { int x = 0; };\n' + M % "for (auto f : reflect::type<P>().fields()) f.get(P{});"),
    ("the methods of a type in reflect", 'import reflect;\nstruct P { int x = 0; };\n' + M % "auto m = reflect::type<P>().methods();"),
]


def check_planned_features(cplus, workdir):
    """What the documents call planned is rejected, with the words that say it is planned, rather than an error that misleads."""
    source = workdir / "planned.cplus"
    ok = True
    for what, text in PLANNED_FEATURES:
        source.write_text(text)
        run = subprocess.run([cplus, "check", str(source)], capture_output=True, text=True, timeout=20)
        errors = [line for line in run.stderr.splitlines() if line.startswith("error:")]
        if run.returncode != 1 or not errors or "does not support" not in errors[0]:
            print(f"FAIL {what} is planned, but: exit code {run.returncode}, {errors[:1]}")
            ok = False
    return ok


SHEET_PRELUDE = """import json;
import http;
import reflect;
struct Point { int x = 0; int y = 0; };
struct Settable { int x = 0; void set() { x = 1; } };
struct P { int x = 0; int y = 0; };
class U { public: string name = "a"; int age = 0; U(string n) : name(n) {} int area() const { return 1; } };
class User { public: string name = "a"; int age = 0; User(string n) : name(n) {} };
class Animal { public: string name = "x"; int id = 0; virtual string sound() const { return "x"; } };
class Dog : public Animal { public: string sound() const override { return "w"; } };
class Left : public Animal {};
class Right : public Animal {};
class Both : public Left, public Right {};
class Pet { public: int id2 = 0; };
class Node { public: string name = "x"; int x = 1; Node? next; void go() {} Node(string n) : name(n) {} };
class Nest { public: string name = "x"; int w = 1; int h = 2; Nest() {} };
interface Speaker { string speak() const; };
template<typename T> class Stack { public: T v; Stack(T a) : v(a) {} };
template<typename T, int N> struct Buffer { array<T, N> items; };
template<typename T> T largest(T a, T b) { return a; }
template<typename T> concept Addable = requires(T a, T b) { a + b; };
class NotFound : public Error { public: NotFound() : Error("x") {} };
long sum(vector<int> v) { return 1; }
void save(vector<int> v) {}
void take(unique<Node> n) {}
void lend(Node n) {}
int fails() throws { return 1; }
const int MAX = 100;
constexpr int N = 5;
User? find(string n) { return null; }
extern "C" struct FILE;
extern "C" { struct tm { int a; }; struct ldiv_t2 { long quot; long rem; }; }
"""
SHEET_BODY = """    int n = 1; long someLong = 1; double someDouble = 1; double x = 2; char c = 'a'; string s = "s"; string t = "t"; vector<int> v = {1};
    int i = 0; bool flag = true; bool ok = true; bool bad = false; float f2 = 1; int pos = 0; string name = "n"; string url = "u"; string token = "t"; string body = "b"; string k = "k"; int y = 3; string text = "1";
    int? number = 1; hashmap<string, int> counts; char* charP = null; Point pp; json::Value obj; array<int, 3> arr;
    %s
"""
SHEET_VARIANTS = [
    'int* p = &n; int* q = &n; User u = User("a"); User? opt = null; hashmap<string, int> m; int a = 1; int b = 2;',
    'Point p; Point* q = &p; User? u = null; User? user = null; int? opt = 1; unique<Node> a = unique<Node>("x"); unique<Node> b = unique<Node>("y"); weak<Node> w; Node node = Node("x");',
    'char* p = null; Both d = Both(); local<Node> a = local<Node>("x"); local<Node> b = a; bool? opt = null; Mutex m = Mutex(); Task<long> r = spawn sum(v);',
    'void* p = null; Animal a = Dog(); hashmap<string, vector<int>> m; array<int, 3> b; Task<long, throws> r2 = spawn throwsLong(); Task<int> tt = spawn zero();',
    'array<int, 3> a = {1, 2, 3}; array<char, 8> b; http::Response r; http::WebSocket ws = try http::connect("ws://x"); http::Request request; hashmap<string, int> m; int e = 1; string v2 = "s";',
    'unique<Node> u = unique<Node>("x"); json::Value a = json::object(); auto e = json::Error("m", 1, 2); auto g = reflect::type<Point>(); int w = 1; Task<long> t2 = spawn sum(v);',
    'string a = "a"; string b = "b"; http::Error e = http::Error("m", "kind", "u"); auto f = reflect::type<Point>().fields()[0];',
    'Settable a = {}; Settable b = {}; bool first = true;',  # last: the habits are only tried with the first
]

# What the cheat sheet writes that is not code to compile: what is left out, what is typed at a shell, what a program prints.
SHEET_NOT_CODE = {"<cmath>", "<cctype>", "std::", "std::weak_ptr", "std::string", "0x...", "--debug", "-Wno-new-delete", "-lname", "import x;", "long double", "/0",
                  "http::fetch", "cplus check", "cplus run app.cplus --race", "cplus run app.cplus -- arg1 arg2", "cplus run app.cplus --debug"}
# Habits that the left side of the table only names a part of, which is fine on its own. The rest of its row says what is wrong.
SHEET_HABITS_THAT_COMPILE = {"int count = 0;", "x", "*p", "u", "if (flag)", "Node? next", "u1 == u2", "v[i]", "m[k]", "n", "s[s.size()]", "'\\0'", "s.substr(pos)",
                             "int x;", "int* p = new int;", "User u;", "array<int, 10000000> a;"}


def span_sources(span):
    """A piece of code from a document, in the places where it could mean something."""
    filled = span.replace("{ ... }", "{ }").replace("{...}", "{ }")
    end = "" if filled.rstrip().endswith((";", "}")) else ";"
    statement = (SHEET_PRELUDE + "long throwsLong() throws { return 1; }\nint zero() { return 0; }\nint f() throws { return 0; }\n"
                 "int main() throws {\n" + SHEET_BODY + "\n" + filled + end + "\nreturn 0;\n}\n")
    for variant in SHEET_VARIANTS:
        yield statement.replace("%s", variant)
    yield SHEET_PRELUDE + filled + end + "\nint main() { return 0; }\n"
    yield SHEET_PRELUDE + 'class Wrap : public Animal { public: int w = 1; int h = 2; Wrap(string n) {} ' + filled + end + " };\nint main() { return 0; }\n"
    yield SHEET_PRELUDE + "class Wrap2 { public: int w = 1; int h = 2; void m() { " + filled + end + " } };\nint main() { return 0; }\n"
    if filled.startswith("template<") and "(" not in filled and "{" not in filled:
        yield SHEET_PRELUDE + filled + " T pass__(T a) { return a; }\nint main() { return 0; }\n"
    if "catch" in filled and "try" not in filled:
        yield SHEET_PRELUDE + "int f() throws { return 0; }\nvoid w() throws { try { try f(); } " + filled.replace(";", "") + " { } }\nint main() { return 0; }\n"
    yield SHEET_PRELUDE + "template<typename T> void w() { T a = T(); " + filled + end + " }\nint main() { w<int>(); return 0; }\n"
    yield SHEET_PRELUDE + "template<typename T, typename Base> void w() { if constexpr (" + filled + ") { } }\nint main() { w<int, Animal>(); return 0; }\n"
    yield SHEET_PRELUDE + "int main() {\n" + filled + " dummy__;\nreturn 0;\n}\n"


def span_compiles(cplus, workdir, span, index, candidates):
    path = workdir / f"span{index}.cplus"
    for source in candidates:
        path.write_text(source)
        run = subprocess.run([cplus, "check", str(path)], capture_output=True, text=True, timeout=60)
        if run.returncode == 0:
            return True
        if run.returncode != 1:
            print(f"FAIL a document's `{span}` stops the compiler: exit code {run.returncode}")
            return True
    return False


def check_cheat_sheet(cplus, workdir):
    """Every form the cheat sheet tells a reader to write compiles, and every C++ habit it warns of is rejected."""
    text = (ROOT / "docs" / "CHEATSHEET.md").read_text()
    works = text.split("## Works today")[1].split("## Different from C++")[0]
    different = text.split("## Different from C++")[1].split("## Planned")[0]
    forms = "\n".join(path.read_text() for path in sorted((ROOT / "tests" / "cases").glob("cheatsheet_forms*.cplus")))
    def rows(part):
        return re.findall(r"^\| (.+?) \| (.+) \|$", part, re.M)
    def is_word(span):
        return re.fullmatch(r"[\W_]+|\w+", span) is not None
    def in_forms(span):
        for piece in span.split("; "):
            piece = piece.strip().rstrip(";")
            pattern = re.escape(piece).replace(r"\{\ \.\.\.\ \}", r"\{.*?\}").replace(r"\{\.\.\.\}", r"\{.*?\}").replace(r"\.\.\.", ".*?")
            if piece and not re.search(pattern, forms, re.S):
                return False
        return True
    write = []
    for _, column in rows(works) + rows(different):
        for span in re.findall(r"`([^`]+)`", column):
            span = span.replace("\\|", "|")
            if span not in write and span not in SHEET_NOT_CODE and not is_word(span) and not in_forms(span):
                write.append(span)
    habits = []
    for column, _ in rows(different):
        for span in re.findall(r"`([^`]+)`", column):
            span = span.replace("\\|", "|")
            if span not in habits and span not in SHEET_HABITS_THAT_COMPILE and not span.startswith(("-g", "-f", "#", "std::setprecision", "curl -k", "verify=")):
                habits.append(span)
    def habit(pair):
        """What the compiler makes of it: whether it compiled, and whether it was ever rejected for itself."""
        index, span = pair
        end = "" if span.rstrip().endswith((";", "}")) else ";"
        helpers = "long throwsLong() throws { return 1; }\nint zero() { return 0; }\nint f() throws { return 0; }\n"
        candidates = [(SHEET_PRELUDE + helpers + "int main() throws {\n" + SHEET_BODY + "\n" + span + end + "\nreturn 0;\n}\n").replace("%s", SHEET_VARIANTS[0]),
                      SHEET_PRELUDE + "int main() throws {\n" + span + end + "\nreturn 0;\n}\n",
                      span + end + "\nint main() { return 0; }\n",
                      "int main() {\n" + span + end + "\nreturn 0;\n}\n"]
        path = workdir / f"habit{index}.cplus"
        for at, source in enumerate(candidates):
            path.write_text(source)
            run = subprocess.run([cplus, "check", str(path)], capture_output=True, text=True, timeout=60)
            if run.returncode == 0:
                return at <= 1  # with nothing else around it, it may mean something else
            errors = [line for line in run.stderr.splitlines() if line.startswith("error:") and "could not compile" not in line]
            names = r"is not declared|already declared|there is no function named" + ("|unknown type" if at >= 2 else "")  # alone, the types of the others are unknown too
            if errors and not re.search(names, errors[0]):
                return False
        return None  # only ever rejected for what it names
    ok = True
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        wrote = list(pool.map(lambda pair: span_compiles(cplus, workdir, pair[1], pair[0], span_sources(pair[1])), enumerate(write)))
        rejected = list(pool.map(habit, enumerate(habits)))
    for span, good in zip(write, wrote):
        if not good:
            print(f"FAIL the cheat sheet writes `{span}`, which does not compile")
            ok = False
    for span, compiled in zip(habits, rejected):
        if compiled:
            print(f"FAIL the cheat sheet warns of `{span}`, which compiles")
            ok = False
        elif compiled is None:
            print(f"FAIL the cheat sheet warns of `{span}`, which is only rejected for a name it uses, so this cannot tell")
            ok = False
    return ok


# What the spec's text writes that is not code: tools and what is typed at a shell, the names of other languages' things, and how
# rules are written.
SPEC_NOT_CODE = {
    "compiler/SpecRefs.h", "cplus explain <topic>", ".cplus", "\\n \\t \\r \\0 \\\\ \\\" \\'", "\\xHH", "\\012", "<cctype>", "<cmath>", "std::format", "std::string", "std::",
    "std::optional<T>", "std::optional", "std::vector", "std::array", "std::unordered_map", "std::numbers::pi", "std::println", "std::weak_ptr", "std::make_shared",
    "std::unique_ptr", "std::async", "int + long", "long + float", "float + double", "Transfer-Encoding", "wss://", "libcurl4-openssl-dev", "CPLUS_CHECK_LEAKS=1",
    "CPLUS_CHECK_LEAKS=0", "-Wno-new-delete", "cplus check", "cplus run", "cplus run greet.cplus -- Ada", "cplus build app.cplus -lsqlite3", "-L <directory>", ".hplus",
    "break cplus_trap", "uncaught error:", "--race", "--debug", "cplus build", "cplus.", "-fsanitize=address", "llvm-symbolizer", "libclang-rt-18-dev", "exit()",
    "cplus run app.cplus --race", "-l<name>", "-L<dir>", "--emit-llvm", "cplus explain", "spec:", "cplus explain cheatsheet", "{ x }", "[ x ]", "value_or()", "new[]",
}
# What the spec's text says is an error, with a program that does it where the text only gives a part. "^" means outside any function.
SPEC_REJECTED = {
    "#include": "^#include <vector>", "#define": "^#define X 1", "#ifdef": "^#ifdef X", "0'10": "int a = 0'10;", "'ab'": "char a = 'ab';", "'é'": "char a = 'é';",
    "L'a'": "auto a = L'a';", "u'a'": "auto a = u'a';", "U'a'": "auto a = U'a';", 'u8"text"': 'auto a = u8"text";', "long long": "long long a = 1;",
    "long double": "long double a = 1;", "0x1.8p3": "double a = 0x1.8p3;", "int n = true;": "int n = true;", "unsigned char": "unsigned char a = 1;",
    "signed char": "signed char a = 1;", "c *= 2": "char c = 'a'; c *= 2;", "s.at(i)": 'string s = "a"; int i = 0; s.at(i);', "int??": "int?? a;",
    "auto x = null;": "auto x = null;", "found?.age = 3": 'User? found = null; found?.age = 3;',
    "found.name": 'User? found = null; println("{}", found.name);', "list[i]": 'vector<int>? list = null; int i = 0; println("{}", list[i]);',
    "for (auto x : list)": "vector<int>? list = null; for (auto x : list) {}", "auto p = {1, 2};": "auto p = {1, 2};",
    "bool operator==(const Point&) const = default;": "^struct Q { bool operator==(const Q&) const = default; };", "new User(...)": 'auto u = new User("a");',
    "u->age": 'User u = User("a"); println("{}", u->age);', "class Dog : Animal": "^class Dog2 : Animal {};", "dynamic_cast<Dog*>": "Animal a = Dog(); auto d = dynamic_cast<Dog*>(a);",
    "dynamic_cast<Dog&>": "Animal a = Dog(); auto d = dynamic_cast<Dog&>(a);", "static_cast<Dog>(a)": "Animal a = Dog(); Dog d = static_cast<Dog>(a);", "(Dog)a": "Animal a = Dog(); Dog d = (Dog)a;",
    "b.Base::id": 'Twice b = Twice(); println("{}", b.Base::id);', "int Rect::area() { ... }": "^struct Rect { int w = 0; };\nint Rect::area() { return 1; }",
    "this->w": "^struct R2 { int w = 0; int f() { return this->w; } };", "&this": "^struct R3 { int w = 0; R3* f() { return &this; } };",
    "vector<User>(n)": "int n = 1; auto v = vector<User>(n);", "int n = v.size();": "vector<int> v; int n = v.size();", "v[-1]": 'vector<int> v = {1}; println("{}", v[-1]);',
    "makeList().push_back(1)": "makeList().push_back(1);", "makeThree().fill(1)": "makeThree().fill(1);", "begin()": "vector<int> v; auto b = v.begin();", "end()": "vector<int> v; auto b = v.end();",
    "const vector<int>&": "^void f2(const vector<int>& v) {}", "int* p = a;": "array<int, 3> a; int* p = a;", "m.find(k)": "hashmap<int, int> m; int k = 1; auto x = m.find(k);",
    "http::fetch": 'auto r = http::fetch("x");', "int*?": "int*? a = null;", "auto*": "int x = 1; auto* p = &x;", "&v[0]": "vector<int> v = {1}; int* p = &v[0];",
    "(int*)0x1000": "int* p = (int*)0x1000;", "int* p = 0;": "int* p = 0;", "T&": "^template<typename T> void f2(T& x) {}", "int* a, b;": "int* a, b;",
    "new int[3]{1, 2, 3}": "auto p = new int[3]{1, 2, 3};", "new (buffer) T": "int buffer = 0; auto p = new (buffer) int;", "new auto(x)": "int x = 1; auto p = new auto(x);",
    "[[nodiscard]]": "^[[nodiscard]] int f2() { return 1; }", "int x{5}": "int x{5};", "int x(5)": "int x(5);", "int f(int x);": "^int f2(int x);", "int values[]": '^extern "C" void f2(int values[]);',
    "char name[64];": '^extern "C" { struct S2 { char name[64]; }; }', "typedef struct { ... } name;": "^typedef struct { int a; } name;", "struct FILE*": "struct FILE* f = null;",
    "f[i]": "FILE* f = null; int i = 0; auto x = f[i];", "f + 1": "FILE* f = null; auto x = f + 1;", "sizeof(FILE)": "long x = sizeof(FILE);", "typedef struct x x;": "^typedef struct x x;",
    "class X;": "^class X;", "json::Mine": "^struct json::Mine {};", "for (auto& x : v)": "vector<int> v; for (auto& x : v) {}", "for (const auto& x : v)": "vector<int> v; for (const auto& x : v) {}",
    "Point operator+(Point other) const": "^struct P2 { int x = 0; P2 operator+(P2 other) const { return other; } };", "1 << 40": "long x = 1 << 40;", "sizeof(void)": "long x = sizeof(void);",
    "{:x}": 'println("{:x}", 1);', 'throw "text"': '^void f2() throws { throw "text"; }', "const Error& e": "try { } catch (const Error& e) { }", "try f() + g()": "int x = try f() + g();",
    "Node n = a;": 'local<Node> a = local<Node>("x"); Node n = a;', "local<Node> a = n;": 'Node n = Node("x"); local<Node> a = n;', "weak<Node>?": "weak<Node>? w = null;",
    "w == null": 'weak<Node> w; println("{}", w == null);', "largest(1, 2.5)": "auto x = largest(1, 2.5);", "Stack(1)": "auto s = Stack(1);", "spawn f(x).wait()": "int x = 1; spawn given(x).wait();",
    "v.back().x = 1": "vector<Point> v = {Point{}}; v.back().x = 1;", "v.back().set()": "vector<Settable> v = {Settable{}}; v.back().set();",
    "v.back().p.set()": "vector<HoldsSettable> v = {HoldsSettable{}}; v.back().p.set();",
    "(f ? a : b).set()": "Settable a = {}; Settable b = {}; bool f = true; (f ? a : b).set();",
    "for (auto [k, v] : m)": "hashmap<string, int> m; for (auto [k, v] : m) {}", "f<vector<T>>": "^template<typename T> void grows(T x) { grows<vector<T>>(vector<T>{x}); }\nvoid uses() { grows(1); }",
}
SPEC_REJECT_PRELUDE = """struct Settable { int x = 0; void set() { x = 1; } };
struct HoldsSettable { Settable p; };
class Base { public: int id = 0; };
class LeftOf : public Base {};
class RightOf : public Base {};
class Twice : public LeftOf, public RightOf {};
vector<int> makeList() { return {}; }
array<int, 3> makeThree() { return {}; }
int f() throws { return 0; }
int g() throws { return 0; }
int given(int x) { return x; }
"""


def check_spec_spans(cplus, workdir):
    """Each piece of code in the spec's text compiles, alone or in a test that runs, or is rejected if the text says it is an error."""
    text = re.sub(r"^```.*?^```", "", (ROOT / "docs" / "SPEC.md").read_text(), flags=re.S | re.M)
    spans = []
    for span in re.findall(r"(?<!`)`([^`]+)`(?!`)", text):
        span = " ".join(span.split()) if "\n" in span else span
        if span not in spans:
            spans.append(span)
    def code_of(line):  # what the compiler reads of a line, and what a test says it prints
        if re.match(r"\s*//[ \t]*expect:", line):
            return line
        out, quote, at = "", None, 0
        while at < len(line):
            c = line[at]
            if quote:
                out += c
                if c == "\\":
                    out += line[at + 1:at + 2]
                    at += 1
                elif c == quote:
                    quote = None
            elif c in "\"'":
                quote = c
                out += c
            elif line.startswith("//", at):
                break
            else:
                out += c
            at += 1
        return out
    running = []
    for path in list((ROOT / "tests" / "cases").glob("*.cplus")) + list((ROOT / "examples").rglob("*.cplus")):
        source = path.read_text()
        if not re.search(r"//[ \t]*error:", source):
            running.append("\n".join(code_of(line) for line in source.splitlines()))
    running = "\n".join(running)
    def in_tests(span):
        pattern = re.escape(span).replace(r"\{\ \.\.\.\ \}", r"\{.*?\}").replace(r"\{\.\.\.\}", r"\{.*?\}").replace(r"\.\.\.", ".*?")
        return re.search(pattern, running, re.S) is not None
    ok = True
    for listed in list(SPEC_NOT_CODE) + list(SPEC_REJECTED):
        if listed not in spans:
            print(f"FAIL the spec no longer has `{listed}`, which tests/run_tests.py still has a list entry for")
            ok = False
    def rejected(pair):
        index, (span, code) = pair
        top = code.startswith("^")
        body = code[1:] if top else code
        source = SHEET_PRELUDE + SPEC_REJECT_PRELUDE + (body + "\nint main() { return 0; }\n" if top else "int main() throws {\n" + body + "\nreturn 0;\n}\n")
        path = workdir / f"rejected{index}.cplus"
        path.write_text(source)
        run = subprocess.run([cplus, "check", str(path)], capture_output=True, text=True, timeout=60)
        errors = [line for line in run.stderr.splitlines() if line.startswith("error:") and "could not compile" not in line]
        missing_name = errors and re.search(r"is not declared|already declared|there is no function named", errors[0])
        return span, run.returncode, errors[:1], missing_name
    left = [span for span in spans if span not in SPEC_NOT_CODE and span not in SPEC_REJECTED and not re.fullmatch(r"[\W_]+|\w+", span) and not in_tests(span)]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for span, code, errors, missing_name in pool.map(rejected, enumerate(SPEC_REJECTED.items())):
            if code != 1 or missing_name:
                print(f"FAIL the spec says `{span}` is an error, but: exit code {code}, {errors}")
                ok = False
        compiled = list(pool.map(lambda pair: span_compiles(cplus, workdir, pair[1], "s%d" % pair[0], span_sources(pair[1])), enumerate(left)))
    for span, good in zip(left, compiled):
        if not good:
            print(f"FAIL the spec writes `{span}`, which neither compiles nor is in a test that runs")
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
        (["run", g, "one", "two"], 2, "to pass arguments to the program, put them after `--`"), (["build", g, "-O2"], 2, "write -O"),
        (["build", g, "-g"], 2, "'-g' is not an option of cplus"), (["build", g, "-fsanitize=thread"], 2, "is not an option of cplus"),
        (["build", g, "--debug", "--race"], 2, "--debug and --race cannot be used together"), (["explain", "a", "b"], 2, "explain takes one topic"),
        (["check", str(text)], 2, "is not a C+ source file"), (["check", str(workdir / "gone.cplus")], 1, "cannot read"),
        (["build", g, "-o", str(missing_dir / "out")], 1, "cannot write"), (["build", g, "-o", str(workdir / "out"), "-lno_such_library_here"], 1, "linking failed"),
    ]
    ok = True
    for args, code, message in cases:
        run = subprocess.run([cplus, *args], capture_output=True, text=True, cwd=workdir)
        lacking = errors_without_help(run.stderr)
        if run.returncode != code or message not in run.stderr or lacking:
            print(f"FAIL cplus {' '.join(args)}: exit code {run.returncode}, expected {code}, and '{message}'. {lacking}\n  " + run.stderr[:300])
            ok = False
    # A disk with no room, which is not pretending: the system says that no file can grow.
    def no_room():
        resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    for args, advice in ((["build", g, "-o", str(workdir / "out")], "choose another with -o"), (["run", g], "set the environment variable TMPDIR")):
        run = subprocess.run([cplus, *args], capture_output=True, text=True, cwd=workdir, preexec_fn=no_room)
        if run.returncode != 1 or "cannot write" not in run.stderr or advice not in run.stderr or errors_without_help(run.stderr):
            print(f"FAIL cplus {' '.join(args)} with no room for the object file: exit code {run.returncode}\n  " + run.stderr[:400])
            ok = False
    # What a machine without these would report, by pretending.
    c_struct = workdir / "c_struct.cplus"
    c_struct.write_text('extern "C" { struct ldiv_t { long quot; long rem; }; ldiv_t ldiv(long a, long b); }\nint main() { return 0; }\n')
    pretending = [
        (["build", str(c_struct), "-o", str(workdir / "out")], "CPLUS_TARGET_FOR_TESTS", "i686-unknown-linux-gnu", "this compiler does not support passing structs by value to C functions such as 'ldiv'"),
        (["build", g, "--race"], "CPLUS_NO_SANITIZERS_FOR_TESTS", "1", "--race needs ThreadSanitizer"),
        (["build", g, "--debug"], "CPLUS_NO_SANITIZERS_FOR_TESTS", "1", "--debug needs AddressSanitizer"),
        (["build", g, "-o", str(workdir / "out")], "CPLUS_LINKER_FOR_TESTS", str(workdir / "no-such-compiler"), "could not run"),
        (["run", g], "CPLUS_LINKER_FOR_TESTS", "true", "could not run"),  # it says all went well, and leaves no program to run
        (["build", g, "-o", str(workdir / "out")], "CPLUS_TARGET_FOR_TESTS", "no-such-machine", "this is about the machine"),
        (["build", g, "-o", str(workdir / "out")], "CPLUS_INVALID_IR_FOR_TESTS", "1", "this is a mistake in the compiler, and not in the program"),
        (["build", g, "-o", str(workdir / "out")], "CPLUS_NO_OBJECT_FILES_FOR_TESTS", "1", "this target cannot emit object files"),
    ]
    for args, name, value, message in pretending:
        run = subprocess.run([cplus, *args], capture_output=True, text=True, cwd=workdir, env=dict(os.environ, **{name: value}))
        if run.returncode == 0 or message not in run.stderr or errors_without_help(run.stderr):
            print(f"FAIL cplus {' '.join(args)} with {name}: exit code {run.returncode}, and not '{message}'\n  " + run.stderr[:300])
            ok = False
    if sys.platform.startswith("linux"):  # a folder that is there, and where nothing can be made
        run = subprocess.run([cplus, "run", g], capture_output=True, text=True, env=dict(os.environ, TMPDIR="/proc"))
        if run.returncode != 1 or "cannot make a folder for temporary files" not in run.stderr or errors_without_help(run.stderr):
            print(f"FAIL cplus run where no folder can be made: exit code {run.returncode}\n  " + run.stderr[:300])
            ok = False
    run = subprocess.run([cplus, "run", g], capture_output=True, text=True, env=dict(os.environ, TMPDIR=str(missing_dir)))
    if run.returncode != 1 or "cannot find a folder for temporary files" not in run.stderr or errors_without_help(run.stderr):
        print(f"FAIL cplus run with no folder for temporary files: exit code {run.returncode}\n  " + run.stderr[:300])
        ok = False
    return ok


def check_threads_linked(cplus, workdir):
    """A program written on threads names their library, as older systems need, and one with no threads does not.
    What is in the way here is not a library, so naming it stops the linker, whatever the system's own libpthread has in it."""
    folder = workdir / "not_a_library"
    folder.mkdir(exist_ok=True)
    (folder / "libpthread.a").write_text("this is not a library\n")
    source = workdir / "linked.cplus"
    def links(text):
        source.write_text(text)
        return subprocess.run([cplus, "build", str(source), "-o", str(workdir / "linked"), "-L", str(folder)], capture_output=True, text=True).returncode == 0
    ok = True
    threads = {"a Mutex": "int main() { Mutex m = Mutex(); m.lock(); m.unlock(); return 0; }\n",
               "a LockGuard": "int main() { Mutex m = Mutex(); LockGuard g = LockGuard(m); return 0; }\n",
               "spawn": "void f() {}\nint main() { spawn f(); return 0; }\n"}
    for what, text in threads.items():
        if links(text):
            print(f"FAIL a program with {what} is written on threads, but does not link them")
            ok = False
    if not links("int main() { return 0; }\n"):
        print("FAIL a program with no threads links them")
        ok = False
    return ok


def check_reflect_alone(cplus, workdir):
    """Reflection only looks at types, so what makes values for JSON is not there for it."""
    source = workdir / "reflect_alone.cplus"
    source.write_text("import reflect;\nclass N { public: int x = 0; };\nstruct S { hashmap<string, N> m; };\nint main() { println(\"{}\", reflect::type<S>().name()); return 0; }\n")
    run = subprocess.run([cplus, "build", str(source), "--emit-llvm"], capture_output=True, text=True)
    made = [name for name in ("#make", "#put") if name in run.stdout]
    if run.returncode != 0 or made:
        print(f"FAIL a program that only imports reflect has what JSON needs: {made}\n  " + run.stderr[:300])
        return False
    return True


def check_json_of_direct(cplus, workdir):
    """json::of makes its value directly, as text between the two would cost twice as much."""
    source = workdir / "json_of.cplus"
    source.write_text("import json;\nint main() { println(\"{}\", json::of(1).whole()); return 0; }\n")
    run = subprocess.run([cplus, "build", str(source), "--emit-llvm"], capture_output=True, text=True)
    body = re.search(r'define [^\n]*"cplus\.json::of<int>"[^\n]*\n(.*?)^}', run.stdout, re.S | re.M)
    calls = re.findall(r'call [^@\n]*@"?([^"(\s]+)', body.group(1)) if body else []
    through_text = [name for name in calls if "json_write" in name or "json_read" in name or "stringify" in name or "parse" in name]
    if run.returncode != 0 or through_text or "cplus_json_build" not in calls:
        print(f"FAIL json::of goes through text: it calls {calls}\n  " + run.stderr[:300])
        return False
    return True


def check_advice_grid(cplus, workdir):
    """What the errors about copies say to write must compile, and must change what was meant to change."""
    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    import advice_grid
    grid = workdir / "grid"
    grid.mkdir(exist_ok=True)
    count, found = advice_grid.problems(cplus, grid)
    for p in found:
        print(f"FAIL advice grid: {p}")
    return not found


def tracked_binaries(root=ROOT):
    """Files in the repo that are not text. A built program holds the paths it was built from, so it must never be committed."""
    listed = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True)
    if listed.returncode != 0:
        return []  # not a checkout, as in an unpacked archive
    found = []
    for name in listed.stdout.decode().split("\0"):
        path = pathlib.Path(root) / name
        if name and path.is_file() and b"\0" in path.read_bytes():
            found.append(name)
    return found


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
            ok = check_leak_report(cplus, pathlib.Path(tmp)) and ok
            ok = check_planned_features(cplus, pathlib.Path(tmp)) and ok
            ok = check_cheat_sheet(cplus, pathlib.Path(tmp)) and ok
            ok = check_spec_spans(cplus, pathlib.Path(tmp)) and ok
            ok = check_print_lines(cplus, pathlib.Path(tmp)) and ok
            ok = check_threads_linked(cplus, pathlib.Path(tmp)) and ok
            ok = check_reflect_alone(cplus, pathlib.Path(tmp)) and ok
            ok = check_json_of_direct(cplus, pathlib.Path(tmp)) and ok
            ok = check_advice_grid(cplus, pathlib.Path(tmp)) and ok
            ok = check_source_rules() and ok
            for name in tracked_binaries():
                print(f"FAIL {name} is in the repo, and it is not text: a built program holds the paths it was built from")
                ok = False
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
