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

#include "pas_config.h"

#if LIBPAS_ENABLED

#include "filc_runtime.h"

#if PAS_ENABLE_FILC

#include "bmalloc_heap.h"
#include "bmalloc_heap_config.h"
#include "filc_native.h"
#include "filc_parking_lot.h"
#include "fugc.h"
#include "pas_hashtable.h"
#include "pas_scavenger.h"
#include "pas_string_stream.h"
#include "pas_utils.h"
#include "verse_heap_inlines.h"
#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <netdb.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <poll.h>

#define DEFINE_LOCK(name) \
    pas_system_mutex filc_## name ## _lock; \
    \
    void filc_ ## name ## _lock_lock(void) \
    { \
        pas_system_mutex_lock(&filc_ ## name ## _lock); \
    } \
    \
    void filc_ ## name ## _lock_unlock(void) \
    { \
        pas_system_mutex_unlock(&filc_ ## name ## _lock); \
    } \
    \
    void filc_ ## name ## _lock_assert_held(void) \
    { \
        pas_system_mutex_assert_held(&filc_ ## name ## _lock); \
    } \
    struct filc_dummy

FILC_FOR_EACH_LOCK(DEFINE_LOCK);

PAS_DEFINE_LOCK(filc_soft_handshake);
PAS_DEFINE_LOCK(filc_global_initialization);

unsigned filc_stop_the_world_count;
pas_system_condition filc_stop_the_world_cond;

filc_thread* filc_first_thread;
pthread_key_t filc_thread_key;
bool filc_is_marking;

pas_heap* filc_default_heap;
pas_heap* filc_destructor_heap;
verse_heap_object_set* filc_destructor_set;

filc_object* filc_free_singleton;

filc_object_array filc_global_variable_roots;

void filc_check_user_sigset(filc_ptr ptr, filc_access_kind access_kind)
{
    filc_check_access_int(ptr, sizeof(filc_user_sigset), access_kind, NULL);
}

struct user_sigaction {
    filc_ptr sa_handler_ish;
#if FILC_MUSL
    filc_user_sigset sa_mask;
    int sa_flags;
#elif FILC_FILBSD
    int sa_flags;
    filc_user_sigset sa_mask;
#else
#error "Don't know what sigaction looks like"
#endif
};

static void check_user_sigaction(filc_ptr ptr, filc_access_kind access_kind)
{
    FILC_CHECK_PTR_FIELD(ptr, struct user_sigaction, sa_handler_ish, access_kind);
    FILC_CHECK_INT_FIELD(ptr, struct user_sigaction, sa_mask, access_kind);
    FILC_CHECK_INT_FIELD(ptr, struct user_sigaction, sa_flags, access_kind);
}

int filc_from_user_signum(int signum)
{
    if (!FILC_MUSL)
        return signum;
    switch (signum) {
    case 1: return SIGHUP;
    case 2: return SIGINT;
    case 3: return SIGQUIT;
    case 4: return SIGILL;
    case 5: return SIGTRAP;
    case 6: return SIGABRT;
    case 7: return SIGBUS;
    case 8: return SIGFPE;
    case 9: return SIGKILL;
    case 10: return SIGUSR1;
    case 11: return SIGSEGV;
    case 12: return SIGUSR2;
    case 13: return SIGPIPE;
    case 14: return SIGALRM;
    case 15: return SIGTERM;
    case 17: return SIGCHLD;
    case 18: return SIGCONT;
    case 19: return SIGSTOP;
    case 20: return SIGTSTP;
    case 21: return SIGTTIN;
    case 22: return SIGTTOU;
    case 23: return SIGURG;
    case 24: return SIGXCPU;
    case 25: return SIGXFSZ;
    case 26: return SIGVTALRM;
    case 27: return SIGPROF;
    case 28: return SIGWINCH;
    case 29: return SIGIO;
    case 31: return SIGSYS;
    default: return -1;
    }
}

int filc_to_user_signum(int signum)
{
    if (!FILC_MUSL)
        return signum;
    switch (signum) {
    case SIGHUP: return 1;
    case SIGINT: return 2;
    case SIGQUIT: return 3;
    case SIGILL: return 4;
    case SIGTRAP: return 5;
    case SIGABRT: return 6;
    case SIGBUS: return 7;
    case SIGFPE: return 8;
    case SIGKILL: return 9;
    case SIGUSR1: return 10;
    case SIGSEGV: return 11;
    case SIGUSR2: return 12;
    case SIGPIPE: return 13;
    case SIGALRM: return 14;
    case SIGTERM: return 15;
    case SIGCHLD: return 17;
    case SIGCONT: return 18;
    case SIGSTOP: return 19;
    case SIGTSTP: return 20;
    case SIGTTIN: return 21;
    case SIGTTOU: return 22;
    case SIGURG: return 23;
    case SIGXCPU: return 24;
    case SIGXFSZ: return 25;
    case SIGVTALRM: return 26;
    case SIGPROF: return 27;
    case SIGWINCH: return 28;
    case SIGIO: return 29;
    case SIGSYS: return 31;
    default: PAS_ASSERT(!"Bad signal number"); return -1;
    }
}

struct free_tid_node;
typedef struct free_tid_node free_tid_node;
struct free_tid_node {
    unsigned tid;
    free_tid_node* next;
};

static free_tid_node* first_free_tid = NULL;
static unsigned next_fresh_tid = 1;

static unsigned allocate_tid(void)
{
    filc_thread_list_lock_assert_held();
    if (first_free_tid) {
        free_tid_node* result_node = first_free_tid;
        first_free_tid = result_node->next;
        unsigned result = result_node->tid;
        bmalloc_deallocate(result_node);
        return result;
    }
    unsigned result = next_fresh_tid;
    PAS_ASSERT(!pas_add_uint32_overflow(next_fresh_tid, 1, &next_fresh_tid));
    return result;
}

static void deallocate_tid(unsigned tid)
{
    filc_thread_list_lock_assert_held();
    /* FIXME: this is wrong, since it's called in the open. */
    free_tid_node* node = bmalloc_allocate(sizeof(free_tid_node));
    node->tid = tid;
    node->next = first_free_tid;
    first_free_tid = node;
}

/* NOTE: Unlike most other allocation functions, this does not track the allocated object properly.
   It registers it with the global thread list. But once the thread is started, it will dispose itself
   once done, and remove it from the list. So, it's necessary to track threads after creating them
   somehow, unless it's a thread that cannot be disposed. */
filc_thread* filc_thread_create(void)
{
    static const bool verbose = false;
    filc_object* thread_object = filc_allocate_special_early(sizeof(filc_thread), FILC_WORD_TYPE_THREAD);
    filc_thread* thread = thread_object->lower;
    if (verbose)
        pas_log("created thread: %p\n", thread);
    PAS_ASSERT(filc_object_for_special_payload(thread) == thread_object);

    pas_system_mutex_construct(&thread->lock);
    pas_system_condition_construct(&thread->cond);
    filc_object_array_construct(&thread->allocation_roots);
    filc_object_array_construct(&thread->mark_stack);

    /* The rest of the fields are initialized to zero already. */

    filc_thread_list_lock_lock();
    thread->next_thread = filc_first_thread;
    thread->prev_thread = NULL;
    if (filc_first_thread)
        filc_first_thread->prev_thread = thread;
    filc_first_thread = thread;
    thread->tid = allocate_tid();
    PAS_ASSERT(thread->tid);
    filc_thread_list_lock_unlock();
    
    return thread;
}

void filc_thread_undo_create(filc_thread* thread)
{
    PAS_ASSERT(thread->is_stopping || thread->error_starting);
    if (thread->is_stopping) {
        PAS_ASSERT(!thread->error_starting);
        PAS_ASSERT(thread == filc_get_my_thread());
        PAS_ASSERT(thread->state & FILC_THREAD_STATE_ENTERED);
    } else {
        PAS_ASSERT(thread->error_starting);
        PAS_ASSERT(thread != filc_get_my_thread());
    }
    PAS_ASSERT(!thread->allocation_roots.num_objects);
    PAS_ASSERT(!thread->mark_stack.num_objects);
    filc_object_array_destruct(&thread->allocation_roots);
    filc_object_array_destruct(&thread->mark_stack);
    filc_thread_destroy_space_with_guard_page(thread);
}

void filc_thread_mark_outgoing_ptrs(filc_thread* thread, filc_object_array* stack)
{
    /* There's a bunch of other stuff that threads "point" to that is part of their roots, and we
       mark those as part of marking thread roots. The things here are the ones that are treated
       as normal outgoing object ptrs rather than roots. */
    
    fugc_mark_or_free(stack, &thread->arg_ptr);
    fugc_mark_or_free(stack, &thread->cookie_ptr);
    fugc_mark_or_free(stack, &thread->result_ptr);

    /* These need to be marked because phase2 of unwinding calls the personality function multiple
       times before finishing using them. */
    fugc_mark_or_free(stack, &thread->unwind_context_ptr);
    fugc_mark_or_free(stack, &thread->exception_object_ptr);
}

void filc_thread_destruct(filc_thread* thread)
{
    PAS_ASSERT(thread->has_stopped || thread->error_starting || thread->forked);

    /* Shockingly, the BSDs use a pthread_mutex/pthread_cond implementation that actually requires
       destruction. What the fugc. */
    pas_system_mutex_destruct(&thread->lock);
    pas_system_condition_destruct(&thread->cond);
}

void filc_thread_relinquish_tid(filc_thread* thread)
{
    /* Have to be entered because deallocate_tid uses bmalloc. */
    PAS_ASSERT(filc_thread_is_entered(filc_get_my_thread()));
    filc_thread_list_lock_lock();
    deallocate_tid(thread->tid);
    thread->tid = 0;
    filc_thread_list_lock_unlock();
}

void filc_thread_dispose(filc_thread* thread)
{
    filc_thread_list_lock_lock();
    PAS_ASSERT(!thread->tid);
    if (thread->prev_thread)
        thread->prev_thread->next_thread = thread->next_thread;
    else {
        PAS_ASSERT(filc_first_thread == thread);
        filc_first_thread = thread->next_thread;
    }
    if (thread->next_thread)
        thread->next_thread->prev_thread = thread->prev_thread;
    thread->next_thread = NULL;
    thread->prev_thread = NULL;
    filc_thread_list_lock_unlock();
}

static void check_zthread(filc_ptr ptr)
{
    filc_check_access_special(ptr, FILC_WORD_TYPE_THREAD, NULL);
}

static filc_signal_handler* signal_table[FILC_MAX_USER_SIGNUM + 1];

static bool is_initialized = false; /* Useful for assertions. */
static bool exit_on_panic = false;
static bool dump_errnos = false;
static bool run_global_ctors = true;
static bool run_global_dtors = true;

void filc_initialize(void)
{
    PAS_ASSERT(!is_initialized);

    /* This must match SpecialObjectSize in FilPizlonator.cpp. */
    PAS_ASSERT(FILC_SPECIAL_OBJECT_SIZE == 32);

#define INITIALIZE_LOCK(name) \
    pas_system_mutex_construct(&filc_## name ## _lock)
    FILC_FOR_EACH_LOCK(INITIALIZE_LOCK);
#undef INITIALIZE_LOCK

    pas_system_condition_construct(&filc_stop_the_world_cond);

    filc_default_heap = verse_heap_create(FILC_WORD_SIZE, 0, 0);
    filc_destructor_heap = verse_heap_create(FILC_WORD_SIZE, 0, 0);
    filc_destructor_set = verse_heap_object_set_create();
    verse_heap_add_to_set(filc_destructor_heap, filc_destructor_set);
    verse_heap_did_become_ready_for_allocation();

    filc_free_singleton = (filc_object*)verse_heap_allocate(
        filc_default_heap, pas_round_up_to_power_of_2(PAS_OFFSETOF(filc_object, word_types),
                                                      FILC_WORD_SIZE));
    filc_free_singleton->lower = NULL;
    filc_free_singleton->upper = NULL;
    filc_free_singleton->flags = FILC_OBJECT_FLAG_FREE;

    filc_object_array_construct(&filc_global_variable_roots);

    filc_thread* thread = filc_thread_create();
    thread->has_started = true;
    thread->has_stopped = false;
    thread->thread = pthread_self();
    thread->tlc_node = verse_heap_get_thread_local_cache_node();
    thread->tlc_node_version = pas_thread_local_cache_node_version(thread->tlc_node);
    PAS_ASSERT(!pthread_key_create(&filc_thread_key, NULL));
    PAS_ASSERT(!pthread_setspecific(filc_thread_key, thread));

    /* This has to happen *after* we do our primordial allocations. */
    fugc_initialize();

    exit_on_panic = filc_get_bool_env("FILC_EXIT_ON_PANIC", false);
    dump_errnos = filc_get_bool_env("FILC_DUMP_ERRNOS", false);
    run_global_ctors = filc_get_bool_env("FILC_RUN_GLOBAL_CTORS", true);
    run_global_dtors = filc_get_bool_env("FILC_RUN_GLOBAL_DTORS", true);
    
    if (filc_get_bool_env("FILC_DUMP_SETUP", false)) {
        pas_log("filc setup:\n");
        pas_log("    testing library: %s\n", PAS_ENABLE_TESTING ? "yes" : "no");
        pas_log("    exit on panic: %s\n", exit_on_panic ? "yes" : "no");
        pas_log("    dump errnos: %s\n", dump_errnos ? "yes" : "no");
        pas_log("    run global ctors: %s\n", run_global_ctors ? "yes" : "no");
        pas_log("    run global dtors: %s\n", run_global_dtors ? "yes" : "no");
        fugc_dump_setup();
    }
    
    is_initialized = true;
}

filc_thread* filc_get_my_thread(void)
{
    return (filc_thread*)pthread_getspecific(filc_thread_key);
}

void filc_assert_my_thread_is_not_entered(void)
{
    PAS_ASSERT(!filc_get_my_thread() || !filc_thread_is_entered(filc_get_my_thread()));
}

static void snapshot_threads(filc_thread*** threads, size_t* num_threads)
{
    filc_thread_list_lock_lock();
    *num_threads = 0;
    filc_thread* thread;
    for (thread = filc_first_thread; thread; thread = thread->next_thread) {
        if (thread == filc_first_thread)
            PAS_ASSERT(!thread->prev_thread);
        else
            PAS_ASSERT(thread->prev_thread->next_thread == thread);
        if (thread->next_thread)
            PAS_ASSERT(thread->next_thread->prev_thread == thread);
        (*num_threads)++;
    }
    /* NOTE: This barely works with fork! We snapshot exited, which disagrees with the idea that
       we can only bmalloc_allocate when entered. But, we snapshot when handshaking, an we cannot
       have handshakes in progress at time of fork, so it's fine. */
    *threads = (filc_thread**)bmalloc_allocate(sizeof(filc_thread*) * *num_threads);
    size_t index = 0;
    for (thread = filc_first_thread; thread; thread = thread->next_thread)
        (*threads)[index++] = thread;
    filc_thread_list_lock_unlock();
}

static bool participates_in_handshakes(filc_thread* thread)
{
    return thread->has_started;
}

static bool participates_in_pollchecks(filc_thread* thread)
{
    return participates_in_handshakes(thread) && !thread->is_stopping;
}

static void assert_participates_in_handshakes(filc_thread* thread)
{
    PAS_ASSERT(thread->has_started);
}

static void assert_participates_in_pollchecks(filc_thread* thread)
{
    assert_participates_in_handshakes(thread);
    PAS_ASSERT(!thread->is_stopping);
}

void filc_stop_the_world(void)
{
    static const bool verbose = false;
    
    filc_assert_my_thread_is_not_entered();
    filc_stop_the_world_lock_lock();
    if (filc_stop_the_world_count++) {
        filc_stop_the_world_lock_unlock();
        return;
    }
    
    sigset_t fullset;
    sigset_t oldset;
    pas_reasonably_fill_sigset(&fullset);
    if (verbose)
        pas_log("%s: blocking signals\n", __PRETTY_FUNCTION__);
    PAS_ASSERT(!pthread_sigmask(SIG_BLOCK, &fullset, &oldset));
    
    filc_thread** threads;
    size_t num_threads;
    snapshot_threads(&threads, &num_threads);

    size_t index;
    for (index = num_threads; index--;) {
        filc_thread* thread = threads[index];
        if (!participates_in_handshakes(thread))
            continue;

        pas_system_mutex_lock(&thread->lock);
        for (;;) {
            uint8_t old_state = thread->state;
            PAS_ASSERT(!(old_state & FILC_THREAD_STATE_STOP_REQUESTED));
            uint8_t new_state = old_state | FILC_THREAD_STATE_STOP_REQUESTED;
            if (pas_compare_and_swap_uint8_weak(&thread->state, old_state, new_state))
                break;
        }
        pas_system_mutex_unlock(&thread->lock);
    }

    for (index = num_threads; index--;) {
        filc_thread* thread = threads[index];
        if (!participates_in_handshakes(thread))
            continue;

        pas_system_mutex_lock(&thread->lock);
        while ((thread->state & FILC_THREAD_STATE_ENTERED))
            pas_system_condition_wait(&thread->cond, &thread->lock);
        pas_system_mutex_unlock(&thread->lock);
    }

    bmalloc_deallocate(threads);

    if (verbose)
        pas_log("%s: unblocking signals\n", __PRETTY_FUNCTION__);
    PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &oldset, NULL));

    filc_stop_the_world_lock_unlock();
}

void filc_resume_the_world(void)
{
    static const bool verbose = false;
    
    filc_assert_my_thread_is_not_entered();
    filc_stop_the_world_lock_lock();
    if (--filc_stop_the_world_count) {
        filc_stop_the_world_lock_unlock();
        return;
    }

    sigset_t fullset;
    sigset_t oldset;
    pas_reasonably_fill_sigset(&fullset);
    if (verbose)
        pas_log("%s: blocking signals\n", __PRETTY_FUNCTION__);
    PAS_ASSERT(!pthread_sigmask(SIG_BLOCK, &fullset, &oldset));
    
    filc_thread** threads;
    size_t num_threads;
    snapshot_threads(&threads, &num_threads);

    size_t index;
    for (index = num_threads; index--;) {
        filc_thread* thread = threads[index];
        if (!participates_in_handshakes(thread))
            continue;

        pas_system_mutex_lock(&thread->lock);
        for (;;) {
            uint8_t old_state = thread->state;
            PAS_ASSERT(old_state & FILC_THREAD_STATE_STOP_REQUESTED);
            uint8_t new_state = old_state & ~FILC_THREAD_STATE_STOP_REQUESTED;
            if (pas_compare_and_swap_uint8_weak(&thread->state, old_state, new_state))
                break;
        }
        pas_system_condition_broadcast(&thread->cond);
        pas_system_mutex_unlock(&thread->lock);
    }

    bmalloc_deallocate(threads);
    if (verbose)
        pas_log("%s: unblocking signals\n", __PRETTY_FUNCTION__);
    PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &oldset, NULL));
    pas_system_condition_broadcast(&filc_stop_the_world_cond);
    filc_stop_the_world_lock_unlock();
}

void filc_wait_for_world_resumption_holding_lock(void)
{
    filc_stop_the_world_lock_assert_held();
    while (filc_stop_the_world_count)
        pas_system_condition_wait(&filc_stop_the_world_cond, &filc_stop_the_world_lock);
}

static void run_pollcheck_callback(filc_thread* thread)
{
    /* Worth noting that this may run either with the thread having entered, or with the thread
       having exited. It doesn't matter.
    
       What matters is that we're holding the lock! */
    PAS_ASSERT(thread->state & FILC_THREAD_STATE_CHECK_REQUESTED);
    PAS_ASSERT(thread->pollcheck_callback);
    assert_participates_in_handshakes(thread);
    if (participates_in_pollchecks(thread))
        thread->pollcheck_callback(thread, thread->pollcheck_arg);
    thread->pollcheck_callback = NULL;
    thread->pollcheck_arg = NULL;
    for (;;) {
        uint8_t old_state = thread->state;
        PAS_ASSERT(old_state & FILC_THREAD_STATE_CHECK_REQUESTED);
        uint8_t new_state = old_state & ~FILC_THREAD_STATE_CHECK_REQUESTED;
        if (pas_compare_and_swap_uint8_weak(&thread->state, old_state, new_state))
            break;
    }
    PAS_ASSERT(!(thread->state & FILC_THREAD_STATE_CHECK_REQUESTED));
    PAS_ASSERT(!thread->pollcheck_callback);
    PAS_ASSERT(!thread->pollcheck_arg);
}

/* Returns true if the callback has run already (either because we ran it or because it ran already
   some other way.

   The thread's lock must be held to call this! */
static bool run_pollcheck_callback_from_handshake(filc_thread* thread)
{
    if (!(thread->state & FILC_THREAD_STATE_CHECK_REQUESTED)) {
        PAS_ASSERT(!thread->pollcheck_callback);
        PAS_ASSERT(!thread->pollcheck_arg);
        return true;
    }

    PAS_ASSERT(thread->pollcheck_callback);
    
    if (!(thread->state & FILC_THREAD_STATE_ENTERED)) {
        run_pollcheck_callback(thread);
        return true;
    }

    return false;
}

void filc_soft_handshake_no_op_callback(filc_thread* my_thread, void* arg)
{
    PAS_ASSERT(my_thread);
    PAS_ASSERT(!arg);
}

void filc_soft_handshake(void (*callback)(filc_thread* my_thread, void* arg), void* arg)
{
    static const bool verbose = false;

    filc_assert_my_thread_is_not_entered();
    filc_soft_handshake_lock_lock();

    sigset_t fullset;
    sigset_t oldset;
    pas_reasonably_fill_sigset(&fullset);
    if (verbose)
        pas_log("%s: blocking signals\n", __PRETTY_FUNCTION__);
    PAS_ASSERT(!pthread_sigmask(SIG_BLOCK, &fullset, &oldset));

    filc_thread** threads;
    size_t num_threads;
    snapshot_threads(&threads, &num_threads);

    /* Tell all the threads that the soft handshake is happening sort of as fast as we possibly
       can, so without calling the callback just yet. We want to maximize the window of time during
       which all threads know that they're supposed to do work for us.
    
       It's questionable if that buys us anything. It does create this kind of situation where
       filc_enter() has to consider the possibility of a pollcheck having been requested, which is
       perhaps awkward. */
    size_t index;
    for (index = num_threads; index--;) {
        filc_thread* thread = threads[index];
        if (!participates_in_handshakes(thread))
            continue;

        pas_system_mutex_lock(&thread->lock);
        PAS_ASSERT(!thread->pollcheck_callback);
        PAS_ASSERT(!thread->pollcheck_arg);
        thread->pollcheck_callback = callback;
        thread->pollcheck_arg = arg;

        for (;;) {
            uint8_t old_state = thread->state;
            PAS_ASSERT(!(old_state & FILC_THREAD_STATE_CHECK_REQUESTED));
            uint8_t new_state = old_state | FILC_THREAD_STATE_CHECK_REQUESTED;
            if (pas_compare_and_swap_uint8_weak(&thread->state, old_state, new_state))
                break;
        }
        pas_system_mutex_unlock(&thread->lock);
    }

    /* Try to run any callbacks we can run ourselves. In the time it takes us to do this, the threads
       that have to run the callbacks themselves might just end up doing it. */
    for (index = num_threads; index--;) {
        filc_thread* thread = threads[index];
        if (!participates_in_handshakes(thread))
            continue;
        
        pas_system_mutex_lock(&thread->lock);
        run_pollcheck_callback_from_handshake(thread);
        pas_system_mutex_unlock(&thread->lock);
    }

    /* Now actually wait for every thread to do it. */
    for (index = num_threads; index--;) {
        filc_thread* thread = threads[index];
        if (!participates_in_handshakes(thread))
            continue;
        
        pas_system_mutex_lock(&thread->lock);
        while (!run_pollcheck_callback_from_handshake(thread))
            pas_system_condition_wait(&thread->cond, &thread->lock);
        pas_system_mutex_unlock(&thread->lock);
    }
    
    bmalloc_deallocate(threads);
    if (verbose)
        pas_log("%s: unblocking signals\n", __PRETTY_FUNCTION__);
    PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &oldset, NULL));
    filc_soft_handshake_lock_unlock();
}

static void run_pollcheck_callback_if_necessary(filc_thread* my_thread)
{
    if (my_thread->state & FILC_THREAD_STATE_CHECK_REQUESTED) {
        run_pollcheck_callback(my_thread);
        pas_system_condition_broadcast(&my_thread->cond);
    }
}

static void stop_if_necessary(filc_thread* my_thread)
{
    while ((my_thread->state & FILC_THREAD_STATE_STOP_REQUESTED)) {
        PAS_ASSERT(!(my_thread->state & FILC_THREAD_STATE_ENTERED));
        pas_system_condition_wait(&my_thread->cond, &my_thread->lock);
    }
}

void filc_enter(filc_thread* my_thread)
{
    static const bool verbose = false;
    
    /* There's some future world where maybe we turn these into testing asserts. But for now, we only
       enter/exit for syscalls, so it probably doesn't matter and it's probably worth it to do a ton
       of assertions. */
    PAS_ASSERT(my_thread == filc_get_my_thread());
    PAS_ASSERT(!(my_thread->state & FILC_THREAD_STATE_DEFERRED_SIGNAL));
    PAS_ASSERT(!(my_thread->state & FILC_THREAD_STATE_ENTERED));
    
    for (;;) {
        uint8_t old_state = my_thread->state;
        PAS_ASSERT(!(old_state & FILC_THREAD_STATE_DEFERRED_SIGNAL));
        PAS_ASSERT(!(old_state & FILC_THREAD_STATE_ENTERED));
        if ((old_state & (FILC_THREAD_STATE_CHECK_REQUESTED |
                          FILC_THREAD_STATE_STOP_REQUESTED))) {
            /* NOTE: We could avoid doing this if the ENTERED state used by signal handling
               was separate from the ENTERED state used for all other purposes.
            
               Note sure it's worth it, since we would only get here for STOP (super rare) or
               for CHECK requests that happen while we're exited (super rare since that's a
               transient kind of state). */
            sigset_t fullset;
            sigset_t oldset;
            pas_reasonably_fill_sigset(&fullset);
            if (verbose)
                pas_log("%s: blocking signals\n", __PRETTY_FUNCTION__);
            PAS_ASSERT(!pthread_sigmask(SIG_BLOCK, &fullset, &oldset));
            pas_system_mutex_lock(&my_thread->lock);
            PAS_ASSERT(!(my_thread->state & FILC_THREAD_STATE_DEFERRED_SIGNAL));
            PAS_ASSERT(!(my_thread->state & FILC_THREAD_STATE_ENTERED));
            run_pollcheck_callback_if_necessary(my_thread);
            while ((my_thread->state & FILC_THREAD_STATE_STOP_REQUESTED)) {
                PAS_ASSERT(!(my_thread->state & FILC_THREAD_STATE_ENTERED));
                pas_system_condition_wait(&my_thread->cond, &my_thread->lock);
            }
            pas_system_mutex_unlock(&my_thread->lock);
            if (verbose)
                pas_log("%s: unblocking signals\n", __PRETTY_FUNCTION__);
            PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &oldset, NULL));
            continue;
        }

        uint8_t new_state = old_state | FILC_THREAD_STATE_ENTERED;
        if (pas_compare_and_swap_uint8_weak(&my_thread->state, old_state, new_state))
            break;
    }

    PAS_ASSERT((my_thread->state & FILC_THREAD_STATE_ENTERED));
}

static void call_signal_handler(filc_thread* my_thread, filc_signal_handler* handler, int signum)
{
    PAS_ASSERT(handler);
    PAS_ASSERT(handler->user_signum == signum);

    /* It's likely that we have a top native frame and it's not locked. Lock it to prevent assertions
       in that case. */
    bool was_top_native_frame_unlocked =
        my_thread->top_native_frame && !my_thread->top_native_frame->locked;
    if (was_top_native_frame_unlocked)
        filc_lock_top_native_frame(my_thread);

    FILC_DEFINE_RUNTIME_ORIGIN(origin, "call_signal_handler", 0);

    struct {
        FILC_FRAME_BODY;
    } actual_frame;
    pas_zero_memory(&actual_frame, sizeof(actual_frame));
    filc_frame* frame = (filc_frame*)&actual_frame;
    frame->origin = &origin;
    filc_push_frame(my_thread, frame);

    filc_native_frame native_frame;
    filc_push_native_frame(my_thread, &native_frame);

    /* Load the function from the handler first since as soon as we exit, the handler might get GC'd.
       Also, we're choosing not to rely on the fact that functions are global and we track them anyway. */
    filc_ptr function_ptr = filc_ptr_load(my_thread, &handler->function_ptr);

    filc_return_buffer return_buffer;
    filc_ptr rets = filc_ptr_for_int_return_buffer(&return_buffer);
    filc_ptr args = filc_ptr_create(my_thread, filc_allocate_int(my_thread, sizeof(int)));
    *(int*)filc_ptr_ptr(args) = signum;
    /* This check shouldn't be necessary; we do it out of an abundance of paranoia! */
    filc_check_function_call(function_ptr);
    bool (*function)(PIZLONATED_SIGNATURE) = (bool (*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(function_ptr);
    filc_lock_top_native_frame(my_thread);
    PAS_ASSERT(!function(my_thread, args, rets));
    filc_unlock_top_native_frame(my_thread);

    filc_pop_native_frame(my_thread, &native_frame);
    filc_pop_frame(my_thread, frame);

    if (was_top_native_frame_unlocked)
        filc_unlock_top_native_frame(my_thread);
}

static void handle_deferred_signals(filc_thread* my_thread)
{
    static const bool verbose = false;
    
    PAS_ASSERT(my_thread == filc_get_my_thread());
    PAS_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    for (;;) {
        uint8_t old_state = my_thread->state;
        if (!(old_state & FILC_THREAD_STATE_DEFERRED_SIGNAL))
            return;
        uint8_t new_state = old_state & ~FILC_THREAD_STATE_DEFERRED_SIGNAL;
        if (pas_compare_and_swap_uint8_weak(&my_thread->state, old_state, new_state))
            break;
    }

    size_t index;
    /* I'm guessing at some point I'll actually have to care about the order here? */
    for (index = FILC_MAX_USER_SIGNUM + 1; index--;) {
        uint64_t num_deferred_signals;
        /* We rely on the CAS for a fence, too. */
        for (;;) {
            num_deferred_signals = my_thread->num_deferred_signals[index];
            if (pas_compare_and_swap_uint64_weak(
                    my_thread->num_deferred_signals + index, num_deferred_signals, 0))
                break;
        }
        if (!num_deferred_signals)
            continue;

        if (verbose)
            pas_log("calling signal handler from pollcheck or exit\n");

        /* We're a bit unsafe here because the handler object might get collected at the next exit. */
        filc_signal_handler* handler = signal_table[index];
        PAS_ASSERT(handler);
        sigset_t oldset;
        if (verbose)
            pas_log("%s: blocking signals\n", __PRETTY_FUNCTION__);
        PAS_ASSERT(!pthread_sigmask(SIG_BLOCK, &handler->mask, &oldset));
        while (num_deferred_signals--)
            call_signal_handler(my_thread, handler, (int)index);
        if (verbose)
            pas_log("%s: unblocking signals\n", __PRETTY_FUNCTION__);
        PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &oldset, NULL));
    }
}

void filc_exit(filc_thread* my_thread)
{
    static const bool verbose = false;
    
    PAS_ASSERT(my_thread == filc_get_my_thread());
    PAS_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    
    for (;;) {
        uint8_t old_state = my_thread->state;
        PAS_ASSERT(old_state & FILC_THREAD_STATE_ENTERED);

        if (old_state & FILC_THREAD_STATE_DEFERRED_SIGNAL) {
            handle_deferred_signals(my_thread);
            continue;
        }

        if ((old_state & FILC_THREAD_STATE_CHECK_REQUESTED)) {
            pas_system_mutex_lock(&my_thread->lock);
            PAS_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
            run_pollcheck_callback_if_necessary(my_thread);
            pas_system_mutex_unlock(&my_thread->lock);
            continue;
        }

        PAS_ASSERT(!(old_state & FILC_THREAD_STATE_DEFERRED_SIGNAL));
        PAS_ASSERT(!(old_state & FILC_THREAD_STATE_CHECK_REQUESTED));
        uint8_t new_state = old_state & ~FILC_THREAD_STATE_ENTERED;
        if (pas_compare_and_swap_uint8_weak(&my_thread->state, old_state, new_state))
            break;
    }

    PAS_ASSERT(!(my_thread->state & FILC_THREAD_STATE_DEFERRED_SIGNAL));
    PAS_ASSERT(!(my_thread->state & FILC_THREAD_STATE_ENTERED));

    if ((my_thread->state & FILC_THREAD_STATE_STOP_REQUESTED)) {
        sigset_t fullset;
        sigset_t oldset;
        pas_reasonably_fill_sigset(&fullset);
        if (verbose)
            pas_log("%s: blocking signals\n", __PRETTY_FUNCTION__);
        PAS_ASSERT(!pthread_sigmask(SIG_BLOCK, &fullset, &oldset));
        pas_system_mutex_lock(&my_thread->lock);
        pas_system_condition_broadcast(&my_thread->cond);
        pas_system_mutex_unlock(&my_thread->lock);
        if (verbose)
            pas_log("%s: unblocking signals\n", __PRETTY_FUNCTION__);
        PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &oldset, NULL));
    }
}

void filc_increase_special_signal_deferral_depth(filc_thread* my_thread)
{
    PAS_ASSERT(my_thread == filc_get_my_thread());
    PAS_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    my_thread->special_signal_deferral_depth++;
    for (;;) {
        uint8_t old_state = my_thread->state;
        PAS_ASSERT(old_state & FILC_THREAD_STATE_ENTERED);
        if (!(old_state & FILC_THREAD_STATE_DEFERRED_SIGNAL))
            break;
        uint8_t new_state = old_state & ~FILC_THREAD_STATE_DEFERRED_SIGNAL;
        if (pas_compare_and_swap_uint8_weak(&my_thread->state, old_state, new_state)) {
            my_thread->have_deferred_signal_special = true;
            break;
        }
    }
}

void filc_decrease_special_signal_deferral_depth(filc_thread* my_thread)
{
    PAS_ASSERT(my_thread == filc_get_my_thread());
    PAS_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    PAS_ASSERT(my_thread->special_signal_deferral_depth);
    my_thread->special_signal_deferral_depth--;
    if (!my_thread->special_signal_deferral_depth && my_thread->have_deferred_signal_special) {
        for (;;) {
            uint8_t old_state = my_thread->state;
            PAS_ASSERT(old_state & FILC_THREAD_STATE_ENTERED);
            PAS_ASSERT(!(old_state & FILC_THREAD_STATE_DEFERRED_SIGNAL));
            uint8_t new_state = old_state | FILC_THREAD_STATE_DEFERRED_SIGNAL;
            if (pas_compare_and_swap_uint8_weak(&my_thread->state, old_state, new_state))
                break;
        }
    }
}

void filc_enter_with_allocation_root(filc_thread* my_thread, filc_object* allocation_root)
{
    filc_enter(my_thread);
    filc_pop_allocation_root(my_thread, allocation_root);
}

void filc_exit_with_allocation_root(filc_thread* my_thread, filc_object* allocation_root)
{
    filc_push_allocation_root(my_thread, allocation_root);
    filc_exit(my_thread);
}

void filc_ptr_array_add(filc_ptr_array* array, void* ptr)
{
    if (array->size >= array->capacity) {
        void** new_array;
        unsigned new_capacity;
        
        PAS_ASSERT(array->size == array->capacity);
        PAS_ASSERT(!pas_mul_uint32_overflow(array->capacity, 2, &new_capacity));

        new_array = bmalloc_allocate(sizeof(void*) * new_capacity);
        memcpy(new_array, array->array, sizeof(void*) * array->size);

        bmalloc_deallocate(array->array);
        array->array = new_array;
        array->capacity = new_capacity;
        PAS_ASSERT(array->size < array->capacity);
    }

    array->array[array->size++] = ptr;
}

static void enlarge_array(filc_object_array* array, size_t anticipated_size)
{
    PAS_ASSERT(anticipated_size > array->objects_capacity);
    
    filc_object** new_objects;
    size_t new_objects_capacity;
    PAS_ASSERT(!pas_mul_uintptr_overflow(anticipated_size, 3, &new_objects_capacity));
    new_objects_capacity /= 2;
    PAS_ASSERT(new_objects_capacity > array->objects_capacity);
    PAS_ASSERT(new_objects_capacity >= anticipated_size);
    size_t total_size;
    PAS_ASSERT(!pas_mul_uintptr_overflow(new_objects_capacity, sizeof(filc_object*), &total_size));
    new_objects = bmalloc_allocate(total_size);
    memcpy(new_objects, array->objects, array->num_objects * sizeof(filc_object*));
    bmalloc_deallocate(array->objects);
    array->objects = new_objects;
    array->objects_capacity = new_objects_capacity;
}

static void enlarge_array_if_necessary(filc_object_array* array, size_t anticipated_size)
{
    if (anticipated_size > array->objects_capacity)
        enlarge_array(array, anticipated_size);
}

void filc_object_array_push(filc_object_array* array, filc_object* object)
{
    enlarge_array_if_necessary(array, array->num_objects + 1);
    PAS_ASSERT(array->num_objects < array->objects_capacity);
    array->objects[array->num_objects++] = object;
}

void filc_object_array_push_all(filc_object_array* to, filc_object_array* from)
{
    size_t new_num_objects;
    PAS_ASSERT(!pas_add_uintptr_overflow(from->num_objects, to->num_objects, &new_num_objects));
    enlarge_array_if_necessary(to, new_num_objects);
    memcpy(to->objects + to->num_objects, from->objects, sizeof(filc_object*) * from->num_objects);
    to->num_objects += from->num_objects;
}

void filc_object_array_pop_all_from_and_push_to(filc_object_array* from, filc_object_array* to)
{
    if (!from->num_objects)
        return;

    if (!to->num_objects) {
        filc_object_array tmp;
        tmp = *to;
        *to = *from;
        *from = tmp;
        PAS_ASSERT(!from->num_objects);
        return;
    }

    filc_object_array_push_all(to, from);
    filc_object_array_reset(from);
}

void filc_object_array_reset(filc_object_array* array)
{
    filc_object_array_destruct(array);
    filc_object_array_construct(array);
}

void filc_push_native_frame(filc_thread* my_thread, filc_native_frame* frame)
{
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    filc_object_array_construct(&frame->array);
    filc_object_array_construct(&frame->pinned);
    filc_ptr_array_construct(&frame->to_bmalloc_deallocate);
    frame->locked = false;
    
    PAS_TESTING_ASSERT(my_thread->top_native_frame != frame);
    filc_assert_top_frame_locked(my_thread);
    frame->parent = my_thread->top_native_frame;
    my_thread->top_native_frame = frame;
}

void filc_pop_native_frame(filc_thread* my_thread, filc_native_frame* frame)
{
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    filc_object_array_destruct(&frame->array);

    size_t index;
    for (index = frame->pinned.num_objects; index--;)
        filc_unpin(frame->pinned.objects[index]);
    filc_object_array_destruct(&frame->pinned);

    for (index = frame->to_bmalloc_deallocate.size; index--;)
        bmalloc_deallocate(frame->to_bmalloc_deallocate.array[index]);
    filc_ptr_array_destruct(&frame->to_bmalloc_deallocate);
    
    PAS_TESTING_ASSERT(!frame->locked);
    
    PAS_TESTING_ASSERT(my_thread->top_native_frame == frame);
    my_thread->top_native_frame = frame->parent;
}

void filc_native_frame_add(filc_native_frame* frame, filc_object* object)
{
    PAS_ASSERT(!frame->locked);
    
    if (!object)
        return;

    filc_object_array_push(&frame->array, object);
}

void filc_native_frame_pin(filc_native_frame* frame, filc_object* object)
{
    PAS_ASSERT(!frame->locked);
    
    if (!object)
        return;

    filc_pin(object);
    filc_object_array_push(&frame->pinned, object);
}

void filc_native_frame_defer_bmalloc_deallocate(filc_native_frame* frame, void* bmalloc_object)
{
    PAS_ASSERT(!frame->locked);

    if (!bmalloc_object)
        return;

    filc_ptr_array_add(&frame->to_bmalloc_deallocate, bmalloc_object);
}

void filc_thread_track_object(filc_thread* my_thread, filc_object* object)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    PAS_TESTING_ASSERT(my_thread->top_native_frame);
    filc_native_frame_add(my_thread->top_native_frame, object);
}

void filc_defer_bmalloc_deallocate(filc_thread* my_thread, void* bmalloc_object)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    PAS_TESTING_ASSERT(my_thread->top_native_frame);
    filc_native_frame_defer_bmalloc_deallocate(my_thread->top_native_frame, bmalloc_object);
}

void* filc_bmalloc_allocate_tmp(filc_thread* my_thread, size_t size)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    void* result = bmalloc_allocate_zeroed(size);
    filc_defer_bmalloc_deallocate(my_thread, result);
    return result;
}

void filc_pollcheck_slow(filc_thread* my_thread, const filc_origin* origin)
{
    PAS_ASSERT(my_thread == filc_get_my_thread());
    PAS_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    if (origin && my_thread->top_frame)
        my_thread->top_frame->origin = origin;

    /* This could be made more efficient, but even if it was, we'd need to have an exit path for the
       STOP_REQUESTED case. */
    filc_exit(my_thread);
    filc_enter(my_thread);
}

void filc_pollcheck_outline(filc_thread* my_thread, const filc_origin* origin)
{
    filc_pollcheck(my_thread, origin);
}

void filc_thread_stop_allocators(filc_thread* my_thread)
{
    assert_participates_in_pollchecks(my_thread);

    pas_thread_local_cache_node* node = my_thread->tlc_node;
    uint64_t version = my_thread->tlc_node_version;
    if (node && version)
        verse_heap_thread_local_cache_node_stop_local_allocators(node, version);
}

void filc_thread_mark_roots(filc_thread* my_thread)
{
    static const bool verbose = false;
    
    assert_participates_in_pollchecks(my_thread);

    size_t index;
    for (index = my_thread->allocation_roots.num_objects; index--;) {
        filc_object* allocation_root = my_thread->allocation_roots.objects[index];
        /* Allocation roots have to have the mark bit set without being put on any mark stack, since
           they have no outgoing references and they are not ready for scanning. */
        verse_heap_set_is_marked_relaxed(allocation_root, true);
    }

    filc_frame* frame;
    for (frame = my_thread->top_frame; frame; frame = frame->parent) {
        PAS_ASSERT(frame->origin);
        PAS_ASSERT(frame->origin->function_origin);
        for (index = frame->origin->function_origin->num_objects; index--;) {
            if (verbose)
                pas_log("Marking thread root %p\n", frame->objects[index]);
            fugc_mark(&my_thread->mark_stack, frame->objects[index]);
        }
    }

    filc_native_frame* native_frame;
    for (native_frame = my_thread->top_native_frame; native_frame; native_frame = native_frame->parent) {
        for (index = native_frame->array.num_objects; index--;)
            fugc_mark(&my_thread->mark_stack, native_frame->array.objects[index]);

        /* In almost all cases where we pin, the object is already otherwise tracked. But we're going to
           be paranoid anyway because that's how we roll. */
        for (index = native_frame->pinned.num_objects; index--;)
            fugc_mark(&my_thread->mark_stack, native_frame->pinned.objects[index]);
    }

    for (index = FILC_NUM_UNWIND_REGISTERS; index--;)
        PAS_ASSERT(filc_ptr_is_totally_null(my_thread->unwind_registers[index]));
}

void filc_thread_sweep_mark_stack(filc_thread* my_thread)
{
    assert_participates_in_pollchecks(my_thread);

    if (my_thread->mark_stack.num_objects) {
        pas_log("Non-empty thread mark stack at start of sweep! Objects:\n");
        size_t index;
        for (index = 0; index < my_thread->mark_stack.num_objects; ++index) {
            filc_object_dump(my_thread->mark_stack.objects[index], &pas_log_stream.base);
            pas_log("\n");
        }
    }
    PAS_ASSERT(!my_thread->mark_stack.num_objects);
    filc_object_array_reset(&my_thread->mark_stack);
}

void filc_thread_donate(filc_thread* my_thread)
{
    assert_participates_in_pollchecks(my_thread);

    fugc_donate(&my_thread->mark_stack);
}

void filc_mark_global_roots(filc_object_array* mark_stack)
{
    size_t index;
    for (index = FILC_MAX_USER_SIGNUM + 1; index--;)
        fugc_mark(mark_stack, filc_object_for_special_payload(signal_table[index]));

    fugc_mark(mark_stack, filc_free_singleton);

    filc_global_initialization_lock_lock();
    /* Global roots point to filc_objects that are global, i.e. they are not GC-allocated, but they do
       have outgoing pointers. So, rather than fugc_marking them, we just shove them into the mark
       stack. */
    filc_object_array_push_all(mark_stack, &filc_global_variable_roots);
    filc_global_initialization_lock_unlock();

    filc_thread** threads;
    size_t num_threads;
    snapshot_threads(&threads, &num_threads);
    for (index = num_threads; index--;)
        fugc_mark(mark_stack, filc_object_for_special_payload(threads[index]));
    bmalloc_deallocate(threads);

    filc_mark_user_global_roots(mark_stack);
}

static void signal_pizlonator(int signum)
{
    static const bool verbose = false;
    
    int user_signum = filc_to_user_signum(signum);
    PAS_ASSERT((unsigned)user_signum <= FILC_MAX_USER_SIGNUM);
    filc_thread* thread = filc_get_my_thread();

    /* We're running on a thread that shouldn't be receiving signals or we're running in a thread
       that hasn't fully started.
       
       This shouldn't happen, because:
       
       - Service threads have all of our signals blocked from the start.
       
       - Newly created threads have signals blocked until they set the thread. */
    PAS_ASSERT(thread);
    
    if ((thread->state & FILC_THREAD_STATE_ENTERED) || thread->special_signal_deferral_depth) {
        /* For all we know the user asked for a mask that allows us to recurse, hence the lock-freedom. */
        for (;;) {
            uint64_t old_value = thread->num_deferred_signals[user_signum];
            if (pas_compare_and_swap_uint64_weak(
                    thread->num_deferred_signals + user_signum, old_value, old_value + 1))
                break;
        }
        if (thread->special_signal_deferral_depth) {
            thread->have_deferred_signal_special = true;
            return;
        }
        for (;;) {
            uint8_t old_state = thread->state;
            PAS_ASSERT(old_state & FILC_THREAD_STATE_ENTERED);
            if (old_state & FILC_THREAD_STATE_DEFERRED_SIGNAL)
                break;
            uint8_t new_state = old_state | FILC_THREAD_STATE_DEFERRED_SIGNAL;
            if (pas_compare_and_swap_uint8_weak(&thread->state, old_state, new_state))
                break;
        }
        return;
    }

    /* These shenanigans work only because if we ever grab the thread's lock, we are either entered
       (so we won't get here) or we block all signals (so we won't get here). */
    filc_enter(thread);
    /* Even if the signal mask allows the signal to recurse, at this point the signal_pizlonator
       will just count and defer. */

    if (verbose)
        pas_log("calling signal handler from pizlonator\n");
    
    call_signal_handler(thread, signal_table[user_signum], user_signum);

    filc_exit(thread);
}

void filc_origin_dump(const filc_origin* origin, pas_stream* stream)
{
    if (origin) {
        PAS_ASSERT(origin->function_origin);
        if (origin->function_origin->filename)
            pas_stream_printf(stream, "%s", origin->function_origin->filename);
        else
            pas_stream_printf(stream, "<somewhere>");
        if (origin->line) {
            pas_stream_printf(stream, ":%u", origin->line);
            if (origin->column)
                pas_stream_printf(stream, ":%u", origin->column);
        }
        if (origin->function_origin->function)
            pas_stream_printf(stream, ": %s", origin->function_origin->function);
    } else {
        /* FIXME: Maybe just assert that this doesn't happen? */
        pas_stream_printf(stream, "<null origin>");
    }
}

void filc_object_flags_dump_with_comma(filc_object_flags flags, bool* comma, pas_stream* stream)
{
    if (flags & FILC_OBJECT_FLAG_FREE) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "free");
    }
    if (flags & FILC_OBJECT_FLAG_RETURN_BUFFER) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "return_buffer");
    }
    if (flags & FILC_OBJECT_FLAG_SPECIAL) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "special");
    }
    if (flags & FILC_OBJECT_FLAG_GLOBAL) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "global");
    }
    if (flags & FILC_OBJECT_FLAG_MMAP) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "mmap");
    }
    if (flags & FILC_OBJECT_FLAG_READONLY) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "readonly");
    }
    if (flags >> FILC_OBJECT_FLAGS_PIN_SHIFT) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "pinned(%u)", flags >> FILC_OBJECT_FLAGS_PIN_SHIFT);
    }
}

void filc_object_flags_dump(filc_object_flags flags, pas_stream* stream)
{
    if (!flags) {
        pas_stream_printf(stream, "none");
        return;
    }
    bool comma = false;
    filc_object_flags_dump_with_comma(flags, &comma, stream);
}

void filc_object_dump_for_ptr(filc_object* object, void* ptr, pas_stream* stream)
{
    static const bool verbose = false;
    
    if (!object) {
        pas_stream_printf(stream, "<null>");
        return;
    }
    pas_stream_printf(stream, "%p,%p", object->lower, object->upper);
    bool comma = true;
    filc_object_flags_dump_with_comma(object->flags, &comma, stream);
    if (!filc_object_num_words(object)) {
        pas_stream_printf(stream, ",empty");
        return;
    }
    pas_stream_printf(stream, ",");
    size_t start_index;
    size_t end_index;
    size_t highlighted_index;
    bool has_highlighted_index;
    size_t index;
    size_t max_end_index;
    has_highlighted_index = false;
    max_end_index = filc_object_num_words(object) - 1;
    if (ptr < object->lower)
        highlighted_index = 0;
    else if (ptr >= object->upper)
        highlighted_index = max_end_index;
    else {
        highlighted_index = filc_object_word_type_index_for_ptr(object, ptr);
        has_highlighted_index = true;
    }
    PAS_ASSERT(highlighted_index < filc_object_num_words(object));
    PAS_ASSERT(highlighted_index <= max_end_index);
    /* FIXME: We really want a total context length and then if the ptr is on one end, then we pring
       more context on the other end. */
    static const size_t context_radius = 20;
    if (highlighted_index > context_radius)
        start_index = highlighted_index - context_radius;
    else
        start_index = 0;
    if (verbose) {
        pas_log("max_end_index = %zu, highlighted_index = %zu, context_radius = %zu\n",
                max_end_index, highlighted_index, context_radius);
    }
    if (max_end_index - highlighted_index > context_radius)
        end_index = highlighted_index + 1 + context_radius;
    else
        end_index = max_end_index;
    if (verbose) {
        pas_log("start_index = %zu\n", start_index);
        pas_log("end_index = %zu\n", end_index);
    }
    PAS_ASSERT(start_index < filc_object_num_words(object));
    PAS_ASSERT(end_index < filc_object_num_words(object));
    if (start_index)
        pas_stream_printf(stream, "...");
    for (index = start_index; index <= end_index; ++index) {
        if (has_highlighted_index && index == highlighted_index)
            pas_stream_printf(stream, "[");
        filc_word_type_dump(filc_object_get_word_type(object, index), stream);
        if (has_highlighted_index && index == highlighted_index)
            pas_stream_printf(stream, "]");
    }
    if (end_index < max_end_index)
        pas_stream_printf(stream, "...");
}

void filc_object_dump(filc_object* object, pas_stream* stream)
{
    filc_object_dump_for_ptr(object, NULL, stream);
}

void filc_ptr_dump(filc_ptr ptr, pas_stream* stream)
{
    pas_stream_printf(stream, "%p,", filc_ptr_ptr(ptr));
    filc_object_dump_for_ptr(filc_ptr_object(ptr), filc_ptr_ptr(ptr), stream);
}

static char* ptr_to_new_string_impl(filc_ptr ptr, pas_allocation_config* allocation_config)
{
    pas_string_stream stream;
    pas_string_stream_construct(&stream, allocation_config);
    filc_ptr_dump(ptr, &stream.base);
    return pas_string_stream_take_string(&stream);
}

char* filc_object_to_new_string(filc_object* object)
{
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);
    pas_string_stream stream;
    pas_string_stream_construct(&stream, &allocation_config);
    filc_object_dump(object, &stream.base);
    return pas_string_stream_take_string(&stream);
}

char* filc_ptr_to_new_string(filc_ptr ptr)
{
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);
    return ptr_to_new_string_impl(ptr, &allocation_config);
}

void filc_word_type_dump(filc_word_type type, pas_stream* stream)
{
    switch (type) {
    case FILC_WORD_TYPE_UNSET:
        pas_stream_printf(stream, "_");
        return;
    case FILC_WORD_TYPE_INT:
        pas_stream_printf(stream, "i");
        return;
    case FILC_WORD_TYPE_PTR:
        pas_stream_printf(stream, "P");
        return;
    case FILC_WORD_TYPE_FREE:
        pas_stream_printf(stream, "/");
        return;
    case FILC_WORD_TYPE_FUNCTION:
        pas_stream_printf(stream, "function");
        return;
    case FILC_WORD_TYPE_THREAD:
        pas_stream_printf(stream, "thread");
        return;
    case FILC_WORD_TYPE_DIRSTREAM:
        pas_stream_printf(stream, "dirstream");
        return;
    case FILC_WORD_TYPE_SIGNAL_HANDLER:
        pas_stream_printf(stream, "signal_handler");
        return;
    case FILC_WORD_TYPE_PTR_TABLE:
        pas_stream_printf(stream, "ptr_table");
        return;
    case FILC_WORD_TYPE_PTR_TABLE_ARRAY:
        pas_stream_printf(stream, "ptr_table_array");
        return;
    case FILC_WORD_TYPE_DL_HANDLE:
        pas_stream_printf(stream, "dl_handle");
        return;
    case FILC_WORD_TYPE_JMP_BUF:
        pas_stream_printf(stream, "jmp_buf");
        return;
    case FILC_WORD_TYPE_EXACT_PTR_TABLE:
        pas_stream_printf(stream, "exact_ptr_table");
        return;
    default:
        pas_stream_printf(stream, "?%u", type);
        return;
    }
}

char* filc_word_type_to_new_string(filc_word_type type)
{
    pas_string_stream stream;
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);
    pas_string_stream_construct(&stream, &allocation_config);
    filc_word_type_dump(type, &stream.base);
    return pas_string_stream_take_string(&stream);
}

void filc_store_barrier_slow(filc_thread* my_thread, filc_object* object)
{
    PAS_TESTING_ASSERT(!(object->flags & FILC_OBJECT_FLAG_RETURN_BUFFER));
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    fugc_mark(&my_thread->mark_stack, object);
}

void filc_store_barrier_outline(filc_thread* my_thread, filc_object* target)
{
    filc_store_barrier(my_thread, target);
}

void filc_check_access_common(filc_ptr ptr, size_t bytes, filc_access_kind access_kind,
                              const filc_origin* origin)
{
    if (PAS_ENABLE_TESTING)
        filc_validate_ptr(ptr, origin);

    FILC_CHECK(
        filc_ptr_object(ptr),
        origin,
        "cannot access pointer with null object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    
    FILC_CHECK(
        filc_ptr_ptr(ptr) >= filc_ptr_lower(ptr),
        origin,
        "cannot access pointer with ptr < lower (ptr = %s).", 
        filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        filc_ptr_ptr(ptr) < filc_ptr_upper(ptr),
        origin,
        "cannot access pointer with ptr >= upper (ptr = %s).",
        filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        bytes <= (uintptr_t)((char*)filc_ptr_upper(ptr) - (char*)filc_ptr_ptr(ptr)),
        origin,
        "cannot access %zu bytes when upper - ptr = %zu (ptr = %s).",
        bytes, (size_t)((char*)filc_ptr_upper(ptr) - (char*)filc_ptr_ptr(ptr)),
        filc_ptr_to_new_string(ptr));

    if (access_kind == filc_write_access) {
        FILC_CHECK(
            !(filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_READONLY),
            origin,
            "cannot write to read-only object (ptr = %s).",
            filc_ptr_to_new_string(ptr));
    }
}

void filc_check_access_special(filc_ptr ptr, filc_word_type word_type, const filc_origin* origin)
{
    PAS_ASSERT(filc_word_type_is_special(word_type));
    
    if (PAS_ENABLE_TESTING)
        filc_validate_ptr(ptr, origin);

    FILC_CHECK(
        filc_ptr_object(ptr),
        origin,
        "cannot access pointer with null object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    
    FILC_CHECK(
        filc_ptr_ptr(ptr) == filc_ptr_lower(ptr),
        origin,
        "cannot access pointer as %s with ptr != lower (ptr = %s).", 
        filc_word_type_to_new_string(word_type), filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_SPECIAL,
        origin,
        "cannot access pointer as %s, object isn't even special (ptr = %s).",
        filc_word_type_to_new_string(word_type), filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        filc_ptr_object(ptr)->word_types[0] == word_type,
        origin,
        "cannot access pointer as %s, object has wrong special type (ptr = %s).",
        filc_word_type_to_new_string(word_type), filc_ptr_to_new_string(ptr));
}

static void check_not_free(filc_ptr ptr, const filc_origin* origin)
{
    FILC_CHECK(
        !(filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_FREE),
        origin,
        "cannot access pointer to free object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
}

static void check_object_accessible(filc_object* object, const filc_origin* origin)
{
    FILC_CHECK(
        !(object->flags & (FILC_OBJECT_FLAG_FREE | FILC_OBJECT_FLAG_SPECIAL)),
        origin,
        "cannot access pointer to free or special object (object = %s).",
        filc_object_to_new_string(object));
}

static void check_accessible(filc_ptr ptr, const filc_origin* origin)
{
    FILC_CHECK(
        !(filc_ptr_object(ptr)->flags & (FILC_OBJECT_FLAG_FREE | FILC_OBJECT_FLAG_SPECIAL)),
        origin,
        "cannot access pointer to free or special object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
}

filc_ptr filc_get_next_bytes_for_va_arg(
    filc_thread* my_thread, filc_ptr ptr_ptr, size_t size, size_t alignment, const filc_origin* origin)
{
    filc_ptr ptr_value;
    uintptr_t ptr_as_int;
    filc_ptr result;

    filc_check_write_ptr(ptr_ptr, origin);
    filc_ptr* ptr = (filc_ptr*)filc_ptr_ptr(ptr_ptr);

    ptr_value = filc_ptr_load_with_manual_tracking(ptr);
    ptr_as_int = (uintptr_t)filc_ptr_ptr(ptr_value);
    ptr_as_int = pas_round_up_to_power_of_2(ptr_as_int, alignment);

    result = filc_ptr_with_ptr(ptr_value, (void*)ptr_as_int);

    filc_ptr_store(my_thread, ptr, filc_ptr_with_ptr(ptr_value, (char*)ptr_as_int + size));

    return result;
}

filc_object* filc_allocate_special_early(size_t size, filc_word_type word_type)
{
    /* NOTE: This cannot assert anything about the Fil-C thread because we do use this before any
       threads have been created. */

    /* NOTE: This must not exit, because we might hold rando locks while calling into this. */

    PAS_ASSERT(filc_word_type_is_special(word_type));

    pas_heap* heap;
    if (filc_special_word_type_has_destructor(word_type))
        heap = filc_destructor_heap;
    else
        heap = filc_default_heap;

    size_t total_size;
    PAS_ASSERT(!pas_add_uintptr_overflow(FILC_SPECIAL_OBJECT_SIZE, size, &total_size));

    filc_object* result = (filc_object*)verse_heap_allocate(heap, total_size);
    result->lower = (char*)result + FILC_SPECIAL_OBJECT_SIZE;
    result->upper = (char*)result + FILC_SPECIAL_OBJECT_SIZE + FILC_WORD_SIZE;
    result->flags = FILC_OBJECT_FLAG_SPECIAL;
    result->word_types[0] = word_type;
    pas_zero_memory(result->lower, size);
    pas_store_store_fence();

    return result;
}

filc_object* filc_allocate_special(filc_thread* my_thread, size_t size, filc_word_type word_type)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    return filc_allocate_special_early(size, word_type);
}

static void prepare_allocate_object(size_t* size, size_t* num_words, size_t* base_object_size)
{
    size_t original_size = *size;
    *size = pas_round_up_to_power_of_2(*size, FILC_WORD_SIZE);
    PAS_ASSERT(*size >= original_size);
    *num_words = filc_object_num_words_for_size(*size);
    PAS_ASSERT(!pas_add_uintptr_overflow(
                   PAS_OFFSETOF(filc_object, word_types), *num_words, base_object_size));
}

static void initialize_object_with_existing_data(filc_object* result, void* data, size_t size,
                                                 size_t num_words, filc_object_flags object_flags,
                                                 filc_word_type initial_word_type)
{
    result->lower = data;
    result->upper = (char*)data + size;
    result->flags = object_flags;

    size_t index;
    for (index = num_words; index--;)
        result->word_types[index] = initial_word_type;
}

filc_object* filc_allocate_with_existing_data(
    filc_thread* my_thread, void* data, size_t size, filc_object_flags object_flags,
    filc_word_type initial_word_type)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    PAS_ASSERT(!(object_flags & FILC_OBJECT_FLAG_FREE));
    PAS_ASSERT(!(object_flags & FILC_OBJECT_FLAG_RETURN_BUFFER));
    PAS_ASSERT(!(object_flags & FILC_OBJECT_FLAG_SPECIAL));
    PAS_ASSERT(!(object_flags & FILC_OBJECT_FLAG_GLOBAL));

    size_t num_words;
    size_t base_object_size;
    prepare_allocate_object(&size, &num_words, &base_object_size);
    filc_object* result = (filc_object*)verse_heap_allocate(filc_default_heap, base_object_size);
    if (size <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        initialize_object_with_existing_data(
            result, data, size, num_words, object_flags, initial_word_type);
    } else {
        filc_exit_with_allocation_root(my_thread, result);
        initialize_object_with_existing_data(
            result, data, size, num_words, object_flags, initial_word_type);
        filc_enter_with_allocation_root(my_thread, result);
    }
    return result;
}

filc_object* filc_allocate_special_with_existing_payload(
    filc_thread* my_thread, void* payload, filc_word_type word_type)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    PAS_ASSERT(word_type == FILC_WORD_TYPE_FUNCTION ||
               word_type == FILC_WORD_TYPE_DL_HANDLE);

    filc_object* result = (filc_object*)verse_heap_allocate(filc_default_heap, FILC_SPECIAL_OBJECT_SIZE);
    result->lower = payload;
    result->upper = (char*)payload + FILC_WORD_SIZE;
    result->flags = FILC_OBJECT_FLAG_SPECIAL;
    result->word_types[0] = word_type;
    pas_store_store_fence();
    return result;
}

static void prepare_allocate(size_t* size, size_t alignment,
                             size_t* num_words, size_t* offset_to_payload, size_t* total_size)
{
    size_t base_object_size;
    prepare_allocate_object(size, num_words, &base_object_size);
    *offset_to_payload = pas_round_up_to_power_of_2(base_object_size, alignment);
    PAS_ASSERT(*offset_to_payload >= base_object_size);
    PAS_ASSERT(!pas_add_uintptr_overflow(*offset_to_payload, *size, total_size));
}

static void initialize_object(filc_object* result, size_t size, size_t num_words,
                              size_t offset_to_payload, filc_object_flags object_flags,
                              filc_word_type initial_word_type)
{
    result->lower = (char*)result + offset_to_payload;
    result->upper = (char*)result + offset_to_payload + size;
    result->flags = object_flags;

    size_t index;
    for (index = num_words; index--;)
        result->word_types[index] = initial_word_type;
    
    pas_zero_memory((char*)result + offset_to_payload, size);
    
    pas_store_store_fence();
}

static filc_object* finish_allocate(
    filc_thread* my_thread, void* allocation, size_t size, size_t num_words, size_t offset_to_payload,
    filc_object_flags object_flags, filc_word_type initial_word_type)
{
    filc_object* result = (filc_object*)allocation;
    if (size <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS)
        initialize_object(result, size, num_words, offset_to_payload, object_flags, initial_word_type);
    else {
        filc_exit_with_allocation_root(my_thread, result);
        initialize_object(result, size, num_words, offset_to_payload, object_flags, initial_word_type);
        filc_enter_with_allocation_root(my_thread, result);
    }
    return result;
}

static filc_object* allocate_impl(
    filc_thread* my_thread, size_t size, filc_object_flags object_flags, filc_word_type initial_word_type)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    size_t num_words;
    size_t offset_to_payload;
    size_t total_size;
    prepare_allocate(&size, FILC_WORD_SIZE, &num_words, &offset_to_payload, &total_size);
    return finish_allocate(
        my_thread, verse_heap_allocate(filc_default_heap, total_size),
        size, num_words, offset_to_payload, object_flags, initial_word_type);
}

filc_object* filc_allocate(filc_thread* my_thread, size_t size)
{
    return allocate_impl(my_thread, size, 0, FILC_WORD_TYPE_UNSET);
}

filc_object* filc_allocate_with_alignment(filc_thread* my_thread, size_t size, size_t alignment)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    alignment = pas_max_uintptr(alignment, FILC_WORD_SIZE);
    size_t num_words;
    size_t offset_to_payload;
    size_t total_size;
    prepare_allocate(&size, alignment, &num_words, &offset_to_payload, &total_size);
    return finish_allocate(
        my_thread, verse_heap_allocate_with_alignment(filc_default_heap, total_size, alignment),
        size, num_words, offset_to_payload, 0, FILC_WORD_TYPE_UNSET);
}

filc_object* filc_allocate_int(filc_thread* my_thread, size_t size)
{
    return allocate_impl(my_thread, size, 0, FILC_WORD_TYPE_INT);
}

static filc_object* finish_reallocate(
    filc_thread* my_thread, void* allocation, filc_object* old_object, size_t new_size, size_t num_words,
    size_t offset_to_payload)
{
    static const bool verbose = false;

    if (verbose)
        pas_log("new_size = %zu\n", new_size);
    
    check_object_accessible(old_object, NULL);

    size_t old_num_words = filc_object_num_words(old_object);
    size_t old_size = filc_object_size(old_object);

    size_t common_num_words = pas_min_uintptr(num_words, old_num_words);
    size_t common_size = pas_min_uintptr(new_size, old_size);

    filc_object* result = (filc_object*)allocation;
    result->lower = (char*)result + offset_to_payload;
    result->upper = (char*)result + offset_to_payload + new_size;
    result->flags = 0;
    if (new_size > FILC_MAX_BYTES_BETWEEN_POLLCHECKS)
        filc_exit_with_allocation_root(my_thread, result);
    size_t index;
    for (index = num_words; index--;)
        result->word_types[index] = FILC_WORD_TYPE_UNSET;
    pas_zero_memory((char*)result + offset_to_payload, new_size);
    if (new_size > FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        filc_enter_with_allocation_root(my_thread, result);
        filc_thread_track_object(my_thread, result);
    }
    pas_uint128* dst = (pas_uint128*)((char*)result + offset_to_payload);
    pas_uint128* src = (pas_uint128*)old_object->lower;
    for (index = common_num_words; index--;) {
        for (;;) {
            filc_word_type word_type = old_object->word_types[index];
            /* Don't have to check for freeing here since old_object has to be a malloc object and
               those get freed by GC, so even if a free happened, we still have access to the memory. */
            pas_uint128 word = __c11_atomic_load((_Atomic pas_uint128*)(src + index), __ATOMIC_RELAXED);
            if (word_type == FILC_WORD_TYPE_UNSET) {
                if (word) {
                    /* We have surely raced between someone initializing the word to be not unset, and if
                       we try again we'll see it no longer unset. */
                    pas_fence();
                    continue;
                }
            }
            /* Surely need the barrier since the destination object is black and the source object is
               whatever. */
            if (word_type == FILC_WORD_TYPE_PTR) {
                filc_ptr ptr;
                ptr.word = word;
                filc_store_barrier(my_thread, filc_ptr_object(ptr));
            }
            /* No need for fences or anything like that since the object has not escaped; not even the
               GC can see it. That's because we don't have pollchecks or exits here, which is itself a
               perf bug, see above. */
            result->word_types[index] = word_type;
            __c11_atomic_store((_Atomic pas_uint128*)(dst + index), word, __ATOMIC_RELAXED);
            break;
        }
        if (new_size > FILC_MAX_BYTES_BETWEEN_POLLCHECKS)
            filc_pollcheck(my_thread, NULL);
    }

    pas_store_store_fence();
    filc_free(my_thread, old_object);

    return result;
}

filc_object* filc_reallocate(filc_thread* my_thread, filc_object* object, size_t new_size)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    size_t num_words;
    size_t offset_to_payload;
    size_t total_size;
    prepare_allocate(&new_size, FILC_WORD_SIZE, &num_words, &offset_to_payload, &total_size);
    return finish_reallocate(
        my_thread, verse_heap_allocate(filc_default_heap, total_size),
        object, new_size, num_words, offset_to_payload);
}

filc_object* filc_reallocate_with_alignment(filc_thread* my_thread, filc_object* object,
                                            size_t new_size, size_t alignment)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    alignment = pas_max_uintptr(alignment, FILC_WORD_SIZE);
    size_t num_words;
    size_t offset_to_payload;
    size_t total_size;
    prepare_allocate(&new_size, FILC_WORD_SIZE, &num_words, &offset_to_payload, &total_size);
    return finish_reallocate(
        my_thread, verse_heap_allocate_with_alignment(filc_default_heap, total_size, alignment),
        object, new_size, num_words, offset_to_payload);
}

void filc_free_yolo(filc_thread* my_thread, filc_object* object)
{
    for (;;) {
        filc_object_flags old_flags = object->flags;
        FILC_CHECK(
            !(old_flags & FILC_OBJECT_FLAG_FREE),
            NULL,
            "cannot free already free object %s.",
            filc_object_to_new_string(object));
        /* Technically, this check is only needed for mmap objects. */
        FILC_CHECK(
            !(old_flags >> FILC_OBJECT_FLAGS_PIN_SHIFT),
            NULL,
            "cannot free pinned object %s.",
            filc_object_to_new_string(object));
        PAS_TESTING_ASSERT(!(old_flags & FILC_OBJECT_FLAG_RETURN_BUFFER));
        filc_object_flags new_flags = old_flags | FILC_OBJECT_FLAG_FREE;
        if (pas_compare_and_swap_uint16_weak_relaxed(&object->flags, old_flags, new_flags))
            break;
    }
    if (filc_object_size(object) > FILC_MAX_BYTES_BETWEEN_POLLCHECKS)
        filc_exit(my_thread);
    size_t index;
    for (index = filc_object_num_words(object); index--;) {
        filc_word_type old_type = filc_object_get_word_type(object, index);
        PAS_TESTING_ASSERT(
            old_type == FILC_WORD_TYPE_UNSET ||
            old_type == FILC_WORD_TYPE_INT ||
            old_type == FILC_WORD_TYPE_PTR ||
            old_type == FILC_WORD_TYPE_DIRSTREAM);
        /* If this was a ptr, and now it's not, then this would be like overwriting a pointer, from
           the GC's standpoint. It's a pointer deletion. But we don't have a deletion barrier! So
           it's fine! */
        object->word_types[index] = FILC_WORD_TYPE_FREE;
    }
    if (filc_object_size(object) > FILC_MAX_BYTES_BETWEEN_POLLCHECKS)
        filc_enter(my_thread);
}

void filc_free(filc_thread* my_thread, filc_object* object)
{
    FILC_CHECK(
        !(object->flags & FILC_OBJECT_FLAG_SPECIAL),
        NULL,
        "cannot free special object %s.",
        filc_object_to_new_string(object));
    FILC_CHECK(
        !(object->flags & FILC_OBJECT_FLAG_GLOBAL),
        NULL,
        "cannot free global object %s.",
        filc_object_to_new_string(object));
    FILC_CHECK(
        !(object->flags & FILC_OBJECT_FLAG_MMAP),
        NULL,
        "cannot free mmap object %s.",
        filc_object_to_new_string(object));
    filc_free_yolo(my_thread, object);
}

static size_t num_ptrtables = 0;

filc_ptr_table* filc_ptr_table_create(filc_thread* my_thread)
{
    filc_ptr_table* result = (filc_ptr_table*)
        filc_allocate_special(my_thread, sizeof(filc_ptr_table), FILC_WORD_TYPE_PTR_TABLE)->lower;

    pas_lock_construct(&result->lock);
    filc_ptr_uintptr_hash_map_construct(&result->encode_map);
    result->free_indices_capacity = 10;
    result->free_indices = bmalloc_allocate(sizeof(uintptr_t) * result->free_indices_capacity);
    result->num_free_indices = 0;
    result->array = filc_ptr_table_array_create(my_thread, 10);

    if (PAS_ENABLE_TESTING)
        pas_atomic_exchange_add_uintptr(&num_ptrtables, 1);

    return result;
}

void filc_ptr_table_destruct(filc_ptr_table* ptr_table)
{
    static const bool verbose = false;
    if (verbose)
        pas_log("Destructing ptrtable\n");
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);
    filc_ptr_uintptr_hash_map_destruct(&ptr_table->encode_map, &allocation_config);
    bmalloc_deallocate(ptr_table->free_indices);

    if (PAS_ENABLE_TESTING)
        pas_atomic_exchange_add_uintptr(&num_ptrtables, (intptr_t)-1);
}

static uintptr_t ptr_table_encode_holding_lock(
    filc_thread* my_thread, filc_ptr_table* ptr_table, filc_ptr ptr)
{
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);

    PAS_ASSERT(ptr_table->array);

    filc_ptr_uintptr_hash_map_add_result add_result =
        filc_ptr_uintptr_hash_map_add(&ptr_table->encode_map, ptr, NULL, &allocation_config);
    if (!add_result.is_new_entry) {
        uintptr_t result = add_result.entry->value;
        PAS_ASSERT(result < ptr_table->array->num_entries);
        PAS_ASSERT(result < ptr_table->array->capacity);
        return (result + FILC_PTR_TABLE_OFFSET) << FILC_PTR_TABLE_SHIFT;
    }

    uintptr_t result;
    if (ptr_table->num_free_indices)
        result = ptr_table->free_indices[--ptr_table->num_free_indices];
    else {
        if (ptr_table->array->num_entries >= ptr_table->array->capacity) {
            PAS_ASSERT(ptr_table->array->num_entries == ptr_table->array->capacity);
            size_t new_capacity = ptr_table->array->capacity << 1;
            PAS_ASSERT(new_capacity > ptr_table->array->capacity);
            filc_ptr_table_array* new_array = filc_ptr_table_array_create(my_thread, new_capacity);

            /* There's some universe where we do this loop exited, but it probably just doesn't matter
               at all. */
            size_t index;
            for (index = ptr_table->array->num_entries; index--;) {
                filc_ptr_store(
                    my_thread, new_array->ptrs + index,
                    filc_ptr_load_with_manual_tracking(ptr_table->array->ptrs + index));
            }

            new_array->num_entries = ptr_table->array->num_entries;
            ptr_table->array = new_array;
        }

        PAS_ASSERT(ptr_table->array->num_entries < ptr_table->array->capacity);
        result = ptr_table->array->num_entries++;
    }

    PAS_ASSERT(result < ptr_table->array->num_entries);
    PAS_ASSERT(result < ptr_table->array->capacity);
    filc_ptr_store(my_thread, &add_result.entry->key, ptr);
    add_result.entry->value = result;
    filc_ptr_store(my_thread, ptr_table->array->ptrs + result, ptr);
    return (result + FILC_PTR_TABLE_OFFSET) << FILC_PTR_TABLE_SHIFT;
}

uintptr_t filc_ptr_table_encode(filc_thread* my_thread, filc_ptr_table* ptr_table, filc_ptr ptr)
{
    if (!filc_ptr_ptr(ptr) || !filc_ptr_object(ptr)
        || (filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_FREE))
        return 0;
    pas_lock_lock(&ptr_table->lock);
    uintptr_t result = ptr_table_encode_holding_lock(my_thread, ptr_table, ptr);
    pas_lock_unlock(&ptr_table->lock);
    return result;
}

filc_ptr filc_ptr_table_decode_with_manual_tracking(filc_ptr_table* ptr_table, uintptr_t encoded_ptr)
{
    filc_ptr_table_array* array = ptr_table->array;
    
    size_t index = (encoded_ptr >> FILC_PTR_TABLE_SHIFT) - FILC_PTR_TABLE_OFFSET;
    if (index >= array->num_entries)
        return filc_ptr_forge_null();

    /* NULL shouldn't have gotten this far. */
    PAS_TESTING_ASSERT(encoded_ptr);

    filc_ptr result = filc_ptr_load_with_manual_tracking(array->ptrs + index);
    if (!filc_ptr_ptr(result))
        return filc_ptr_forge_null();

    PAS_TESTING_ASSERT(filc_ptr_object(result));
    if (filc_ptr_object(result)->flags & FILC_OBJECT_FLAG_FREE)
        return filc_ptr_forge_null();

    return result;
}

filc_ptr filc_ptr_table_decode(filc_thread* my_thread, filc_ptr_table* ptr_table,
                               uintptr_t encoded_ptr)
{
    filc_ptr result = filc_ptr_table_decode_with_manual_tracking(ptr_table, encoded_ptr);
    filc_thread_track_object(my_thread, filc_ptr_object(result));
    return result;
}

void filc_ptr_table_mark_outgoing_ptrs(filc_ptr_table* ptr_table, filc_object_array* stack)
{
    static const bool verbose = false;
    if (verbose)
        pas_log("Marking ptr table at %p.\n", ptr_table);
    /* This needs to rehash the the whole table, marking non-free objects, and just skipping the free
       ones.
       
       Then it needs to walk the array and remove the free entries, putting their indices into the
       free_indices array.
    
       This may result in the hashtable and the array disagreeing a bit, and that's fine. They'll only
       disagree on things that are free.
    
       If the hashtable has an entry that the array doesn't have: this means that the object in question
       is free, so we'll never look up that entry in the hashtable due to the free check. New objects
       that take the same address will get a fresh entry in the hashtable and a fresh index.
    
       If the array has an entry that the hashtable doesn't have: decoding that object will fail the
       free check, so you won't be able to tell that the object has an index. Adding new objects that
       take the same address won't be able to reuse that index, because it'll seem to be taken. */

    pas_lock_lock(&ptr_table->lock);

    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);

    filc_ptr_uintptr_hash_map new_encode_map;
    filc_ptr_uintptr_hash_map_construct(&new_encode_map);
    size_t index;
    for (index = ptr_table->encode_map.table_size; index--;) {
        filc_ptr_uintptr_hash_map_entry entry = ptr_table->encode_map.table[index];
        if (filc_ptr_uintptr_hash_map_entry_is_empty_or_deleted(entry))
            continue;
        if (filc_ptr_object(entry.key)->flags & FILC_OBJECT_FLAG_FREE)
            continue;
        fugc_mark(stack, filc_ptr_object(entry.key));
        filc_ptr_uintptr_hash_map_add_new(&new_encode_map, entry, NULL, &allocation_config);
    }
    filc_ptr_uintptr_hash_map_destruct(&ptr_table->encode_map, &allocation_config);
    ptr_table->encode_map = new_encode_map;

    fugc_mark(stack, filc_object_for_special_payload(ptr_table->array));

    /* It's not necessary to mark entries in this array, since they'll be marked when we
       filc_ptr_table_array_mark_outgoing_ptrs(). It's not clear that we could avoid marking them in
       that function, though maybe we could avoid it. */
    for (index = ptr_table->array->num_entries; index--;) {
        filc_ptr ptr = filc_ptr_load_with_manual_tracking(ptr_table->array->ptrs + index);
        if (!filc_ptr_ptr(ptr))
            continue;
        if (!(filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_FREE))
            continue;
        if (ptr_table->num_free_indices >= ptr_table->free_indices_capacity) {
            PAS_ASSERT(ptr_table->num_free_indices == ptr_table->free_indices_capacity);

            size_t new_free_indices_capacity = ptr_table->free_indices_capacity << 1;
            PAS_ASSERT(new_free_indices_capacity > ptr_table->free_indices_capacity);

            uintptr_t* new_free_indices = bmalloc_allocate(sizeof(uintptr_t) * new_free_indices_capacity);
            memcpy(new_free_indices, ptr_table->free_indices,
                   sizeof(uintptr_t) * ptr_table->num_free_indices);

            bmalloc_deallocate(ptr_table->free_indices);
            ptr_table->free_indices = new_free_indices;
            ptr_table->free_indices_capacity = new_free_indices_capacity;
        }
        PAS_ASSERT(ptr_table->num_free_indices < ptr_table->free_indices_capacity);
        ptr_table->free_indices[ptr_table->num_free_indices++] = index;
        filc_ptr_store_without_barrier(ptr_table->array->ptrs + index, filc_ptr_forge_null());
    }
    
    pas_lock_unlock(&ptr_table->lock);
}

filc_ptr_table_array* filc_ptr_table_array_create(filc_thread* my_thread, size_t capacity)
{
    size_t array_size;
    PAS_ASSERT(!pas_mul_uintptr_overflow(sizeof(filc_ptr), capacity, &array_size));
    size_t total_size;
    PAS_ASSERT(!pas_add_uintptr_overflow(
                   PAS_OFFSETOF(filc_ptr_table_array, ptrs), array_size, &total_size));
    
    filc_ptr_table_array* result = (filc_ptr_table_array*)
        filc_allocate_special(my_thread, total_size, FILC_WORD_TYPE_PTR_TABLE_ARRAY)->lower;
    result->capacity = capacity;

    return result;
}

void filc_ptr_table_array_mark_outgoing_ptrs(filc_ptr_table_array* array, filc_object_array* stack)
{
    size_t index;
    for (index = array->num_entries; index--;)
        fugc_mark(stack, filc_ptr_object(filc_ptr_load_with_manual_tracking(array->ptrs + index)));
}

filc_exact_ptr_table* filc_exact_ptr_table_create(filc_thread* my_thread)
{
    filc_exact_ptr_table* result = (filc_exact_ptr_table*)filc_allocate_special(
        my_thread, sizeof(filc_exact_ptr_table), FILC_WORD_TYPE_EXACT_PTR_TABLE)->lower;

    pas_lock_construct(&result->lock);
    filc_uintptr_ptr_hash_map_construct(&result->decode_map);

    return result;
}

void filc_exact_ptr_table_destruct(filc_exact_ptr_table* ptr_table)
{
    static const bool verbose = false;
    if (verbose)
        pas_log("Destructing exact_ptrtable\n");
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);
    filc_uintptr_ptr_hash_map_destruct(&ptr_table->decode_map, &allocation_config);
}

uintptr_t filc_exact_ptr_table_encode(filc_thread* my_thread, filc_exact_ptr_table* ptr_table,
                                      filc_ptr ptr)
{
    if (!filc_ptr_object(ptr)
        || (filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_FREE))
        return (uintptr_t)filc_ptr_ptr(ptr);
    
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);

    filc_uintptr_ptr_hash_map_entry decode_entry;
    decode_entry.key = (uintptr_t)filc_ptr_ptr(ptr);
    filc_ptr_store(my_thread, &decode_entry.value, ptr);
    
    pas_lock_lock(&ptr_table->lock);
    filc_uintptr_ptr_hash_map_set(&ptr_table->decode_map, decode_entry, NULL, &allocation_config);
    pas_lock_unlock(&ptr_table->lock);
    
    return (uintptr_t)filc_ptr_ptr(ptr);
}

filc_ptr filc_exact_ptr_table_decode_with_manual_tracking(filc_exact_ptr_table* ptr_table,
                                                          uintptr_t encoded_ptr)
{
    if (!ptr_table->decode_map.key_count)
        return filc_ptr_forge_invalid((void*)encoded_ptr);
    pas_lock_lock(&ptr_table->lock);
    filc_uintptr_ptr_hash_map_entry result =
        filc_uintptr_ptr_hash_map_get(&ptr_table->decode_map, encoded_ptr);
    pas_lock_unlock(&ptr_table->lock);
    if (filc_ptr_is_totally_null(result.value))
        return filc_ptr_forge_invalid((void*)encoded_ptr);
    PAS_ASSERT((uintptr_t)filc_ptr_ptr(result.value) == encoded_ptr);
    return result.value;
}

filc_ptr filc_exact_ptr_table_decode(filc_thread* my_thread, filc_exact_ptr_table* ptr_table,
                                     uintptr_t encoded_ptr)
{
    filc_ptr result = filc_exact_ptr_table_decode_with_manual_tracking(ptr_table, encoded_ptr);
    filc_thread_track_object(my_thread, filc_ptr_object(result));
    return result;
}

void filc_exact_ptr_table_mark_outgoing_ptrs(filc_exact_ptr_table* ptr_table, filc_object_array* stack)
{
    static const bool verbose = false;
    if (verbose)
        pas_log("Marking exact ptr table at %p.\n", ptr_table);

    pas_lock_lock(&ptr_table->lock);
    
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);

    filc_uintptr_ptr_hash_map new_decode_map;
    filc_uintptr_ptr_hash_map_construct(&new_decode_map);
    size_t index;
    for (index = ptr_table->decode_map.table_size; index--;) {
        filc_uintptr_ptr_hash_map_entry entry = ptr_table->decode_map.table[index];
        if (filc_uintptr_ptr_hash_map_entry_is_empty_or_deleted(entry))
            continue;
        if (filc_ptr_object(entry.value)->flags & FILC_OBJECT_FLAG_FREE)
            continue;
        fugc_mark(stack, filc_ptr_object(entry.value));
        filc_uintptr_ptr_hash_map_add_new(&new_decode_map, entry, NULL, &allocation_config);
    }
    filc_uintptr_ptr_hash_map_destruct(&ptr_table->decode_map, &allocation_config);
    ptr_table->decode_map = new_decode_map;
    
    pas_lock_unlock(&ptr_table->lock);
}

void filc_pin(filc_object* object)
{
    if (!object)
        return;
    if (object->flags & FILC_OBJECT_FLAG_GLOBAL)
        return;
    for (;;) {
        filc_object_flags old_flags = object->flags;
        FILC_CHECK(
            !(old_flags & FILC_OBJECT_FLAG_FREE),
            NULL,
            "cannot pin free object %s.",
            filc_object_to_new_string(object));
        PAS_ASSERT(!(old_flags & FILC_OBJECT_FLAG_RETURN_BUFFER));
        filc_object_flags new_flags = old_flags + ((filc_object_flags)1 << FILC_OBJECT_FLAGS_PIN_SHIFT);
        FILC_CHECK(
            new_flags >> FILC_OBJECT_FLAGS_PIN_SHIFT,
            NULL,
            "pin count overflow in %s.",
            filc_object_to_new_string(object));
        if (pas_compare_and_swap_uint16_weak_relaxed(&object->flags, old_flags, new_flags))
            break;
    }
}

void filc_unpin(filc_object* object)
{
    if (!object)
        return;
    if (object->flags & FILC_OBJECT_FLAG_GLOBAL)
        return;
    for (;;) {
        filc_object_flags old_flags = object->flags;
        PAS_ASSERT(!(old_flags & FILC_OBJECT_FLAG_FREE)); /* should never happen */
        PAS_ASSERT(old_flags >> FILC_OBJECT_FLAGS_PIN_SHIFT);
        PAS_ASSERT(!(old_flags & FILC_OBJECT_FLAG_RETURN_BUFFER));
        PAS_ASSERT(old_flags >= ((filc_object_flags)1 << FILC_OBJECT_FLAGS_PIN_SHIFT));
        filc_object_flags new_flags = old_flags - ((filc_object_flags)1 << FILC_OBJECT_FLAGS_PIN_SHIFT);
        if (pas_compare_and_swap_uint16_weak_relaxed(&object->flags, old_flags, new_flags))
            break;
    }
}

void filc_pin_tracked(filc_thread* my_thread, filc_object* object)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    PAS_TESTING_ASSERT(my_thread->top_native_frame);
    filc_native_frame_pin(my_thread->top_native_frame, object);
}

filc_ptr filc_native_zgc_alloc(filc_thread* my_thread, size_t size)
{
    return filc_ptr_create_with_manual_tracking(filc_allocate(my_thread, size));
}

filc_ptr filc_native_zgc_aligned_alloc(filc_thread* my_thread, size_t alignment, size_t size)
{
    return filc_ptr_create_with_manual_tracking(filc_allocate_with_alignment(my_thread, size, alignment));
}

static filc_object* object_for_deallocate(filc_ptr ptr)
{
    FILC_CHECK(
        filc_ptr_object(ptr),
        NULL,
        "cannot free ptr with no object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        filc_ptr_ptr(ptr) == filc_ptr_lower(ptr),
        NULL,
        "cannot free ptr with ptr != lower (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    return filc_ptr_object(ptr);
}

filc_ptr filc_native_zgc_realloc(filc_thread* my_thread, filc_ptr old_ptr, size_t size)
{
    static const bool verbose = false;
    
    if (!filc_ptr_ptr(old_ptr))
        return filc_native_zgc_alloc(my_thread, size);
    if (verbose)
        pas_log("zrealloc to size = %zu\n", size);
    return filc_ptr_create_with_manual_tracking(
        filc_reallocate(my_thread, object_for_deallocate(old_ptr), size));
}

filc_ptr filc_native_zgc_aligned_realloc(filc_thread* my_thread,
                                         filc_ptr old_ptr, size_t alignment, size_t size)
{
    if (!filc_ptr_ptr(old_ptr))
        return filc_native_zgc_aligned_alloc(my_thread, alignment, size);
    return filc_ptr_create_with_manual_tracking(
        filc_reallocate_with_alignment(my_thread, object_for_deallocate(old_ptr), alignment, size));
}

void filc_native_zgc_free(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    if (!filc_ptr_ptr(ptr))
        return;
    filc_free(my_thread, object_for_deallocate(ptr));
}

filc_ptr filc_native_zgetlower(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    return filc_ptr_with_ptr(ptr, filc_ptr_lower(ptr));
}

filc_ptr filc_native_zgetupper(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    return filc_ptr_with_ptr(ptr, filc_ptr_upper(ptr));
}

bool filc_native_zhasvalidcap(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    return filc_ptr_object(ptr) && !(filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_FREE);
}

bool filc_native_zisunset(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_check_access_common(ptr, 1, filc_read_access, NULL);
    check_not_free(ptr, NULL);
    if (filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_SPECIAL)
        return false;
    uintptr_t offset = filc_ptr_offset(ptr);
    uintptr_t word_type_index = offset / FILC_WORD_SIZE;
    return filc_object_get_word_type(filc_ptr_object(ptr), word_type_index) == FILC_WORD_TYPE_UNSET;
}

bool filc_native_zisint(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_check_access_common(ptr, 1, filc_read_access, NULL);
    check_not_free(ptr, NULL);
    if (filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_SPECIAL)
        return false;
    uintptr_t offset = filc_ptr_offset(ptr);
    uintptr_t word_type_index = offset / FILC_WORD_SIZE;
    return filc_object_get_word_type(filc_ptr_object(ptr), word_type_index) == FILC_WORD_TYPE_INT;
}

int filc_native_zptrphase(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_check_access_common(ptr, 1, filc_read_access, NULL);
    check_not_free(ptr, NULL);
    if (filc_ptr_object(ptr)->flags & FILC_OBJECT_FLAG_SPECIAL)
        return -1;
    uintptr_t offset = filc_ptr_offset(ptr);
    uintptr_t word_type_index = offset / FILC_WORD_SIZE;
    uintptr_t offset_in_word = offset % FILC_WORD_SIZE;
    if (filc_object_get_word_type(filc_ptr_object(ptr), word_type_index) != FILC_WORD_TYPE_PTR)
        return -1;
    return (int)offset_in_word;
}

filc_ptr filc_native_zptrtable_new(filc_thread* my_thread)
{
    return filc_ptr_for_special_payload_with_manual_tracking(filc_ptr_table_create(my_thread));
}

size_t filc_native_zptrtable_encode(filc_thread* my_thread, filc_ptr table_ptr, filc_ptr ptr)
{
    filc_check_access_special(table_ptr, FILC_WORD_TYPE_PTR_TABLE, NULL);
    return filc_ptr_table_encode(my_thread, (filc_ptr_table*)filc_ptr_ptr(table_ptr), ptr);
}

filc_ptr filc_native_zptrtable_decode(filc_thread* my_thread, filc_ptr table_ptr, size_t encoded_ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_check_access_special(table_ptr, FILC_WORD_TYPE_PTR_TABLE, NULL);
    return filc_ptr_table_decode_with_manual_tracking(
        (filc_ptr_table*)filc_ptr_ptr(table_ptr), encoded_ptr);
}

filc_ptr filc_native_zexact_ptrtable_new(filc_thread* my_thread)
{
    return filc_ptr_for_special_payload_with_manual_tracking(filc_exact_ptr_table_create(my_thread));
}

size_t filc_native_zexact_ptrtable_encode(filc_thread* my_thread, filc_ptr table_ptr, filc_ptr ptr)
{
    filc_check_access_special(table_ptr, FILC_WORD_TYPE_EXACT_PTR_TABLE, NULL);
    return filc_exact_ptr_table_encode(my_thread, (filc_exact_ptr_table*)filc_ptr_ptr(table_ptr), ptr);
}

filc_ptr filc_native_zexact_ptrtable_decode(filc_thread* my_thread, filc_ptr table_ptr,
                                            size_t encoded_ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_check_access_special(table_ptr, FILC_WORD_TYPE_EXACT_PTR_TABLE, NULL);
    return filc_exact_ptr_table_decode_with_manual_tracking(
        (filc_exact_ptr_table*)filc_ptr_ptr(table_ptr), encoded_ptr);
}

size_t filc_native_ztesting_get_num_ptrtables(filc_thread* my_thread)
{
    PAS_UNUSED_PARAM(my_thread);
    return num_ptrtables;
}

void filc_validate_object(filc_object* object, const filc_origin* origin)
{
    if (object == filc_free_singleton) {
        FILC_ASSERT(!object->lower, origin);
        FILC_ASSERT(!object->upper, origin);
        FILC_ASSERT(object->flags == FILC_OBJECT_FLAG_FREE, origin);
        return;
    }

    FILC_ASSERT(object->lower, origin);
    FILC_ASSERT(object->upper, origin);
    
    if (object->flags & FILC_OBJECT_FLAG_SPECIAL) {
        FILC_ASSERT(object->upper == (char*)object->lower + FILC_WORD_SIZE, origin);
        FILC_ASSERT(object->word_types[0] == FILC_WORD_TYPE_FREE ||
                    object->word_types[0] == FILC_WORD_TYPE_FUNCTION ||
                    object->word_types[0] == FILC_WORD_TYPE_THREAD ||
                    object->word_types[0] == FILC_WORD_TYPE_DIRSTREAM ||
                    object->word_types[0] == FILC_WORD_TYPE_SIGNAL_HANDLER ||
                    object->word_types[0] == FILC_WORD_TYPE_PTR_TABLE ||
                    object->word_types[0] == FILC_WORD_TYPE_PTR_TABLE_ARRAY ||
                    object->word_types[0] == FILC_WORD_TYPE_DL_HANDLE ||
                    object->word_types[0] == FILC_WORD_TYPE_JMP_BUF ||
                    object->word_types[0] == FILC_WORD_TYPE_EXACT_PTR_TABLE,
                    origin);
        if (object->word_types[0] != FILC_WORD_TYPE_FUNCTION &&
            object->word_types[0] != FILC_WORD_TYPE_DL_HANDLE)
            FILC_ASSERT(pas_is_aligned((uintptr_t)object->lower, FILC_WORD_SIZE), origin);
        return;
    }

    FILC_ASSERT(pas_is_aligned((uintptr_t)object->lower, FILC_WORD_SIZE), origin);
    FILC_ASSERT(pas_is_aligned((uintptr_t)object->upper, FILC_WORD_SIZE), origin);
    FILC_ASSERT(object->upper >= object->lower, origin);

    size_t index;
    for (index = filc_object_num_words(object); index--;) {
        filc_word_type word_type = filc_object_get_word_type(object, index);
        FILC_ASSERT(word_type == FILC_WORD_TYPE_UNSET ||
                    word_type == FILC_WORD_TYPE_INT ||
                    word_type == FILC_WORD_TYPE_PTR ||
                    word_type == FILC_WORD_TYPE_FREE,
                    origin);
    }
}

void filc_validate_normal_object(filc_object* object, const filc_origin* origin)
{
    FILC_ASSERT(!(object->flags & FILC_OBJECT_FLAG_RETURN_BUFFER), origin);
    filc_validate_object(object, origin);
}

void filc_validate_return_buffer_object(filc_object* object, const filc_origin* origin)
{
    FILC_ASSERT(object->flags & FILC_OBJECT_FLAG_RETURN_BUFFER, origin);
    FILC_ASSERT(!(object->flags & FILC_OBJECT_FLAG_SPECIAL), origin);
    FILC_ASSERT(!(object->flags & FILC_OBJECT_FLAG_FREE), origin);
    filc_validate_object(object, origin);
}

void filc_validate_ptr(filc_ptr ptr, const filc_origin* origin)
{
    if (filc_ptr_is_boxed_int(ptr))
        return;

    filc_validate_object(filc_ptr_object(ptr), origin);
}

void filc_validate_normal_ptr(filc_ptr ptr, const filc_origin* origin)
{
    if (filc_ptr_is_boxed_int(ptr))
        return;

    filc_validate_normal_object(filc_ptr_object(ptr), origin);
}

void filc_validate_return_buffer_ptr(filc_ptr ptr, const filc_origin* origin)
{
    FILC_ASSERT(!filc_ptr_is_boxed_int(ptr), origin);
    filc_validate_return_buffer_object(filc_ptr_object(ptr), origin);
}

filc_ptr filc_native_zptr_to_new_string(filc_thread* my_thread, filc_ptr ptr)
{
    char* str = filc_ptr_to_new_string(ptr);
    filc_ptr result = filc_strdup(my_thread, str);
    bmalloc_deallocate(str);
    return result;
}

static void check_int(filc_ptr ptr, uintptr_t bytes, const filc_origin* origin)
{
    uintptr_t offset;
    uintptr_t first_word_type_index;
    uintptr_t last_word_type_index;
    uintptr_t word_type_index;

    offset = filc_ptr_offset(ptr);
    first_word_type_index = offset / FILC_WORD_SIZE;
    last_word_type_index = (offset + bytes - 1) / FILC_WORD_SIZE;

    /* FIXME: Eventually, we'll want this to exit.
     
       If we do make it exit, then we'll have to make sure that we check that the object is not
       FREE, since any exit might observe munmap. */

    for (word_type_index = first_word_type_index;
         word_type_index <= last_word_type_index;
         word_type_index++) {
        for (;;) {
            filc_word_type word_type = filc_object_get_word_type(filc_ptr_object(ptr), word_type_index);
            if (word_type == FILC_WORD_TYPE_UNSET) {
                if (pas_compare_and_swap_uint8_weak(
                        filc_ptr_object(ptr)->word_types + word_type_index,
                        FILC_WORD_TYPE_UNSET,
                        FILC_WORD_TYPE_INT))
                    break;
                continue;
            }
            
            FILC_CHECK(
                word_type == FILC_WORD_TYPE_INT,
                origin,
                "cannot access %zu bytes as int, span contains non-ints (ptr = %s).",
                bytes, filc_ptr_to_new_string(ptr));
            break;
        }
    }
}

void filc_check_access_int(filc_ptr ptr, size_t bytes, filc_access_kind access_kind,
                           const filc_origin* origin)
{
    if (!bytes)
        return;
    filc_check_access_common(ptr, bytes, access_kind, origin);
    check_int(ptr, bytes, origin);
}

void filc_check_access_ptr(filc_ptr ptr, filc_access_kind access_kind, const filc_origin* origin)
{
    uintptr_t offset;
    uintptr_t word_type_index;

    filc_check_access_common(ptr, sizeof(filc_ptr), access_kind, origin);

    offset = filc_ptr_offset(ptr);
    FILC_CHECK(
        pas_is_aligned(offset, FILC_WORD_SIZE),
        origin,
        "cannot access memory as ptr without 16-byte alignment; in this case ptr %% 16 = %zu (ptr = %s).",
        (size_t)(offset % FILC_WORD_SIZE), filc_ptr_to_new_string(ptr));
    word_type_index = offset / FILC_WORD_SIZE;
    
    for (;;) {
        filc_word_type word_type = filc_object_get_word_type(filc_ptr_object(ptr), word_type_index);
        if (word_type == FILC_WORD_TYPE_UNSET) {
            if (pas_compare_and_swap_uint8_weak(
                    filc_ptr_object(ptr)->word_types + word_type_index,
                    FILC_WORD_TYPE_UNSET,
                    FILC_WORD_TYPE_PTR))
                break;
            continue;
        }
        
        FILC_CHECK(
            word_type == FILC_WORD_TYPE_PTR,
            origin,
            "cannot access %zu bytes as ptr, word is non-ptr (ptr = %s).",
            FILC_WORD_SIZE, filc_ptr_to_new_string(ptr));
        break;
    }
}

void filc_cpt_access_int(filc_thread* my_thread, filc_ptr ptr, size_t bytes,
                         filc_access_kind access_kind)
{
    filc_check_access_int(ptr, bytes, access_kind, NULL);
    filc_pin_tracked(my_thread, filc_ptr_object(ptr));
}

void filc_check_read_int(filc_ptr ptr, size_t bytes, const filc_origin* origin)
{
    filc_check_access_int(ptr, bytes, filc_read_access, origin);
}

void filc_check_write_int(filc_ptr ptr, size_t bytes, const filc_origin* origin)
{
    filc_check_access_int(ptr, bytes, filc_write_access, origin);
}

void filc_check_read_ptr(filc_ptr ptr, const filc_origin* origin)
{
    filc_check_access_ptr(ptr, filc_read_access, origin);
}

void filc_check_write_ptr(filc_ptr ptr, const filc_origin* origin)
{
    filc_check_access_ptr(ptr, filc_write_access, origin);
}

void filc_cpt_read_int(filc_thread* my_thread, filc_ptr ptr, size_t bytes)
{
    filc_cpt_access_int(my_thread, ptr, bytes, filc_read_access);
}

void filc_cpt_write_int(filc_thread* my_thread, filc_ptr ptr, size_t bytes)
{
    filc_cpt_access_int(my_thread, ptr, bytes, filc_write_access);
}

void filc_check_function_call(filc_ptr ptr)
{
    filc_check_access_special(ptr, FILC_WORD_TYPE_FUNCTION, NULL);
}

void filc_check_pin_and_track_mmap(filc_thread* my_thread, filc_ptr ptr)
{
    filc_object* object = filc_ptr_object(ptr);
    PAS_ASSERT(object); /* To use this function you should have already checked that the ptr is
                           accessible. */
    FILC_CHECK(
        object->flags & FILC_OBJECT_FLAG_MMAP,
        NULL,
        "cannot perform this operation on something that was not mmapped (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        !(object->flags & FILC_OBJECT_FLAG_FREE),
        NULL,
        "cannot perform this operation on a free object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    filc_pin_tracked(my_thread, object);
}

void filc_memset_with_exit(
    filc_thread* my_thread, filc_object* object, void* ptr, unsigned value, size_t bytes)
{
    if (bytes <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        memset(ptr, value, bytes);
        return;
    }
    filc_pin(object);
    filc_exit(my_thread);
    memset(ptr, value, bytes);
    filc_enter(my_thread);
    filc_unpin(object);
}

void filc_memcpy_with_exit(
    filc_thread* my_thread, filc_object* dst_object, filc_object* src_object,
    void* dst, const void* src, size_t bytes)
{
    if (bytes <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        memcpy(dst, src, bytes);
        return;
    }
    filc_pin(dst_object);
    filc_pin(src_object);
    filc_exit(my_thread);
    memcpy(dst, src, bytes);
    filc_enter(my_thread);
    filc_unpin(dst_object);
    filc_unpin(src_object);
}

void filc_memmove_with_exit(
    filc_thread* my_thread, filc_object* dst_object, filc_object* src_object,
    void* dst, const void* src, size_t bytes)
{
    if (bytes <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        memmove(dst, src, bytes);
        return;
    }
    filc_pin(dst_object);
    filc_pin(src_object);
    filc_exit(my_thread);
    memmove(dst, src, bytes);
    filc_enter(my_thread);
    filc_unpin(dst_object);
    filc_unpin(src_object);
}

void filc_low_level_ptr_safe_bzero(void* raw_ptr, size_t bytes)
{
    static const bool verbose = false;
    size_t words;
    pas_uint128* ptr;
    if (verbose)
        pas_log("bytes = %zu\n", bytes);
    ptr = (pas_uint128*)raw_ptr;
    PAS_ASSERT(pas_is_aligned(bytes, FILC_WORD_SIZE));
    words = bytes / FILC_WORD_SIZE;
    while (words--)
        __c11_atomic_store((_Atomic pas_uint128*)ptr++, 0, __ATOMIC_RELAXED);
}

void filc_low_level_ptr_safe_bzero_with_exit(
    filc_thread* my_thread, filc_object* object, void* raw_ptr, size_t bytes)
{
    if (bytes <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        filc_low_level_ptr_safe_bzero(raw_ptr, bytes);
        return;
    }
    filc_pin(object);
    filc_exit(my_thread);
    filc_low_level_ptr_safe_bzero(raw_ptr, bytes);
    filc_enter(my_thread);
    filc_unpin(object);
}

void filc_memset(filc_thread* my_thread, filc_ptr ptr, unsigned value, size_t count,
                 const filc_origin* passed_origin)
{
    static const bool verbose = false;
    char* raw_ptr;
    
    if (!count)
        return;
    
    if (passed_origin)
        my_thread->top_frame->origin = passed_origin;

    FILC_DEFINE_RUNTIME_ORIGIN(origin, "memset", 1);
    struct {
        FILC_FRAME_BODY;
        filc_object* objects[1];
    } actual_frame;
    pas_zero_memory(&actual_frame, sizeof(actual_frame));
    filc_frame* frame = (filc_frame*)&actual_frame;
    frame->origin = &origin;
    frame->objects[0] = filc_ptr_object(ptr);
    filc_push_frame(my_thread, frame);
    
    raw_ptr = filc_ptr_ptr(ptr);
    
    if (verbose)
        pas_log("count = %zu\n", count);
    filc_check_access_common(ptr, count, filc_write_access, NULL);
    
    if (!value) {
        /* FIXME: If the hanging chads in this range are already UNSET, then we don't have to do
           anything. In particular, we could leave them UNSET and then skip the memset.
           
           But, we cnanot leave them UNSET and do the memset since that might race with someone
           converting the range to PTR and result in a partially-nulled ptr. */
        
        char* start = raw_ptr;
        char* end = raw_ptr + count;
        char* aligned_start = (char*)pas_round_up_to_power_of_2((uintptr_t)start, FILC_WORD_SIZE);
        char* aligned_end = (char*)pas_round_down_to_power_of_2((uintptr_t)end, FILC_WORD_SIZE);
        if (aligned_start > end || aligned_end < start) {
            check_int(ptr, count, NULL);
            memset(raw_ptr, 0, count);
            goto done;
        }
        if (aligned_start > start) {
            check_int(ptr, aligned_start - start, NULL);
            memset(start, 0, aligned_start - start);
        }
        check_accessible(ptr, NULL);
        filc_low_level_ptr_safe_bzero_with_exit(
            my_thread, filc_ptr_object(ptr), aligned_start, aligned_end - aligned_start);
        if (end > aligned_end) {
            check_int(filc_ptr_with_ptr(ptr, aligned_end), end - aligned_end, NULL);
            memset(aligned_end, 0, end - aligned_end);
        }
        goto done;
    } else {
        check_int(ptr, count, NULL);
        filc_memset_with_exit(my_thread, filc_ptr_object(ptr), raw_ptr, value, count);
    }

done:
    filc_pop_frame(my_thread, frame);
}

enum memmove_smidgen_part {
    memmove_lower_smidgen,
    memmove_upper_smidgen
};

typedef enum memmove_smidgen_part memmove_smidgen_part;

static void memmove_smidgen(memmove_smidgen_part part, filc_ptr dst, filc_ptr src,
                            char* dst_start, char* aligned_dst_start,
                            char* dst_end, char* aligned_dst_end,
                            char* src_start)
{
    switch (part) {
    case memmove_lower_smidgen:
        if (aligned_dst_start > dst_start) {
            check_int(dst, aligned_dst_start - dst_start, NULL);
            check_int(src, aligned_dst_start - dst_start, NULL);
            memmove(dst_start, src_start, aligned_dst_start - dst_start);
        }
        return;

    case memmove_upper_smidgen:
        if (dst_end > aligned_dst_end) {
            check_int(filc_ptr_with_ptr(dst, aligned_dst_end), dst_end - aligned_dst_end, NULL);
            check_int(filc_ptr_with_offset(src, aligned_dst_end - dst_start),
                      dst_end - aligned_dst_end, NULL);
            memmove(aligned_dst_end, src_start + (aligned_dst_end - dst_start),
                    dst_end - aligned_dst_end);
        }
        return;
    }
    PAS_ASSERT(!"Bad part");
}

PAS_ALWAYS_INLINE static void memmove_impl(filc_thread* my_thread, filc_ptr dst, filc_ptr src,
                                           size_t count, filc_barrier_mode barriered,
                                           filc_pollcheck_mode pollchecked)
{
    filc_object* dst_object = filc_ptr_object(dst);
    filc_object* src_object = filc_ptr_object(src);

    char* dst_start = filc_ptr_ptr(dst);
    char* src_start = filc_ptr_ptr(src);

    char* dst_end = dst_start + count;
    char* aligned_dst_start = (char*)pas_round_up_to_power_of_2((uintptr_t)dst_start, FILC_WORD_SIZE);
    char* aligned_dst_end = (char*)pas_round_down_to_power_of_2((uintptr_t)dst_end, FILC_WORD_SIZE);

    if (aligned_dst_start > dst_end || aligned_dst_end < dst_start) {
        check_int(dst, count, NULL);
        check_int(src, count, NULL);
        memmove(dst_start, src_start, count);
        return;
    }

    bool is_up = dst_start < src_start;

    memmove_smidgen(is_up ? memmove_lower_smidgen : memmove_upper_smidgen,
                    dst, src, dst_start, aligned_dst_start, dst_end, aligned_dst_end, src_start);

    bool src_can_has_ptrs =
        pas_modulo_power_of_2((uintptr_t)dst_start, FILC_WORD_SIZE) ==
        pas_modulo_power_of_2((uintptr_t)src_start, FILC_WORD_SIZE);

    check_accessible(dst, NULL);
    if (src_can_has_ptrs)
        check_accessible(src, NULL);
    else
        check_int(src, count, NULL);
    
    pas_uint128* cur_dst = (pas_uint128*)aligned_dst_start;
    pas_uint128* cur_src = (pas_uint128*)(src_start + (aligned_dst_start - dst_start));
    size_t cur_dst_word_index = ((char*)cur_dst - (char*)dst_object->lower) / FILC_WORD_SIZE;
    size_t cur_src_word_index = ((char*)cur_src - (char*)src_object->lower) / FILC_WORD_SIZE;
    size_t countdown = (aligned_dst_end - aligned_dst_start) / FILC_WORD_SIZE;

    if (!is_up) {
        cur_dst += countdown - 1;
        cur_src += countdown - 1;
        cur_dst_word_index += countdown - 1;
        cur_src_word_index += countdown - 1;
    }
    
    while (countdown--) {
        for (;;) {
            filc_word_type src_word_type;
            pas_uint128 src_word;
            if (src_can_has_ptrs) {
                src_word_type = filc_object_get_word_type(src_object, cur_src_word_index);
                src_word = __c11_atomic_load((_Atomic pas_uint128*)cur_src, __ATOMIC_RELAXED);
            } else {
                src_word_type = FILC_WORD_TYPE_INT;
                src_word = *cur_src;
            }
            if (!src_word) {
                /* copying an unset zero word is always legal to any destination type, no
                   problem. it's even OK to copy a zero into free memory. and there's zero value
                   in changing the destination type from unset to anything. */
                __c11_atomic_store((_Atomic pas_uint128*)cur_dst, 0, __ATOMIC_RELAXED);
                break;
            }
            if (src_word_type == FILC_WORD_TYPE_UNSET) {
                /* We have surely raced between someone initializing the word to be not unset, and if
                   we try again we'll see it no longer unset. */
                pas_fence();
                continue;
            }
            FILC_CHECK(
                src_word_type == FILC_WORD_TYPE_INT ||
                src_word_type == FILC_WORD_TYPE_PTR,
                NULL,
                "cannot copy anything but int or ptr (dst = %s, src = %s).",
                filc_ptr_to_new_string(filc_ptr_with_ptr(dst, cur_dst)),
                filc_ptr_to_new_string(filc_ptr_with_ptr(src, cur_src)));
            filc_word_type dst_word_type = filc_object_get_word_type(dst_object, cur_dst_word_index);
            if (dst_word_type == FILC_WORD_TYPE_UNSET) {
                if (!pas_compare_and_swap_uint8_weak(
                        dst_object->word_types + cur_dst_word_index,
                        FILC_WORD_TYPE_UNSET,
                        src_word_type))
                    continue;
            } else {
                FILC_CHECK(
                    src_word_type == dst_word_type,
                    NULL,
                    "type mismatch while copying (dst = %s, src = %s).",
                    filc_ptr_to_new_string(filc_ptr_with_ptr(dst, cur_dst)),
                    filc_ptr_to_new_string(filc_ptr_with_ptr(src, cur_src)));
            }
            if (src_word_type == FILC_WORD_TYPE_PTR && barriered) {
                filc_ptr ptr;
                ptr.word = src_word;
                filc_store_barrier(my_thread, filc_ptr_object(ptr));
            }
            __c11_atomic_store((_Atomic pas_uint128*)cur_dst, src_word, __ATOMIC_RELAXED);
            break;
        }
        if (is_up) {
            cur_dst++;
            cur_src++;
            cur_dst_word_index++;
            cur_src_word_index++;
        } else {
            cur_dst--;
            cur_src--;
            cur_dst_word_index--;
            cur_src_word_index--;
        }
        if (pollchecked && filc_pollcheck(my_thread, NULL)) {
            check_accessible(dst, NULL);
            check_accessible(src, NULL);
        }
    }
    
    memmove_smidgen(is_up ? memmove_upper_smidgen : memmove_lower_smidgen,
                    dst, src, dst_start, aligned_dst_start, dst_end, aligned_dst_end, src_start);
}

void filc_memmove(filc_thread* my_thread, filc_ptr dst, filc_ptr src, size_t count,
                  const filc_origin* passed_origin)
{
    static const bool verbose = false;
    
    if (!count)
        return;
    
    if (passed_origin)
        my_thread->top_frame->origin = passed_origin;
    
    filc_object* dst_object = filc_ptr_object(dst);
    filc_object* src_object = filc_ptr_object(src);

    FILC_DEFINE_RUNTIME_ORIGIN(origin, "memmove", 2);
    struct {
        FILC_FRAME_BODY;
        filc_object* objects[2];
    } actual_frame;
    pas_zero_memory(&actual_frame, sizeof(actual_frame));
    filc_frame* frame = (filc_frame*)&actual_frame;
    frame->origin = &origin;
    frame->objects[0] = dst_object;
    frame->objects[1] = src_object;
    filc_push_frame(my_thread, frame);
    
    filc_check_access_common(dst, count, filc_write_access, NULL);
    filc_check_access_common(src, count, filc_read_access, NULL);

    memmove_impl(my_thread, dst, src, count, filc_barriered, filc_pollchecked);

    filc_pop_frame(my_thread, frame);
}

filc_ptr filc_clone_readonly_for_zargs(filc_thread* my_thread, filc_ptr ptr)
{
    if (!filc_ptr_available(ptr))
        return filc_ptr_forge_null();

    FILC_DEFINE_RUNTIME_ORIGIN(origin, "zargs", 2);
    struct {
        FILC_FRAME_BODY;
        filc_object* objects[2];
    } actual_frame;
    pas_zero_memory(&actual_frame, sizeof(actual_frame));
    filc_frame* frame = (filc_frame*)&actual_frame;
    frame->origin = &origin;
    frame->objects[0] = filc_ptr_object(ptr);
    filc_push_frame(my_thread, frame);
    
    filc_check_access_common(ptr, filc_ptr_available(ptr), filc_read_access, NULL);
    
    filc_object* result_object = allocate_impl(
        my_thread, filc_ptr_available(ptr), FILC_OBJECT_FLAG_READONLY, FILC_WORD_TYPE_UNSET);
    frame->objects[1] = result_object;
    filc_ptr result = filc_ptr_create_with_manual_tracking(result_object);
    memmove_impl(my_thread, result, ptr, filc_ptr_available(ptr), filc_barriered, filc_pollchecked);

    filc_pop_frame(my_thread, frame);
    return result;
}

void filc_memcpy_for_zreturn(filc_thread* my_thread, filc_ptr dst, filc_ptr src, size_t count,
                             const filc_origin* passed_origin)
{
    PAS_ASSERT(filc_ptr_object(dst));
    PAS_ASSERT(filc_ptr_object(dst)->flags & FILC_OBJECT_FLAG_RETURN_BUFFER);

    if (!count)
        return;

    if (passed_origin)
        my_thread->top_frame->origin = passed_origin;

    FILC_DEFINE_RUNTIME_ORIGIN(origin, "zreturn", 1);
    struct {
        FILC_FRAME_BODY;
        filc_object* objects[1];
    } actual_frame;
    pas_zero_memory(&actual_frame, sizeof(actual_frame));
    filc_frame* frame = (filc_frame*)&actual_frame;
    frame->origin = &origin;
    frame->objects[0] = filc_ptr_object(src);
    filc_push_frame(my_thread, frame);

    filc_check_access_common(src, count, filc_read_access, NULL);
    memmove_impl(my_thread, dst, src, count, filc_unbarriered, filc_not_pollchecked);
    
    filc_pop_frame(my_thread, frame);
}

filc_ptr filc_native_zcall(filc_thread* my_thread, filc_ptr callee_ptr, filc_ptr args_ptr,
                           size_t ret_bytes)
{
    filc_check_function_call(callee_ptr);

    filc_ptr args_copy;
    if (!filc_ptr_available(args_ptr))
        args_copy = filc_ptr_forge_null();
    else {
        filc_check_access_common(args_ptr, filc_ptr_available(args_ptr), filc_read_access, NULL);

        /* It's weird but true that the ABI args are not readonly right now. Not a problem we need to
           solve yet. */
        args_copy = filc_ptr_create(my_thread, filc_allocate(my_thread, filc_ptr_available(args_ptr)));
        memmove_impl(my_thread, args_copy, args_ptr, filc_ptr_available(args_ptr), filc_barriered,
                     filc_pollchecked);
    }

    size_t num_words;
    size_t offset_to_payload;
    size_t total_size;
    prepare_allocate(&ret_bytes, FILC_WORD_SIZE, &num_words, &offset_to_payload, &total_size);
    PAS_ASSERT(FILC_WORD_SIZE == BMALLOC_MINALIGN_SIZE);
    filc_object* ret_object = (filc_object*)bmalloc_allocate(total_size);
    initialize_object(ret_object, ret_bytes, num_words, offset_to_payload, FILC_OBJECT_FLAG_RETURN_BUFFER,
                      FILC_WORD_TYPE_UNSET);
    filc_ptr rets = filc_ptr_create_with_manual_tracking(ret_object);

    /* We allocate the result_object before the call as a hack to avoid triggering a pollcheck between
       when the callee returns and when we grab its return values.

       NOTE: We could *almost* pass this as the return buffer, except:

       - This object is readonly, but the return buffer isn't.
       
       - When storing to this object, we need barriers, but the callee won't barrier when storing to
         its return buffer. */
    filc_ptr result = filc_ptr_create(
        my_thread, allocate_impl(my_thread, ret_bytes, FILC_OBJECT_FLAG_READONLY, FILC_WORD_TYPE_UNSET));

    filc_lock_top_native_frame(my_thread);
    bool (*callee)(PIZLONATED_SIGNATURE) = (bool (*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(callee_ptr);
    /* FIXME: The only things stopping us from allowing exceptions to be thrown are:

       - generate_pizlonated_forwarders.rb will say that our frame can't catch.

       - We'll need some way of deallocating ret_object upon unwind. */
    PAS_ASSERT(!callee(my_thread, args_copy, rets));
    filc_unlock_top_native_frame(my_thread);

    /* FIXME: This doesn't really need the full complexirty of memmove_impl, but who cares. */
    memmove_impl(my_thread, result, rets, ret_bytes, filc_barriered, filc_not_pollchecked);

    bmalloc_deallocate(ret_object);
    
    return result;
}

int filc_native_zmemcmp(filc_thread* my_thread, filc_ptr ptr1, filc_ptr ptr2, size_t count)
{
    if (!count)
        return 0;

    filc_check_access_common(ptr1, count, filc_read_access, NULL);
    filc_check_access_common(ptr2, count, filc_read_access, NULL);
    check_accessible(ptr1, NULL);
    check_accessible(ptr2, NULL);

    if (count <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS)
        return memcmp(filc_ptr_ptr(ptr1), filc_ptr_ptr(ptr2), count);

    filc_pin(filc_ptr_object(ptr1));
    filc_pin(filc_ptr_object(ptr2));
    filc_exit(my_thread);
    int result = memcmp(filc_ptr_ptr(ptr1), filc_ptr_ptr(ptr2), count);
    filc_enter(my_thread);
    filc_unpin(filc_ptr_object(ptr1));
    filc_unpin(filc_ptr_object(ptr2));
    return result;
}

static char* finish_check_and_get_new_str(char* base, size_t length)
{
    char* result = bmalloc_allocate(length + 1);
    memcpy(result, base, length + 1);
    FILC_ASSERT(!result[length], NULL);
    return result;
}

char* filc_check_and_get_new_str(filc_ptr str)
{
    size_t available;
    size_t length;
    filc_check_access_common(str, 1, filc_read_access, NULL);
    available = filc_ptr_available(str);
    length = strnlen((char*)filc_ptr_ptr(str), available);
    FILC_ASSERT(length < available, NULL);
    FILC_ASSERT(length + 1 <= available, NULL);
    check_int(str, length + 1, NULL);

    return finish_check_and_get_new_str((char*)filc_ptr_ptr(str), length);
}

char* filc_check_and_get_new_str_for_int_memory(char* base, size_t size)
{
    size_t length;
    FILC_ASSERT(size, NULL);
    length = strnlen(base, size);
    FILC_ASSERT(length < size, NULL);
    FILC_ASSERT(length + 1 <= size, NULL);

    return finish_check_and_get_new_str(base, length);
}

char* filc_check_and_get_new_str_or_null(filc_ptr ptr)
{
    if (filc_ptr_ptr(ptr))
        return filc_check_and_get_new_str(ptr);
    return NULL;
}

char* filc_check_and_get_tmp_str(filc_thread* my_thread, filc_ptr ptr)
{
    char* result = filc_check_and_get_new_str(ptr);
    filc_defer_bmalloc_deallocate(my_thread, result);
    return result;
}

char* filc_check_and_get_tmp_str_for_int_memory(filc_thread* my_thread, char* base, size_t size)
{
    char* result = filc_check_and_get_new_str_for_int_memory(base, size);
    filc_defer_bmalloc_deallocate(my_thread, result);
    return result;
}

char* filc_check_and_get_tmp_str_or_null(filc_thread* my_thread, filc_ptr ptr)
{
    char* result = filc_check_and_get_new_str_or_null(ptr);
    filc_defer_bmalloc_deallocate(my_thread, result);
    return result;
}

filc_ptr filc_strdup(filc_thread* my_thread, const char* str)
{
    if (!str)
        return filc_ptr_forge_null();
    size_t size = strlen(str) + 1;
    filc_ptr result = filc_ptr_create(my_thread, filc_allocate_int(my_thread, size));
    filc_memcpy_with_exit(my_thread, filc_ptr_object(result), NULL, filc_ptr_ptr(result), str, size);
    return result;
}

filc_global_initialization_context* filc_global_initialization_context_create(
    filc_global_initialization_context* parent)
{
    static const bool verbose = false;
    
    filc_global_initialization_context* result;

    if (verbose)
        pas_log("creating context with parent = %p\n", parent);
    
    if (parent) {
        parent->ref_count++;
        return parent;
    }

    /* Can't exit to grab this lock, because the GC might grab it, and we support running the GC in
       STW mode.
       
       Also, not need to exit to grab this lock, since we don't exit while the lock is held anyway. */
    filc_global_initialization_lock_lock();
    result = (filc_global_initialization_context*)
        bmalloc_allocate(sizeof(filc_global_initialization_context));
    if (verbose)
        pas_log("new context at %p\n", result);
    result->ref_count = 1;
    pas_ptr_hash_map_construct(&result->map);

    return result;
}

bool filc_global_initialization_context_add(
    filc_global_initialization_context* context, filc_ptr* pizlonated_gptr, filc_object* object)
{
    static const bool verbose = false;
    
    pas_allocation_config allocation_config;

    filc_global_initialization_lock_assert_held();
    filc_testing_validate_object(object, NULL);
    PAS_ASSERT(object->flags & FILC_OBJECT_FLAG_GLOBAL);

    if (verbose)
        pas_log("dealing with pizlonated_gptr = %p\n", pizlonated_gptr);

    filc_ptr gptr_value = filc_ptr_load_with_manual_tracking(pizlonated_gptr);
    if (filc_ptr_ptr(gptr_value)) {
        PAS_ASSERT(filc_ptr_lower(gptr_value) == filc_ptr_ptr(gptr_value));
        PAS_ASSERT(filc_ptr_object(gptr_value) == object);
        /* This case happens if there is a race like this:
           
           Thread #1: runs global fast path for g_foo, but it's NULL, so it starts to create its
                      context, but doesn't get as far as locking the lock.
           Thread #2: runs global fast path for g_foo, it's NULL, so it runs the initializer, including
                      locking/unlocking the initialization lock and all that.
           Thread #1: finally gets the lock and calls this function, and we find that the global is
                      already initialized. */
        if (verbose)
            pas_log("was already initialized\n");
        return false;
    }

    bmalloc_initialize_allocation_config(&allocation_config);

    if (verbose)
        pas_log("object = %s\n", filc_object_to_new_string(object));

    pas_ptr_hash_map_add_result add_result = pas_ptr_hash_map_add(
        &context->map, pizlonated_gptr, NULL, &allocation_config);
    if (!add_result.is_new_entry) {
        if (verbose)
            pas_log("was already seen\n");
        filc_object* existing_object = add_result.entry->value;
        PAS_ASSERT(existing_object == object);
        return false;
    }

    if (verbose)
        pas_log("going to initialize object = %s\n", filc_object_to_new_string(object));

    filc_object_array_push(&filc_global_variable_roots, object);

    add_result.entry->key = pizlonated_gptr;
    add_result.entry->value = object;

    return true;
}

void filc_global_initialization_context_destroy(filc_global_initialization_context* context)
{
    static const bool verbose = false;
    
    size_t index;
    pas_allocation_config allocation_config;

    PAS_ASSERT(context->ref_count);
    if (--context->ref_count)
        return;

    if (verbose)
        pas_log("destroying/comitting context at %p\n", context);
    
    pas_store_store_fence();

    for (index = context->map.table_size; index--;) {
        pas_ptr_hash_map_entry entry = context->map.table[index];
        if (pas_ptr_hash_map_entry_is_empty_or_deleted(entry))
            continue;
        filc_ptr* pizlonated_gptr = (filc_ptr*)entry.key;
        filc_object* object = (filc_object*)entry.value;
        PAS_TESTING_ASSERT(filc_ptr_is_totally_null(*pizlonated_gptr));
        filc_testing_validate_object(object, NULL);
        filc_ptr_store_without_barrier(pizlonated_gptr, filc_ptr_create_with_manual_tracking(object));
    }

    bmalloc_initialize_allocation_config(&allocation_config);

    pas_ptr_hash_map_destruct(&context->map, &allocation_config);
    bmalloc_deallocate(context);
    filc_global_initialization_lock_unlock();
}

static filc_ptr get_constant_value(filc_constant_kind kind, void* target,
                                   filc_global_initialization_context* context)
{
    switch (kind) {
    case filc_global_constant: {
        filc_ptr result = ((filc_ptr (*)(filc_global_initialization_context*))target)(context);
        PAS_ASSERT(filc_ptr_object(result));
        PAS_ASSERT(filc_ptr_ptr(result));
        return result;
    }
    case filc_expr_constant: {
        filc_constexpr_node* node = (filc_constexpr_node*)target;
        switch (node->opcode) {
        case filc_constexpr_add_ptr_immediate:
            return filc_ptr_with_offset(get_constant_value(node->left_kind, node->left_target, context),
                                        node->right_value);
        }
        PAS_ASSERT(!"Bad constexpr opcode");
    } }
    PAS_ASSERT(!"Bad relocation kind");
    return filc_ptr_forge_null();
}

void filc_execute_constant_relocations(
    void* constant, filc_constant_relocation* relocations, size_t num_relocations,
    filc_global_initialization_context* context)
{
    static const bool verbose = false;
    size_t index;
    PAS_ASSERT(context);
    if (verbose)
        pas_log("Executing constant relocations!\n");
    /* Nothing here needs to be atomic, since the constant doesn't become visible to the universe
       until the initialization context is destroyed. */
    for (index = num_relocations; index--;) {
        filc_constant_relocation* relocation;
        filc_ptr* ptr_ptr;
        relocation = relocations + index;
        PAS_ASSERT(pas_is_aligned(relocation->offset, FILC_WORD_SIZE));
        ptr_ptr = (filc_ptr*)((char*)constant + relocation->offset);
        PAS_ASSERT(filc_ptr_is_totally_null(*ptr_ptr));
        PAS_ASSERT(pas_is_aligned((uintptr_t)ptr_ptr, FILC_WORD_SIZE));
        filc_ptr_store_without_barrier(
            ptr_ptr, get_constant_value(relocation->kind, relocation->target, context));
    }
}

static bool did_run_deferred_global_ctors = false;
static bool (**deferred_global_ctors)(PIZLONATED_SIGNATURE) = NULL; 
static size_t num_deferred_global_ctors = 0;
static size_t deferred_global_ctors_capacity = 0;

static void run_global_ctor(filc_thread* my_thread, bool (*global_ctor)(PIZLONATED_SIGNATURE))
{
    if (!run_global_ctors) {
        pas_log("filc: skipping global ctor.\n");
        return;
    }

    FILC_DEFINE_RUNTIME_ORIGIN(origin, "run_global_ctor", 0);

    struct {
        FILC_FRAME_BODY;
    } actual_frame;
    pas_zero_memory(&actual_frame, sizeof(actual_frame));
    filc_frame* frame = (filc_frame*)&actual_frame;
    frame->origin = &origin;
    filc_push_frame(my_thread, frame);
    
    filc_return_buffer return_buffer;
    filc_lock_top_native_frame(my_thread);
    PAS_ASSERT(
        !global_ctor(my_thread, filc_ptr_forge_null(), filc_ptr_for_int_return_buffer(&return_buffer)));
    filc_unlock_top_native_frame(my_thread);

    filc_pop_frame(my_thread, frame);
}

void filc_defer_or_run_global_ctor(bool (*global_ctor)(PIZLONATED_SIGNATURE))
{
    filc_thread* my_thread = filc_get_my_thread();
    
    if (did_run_deferred_global_ctors) {
        filc_enter(my_thread);
        run_global_ctor(my_thread, global_ctor);
        filc_exit(my_thread);
        return;
    }

    if (num_deferred_global_ctors >= deferred_global_ctors_capacity) {
        bool (**new_deferred_global_ctors)(PIZLONATED_SIGNATURE);
        size_t new_deferred_global_ctors_capacity;

        PAS_ASSERT(num_deferred_global_ctors == deferred_global_ctors_capacity);

        new_deferred_global_ctors_capacity = (deferred_global_ctors_capacity + 1) * 2;
        new_deferred_global_ctors = (bool (**)(PIZLONATED_SIGNATURE))bmalloc_allocate(
            new_deferred_global_ctors_capacity * sizeof(bool (*)(PIZLONATED_SIGNATURE)));

        memcpy(new_deferred_global_ctors, deferred_global_ctors,
               num_deferred_global_ctors * sizeof(bool (*)(PIZLONATED_SIGNATURE)));

        bmalloc_deallocate(deferred_global_ctors);

        deferred_global_ctors = new_deferred_global_ctors;
        deferred_global_ctors_capacity = new_deferred_global_ctors_capacity;
    }

    deferred_global_ctors[num_deferred_global_ctors++] = global_ctor;
}

void filc_run_deferred_global_ctors(filc_thread* my_thread)
{
    FILC_CHECK(
        !did_run_deferred_global_ctors,
        NULL,
        "cannot run deferred global constructors twice.");
    did_run_deferred_global_ctors = true;
    /* It's important to run the destructors in exactly the order in which they were deferred, since
       this allows us to match the priority semantics despite not having the priority. */
    for (size_t index = 0; index < num_deferred_global_ctors; ++index)
        run_global_ctor(my_thread, deferred_global_ctors[index]);
    bmalloc_deallocate(deferred_global_ctors);
    num_deferred_global_ctors = 0;
    deferred_global_ctors_capacity = 0;
}

void filc_run_global_dtor(bool (*global_dtor)(PIZLONATED_SIGNATURE))
{
    if (!run_global_dtors) {
        pas_log("filc: skipping global dtor.\n");
        return;
    }
    
    filc_thread* my_thread = filc_get_my_thread();
    
    filc_enter(my_thread);

    FILC_DEFINE_RUNTIME_ORIGIN(origin, "run_global_dtor", 0);

    struct {
        FILC_FRAME_BODY;
    } actual_frame;
    pas_zero_memory(&actual_frame, sizeof(actual_frame));
    filc_frame* frame = (filc_frame*)&actual_frame;
    frame->origin = &origin;
    filc_push_frame(my_thread, frame);

    filc_return_buffer return_buffer;
    filc_lock_top_native_frame(my_thread);
    PAS_ASSERT(
        !global_dtor(my_thread, filc_ptr_forge_null(), filc_ptr_for_int_return_buffer(&return_buffer)));
    filc_unlock_top_native_frame(my_thread);

    filc_pop_frame(my_thread, frame);
    filc_exit(my_thread);
}

void filc_native_zrun_deferred_global_ctors(filc_thread* my_thread)
{
    filc_run_deferred_global_ctors(my_thread);
}

void filc_thread_dump_stack(filc_thread* thread, pas_stream* stream)
{
    filc_frame* frame;
    for (frame = thread->top_frame; frame; frame = frame->parent) {
        pas_stream_printf(stream, "    ");
        filc_origin_dump(frame->origin, stream);
        pas_stream_printf(stream, "\n");
    }
}

static void panic_impl(
    const filc_origin* origin, const char* prefix, const char* kind_string, const char* format,
    va_list args)
{
    filc_thread* my_thread = filc_get_my_thread();
    if (origin && my_thread->top_frame)
        my_thread->top_frame->origin = origin;
    pas_log("%s: ", prefix);
    pas_vlog(format, args);
    pas_log("\n");
    filc_thread_dump_stack(my_thread, &pas_log_stream.base);
    if (exit_on_panic) {
        pas_log("[%d] filc panic: %s\n", pas_getpid(), kind_string);
        _exit(42);
        PAS_ASSERT(!"Should not be reached");
    }
    pas_panic("%s\n", kind_string);
}

void filc_safety_panic(const filc_origin* origin, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    panic_impl(
        origin, "filc safety error", "thwarted a futile attempt to violate memory safety.", format, args);
}

void filc_internal_panic(const filc_origin* origin, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    panic_impl(origin, "filc internal error", "internal Fil-C error (it's probably a bug).", format, args);
}

void filc_user_panic(const filc_origin* origin, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    panic_impl(origin, "filc user error", "user thwarted themselves.", format, args);
}

void filc_error(const char* reason, const filc_origin* origin)
{
    filc_safety_panic(origin, "%s", reason);
}

void filc_system_mutex_lock(filc_thread* my_thread, pas_system_mutex* lock)
{
    if (pas_system_mutex_trylock(lock))
        return;

    filc_exit(my_thread);
    pas_system_mutex_lock(lock);
    filc_enter(my_thread);
}

static void print_str(const char* str)
{
    size_t length;
    length = strlen(str);
    while (length) {
        ssize_t result = write(2, str, length);
        PAS_ASSERT(result);
        if (result < 0 && errno == EINTR)
            continue;
        PAS_ASSERT(result > 0);
        PAS_ASSERT((size_t)result <= length);
        str += result;
        length -= result;
    }
}

void filc_native_zprint(filc_thread* my_thread, filc_ptr str_ptr)
{
    print_str(filc_check_and_get_tmp_str(my_thread, str_ptr));
}

void filc_native_zprint_long(filc_thread* my_thread, long x)
{
    PAS_UNUSED_PARAM(my_thread);
    char buf[100];
    snprintf(buf, sizeof(buf), "%ld", x);
    print_str(buf);
}

void filc_native_zprint_ptr(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    pas_allocation_config allocation_config;
    pas_string_stream stream;
    bmalloc_initialize_allocation_config(&allocation_config);
    pas_string_stream_construct(&stream, &allocation_config);
    filc_ptr_dump(ptr, &stream.base);
    print_str(pas_string_stream_get_string(&stream));
    pas_string_stream_destruct(&stream);
}

void filc_native_zerror(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    char* str = filc_check_and_get_new_str(ptr);
    filc_user_panic(NULL, "%s", str);
}

size_t filc_native_zstrlen(filc_thread* my_thread, filc_ptr ptr)
{
    return strlen(filc_check_and_get_tmp_str(my_thread, ptr));
}

int filc_native_zisdigit(filc_thread* my_thread, int chr)
{
    PAS_UNUSED_PARAM(my_thread);
    return isdigit(chr);
}

void filc_native_zfence(filc_thread* my_thread)
{
    PAS_UNUSED_PARAM(my_thread);
    pas_fence();
}

void filc_native_zstore_store_fence(filc_thread* my_thread)
{
    PAS_UNUSED_PARAM(my_thread);
    pas_store_store_fence();
}

void filc_native_zcompiler_fence(filc_thread* my_thread)
{
    PAS_UNUSED_PARAM(my_thread);
    pas_compiler_fence(); /* lmao we don't need this */
}

bool filc_native_zunfenced_weak_cas_ptr(
    filc_thread* my_thread, filc_ptr ptr, filc_ptr expected, filc_ptr new_value)
{
    filc_check_write_ptr(ptr, NULL);
    return filc_ptr_unfenced_weak_cas(my_thread, filc_ptr_ptr(ptr), expected, new_value);
}

bool filc_native_zweak_cas_ptr(filc_thread* my_thread, filc_ptr ptr, filc_ptr expected, filc_ptr new_value)
{
    filc_check_write_ptr(ptr, NULL);
    return filc_ptr_weak_cas(my_thread, filc_ptr_ptr(ptr), expected, new_value);
}

filc_ptr filc_native_zunfenced_strong_cas_ptr(
    filc_thread* my_thread, filc_ptr ptr, filc_ptr expected, filc_ptr new_value)
{
    filc_check_write_ptr(ptr, NULL);
    return filc_ptr_unfenced_strong_cas(my_thread, filc_ptr_ptr(ptr), expected, new_value);
}

filc_ptr filc_native_zstrong_cas_ptr(
    filc_thread* my_thread, filc_ptr ptr, filc_ptr expected, filc_ptr new_value)
{
    filc_check_write_ptr(ptr, NULL);
    return filc_ptr_strong_cas(my_thread, filc_ptr_ptr(ptr), expected, new_value);
}

filc_ptr filc_native_zunfenced_xchg_ptr(filc_thread* my_thread, filc_ptr ptr, filc_ptr new_value)
{
    filc_check_write_ptr(ptr, NULL);
    return filc_ptr_unfenced_xchg(my_thread, filc_ptr_ptr(ptr), new_value);
}

filc_ptr filc_native_zxchg_ptr(filc_thread* my_thread, filc_ptr ptr, filc_ptr new_value)
{
    filc_check_write_ptr(ptr, NULL);
    return filc_ptr_xchg(my_thread, filc_ptr_ptr(ptr), new_value);
}

void filc_native_zatomic_store_ptr(filc_thread* my_thread, filc_ptr ptr, filc_ptr new_value)
{
    filc_check_write_ptr(ptr, NULL);
    filc_ptr_store_fenced(my_thread, filc_ptr_ptr(ptr), new_value);
}

void filc_native_zunfenced_atomic_store_ptr(filc_thread* my_thread, filc_ptr ptr, filc_ptr new_value)
{
    filc_check_write_ptr(ptr, NULL);
    filc_ptr_store(my_thread, filc_ptr_ptr(ptr), new_value);
}

filc_ptr filc_native_zatomic_load_ptr(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_check_read_ptr(ptr, NULL);
    return filc_ptr_load_fenced_with_manual_tracking(filc_ptr_ptr(ptr));
}

filc_ptr filc_native_zunfenced_atomic_load_ptr(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_check_read_ptr(ptr, NULL);
    return filc_ptr_load_with_manual_tracking(filc_ptr_ptr(ptr));
}

bool filc_native_zis_runtime_testing_enabled(filc_thread* my_thread)
{
    PAS_UNUSED_PARAM(my_thread);
    return !!PAS_ENABLE_TESTING;
}

void filc_native_zvalidate_ptr(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_validate_ptr(ptr, NULL);
}

void filc_native_zgc_request_and_wait(filc_thread* my_thread)
{
    static const bool verbose = false;
    if (verbose)
        pas_log("Requesting GC and waiting.\n");
    filc_exit(my_thread);
    fugc_wait(fugc_request_fresh());
    filc_enter(my_thread);
    if (verbose)
        pas_log("Done with GC.\n");
}

void filc_native_zscavenge_synchronously(filc_thread* my_thread)
{
    filc_exit(my_thread);
    pas_scavenger_run_synchronously_now();
    filc_enter(my_thread);
}

void filc_native_zscavenger_suspend(filc_thread* my_thread)
{
    filc_exit(my_thread);
    pas_scavenger_suspend();
    filc_enter(my_thread);
}

void filc_native_zscavenger_resume(filc_thread* my_thread)
{
    filc_exit(my_thread);
    pas_scavenger_resume();
    filc_enter(my_thread);
}

struct stack_frame_description {
    filc_ptr function_name;
    filc_ptr filename;
    unsigned line;
    unsigned column;
    bool can_throw;
    bool can_catch;
    filc_ptr personality_function;
    filc_ptr eh_data;
};

typedef struct stack_frame_description stack_frame_description;

static void check_stack_frame_description(
    filc_ptr stack_frame_description_ptr, filc_access_kind access_kind)
{
    FILC_CHECK_PTR_FIELD(
        stack_frame_description_ptr, stack_frame_description, function_name, access_kind);
    FILC_CHECK_PTR_FIELD(stack_frame_description_ptr, stack_frame_description, filename, access_kind);
    FILC_CHECK_INT_FIELD(stack_frame_description_ptr, stack_frame_description, line, access_kind);
    FILC_CHECK_INT_FIELD(stack_frame_description_ptr, stack_frame_description, column, access_kind);
    FILC_CHECK_INT_FIELD(stack_frame_description_ptr, stack_frame_description, can_throw, access_kind);
    FILC_CHECK_INT_FIELD(stack_frame_description_ptr, stack_frame_description, can_catch, access_kind);
    FILC_CHECK_PTR_FIELD(
        stack_frame_description_ptr, stack_frame_description, personality_function, access_kind);
    FILC_CHECK_PTR_FIELD(stack_frame_description_ptr, stack_frame_description, eh_data, access_kind);
}

struct stack_scan_callback_args {
    filc_ptr description_ptr;
    filc_ptr arg_ptr;
};

typedef struct stack_scan_callback_args stack_scan_callback_args;

static void check_stack_scan_callback_args(
    filc_ptr stack_scan_callback_args_ptr, filc_access_kind access_kind)
{
    FILC_CHECK_PTR_FIELD(
        stack_scan_callback_args_ptr, stack_scan_callback_args, description_ptr, access_kind);
    FILC_CHECK_PTR_FIELD(stack_scan_callback_args_ptr, stack_scan_callback_args, arg_ptr, access_kind);
}

void filc_native_zstack_scan(filc_thread* my_thread, filc_ptr callback_ptr, filc_ptr arg_ptr)
{
    filc_check_function_call(callback_ptr);
    bool (*callback)(PIZLONATED_SIGNATURE) = (bool (*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(callback_ptr);

    filc_frame* my_frame = my_thread->top_frame;
    PAS_ASSERT(my_frame->origin);
    PAS_ASSERT(!strcmp(my_frame->origin->function_origin->function, "zstack_scan"));
    PAS_ASSERT(!strcmp(my_frame->origin->function_origin->filename, "<runtime>"));
    PAS_ASSERT(my_frame->parent);

    filc_frame* first_frame = my_frame->parent;
    filc_frame* current_frame;
    for (current_frame = first_frame; current_frame; current_frame = current_frame->parent) {
        PAS_ASSERT(current_frame->origin);
        filc_ptr description_ptr = filc_ptr_create(
            my_thread, filc_allocate(my_thread, sizeof(stack_frame_description)));
        check_stack_frame_description(description_ptr, filc_write_access);
        stack_frame_description* description = (stack_frame_description*)filc_ptr_ptr(description_ptr);
        filc_ptr_store(
            my_thread,
            &description->function_name,
            filc_strdup(my_thread, current_frame->origin->function_origin->function));
        filc_ptr_store(
            my_thread,
            &description->filename,
            filc_strdup(my_thread, current_frame->origin->function_origin->filename));
        description->line = current_frame->origin->line;
        description->column = current_frame->origin->column;
        description->can_throw = current_frame->origin->function_origin->can_throw;
        description->can_catch = current_frame->origin->function_origin->can_catch;
        filc_ptr personality_function;
        bool has_personality;
        if (current_frame->origin->function_origin->personality_getter) {
            has_personality = true;
            personality_function = current_frame->origin->function_origin->personality_getter(NULL);
        } else {
            has_personality = false;
            personality_function = filc_ptr_forge_null();
        }
        filc_ptr_store(my_thread, &description->personality_function, personality_function);
        filc_ptr eh_data;
        filc_origin_with_eh* origin_with_eh = (filc_origin_with_eh*)current_frame->origin;
        if (has_personality && origin_with_eh->eh_data_getter)
            eh_data = origin_with_eh->eh_data_getter(NULL);
        else
            eh_data = filc_ptr_forge_null();
        filc_ptr_store(my_thread, &description->eh_data, eh_data);

        filc_ptr args_ptr = filc_ptr_create(
            my_thread, filc_allocate(my_thread, sizeof(stack_scan_callback_args)));
        check_stack_scan_callback_args(args_ptr, filc_write_access);
        stack_scan_callback_args* args = (stack_scan_callback_args*)filc_ptr_ptr(args_ptr);
        filc_ptr_store(my_thread, &args->description_ptr, description_ptr);
        filc_ptr_store(my_thread, &args->arg_ptr, arg_ptr);

        filc_lock_top_native_frame(my_thread);
        filc_return_buffer return_buffer;
        filc_ptr rets_ptr = filc_ptr_for_int_return_buffer(&return_buffer);
        PAS_ASSERT(!callback(my_thread, args_ptr, rets_ptr));
        filc_unlock_top_native_frame(my_thread);
        if (!*(bool*)filc_ptr_ptr(rets_ptr))
            return;
    }
}

enum unwind_reason_code {
    unwind_reason_none = 0,
    unwind_reason_ok = 0,
    unwind_reason_foreign_exception_caught = 1,
    unwind_reason_fatal_phase2_error = 2,
    unwind_reason_fatal_phase1_error = 3,
    unwind_reason_normal_stop = 4,
    unwind_reason_end_of_stack = 5,
    unwind_reason_handler_found = 6,
    unwind_reason_install_context = 7,
    unwind_reason_continue_unwind = 8,
};

typedef enum unwind_reason_code unwind_reason_code;

enum unwind_action {
    unwind_action_search_phase = 1,
    unwind_action_cleanup_phase = 2,
    unwind_action_handler_frame = 4,
    unwind_action_force_unwind = 8,
    unwind_action_end_of_stack = 16
};

typedef enum unwind_action unwind_action;

struct unwind_context {
    filc_ptr language_specific_data;
    filc_ptr registers[FILC_NUM_UNWIND_REGISTERS];
};

typedef struct unwind_context unwind_context;

static void check_unwind_context(filc_ptr unwind_context_ptr, filc_access_kind access_kind)
{
    FILC_CHECK_PTR_FIELD(unwind_context_ptr, unwind_context, language_specific_data, access_kind);
    unsigned index;
    for (index = FILC_NUM_UNWIND_REGISTERS; index--;) {
        filc_check_access_ptr(
            filc_ptr_with_offset(
                unwind_context_ptr, PAS_OFFSETOF(unwind_context, registers) + index * sizeof(filc_ptr)),
            access_kind, NULL);
    }
}

typedef unsigned long long unwind_exception_class;

struct unwind_exception {
    unwind_exception_class exception_class;
    filc_ptr exception_cleanup;
};

typedef struct unwind_exception unwind_exception;

static void check_unwind_exception(filc_ptr unwind_exception_ptr, filc_access_kind access_kind)
{
    FILC_CHECK_INT_FIELD(unwind_exception_ptr, unwind_exception, exception_class, access_kind);
    FILC_CHECK_PTR_FIELD(unwind_exception_ptr, unwind_exception, exception_cleanup, access_kind);
}

struct unwind_personality_args {
    int version;
    unwind_action actions;
    unwind_exception_class exception_class;
    filc_ptr exception_object;
    filc_ptr context;
};

typedef struct unwind_personality_args unwind_personality_args;

static void check_unwind_personality_args(
    filc_ptr unwind_personality_args_ptr, filc_access_kind access_kind)
{
    FILC_CHECK_INT_FIELD(unwind_personality_args_ptr, unwind_personality_args, version, access_kind);
    FILC_CHECK_INT_FIELD(unwind_personality_args_ptr, unwind_personality_args, actions, access_kind);
    FILC_CHECK_INT_FIELD(
        unwind_personality_args_ptr, unwind_personality_args, exception_class, access_kind);
    FILC_CHECK_PTR_FIELD(
        unwind_personality_args_ptr, unwind_personality_args, exception_object, access_kind);
    FILC_CHECK_PTR_FIELD(unwind_personality_args_ptr, unwind_personality_args, context, access_kind);
}

static unwind_reason_code call_personality(
    filc_thread* my_thread, filc_frame* current_frame, int version, unwind_action actions,
    filc_ptr exception_object_ptr, filc_ptr context_ptr)
{
    check_unwind_context(context_ptr, filc_write_access);
    unwind_context* context = (unwind_context*)filc_ptr_ptr(context_ptr);

    filc_origin_with_eh* origin_with_eh = (filc_origin_with_eh*)current_frame->origin;
    filc_ptr (*eh_data_getter)(filc_global_initialization_context*) = origin_with_eh->eh_data_getter;
    filc_ptr eh_data;
    if (eh_data_getter)
        eh_data = eh_data_getter(NULL);
    else
        eh_data = filc_ptr_forge_null();
    filc_ptr_store(my_thread, &context->language_specific_data, eh_data);
    
    check_unwind_exception(exception_object_ptr, filc_read_access);
    unwind_exception* exception_object = (unwind_exception*)filc_ptr_ptr(exception_object_ptr);
    unwind_exception_class exception_class = exception_object->exception_class;

    filc_ptr personality_ptr = current_frame->origin->function_origin->personality_getter(NULL);
    filc_thread_track_object(my_thread, filc_ptr_object(personality_ptr));
    filc_check_function_call(personality_ptr);
    
    filc_ptr personality_args_ptr = filc_ptr_create(
        my_thread, filc_allocate(my_thread, sizeof(unwind_personality_args)));
    check_unwind_personality_args(personality_args_ptr, filc_write_access);
    unwind_personality_args* personality_args =
        (unwind_personality_args*)filc_ptr_ptr(personality_args_ptr);
    
    personality_args->version = version;
    personality_args->actions = actions;
    personality_args->exception_class = exception_class;
    filc_ptr_store(my_thread, &personality_args->exception_object, exception_object_ptr);
    filc_ptr_store(my_thread, &personality_args->context, context_ptr);
    
    filc_lock_top_native_frame(my_thread);
    filc_return_buffer return_buffer;
    filc_ptr personality_rets_ptr = filc_ptr_for_int_return_buffer(&return_buffer);
    PAS_ASSERT(!((bool (*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(personality_ptr))(
                   my_thread, personality_args_ptr, personality_rets_ptr));
    filc_unlock_top_native_frame(my_thread);
    
    return *(unwind_reason_code*)filc_ptr_ptr(personality_rets_ptr);
}

filc_exception_and_int filc_native__Unwind_RaiseException(
    filc_thread* my_thread, filc_ptr exception_object_ptr)
{
    filc_ptr context_ptr = filc_ptr_create(my_thread, filc_allocate(my_thread, sizeof(unwind_context)));

    filc_frame* my_frame = my_thread->top_frame;
    PAS_ASSERT(my_frame->origin);
    PAS_ASSERT(!strcmp(my_frame->origin->function_origin->function, "_Unwind_RaiseException"));
    PAS_ASSERT(!strcmp(my_frame->origin->function_origin->filename, "<runtime>"));
    PAS_ASSERT(my_frame->parent);

    filc_frame* first_frame = my_frame->parent;
    filc_frame* current_frame;

    /* Phase 1 */
    for (current_frame = first_frame; current_frame; current_frame = current_frame->parent) {
        PAS_ASSERT(current_frame->origin);
        
        if (!current_frame->origin->function_origin->can_catch)
            return filc_exception_and_int_with_int(unwind_reason_fatal_phase1_error);

        if (!current_frame->origin->function_origin->personality_getter) {
            if (!current_frame->origin->function_origin->can_throw)
                return filc_exception_and_int_with_int(unwind_reason_fatal_phase1_error);
            continue;
        }

        unwind_reason_code personality_result = call_personality(
            my_thread, current_frame, 1, unwind_action_search_phase, exception_object_ptr, context_ptr);
        if (personality_result == unwind_reason_handler_found) {
            my_thread->found_frame_for_unwind = current_frame;
            filc_ptr_store(my_thread, &my_thread->unwind_context_ptr, context_ptr);
            filc_ptr_store(my_thread, &my_thread->exception_object_ptr, exception_object_ptr);
            /* This triggers phase 2. */
            return filc_exception_and_int_with_exception();
        }

        if (personality_result == unwind_reason_continue_unwind
            && current_frame->origin->function_origin->can_throw)
            continue;

        return filc_exception_and_int_with_int(unwind_reason_fatal_phase1_error);
    }

    return filc_exception_and_int_with_int(unwind_reason_end_of_stack);
}

static bool landing_pad_impl(filc_thread* my_thread, filc_ptr context_ptr, filc_ptr exception_object_ptr,
                             filc_frame* found_frame, filc_frame* current_frame)
{
    /* Middle of Phase 2 */
    
    PAS_ASSERT(current_frame->origin);

    /* If the frame didn't support catching, then we wouldn't have gotten here. Only frames that
       support unwinding call landing_pads. */
    PAS_ASSERT(current_frame->origin->function_origin->can_catch);
    
    if (!current_frame->origin->function_origin->personality_getter)
        return false;

    unwind_action action = unwind_action_cleanup_phase;
    if (current_frame == found_frame)
        action = (unwind_action)(unwind_action_cleanup_phase | unwind_action_handler_frame);
    
    unwind_reason_code personality_result = call_personality(
        my_thread, current_frame, 1, action, exception_object_ptr, context_ptr);
    if (personality_result == unwind_reason_continue_unwind)
        return false;

    FILC_CHECK(
        personality_result == unwind_reason_install_context,
        NULL,
        "personality function returned neither continue_unwind nor install_context.");

    check_unwind_context(context_ptr, filc_write_access);
    unwind_context* context = (unwind_context*)filc_ptr_ptr(context_ptr);
    unsigned index;
    for (index = FILC_NUM_UNWIND_REGISTERS; index--;) {
        PAS_ASSERT(filc_ptr_is_totally_null(my_thread->unwind_registers[index]));
        my_thread->unwind_registers[index] = filc_ptr_load(my_thread, context->registers + index);
    }
    return true;
}

bool filc_landing_pad(filc_thread* my_thread)
{
    filc_frame* current_frame = my_thread->top_frame;
    PAS_ASSERT(current_frame);
    
    FILC_DEFINE_RUNTIME_ORIGIN(origin, "landing_pad", 0);
    struct {
        FILC_FRAME_BODY;
    } actual_frame;
    pas_zero_memory(&actual_frame, sizeof(actual_frame));
    filc_frame* frame = (filc_frame*)&actual_frame;
    frame->origin = &origin;
    filc_push_frame(my_thread, frame);
    PAS_ASSERT(current_frame == frame->parent);

    filc_native_frame native_frame;
    filc_push_native_frame(my_thread, &native_frame);

    filc_ptr context_ptr = filc_ptr_load(my_thread, &my_thread->unwind_context_ptr);
    filc_ptr exception_object_ptr = filc_ptr_load(my_thread, &my_thread->exception_object_ptr);
    filc_frame* found_frame = my_thread->found_frame_for_unwind;

    bool result = landing_pad_impl(
        my_thread, context_ptr, exception_object_ptr, found_frame, current_frame);
    /* Super important that between here and the return, we do NOT pollcheck or exit. Otherwise, the
       GC will miss the unwind_registers. */
    if (!result) {
        FILC_CHECK(
            current_frame != found_frame,
            NULL,
            "personality function told us to continue phase2 unwinding past the frame found in phase1.");
        PAS_ASSERT(current_frame->origin->function_origin->can_catch);
        FILC_CHECK(
            current_frame->origin->function_origin->can_throw,
            NULL,
            "cannot unwind from landing pad, function claims not to throw.");
        FILC_CHECK(
            current_frame->parent,
            NULL,
            "cannot unwind from landing pad, reached end of stack.");
        FILC_CHECK(
            current_frame->parent->origin->function_origin->can_catch,
            NULL,
            "cannot unwind from landing pad, parent frame doesn't support catching.");
    }

    filc_pop_native_frame(my_thread, &native_frame);
    filc_pop_frame(my_thread, frame);

    return result;
}

void filc_resume_unwind(filc_thread* my_thread, filc_origin *origin)
{
    filc_frame* current_frame = my_thread->top_frame;

    /* The compiler always passes non-NULL, but I'm going to keep following the convention that
       runtime functions that taken an origin can take NULL to indicate that the origin has
       already been set. */
    if (origin)
        current_frame->origin = origin;

    /* The frame has to have an origin (maybe because we set it). */
    PAS_ASSERT(current_frame->origin);

    /* NOTE: We cannot assert that the origin catches, because the origin corresponds to the
       resume instruction. The resume instruction doesn't "catch". */

    FILC_CHECK(
        current_frame->origin->function_origin->can_throw,
        NULL,
        "cannot resume unwinding, current frame claims not to throw.");
    FILC_CHECK(
        current_frame->parent,
        NULL,
        "cannot resume unwinding, reached end of stack.");
    FILC_CHECK(
        current_frame->parent->origin->function_origin->can_catch,
        NULL,
        "cannot resume unwinding, parent frame doesn't support catching.");
}

filc_jmp_buf* filc_jmp_buf_create(filc_thread* my_thread, filc_jmp_buf_kind kind)
{
    PAS_ASSERT(kind == filc_jmp_buf_setjmp ||
               kind == filc_jmp_buf__setjmp ||
               kind == filc_jmp_buf_sigsetjmp);

    filc_frame* frame = my_thread->top_frame;
    PAS_ASSERT(frame->origin);
    PAS_ASSERT(frame->origin->function_origin);
    
    filc_jmp_buf* result = (filc_jmp_buf*)filc_allocate_special(
        my_thread,
        PAS_OFFSETOF(filc_jmp_buf, objects)
        + frame->origin->function_origin->num_objects * sizeof(filc_object*),
        FILC_WORD_TYPE_JMP_BUF)->lower;

    result->kind = kind;
    result->saved_top_frame = frame;
    /* NOTE: We could possibly do more stuff to track the state of the top native frame, but we don't,
       because frames that create native frames don't setjmp. Basically, native code doesn't setjmp. */
    filc_native_frame_assert_locked(my_thread->top_native_frame);
    result->saved_top_native_frame = my_thread->top_native_frame;
    result->saved_allocation_roots_num_objects = my_thread->allocation_roots.num_objects;
    result->num_objects = frame->origin->function_origin->num_objects;
    size_t index;
    for (index = frame->origin->function_origin->num_objects; index--;) {
        filc_store_barrier(my_thread, frame->objects[index]);
        result->objects[index] = frame->objects[index];
    }

    PAS_ASSERT(result->num_objects == result->saved_top_frame->origin->function_origin->num_objects);

    return result;
}

void filc_jmp_buf_mark_outgoing_ptrs(filc_jmp_buf* jmp_buf, filc_object_array* stack)
{
    size_t index;
    for (index = jmp_buf->num_objects; index--;)
        fugc_mark(stack, jmp_buf->objects[index]);
}

static void longjmp_impl(filc_thread* my_thread, filc_ptr jmp_buf_ptr, int value, filc_jmp_buf_kind kind)
{
    PAS_ASSERT(kind == filc_jmp_buf_setjmp ||
               kind == filc_jmp_buf__setjmp ||
               kind == filc_jmp_buf_sigsetjmp);
    
    filc_check_access_special(jmp_buf_ptr, FILC_WORD_TYPE_JMP_BUF, NULL);
    filc_jmp_buf* jmp_buf = (filc_jmp_buf*)filc_ptr_ptr(jmp_buf_ptr);

    FILC_CHECK(
        !my_thread->special_signal_deferral_depth,
        NULL,
        "cannot longjmp from a special signal deferral scope.");
    
    FILC_CHECK(
        jmp_buf->kind == kind,
        NULL,
        "cannot mix %s with %s.",
        filc_jmp_buf_kind_get_longjmp_string(kind), filc_jmp_buf_kind_get_string(jmp_buf->kind));

    filc_frame* current_frame;
    bool found_frame = false;
    for (current_frame = my_thread->top_frame;
         current_frame && !found_frame;
         current_frame = current_frame->parent) {
        PAS_ASSERT(current_frame->origin);
        PAS_ASSERT(current_frame->origin->function_origin);
        PAS_ASSERT(current_frame->origin->function_origin->num_setjmps
                   <= current_frame->origin->function_origin->num_objects);
        unsigned index;
        for (index = current_frame->origin->function_origin->num_setjmps; index-- && !found_frame;) {
            unsigned object_index = current_frame->origin->function_origin->num_objects - 1 - index;
            PAS_ASSERT(object_index < current_frame->origin->function_origin->num_objects);
            if (filc_object_for_special_payload(jmp_buf) == current_frame->objects[object_index]) {
                PAS_ASSERT(current_frame == jmp_buf->saved_top_frame);
                found_frame = true;
                break;
            }
        }
    }

    FILC_CHECK(
        found_frame,
        NULL,
        "cannot longjmp unless the setjmp destination is on the stack.");

    while (my_thread->top_frame != jmp_buf->saved_top_frame)
        filc_pop_frame(my_thread, my_thread->top_frame);
    while (my_thread->top_native_frame != jmp_buf->saved_top_native_frame) {
        if (my_thread->top_native_frame->locked)
            filc_unlock_top_native_frame(my_thread);
        filc_pop_native_frame(my_thread, my_thread->top_native_frame);
    }
    my_thread->allocation_roots.num_objects = jmp_buf->saved_allocation_roots_num_objects;

    PAS_ASSERT(my_thread->top_frame == jmp_buf->saved_top_frame);
    PAS_ASSERT(my_thread->top_frame->origin->function_origin->num_objects == jmp_buf->num_objects);
    size_t index;
    for (index = jmp_buf->num_objects; index--;)
        my_thread->top_frame->objects[index] = jmp_buf->objects[index];

    switch (kind) {
    case filc_jmp_buf_setjmp:
        longjmp(jmp_buf->u.system_buf, value);
        PAS_ASSERT(!"Should not be reached");
        break;
    case filc_jmp_buf__setjmp:
        _longjmp(jmp_buf->u.system_buf, value);
        PAS_ASSERT(!"Should not be reached");
        break;
    case filc_jmp_buf_sigsetjmp:
        siglongjmp(jmp_buf->u.system_sigbuf, value);
        PAS_ASSERT(!"Should not be reached");
        break;
    }
    PAS_ASSERT(!"Should not be reached");
}

void filc_native_zlongjmp(filc_thread* my_thread, filc_ptr jmp_buf_ptr, int value)
{
    longjmp_impl(my_thread, jmp_buf_ptr, value, filc_jmp_buf_setjmp);
}

void filc_native_z_longjmp(filc_thread* my_thread, filc_ptr jmp_buf_ptr, int value)
{
    longjmp_impl(my_thread, jmp_buf_ptr, value, filc_jmp_buf__setjmp);
}

void filc_native_zsiglongjmp(filc_thread* my_thread, filc_ptr jmp_buf_ptr, int value)
{
    longjmp_impl(my_thread, jmp_buf_ptr, value, filc_jmp_buf_sigsetjmp);
}

static bool (*pizlonated_errno_handler)(PIZLONATED_SIGNATURE);

void filc_native_zregister_sys_errno_handler(filc_thread* my_thread, filc_ptr errno_handler)
{
    PAS_UNUSED_PARAM(my_thread);
    FILC_CHECK(
        !pizlonated_errno_handler,
        NULL,
        "errno handler already registered.");
    filc_check_function_call(errno_handler);
    pizlonated_errno_handler = (bool(*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(errno_handler);
}

static bool (*pizlonated_dlerror_handler)(PIZLONATED_SIGNATURE);

void filc_native_zregister_sys_dlerror_handler(filc_thread* my_thread, filc_ptr dlerror_handler)
{
    PAS_UNUSED_PARAM(my_thread);
    FILC_CHECK(
        !pizlonated_dlerror_handler,
        NULL,
        "dlerror handler already registered.");
    filc_check_function_call(dlerror_handler);
    pizlonated_dlerror_handler = (bool(*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(dlerror_handler);
}

void filc_set_user_errno(int errno_value)
{
    FILC_CHECK(
        pizlonated_errno_handler,
        NULL,
        "errno handler not registered when trying to set errno = %d.", errno_value);
    filc_thread* my_thread = filc_get_my_thread();
    filc_ptr args = filc_ptr_create(my_thread, filc_allocate_int(my_thread, sizeof(int)));
    *(int*)filc_ptr_ptr(args) = errno_value;
    filc_return_buffer return_buffer;
    filc_ptr rets = filc_ptr_for_int_return_buffer(&return_buffer);
    filc_lock_top_native_frame(my_thread);
    PAS_ASSERT(!pizlonated_errno_handler(my_thread, args, rets));
    filc_unlock_top_native_frame(my_thread);
}

void filc_set_errno(int errno_value)
{
    int user_errno = filc_to_user_errno(errno_value);
    if (dump_errnos) {
        pas_log("Setting errno! System errno = %d, user errno = %d, system error = %s\n",
                errno_value, user_errno, strerror(errno_value));
        filc_thread_dump_stack(filc_get_my_thread(), &pas_log_stream.base);
    }
    filc_set_user_errno(user_errno);
}

static void set_dlerror(const char* error)
{
    PAS_ASSERT(error);
    FILC_CHECK(
        pizlonated_dlerror_handler,
        NULL,
        "dlerror handler not registered when trying to set dlerror = %s.", error);
    filc_thread* my_thread = filc_get_my_thread();
    filc_ptr args = filc_ptr_create(my_thread, filc_allocate(my_thread, sizeof(filc_ptr)));
    filc_check_write_ptr(args, NULL);
    filc_ptr_store(my_thread, (filc_ptr*)filc_ptr_ptr(args), filc_strdup(my_thread, error));
    filc_return_buffer return_buffer;
    filc_ptr rets = filc_ptr_for_int_return_buffer(&return_buffer);
    filc_lock_top_native_frame(my_thread);
    PAS_ASSERT(!pizlonated_dlerror_handler(my_thread, args, rets));
    filc_unlock_top_native_frame(my_thread);
}

void filc_extract_user_iovec_entry(filc_thread* my_thread, filc_ptr user_iov_entry_ptr,
                                   filc_ptr* user_iov_base, size_t* iov_len)
{
    filc_check_read_ptr(
        filc_ptr_with_offset(user_iov_entry_ptr, PAS_OFFSETOF(filc_user_iovec, iov_base)), NULL);
    filc_check_read_int(
        filc_ptr_with_offset(user_iov_entry_ptr, PAS_OFFSETOF(filc_user_iovec, iov_len)),
        sizeof(size_t), NULL);
    *user_iov_base = filc_ptr_load(
        my_thread, &((filc_user_iovec*)filc_ptr_ptr(user_iov_entry_ptr))->iov_base);
    *iov_len = ((filc_user_iovec*)filc_ptr_ptr(user_iov_entry_ptr))->iov_len;
}

void filc_prepare_iovec_entry(filc_thread* my_thread, filc_ptr user_iov_entry_ptr,
                              struct iovec* iov_entry, filc_access_kind access_kind)
{
    filc_ptr user_iov_base;
    size_t iov_len;
    filc_extract_user_iovec_entry(my_thread, user_iov_entry_ptr, &user_iov_base, &iov_len);
    filc_check_access_int(user_iov_base, iov_len, access_kind, NULL);
    filc_pin_tracked(my_thread, filc_ptr_object(user_iov_base));
    iov_entry->iov_base = filc_ptr_ptr(user_iov_base);
    iov_entry->iov_len = iov_len;
}

struct iovec* filc_prepare_iovec(filc_thread* my_thread, filc_ptr user_iov, int iovcnt,
                                 filc_access_kind access_kind)
{
    struct iovec* iov;
    size_t index;
    FILC_CHECK(
        iovcnt >= 0,
        NULL,
        "iovcnt cannot be negative; iovcnt = %d.\n", iovcnt);
    iov = filc_bmalloc_allocate_tmp(
        my_thread, filc_mul_size(sizeof(struct iovec), iovcnt));
    for (index = 0; index < (size_t)iovcnt; ++index) {
        filc_prepare_iovec_entry(
            my_thread, filc_ptr_with_offset(user_iov, filc_mul_size(sizeof(filc_user_iovec), index)),
            iov + index, access_kind);
    }
    return iov;
}

ssize_t filc_native_zsys_writev(filc_thread* my_thread, int fd, filc_ptr user_iov, int iovcnt)
{
    ssize_t result;
    struct iovec* iov = filc_prepare_iovec(my_thread, user_iov, iovcnt, filc_read_access);
    filc_exit(my_thread);
    result = writev(fd, iov, iovcnt);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

ssize_t filc_native_zsys_read(filc_thread* my_thread, int fd, filc_ptr buf, size_t size)
{
    filc_cpt_write_int(my_thread, buf, size);
    return FILC_SYSCALL(my_thread, read(fd, filc_ptr_ptr(buf), size));
}

ssize_t filc_native_zsys_readv(filc_thread* my_thread, int fd, filc_ptr user_iov, int iovcnt)
{
    ssize_t result;
    struct iovec* iov = filc_prepare_iovec(my_thread, user_iov, iovcnt, filc_write_access);
    filc_exit(my_thread);
    result = readv(fd, iov, iovcnt);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

ssize_t filc_native_zsys_write(filc_thread* my_thread, int fd, filc_ptr buf, size_t size)
{
    filc_cpt_read_int(my_thread, buf, size);
    return FILC_SYSCALL(my_thread, write(fd, filc_ptr_ptr(buf), size));
}

int filc_native_zsys_close(filc_thread* my_thread, int fd)
{
    filc_exit(my_thread);
    int result = close(fd);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

long filc_native_zsys_lseek(filc_thread* my_thread, int fd, long offset, int whence)
{
    filc_exit(my_thread);
    long result = lseek(fd, offset, whence);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

void filc_native_zsys_exit(filc_thread* my_thread, int return_code)
{
    filc_exit(my_thread);
    exit(return_code);
    PAS_ASSERT(!"Should not be reached");
}

unsigned filc_native_zsys_getuid(filc_thread* my_thread)
{
    filc_exit(my_thread);
    unsigned result = getuid();
    filc_enter(my_thread);
    return result;
}

unsigned filc_native_zsys_geteuid(filc_thread* my_thread)
{
    filc_exit(my_thread);
    unsigned result = geteuid();
    filc_enter(my_thread);
    return result;
}

unsigned filc_native_zsys_getgid(filc_thread* my_thread)
{
    filc_exit(my_thread);
    unsigned result = getgid();
    filc_enter(my_thread);
    return result;
}

unsigned filc_native_zsys_getegid(filc_thread* my_thread)
{
    filc_exit(my_thread);
    unsigned result = getegid();
    filc_enter(my_thread);
    return result;
}

int filc_from_user_open_flags(int user_flags)
{
    if (!FILC_MUSL)
        return user_flags;
    
    int result = 0;

    if (filc_check_and_clear(&user_flags, 01))
        result |= O_WRONLY;
    if (filc_check_and_clear(&user_flags, 02))
        result |= O_RDWR;
    if (filc_check_and_clear(&user_flags, 0100))
        result |= O_CREAT;
    if (filc_check_and_clear(&user_flags, 0200))
        result |= O_EXCL;
    if (filc_check_and_clear(&user_flags, 0400))
        result |= O_NOCTTY;
    if (filc_check_and_clear(&user_flags, 01000))
        result |= O_TRUNC;
    if (filc_check_and_clear(&user_flags, 02000))
        result |= O_APPEND;
    if (filc_check_and_clear(&user_flags, 04000))
        result |= O_NONBLOCK;
    if (filc_check_and_clear(&user_flags, 0200000))
        result |= O_DIRECTORY;
    if (filc_check_and_clear(&user_flags, 0400000))
        result |= O_NOFOLLOW;
    if (filc_check_and_clear(&user_flags, 02000000))
        result |= O_CLOEXEC;
    if (filc_check_and_clear(&user_flags, 020000))
        result |= O_ASYNC;
    filc_check_and_clear(&user_flags, 0100000); // O_LARGEFILE

    if (user_flags)
        return -1;
    return result;
}

int filc_to_user_open_flags(int flags)
{
    if (!FILC_MUSL)
        return flags;
    
    int result = 0;

    if (filc_check_and_clear(&flags, O_WRONLY))
        result |= 01;
    if (filc_check_and_clear(&flags, O_RDWR))
        result |= 02;
    if (filc_check_and_clear(&flags, O_CREAT))
        result |= 0100;
    if (filc_check_and_clear(&flags, O_EXCL))
        result |= 0200;
    if (filc_check_and_clear(&flags, O_NOCTTY))
        result |= 0400;
    if (filc_check_and_clear(&flags, O_TRUNC))
        result |= 01000;
    if (filc_check_and_clear(&flags, O_APPEND))
        result |= 02000;
    if (filc_check_and_clear(&flags, O_NONBLOCK))
        result |= 04000;
    if (filc_check_and_clear(&flags, O_DIRECTORY))
        result |= 0200000;
    if (filc_check_and_clear(&flags, O_NOFOLLOW))
        result |= 0400000;
    if (filc_check_and_clear(&flags, O_CLOEXEC))
        result |= 02000000;
    if (filc_check_and_clear(&flags, O_ASYNC))
        result |= 020000;

    /* Fun fact: on MacOS, I get an additional 0x10000 flag, and I don't know what it is.
       Ima just ignore it and hope for the best LOL! */
    PAS_ASSERT(!(flags & ~0x10000));
    
    return result;
}

int filc_native_zsys_open(filc_thread* my_thread, filc_ptr path_ptr, int user_flags, filc_ptr args)
{
    int flags = filc_from_user_open_flags(user_flags);
    int mode = 0;
    if (flags >= 0 && (flags & O_CREAT))
        mode = filc_ptr_get_next_int(&args);
    if (flags < 0) {
        filc_set_errno(EINVAL);
        return -1;
    }
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_exit(my_thread);
    int result = open(path, flags, mode);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_getpid(filc_thread* my_thread)
{
    filc_exit(my_thread);
    int result = getpid();
    filc_enter(my_thread);
    return result;
}

static bool from_user_clock_id(int user_clock_id, clockid_t* result)
{
    if (!FILC_MUSL) {
        *result = user_clock_id;
        return true;
    }
    
    switch (user_clock_id) {
    case 0:
        *result = CLOCK_REALTIME;
        return true;
    case 1:
        *result = CLOCK_MONOTONIC;
        return true;
    case 2:
        *result = CLOCK_PROCESS_CPUTIME_ID;
        return true;
    case 3:
        *result = CLOCK_THREAD_CPUTIME_ID;
        return true;
#if PAS_OS(DARWIN)
    case 4:
        *result = CLOCK_MONOTONIC_RAW;
        return true;
#endif /* PAS_OS(DARWIN) */
    default:
        *result = 0;
        return false;
    }
}

int filc_native_zsys_clock_gettime(filc_thread* my_thread, int user_clock_id, filc_ptr timespec_ptr)
{
    clockid_t clock_id;
    if (!from_user_clock_id(user_clock_id, &clock_id)) {
        filc_set_errno(EINVAL);
        return -1;
    }
    struct timespec ts;
    filc_exit(my_thread);
    int result = clock_gettime(clock_id, &ts);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0) {
        filc_set_errno(my_errno);
        return -1;
    }
    filc_check_write_int(timespec_ptr, sizeof(filc_user_timespec), NULL);
    filc_user_timespec* user_timespec = (filc_user_timespec*)filc_ptr_ptr(timespec_ptr);
    user_timespec->tv_sec = ts.tv_sec;
    user_timespec->tv_nsec = ts.tv_nsec;
    return 0;
}

static bool from_user_fstatat_flag(int user_flag, int* result)
{
    if (!FILC_MUSL) {
        *result = user_flag;
        return true;
    }
    
    *result = 0;
    if (filc_check_and_clear(&user_flag, 0x100))
        *result |= AT_SYMLINK_NOFOLLOW;
    if (filc_check_and_clear(&user_flag, 0x200))
        *result |= AT_EACCESS; /* NOTE: in the case of unlinkat, this would be REMOVEDIR. */
    if (filc_check_and_clear(&user_flag, 0x400))
        *result |= AT_SYMLINK_FOLLOW;
    return !user_flag;
}

/* NOTE: We only use this in the musl mode. */
struct musl_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    unsigned st_mode;
    uint64_t st_nlink;
    unsigned st_uid;
    unsigned st_gid;
    uint64_t st_rdev;
    int64_t st_size;
    long st_blksize;
    int64_t st_blocks;
    uint64_t st_atim[2];
    uint64_t st_mtim[2];
    uint64_t st_ctim[2];
};

static int handle_fstat_result(filc_ptr user_stat_ptr, struct stat *st,
                               int result, int my_errno)
{
    if (!FILC_MUSL) {
        filc_check_write_int(user_stat_ptr, sizeof(struct stat), NULL);
        if (result < 0) {
            filc_set_errno(my_errno);
            return -1;
        }
        memcpy(filc_ptr_ptr(user_stat_ptr), st, sizeof(struct stat));
        return 0;
    }
    
    filc_check_write_int(user_stat_ptr, sizeof(struct musl_stat), NULL);
    if (result < 0) {
        filc_set_errno(my_errno);
        return -1;
    }
    struct musl_stat* musl_stat = (struct musl_stat*)filc_ptr_ptr(user_stat_ptr);
    musl_stat->st_dev = st->st_dev;
    musl_stat->st_ino = st->st_ino;
    musl_stat->st_mode = st->st_mode;
    musl_stat->st_nlink = st->st_nlink;
    musl_stat->st_uid = st->st_uid;
    musl_stat->st_gid = st->st_gid;
    musl_stat->st_rdev = st->st_rdev;
    musl_stat->st_size = st->st_size;
    musl_stat->st_blksize = st->st_blksize;
    musl_stat->st_blocks = st->st_blocks;
    musl_stat->st_atim[0] = st->st_atimespec.tv_sec;
    musl_stat->st_atim[1] = st->st_atimespec.tv_nsec;
    musl_stat->st_mtim[0] = st->st_mtimespec.tv_sec;
    musl_stat->st_mtim[1] = st->st_mtimespec.tv_nsec;
    musl_stat->st_ctim[0] = st->st_ctimespec.tv_sec;
    musl_stat->st_ctim[1] = st->st_ctimespec.tv_nsec;
    return 0;
}

int filc_from_user_atfd(int fd)
{
    if (!FILC_MUSL)
        return fd;
    if (fd == -100)
        fd = AT_FDCWD;
    return fd;
}

int filc_native_zsys_fstatat(
    filc_thread* my_thread, int user_fd, filc_ptr path_ptr, filc_ptr user_stat_ptr,
    int user_flag)
{
    int flag;
    if (!from_user_fstatat_flag(user_flag, &flag)) {
        filc_set_errno(EINVAL);
        return -1;
    }
    int fd = filc_from_user_atfd(user_fd);
    struct stat st;
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_exit(my_thread);
    int result = fstatat(fd, path, &st, flag);
    int my_errno = errno;
    filc_enter(my_thread);
    return handle_fstat_result(user_stat_ptr, &st, result, my_errno);
}

int filc_native_zsys_fstat(filc_thread* my_thread, int fd, filc_ptr user_stat_ptr)
{
    struct stat st;
    filc_exit(my_thread);
    int result = fstat(fd, &st);
    int my_errno = errno;
    filc_enter(my_thread);
    return handle_fstat_result(user_stat_ptr, &st, result, my_errno);
}

static bool from_user_sa_flags(int user_flags, int* flags)
{
    if (!FILC_MUSL) {
        if ((user_flags & SA_SIGINFO))
            return false;
        *flags = user_flags;
        return true;
    }
    *flags = 0;
    /* NOTE: We explicitly exclude SA_SIGINFO because we do not support it yet!! */
    if (filc_check_and_clear(&user_flags, 1))
        *flags |= SA_NOCLDSTOP;
    if (filc_check_and_clear(&user_flags, 2))
        *flags |= SA_NOCLDWAIT;
    if (filc_check_and_clear(&user_flags, 0x08000000))
        *flags |= SA_ONSTACK;
    if (filc_check_and_clear(&user_flags, 0x10000000))
        *flags |= SA_RESTART;
    if (filc_check_and_clear(&user_flags, 0x40000000))
        *flags |= SA_NODEFER;
    if (filc_check_and_clear(&user_flags, 0x80000000))
        *flags |= SA_RESETHAND;
    return !user_flags;
}

static int to_user_sa_flags(int sa_flags)
{
    if (!FILC_MUSL)
        return sa_flags;
    int result = 0;
    if (filc_check_and_clear(&sa_flags, SA_NOCLDSTOP))
        result |= 1;
    if (filc_check_and_clear(&sa_flags, SA_NOCLDWAIT))
        result |= 2;
    if (filc_check_and_clear(&sa_flags, SA_SIGINFO))
        result |= 4;
    if (filc_check_and_clear(&sa_flags, SA_ONSTACK))
        result |= 0x08000000;
    if (filc_check_and_clear(&sa_flags, SA_RESTART))
        result |= 0x10000000;
    if (filc_check_and_clear(&sa_flags, SA_NODEFER))
        result |= 0x40000000;
    if (filc_check_and_clear(&sa_flags, SA_RESETHAND))
        result |= 0x80000000;
    PAS_ASSERT(!sa_flags);
    return result;
}

static bool is_unsafe_signal(int signum)
{
    switch (signum) {
    case SIGILL:
    case SIGTRAP:
    case SIGBUS:
    case SIGSEGV:
    case SIGFPE:
#if PAS_OS(FREEBSD)
    case SIGTHR:
    case SIGLIBRT:
#endif /* PAS_OS(FREEBSD) */
        return true;
    default:
        return false;
    }
}

static bool is_special_signal_handler(void* handler)
{
    return handler == SIG_DFL || handler == SIG_IGN;
}

static bool is_user_special_signal_handler(void* handler)
{
    if (!FILC_MUSL)
        return is_special_signal_handler(handler);
    return handler == NULL || handler == (void*)(uintptr_t)1;
}

static void* from_user_special_signal_handler(void* handler)
{
    PAS_ASSERT(is_user_special_signal_handler(handler));
    if (!FILC_MUSL)
        return handler;
    return handler == NULL ? SIG_DFL : SIG_IGN;
}

static filc_ptr to_user_special_signal_handler(void* handler)
{
    if (!FILC_MUSL) {
        PAS_ASSERT(is_special_signal_handler(handler));
        return filc_ptr_forge_invalid(handler);
    }
    if (handler == SIG_DFL)
        return filc_ptr_forge_invalid(NULL);
    if (handler == SIG_IGN)
        return filc_ptr_forge_invalid((void*)(uintptr_t)1);
    PAS_ASSERT(!"Bad special handler");
    return filc_ptr_forge_invalid(NULL);
}

int filc_native_zsys_sigaction(
    filc_thread* my_thread, int user_signum, filc_ptr act_ptr, filc_ptr oact_ptr)
{
    static const bool verbose = false;
    
    int signum = filc_from_user_signum(user_signum);
    if (signum < 0) {
        if (verbose)
            pas_log("bad signum\n");
        filc_set_errno(EINVAL);
        return -1;
    }
    if (is_unsafe_signal(signum) && filc_ptr_ptr(act_ptr)) {
        filc_set_errno(ENOSYS);
        return -1;
    }
    if (filc_ptr_ptr(act_ptr))
        check_user_sigaction(act_ptr, filc_read_access);
    struct user_sigaction* user_act = (struct user_sigaction*)filc_ptr_ptr(act_ptr);
    struct user_sigaction* user_oact = (struct user_sigaction*)filc_ptr_ptr(oact_ptr);
    struct sigaction act;
    struct sigaction oact;
    if (user_act) {
        filc_from_user_sigset(&user_act->sa_mask, &act.sa_mask);
        filc_ptr user_handler = filc_ptr_load(my_thread, &user_act->sa_handler_ish);
        if (is_user_special_signal_handler(filc_ptr_ptr(user_handler)))
            act.sa_handler = from_user_special_signal_handler(filc_ptr_ptr(user_handler));
        else {
            filc_check_function_call(user_handler);
            filc_object* handler_object = filc_allocate_special(
                my_thread, sizeof(filc_signal_handler), FILC_WORD_TYPE_SIGNAL_HANDLER);
            filc_thread_track_object(my_thread, handler_object);
            filc_signal_handler* handler = (filc_signal_handler*)handler_object->lower;
            handler->function_ptr = user_handler;
            handler->mask = act.sa_mask;
            handler->user_signum = user_signum;
            pas_store_store_fence();
            PAS_ASSERT((unsigned)user_signum <= FILC_MAX_USER_SIGNUM);
            filc_store_barrier(my_thread, filc_object_for_special_payload(handler));
            signal_table[user_signum] = handler;
            act.sa_handler = signal_pizlonator;
        }
        if (!from_user_sa_flags(user_act->sa_flags, &act.sa_flags)) {
            filc_set_errno(EINVAL);
            return -1;
        }
    }
    if (user_oact)
        pas_zero_memory(&oact, sizeof(struct sigaction));
    filc_exit(my_thread);
    int result = sigaction(signum, user_act ? &act : NULL, user_oact ? &oact : NULL);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0) {
        filc_set_errno(my_errno);
        return -1;
    }
    if (user_oact) {
        check_user_sigaction(oact_ptr, filc_write_access);
        if (is_unsafe_signal(signum))
            PAS_ASSERT(oact.sa_handler == SIG_DFL);
        if (is_special_signal_handler(oact.sa_handler))
            filc_ptr_store(my_thread, &user_oact->sa_handler_ish,
                           to_user_special_signal_handler(oact.sa_handler));
        else {
            PAS_ASSERT(oact.sa_handler == signal_pizlonator);
            PAS_ASSERT((unsigned)user_signum <= FILC_MAX_USER_SIGNUM);
            /* FIXME: The signal_table entry should really be a filc_ptr so we can return it here. */
            filc_ptr_store(my_thread, &user_oact->sa_handler_ish,
                           filc_ptr_load_with_manual_tracking(&signal_table[user_signum]->function_ptr));
        }
        filc_to_user_sigset(&oact.sa_mask, &user_oact->sa_mask);
        user_oact->sa_flags = to_user_sa_flags(oact.sa_flags);
    }
    return 0;
}

int filc_native_zsys_pipe(filc_thread* my_thread, filc_ptr fds_ptr)
{
    int fds[2];
    filc_exit(my_thread);
    int result = pipe(fds);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0) {
        /* Make sure not to modify what fds_ptr points to on error, even if the system modified
           our fds, since that would be nonconforming behavior. Probably doesn't matter since of
           course we would never run on a nonconforming system. */
        filc_set_errno(my_errno);
        return -1;
    }
    filc_check_write_int(fds_ptr, sizeof(int) * 2, NULL);
    ((int*)filc_ptr_ptr(fds_ptr))[0] = fds[0];
    ((int*)filc_ptr_ptr(fds_ptr))[1] = fds[1];
    return 0;
}

int filc_native_zsys_select(filc_thread* my_thread, int nfds,
                            filc_ptr readfds_ptr, filc_ptr writefds_ptr, filc_ptr exceptfds_ptr,
                            filc_ptr timeout_ptr)
{
    PAS_ASSERT(FD_SETSIZE == 1024);
    FILC_CHECK(
        nfds <= 1024,
        NULL,
        "attempt to select with nfds = %d (should be 1024 or less).",
        nfds);
    if (filc_ptr_ptr(readfds_ptr))
        filc_check_write_int(readfds_ptr, sizeof(fd_set), NULL);
    if (filc_ptr_ptr(writefds_ptr))
        filc_check_write_int(writefds_ptr, sizeof(fd_set), NULL);
    if (filc_ptr_ptr(exceptfds_ptr))
        filc_check_write_int(exceptfds_ptr, sizeof(fd_set), NULL);
    if (filc_ptr_ptr(timeout_ptr))
        filc_check_write_int(timeout_ptr, sizeof(filc_user_timeval), NULL);
    fd_set* readfds = (fd_set*)filc_ptr_ptr(readfds_ptr);
    fd_set* writefds = (fd_set*)filc_ptr_ptr(writefds_ptr);
    fd_set* exceptfds = (fd_set*)filc_ptr_ptr(exceptfds_ptr);
    filc_user_timeval* user_timeout = (filc_user_timeval*)filc_ptr_ptr(timeout_ptr);
    struct timeval timeout;
    if (user_timeout) {
        timeout.tv_sec = user_timeout->tv_sec;
        timeout.tv_usec = user_timeout->tv_usec;
    }
    filc_pin(filc_ptr_object(readfds_ptr));
    filc_pin(filc_ptr_object(writefds_ptr));
    filc_pin(filc_ptr_object(exceptfds_ptr));
    filc_exit(my_thread);
    int result = select(nfds, readfds, writefds, exceptfds, user_timeout ? &timeout : NULL);
    int my_errno = errno;
    filc_enter(my_thread);
    filc_unpin(filc_ptr_object(readfds_ptr));
    filc_unpin(filc_ptr_object(writefds_ptr));
    filc_unpin(filc_ptr_object(exceptfds_ptr));
    if (result < 0)
        filc_set_errno(my_errno);
    if (user_timeout) {
        filc_check_write_int(timeout_ptr, sizeof(filc_user_timeval), NULL);
        user_timeout->tv_sec = timeout.tv_sec;
        user_timeout->tv_usec = timeout.tv_usec;
    }
    return result;
}

void filc_native_zsys_sched_yield(filc_thread* my_thread)
{
    filc_exit(my_thread);
    sched_yield();
    filc_enter(my_thread);
}

static bool from_user_resource(int user_resource, int* result)
{
    if (!FILC_MUSL) {
        *result = user_resource;
        return true;
    }
    
    switch (user_resource) {
    case 0:
        *result = RLIMIT_CPU;
        return true;
    case 1:
        *result = RLIMIT_FSIZE;
        return true;
    case 2:
        *result = RLIMIT_DATA;
        return true;
    case 3:
        *result = RLIMIT_STACK;
        return true;
    case 4:
        *result = RLIMIT_CORE;
        return true;
    case 5:
        *result = RLIMIT_RSS;
        return true;
    case 6:
        *result = RLIMIT_NPROC;
        return true;
    case 7:
        *result = RLIMIT_NOFILE;
        return true;
    case 8:
        *result = RLIMIT_MEMLOCK;
        return true;
#if !PAS_OS(OPENBSD)
    case 9:
        *result = RLIMIT_AS;
        return true;
#endif /* !PAS_OS(OPENBSD) */
    default:
        return false;
    }
}

#if FILC_MUSL
static unsigned long long to_user_rlimit_value(rlim_t value)
{
    if (value == RLIM_INFINITY)
        return ~0ULL;
    return value;
}
#else /* FILC_MUSL -> so !FILC_MUSL */
#define to_user_rlimit_value(value) (value)
#endif /* FILC_MUSL -> so !FILC_MUSL */

int filc_native_zsys_getrlimit(filc_thread* my_thread, int user_resource, filc_ptr rlim_ptr)
{
    int resource;
    if (!from_user_resource(user_resource, &resource))
        goto einval;
    struct rlimit rlim;
    filc_exit(my_thread);
    int result = getrlimit(resource, &rlim);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    else {
        PAS_ASSERT(!result);
        filc_check_write_int(rlim_ptr, sizeof(filc_user_rlimit), NULL);
        filc_user_rlimit* user_rlim = (filc_user_rlimit*)filc_ptr_ptr(rlim_ptr);
        user_rlim->rlim_cur = to_user_rlimit_value(rlim.rlim_cur);
        user_rlim->rlim_max = to_user_rlimit_value(rlim.rlim_max);
    }
    return result;

einval:
    filc_set_errno(EINVAL);
    return -1;
}

unsigned filc_native_zsys_umask(filc_thread* my_thread, unsigned mask)
{
    filc_exit(my_thread);
    unsigned result = umask(mask);
    filc_enter(my_thread);
    return result;
}

int filc_native_zsys_getitimer(filc_thread* my_thread, int which, filc_ptr user_value_ptr)
{
    filc_exit(my_thread);
    struct itimerval value;
    int result = getitimer(which, &value);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0) {
        filc_set_errno(my_errno);
        return -1;
    }
    filc_check_write_int(user_value_ptr, sizeof(filc_user_itimerval), NULL);
    filc_user_itimerval* user_value = (filc_user_itimerval*)filc_ptr_ptr(user_value_ptr);
    user_value->it_interval.tv_sec = value.it_interval.tv_sec;
    user_value->it_interval.tv_usec = value.it_interval.tv_usec;
    user_value->it_value.tv_sec = value.it_value.tv_sec;
    user_value->it_value.tv_usec = value.it_value.tv_usec;
    return 0;
}

int filc_native_zsys_setitimer(filc_thread* my_thread, int which, filc_ptr user_new_value_ptr,
                               filc_ptr user_old_value_ptr)
{
    filc_check_write_int(user_new_value_ptr, sizeof(filc_user_itimerval), NULL);
    struct itimerval new_value;
    filc_user_itimerval* user_new_value = (filc_user_itimerval*)filc_ptr_ptr(user_new_value_ptr);
    new_value.it_interval.tv_sec = user_new_value->it_interval.tv_sec;
    new_value.it_interval.tv_usec = user_new_value->it_interval.tv_usec;
    new_value.it_value.tv_sec = user_new_value->it_value.tv_sec;
    new_value.it_value.tv_usec = user_new_value->it_value.tv_usec;
    filc_exit(my_thread);
    struct itimerval old_value;
    int result = setitimer(which, &new_value, &old_value);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0) {
        filc_set_errno(my_errno);
        return -1;
    }
    filc_user_itimerval* user_old_value = (filc_user_itimerval*)filc_ptr_ptr(user_old_value_ptr);
    if (user_old_value) {
        filc_check_read_int(user_old_value_ptr, sizeof(filc_user_itimerval), NULL);
        user_old_value->it_interval.tv_sec = old_value.it_interval.tv_sec;
        user_old_value->it_interval.tv_usec = old_value.it_interval.tv_usec;
        user_old_value->it_value.tv_sec = old_value.it_value.tv_sec;
        user_old_value->it_value.tv_usec = old_value.it_value.tv_usec;
    }
    return 0;
}

int filc_native_zsys_pause(filc_thread* my_thread)
{
    filc_exit(my_thread);
    int result = pause();
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(result == -1);
    filc_set_errno(my_errno);
    return -1;
}

int filc_native_zsys_pselect(filc_thread* my_thread, int nfds,
                             filc_ptr readfds_ptr, filc_ptr writefds_ptr, filc_ptr exceptfds_ptr,
                             filc_ptr timeout_ptr, filc_ptr sigmask_ptr)
{
    PAS_ASSERT(FD_SETSIZE == 1024);
    FILC_CHECK(
        nfds <= 1024,
        NULL,
        "attempt to select with nfds = %d (should be 1024 or less).",
        nfds);
    if (filc_ptr_ptr(readfds_ptr))
        filc_check_write_int(readfds_ptr, sizeof(fd_set), NULL);
    if (filc_ptr_ptr(writefds_ptr))
        filc_check_write_int(writefds_ptr, sizeof(fd_set), NULL);
    if (filc_ptr_ptr(exceptfds_ptr))
        filc_check_write_int(exceptfds_ptr, sizeof(fd_set), NULL);
    if (filc_ptr_ptr(timeout_ptr))
        filc_check_read_int(timeout_ptr, sizeof(filc_user_timespec), NULL);
    if (filc_ptr_ptr(sigmask_ptr))
        filc_check_user_sigset(sigmask_ptr, filc_read_access);
    fd_set* readfds = (fd_set*)filc_ptr_ptr(readfds_ptr);
    fd_set* writefds = (fd_set*)filc_ptr_ptr(writefds_ptr);
    fd_set* exceptfds = (fd_set*)filc_ptr_ptr(exceptfds_ptr);
    filc_user_timespec* user_timeout = (filc_user_timespec*)filc_ptr_ptr(timeout_ptr);
    struct timespec timeout;
    if (user_timeout) {
        timeout.tv_sec = user_timeout->tv_sec;
        timeout.tv_nsec = user_timeout->tv_nsec;
    }
    filc_user_sigset* user_sigmask = (filc_user_sigset*)filc_ptr_ptr(sigmask_ptr);
    sigset_t sigmask;
    if (user_sigmask)
        filc_from_user_sigset(user_sigmask, &sigmask);
    filc_pin(filc_ptr_object(readfds_ptr));
    filc_pin(filc_ptr_object(writefds_ptr));
    filc_pin(filc_ptr_object(exceptfds_ptr));
    filc_exit(my_thread);
    int result = pselect(nfds, readfds, writefds, exceptfds,
                         user_timeout ? &timeout : NULL,
                         user_sigmask ? &sigmask : NULL);
    int my_errno = errno;
    filc_enter(my_thread);
    filc_unpin(filc_ptr_object(readfds_ptr));
    filc_unpin(filc_ptr_object(writefds_ptr));
    filc_unpin(filc_ptr_object(exceptfds_ptr));
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_getpeereid(filc_thread* my_thread, int fd, filc_ptr uid_ptr, filc_ptr gid_ptr)
{
    filc_exit(my_thread);
    uid_t uid;
    gid_t gid;
    int result = getpeereid(fd, &uid, &gid);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(result == -1 || !result);
    if (!result) {
        filc_check_write_int(uid_ptr, sizeof(unsigned), NULL);
        filc_check_write_int(gid_ptr, sizeof(unsigned), NULL);
        *(unsigned*)filc_ptr_ptr(uid_ptr) = uid;
        *(unsigned*)filc_ptr_ptr(gid_ptr) = gid;
        return 0;
    }
    filc_set_errno(my_errno);
    return -1;
}

int filc_native_zsys_kill(filc_thread* my_thread, int pid, int user_sig)
{
    int sig = filc_from_user_signum(user_sig);
    if (sig < 0) {
        filc_set_errno(EINVAL);
        return -1;
    }
    filc_exit(my_thread);
    int result = kill(pid, sig);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_raise(filc_thread* my_thread, int user_sig)
{
    int sig = filc_from_user_signum(user_sig);
    if (sig < 0) {
        filc_set_errno(EINVAL);
        return -1;
    }
    filc_exit(my_thread);
    int result = raise(sig);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_dup(filc_thread* my_thread, int fd)
{
    filc_exit(my_thread);
    int result = dup(fd);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_dup2(filc_thread* my_thread, int oldfd, int newfd)
{
    filc_exit(my_thread);
    int result = dup2(oldfd, newfd);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_sigprocmask(filc_thread* my_thread, int user_how, filc_ptr user_set_ptr,
                                 filc_ptr user_oldset_ptr)
{
    static const bool verbose = false;
    
    int how;
    if (FILC_MUSL) {
        switch (user_how) {
        case 0:
            how = SIG_BLOCK;
            break;
        case 1:
            how = SIG_UNBLOCK;
            break;
        case 2:
            how = SIG_SETMASK;
            break;
        default:
            filc_set_errno(EINVAL);
            return -1;
        }
    } else
        how = user_how;
    sigset_t* set;
    sigset_t* oldset;
    if (filc_ptr_ptr(user_set_ptr)) {
        filc_check_user_sigset(user_set_ptr, filc_read_access);
        set = alloca(sizeof(sigset_t));
        filc_from_user_sigset((filc_user_sigset*)filc_ptr_ptr(user_set_ptr), set);
    } else
        set = NULL;
    if (filc_ptr_ptr(user_oldset_ptr)) {
        oldset = alloca(sizeof(sigset_t));
        pas_zero_memory(oldset, sizeof(sigset_t));
    } else
        oldset = NULL;
    filc_exit(my_thread);
    if (verbose)
        pas_log("%s: setting sigmask\n", __PRETTY_FUNCTION__);
    int result = pthread_sigmask(how, set, oldset);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(result == -1 || !result);
    if (result < 0) {
        filc_set_errno(my_errno);
        return -1;
    }
    if (filc_ptr_ptr(user_oldset_ptr)) {
        PAS_ASSERT(oldset);
        filc_check_user_sigset(user_oldset_ptr, filc_write_access);
        filc_to_user_sigset(oldset, (filc_user_sigset*)filc_ptr_ptr(user_oldset_ptr));
    }
    return 0;
}

int filc_native_zsys_chdir(filc_thread* my_thread, filc_ptr path_ptr)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_exit(my_thread);
    int result = chdir(path);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_fork(filc_thread* my_thread)
{
    static const bool verbose = false;
    filc_exit(my_thread);
    if (verbose)
        pas_log("suspending scavenger\n");
    pas_scavenger_suspend();
    if (verbose)
        pas_log("suspending GC\n");
    fugc_suspend();
    if (verbose)
        pas_log("stopping world\n");
    filc_stop_the_world();
    /* NOTE: We don't have to lock the soft handshake lock, since now that the world is stopped and the
       FUGC is suspended, nobody could be using it. */
    if (verbose)
        pas_log("locking the parking lot\n");
    void* parking_lot_cookie = filc_parking_lot_lock();
    if (verbose)
        pas_log("locking thread list\n");
    filc_thread_list_lock_lock();
    pas_lock_disallowed = true;
    filc_thread* thread;
    for (thread = filc_first_thread; thread; thread = thread->next_thread)
        pas_system_mutex_lock(&thread->lock);
    int result = fork();
    int my_errno = errno;
    pas_lock_disallowed = false;
    if (verbose)
        pas_log("fork result = %d\n", result);
    if (!result) {
        /* We're in the child. Make sure that the thread list only contains the current thread and that
           the other threads know that they are dead due to fork. */
        for (thread = filc_first_thread; thread;) {
            filc_thread* next_thread = thread->next_thread;
            thread->prev_thread = NULL;
            thread->next_thread = NULL;
            if (thread != my_thread) {
                thread->forked = true;

                /* We can inspect the thread's TLC without any locks, since the thread is dead and
                   stopped. Also, start_thread (and other parts of the runtime) ensure that we only
                   call into libpas while entered - so the fact that we stop the world before
                   forking ensures that the dead thread is definitely not in the middle of a call
                   into libpas. */
                if (thread->tlc_node && thread->tlc_node->version == thread->tlc_node_version)
                    pas_thread_local_cache_destroy_remote_from_node(thread->tlc_node->cache);
            }
            pas_system_mutex_unlock(&thread->lock);
            thread = next_thread;
        }
        filc_first_thread = my_thread;
        PAS_ASSERT(filc_first_thread == my_thread);
        PAS_ASSERT(!filc_first_thread->next_thread);
        PAS_ASSERT(!filc_first_thread->prev_thread);

        /* FIXME: Maybe reuse tids??? */
    } else {
        for (thread = filc_first_thread; thread; thread = thread->next_thread)
            pas_system_mutex_unlock(&thread->lock);
    }
    filc_thread_list_lock_unlock();
    filc_parking_lot_unlock(parking_lot_cookie);
    filc_resume_the_world();
    fugc_resume();
    pas_scavenger_resume();
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

static int to_user_wait_status(int status)
{
    if (!FILC_MUSL)
        return status;
    if (WIFEXITED(status))
        return WEXITSTATUS(status) << 8;
    if (WIFSIGNALED(status))
        return filc_to_user_signum(WTERMSIG(status)) | (WCOREDUMP(status) ? 0x80 : 0);
    if (WIFSTOPPED(status))
        return 0x7f | (filc_to_user_signum(WSTOPSIG(status)) << 8);
    if (WIFCONTINUED(status))
        return 0xffff;
    PAS_ASSERT(!"Should not be reached");
    return 0;
}

int filc_native_zsys_waitpid(filc_thread* my_thread, int pid, filc_ptr status_ptr, int options)
{
    filc_exit(my_thread);
    int status;
    int result = waitpid(pid, &status, options);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0) {
        PAS_ASSERT(result == -1);
        filc_set_errno(my_errno);
        return -1;
    }
    if (filc_ptr_ptr(status_ptr)) {
        filc_check_write_int(status_ptr, sizeof(int), NULL);
        *(int*)filc_ptr_ptr(status_ptr) = to_user_wait_status(status);
    }
    return result;
}

int filc_native_zsys_listen(filc_thread* my_thread, int sockfd, int backlog)
{
    filc_exit(my_thread);
    int result = listen(sockfd, backlog);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setsid(filc_thread* my_thread)
{
    filc_exit(my_thread);
    int result = setsid();
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

static size_t length_of_null_terminated_ptr_array(filc_ptr array_ptr)
{
    size_t result = 0;
    for (;; result++) {
        filc_check_read_ptr(array_ptr, NULL);
        if (!filc_ptr_ptr(filc_ptr_load_with_manual_tracking((filc_ptr*)filc_ptr_ptr(array_ptr))))
            return result;
        array_ptr = filc_ptr_with_offset(array_ptr, sizeof(filc_ptr));
    }
}

char** filc_check_and_get_null_terminated_string_array(filc_thread* my_thread,
                                                       filc_ptr user_array_ptr)
{
    size_t array_length;
    char** array;
    array_length = length_of_null_terminated_ptr_array(user_array_ptr);
    array = (char**)filc_bmalloc_allocate_tmp(
        my_thread, filc_mul_size((array_length + 1), sizeof(char*)));
    (array)[array_length] = NULL;
    size_t index;
    for (index = array_length; index--;) {
        (array)[index] = filc_check_and_get_tmp_str(
            my_thread, filc_ptr_load(my_thread, (filc_ptr*)filc_ptr_ptr(user_array_ptr) + index));
    }
    return array;
}

int filc_native_zsys_execve(filc_thread* my_thread, filc_ptr pathname_ptr, filc_ptr argv_ptr,
                            filc_ptr envp_ptr)
{
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    char** argv = filc_check_and_get_null_terminated_string_array(my_thread, argv_ptr);
    char** envp = filc_check_and_get_null_terminated_string_array(my_thread, envp_ptr);
    filc_exit(my_thread);
    int result = execve(pathname, argv, envp);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(result == -1);
    filc_set_errno(my_errno);
    return -1;
}

int filc_native_zsys_getppid(filc_thread* my_thread)
{
    filc_exit(my_thread);
    int result = getppid();
    filc_enter(my_thread);
    return result;
}

int filc_native_zsys_chroot(filc_thread* my_thread, filc_ptr path_ptr)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_exit(my_thread);
    int result = chroot(path);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setuid(filc_thread* my_thread, unsigned uid)
{
    filc_exit(my_thread);
    int result = setuid(uid);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_seteuid(filc_thread* my_thread, unsigned uid)
{
    filc_exit(my_thread);
    int result = seteuid(uid);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setreuid(filc_thread* my_thread, unsigned ruid, unsigned euid)
{
    filc_exit(my_thread);
    int result = setreuid(ruid, euid);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setgid(filc_thread* my_thread, unsigned gid)
{
    filc_exit(my_thread);
    int result = setgid(gid);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setegid(filc_thread* my_thread, unsigned gid)
{
    filc_exit(my_thread);
    int result = setegid(gid);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setregid(filc_thread* my_thread, unsigned rgid, unsigned egid)
{
    filc_exit(my_thread);
    int result = setregid(rgid, egid);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_nanosleep(filc_thread* my_thread, filc_ptr user_req_ptr, filc_ptr user_rem_ptr)
{
    filc_check_read_int(user_req_ptr, sizeof(filc_user_timespec), NULL);
    struct timespec req;
    struct timespec rem;
    req.tv_sec = ((filc_user_timespec*)filc_ptr_ptr(user_req_ptr))->tv_sec;
    req.tv_nsec = ((filc_user_timespec*)filc_ptr_ptr(user_req_ptr))->tv_nsec;
    filc_exit(my_thread);
    int result = nanosleep(&req, &rem);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0) {
        filc_set_errno(my_errno);
        if (my_errno == EINTR && filc_ptr_ptr(user_rem_ptr)) {
            filc_check_write_int(user_rem_ptr, sizeof(filc_user_timespec), NULL);
            ((filc_user_timespec*)filc_ptr_ptr(user_rem_ptr))->tv_sec = rem.tv_sec;
            ((filc_user_timespec*)filc_ptr_ptr(user_rem_ptr))->tv_nsec = rem.tv_nsec;
        }
    }
    return result;
}

long filc_native_zsys_readlink(filc_thread* my_thread, filc_ptr path_ptr, filc_ptr buf_ptr, size_t bufsize)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_check_write_int(buf_ptr, bufsize, NULL);
    filc_pin(filc_ptr_object(buf_ptr));
    filc_exit(my_thread);
    long result = readlink(path, (char*)filc_ptr_ptr(buf_ptr), bufsize);
    int my_errno = errno;
    filc_enter(my_thread);
    filc_unpin(filc_ptr_object(buf_ptr));
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_chown(filc_thread* my_thread, filc_ptr pathname_ptr, unsigned owner,
                           unsigned group)
{
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    return FILC_SYSCALL(my_thread, chown(pathname, owner, group));
}

int filc_native_zsys_lchown(filc_thread* my_thread, filc_ptr pathname_ptr, unsigned owner,
                            unsigned group)
{
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    return FILC_SYSCALL(my_thread, lchown(pathname, owner, group));
}

int filc_native_zsys_rename(filc_thread* my_thread, filc_ptr oldname_ptr, filc_ptr newname_ptr)
{
    char* oldname = filc_check_and_get_tmp_str(my_thread, oldname_ptr);
    char* newname = filc_check_and_get_tmp_str(my_thread, newname_ptr);
    filc_exit(my_thread);
    int result = rename(oldname, newname);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_unlink(filc_thread* my_thread, filc_ptr path_ptr)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_exit(my_thread);
    int result = unlink(path);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_link(filc_thread* my_thread, filc_ptr oldname_ptr, filc_ptr newname_ptr)
{
    char* oldname = filc_check_and_get_tmp_str(my_thread, oldname_ptr);
    char* newname = filc_check_and_get_tmp_str(my_thread, newname_ptr);
    filc_exit(my_thread);
    int result = link(oldname, newname);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

static bool from_user_prot(int user_prot, int* prot)
{
    if (!FILC_MUSL) {
        if ((user_prot & PROT_EXEC))
            return false;
        *prot = user_prot;
        return true;
    }
    
    *prot = 0;
    if (filc_check_and_clear(&user_prot, 1))
        *prot |= PROT_READ;
    if (filc_check_and_clear(&user_prot, 2))
        *prot |= PROT_WRITE;
    return !user_prot;
}

static bool from_user_mmap_flags(int user_flags, int* flags)
{
    if (!FILC_MUSL) {
        if ((user_flags & MAP_FIXED))
            return false;
        *flags = user_flags;
        return true;
    }
    
    *flags = 0;
    if (filc_check_and_clear(&user_flags, 0x01))
        *flags |= MAP_SHARED;
    if (filc_check_and_clear(&user_flags, 0x02))
        *flags |= MAP_PRIVATE;
    if (filc_check_and_clear(&user_flags, 0x20))
        *flags |= MAP_ANON;
    return !user_flags;
}

static filc_ptr mmap_error_result(void)
{
    return filc_ptr_forge_invalid((void*)(intptr_t)-1);
}

filc_ptr filc_native_zsys_mmap(filc_thread* my_thread, filc_ptr address, size_t length, int user_prot,
                               int user_flags, int fd, long offset)
{
    static const bool verbose = false;
    if (filc_ptr_ptr(address)) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }
    int prot;
    if (!from_user_prot(user_prot, &prot)) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }
    int flags;
    if (!from_user_mmap_flags(user_flags, &flags)) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }
    if (!(flags & MAP_SHARED) && !(flags & MAP_PRIVATE)) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }
    if (!!(flags & MAP_SHARED) && !!(flags & MAP_PRIVATE)) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }
    filc_exit(my_thread);
    void* raw_result = mmap(NULL, length, prot, flags, fd, offset);
    int my_errno = errno;
    filc_enter(my_thread);
    if (raw_result == (void*)(intptr_t)-1) {
        filc_set_errno(my_errno);
        return mmap_error_result();
    }
    PAS_ASSERT(raw_result);
    filc_word_type initial_word_type;
    if ((flags & MAP_PRIVATE) && (flags & MAP_ANON) && fd == -1 && !offset
        && prot == (PROT_READ | PROT_WRITE)) {
        if (verbose)
            pas_log("using unset word type.\n");
        initial_word_type = FILC_WORD_TYPE_UNSET;
    } else {
        if (verbose)
            pas_log("using int word type.\n");
        initial_word_type = FILC_WORD_TYPE_INT;
    }
    filc_object* object = filc_allocate_with_existing_data(
        my_thread, raw_result, length, FILC_OBJECT_FLAG_MMAP, initial_word_type);
    PAS_ASSERT(object->lower == raw_result);
    return filc_ptr_create_with_manual_tracking(object);
}

int filc_native_zsys_munmap(filc_thread* my_thread, filc_ptr address, size_t length)
{
    filc_object* object = object_for_deallocate(address);
    FILC_CHECK(
        filc_object_size(object) == length,
        NULL,
        "cannot partially munmap (ptr = %s, length = %zu).",
        filc_ptr_to_new_string(address), length);
    FILC_CHECK(
        object->flags & FILC_OBJECT_FLAG_MMAP,
        NULL,
        "cannot munmap something that was not mmapped (ptr = %s).",
        filc_ptr_to_new_string(address));
    filc_free_yolo(my_thread, object);
    filc_exit(my_thread);
    filc_soft_handshake(filc_soft_handshake_no_op_callback, NULL);
    fugc_handshake(); /* Make sure we don't try to mark unmapped memory. */
    int result = munmap(filc_ptr_ptr(address), length);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_ftruncate(filc_thread* my_thread, int fd, long length)
{
    filc_exit(my_thread);
    int result = ftruncate(fd, length);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

filc_ptr filc_native_zsys_getcwd(filc_thread* my_thread, filc_ptr buf_ptr, size_t size)
{
    filc_check_write_int(buf_ptr, size, NULL);
    filc_pin(filc_ptr_object(buf_ptr));
    filc_exit(my_thread);
    char* result = getcwd((char*)filc_ptr_ptr(buf_ptr), size);
    int my_errno = errno;
    filc_enter(my_thread);
    filc_unpin(filc_ptr_object(buf_ptr));
    PAS_ASSERT(!result || result == (char*)filc_ptr_ptr(buf_ptr));
    if (!result)
        filc_set_errno(my_errno);
    if (!result)
        return filc_ptr_forge_null();
    return buf_ptr;
}

static bool from_user_dlopen_flags(int user_flags, int* flags)
{
    if (!FILC_MUSL) {
        *flags = user_flags;
        return true;
    }
    
    *flags = 0;
    if (filc_check_and_clear(&user_flags, 1))
        *flags |= RTLD_LAZY;
    if (filc_check_and_clear(&user_flags, 2))
        *flags |= RTLD_NOW;
    if (filc_check_and_clear(&user_flags, 4))
        *flags |= RTLD_NOLOAD;
    if (filc_check_and_clear(&user_flags, 4096))
        *flags |= RTLD_NODELETE;
    if (filc_check_and_clear(&user_flags, 256))
        *flags |= RTLD_GLOBAL;
    else
        *flags |= RTLD_LOCAL;
    return !user_flags;
}

filc_ptr filc_native_zsys_dlopen(filc_thread* my_thread, filc_ptr filename_ptr, int user_flags)
{
    int flags;
    if (!from_user_dlopen_flags(user_flags, &flags)) {
        set_dlerror("Unrecognized flag to dlopen");
        return filc_ptr_forge_null();
    }
    char* filename = filc_check_and_get_tmp_str_or_null(my_thread, filename_ptr);
    filc_exit(my_thread);
    void* handle = dlopen(filename, flags);
    filc_enter(my_thread);
    if (!handle) {
        set_dlerror(dlerror());
        return filc_ptr_forge_null();
    }
    return filc_ptr_create_with_manual_tracking(
        filc_allocate_special_with_existing_payload(my_thread, handle, FILC_WORD_TYPE_DL_HANDLE));
}

filc_ptr filc_native_zsys_dlsym(filc_thread* my_thread, filc_ptr handle_ptr, filc_ptr symbol_ptr)
{
    filc_check_access_special(handle_ptr, FILC_WORD_TYPE_DL_HANDLE, NULL);
    void* handle = filc_ptr_ptr(handle_ptr);
    char* symbol = filc_check_and_get_tmp_str(my_thread, symbol_ptr);
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);
    pas_string_stream stream;
    pas_string_stream_construct(&stream, &allocation_config);
    pas_string_stream_printf(&stream, "pizlonated_%s", symbol);
    filc_exit(my_thread);
    filc_ptr (*raw_symbol)(filc_global_initialization_context*) =
        dlsym(handle, pas_string_stream_get_string(&stream));
    filc_enter(my_thread);
    pas_string_stream_destruct(&stream);
    if (!raw_symbol) {
        set_dlerror(dlerror());
        return filc_ptr_forge_null();
    }
    return raw_symbol(NULL);
}

int filc_native_zsys_faccessat(filc_thread* my_thread, int user_dirfd, filc_ptr pathname_ptr,
                               int mode, int user_flags)
{
    int dirfd = filc_from_user_atfd(user_dirfd);
    int flags;
    if (!from_user_fstatat_flag(user_flags, &flags)) {
        filc_set_errno(EINVAL);
        return -1;
    }
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    filc_exit(my_thread);
    int result = faccessat(dirfd, pathname, mode, flags);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_sigwait(filc_thread* my_thread, filc_ptr sigmask_ptr, filc_ptr sig_ptr)
{
    filc_check_user_sigset(sigmask_ptr, filc_read_access);
    sigset_t sigmask;
    filc_from_user_sigset((filc_user_sigset*)filc_ptr_ptr(sigmask_ptr), &sigmask);
    filc_exit(my_thread);
    int signum;
    int result = sigwait(&sigmask, &signum);
    filc_enter(my_thread);
    if (result)
        return result;
    filc_check_write_int(sig_ptr, sizeof(int), NULL);
    *(int*)filc_ptr_ptr(sig_ptr) = filc_to_user_signum(signum);
    return 0;
}

int filc_native_zsys_fsync(filc_thread* my_thread, int fd)
{
    filc_exit(my_thread);
    int result = fsync(fd);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_shutdown(filc_thread* my_thread, int fd, int user_how)
{
    int how;
    if (FILC_MUSL) {
        switch (user_how) {
        case 0:
            how = SHUT_RD;
            break;
        case 1:
            how = SHUT_WR;
            break;
        case 2:
            how = SHUT_RDWR;
            break;
        default:
            filc_set_errno(EINVAL);
            return -1;
        }
    } else
        how = user_how;
    filc_exit(my_thread);
    int result = shutdown(fd, how);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_rmdir(filc_thread* my_thread, filc_ptr path_ptr)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_exit(my_thread);
    int result = rmdir(path);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

static void from_user_utime_timespec(filc_user_timespec* user_tv, struct timespec* tv)
{
    if (FILC_MUSL) {
        if (user_tv->tv_nsec == 0x3fffffff) {
            tv->tv_sec = 0;
            tv->tv_nsec = UTIME_NOW;
            return;
        }
        if (user_tv->tv_nsec == 0x3ffffffe) {
            tv->tv_sec = 0;
            tv->tv_nsec = UTIME_OMIT;
            return;
        }
    }
    tv->tv_sec = user_tv->tv_sec;
    tv->tv_nsec = user_tv->tv_nsec;
}

int filc_native_zsys_futimens(filc_thread* my_thread, int fd, filc_ptr times_ptr)
{
    struct timespec times[2];
    if (filc_ptr_ptr(times_ptr)) {
        filc_check_read_int(times_ptr, sizeof(filc_user_timespec) * 2, NULL);
        filc_user_timespec* user_times = (filc_user_timespec*)filc_ptr_ptr(times_ptr);
        from_user_utime_timespec(user_times + 0, times + 0);
        from_user_utime_timespec(user_times + 1, times + 1);
    }
    filc_exit(my_thread);
    int result = futimens(fd, filc_ptr_ptr(times_ptr) ? times : NULL);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_fchown(filc_thread* my_thread, int fd, unsigned uid, unsigned gid)
{
    return FILC_SYSCALL(my_thread, fchown(fd, uid, gid));
}

int filc_native_zsys_fchownat(filc_thread* my_thread, int user_fd, filc_ptr pathname_ptr, unsigned uid,
                              unsigned gid, int user_flags)
{
    int fd = filc_from_user_atfd(user_fd);
    int flags;
    if (!from_user_fstatat_flag(user_flags, &flags)) {
        filc_set_errno(EINVAL);
        return -1;
    }
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    return FILC_SYSCALL(my_thread, fchownat(fd, pathname, uid, gid, flags));
}

int filc_native_zsys_fchdir(filc_thread* my_thread, int fd)
{
    filc_exit(my_thread);
    int result = fchdir(fd);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

void filc_native_zsys_sync(filc_thread* my_thread)
{
    filc_exit(my_thread);
    sync();
    filc_enter(my_thread);
}

int filc_native_zsys_access(filc_thread* my_thread, filc_ptr path_ptr, int mode)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    return FILC_SYSCALL(my_thread, access(path, mode));
}

int filc_native_zsys_symlink(filc_thread* my_thread, filc_ptr oldname_ptr, filc_ptr newname_ptr)
{
    char* oldname = filc_check_and_get_tmp_str(my_thread, oldname_ptr);
    char* newname = filc_check_and_get_tmp_str(my_thread, newname_ptr);
    filc_exit(my_thread);
    int result = symlink(oldname, newname);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_mprotect(filc_thread* my_thread, filc_ptr addr_ptr, size_t len, int user_prot)
{
    int prot;
    if (!from_user_prot(user_prot, &prot)) {
        filc_set_errno(EINVAL);
        return -1;
    }
    if (prot == (PROT_READ | PROT_WRITE))
        filc_check_access_common(addr_ptr, len, filc_write_access, NULL);
    else {
        /* Protect the GC. We don't want the GC scanning pointers in protected memory. */
        filc_check_write_int(addr_ptr, len, NULL);
    }
    filc_check_pin_and_track_mmap(my_thread, addr_ptr);
    filc_exit(my_thread);
    int result = mprotect(filc_ptr_ptr(addr_ptr), len, prot);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_getgroups(filc_thread* my_thread, int size, filc_ptr list_ptr)
{
    size_t total_size;
    FILC_CHECK(
        !pas_mul_uintptr_overflow(sizeof(unsigned), size, &total_size),
        NULL,
        "size argument too big, causes overflow; size = %d.",
        size);
    filc_check_write_int(list_ptr, total_size, NULL);
    filc_pin(filc_ptr_object(list_ptr));
    filc_exit(my_thread);
    PAS_ASSERT(sizeof(gid_t) == sizeof(unsigned));
    int result = getgroups(size, (gid_t*)filc_ptr_ptr(list_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    filc_unpin(filc_ptr_object(list_ptr));
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_getpgrp(filc_thread* my_thread)
{
    filc_exit(my_thread);
    int result = getpgrp();
    filc_enter(my_thread);
    return result;
}

int filc_native_zsys_getpgid(filc_thread* my_thread, int pid)
{
    filc_exit(my_thread);
    int result = getpgid(pid);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result == -1)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setpgid(filc_thread* my_thread, int pid, int pgrp)
{
    filc_exit(my_thread);
    int result = setpgid(pid, pgrp);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

long filc_native_zsys_pread(filc_thread* my_thread, int fd, filc_ptr buf_ptr, size_t nbytes,
                            long offset)
{
    filc_cpt_write_int(my_thread, buf_ptr, nbytes);
    return FILC_SYSCALL(my_thread, pread(fd, filc_ptr_ptr(buf_ptr), nbytes, offset));
}

long filc_native_zsys_preadv(filc_thread* my_thread, int fd, filc_ptr user_iov_ptr, int iovcnt,
                             long offset)
{
    struct iovec* iov = filc_prepare_iovec(my_thread, user_iov_ptr, iovcnt, filc_write_access);
    return FILC_SYSCALL(my_thread, preadv(fd, iov, iovcnt, offset));
}

long filc_native_zsys_pwrite(filc_thread* my_thread, int fd, filc_ptr buf_ptr, size_t nbytes,
                             long offset)
{
    filc_cpt_read_int(my_thread, buf_ptr, nbytes);
    return FILC_SYSCALL(my_thread, pwrite(fd, filc_ptr_ptr(buf_ptr), nbytes, offset));
}

long filc_native_zsys_pwritev(filc_thread* my_thread, int fd, filc_ptr user_iov_ptr, int iovcnt,
                              long offset)
{
    struct iovec* iov = filc_prepare_iovec(my_thread, user_iov_ptr, iovcnt, filc_read_access);
    return FILC_SYSCALL(my_thread, pwritev(fd, iov, iovcnt, offset));
}

int filc_native_zsys_getsid(filc_thread* my_thread, int pid)
{
    return FILC_SYSCALL(my_thread, getsid(pid));
}

static int mlock_impl(filc_thread* my_thread, filc_ptr addr_ptr, size_t len,
                      int (*actual_mlock)(const void*, size_t))
{
    filc_check_access_common(addr_ptr, len, filc_read_access, NULL);
    filc_check_pin_and_track_mmap(my_thread, addr_ptr);
    return FILC_SYSCALL(my_thread, actual_mlock(filc_ptr_ptr(addr_ptr), len));
}

int filc_native_zsys_mlock(filc_thread* my_thread, filc_ptr addr_ptr, size_t len)
{
    return mlock_impl(my_thread, addr_ptr, len, mlock);
}

int filc_native_zsys_munlock(filc_thread* my_thread, filc_ptr addr_ptr, size_t len)
{
    return mlock_impl(my_thread, addr_ptr, len, munlock);
}

static bool from_user_mlockall_flags(int user_flags, int* flags)
{
    if (!FILC_MUSL) {
        *flags = user_flags;
        return true;
    }

    *flags = 0;
    if (filc_check_and_clear(&user_flags, 1))
        *flags |= MCL_CURRENT;
    if (filc_check_and_clear(&user_flags, 2))
        *flags |= MCL_FUTURE;
    return !user_flags;
}

int filc_native_zsys_mlockall(filc_thread* my_thread, int user_flags)
{
    int flags;
    if (!from_user_mlockall_flags(user_flags, &flags)) {
        filc_set_errno(EINVAL);
        return -1;
    }
    return FILC_SYSCALL(my_thread, mlockall(flags));
}

int filc_native_zsys_munlockall(filc_thread* my_thread)
{
    return FILC_SYSCALL(my_thread, munlockall());
}

int filc_native_zsys_sigpending(filc_thread* my_thread, filc_ptr set_ptr)
{
    sigset_t set;
    if (FILC_SYSCALL(my_thread, sigpending(&set)) < 0)
        return -1;
    filc_check_user_sigset(set_ptr, filc_write_access);
    filc_to_user_sigset(&set, (filc_user_sigset*)filc_ptr_ptr(set_ptr));
    return 0;
}

filc_ptr filc_native_zthread_self(filc_thread* my_thread)
{
    static const bool verbose = false;
    filc_ptr result = filc_ptr_for_special_payload_with_manual_tracking(my_thread);
    if (verbose)
        pas_log("my_thread = %p, zthread_self result = %s\n", my_thread, filc_ptr_to_new_string(result));
    return result;
}

unsigned filc_native_zthread_get_id(filc_thread* my_thread, filc_ptr thread_ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    check_zthread(thread_ptr);
    filc_thread* thread = (filc_thread*)filc_ptr_ptr(thread_ptr);
    return thread->tid;
}

unsigned filc_native_zthread_self_id(filc_thread* my_thread)
{
    PAS_ASSERT(my_thread->tid);
    return my_thread->tid;
}

filc_ptr filc_native_zthread_get_cookie(filc_thread* my_thread, filc_ptr thread_ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    check_zthread(thread_ptr);
    filc_thread* thread = (filc_thread*)filc_ptr_ptr(thread_ptr);
    return filc_ptr_load_with_manual_tracking(&thread->cookie_ptr);
}

void filc_native_zthread_set_self_cookie(filc_thread* my_thread, filc_ptr cookie_ptr)
{
    filc_ptr_store(my_thread, &my_thread->cookie_ptr, cookie_ptr);
}

static void* start_thread(void* arg)
{
    static const bool verbose = false;
    
    filc_thread* thread;

    thread = (filc_thread*)arg;

    unsigned tid = thread->tid;

    if (verbose)
        pas_log("thread %u (%p) starting\n", tid, thread);

    PAS_ASSERT(thread->has_started);
    PAS_ASSERT(!thread->has_stopped);
    PAS_ASSERT(!thread->error_starting);

    pthread_detach(thread->thread);

    PAS_ASSERT(!pthread_setspecific(filc_thread_key, thread));
    PAS_ASSERT(!thread->thread);
    pas_fence();
    thread->thread = pthread_self();

    PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &thread->initial_blocked_sigs, NULL));
    
    filc_enter(thread);

    thread->tlc_node = verse_heap_get_thread_local_cache_node();
    thread->tlc_node_version = pas_thread_local_cache_node_version(thread->tlc_node);

    FILC_DEFINE_RUNTIME_ORIGIN(origin, "start_thread", 0);

    struct {
        FILC_FRAME_BODY;
    } actual_frame;
    pas_zero_memory(&actual_frame, sizeof(actual_frame));
    filc_frame* frame = (filc_frame*)&actual_frame;
    frame->origin = &origin;
    filc_push_frame(thread, frame);

    filc_native_frame native_frame;
    filc_push_native_frame(thread, &native_frame);

    filc_return_buffer return_buffer;
    filc_ptr rets = filc_ptr_for_ptr_return_buffer(&return_buffer);

    filc_ptr args = filc_ptr_create(thread, filc_allocate(thread, sizeof(filc_ptr)));
    filc_check_write_ptr(args, NULL);
    filc_ptr_store(thread, (filc_ptr*)filc_ptr_ptr(args), filc_ptr_load(thread, &thread->arg_ptr));
    filc_ptr_store(thread, &thread->arg_ptr, filc_ptr_forge_null());

    filc_lock_top_native_frame(thread);

    if (verbose)
        pas_log("thread %u calling main function\n", tid);

    PAS_ASSERT(!thread->thread_main(thread, args, rets));

    if (verbose)
        pas_log("thread %u main function returned\n", tid);

    filc_unlock_top_native_frame(thread);
    filc_ptr result = *(filc_ptr*)filc_ptr_ptr(rets);
    filc_thread_track_object(thread, filc_ptr_object(result));

    pas_system_mutex_lock(&thread->lock);
    PAS_ASSERT(!thread->has_stopped);
    PAS_ASSERT(thread->thread);
    PAS_ASSERT(thread->thread == pthread_self());
    filc_ptr_store(thread, &thread->result_ptr, result);
    pas_system_mutex_unlock(&thread->lock);

    filc_pop_native_frame(thread, &native_frame);
    filc_pop_frame(thread, frame);

    sigset_t set;
    pas_reasonably_fill_sigset(&set);
    if (verbose)
        pas_log("%s: blocking signals\n", __PRETTY_FUNCTION__);
    PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &set, NULL));

    fugc_donate(&thread->mark_stack);
    filc_thread_stop_allocators(thread);
    filc_thread_relinquish_tid(thread);
    thread->is_stopping = true;
    filc_thread_undo_create(thread);
    pas_thread_local_cache_destroy(pas_lock_is_not_held);
    filc_exit(thread);

    pas_system_mutex_lock(&thread->lock);
    PAS_ASSERT(!thread->has_stopped);
    PAS_ASSERT(thread->thread);
    PAS_ASSERT(!(thread->state & FILC_THREAD_STATE_ENTERED));
    PAS_ASSERT(!(thread->state & FILC_THREAD_STATE_DEFERRED_SIGNAL));
    size_t index;
    for (index = FILC_MAX_USER_SIGNUM + 1; index--;)
        PAS_ASSERT(!thread->num_deferred_signals[index]);
    thread->thread = NULL;
    thread->has_stopped = true;
    pas_system_condition_broadcast(&thread->cond);
    pas_system_mutex_unlock(&thread->lock);
    
    if (verbose)
        pas_log("thread %u disposing\n", tid);

    filc_thread_dispose(thread);

    /* At this point, the GC no longer sees this thread except if the user is holding references to it.
       And since we're exited, the GC could run at any time. So the thread might be alive or it might be
       dead - we don't know. */

    return NULL;
}

filc_ptr filc_native_zthread_create(filc_thread* my_thread, filc_ptr callback_ptr, filc_ptr arg_ptr)
{
    filc_check_function_call(callback_ptr);
    filc_thread* thread = filc_thread_create();
    filc_thread_track_object(my_thread, filc_object_for_special_payload(thread));
    pas_system_mutex_lock(&thread->lock);
    /* I don't see how this could ever happen. */
    PAS_ASSERT(!thread->thread);
    PAS_ASSERT(filc_ptr_is_totally_null(thread->arg_ptr));
    PAS_ASSERT(filc_ptr_is_totally_null(thread->result_ptr));
    PAS_ASSERT(filc_ptr_is_totally_null(thread->cookie_ptr));
    thread->thread_main = (bool (*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(callback_ptr);
    filc_ptr_store(my_thread, &thread->arg_ptr, arg_ptr);
    pas_system_mutex_unlock(&thread->lock);
    filc_exit(my_thread);
    /* Make sure we don't create threads while in a handshake. This will hold the thread in the
       !has_started && !thread state, so if the soft handshake doesn't see it, that's fine. */
    filc_stop_the_world_lock_lock();
    filc_wait_for_world_resumption_holding_lock();
    filc_soft_handshake_lock_lock();
    thread->has_started = true;
    pthread_t ignored_thread;
    sigset_t fullset;
    pas_reasonably_fill_sigset(&fullset);
    PAS_ASSERT(!pthread_sigmask(SIG_BLOCK, &fullset, &thread->initial_blocked_sigs));
    int result = pthread_create(&ignored_thread, NULL, start_thread, thread);
    PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &thread->initial_blocked_sigs, NULL));
    if (result)
        thread->has_started = false;
    filc_soft_handshake_lock_unlock();
    filc_stop_the_world_lock_unlock();
    filc_enter(my_thread);
    if (result) {
        pas_system_mutex_lock(&thread->lock);
        PAS_ASSERT(!thread->thread);
        thread->error_starting = true;
        filc_thread_undo_create(thread);
        pas_system_mutex_unlock(&thread->lock);
        filc_thread_relinquish_tid(thread);
        filc_thread_dispose(thread);
        filc_set_errno(result);
        return filc_ptr_forge_null();
    }
    return filc_ptr_for_special_payload_with_manual_tracking(thread);
}

bool filc_native_zthread_join(filc_thread* my_thread, filc_ptr thread_ptr, filc_ptr result_ptr)
{
    check_zthread(thread_ptr);
    filc_thread* thread = (filc_thread*)filc_ptr_ptr(thread_ptr);
    /* Should never happen because we'd never vend such a thread to the user. */
    PAS_ASSERT(thread->has_started);
    PAS_ASSERT(!thread->error_starting);
    if (thread->forked) {
        filc_set_errno(ESRCH);
        return false;
    }
    filc_exit(my_thread);
    pas_system_mutex_lock(&thread->lock);
    /* Note that this loop doesn't have to worry about forked. If we forked and this ended up in a child,
       then this thread would be dead and we wouldn't care. */
    while (!thread->has_stopped)
        pas_system_condition_wait(&thread->cond, &thread->lock);
    pas_system_mutex_unlock(&thread->lock);
    filc_enter(my_thread);
    if (filc_ptr_ptr(result_ptr)) {
        filc_check_write_ptr(result_ptr, NULL);
        filc_ptr_store(
            my_thread, (filc_ptr*)filc_ptr_ptr(result_ptr),
            filc_ptr_load_with_manual_tracking(&thread->result_ptr));
    }
    return true;
}

typedef struct {
    filc_thread* my_thread;
    bool (*condition)(PIZLONATED_SIGNATURE);
    bool (*before_sleep)(PIZLONATED_SIGNATURE);
    filc_ptr arg_ptr;
} zpark_if_data;

static bool zpark_if_validate_callback(void* arg)
{
    zpark_if_data* data = (zpark_if_data*)arg;

    filc_return_buffer return_buffer;
    filc_ptr rets = filc_ptr_for_int_return_buffer(&return_buffer);

    filc_ptr args = filc_ptr_create(
        data->my_thread, filc_allocate(data->my_thread, sizeof(filc_ptr)));
    filc_check_write_ptr(args, NULL);
    filc_ptr_store(data->my_thread, (filc_ptr*)filc_ptr_ptr(args), data->arg_ptr);

    filc_lock_top_native_frame(data->my_thread);
    PAS_ASSERT(!data->condition(data->my_thread, args, rets));
    filc_unlock_top_native_frame(data->my_thread);

    return *(bool*)filc_ptr_ptr(rets);
}

static void zpark_if_before_sleep_callback(void* arg)
{
    zpark_if_data* data = (zpark_if_data*)arg;

    filc_return_buffer return_buffer;
    filc_ptr rets = filc_ptr_for_int_return_buffer(&return_buffer);

    filc_ptr args = filc_ptr_create(
        data->my_thread, filc_allocate(data->my_thread, sizeof(filc_ptr)));
    filc_check_write_ptr(args, NULL);
    filc_ptr_store(data->my_thread, (filc_ptr*)filc_ptr_ptr(args), data->arg_ptr);

    filc_lock_top_native_frame(data->my_thread);
    PAS_ASSERT(!data->before_sleep(data->my_thread, args, rets));
    filc_unlock_top_native_frame(data->my_thread);
}

int filc_native_zpark_if(filc_thread* my_thread, filc_ptr address_ptr, filc_ptr condition_ptr,
                         filc_ptr before_sleep_ptr, filc_ptr arg_ptr,
                         double absolute_timeout_in_milliseconds)
{
    FILC_CHECK(
        filc_ptr_ptr(address_ptr),
        NULL,
        "cannot zpark on a null address.");
    filc_check_function_call(condition_ptr);
    filc_check_function_call(before_sleep_ptr);
    zpark_if_data data;
    data.my_thread = my_thread;
    data.condition = (bool (*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(condition_ptr);
    data.before_sleep = (bool (*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(before_sleep_ptr);
    data.arg_ptr = arg_ptr;
    return filc_park_conditionally(my_thread,
                                   filc_ptr_ptr(address_ptr),
                                   zpark_if_validate_callback,
                                   zpark_if_before_sleep_callback,
                                   &data,
                                   absolute_timeout_in_milliseconds);
}

typedef struct {
    filc_thread* my_thread;
    bool (*callback)(PIZLONATED_SIGNATURE);
    filc_ptr arg_ptr;
} zunpark_one_data;

typedef struct {
    bool did_unpark_thread;
    bool may_have_more_threads;
    filc_ptr arg_ptr;
} zunpark_one_callback_args;

static void zunpark_one_callback(filc_unpark_result result, void* arg)
{
    zunpark_one_data* data = (zunpark_one_data*)arg;

    filc_return_buffer return_buffer;
    filc_ptr rets = filc_ptr_for_int_return_buffer(&return_buffer);

    filc_ptr args = filc_ptr_create(
        data->my_thread, filc_allocate(data->my_thread, sizeof(zunpark_one_callback_args)));
    FILC_CHECK_INT_FIELD(args, zunpark_one_callback_args, did_unpark_thread, filc_write_access);
    FILC_CHECK_INT_FIELD(args, zunpark_one_callback_args, may_have_more_threads, filc_write_access);
    FILC_CHECK_PTR_FIELD(args, zunpark_one_callback_args, arg_ptr, filc_write_access);
    zunpark_one_callback_args* raw_args = (zunpark_one_callback_args*)filc_ptr_ptr(args);
    raw_args->did_unpark_thread = result.did_unpark_thread;
    raw_args->may_have_more_threads = result.may_have_more_threads;
    filc_ptr_store(data->my_thread, &raw_args->arg_ptr, data->arg_ptr);

    filc_lock_top_native_frame(data->my_thread);
    PAS_ASSERT(!data->callback(data->my_thread, args, rets));
    filc_unlock_top_native_frame(data->my_thread);
}

void filc_native_zunpark_one(filc_thread* my_thread, filc_ptr address_ptr, filc_ptr callback_ptr,
                             filc_ptr arg_ptr)
{
    FILC_CHECK(
        filc_ptr_ptr(address_ptr),
        NULL,
        "cannot zunpark on a null address.");
    filc_check_function_call(callback_ptr);
    zunpark_one_data data;
    data.my_thread = my_thread;
    data.callback = (bool (*)(PIZLONATED_SIGNATURE))filc_ptr_ptr(callback_ptr);
    data.arg_ptr = arg_ptr;
    filc_unpark_one(my_thread, filc_ptr_ptr(address_ptr), zunpark_one_callback, &data);
}

unsigned filc_native_zunpark(filc_thread* my_thread, filc_ptr address_ptr, unsigned count)
{
    FILC_CHECK(
        filc_ptr_ptr(address_ptr),
        NULL,
        "cannot zunpark on a null address.");
    return filc_unpark(my_thread, filc_ptr_ptr(address_ptr), count);
}

void filc_thread_destroy_space_with_guard_page(filc_thread* my_thread)
{
#if FILC_MUSL
    PAS_UNUSED_PARAM(my_thread);
#else /* FILC_MUSL -> so !FILC_MUSL */
    if (!my_thread->space_with_guard_page) {
        PAS_ASSERT(!my_thread->guard_page);
        return;
    }
    PAS_ASSERT(my_thread->guard_page > my_thread->space_with_guard_page);
    pas_page_malloc_deallocate(
        my_thread->space_with_guard_page,
        my_thread->guard_page - my_thread->space_with_guard_page + pas_page_malloc_alignment());
    my_thread->space_with_guard_page = NULL;
    my_thread->guard_page = NULL;
#endif /* FILC_MUSL -> so end of !FILC_MUSL */
}

#if !FILC_MUSL
char* filc_thread_get_end_of_space_with_guard_page_with_size(filc_thread* my_thread,
                                                             size_t desired_size)
{
    PAS_ASSERT(my_thread->guard_page >= my_thread->space_with_guard_page);
    if ((size_t)(my_thread->guard_page - my_thread->space_with_guard_page) >= desired_size) {
        PAS_ASSERT(my_thread->space_with_guard_page);
        PAS_ASSERT(my_thread->guard_page);
        return my_thread->guard_page;
    }
    filc_thread_destroy_space_with_guard_page(my_thread);
    size_t size = pas_round_up_to_power_of_2(desired_size, pas_page_malloc_alignment());
    pas_aligned_allocation_result result = pas_page_malloc_try_allocate_without_deallocating_padding(
        size + pas_page_malloc_alignment(), pas_alignment_create_trivial(), pas_committed);
    PAS_ASSERT(!result.left_padding_size);
    PAS_ASSERT(!result.right_padding_size);
    pas_page_malloc_protect_reservation((char*)result.result + size, pas_page_malloc_alignment());
    my_thread->space_with_guard_page = (char*)result.result;
    my_thread->guard_page = (char*)result.result + size;
    return (char*)result.result + size;
}
#endif /* !FILC_MUSL */

size_t filc_mul_size(size_t a, size_t b)
{
    size_t result;
    FILC_CHECK(
        !pas_mul_uintptr_overflow(a, b, &result),
        NULL,
        "multiplication %zu * %zu overflowed", a, b);
    return result;
}

bool filc_get_bool_env(const char* name, bool default_value)
{
    char* value = getenv(name);
    if (!value)
        return default_value;
    if (!strcmp(value, "1") ||
        !strcasecmp(value, "yes") ||
        !strcasecmp(value, "true"))
        return true;
    if (!strcmp(value, "0") ||
        !strcasecmp(value, "no") ||
        !strcasecmp(value, "false"))
        return false;
    pas_panic("invalid environment variable %s value: %s (expected boolean like 1, yes, true, 0, no, "
              "or false)\n", name, value);
    return false;
}

unsigned filc_get_unsigned_env(const char* name, unsigned default_value)
{
    char* value = getenv(name);
    if (!value)
        return default_value;
    unsigned result;
    if (sscanf(value, "%u", &result) == 1)
        return result;
    pas_panic("invalid environment variable %s value: %s (expected decimal unsigned int)\n",
              name, value);
    return 0;
}

size_t filc_get_size_env(const char* name, size_t default_value)
{
    char* value = getenv(name);
    if (!value)
        return default_value;
    size_t result;
    if (sscanf(value, "%zu", &result) == 1)
        return result;
    pas_panic("invalid environment variable %s value: %s (expected decimal byte size)\n",
              name, value);
    return 0;
}

#endif /* PAS_ENABLE_FILC */

#endif /* LIBPAS_ENABLED */

