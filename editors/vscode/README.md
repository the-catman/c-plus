# C+ for VS Code

Colors for C+ code, and, from the language server that is part of the compiler, `cplus lsp`:

- errors as you type, with their help, as `cplus check` gives them
- the type of a name, and the comment above its declaration, when you hover over it
- go to definition (F12)
- formatting on save, in the style of `cplus fmt`

## Installing it

Build the compiler first, as the README at the root of the repository says. Then, in this folder:

```
npm install
npx @vscode/vsce package --skip-license --allow-missing-repository
code --install-extension cplus-0.0.1.vsix
```

If `cplus` is not on your `PATH`, set `cplus.path` in the settings to the full path of the compiler, such as the
`build/cplus` of your checkout. After the compiler is built again, the command "C+: Restart the language server" starts the
new one.
