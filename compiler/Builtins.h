#pragma once

// What the language offers that is written in C+ itself. The parser adds it after the program, only
// when the program uses it. Its tokens have no location, which is how the compiler knows
// them, and a few things are allowed only here:
//
//   __hash(x)       a number for x, the same for two values that are ==, and never negative
//   __move(x)       as move(x), but x is always left empty, even if it could have been copied
//   __Hashable      the constraint Hashable, which a program's own `Hashable` cannot replace
//   V?              stays V when V can already be empty: a pointer, an optional or a weak reference
//   __zero(T)       a T that is all zero bytes, for what must be filled in before anything can see it
//   __type_of(T)    the descriptor of T, as a void*: see runtime/reflect.h
//   __JsonWritable, __JsonReadable  what can be turned into JSON, and made from it
//
// A module the program imports has names such as `json::parse`. The parser makes each of them one identifier.

namespace cplus {

// Codegen knows that `entries_` and `hashes_` are the first two fields of a hashmap. It finds the entry for
// `m[key]` by calling `__place`, `__place_to_assign` or `__existing`, and it passes zero bytes for
// `nothing`, with nothing able to run before they are replaced. Printing and == are in Codegen too.
inline constexpr const char* kHashmapSource = R"cplus(
template<typename K, typename V> struct hashmap_entry {
    K first;
    V second;
};

template<__Hashable K, typename V> struct hashmap {
private:
    vector<hashmap_entry<K, V>> entries_;  // in the order they were added, erased ones left empty
    vector<long> hashes_;                  // the hash of each entry, or -1 once it is erased
    vector<int> table_;                    // open addressing: an entry's position + 1, 0 if free, -1 if erased
    long live_ = 0;

    long __find(K key, long hash) const {
        if (table_.empty()) return -1;
        long mask = table_.size() - 1;
        long place = hash & mask;
        while (true) {
            int found = table_[place];
            if (found == 0) return -1;
            if (found > 0 && hashes_[found - 1] == hash && entries_[found - 1].first == key) return found - 1;
            place = (place + 1) & mask;
        }
    }

    void __link(long hash, long position) {
        long mask = table_.size() - 1;
        long place = hash & mask;
        while (table_[place] > 0) place = (place + 1) & mask;
        table_[place] = int(position + 1);
    }

    // Drops the erased entries and makes a table with room for `wanted`. An entry counts toward the
    // table being full even when erased, as its place stays taken, so there is always a free place.
    void __rebuild(long wanted) {
        if (live_ != entries_.size()) {
            vector<hashmap_entry<K, V>> kept;
            vector<long> keptHashes;
            for (long i = 0; i < entries_.size(); i++) {
                if (hashes_[i] < 0) continue;
                kept.push_back(__move(entries_[i]));
                keptHashes.push_back(hashes_[i]);
            }
            entries_ = __move(kept);
            hashes_ = __move(keptHashes);
        }
        long size = 8;
        while (size < wanted * 3) size *= 2;
        table_ = vector<int>(size);
        for (long i = 0; i < hashes_.size(); i++) __link(hashes_[i], i);
    }

    long __add(K key, long hash, V value) {
        if ((entries_.size() + 1) * 3 > table_.size() * 2) __rebuild(live_ + 1);
        entries_.push_back(hashmap_entry<K, V>{key, __move(value)});
        hashes_.push_back(hash);
        __link(hash, entries_.size() - 1);
        live_ += 1;
        return entries_.size() - 1;
    }

    long __place(K key) {
        long hash = __hash(key);
        long position = __find(key, hash);
        if (position >= 0) return position;
        V value = V();
        position = __find(key, hash);  // the constructor of V may have added it
        return position >= 0 ? position : __add(key, hash, __move(value));
    }

    long __place_to_assign(K key, V nothing) {
        long hash = __hash(key);
        long position = __find(key, hash);
        return position >= 0 ? position : __add(key, hash, __move(nothing));
    }

    long __existing(K key) const { return __find(key, __hash(key)); }

    // For reading JSON, which has no key or value until it has read them.
    void __put(hashmap_entry<K, V> entry) {
        long hash = __hash(entry.first);
        long position = __find(entry.first, hash);
        if (position < 0) __add(entry.first, hash, __move(entry.second));
        else entries_[position].second = __move(entry.second);
    }

    void __fill(vector<hashmap_entry<K, V>> items) {
        for (long i = 0; i < items.size(); i++) insert(__move(items[i]));
    }

    bool __same(const hashmap<K, V>* other) const {
        if (live_ != other->live_) return false;
        for (long i = 0; i < entries_.size(); i++) {
            if (hashes_[i] < 0) continue;
            long position = other->__find(entries_[i].first, hashes_[i]);
            if (position < 0 || entries_[i].second != other->entries_[position].second) return false;
        }
        return true;
    }

public:
    long size() const { return live_; }
    bool empty() const { return live_ == 0; }
    bool contains(K key) const { return __existing(key) >= 0; }
    long count(K key) const { return __existing(key) >= 0 ? 1 : 0; }

    V? get(K key) const {
        long position = __existing(key);
        if (position < 0) return null;
        return entries_[position].second;
    }

    // Keeps the value that is there, if there is one, as in C++.
    bool insert(hashmap_entry<K, V> entry) {
        long hash = __hash(entry.first);
        if (__find(entry.first, hash) >= 0) return false;
        __add(entry.first, hash, __move(entry.second));
        return true;
    }

    long erase(K key) {
        long hash = __hash(key);
        long position = __find(key, hash);
        if (position < 0) return 0;
        long mask = table_.size() - 1;
        long place = hash & mask;
        while (table_[place] != position + 1) place = (place + 1) & mask;
        table_[place] = -1;
        hashes_[position] = -1;
        // Released when this returns, with the map in order, since a destructor could use the map.
        hashmap_entry<K, V> gone = __move(entries_[position]);
        live_ -= 1;
        if (entries_.size() - live_ > live_ + 8) __rebuild(live_);
        return 1;
    }

    void clear() {
        vector<hashmap_entry<K, V>> gone = __move(entries_);
        hashes_ = {};
        table_ = {};
        live_ = 0;
    }

    vector<hashmap_entry<K, V>> items() const {
        if (live_ == entries_.size()) return entries_;
        vector<hashmap_entry<K, V>> out;
        for (long i = 0; i < entries_.size(); i++)
            if (hashes_[i] >= 0) out.push_back(entries_[i]);
        return out;
    }

    vector<K> keys() const {
        vector<K> out;
        for (long i = 0; i < entries_.size(); i++)
            if (hashes_[i] >= 0) out.push_back(entries_[i].first);
        return out;
    }

    vector<V> values() const {
        vector<V> out;
        for (long i = 0; i < entries_.size(); i++)
            if (hashes_[i] >= 0) out.push_back(entries_[i].second);
        return out;
    }
};
)cplus";

inline constexpr const char* kJsonSource = R"cplus(
extern "C" {
    bool cplus_json_read(void* text, void* type, void* out, void* problem, int* line, int* column);
    void cplus_json_write(void* value, void* type, int indent, void* out);
}

class json::Error : public Error {
public:
    int line;
    int column;
    json::Error(string m, int l, int c) : Error(m), line(l), column(c) {}
};

// A value of any shape. runtime/json.c knows these fields, by their positions, and what the numbers in kind_ mean.
struct json::Value {
private:
    int kind_ = 0;  // 0 null, 1 bool, 2 number, 3 string, 4 array, 5 object
    bool flag_ = false;
    double number_ = 0;
    bool isWhole_ = false;
    long whole_ = 0;
    string text_;
    vector<json::Value> items_;
    hashmap<string, json::Value> fields_;

public:
    bool isNull() const { return kind_ == 0; }
    bool isBool() const { return kind_ == 1; }
    bool isNumber() const { return kind_ == 2; }
    bool isString() const { return kind_ == 3; }
    bool isArray() const { return kind_ == 4; }
    bool isObject() const { return kind_ == 5; }

    bool? boolean() const {
        if (kind_ != 1) return null;
        return flag_;
    }
    double? number() const {
        if (kind_ != 2) return null;
        return number_;
    }
    long? whole() const {
        if (kind_ != 2 || !isWhole_) return null;
        return whole_;
    }
    string? text() const {
        if (kind_ != 3) return null;
        return text_;
    }
    json::Value? get(string key) const {
        if (kind_ != 5) return null;
        return fields_.get(key);
    }
    json::Value? at(long index) const {
        if (kind_ != 4 || index < 0 || index >= items_.size()) return null;
        return items_[index];
    }
    long size() const { return kind_ == 4 ? items_.size() : kind_ == 5 ? fields_.size() : 0; }
    vector<json::Value> items() const { return items_; }
    vector<string> keys() const { return fields_.keys(); }

    void set(string key, json::Value value) {
        if (kind_ != 5) __become(5);
        fields_[key] = value;
    }
    void push(json::Value value) {
        if (kind_ != 4) __become(4);
        items_.push_back(value);
    }

    void __become(int kind) {
        kind_ = kind;
        flag_ = false;
        number_ = 0;
        isWhole_ = false;
        whole_ = 0;
        text_ = "";
        items_ = {};
        fields_ = {};
    }
};

json::Value json::object() {
    json::Value value;
    value.__become(5);
    return value;
}

json::Value json::array() {
    json::Value value;
    value.__become(4);
    return value;
}

template<__JsonWritable T> json::Value json::of(T value) {
    string text;
    cplus_json_write((void*)&value, __type_of(T), 0, (void*)&text);
    json::Value out;
    string problem;
    int line = 0;
    int column = 0;
    cplus_json_read((void*)&text, __type_of(json::Value), (void*)&out, (void*)&problem, &line, &column);
    return out;
}

template<__JsonReadable T> T json::parse(string text) throws {
    T value = __zero(T);
    string problem;
    int line = 0;
    int column = 0;
    if (!cplus_json_read((void*)&text, __type_of(T), (void*)&value, (void*)&problem, &line, &column))
        throw json::Error(problem, line, column);
    return value;
}

template<__JsonWritable T> string json::stringify(T value) {
    string out;
    cplus_json_write((void*)&value, __type_of(T), 0, (void*)&out);
    return out;
}

template<__JsonWritable T> string json::pretty(T value) {
    string out;
    cplus_json_write((void*)&value, __type_of(T), 2, (void*)&out);
    return out;
}
)cplus";

// What `spawn` gives. The thread that runs the call leaves its result here: see genSpawn in Codegen.cpp. `Task<void>` is a
// Task of the empty struct `void`, and `Task<int, throws>` is a `__TaskThrows<int>`: see addBuiltins and instanceName in Parser.cpp.
inline constexpr const char* kTaskSource = R"cplus(
extern "C" {
    void* cplus_task_new();
    void cplus_task_free(void* task);
    void cplus_task_finish(void* task);
    void cplus_task_wait(void* task);
    bool cplus_task_is_done(void* task);
    bool cplus_task_was_waited_for(void* task);
    void cplus_task_lost_error(void* text);
}

struct __Void {};

template<typename T> class Task {
    void* state_;
    T value_ = __zero(T);

    Task() : state_(cplus_task_new()) {}
    void __finish(T value) {
        value_ = move(value);
        cplus_task_finish(state_);
    }

public:
    ~Task() { cplus_task_free(state_); }
    T wait() {
        cplus_task_wait(state_);
        if constexpr (__Copyable<T>) return value_;
        else return __move(value_);
    }
    bool done() const { return cplus_task_is_done(state_); }
};

template<typename T> class __TaskThrows {
    void* state_;
    T value_ = __zero(T);
    Error? error_;

    __TaskThrows() : state_(cplus_task_new()) {}
    void __finish(T value) {
        value_ = move(value);
        cplus_task_finish(state_);
    }
    void __fail(Error error) {
        error_ = error;
        cplus_task_finish(state_);
    }

public:
    ~__TaskThrows() {
        if (auto error = error_) {
            if (!cplus_task_was_waited_for(state_)) {
                string text = format("{}", error);
                cplus_task_lost_error((void*)&text);
            }
        }
        cplus_task_free(state_);
    }
    T wait() throws {
        cplus_task_wait(state_);
        if (auto error = error_) throw error;
        if constexpr (__Copyable<T>) return value_;
        else return __move(value_);
    }
    bool done() const { return cplus_task_is_done(state_); }
};
)cplus";

inline constexpr const char* kMutexSource = R"cplus(
extern "C" {
    void* cplus_mutex_new();
    void cplus_mutex_free(void* mutex);
    void cplus_mutex_lock(void* mutex);
    void cplus_mutex_unlock(void* mutex);
    bool cplus_mutex_try_lock(void* mutex);
}

class Mutex {
    void* mutex_;

public:
    Mutex() : mutex_(cplus_mutex_new()) {}
    ~Mutex() { cplus_mutex_free(mutex_); }
    void lock() { cplus_mutex_lock(mutex_); }
    void unlock() { cplus_mutex_unlock(mutex_); }
    bool try_lock() { return cplus_mutex_try_lock(mutex_); }
};
)cplus";

inline constexpr const char* kLockGuardSource = R"cplus(
class LockGuard {
    Mutex mutex_;

public:
    LockGuard(Mutex mutex) : mutex_(mutex) { mutex_.lock(); }
    ~LockGuard() { mutex_.unlock(); }
};
)cplus";

// The C side of this is runtime/http.c, which says why it is written as it is.
inline constexpr const char* kHttpSource = R"cplus(
extern "C" {
    bool cplus_http_send(void* method, void* url, void* headers, void* body, double timeout, long maxBytes, bool follow, void* caFile,
                         int* status, void* outBody, void* outUrl, void* outHeaders, void* kind, void* problem);
    void* cplus_ws_connect(void* url, void* headers, double timeout, long maxBytes, void* caFile, void* kind, void* problem);
    bool cplus_ws_send(void* socket, void* data, bool binary, double timeout, void* kind, void* problem);
    int cplus_ws_receive(void* socket, void* out, double timeout, void* kind, void* problem);
    void cplus_ws_close(void* socket);
    void cplus_ws_free(void* socket);
    int cplus_ws_close_code(void* socket);
    void cplus_ws_close_reason(void* socket, void* out);
}

// Errors end up in logs, so what one says of a URL leaves out what is most likely to be secret: a name and password, and the query.
string http::__urlForError(string url) {
    long end = url.find("?");
    string out = end >= 0 ? url.substr(0, end) : url;
    end = out.find("#");
    if (end >= 0) out = out.substr(0, end);
    long scheme = out.find("://");
    long start = scheme >= 0 ? scheme + 3 : 0;
    long name = -1;
    for (long i = start; i < out.size() && out[i] != '/'; i++)
        if (out[i] == '@') name = i;
    if (name < 0) return out;
    return out.substr(0, start) + out.substr(name + 1, string::npos);
}

class http::Error : public Error {
public:
    string kind;
    string url;
    http::Error(string m, string k, string u) : Error(m), kind(k), url(http::__urlForError(u)) {}
};

struct http::Header {
    string name;
    string value;
};

struct http::Request {
    string method = "GET";
    string url;
    hashmap<string, string> headers;
    string body;
    double timeout = 30;
    long maxBytes = 1073741824;
    bool followRedirects = true;
    string caFile;
};

string http::__lower(string text) {
    string out;
    for (char c : text) out += tolower(c);
    return out;
}

struct http::Response {
    int status = 0;
    bool ok = false;
    string body;
    string url;
    vector<http::Header> headers;

    // As in fetch: any case of the name, with the values of a header that came more than once joined.
    string? header(string name) const {
        string wanted = http::__lower(name);
        string? found = null;
        for (auto h : headers) {
            if (http::__lower(h.name) == wanted) {
                if (auto before = found) found = before + ", " + h.value;
                else found = h.value;
            }
        }
        return found;
    }
};

struct http::Message {
    string data;
    bool isBinary;
};

// The characters HTTP allows in the name of a method or a header.
bool http::__isToken(string text) {
    const string others = "!#$%&'*+-.^_`|~";
    if (text.empty()) return false;
    for (char c : text)
        if (!isalnum(c) && others.find(c) < 0) return false;
    return true;
}

// `name: value` lines for the runtime, which can then trust them. A line break in a value is what would let text from
// outside add a header, or a whole request, of its own.
string http::__headerLines(http::Request request, bool forWebSocket) throws {
    string lines;
    for (auto e : request.headers) {
        string name = http::__lower(e.first);
        if (!http::__isToken(e.first))
            throw http::Error(format("`{}` cannot be the name of a header: use letters, digits and - only", e.first), "bad-request", request.url);
        for (char c : e.second)
            if (c == '\r' || c == '\n' || c == '\0')
                throw http::Error(format("the value of the header `{}` has a line break or a null byte in it, which could add a header of its own", e.first), "bad-request", request.url);
        if (name == "content-length" || name == "transfer-encoding")
            throw http::Error(format("the header `{}` comes from the body, so it cannot be set", e.first), "bad-request", request.url);
        if (forWebSocket && (name == "host" || name == "upgrade" || name == "connection" || (name.starts_with("sec-websocket-") && name != "sec-websocket-protocol")))
            throw http::Error(format("the header `{}` is part of opening a WebSocket, so it cannot be set", e.first), "bad-request", request.url);
        lines += e.first + ": " + e.second + "\r\n";
    }
    return lines;
}

// From the `name:value` lines of the runtime.
vector<http::Header> http::__parseHeaders(string raw) {
    vector<http::Header> out;
    long start = 0;
    while (start < raw.size()) {
        long end = raw.find("\n", start);
        string line = raw.substr(start, end - start);
        long colon = line.find(":");
        out.push_back(http::Header{line.substr(0, colon), line.substr(colon + 1, string::npos)});
        start = end + 1;
    }
    return out;
}

http::Response http::send(http::Request request) throws {
    string method;
    for (char c : request.method) method += toupper(c);  // as fetch does, and a lowercase `head` would otherwise wait for a body
    if (!http::__isToken(method))
        throw http::Error(format("`{}` cannot be a method: use a word like GET or POST", request.method), "bad-request", request.url);
    string lines = try http::__headerLines(request, false);
    http::Response response;
    string rawHeaders;
    string kind;
    string problem;
    if (!cplus_http_send((void*)&method, (void*)&request.url, (void*)&lines, (void*)&request.body, request.timeout, request.maxBytes,
                         request.followRedirects, (void*)&request.caFile, &response.status, (void*)&response.body, (void*)&response.url,
                         (void*)&rawHeaders, (void*)&kind, (void*)&problem))
        throw http::Error(problem, kind, request.url);
    response.ok = response.status >= 200 && response.status <= 299;
    response.headers = http::__parseHeaders(rawHeaders);
    return response;
}

http::Response http::get(string url) throws {
    http::Request request;
    request.url = url;
    return try http::send(request);
}

http::Response http::__withBody(string method, string url, string body, string contentType) throws {
    http::Request request;
    request.method = method;
    request.url = url;
    request.body = body;
    request.headers["Content-Type"] = contentType;
    return try http::send(request);
}

http::Response http::post(string url, string body, string contentType) throws { return try http::__withBody("POST", url, body, contentType); }
http::Response http::put(string url, string body, string contentType) throws { return try http::__withBody("PUT", url, body, contentType); }
http::Response http::patch(string url, string body, string contentType) throws { return try http::__withBody("PATCH", url, body, contentType); }

http::Response http::delete(string url) throws {
    http::Request request;
    request.method = "DELETE";
    request.url = url;
    return try http::send(request);
}

// For a part of a URL, as JavaScript's encodeURIComponent, so that text from outside stays that one part.
string http::encode(string text) {
    const string digits = "0123456789ABCDEF";
    string out;
    for (char c : text) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += digits[int(c) / 16];
            out += digits[int(c) % 16];
        }
    }
    return out;
}

class http::WebSocket {
    void* socket_;
    string url_;

public:
    double timeout = 0;

    http::WebSocket(void* socket, string url) : socket_(socket), url_(url) {}
    ~http::WebSocket() { cplus_ws_free(socket_); }

    void send(string text) throws {
        string kind;
        string problem;
        if (!cplus_ws_send(socket_, (void*)&text, false, timeout, (void*)&kind, (void*)&problem))
            throw http::Error(problem, kind, url_);
    }
    void sendBinary(string data) throws {
        string kind;
        string problem;
        if (!cplus_ws_send(socket_, (void*)&data, true, timeout, (void*)&kind, (void*)&problem))
            throw http::Error(problem, kind, url_);
    }
    // The next message, or null once the connection has closed as it should.
    http::Message? receive() throws {
        string data;
        string kind;
        string problem;
        int got = cplus_ws_receive(socket_, (void*)&data, timeout, (void*)&kind, (void*)&problem);
        if (got < 0) throw http::Error(problem, kind, url_);
        if (got == 0) return null;
        return http::Message{data, got == 2};
    }
    void close() { cplus_ws_close(socket_); }
    int? closeCode() const {
        int code = cplus_ws_close_code(socket_);
        if (code == 0) return null;
        return code;
    }
    string closeReason() const {
        string out;
        cplus_ws_close_reason(socket_, (void*)&out);
        return out;
    }
};

http::WebSocket http::connectWith(http::Request request) throws {
    string lines = try http::__headerLines(request, true);
    string kind;
    string problem;
    void* socket = cplus_ws_connect((void*)&request.url, (void*)&lines, request.timeout, request.maxBytes, (void*)&request.caFile,
                                    (void*)&kind, (void*)&problem);
    if (socket == null) throw http::Error(problem, kind, request.url);
    return http::WebSocket(socket, http::__urlForError(request.url));
}

http::WebSocket http::connect(string url) throws {
    http::Request request;
    request.url = url;
    return try http::connectWith(request);
}
)cplus";

inline constexpr const char* kReflectSource = R"cplus(
extern "C" {
    string cplus_type_name(void* type);
    string cplus_type_kind(void* type);
    long cplus_type_size(void* type);
    long cplus_type_length(void* type);
    void* cplus_type_elem(void* type);
    long cplus_type_field_count(void* type);
    string cplus_type_field_name(void* type, long index);
    long cplus_type_field_offset(void* type, long index);
    void* cplus_type_field_type(void* type, long index);
    bool cplus_type_field_public(void* type, long index);
    void* cplus_type_real(void* type, void* place);
}

struct reflect::Field {
    string name;
    long offset;
    reflect::Type type;
    bool isPublic;
};

struct reflect::Type {
private:
    void* type_;

public:
    reflect::Type(void* type) : type_(type) {}
    string name() const { return cplus_type_name(type_); }
    string kind() const { return cplus_type_kind(type_); }
    long size() const { return cplus_type_size(type_); }
    long length() const { return cplus_type_length(type_); }
    reflect::Type? elem() const {
        void* found = cplus_type_elem(type_);
        if (found == null) return null;
        return reflect::Type(found);
    }
    vector<reflect::Field> fields() const {
        vector<reflect::Field> out;
        for (long i = 0; i < cplus_type_field_count(type_); i++)
            out.push_back(reflect::Field{cplus_type_field_name(type_, i), cplus_type_field_offset(type_, i),
                                         reflect::Type(cplus_type_field_type(type_, i)), cplus_type_field_public(type_, i)});
        return out;
    }
};

template<typename T> reflect::Type reflect::type() { return reflect::Type(__type_of(T)); }

// What the value really is: a class reference may refer to something that derives from its class.
template<typename T> reflect::Type reflect::of(T value) { return reflect::Type(cplus_type_real(__type_of(T), (void*)&value)); }
)cplus";

}  // namespace cplus
