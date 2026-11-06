// What the module `reflect` asks about a type: see reflect.h. Only a program that imports it links this.

#include <stddef.h>

#include "internal.h"

const char* cplus_type_name(const cplus_type* t) { return t->name; }

const char* cplus_type_kind(const cplus_type* t) {
    switch (t->kind) {
        case CPLUS_KIND_BOOL: return "bool";
        case CPLUS_KIND_INT: return "int";
        case CPLUS_KIND_LONG: return "long";
        case CPLUS_KIND_FLOAT: return "float";
        case CPLUS_KIND_DOUBLE: return "double";
        case CPLUS_KIND_CHAR: return "char";
        case CPLUS_KIND_STRING: return "string";
        case CPLUS_KIND_VECTOR: return "vector";
        case CPLUS_KIND_ARRAY: return "array";
        case CPLUS_KIND_OPTIONAL: return "optional";
        case CPLUS_KIND_STRUCT: case CPLUS_KIND_JSON_VALUE: return "struct";
        case CPLUS_KIND_CLASS: return "class";
        case CPLUS_KIND_UNIQUE: return "unique";
        case CPLUS_KIND_WEAK: return "weak";
        case CPLUS_KIND_MAP: return "hashmap";
        default: return "pointer";
    }
}

int64_t cplus_type_size(const cplus_type* t) { return t->size; }
int64_t cplus_type_length(const cplus_type* t) { return t->kind == CPLUS_KIND_ARRAY ? t->length : 0; }
const cplus_type* cplus_type_elem(const cplus_type* t) { return t->elem; }
int64_t cplus_type_field_count(const cplus_type* t) { return t->kind == CPLUS_KIND_MAP ? 0 : t->field_count; }
const char* cplus_type_field_name(const cplus_type* t, int64_t i) { return t->fields[i].name; }
int64_t cplus_type_field_offset(const cplus_type* t, int64_t i) { return t->fields[i].offset; }
const cplus_type* cplus_type_field_type(const cplus_type* t, int64_t i) { return t->fields[i].type; }
int32_t cplus_type_field_public(const cplus_type* t, int64_t i) { return (t->fields[i].flags & CPLUS_FIELD_PUBLIC) != 0; }

// What an object really is, given what it is known as. The value is at `place`.
const cplus_type* cplus_type_real(const cplus_type* t, const void* place) {
    if (t->kind == CPLUS_KIND_OPTIONAL && t->length < 0) t = t->elem;
    if (t->kind == CPLUS_KIND_UNIQUE) t = t->elem;
    if (t->kind != CPLUS_KIND_CLASS) return t;
    const char* object = *(const char* const*)place;
    if (!object) return t;
    return (*(const cplus_type* const* const*)(object + 8))[CPLUS_VTABLE_TYPE];
}
