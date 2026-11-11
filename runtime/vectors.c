// What insert() and erase() on a vector need beyond the code generated for them, which only a program that calls them links.

#include <inttypes.h>
#include <stdio.h>

#include "internal.h"

// `v.insert(i, x)` with i outside 0 to v.size(), or `v.erase(i)` with i outside 0 to v.size() - 1.
_Noreturn void cplus_trap_vec_position(const char* method, int64_t pos, int64_t size, const char* file, int32_t line, int32_t column) {
    char message[160];
    snprintf(message, sizeof message, "%s() index %" PRId64 " is out of range for a vector of size %" PRId64, method, pos, size);
    cplus_trap(message, file, line, column);
}
