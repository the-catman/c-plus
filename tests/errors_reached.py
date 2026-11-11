#!/usr/bin/env python3
"""Lists the errors of the compiler that no test reaches, and the advice that no test gets to.

Lines are not enough for this, since `if (bad) fail(...);` runs its line whether or not it fails, so it goes by code regions.

  cmake -S . -B build-cov -G Ninja -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
  ninja -C build-cov
  LLVM_PROFILE_FILE="$PWD/build-cov/prof/%p.profraw" python3 tests/run_tests.py build-cov/cplus
  llvm-profdata merge -sparse build-cov/prof/*.profraw -o build-cov/cov.profdata
  llvm-cov export build-cov/cplus -instr-profile=build-cov/cov.profdata -format=text compiler/*.cpp > build-cov/cov.json
  python3 tests/errors_reached.py build-cov/cov.json

Run it from the root of the repository.
"""
import bisect
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPORTS = re.compile(r"(?<![\w.:])(diags_\.error|diags\.error|fail|notYet|toolError|usageError|notImported|out\.errors\.push_back)\(")
ADVICE = re.compile(r"\bhelp\s*\+?=\s")


def main():
    segments = {}
    for file in json.load(open(sys.argv[1]))["data"][0]["files"]:
        segments[pathlib.Path(file["filename"]).stem] = [(s[0], s[1], s[2], s[3]) for s in file["segments"]]

    def unreached(pattern):
        total = 0
        found = []
        for name in ["Lexer", "Parser", "Sema", "ConstEval", "main", "Explain", "CHeaders", "Modules"]:
            source = (ROOT / "compiler" / f"{name}.cpp").read_text()
            lines = source.split("\n")
            regions = segments.get(name, [])
            places = [(line, column) for line, column, *_ in regions]
            for match in pattern.finditer(source):
                before = source[max(0, match.start() - 20):match.start()]
                if "Parser::" in before or "void " in before or "static" in before:
                    continue  # where they are defined
                total += 1
                line = source.count("\n", 0, match.start()) + 1
                column = match.start() - source.rfind("\n", 0, match.start())
                at = bisect.bisect_right(places, (line, column)) - 1
                while at >= 0 and not regions[at][3]:
                    at -= 1
                if at >= 0 and regions[at][2] == 0:
                    found.append(f"  compiler/{name}.cpp:{line}: {lines[line - 1].strip()[:140]}")
        return total, found

    for what, pattern in (("errors", REPORTS), ("pieces of advice", ADVICE)):
        total, found = unreached(pattern)
        print(f"{total} {what}, {len(found)} that no test reaches")
        print("\n".join(found))


if __name__ == "__main__":
    main()
