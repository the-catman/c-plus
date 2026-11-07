// For programs built with --race, on systems whose linker can put these in the way of what they wrap. Not built with
// ThreadSanitizer, since it would then call these from themselves.
//
// ThreadSanitizer keeps a stack of its own, of 64K calls, and does not check it. A deeper call writes over whatever is next,
// which has been seen to include what the system's loader uses, so nothing after that can be trusted, including the report of
// a crash. Every function built with ThreadSanitizer calls these on the way in and out, so this stops the program first.

#include "internal.h"

void __real___tsan_func_entry(void* caller);
void __real___tsan_func_exit(void);

enum { MOST_CALLS = 60000 };  // what is left is for what ThreadSanitizer calls itself, and for reporting this

static _Thread_local int calls;

void __wrap___tsan_func_entry(void* caller) {
    if (++calls > MOST_CALLS) {
        calls = 0;  // cplus_stop comes through here too
        cplus_stop("the program ran out of stack, as --race can only follow 60000 calls at once",
                   "if a function calls itself, check that its recursion stops. Without --race, it can go as deep as its stack allows");
    }
    __real___tsan_func_entry(caller);
}

void __wrap___tsan_func_exit(void) {
    calls--;
    __real___tsan_func_exit();
}
