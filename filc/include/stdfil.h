/*
 * Copyright (c) 2023-2024 Epic Games, Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY EPIC GAMES, INC. ``AS IS AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL EPIC GAMES, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#ifndef FILC_STDFIL_H
#define FILC_STDFIL_H

#ifdef __cplusplus
extern "C" {
#endif

#if 0
} /* tell emacs what's up */
#endif

/* This header defines the set of standard Fil-C APIs that are intended to be stable over time. */

/* You shouldn't use the filc_bool type or rely on its existence; I just need it to hack around C++
   being incompatible with C in this regard. */
#ifdef __cplusplus
typedef bool filc_bool;
#else
typedef _Bool filc_bool;
#endif

/* Allocate `count` bytes of memory zero-initialized and with all word types set to the unset type.
   May allocate slightly more than `count`, based on the runtime's minalign (which is currently 16).
   
   This is a GC allocation, so freeing it is optional. Also, if you free it and then use it, your
   program is guaranteed to panic.

   Memory that has the unset type may be used for any type of access, but then the type monotonically
   transitions. For example, if you access some word in this object using int, then the type of that
   word becomes int and stays that until the memory is freed.

   libc's malloc just forwards to this. There is no difference between calling `malloc` and
   `zgc_alloc`. */
void* zgc_alloc(__SIZE_TYPE__ count);

/* Allocate `count` bytes of memory with the GC, aligned to `alignment`. Supports very large alignments,
   up to at least 128k (may support even larger ones in the future). Like with `zgc_alloc`, the memory
   starts out with unset type. */
void* zgc_aligned_alloc(__SIZE_TYPE__ alignment, __SIZE_TYPE__ count);

/* Reallocates the object pointed at by `old_ptr` to now have `count` bytes, and returns the new
   pointer. `old_ptr` must satisfy `old_ptr == zgetlower(old_ptr)`, otherwise the runtime panics your
   process. If `count` is larger than the size of `old_ptr`'s allocation, then the new space is
   initialized to unset type. For the memory that is copied, the type is preserved.

   libc's realloc just forwards to this. There is no difference between calling `realloc` and
   `zgc_realloc`. */
void* zgc_realloc(void* old_ptr, __SIZE_TYPE__ count);

/* Just like `zgc_realloc`, but allows you to specify arbitrary alignment on the newly allocated
   memory. */
void* zgc_aligned_realloc(void* old_ptr, __SIZE_TYPE__ alignment, __SIZE_TYPE__ count);

/* Frees the object pointed to by `ptr`. `ptr` must satisfy `ptr == zgetlower(ptr)`, otherwise the
   runtime panics your process. `ptr` must point to memory allocated by `zgc_alloc`,
   `zgc_aligned_alloc`, `zgc_realloc`, or `zgc_aligned_realloc`, and that memory must not have been
   freed yet.
   
   Freeing objects is optional in Fil-C, since Fil-C is garbage collected.
   
   Freeing an object in Fil-C does not cause memory to be reclaimed immediately. Instead, it
   transitions all of the word types in the object to the free type, preventing any future accesses
   from working, and also sets the free flag in the object header. This has two GC implications:
   
   - The GC doesn't have to scan any outgoing pointers from this object, since those pointers are not
     reachable to the program (all accesses to them now trap). Hence, freeing an object has the
     benefit that dangling pointers don't lead to memory leaks, as they would in GC'd systems that
     don't support freeing.
     
   - The GC can replace all pointers to this object with pointers that still have the same integer
     address but use the free singleton as their capability. This allows the GC to reclaim memory for
     this object on the next cycle, even if there were still dangling pointers to this object. Those
     dangling pointers would already have trapped on access even before the next cycle (since the
     object's capability has the free type in each word, and the free bit set in the header).
     Switching to the free singleton is not user-visible, except via ptr introspection like `%P` or
     `zptr_to_new_string`.
   
   libc's free just forwards to this. There is no difference between calling `free` and `zgc_free`. */
void zgc_free(void* ptr);

/* Accessors for the bounds.
 
   The lower and upper bounds have the same capability as the incoming ptr. So, if you know that a
   ptr points into the middle of struct foo and you want to get to the base of struct foo, you can
   totally do:

       struct foo* foo = (struct foo*)zgetlower(ptr);

   Or if you know that ptr points to an array of struct foos, and you want to get a pointer to the
   last one:
 
       struct foo* foo = (struct foo*)zgetupper(ptr) - 1;
       
   In both cases, the pointer is usable provided that the bounds are big enough for struct foo and
   that the type is compatible with struct foo. */
void* zgetlower(void* ptr);
void* zgetupper(void* ptr);

/* Get the pointer's array length, which is the distance to upper in units of the ptr's static type. */
#define zlength(ptr) ({ \
        __typeof__((ptr) + 0) __d_ptr = (ptr); \
        (__typeof__((ptr) + 0))zgetupper(__d_ptr) - __d_ptr; \
    })

/* Tells if the pointer has a capability and that capability is not free. */
filc_bool zhasvalidcap(void* ptr);

/* Tells if the pointer is in bounds of lower/upper. This is not a guarantee that accesses will
   succeed, since this does not check type. For example, valid function pointers are zinbounds but
   cannot be "accessed" regardless of type (can only be called if in bounds). */
filc_bool zinbounds(void* ptr);

/* Tells if a value of the given size is in bounds of the pointer. */
filc_bool zvalinbounds(void* ptr, __SIZE_TYPE__ size);

/* Returns true if the pointer points to a byte with unset type. */
filc_bool zisunset(void* ptr);

/* Returns true if the pointer points at an integer byte.
   
   If this returns false, then the pointer may point to a pointer, or be unset, or to opaque memory,
   or to any other type when we add more types.
 
   Pointer must be in bounds, else your process dies. */
filc_bool zisint(void* ptr);

/* Returns the pointer phase of the pointer.
   
   - 0 means this points to the base of a pointer.
   
   - 1..31 (inclusive) means you're pointing into the middle of a pointer; so, you subtract that many
     bytes from your pointer, then you'll be able to dereference it.
   
   - -1 means that this does not point to a pointer at all. This means it could mean that it's an int
     or it could be unset or it could mean opaque memory. (Or any other type when we add more types.)
     
   Pointer must be in bounds, else your process dies. */
int zptrphase(void* ptr);

/* Returns true if the pointer points at any kind of pointer memory. Equivalent to
   isptrphase(p) != -1. */
filc_bool zisptr(void* ptr);

/* Returns true if the pointer points at pointers or integers.
 
   New types, as well as opaque memory, will return false. */
filc_bool zisintorptr(void* ptr);

/* Construct a pointer that has the capability from `object` but the address from `address`. This
   is a memory-safe operation, and it's guaranteed to be equivalent to:
   
       object -= (uintptr_t)object;
       object += address;
   
   This is useful for situations where you want to use part of the object's address for tag bits. */
void* zmkptr(void* object, unsigned long address);

/* Memory-safe helpers for doing bit math on addresses. */
void* zorptr(void* ptr, unsigned long bits);
void* zandptr(void* ptr, unsigned long bits);
void* zxorptr(void* ptr, unsigned long bits);

/* Returns a pointer that points to `newptr` masked by the `mask`, while preserving the
   bits from `oldptr` masked by `~mask`. Also asserts that `newptr` has no bits in `~mask`.
   
   Useful for situations where you want to reassign a pointer from `oldptr` to `newptr` but
   you have some kind of tagging in `~mask`. */
void* zretagptr(void* newptr, void* oldptr, unsigned long mask);

/* The pointer-nullifying memmove.
   
   This memmove will kill your process if anything goes out of bounds.
   
   But on pointers (either destination thinks the byte is a pointer or the source thinks the byte is
   a pointer), the value copied is zero.
   
   For example, if you call this to copy pointers to ints, those ints will become zero.
   
   Or if you call this to copy ints to pointers, those pointers will become zero.

   Also if you copy pointers to pointers, then zero will be copied.

   But if you copy ints to ints, then the actual bytes are copied. */
void zmemmove_nullify(void* dst, const void* src, __SIZE_TYPE__ count);

/* Allocates a new string (with zgc_alloc(char, strlen+1)) and prints a dump of the ptr to that string.
   Returns that string. You have to zgc_free the string when you're done with it.

   This is exposed as %P in the zprintf family of functions. */
char* zptr_to_new_string(const void* ptr);

/* Mostly type-oblivious memcmp implementation. This works for any two ranges so long as they contain
   ints, ptrs, or unset words. It's fine to compare ints to ptrs, for example. */
int zmemcmp(const void* ptr1, const void* ptr2, __SIZE_TYPE__ count);

/* The zptrtable can be used to encode pointers as integers. The integers are __SIZE_TYPE__ but
   tend to be small; you can usually get away with storing them in 32 bits.
   
   The zptrtable itself is garbage collected, so you don't have to free it (and attempting to
   free it will kill the shit out of your process).
   
   You can have as many zptrtables as you like.
   
   Encoding a ptr is somewhat expensive. Currently, the zptrtable takes a per-zptrtable lock to
   do it (so at least it's not a global lock).
   
   Decoding a ptr is cheap. There is no locking.
   
   The zptrtable automatically purges pointers to free objects and reuses their indices.
   However, the table does keep a strong reference to objects. So, if you encode a ptr and then
   never free it, then the zptrtable will keep it alive. But if you free it, the zptrtable will
   autopurge it.
   
   If you try to encode a ptr to a free object, you get 0. If you decode 0 or if the object that
   would have been decoded is free, this returns NULL. Valid pointers encode to some non-zero
   integer. You cannot rely on those integers to be sequential, but you can rely on them to:

   - Stay out of the the "null page" (i.e. they are >=16384) just to avoid clashing with
     assumptions about pointers (even though the indices are totally no pointers).

   - Fit in 32 bits unless you have hundreds of millions of objects in the table.

   - Definitely fit in 64 bits in the general case.
   
   - Be multiples of 16 to look even more ptr-like (and allow low bit tagging if you're into
     that sort of thing). */
struct zptrtable;
typedef struct zptrtable zptrtable;

zptrtable* zptrtable_new(void);
__SIZE_TYPE__ zptrtable_encode(zptrtable* table, void* ptr);
void* zptrtable_decode(zptrtable* table, __SIZE_TYPE__ encoded_ptr);

/* The exact_ptrtable is like ptrtable, but:

   - The encoded ptr is always exactly the pointer's integer value.

   - Decoding is slower and may have to grab a lock.

   - Decoding a pointer to a freed object gives exactly the pointer's integer value but with a null
     capability (so you cannot dereference it). */
struct zexact_ptrtable;
typedef struct zexact_ptrtable zexact_ptrtable;

zexact_ptrtable* zexact_ptrtable_new(void);
__SIZE_TYPE__ zexact_ptrtable_encode(zexact_ptrtable* table, void* ptr);
void* zexact_ptrtable_decode(zexact_ptrtable* table, __SIZE_TYPE__ encoded_ptr);

/* This function is just for testing zptrtable and it only returns accurate data if
   zis_runtime_testing_enabled(). */
__SIZE_TYPE__ ztesting_get_num_ptrtables(void);

/* Low-level printing functions. These might die someday. They are useful for Fil-C's own tests. They
   print directly to stdout using write(). They are safe (passing an invalid ptr to zprint() will trap
   for sure, and it will never print out of bounds even if there is no null terminator). */
void zprint(const char* str);
void zprint_long(long x);
void zprint_ptr(const void* ptr);

/* Low-level functions that should be provided by libc, which lives above this. These are exposed for
   the purpose of Fil-C's own snprintf implementation, which lives below libc. They are also safe to
   call instead of what libc offers. */
__SIZE_TYPE__ zstrlen(const char* str);
int zisdigit(int chr);

/* This is almost like sprintf, but because Fil-C knows the upper bounds of buf, this actually ends
   up working exactly like snprintf where the size is upper-ptr. Hence, in Fil-C, it's preferable
   to call zsprintf instead of zsnprintf.

   In libc, sprintf (without the z) behaves kinda like zsprintf, but traps on OOB.

   The main difference from the libc sprintf is that it uses a different implementation under the hood.
   This is based on the samba snprintf, origindally by Patrick Powell, but it uses the zstrlen/zisdigit/etc
   functions rather than the libc ones, and it has one additional feature:

       - '%P', which prints the full filc_ptr (i.e. 0xptr,0xlower,0xupper,...type...).

   It's not obvious that this code will do the right thing for floating point formats. But this code is
   pizlonated, so if it goes wrong, at least it'll stop your program from causing any more damage. */
int zvsprintf(char* buf, const char* format, __builtin_va_list args);
int zsprintf(char* buf, const char* format, ...);

int zvsnprintf(char* buf, __SIZE_TYPE__ size, const char* format, __builtin_va_list args);
int zsnprintf(char* buf, __SIZE_TYPE__ size, const char* format, ...);

/* This is like asprintf, but instead of super annoyingly returning the string in an out argument,
   it just fucking returns it in the return value like a fucking sensible function. */
char* zvasprintf(const char* format, __builtin_va_list args);
char* zasprintf(const char* format, ...);

/* This is mostly just like printf, but does only per-call buffering. In particular, this relies on
   zvasprintf under the hood and then prints the entire string in one write(2) call (unless write
   demands that we call it again).

   Note that the main reason why you might want to use this for debugging over printf is that it supports:

       - '%P', which prints the full filc_ptr (i.e. 0xptr,0xlower,0xupper,...type...).

   But if you want to debug floating point, you should maybe go with printf. */
void zvprintf(const char* format, __builtin_va_list args);
void zprintf(const char* format, ...);

/* This prints the given message and then shuts down the program using the same shutdown codepath
   used for memory safety violatins (i.e. it's designed to really kill the shit out of the process). */
void zerror(const char* str);
void zerrorf(const char* str, ...);

/* Definitely assert something. This is not some kind of optional assert that you can compile out.
   It's gonna be there and do its thing no matter what, even in production, like a real assert
   should. */
#define ZASSERT(exp) do { \
        if ((exp)) \
            break; \
        zerrorf("%s:%d: %s: assertion %s failed.", __FILE__, __LINE__, __PRETTY_FUNCTION__, #exp); \
    } while (0)

void zfence(void);
void zstore_store_fence(void);
void zcompiler_fence(void);

/* These functions are deprecated. I added them back when the clang builtin atomics didn't work
   for pointers. I have since fixed that. Therefore, you don't need to use these functions.
   However, I have already written code that uses these functions, so I am keeping these around
   for now.

   I have simplified the memory ordering approach based on pretty good data that only a tiny
   fraction of algorithms ever benefit from unfenced CAS on modern CPUs, and the fact that CPUs
   usually only give you either one or two variants. The "unfenced" variants are like RELAXED
   in the C model. The not-"unfenced" ones are like SEQ_CST. Strong CAS just returns the old
   value rather than both a bool and the old value, since emprically, relying on the bit that
   the CAS instruction returns for brancing on the CAS is never any faster than branching on
   a comparison of your expected value and the old value returned by CAS.

   I may add more ptr atomic functions as I find a need for them. */
filc_bool zunfenced_weak_cas_ptr(void** ptr, void* expected, void* new_value);
filc_bool zweak_cas_ptr(void** ptr, void* expected, void* new_value);
void* zunfenced_strong_cas_ptr(void** ptr, void* expected, void* new_value);
void* zstrong_cas_ptr(void** ptr, void* expected, void* new_value);
filc_bool zunfenced_intense_cas_ptr(void** ptr, void** expected, void* new_value);
filc_bool zintense_cas_ptr(void** ptr, void** expected, void* new_value);
void* zunfenced_xchg_ptr(void** ptr, void* new_value);
void* zxchg_ptr(void** ptr, void* new_value);
void zatomic_store_ptr(void** ptr, void* new_value);
void zunfenced_atomic_store_ptr(void** ptr, void* new_value);
void* zatomic_load_ptr(void** ptr);
void* zunfenced_atomic_load_ptr(void** ptr);

/* Returns a readonly snapshot of the passed-in arguments object. The arguments are laid out as if you
   had written a struct with the arguments as fields. */
void* zargs(void);

/* The return buffer size used by all C code. When C code returns something larger than this, it does so
   by passing an argument that points to a return buffer. */
#define ZC_RET_BYTES 16

/* Calls the `callee` with the arguments being a snapshot of the passed-in `args` object. The `args`
   object does not have to be readonly, but can be. Allows the `callee` to return up to `ret_bytes` data
   as the result, and returns it as a new readonly allocation. Note that in almost all cases, `ret_bytes`
   has to be ZC_RET_BYTES. It'll only be something else when dealing with something other than C being
   compiled to LLVM IR.

   FIXME: This currently does not support unwinding and exceptions.

   Here's an example of how to use this together with zargs() and zreturn() to create a function that is
   a strong alias for another function:

       void alias(void) { zreturn(zcall(target_function, zargs(), ZC_RET_BYTES), ZC_RET_BYTES); }

   This works because taking/returning void in alias() only has the effect of making alias() itself not
   check anything about its arguments or return. */
void* zcall(void* callee, void* args, __SIZE_TYPE__ ret_bytes);

/* Simplified version of `zcall` that returns a value of type `ret_type`. Note this only works for return
   types that wouldn't be returned by passing a return argument. */
#define zcall_value(callee, args, ret_type) (*(ret_type*)zcall((callee), (args), ZC_RET_BYTES))

/* Returns from the caller with the first `ret_bytes` bytes of the `rets` object as the return value.
   Snapshots the `rets` object, which may or may not be readonly. The type of `rets` does not have to
   agree with the C return type. */
__attribute__((__noreturn__)) void zreturn(void* rets, __SIZE_TYPE__ ret_bytes);

/* Simplified version of `zreturn` that takes a value. */
#define zreturn_value(value) do { \
        __typeof__(value) __fc_tmp = (value); \
        zreturn(&(__fc_tmp), sizeof(__fc_tmp)); \
    } while (0)

/* Polymorphic forwarding function. Calls the target_function with the caller's arguments and causes the
   caller to return with the target_function's return value. Only works for C/C++ functions. */
#define zcforward(target_function) zreturn(zcall((target_function), zargs(), ZC_RET_BYTES), ZC_RET_BYTES)

enum zpark_result {
    zpark_condition_failed,
    zpark_timed_out,
    zpark_unparked
};

typedef enum zpark_result zpark_result;

/* Parks the thread in a queue associated with the given address, which cannot be null. The
   parking only succeeds if the condition function returns true while the queue lock is held.
  
   If condition returns false, it will unlock the internal parking queue and then it will
   return zpark_condition_failed.
  
   If condition returns true, it will enqueue the thread, unlock the parking queue lock, call
   the before_sleep function, and then it will sleep so long as the thread continues to be on the
   queue and the timeout hasn't fired. Finally, this returns zpark_unpakred if we actually got
   unparked or zpark_timed_out if the timeout was hit.
  
   Note that before_sleep is called with no locks held, so it's OK to do pretty much anything so
   long as you don't recursively call zpark_if(). You can call zunpark_one()/zunpark_all()
   though. It's useful to do that in before_sleep() for implementing condition variables. If you
   do call into the zpark_if recursively, you'll get a trap.
   
   Crucially, when zpark_if calls your callbacks, it is only holding the queue lock associated
   with the address, and not any other locks that the Fil-C runtime uses.

   The timeout is according to the REALTIME clock on POSIX, but formatted as a double because
   this is a civilized API. Use positive infinity (aka 1. / 0.) if you just want this to wait
   forever.
   
   This is signal-safe, but you're on your own if you park on something in a signal handler that
   can only be unparked by the thread that the handler interrupted. Also, to make that work, the
   condition callback is called with signals blocked. This is fine, since if you use that
   function to do unbounded work, then you run the risk of blocking the whole program (since the
   parking lot is holding a queue lock for the bucket for that address, which may be used by any
   number of other addresses).

   Errors are reported by killing the shit out of your program. */
zpark_result zpark_if(const void* address,
                      filc_bool (*condition)(void* arg),
                      void (*before_sleep)(void* arg),
                      void* arg,
                      double absolute_timeout_in_milliseconds);

/* Simplified version of zpark_if. If the address is int-aligned, then this does a zpark_if with
   a condition that returns true if the address contains the expected value. Does nothing on
   before_sleep.
   
   This function has adorable behavior when address is misaligned. In that case, the address
   passed to zpark_if is the original misaligned address, but the rounded-down address is used for
   the comparison. This lets you use an atomic int as four notification channels.

   This matches the basic futex API except futexes would error on misaligned.

   Note that while this expects you to use an int, zpark_if has no such restriction. You could use
   any atomic word there (or words, if you're fancy).

   This is signal-safe. */
zpark_result zcompare_and_park(const int* address, int expected_value,
                               double absolute_timeout_in_milliseconds);

/* Unparks one thread from the queue associated with the given address, and calls the given
   callback while the address is locked. Reports to the callback whether any thread got
   unparked and whether there may be any other threads still on the queue.

   This is signal-safe. But, that implies that the callback is called with signals blocked. That's
   fine, since you have to avoid unbounded work in that function anyway, since it's called with
   the bucket lock held, an the bucket lock may be shared between your address and any number of
   other addresses. */
void zunpark_one(const void* address,
                 void (*callback)(filc_bool did_unpark_thread,
                                  filc_bool may_have_more_threads,
                                  void* arg),
                 void* arg);

/* Unparks up to count threads from the queue associated with the given address, which cannot
   be null. Returns the number of threads unparked. */
unsigned zunpark(const void* address, unsigned count);

/* Returns true if running in the build of the runtime that has extra (super expensive) testing
   checks.

   This is here so that the test suite can assert that it runs with testing asserts enabled. */
filc_bool zis_runtime_testing_enabled(void);

/* Asks Fil-C to run additional pointer validation on this pointer. If memory safety holds, then
   these checks will succeed. If they don't, then it's a Fil-C bug, and we should fix it. It could
   be a real bug, or it could be a bug in the validation checks. They are designed to be hella strict
   and maybe I made them too strict.
   
   If you run with pizfix/lib_test in your library path, then this check happens in a bunch of
   random places anyway (and that's the main reason why the lib_test version is so slow). */
void zvalidate_ptr(void* ptr);

/* Request and wait for a fresh garbage collection cycle. If a GC cycle is already happening, then this
   will cause another one to happen after that one finishes, and will wait for that one.

   GCing doesn't automatically decommit the freed memory. If you want that to also happen, then call
   zscavenge_synchronously() after this returns.

   If the GC is running concurrently (the default), then other threads do not wait. Only the calling
   thread waits.

   If the GC is running in stop-the-world mode (not the default, also not recommended), then this will
   stop all threads to do the GC. */
void zgc_request_and_wait(void);

/* Request a synchronous scavenge. This decommits all memory that can be decommitted.
   
   If we you want to free all memory that can possibly be freed and you're happy to wait, then you should
   first zgc_request_and_wait() and then zscavenge_synchronously().

   Note that it's fine to call this whether the scavenger is suspended or not. Even if the scavenger is
   suspended, this will scavenge synchronously. If the scavenger is not suspended, then this will at worst
   contend on some locks with the scavenger thread (and at best cause the scavenge to happen faster due to
   parallelism). */
void zscavenge_synchronously(void);

/* Suspend the scavenger. If the scavenger is suspended, then free pages are not returned to the OS.
   This is intended to be used only for testing. */
void zscavenger_suspend(void);
void zscavenger_resume(void);

struct zstack_frame_description;
typedef struct zstack_frame_description zstack_frame_description;

struct zstack_frame_description {
    const char* function_name;
    const char* filename;
    unsigned line;
    unsigned column;

    /* Whether the frame supports throwing (i.e. the llvm::Function did not have the nounwind
       attribute set).
    
       C code by default does not support throwing, but you can enable it with -fexceptions. 
    
       Supporting throwing doesn't mean that there's a personality function. It's totally unrelated
       For example, a C++ function may have a personality function, but since it's throw(), it's got
       nounwind set, and so it doesn't supporting throwing. */
    filc_bool can_throw;

    /* Whether the frame supports catching. Only frames that support catching can have personality
       functions. But not all of them do. */
    filc_bool can_catch;

    /* personality_function and eh_data are set for frames that can catch exceptions. The eh_data is
       NULL if the personality_function is NULL. If the personality_function is not NULL, then the
       eh_data's meaning is up to that function. The signature of the personality_function is up to the
       compiler. The signature of the eh_data is up to the compiler. When unwinding, you can call the
       personality_function, or not - up to you. If you call it, you have to know what the signature
       is. It's expected that only the libunwind implementation calls personality_function, since
       that's what knows what its signature is supposed to be. */
    void* personality_function;
    void* eh_data;
};

/* Walks the Fil-C stack and calls callback for every frame found. Continues walking so long as the
   callback returns true. Guaranteed to skip the zstack_scan frame. */
void zstack_scan(filc_bool (*callback)(
                     zstack_frame_description description,
                     void* arg),
                 void* arg);

/* This is the only low-level threading API that we will guarantee working. */
unsigned zthread_self_id(void);

#ifdef __cplusplus
}
#endif

#endif /* FILC_STDFIL_H */

