# C+

A compiled language shaped like C++, with safer defaults. The compiler uses LLVM 18.

> **Notice:** this project is 100% AI generated, all code and documentation included.

```
cmake -S . -B build -G Ninja && ninja -C build
build/cplus run examples/bank.cplus
```

`import http;` needs the development files of libcurl when the compiler is built, such as `libcurl4-openssl-dev` on Debian or Ubuntu.

`cplus explain <topic>` prints what the language says on a topic, such as `hashmap` or `spawn`, with an example that runs.

`cplus fmt` writes C+ in its one style. For editors, `cplus lsp` is a language server, and [editors/vscode](editors/vscode) is an extension for VS Code.

See [docs/CHEATSHEET.md](docs/CHEATSHEET.md) for the language and [PLAN.md](PLAN.md) for the roadmap.
