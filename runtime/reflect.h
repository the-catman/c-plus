#ifndef CPLUS_REFLECT_H
#define CPLUS_REFLECT_H

// What the running program knows about a type: a descriptor, which the compiler writes as constant data
// for the types a program reflects on or turns into JSON, and for nothing else. See PLAN.md, 2.14.

#include <stdint.h>

enum {
    CPLUS_KIND_BOOL,
    CPLUS_KIND_INT,
    CPLUS_KIND_LONG,
    CPLUS_KIND_FLOAT,
    CPLUS_KIND_DOUBLE,
    CPLUS_KIND_CHAR,
    CPLUS_KIND_STRING,
    CPLUS_KIND_VECTOR,    // `elem` is what it holds
    CPLUS_KIND_ARRAY,     // `elem` is what it holds, and `length` how many
    CPLUS_KIND_OPTIONAL,  // `elem` is what it holds. `length` is where the value is, after the byte that says if there
                          // is one, or -1 for a reference, which is a pointer that may be null
    CPLUS_KIND_STRUCT,    // `fields`, measured from where the struct is
    CPLUS_KIND_CLASS,     // `fields`, measured from the start of the object. The value is a pointer to the object
    CPLUS_KIND_UNIQUE,    // `elem` is the class
    CPLUS_KIND_WEAK,
    CPLUS_KIND_MAP,       // a hashmap. Its entries are a vector at its start, of type `elem`, whose fields are the key
                          // and the value, and `length` is where the vector of hashes is. -1 there is an erased entry
    CPLUS_KIND_JSON_VALUE,  // a json::Value, whose fields json.c knows by position: see compiler/Builtins.h
    CPLUS_KIND_OTHER      // a pointer, which only this program can make sense of
};

enum {
    CPLUS_FIELD_PUBLIC = 1,
    CPLUS_FIELD_REQUIRED = 2  // public, and it has no value until it is given one
};

typedef struct cplus_type cplus_type;

typedef struct {
    const char* name;
    int64_t offset;
    const cplus_type* type;
    int64_t flags;
} cplus_field;

struct cplus_type {
    int64_t kind;
    const char* name;
    int64_t size;  // what a value takes in a variable: for a class, a pointer
    const cplus_type* elem;
    int64_t length;
    const cplus_field* fields;
    int64_t field_count;
    void (*make)(void* place);     // makes a value in zero bytes: for a class, a new object. Null if zero bytes are one
    void (*retain)(void* place);   // what the value at this address holds, or null if it holds nothing to count
    void (*release)(void* place);
    void (*put)(void* map, void* entry);  // a map: adds this entry, or replaces the value of that key, using it up
};

#endif
