// Starts `cplus lsp` for C+ files. The errors as you type, hover, go to definition and formatting all come from it; this
// only connects it to VS Code. The colors come from syntaxes/cplus.tmLanguage.json.
const vscode = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

async function start() {
  const command = vscode.workspace.getConfiguration("cplus").get("path") || "cplus";
  client = new LanguageClient(
    "cplus",
    "C+",
    { command, args: ["lsp"] },
    { documentSelector: [{ scheme: "file", language: "cplus" }, { scheme: "untitled", language: "cplus" }] },
  );
  try {
    await client.start();
  } catch (error) {
    client = undefined;
    vscode.window.showErrorMessage(
      `C+: could not start \`${command} lsp\` (${error.message}). Build the compiler, then set "cplus.path" in the settings to where it is, ` +
        "such as the build/cplus of a checkout of C+.",
    );
  }
}

async function restart() {
  if (client) await client.stop();
  await start();
}

function activate(context) {
  start();
  context.subscriptions.push(
    vscode.commands.registerCommand("cplus.restart", restart),
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (event.affectsConfiguration("cplus.path")) restart();
    }),
  );
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
