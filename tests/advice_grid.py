"""Follows the advice about copies: builds programs from a grid of shapes, takes the code the error suggests, puts
it in place of the line that was wrong, and checks that the program compiles and changed what was meant to change.
`python3 tests/advice_grid.py build/cplus` runs it alone, and run_tests.py runs it with the rest."""
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PRELUDE = """
struct S {
    int x = 1;
    vector<int> items;
    void set() { x = 7; }
    void add(int n) { x = x + n; }
    void merge(S other) { x = x + other.x; }
};
struct H {
    vector<int> items;
    S s;
    S? os;
    vector<int>? ov;
};
struct O { H? h; };
class C {
public:
    int n = 1;
    void go() { n = n + 1; }
};
struct Plain { int y = 0; S p; };
vector<int>? gives() { return vector<int>{1}; }
S? givesS() { return S{}; }
"""

# What is declared, the line that is wrong, and what must be printed after what the error says has taken its place.
SAME = [  # (declared, wrong, printed, expected)
    ("vector<int>? o = vector<int>{1};", "o.push_back(N);", "o", "[1, 5]"),
    ("vector<int>? o = vector<int>{1};", "o.clear();", "o", "[]"),
    ("vector<int>? o = vector<int>{1};", "o[0] = N;", "o", "[5]"),
    ("S? o = S{};", "o.set();", "o?.x", "7"),
    ("S? o = S{};", "o?.set();", "o?.x", "7"),
    ("S? o = S{};", "o.add(N);", "o?.x", "6"),
    ("S? o = S{};", "o?.add(N);", "o?.x", "6"),
    ("S? o = S{};", "o?.items.push_back(N);", "o?.items", "[5]"),
    ("S? o = S{};", "o.value().x = N;", "o?.x", "5"),
    ("S? o = S{};", "o.value().set();", "o?.x", "7"),
    ("int? o = 1;", "o.value() = N;", "o", "5"),
    ("H h = {};\nh.os = S{};", "h.os.set();", "h.os?.x", "7"),
    ("H h = {};\nh.os = S{};", "h.os?.set();", "h.os?.x", "7"),
    ("H h = {};\nh.os = S{};", "h.os?.items.push_back(N);", "h.os?.items", "[5]"),
    ("H h = {};\nh.ov = vector<int>{1};", "h.ov.push_back(N);", "h.ov", "[1, 5]"),
    ("H h = {};\nh.ov = vector<int>{1};", "h.ov[0] = N;", "h.ov", "[5]"),
    ("vector<S?> w = {S{}};", "w[0].set();", "w[0]?.x", "7"),
    ("vector<S?> w = {S{}};", "w[0]?.set();", "w[0]?.x", "7"),
    ("vector<S?> w = {S{}};\nint i = 0;", "w[i]?.set();", "w[0]?.x", "7"),
    ("vector<S?> w = {S{}};", "w[0]?.items.push_back(N);", "w[0]?.items", "[5]"),
    ("vector<S?> w = {S{}};", "w.back().set();", "w[0]?.x", "7"),
    ("vector<S?> w = {S{}};", "w.back()?.set();", "w[0]?.x", "7"),
    ("vector<S?> w = {S{}};", "w.front()?.set();", "w[0]?.x", "7"),
    ("vector<S?> w = {S{}};", "w.front()?.items.push_back(N);", "w[0]?.items", "[5]"),
    ("vector<vector<int>?> vs = {vector<int>{1}};", "vs.back().push_back(N);", "vs[0]", "[1, 5]"),
    ("vector<vector<int>?> vs = {vector<int>{1}};", "vs.front()[0] = N;", "vs[0]", "[5]"),
    ("vector<S> ws = {S{}};", "ws.back().set();", "ws[0].x", "7"),
    ("vector<S> ws = {S{}};", "ws.back().x = N;", "ws[0].x", "5"),
    ("vector<S> ws = {S{}};", "ws.front().x = N;", "ws[0].x", "5"),
    ("vector<S> ws = {S{}};", "ws.back().items.push_back(N);", "ws[0].items", "[5]"),
    ("vector<Plain> ps = {Plain{}};", "ps.back().p.set();", "ps[0].p.x", "7"),
    ("vector<Plain> ps = {Plain{}};", "ps.back().p.x = N;", "ps[0].p.x", "5"),
    ("vector<Plain> ps = {Plain{}};", "ps.front().p.add(N);", "ps[0].p.x", "6"),
    ('hashmap<string, S> m = {{"a", S{}}};', 'm.get("a").set();', 'm["a"].x', "7"),
    ('hashmap<string, S> m = {{"a", S{}}};', 'm.get("a")?.set();', 'm["a"].x', "7"),
    ('hashmap<string, S> m = {{"a", S{}}};', 'm.get("a")?.items.push_back(N);', 'm["a"].items', "[5]"),
    ('hashmap<string, S> m = {{"a", S{}}};', 'm.at("a").set();', 'm["a"].x', "7"),
    ('hashmap<string, S> m = {{"a", S{}}};', 'm.at("a").x = N;', 'm["a"].x', "5"),
    ('hashmap<string, S> m = {{"a", S{}}};\nstring k = "a";', "m.get(k)?.set();", 'm["a"].x', "7"),
    ('hashmap<string, S> m = {{"a", S{}}};\nstring k = "a";', "m.at(k).set();", 'm["a"].x', "7"),
    ('hashmap<string, vector<int>> m = {{"a", vector<int>{1}}};', 'm.get("a").push_back(N);', 'm["a"]', "[1, 5]"),
    ('hashmap<string, vector<int>> m = {{"a", vector<int>{1}}};', 'm.get("a")[0] = N;', 'm["a"]', "[5]"),
    ('hashmap<string, vector<int>> m = {{"a", vector<int>{1}}};', 'm.at("a").push_back(N);', 'm["a"]', "[1, 5]"),
    ("hashmap<int, S> mi = {{1, S{}}};", "mi.get(1)?.set();", "mi[1].x", "7"),
    ("hashmap<int, S> mi = {{1, S{}}};", "mi.at(1).set();", "mi[1].x", "7"),
    ("hashmap<char, S> mc = {{'a', S{}}};", "mc.get('a')?.set();", "mc['a'].x", "7"),
    ('hashmap<string, S?> mo = {{"a", S{}}};', 'mo.at("a").set();', 'mo["a"]?.x', "7"),
    ('hashmap<string, S?> mo = {{"a", S{}}};', 'mo.at("a")?.set();', 'mo["a"]?.x', "7"),
    ('hashmap<string, S?> mo = {{"a", S{}}};', 'mo.at("a")?.items.push_back(N);', 'mo["a"]?.items', "[5]"),
    ('hashmap<string, S?> mo = {{"a", S{}}};', 'mo.get("a")?.set();', 'mo["a"]?.x', "7"),
    ('hashmap<string, S?> mo = {{"a", S{}}};', 'mo.get("a")?.items.push_back(N);', 'mo["a"]?.items', "[5]"),
    ('hashmap<string, vector<int>?> mo = {{"a", vector<int>{1}}};', 'mo.get("a").push_back(N);', 'mo["a"]', "[1, 5]"),
    ('hashmap<string, vector<int>?> mo = {{"a", vector<int>{1}}};', 'mo.at("a").push_back(N);', 'mo["a"]', "[1, 5]"),
    ("O? a = O{.h = H{}};", "a?.h?.items.push_back(N);", "a?.h?.items", "[5]"),
    ("O? a = O{.h = H{}};", "a?.h?.s.set();", "a?.h?.s.x", "7"),
    ("O? b = O{.h = H{}};", "b?.h?.items.push_back(N);", "b?.h?.items", "[5]"),
    ("vector<O?> vo = {O{.h = H{}}};", "vo.back()?.h?.items.push_back(N);", "vo[0]?.h?.items", "[5]"),
    ('hashmap<string, O?> mo = {{"a", O{.h = H{}}}};', 'mo.at("a")?.h?.items.push_back(N);', 'mo["a"]?.h?.items', "[5]"),
    ("vector<unique<C>>? u = vector<unique<C>>{};", "u.clear();", "u?.size()", "0"),
    ("vector<unique<C>>? u = vector<unique<C>>{};", "u?.clear();", "u?.size()", "0"),
]

# What only exists as a copy: the code that follows the advice has to compile, and the wording says so.
TEMPORARY = [
    ("", "gives().push_back(N);"),
    ("", "gives()[0] = N;"),
    ("", "givesS()?.set();"),
    ("", "givesS().set();"),
    ("", "givesS()?.items.push_back(N);"),
]

# Shapes the compiler cannot be sure of: it must either give no code, or give code that does what was meant. What it
# must never do is say that what is in the program only exists as a copy.
NOT_SIMPLE = [
    ('hashmap<string, hashmap<string, S>> m = {{"a", {{"b", S{}}}}};', 'm.at("a").at("b").set();'),
    ('hashmap<string, O> m = {{"a", O{.h = H{}}}};', 'm.get("a")?.h?.items.clear();'),
    ("vector<vector<S?>> w = {{S{}}};", "w.back().back()?.set();"),
    ("vector<vector<S>> w = {{S{}}};", "w.back().back().x = N;"),
    ("vector<vector<S>> w = {{S{}}};", "w.back().back().set();"),
    ('vector<hashmap<string, vector<int>>> w = {{{"b", vector<int>{1}}}};', 'w.back().get("b").push_back(N);'),
    ("vector<H> w = {H{}};", "w.back().items.back() = N;"),
    ("vector<H> w = {H{}};", "w.back().os?.items.clear();"),
    ("O? o = O{.h = H{}};", "o.value().h.value().items.clear();"),
]

# Outside main: the line that is wrong goes where the @ is, and main shows what it did.
ELSEWHERE = [  # (definitions, wrong, main, expected)
    ("void f(H* p) { @ }", "p->os?.set();", 'H h = {};\nh.os = S{};\nf(&h);\nprintln("{}", h.os?.x);', "7"),
    ("void f(H* p) { @ }", "p->os.set();", 'H h = {};\nh.os = S{};\nf(&h);\nprintln("{}", h.os?.x);', "7"),
    ("void f(H* p) { @ }", "(*p).os?.set();", 'H h = {};\nh.os = S{};\nf(&h);\nprintln("{}", h.os?.x);', "7"),
    ("void f(H* p) { @ }", "p->ov.push_back(N);", 'H h = {};\nh.ov = vector<int>{1};\nf(&h);\nprintln("{}", h.ov);', "[1, 5]"),
    ("void f(H* p) { @ }", "p->ov[0] = N;", 'H h = {};\nh.ov = vector<int>{1};\nf(&h);\nprintln("{}", h.ov);', "[5]"),
    ("void f(H* p) { @ }", "p[0].os?.set();", 'H h = {};\nh.os = S{};\nf(&h);\nprintln("{}", h.os?.x);', "7"),
    ("void f(H** p) { @ }", "(*p)->os?.set();", 'H h = {};\nh.os = S{};\nH* q = &h;\nf(&q);\nprintln("{}", h.os?.x);', "7"),
    ("void f(vector<S>* p) { @ }", "p->back().set();", 'vector<S> w = {S{}};\nf(&w);\nprintln("{}", w[0].x);', "7"),
    ("void f(vector<S>* p) { @ }", "(*p).back().x = N;", 'vector<S> w = {S{}};\nf(&w);\nprintln("{}", w[0].x);', "5"),
    ('void f(hashmap<string, S>* p) { @ }', 'p->at("a").set();', 'hashmap<string, S> m = {{"a", S{}}};\nf(&m);\nprintln("{}", m["a"].x);', "7"),
    ('void f(hashmap<string, S>* p) { @ }', 'p->get("a")?.set();', 'hashmap<string, S> m = {{"a", S{}}};\nf(&m);\nprintln("{}", m["a"].x);', "7"),
    ("void f(vector<int>?* p) { @ }", "p->push_back(N);", 'vector<int>? o = vector<int>{1};\nf(&o);\nprintln("{}", o);', "[1, 5]"),
    ("void f(S?* p) { @ }", "p->set();", 'S? o = S{};\nf(&o);\nprintln("{}", o?.x);', "7"),
    ("struct M { S? o; vector<int>? ov; vector<S> ws; hashmap<string, S> ms; void f() { @ } };", "o?.set();", 'M m = {};\nm.o = S{};\nm.f();\nprintln("{}", m.o?.x);', "7"),
    ("struct M { S? o; vector<int>? ov; vector<S> ws; hashmap<string, S> ms; void f() { @ } };", "o.set();", 'M m = {};\nm.o = S{};\nm.f();\nprintln("{}", m.o?.x);', "7"),
    ("struct M { S? o; vector<int>? ov; vector<S> ws; hashmap<string, S> ms; void f() { @ } };", "ov.push_back(N);", 'M m = {};\nm.ov = vector<int>{1};\nm.f();\nprintln("{}", m.ov);', "[1, 5]"),
    ("struct M { S? o; vector<int>? ov; vector<S> ws; hashmap<string, S> ms; void f() { @ } };", "ws.back().set();", 'M m = {};\nm.ws.push_back(S{});\nm.f();\nprintln("{}", m.ws[0].x);', "7"),
    ("struct M { S? o; vector<int>? ov; vector<S> ws; hashmap<string, S> ms; void f() { @ } };", 'ms.at("a").set();', 'M m = {};\nm.ms["a"] = S{};\nm.f();\nprintln("{}", m.ms["a"].x);', "7"),
    ("struct M { S? o; vector<int>? ov; vector<S> ws; hashmap<string, S> ms; void f() { @ } };", 'ms.get("a")?.set();', 'M m = {};\nm.ms["a"] = S{};\nm.f();\nprintln("{}", m.ms["a"].x);', "7"),
    ("class K { public: S? o; vector<S> ws; void f() { @ } };", "this.o?.set();", 'K k = K();\nk.o = S{};\nk.f();\nprintln("{}", k.o?.x);', "7"),
    ("class K { public: S? o; vector<S> ws; void f() { @ } };", "o?.set();", 'K k = K();\nk.o = S{};\nk.f();\nprintln("{}", k.o?.x);', "7"),
    ("class K { public: S? o; vector<S> ws; void f() { @ } };", "this.ws.back().set();", 'K k = K();\nk.ws.push_back(S{});\nk.f();\nprintln("{}", k.ws[0].x);', "7"),
    # Names the program has in a field, a parameter, or a loop
    ("struct M { vector<int>? ov; int v = 5; void f() { @ } };", "ov.push_back(v);", 'M m = {};\nm.ov = vector<int>{1};\nm.f();\nprintln("{}", m.ov);', "[1, 5]"),
    ("void f(vector<int>?* p, int v) { @ }", "p->push_back(v);", 'vector<int>? o = vector<int>{1};\nf(&o, 5);\nprintln("{}", o);', "[1, 5]"),
    ("void f(vector<int>?* p) { vector<int> list = {5}; for (auto v : list) { @ } }", "p->push_back(v);", 'vector<int>? o = vector<int>{1};\nf(&o);\nprintln("{}", o);', "[1, 5]"),
    ("void f(vector<int>?* p) { if (true) { int inner = 5; int v = 5; @ } }", "p->push_back(inner);", 'vector<int>? o = vector<int>{1};\nf(&o);\nprintln("{}", o);', "[1, 5]"),
    # When there is nothing there, what `?.` did was nothing
    ("void f(vector<unique<C>>?* p) { @ }", "(*p)?.clear();", 'vector<unique<C>>? u = null;\nf(&u);\nprintln("{}", u.has_value());', "false"),
    ("void f(vector<int>?* p) { @ }", "(*p)?.clear();", 'vector<int>? o = null;\nf(&o);\nprintln("{}", o.has_value());', "false"),
]

# What is not wrong, and must stay that way: through a pointer, or a class, a method that gave a copy reaches the real thing.
VALID = [  # (definitions, main, expected)
    ("", "S a = {};\nvector<S*> ps = {&a};\nps.back()[0].set();\nprintln(\"{}\", a.x);", "7"),
    ("", "S a = {};\nvector<S*> ps = {&a};\nps.back()->set();\nprintln(\"{}\", a.x);", "7"),
    ("struct WithPointer { S* p; };", "S a = {};\nvector<WithPointer> v = {WithPointer{.p = &a}};\nv.back().p[0].set();\nprintln(\"{}\", a.x);", "7"),
    ("", "S a = {};\nhashmap<string, S*> m = {{\"k\", &a}};\nm.at(\"k\")[0].set();\nprintln(\"{}\", a.x);", "7"),
    ("class Box { public: S s; void set() { s.x = 7; } };", "vector<Box> v = {Box()};\nv.back().set();\nprintln(\"{}\", v[0].s.x);", "7"),
    ("class Box { public: S s; };", "vector<Box> v = {Box()};\nv.back().s.set();\nprintln(\"{}\", v[0].s.x);", "7"),
    ("class Box { public: S s; };\nstruct InABox { Box b; };", "vector<InABox> v = {InABox{.b = Box()}};\nv.back().b.s.set();\nprintln(\"{}\", v[0].b.s.x);", "7"),
    ("struct Reader { int x = 3; int area() const { return x * x; } };", "vector<Reader> v = {Reader{}};\nprintln(\"{}\", v.back().area());", "9"),
    ("", "S a = {};\nH h = {};\nh.os = S{};\nH* const p = &h;\nif (auto v = p[0].os) { v.set(); p[0].os = v; }\nprintln(\"{}\", p[0].os?.x);", "7"),
]

CONST = [
    ('const hashmap<string, vector<int>> m = {};', 'm.at("a").push_back(N);'),
    ('const hashmap<string, vector<int>> m = {};', 'm.get("a").push_back(N);'),
    ('const hashmap<string, S> m = {};', 'm.at("a").x = N;'),
    ('const hashmap<string, S> m = {};', 'm.get("a")?.set();'),
    ("const S? o = S{};", "o?.items.clear();"),
    ("const S? o = S{};", "o?.set();"),
    ("const vector<S> w = {S{}};", "w.back().x = N;"),
    ("const vector<S> w = {S{}};", "w.back().set();"),
    ("const O? a = O{.h = H{}};", "a?.h?.items.clear();"),
    ("H h = {};\nconst H* p = &h;", "p->os?.set();"),
    ("vector<S> w = {S{}};\nconst vector<S>* p = &w;", "p->back().set();"),
    ("H h = {};\nconst H* p = &h;", "p->ov.push_back(N);"),
]

# Names of the program's own that what is suggested must not hide.
COLLISIONS = ["", "int v = 5;", "int inner = 5;", "int opt = 5;", "int v = 5;\nint inner = 5;\nint item = 5;\nint held = 5;\nint opt = 5;\nint copy = 5;\nint c = 5;\nint d = 5;"]

CODE = re.compile(r"`([^`]*)`")


def program(body, definitions=""):
    return PRELUDE + definitions + "\nint main() {\n" + body + "\n    return 0;\n}\n"


def is_statement(code):
    return ";" in code or code.startswith("if (")


def compile_errors(cplus, path):
    return subprocess.run([cplus, "check", str(path)], capture_output=True, text=True, timeout=60)


def followed(code, wrong, value):
    """The suggested code, with what it left out filled in from the line that was wrong."""
    args = re.search(r"(?:\.|->)(\w+)\(([^()]*)\);$", wrong)
    code = code.replace("(...)", "(" + (args.group(2) if args else "") + ")")
    code = code.replace("[i]", "[0]").replace(" = ...", " = " + value)
    return code


def run_case(cplus, workdir, index, declared, wrong, kind, printed=None, expected=None, extra="", value="N"):
    """What is wrong with this case, or None."""
    name = "5" if value == "N" else value
    value_text = name
    body = (extra + "\n" if extra else "") + declared + "\n" + wrong.replace("N", value_text) + "\n"
    source = workdir / f"grid{index}.cplus"
    source.write_text(program(body))
    result = compile_errors(cplus, source)
    shown = f"{declared!r} / {wrong!r} with {extra!r}"
    if result.returncode == 0:
        return f"{shown}: no error was reported"
    if "internal" in result.stderr or result.returncode not in (1,):
        return f"{shown}: the compiler stopped with {result.returncode}: {result.stderr[:300]}"
    # The error about this line, which is what the code is for
    helps = re.findall(r"= help: (.*)", result.stderr)
    if not helps:
        return f"{shown}: no help"
    text = helps[0]
    if len(helps) > 1:
        return f"{shown}: more than one error: {result.stderr[:500]}"
    if re.search(r"[\x00-\x08\x0b-\x1f\x7f]", text):
        return f"{shown}: unprintable bytes in the help: {text!r}"
    claims_temporary = "only exists as a copy" in text
    if kind == "const":
        if "const" not in text:
            return f"{shown}: the help does not say what is const: {text}"
        return None
    if kind != "temporary" and claims_temporary:
        return f"{shown}: says this only exists as a copy, but the program can reach it: {text}"
    if kind == "temporary" and not claims_temporary:
        return f"{shown}: does not say this only exists as a copy: {text}"
    # The last piece of code that is a statement, or an assignment with a place to change
    codes = [c for c in CODE.findall(text) if is_statement(c) or " = ..." in c or re.search(r"\.\w+\((\.\.\.)?\)$", c)]
    if not codes:
        if kind == "simple":
            return f"{shown}: no code to follow in: {text}"
        return None
    if kind == "not_simple" and ("[ ]" in text or "temporary copy" in text):
        return None
    code = followed(codes[-1], wrong.replace("N", value_text), value_text)
    if kind == "temporary":  # the variable it says to store it in comes first
        first = [c for c in codes if c.startswith("auto ")]
        code = (first[0] + " " if first else "") + code
    if "..." in code:
        return f"{shown}: left something unsaid after filling in what it left out: {code}"
    fixed = (extra + "\n" if extra else "") + declared + "\n" + code + (";" if not code.rstrip().endswith((";", "}")) else "") + "\n"
    if kind in ("simple", "not_simple") and printed:
        fixed += f'println("{{}}", {printed});\n'
    target = workdir / f"grid{index}_fixed.cplus"
    target.write_text(program(fixed))
    run = subprocess.run([cplus, "run", str(target)], capture_output=True, text=True, timeout=120)
    if run.returncode != 0:
        first = run.stderr.split("\n\n")[0]
        return f"{shown}: following `{code}` does not work:\n{first[:500]}\n(help: {text})"
    if expected is not None and run.stdout.strip() != expected:
        return f"{shown}: following `{code}` printed {run.stdout.strip()!r}, and it should be {expected!r}"
    return None


def run_elsewhere(cplus, workdir, index, definitions, wrong, body, expected):
    """A case outside main: the line goes in the definitions, and main shows what it did."""
    wrong = wrong.replace("N", "5")
    shown = f"{definitions!r} / {wrong!r}"
    source = workdir / f"else{index}.cplus"
    source.write_text(program(body, "\n" + definitions.replace("@", wrong) + "\n"))
    result = compile_errors(cplus, source)
    if result.returncode != 1:
        return f"{shown}: the compiler gave {result.returncode}: {result.stderr[:300]}"
    helps = re.findall(r"= help: (.*)", result.stderr)
    if len(helps) != 1:
        return f"{shown}: expected one error: {result.stderr[:500]}"
    text = helps[0]
    if "only exists as a copy" in text:
        return f"{shown}: says this only exists as a copy: {text}"
    codes = [c for c in CODE.findall(text) if is_statement(c) or " = ..." in c or re.search(r"\.\w+\((\.\.\.)?\)$", c)]
    if not codes:
        return f"{shown}: no code to follow in: {text}"
    value = re.search(r"\(([^()]*)\);$", wrong)
    code = followed(codes[-1], wrong, value.group(1) if value and value.group(1) else "5")
    if "..." in code:
        return f"{shown}: left something unsaid: {code}"
    code += ";" if not code.rstrip().endswith((";", "}")) else ""
    target = workdir / f"else{index}_fixed.cplus"
    target.write_text(program(body, "\n" + definitions.replace("@", code) + "\n"))
    run = subprocess.run([cplus, "run", str(target)], capture_output=True, text=True, timeout=120)
    if run.returncode != 0:
        return f"{shown}: following `{code}` does not work:\n{run.stderr.split(chr(10) + chr(10))[0][:500]}\n(help: {text})"
    if run.stdout.strip() != expected:
        return f"{shown}: following `{code}` printed {run.stdout.strip()!r}, and it should be {expected!r}"
    return None


def run_valid(cplus, workdir, index, definitions, body, expected):
    """A program that is not wrong, which must compile and do what it says."""
    target = workdir / f"valid{index}.cplus"
    target.write_text(program(body, "\n" + definitions + "\n"))
    run = subprocess.run([cplus, "run", str(target)], capture_output=True, text=True, timeout=120)
    if run.returncode != 0 or run.stdout.strip() != expected:
        return f"valid program {body!r} gave {run.returncode}, {run.stdout.strip()!r}: {run.stderr[:400]}"
    return None


def problems(cplus, workdir):
    cases = []
    seen = set()
    for extra in COLLISIONS:
        for declared, wrong, printed, expected in SAME:
            if (declared, wrong, extra) not in seen:
                seen.add((declared, wrong, extra))
                cases.append((declared, wrong, "simple", printed, expected, extra))
        for declared, wrong in TEMPORARY:
            cases.append((declared, wrong, "temporary", None, None, extra))
    for declared, wrong in NOT_SIMPLE:
        cases.append((declared, wrong, "not_simple", None, None, ""))
    for declared, wrong in CONST:
        cases.append((declared, wrong, "const", None, None, ""))

    def one(item):
        index, (declared, wrong, kind, printed, expected, extra) = item
        # With a variable as the value, the suggestion must leave that variable alone
        value = "N"
        if extra.startswith("int v"):
            value = "v"
        elif extra.startswith("int inner"):
            value = "inner"
        return run_case(cplus, workdir, index, declared, wrong, kind, printed, expected, extra, value)

    with ThreadPoolExecutor(max_workers=8) as pool:
        found = [p for p in pool.map(one, enumerate(cases)) if p]
        found += [p for p in pool.map(lambda c: run_elsewhere(cplus, workdir, c[0], *c[1]), enumerate(ELSEWHERE)) if p]
        found += [p for p in pool.map(lambda c: run_valid(cplus, workdir, c[0], *c[1]), enumerate(VALID)) if p]
    return len(cases) + len(ELSEWHERE) + len(VALID), found


def main():
    cplus = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory() as tmp:
        count, found = problems(cplus, Path(tmp))
    for p in found:
        print(p)
        print()
    print(f"{count} cases, {len(found)} problems")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
