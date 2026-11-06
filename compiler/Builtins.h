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
