#pragma once

// What the language offers that is written in C+ itself. The parser adds it after the program, only
// when the program uses it. Its tokens have no location, which is how the compiler knows
// them, and a few things are allowed only here:
//
//   __hash(x)       a number for x, the same for two values that are ==, and never negative
//   __move(x)       as move(x), but x is always left empty, even if it could have been copied
//   __Hashable      the constraint Hashable, which a program's own `Hashable` cannot replace
//   V?              stays V when V can already be empty: a pointer, an optional or a weak reference

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

}  // namespace cplus
