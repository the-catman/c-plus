// `cplus lsp`: the Language Server Protocol, on stdin and stdout, for the errors, hovers, definitions and formatting of the
// files an editor has open.
//
// Each version of a file is checked in a child process of its own. The compiler keeps some of what it makes for as long as it
// runs, which is right for one compile, and a server runs for hours. It also sees half-written code all day, and a crash in
// the compiler then costs one check and not the server. What the child found comes back as JSON on a pipe.

#include "Lsp.h"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Diagnostics.h"
#include "Format.h"
#include "Index.h"
#include "Lexer.h"
#include "Parser.h"
#include "Sema.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

// In a compiler built to measure coverage, what a child ran is written out by this, as _exit skips it.
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));

namespace cplus {

namespace {

namespace json = llvm::json;

// ---- Positions ---------------------------------------------------------------
// The compiler counts columns in bytes. LSP counts them in UTF-16 code units, unless the editor can count bytes too.

enum class Units { Utf16, Utf8 };

size_t charLength(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

int64_t toUnits(std::string_view line, size_t bytes, Units units) {
    bytes = std::min(bytes, line.size());
    if (units == Units::Utf8) return (int64_t)bytes;
    int64_t n = 0;
    for (size_t i = 0; i < bytes;) {
        const size_t len = charLength((unsigned char)line[i]);
        n += len == 4 ? 2 : 1;
        i += len;
    }
    return n;
}

size_t toBytes(std::string_view line, int64_t count, Units units) {
    if (count <= 0) return 0;
    if (units == Units::Utf8) return std::min((size_t)count, line.size());
    int64_t n = 0;
    size_t i = 0;
    while (i < line.size() && n < count) {
        const size_t len = charLength((unsigned char)line[i]);
        n += len == 4 ? 2 : 1;
        i += len;
    }
    return std::min(i, line.size());
}

json::Value position(int64_t line, int64_t character) { return json::Object{{"line", line}, {"character", character}}; }

json::Value range(const SourceFile& file, Loc loc, Units units) {
    if (loc.line == 0 || loc.line > file.lineStarts.size()) return json::Object{{"start", position(0, 0)}, {"end", position(0, 0)}};
    std::string_view line = file.line(loc.line);
    const size_t start = loc.col ? loc.col - 1 : 0;
    const size_t end = start + std::max<uint32_t>(loc.len, 1);
    return json::Object{{"start", position(loc.line - 1, toUnits(line, start, units))}, {"end", position(loc.line - 1, toUnits(line, end, units))}};
}

// Where a position of the editor's is in `text`, as a byte offset.
size_t offsetOf(const std::string& text, int64_t line, int64_t character, Units units) {
    size_t start = 0;
    for (int64_t n = 0; n < line; ++n) {
        const size_t next = text.find('\n', start);
        if (next == std::string::npos) return text.size();
        start = next + 1;
    }
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    return start + toBytes(std::string_view(text).substr(start, end - start), character, units);
}

// The position just after the end of `text`.
json::Value endOf(const std::string& text, Units units) {
    const int64_t lines = std::count(text.begin(), text.end(), '\n');
    const size_t last = text.rfind('\n');
    const size_t start = last == std::string::npos ? 0 : last + 1;
    return position(lines, toUnits(std::string_view(text).substr(start), text.size() - start, units));
}

// ---- What a check finds, in the child -----------------------------------------

std::string serialize(const json::Value& value) {
    std::string out;
    llvm::raw_string_ostream os(out);
    os << value;
    os.flush();
    return out;
}

std::string checkText(const std::string& uri, const std::string& text, Units units) {
    if (std::getenv("CPLUS_LSP_CRASH_FOR_TESTS")) std::abort();  // so the tests reach what a crash in the compiler does
    SourceFile file(uri, text);
    Diagnostics diags(file);
    std::vector<Message> messages;
    diags.collectInto(&messages);
    Lexer lexer(file, diags);
    Parser parser(lexer.lexAll(), diags);
    Program program;
    parser.parseProgram(program);
    const bool checked = !parser.gaveUp();  // otherwise what was read is too little to check, as for `cplus check`
    if (checked) {
        Sema sema(diags, parser, true);
        sema.check(program);
    }

    json::Array diagnostics;
    for (const Message& m : messages) {
        if (m.level == "note") {  // more about the one before
            if (diagnostics.empty()) continue;
            json::Object& last = *diagnostics.back().getAsObject();
            if (m.loc.line) last.getArray("relatedInformation")->push_back(json::Object{
                    {"location", json::Object{{"uri", uri}, {"range", range(file, m.loc, units)}}}, {"message", m.message}});
            else last["message"] = last.getString("message")->str() + "\nnote: " + m.message;
            continue;
        }
        std::string message = m.message;
        if (!m.help.empty()) message += "\nhelp: " + m.help;
        if (!m.spec.empty()) message += "\nspec: docs/SPEC.md#" + m.spec + ", or run `cplus explain " + m.spec + "`";
        json::Object d{{"range", range(file, m.loc, units)}, {"severity", m.level == "error" ? 1 : 2}, {"source", "cplus"},
                       {"message", message}, {"relatedInformation", json::Array{}}};
        if (!m.spec.empty()) d["code"] = m.spec;
        diagnostics.push_back(std::move(d));
    }
    json::Array references;
    if (checked)
        for (const Reference& r : indexProgram(program, file)) {
            json::Object o{{"range", range(file, r.at, units)}, {"what", r.what}};
            if (r.declared.line) o["declared"] = range(file, r.declared, units);
            references.push_back(std::move(o));
        }
    return serialize(json::Object{{"diagnostics", std::move(diagnostics)}, {"references", std::move(references)}});
}

std::string formatText(const std::string& uri, const std::string& text) {
    SourceFile file(uri, text);
    Diagnostics diags(file);
    std::vector<Message> messages;
    diags.collectInto(&messages);
    std::string out;
    switch (formatSource(file, diags, out)) {
    case FormatResult::Formatted: return serialize(json::Object{{"result", "formatted"}, {"text", out}});
    case FormatResult::HasErrors: return serialize(json::Object{{"result", "errors"}});
    case FormatResult::WouldChange: break;
    }
    return serialize(json::Object{{"result", "bug"}});
}

// Runs `work` in a child process, and gives what it returned, or nothing, with `why` saying what went wrong.
std::optional<std::string> isolated(const std::function<std::string()>& work, std::string& why) {
    int fds[2];
    if (pipe(fds) != 0) {
        why = std::string("could not make a pipe: ") + std::strerror(errno);
        return std::nullopt;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) {
        why = std::string("could not start a process: ") + std::strerror(errno);
        close(fds[0]);
        close(fds[1]);
        return std::nullopt;
    }
    if (pid == 0) {
        close(fds[0]);
        const std::string out = work();
        const char* p = out.data();
        size_t left = out.size();
        while (left > 0) {
            const ssize_t n = write(fds[1], p, left);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) _exit(2);
            p += n;
            left -= (size_t)n;
        }
        if (__llvm_profile_write_file) __llvm_profile_write_file();
        _exit(0);  // and not exit(), which would write out the server's buffers a second time
    }
    close(fds[1]);
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool late = false;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            late = true;
            kill(pid, SIGKILL);
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int ready = poll(&p, 1, (int)left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;
        char buffer[65536];
        const ssize_t n = read(fds[0], buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(buffer, (size_t)n);
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (late) why = "took longer than 30 seconds";
    else if (WIFSIGNALED(status)) why = std::string("stopped with the signal ") + strsignal(WTERMSIG(status));
    else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) why = "exited with " + std::to_string(WEXITSTATUS(status));
    else return out;
    return std::nullopt;
}

// What a child gave, as JSON.
std::optional<json::Value> readJson(const std::optional<std::string>& text) {
    if (!text) return std::nullopt;
    llvm::Expected<json::Value> parsed = json::parse(*text);
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        return std::nullopt;
    }
    return std::move(*parsed);
}

// ---- Messages ----------------------------------------------------------------

// Reads the messages an editor sends: a `Content-Length` header, an empty line, and that many bytes of JSON.
class Input {
public:
    // Whether a message can be read without waiting, or the input has ended.
    bool ready() {
        size_t start, length;
        if (eof_ || whole(start, length)) return true;
        pollfd p{STDIN_FILENO, POLLIN, 0};
        return poll(&p, 1, 0) > 0;
    }

    // The next message's JSON, or nothing once the input has ended.
    std::optional<std::string> next() {
        for (;;) {
            size_t start, length;
            if (whole(start, length)) {
                std::string body = buffer_.substr(start, length);
                buffer_.erase(0, start + length);
                return body;
            }
            if (eof_) return std::nullopt;
            char chunk[65536];
            const ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) eof_ = true;
            else buffer_.append(chunk, (size_t)n);
        }
    }

private:
    std::string buffer_;
    bool eof_ = false;

    bool whole(size_t& start, size_t& length) {
        for (;;) {
            const size_t end = buffer_.find("\r\n\r\n");
            if (end == std::string::npos) return false;
            std::optional<size_t> declared;
            size_t at = 0;
            while (at < end) {
                size_t lineEnd = buffer_.find("\r\n", at);
                if (lineEnd == std::string::npos || lineEnd > end) lineEnd = end;
                std::string header = buffer_.substr(at, lineEnd - at);
                const size_t colon = header.find(':');
                if (colon != std::string::npos) {
                    std::string name = header.substr(0, colon);
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    if (name == "content-length") declared = std::strtoull(header.c_str() + colon + 1, nullptr, 10);
                }
                at = lineEnd + 2;
            }
            if (!declared) {  // no message to be read here: what is left may still be one
                std::fprintf(stderr, "cplus lsp: a message had no Content-Length header, so it was left out\n");
                buffer_.erase(0, end + 4);
                continue;
            }
            start = end + 4;
            length = *declared;
            return buffer_.size() >= start + length;
        }
    }
};

// ---- The server ----------------------------------------------------------------

struct Ref {
    int64_t line = 0, start = 0, end = 0;
    std::string what;
    std::optional<json::Value> declared;
};

struct Document {
    std::string text;
    int64_t version = 0;
    bool dirty = true;
    std::vector<Ref> refs;
};

class Server {
public:
    explicit Server(const char* version) : version_(version) {}

    int run() {
        for (;;) {
            if (!input_.ready()) checkAll();  // what changed is checked once the editor has stopped sending
            std::optional<std::string> body = input_.next();
            if (!body) return shutdown_ ? 0 : 1;
            llvm::Expected<json::Value> parsed = json::parse(*body);
            if (!parsed) {
                const std::string problem = llvm::toString(parsed.takeError());
                replyError(nullptr, -32700, "the message is not JSON: " + problem);
                continue;
            }
            const json::Object* message = parsed->getAsObject();
            if (!message) {
                replyError(nullptr, -32600, "a message must be a JSON object");
                continue;
            }
            const std::optional<llvm::StringRef> method = message->getString("method");
            const json::Value* id = message->get("id");
            if (!method) continue;  // an answer, to a request this server never sends
            static const json::Object none;
            const json::Object* params = message->getObject("params");
            if (!params) params = &none;
            if (*method == "exit") return shutdown_ ? 0 : 1;
            if (id) request(method->str(), *id, *params);
            else notification(method->str(), *params);
        }
    }

private:
    const char* version_;
    Input input_;
    Units units_ = Units::Utf16;
    bool initialized_ = false;
    bool shutdown_ = false;
    std::map<std::string, Document> documents_;

    void send(json::Object message) {
        message["jsonrpc"] = "2.0";
        const std::string body = serialize(json::Value(std::move(message)));
        std::printf("Content-Length: %zu\r\n\r\n", body.size());
        std::fwrite(body.data(), 1, body.size(), stdout);
        std::fflush(stdout);
    }
    void reply(const json::Value& id, json::Value result) { send(json::Object{{"id", id}, {"result", std::move(result)}}); }
    void replyError(const json::Value& id, int code, const std::string& text) {
        send(json::Object{{"id", id}, {"error", json::Object{{"code", code}, {"message", text}}}});
    }
    void notify(const std::string& method, json::Value params) { send(json::Object{{"method", method}, {"params", std::move(params)}}); }

    static std::string uriOf(const json::Object& params) {
        const json::Object* doc = params.getObject("textDocument");
        std::optional<llvm::StringRef> uri = doc ? doc->getString("uri") : std::nullopt;
        return uri ? uri->str() : "";
    }

    void request(const std::string& method, const json::Value& id, const json::Object& params) {
        if (method == "initialize") {
            if (const json::Object* caps = params.getObject("capabilities"))
                if (const json::Object* general = caps->getObject("general"))
                    if (const json::Array* encodings = general->getArray("positionEncodings"))
                        for (const json::Value& e : *encodings)
                            if (e.getAsString() && *e.getAsString() == "utf-8") units_ = Units::Utf8;
            initialized_ = true;
            json::Object capabilities{
                {"positionEncoding", units_ == Units::Utf8 ? "utf-8" : "utf-16"},
                {"textDocumentSync", json::Object{{"openClose", true}, {"change", 1}}},
                {"hoverProvider", true},
                {"definitionProvider", true},
                {"documentFormattingProvider", true},
            };
            reply(id, json::Object{{"capabilities", std::move(capabilities)}, {"serverInfo", json::Object{{"name", "cplus"}, {"version", version_}}}});
            return;
        }
        if (!initialized_) return replyError(id, -32002, "the server has not been initialized: send `initialize` first");
        if (shutdown_) return replyError(id, -32600, "the server is shutting down, after `shutdown`: send `exit`");
        if (method == "shutdown") {
            shutdown_ = true;
            return reply(id, nullptr);
        }
        if (method == "textDocument/hover" || method == "textDocument/definition") {
            const Ref* ref = refAt(params);
            if (!ref) return reply(id, nullptr);
            if (method == "textDocument/hover") {
                json::Object range{{"start", position(ref->line, ref->start)}, {"end", position(ref->line, ref->end)}};
                return reply(id, json::Object{{"contents", json::Object{{"kind", "markdown"}, {"value", ref->what}}}, {"range", std::move(range)}});
            }
            if (!ref->declared) return reply(id, nullptr);
            return reply(id, json::Object{{"uri", uriOf(params)}, {"range", *ref->declared}});
        }
        if (method == "textDocument/formatting") return formatting(id, params);
        replyError(id, -32601, "cplus lsp does not have the method `" + method + "`");
    }

    void notification(const std::string& method, const json::Object& params) {
        if (!initialized_ || shutdown_) return;
        const std::string uri = uriOf(params);
        if (method == "textDocument/didOpen") {
            const json::Object* doc = params.getObject("textDocument");
            Document& d = documents_[uri];
            d.text = doc && doc->getString("text") ? doc->getString("text")->str() : "";
            d.version = doc && doc->getInteger("version") ? *doc->getInteger("version") : 0;
            d.dirty = true;
        } else if (method == "textDocument/didChange") {
            auto it = documents_.find(uri);
            if (it == documents_.end()) return;
            Document& d = it->second;
            if (const json::Object* doc = params.getObject("textDocument"))
                if (doc->getInteger("version")) d.version = *doc->getInteger("version");
            if (const json::Array* changes = params.getArray("contentChanges"))
                for (const json::Value& change : *changes) {
                    const json::Object* c = change.getAsObject();
                    if (!c || !c->getString("text")) continue;
                    const std::string text = c->getString("text")->str();
                    const json::Object* r = c->getObject("range");
                    if (!r) {
                        d.text = text;
                        continue;
                    }
                    auto at = [&](const char* which) {
                        const json::Object* p = r->getObject(which);
                        return p ? offsetOf(d.text, p->getInteger("line").value_or(0), p->getInteger("character").value_or(0), units_) : 0;
                    };
                    const size_t start = at("start"), end = std::max(at("end"), start);
                    d.text.replace(start, end - start, text);
                }
            d.dirty = true;
        } else if (method == "textDocument/didClose") {
            if (documents_.erase(uri)) notify("textDocument/publishDiagnostics", json::Object{{"uri", uri}, {"diagnostics", json::Array{}}});
        }
    }

    void checkAll() {
        for (auto& [uri, d] : documents_)
            if (d.dirty) check(uri, d);
    }

    void check(const std::string& uri, Document& d) {
        d.dirty = false;
        d.refs.clear();
        std::string why;
        const Units units = units_;
        std::optional<std::string> found = isolated([&] { return checkText(uri, d.text, units); }, why);
        json::Array diagnostics;
        const std::optional<json::Value> parsed = readJson(found);
        const json::Object* result = parsed ? parsed->getAsObject() : nullptr;
        if (result && result->getArray("diagnostics")) {
            diagnostics = *result->getArray("diagnostics");
            if (const json::Array* refs = result->getArray("references"))
                for (const json::Value& v : *refs) {
                    const json::Object* o = v.getAsObject();
                    const json::Object* r = o ? o->getObject("range") : nullptr;
                    if (!r || !r->getObject("start") || !r->getObject("end") || !o->getString("what")) continue;
                    Ref ref;
                    ref.line = r->getObject("start")->getInteger("line").value_or(0);
                    ref.start = r->getObject("start")->getInteger("character").value_or(0);
                    ref.end = r->getObject("end")->getInteger("character").value_or(0);
                    ref.what = o->getString("what")->str();
                    if (const json::Value* declared = o->get("declared")) ref.declared = *declared;
                    d.refs.push_back(std::move(ref));
                }
        } else {
            if (why.empty()) why = "gave an answer that could not be read";
            diagnostics.push_back(json::Object{
                {"range", json::Object{{"start", position(0, 0)}, {"end", position(0, 0)}}},
                {"severity", 1},
                {"source", "cplus"},
                {"message", "cplus could not check this file, because the compiler " + why +
                                ". This is a mistake in the compiler, and not in the program.\nhelp: keep the file as it is now, to report it, "
                                "and run `cplus check` on it to see what the compiler does"},
            });
        }
        notify("textDocument/publishDiagnostics", json::Object{{"uri", uri}, {"version", d.version}, {"diagnostics", std::move(diagnostics)}});
    }

    Document* document(const json::Object& params) {
        auto it = documents_.find(uriOf(params));
        if (it == documents_.end()) return nullptr;
        if (it->second.dirty) check(it->first, it->second);
        return &it->second;
    }

    // The narrowest name at the position the request asks about.
    const Ref* refAt(const json::Object& params) {
        Document* d = document(params);
        const json::Object* p = params.getObject("position");
        if (!d || !p) return nullptr;
        const int64_t line = p->getInteger("line").value_or(-1), character = p->getInteger("character").value_or(-1);
        const Ref* best = nullptr;
        for (const Ref& r : d->refs)
            if (r.line == line && r.start <= character && character <= r.end && (!best || r.end - r.start < best->end - best->start)) best = &r;
        return best;
    }

    void formatting(const json::Value& id, const json::Object& params) {
        Document* d = document(params);
        if (!d) return reply(id, nullptr);
        std::string why;
        const std::string uri = uriOf(params);
        std::optional<std::string> found = isolated([&] { return formatText(uri, d->text); }, why);
        const std::optional<json::Value> parsed = readJson(found);
        const json::Object* result = parsed ? parsed->getAsObject() : nullptr;
        const std::optional<llvm::StringRef> kind = result ? result->getString("result") : std::nullopt;
        if (kind && *kind == "errors") return reply(id, json::Array{});  // the errors are shown already, and it is left as it is
        if (kind && *kind == "formatted" && result->getString("text")) {
            const std::string text = result->getString("text")->str();
            if (text == d->text) return reply(id, json::Array{});
            json::Object edit{{"range", json::Object{{"start", position(0, 0)}, {"end", endOf(d->text, units_)}}}, {"newText", text}};
            return reply(id, json::Array{std::move(edit)});
        }
        replyError(id, -32603,
                   kind ? "cplus fmt would have changed the tokens or comments of this file, which is a bug in cplus fmt: keep the file, to report it"
                        : "cplus fmt " + (why.empty() ? std::string("gave an answer that could not be read") : why) +
                              ", which is a mistake in the compiler: keep the file, to report it");
    }
};

}  // namespace

int runLanguageServer(const char* version) {
    signal(SIGPIPE, SIG_IGN);  // an editor that goes away ends the input, which ends the server
    if (isatty(STDIN_FILENO))
        std::fprintf(stderr, "cplus lsp is a language server, which an editor starts and talks to in JSON. It is waiting for that now;\n"
                             "Ctrl-D ends it. For VS Code, editors/vscode in the C+ repository is the extension that starts it.\n");
    Server server(version);
    return server.run();
}

}  // namespace cplus
