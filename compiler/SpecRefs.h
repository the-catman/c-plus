// Anchors into docs/SPEC.md that diagnostics cite. Each value must match a heading
// in the spec (GitHub-style slug). tests/run_tests.py verifies that they all exist.
#pragma once

namespace cplus::spec {

inline constexpr const char* Preprocessor = "no-preprocessor";
inline constexpr const char* SourceFiles = "source-files";
inline constexpr const char* Grammar = "grammar";
inline constexpr const char* Comments = "comments";
inline constexpr const char* IntegerLiterals = "integer-literals";
inline constexpr const char* StringLiterals = "string-literals";
inline constexpr const char* CharacterLiterals = "character-literals";
inline constexpr const char* Strings = "string";
inline constexpr const char* Char = "char";
inline constexpr const char* IntegerTypes = "integer-types";
inline constexpr const char* FloatingPointTypes = "floating-point-types";
inline constexpr const char* Casts = "casts";
inline constexpr const char* Math = "math-functions";
inline constexpr const char* ImplicitConversions = "implicit-conversions";
inline constexpr const char* Optionals = "optionals";
inline constexpr const char* Variables = "variables";
inline constexpr const char* Constants = "constants";
inline constexpr const char* DefiniteAssignment = "definite-assignment";
inline constexpr const char* Functions = "functions";
inline constexpr const char* MainFunction = "the-main-function";
inline constexpr const char* CFunctions = "c-functions";
inline constexpr const char* Conditions = "conditions";
inline constexpr const char* ControlFlow = "control-flow";
inline constexpr const char* NestingLimits = "nesting-limits";
inline constexpr const char* HashMaps = "hash-maps";
inline constexpr const char* Json = "json";
inline constexpr const char* Operators = "operators";
inline constexpr const char* IntegerOverflow = "integer-overflow";
inline constexpr const char* Structs = "structs";
inline constexpr const char* Classes = "classes";
inline constexpr const char* Inheritance = "inheritance";
inline constexpr const char* Interfaces = "interfaces";
inline constexpr const char* Weak = "weak-references";
inline constexpr const char* Unique = "unique-references";
inline constexpr const char* Local = "local-references";
inline constexpr const char* Errors = "errors";
inline constexpr const char* Constexpr = "constexpr";
inline constexpr const char* Generics = "generics";
inline constexpr const char* Constraints = "constraints";
inline constexpr const char* Methods = "methods";
inline constexpr const char* Vectors = "vectors";
inline constexpr const char* Arrays = "arrays";
inline constexpr const char* RangeFor = "range-based-for-loops";
inline constexpr const char* Printing = "printing";
inline constexpr const char* Pointers = "pointers";
inline constexpr const char* PointerDeclarations = "pointer-declarations";
inline constexpr const char* Sizeof = "sizeof";
inline constexpr const char* CTypes = "c-types-known-by-name";
inline constexpr const char* CStructs = "c-structs";
inline constexpr const char* NewDelete = "new-and-delete";
inline constexpr const char* Modules = "modules";
inline constexpr const char* Http = "http";
inline constexpr const char* Concurrency = "concurrency";
inline constexpr const char* Reflection = "reflection";
inline constexpr const char* Mutex = "mutex";
inline constexpr const char* Status = "implementation-status";

}  // namespace cplus::spec
