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
#include <sys/mman.h>
#include <dlfcn.h>
#include <poll.h>
#include <sys/timex.h>
#include <sys/file.h>
#include <sys/sendfile.h>
#include <futex_calls.h>
#include <dirent.h>
#include <sys/random.h>
#include <sys/epoll.h>
#include <sys/sysinfo.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/eventfd.h>
#include <sys/xattr.h>
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/signalfd.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>

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

const filc_object filc_free_singleton = {
    .upper = (void*)(&filc_free_singleton + 1),
    .aux = FILC_AUX_CREATE(FILC_OBJECT_FLAG_FREE |
                           FILC_OBJECT_FLAG_GLOBAL |
                           FILC_OBJECT_FLAG_READONLY,
                           NULL)
};

filc_object_array filc_global_variable_roots;

void filc_check_user_sigset(filc_ptr ptr, filc_access_kind access_kind)
{
    filc_check_access(ptr, sizeof(sigset_t), access_kind);
}

static void check_user_sigaction(filc_ptr ptr, filc_access_kind access_kind)
{
    filc_check_access(ptr, sizeof(struct sigaction), access_kind);
}

/* NOTE: Unlike most other allocation functions, this does not track the allocated object properly.
   It registers it with the global thread list. But once the thread is started, it will dispose itself
   once done, and remove it from the list. So, it's necessary to track threads after creating them
   somehow, unless it's a thread that cannot be disposed. */
filc_thread* filc_thread_create_with_manual_tracking(void)
{
    static const bool verbose = false;
    if (sizeof(filc_thread) > FILC_THREAD_ALLOCATOR_OFFSET) {
        pas_log("Expected allocator offset to be %u, but base thread size is %zu.\n",
                FILC_THREAD_ALLOCATOR_OFFSET, sizeof(filc_thread));
    }
    PAS_ASSERT(sizeof(filc_thread) <= FILC_THREAD_ALLOCATOR_OFFSET);
    filc_object* thread_object = filc_allocate_special_early(
        FILC_THREAD_SIZE_WITH_ALLOCATORS, FILC_CC_ALIGNMENT, FILC_SPECIAL_TYPE_THREAD);
    filc_thread* thread = filc_object_lower(thread_object);
    if (verbose)
        pas_log("created thread: %p, size %zu\n", thread, (size_t)FILC_THREAD_SIZE_WITH_ALLOCATORS);
    PAS_ASSERT(filc_object_for_special_payload(thread) == thread_object);

    pas_system_mutex_construct(&thread->lock);
    pas_system_condition_construct(&thread->cond);
    filc_ptr_array_construct(&thread->allocation_roots);
    filc_object_array_construct(&thread->mark_stack);

    unsigned allocator_index = 0;
    unsigned last_size = UINT_MAX;
#define SIZE_CLASS(size) do { \
        PAS_ASSERT(allocator_index < FILC_THREAD_NUM_ALLOCATORS); \
        PAS_ASSERT((allocator_index << VERSE_HEAP_MIN_ALIGN_SHIFT) <= (size)); \
        PAS_ASSERT(size); \
        PAS_ASSERT(pas_is_aligned((size), VERSE_HEAP_MIN_ALIGN)); \
        verse_local_allocator_construct( \
            filc_thread_allocator(thread, allocator_index), fugc_default_heap, (size), \
            FILC_THREAD_ALLOCATOR_SIZE); \
        last_size = (size); \
        allocator_index++; \
    } while (false)
    SIZE_CLASS(16);
    SIZE_CLASS(16);
    SIZE_CLASS(32);
    SIZE_CLASS(48);
    SIZE_CLASS(64);
    SIZE_CLASS(80);
    SIZE_CLASS(96);
    SIZE_CLASS(128);
    SIZE_CLASS(128);
    SIZE_CLASS(160);
    SIZE_CLASS(160);
    SIZE_CLASS(192);
    SIZE_CLASS(192);
    SIZE_CLASS(224);
    SIZE_CLASS(224);
    SIZE_CLASS(256);
    SIZE_CLASS(256);
    SIZE_CLASS(304);
    SIZE_CLASS(304);
    SIZE_CLASS(304);
    SIZE_CLASS(352);
    SIZE_CLASS(352);
    SIZE_CLASS(352);
    SIZE_CLASS(416);
    SIZE_CLASS(416);
    SIZE_CLASS(416);
    SIZE_CLASS(416);
#undef SIZE_CLASS
    PAS_ASSERT(allocator_index == FILC_THREAD_NUM_ALLOCATORS);
    PAS_ASSERT(last_size == FILC_THREAD_MAX_INLINE_SIZE_CLASS);

    /* The rest of the fields are initialized to zero already. */

    filc_thread_list_lock_lock();
    thread->next_thread = filc_first_thread;
    thread->prev_thread = NULL;
    if (filc_first_thread)
        filc_first_thread->prev_thread = thread;
    filc_first_thread = thread;
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
    PAS_ASSERT(!thread->allocation_roots.size);
    PAS_ASSERT(!thread->mark_stack.num_objects);
    filc_ptr_array_destruct(&thread->allocation_roots);
    filc_object_array_destruct(&thread->mark_stack);
    filc_thread_destroy_space_with_guard_page(thread);
}

void filc_thread_mark_outgoing_ptrs(filc_thread* thread, filc_object_array* stack)
{
    /* There's a bunch of other stuff that threads "point" to that is part of their roots, and we
       mark those as part of marking thread roots. The things here are the ones that are treated
       as normal outgoing object ptrs rather than roots. */
    
    fugc_mark_or_free_flight(stack, &thread->arg_ptr);
    fugc_mark_or_free_flight(stack, &thread->cookie_ptr);
    fugc_mark_or_free_flight(stack, &thread->result_ptr);

    /* These need to be marked because phase2 of unwinding calls the personality function multiple
       times before finishing using them. */
    fugc_mark_or_free_flight(stack, &thread->unwind_context_ptr);
    fugc_mark_or_free_flight(stack, &thread->exception_object_ptr);
}

void filc_thread_destruct(filc_thread* thread)
{
    static const bool verbose = false;

    if (verbose)
        pas_log("destructing thread %p\n", thread);
    
    PAS_ASSERT(thread->has_stopped || thread->error_starting || thread->forked);

#if PAS_OS(LINUX)
    /* On Linux, pthread_condition_destroy waits if there's anyone waiting on the condition
       variable, which is totally not what we want.
       
       Also, the Linux mutex/condition destroy functions don't actually release any resources.
       
       So we can skip them. */
#else /* PAS_OS(LINUX) -> so !PAS_OS(LINUX) */
    /* Shockingly, the BSDs use a pthread_mutex/pthread_cond implementation that actually requires
       destruction. What the fugc. */
    pas_system_mutex_destruct(&thread->lock);
    pas_system_condition_destruct(&thread->cond);
#endif /* PAS_OS(LINUX) -> so end of !PAS_OS(LINUX) */

    if (verbose)
        pas_log("destructed thread %p\n", thread);
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
    filc_check_access_special(ptr, FILC_SPECIAL_TYPE_THREAD);
}

static filc_signal_handler* signal_table[FILC_MAX_USER_SIGNUM + 1];

static bool is_initialized = false; /* Useful for assertions. */
static bool exit_on_panic = false;
static bool dump_errnos = false;
static bool run_global_ctors = true;
static bool run_global_dtors = true;
static bool verbose_stop_the_world = false;

static void set_stack_limit(filc_thread* thread)
{
    static const bool verbose = false;
    
    static const size_t stack_slack = 32768;
    
    char* stack = (char*)pthread_getstack_yolo(pthread_self());
    size_t stack_size = pthread_getstacksize_yolo(pthread_self());

    if (verbose)
        pas_log("stack = %p, stack_size = %zu\n", stack, stack_size);

    PAS_ASSERT(stack);
    PAS_ASSERT(stack_size);
    PAS_ASSERT((char*)&stack < stack);
    PAS_ASSERT((char*)&stack > stack - stack_size);
    PAS_ASSERT(stack_size > stack_slack);
    PAS_ASSERT((char*)&stack > stack - stack_size + stack_slack);

    thread->stack_limit = stack - stack_size + stack_slack;
}

static int file_log_fd = -1;

static void open_new_log_file(void)
{
    char buf[100];
    snprintf(buf, sizeof(buf), "filc-log-%d.txt", getpid());
    file_log_fd = open(buf, O_CREAT | O_WRONLY, 0644);
    PAS_ASSERT(file_log_fd >= 0);
    pas_fd_stream* stream = (pas_fd_stream*)bmalloc_allocate(sizeof(pas_fd_stream));
    pas_fd_stream_construct(stream, file_log_fd);
    pas_log_stream = &stream->base;
}

static void open_new_log_file_if_necessary(void)
{
    if (file_log_fd >= 0)
        open_new_log_file();
}

void filc_initialize(void)
{
    if (filc_get_bool_env("FILC_LOG_TO_FILE", false))
        open_new_log_file();
    
    PAS_ASSERT(!is_initialized);

    /* There's a bunch of alignment math that is simpler if these things are true. If you make any of
       these conditions fail to hold, then it's an audit-the-runtime kind of situation. */
    PAS_ASSERT(sizeof(filc_object) == FILC_MINALIGN);
    PAS_ASSERT(FILC_MINALIGN == VERSE_HEAP_MIN_ALIGN);
    PAS_ASSERT(sizeof(filc_lower_or_box) == sizeof(void*));
    PAS_ASSERT(sizeof(filc_lower_or_box) == sizeof(uintptr_t));
    PAS_ASSERT(sizeof(filc_lower_or_box) == FILC_WORD_SIZE);

    /* The compiler hardcodes these constants. */
    PAS_ASSERT(FILC_FLIGHT_PTR_ALIGNMENT == 16);
    PAS_ASSERT(FILC_OBJECT_AUX_PTR_MASK == 0xfffffffffffflu);
    PAS_ASSERT(FILC_OBJECT_AUX_FLAGS_SHIFT == 48);
    PAS_ASSERT(sizeof(filc_object) == 16);
    PAS_ASSERT(FILC_SPECIAL_TYPE_FUNCTION == 1);
    PAS_ASSERT(FILC_SPECIAL_TYPE_MASK == 15);
    PAS_ASSERT(FILC_OBJECT_FLAG_GLOBAL == 1);
    PAS_ASSERT(FILC_OBJECT_FLAG_READONLY == 2);
    PAS_ASSERT(FILC_OBJECT_FLAG_FREE == 4);
    PAS_ASSERT(FILC_OBJECT_FLAG_GLOBAL_AUX == 16);
    PAS_ASSERT(FILC_OBJECT_FLAGS_SPECIAL_SHIFT == 6);
    PAS_ASSERT(FILC_OBJECT_FLAGS_ALIGN_SHIFT == 10);
    PAS_ASSERT(FILC_ATOMIC_BOX_BIT == 1);
    PAS_ASSERT(FILC_NUM_UNWIND_REGISTERS == 2);
    PAS_ASSERT(FILC_CC_INLINE_SIZE == 256);
    PAS_ASSERT(FILC_CC_ALIGNMENT == 64);

#define INITIALIZE_LOCK(name) \
    pas_system_mutex_construct(&filc_## name ## _lock)
    FILC_FOR_EACH_LOCK(INITIALIZE_LOCK);
#undef INITIALIZE_LOCK

    pas_system_condition_construct(&filc_stop_the_world_cond);

    fugc_initialize_heaps();

    filc_object_array_construct(&filc_global_variable_roots);

    filc_thread* thread = filc_thread_create_with_manual_tracking();
    thread->has_started = true;
    thread->has_stopped = false;
    thread->thread = pthread_self();
    thread->tid = gettid();
    thread->has_set_tid = true;
    thread->tlc_node = verse_heap_get_thread_local_cache_node();
    thread->tlc_node_version = pas_thread_local_cache_node_version(thread->tlc_node);
    PAS_ASSERT(!pthread_key_create(&filc_thread_key, NULL));
    PAS_ASSERT(!pthread_setspecific(filc_thread_key, thread));
    set_stack_limit(thread);

    /* This has to happen *after* we do our primordial allocations. */
    fugc_initialize_collector();

    exit_on_panic = filc_get_bool_env("FILC_EXIT_ON_PANIC", false);
    dump_errnos = filc_get_bool_env("FILC_DUMP_ERRNOS", false);
    run_global_ctors = filc_get_bool_env("FILC_RUN_GLOBAL_CTORS", true);
    run_global_dtors = filc_get_bool_env("FILC_RUN_GLOBAL_DTORS", true);
    verbose_stop_the_world = filc_get_bool_env("FILC_VERBOSE_STW", false);
    
    if (filc_get_bool_env("FILC_DUMP_SETUP", false)) {
        pas_log("filc setup:\n");
        pas_log("    testing library: %s\n", PAS_ENABLE_TESTING ? "yes" : "no");
        pas_log("    exit on panic: %s\n", exit_on_panic ? "yes" : "no");
        pas_log("    dump errnos: %s\n", dump_errnos ? "yes" : "no");
        pas_log("    run global ctors: %s\n", run_global_ctors ? "yes" : "no");
        pas_log("    run global dtors: %s\n", run_global_dtors ? "yes" : "no");
        pas_log("    verbose stop the world: %s\n", verbose_stop_the_world ? "yes" : "no");
        fugc_dump_setup();
    }
    
    is_initialized = true;
}

PAS_NEVER_INLINE void* filc_thread_allocate_slow(size_t size)
{
    return verse_heap_allocate(fugc_default_heap, size);
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
            if ((my_thread->state & FILC_THREAD_STATE_STOP_REQUESTED)
                && verbose_stop_the_world) {
                pas_log("[%d] thread %u (%p) stopping\n", pas_getpid(), my_thread->tid, my_thread);
                filc_thread_dump_stack(my_thread, pas_log_stream);
            }
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

    FILC_DEFINE_FRAME("call_signal_handler");
    filc_push_frame(my_thread, frame);

    filc_native_frame native_frame;
    filc_push_native_frame(my_thread, &native_frame);

    /* Load the function from the handler first since as soon as we exit, the handler might get GC'd.
       Also, we're choosing not to rely on the fact that functions are global and we track them anyway. */
    filc_ptr function_ptr = filc_flight_ptr_load(my_thread, &handler->function_ptr);

    /* This check shouldn't be necessary; we do it out of an abundance of paranoia! */
    filc_check_function_call(function_ptr);
    filc_call_user_void_int(
        my_thread, (pizlonated_function)filc_ptr_ptr(function_ptr), signum);

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

void filc_enter_with_allocation_root(filc_thread* my_thread, void* allocation_root)
{
    filc_enter(my_thread);
    filc_pop_allocation_root(my_thread, allocation_root);
}

void filc_exit_with_allocation_root(filc_thread* my_thread, void* allocation_root)
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
        if (array->capacity)
            PAS_ASSERT(!pas_mul_uint32_overflow(array->capacity, 2, &new_capacity));
        else
            new_capacity = 2;

        new_array = bmalloc_allocate(sizeof(void*) * new_capacity);
        memcpy(new_array, array->array, sizeof(void*) * array->size);

        bmalloc_deallocate(array->array);
        array->array = new_array;
        array->capacity = new_capacity;
        PAS_ASSERT(array->size < array->capacity);
    }

    array->array[array->size++] = ptr;
}

PAS_NEVER_INLINE static void enlarge_array(filc_object_array* array, size_t anticipated_size)
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
    if (PAS_UNLIKELY(anticipated_size > array->objects_capacity))
        enlarge_array(array, anticipated_size);
}

void filc_object_array_push(filc_object_array* array, filc_object* object)
{
    enlarge_array_if_necessary(array, array->num_objects + 1);
    PAS_TESTING_ASSERT(array->num_objects < array->objects_capacity);
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

static PAS_NEVER_INLINE void native_frame_enlarge(filc_native_frame* frame)
{
    uintptr_t* new_array;
    unsigned new_capacity;

    PAS_TESTING_ASSERT(frame->size == frame->capacity);

    new_capacity = frame->capacity << 1;
    PAS_ASSERT(new_capacity > frame->capacity);

    new_array = bmalloc_allocate(sizeof(uintptr_t) * new_capacity);
    memcpy(new_array, frame->array, sizeof(uintptr_t) * frame->size);

    if (frame->array != frame->inline_array)
        bmalloc_deallocate(frame->array);

    frame->array = new_array;
    frame->capacity = new_capacity;

    PAS_TESTING_ASSERT(frame->size < frame->capacity);
}

static PAS_ALWAYS_INLINE void native_frame_add_impl(filc_native_frame* frame, uintptr_t encoded_ptr)
{
    PAS_TESTING_ASSERT(
        (encoded_ptr & FILC_NATIVE_FRAME_PTR_MASK) == FILC_NATIVE_FRAME_TRACKED_PTR ||
        (encoded_ptr & FILC_NATIVE_FRAME_PTR_MASK) == FILC_NATIVE_FRAME_BMALLOC_PTR);

    if (frame->size >= frame->capacity)
        native_frame_enlarge(frame);

    frame->array[frame->size++] = encoded_ptr;
}

void filc_native_frame_add(filc_native_frame* frame, filc_object* object)
{
    PAS_TESTING_ASSERT(!frame->locked);
    
    if (!object)
        return;

    native_frame_add_impl(frame, (uintptr_t)object | FILC_NATIVE_FRAME_TRACKED_PTR);
}

void filc_native_frame_defer_bmalloc_deallocate(filc_native_frame* frame, void* bmalloc_object)
{
    PAS_TESTING_ASSERT(!frame->locked);

    if (!bmalloc_object)
        return;

    native_frame_add_impl(frame, (uintptr_t)bmalloc_object | FILC_NATIVE_FRAME_BMALLOC_PTR);
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

PAS_NEVER_INLINE void filc_pollcheck_slow(filc_thread* my_thread, const filc_origin* origin)
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

static void stop_thread_allocators(filc_thread* my_thread)
{
    unsigned index;
    for (index = FILC_THREAD_NUM_ALLOCATORS; index--;)
        verse_local_allocator_stop(filc_thread_allocator(my_thread, index));
}

void filc_thread_stop_allocators(filc_thread* my_thread)
{
    assert_participates_in_pollchecks(my_thread);

    pas_thread_local_cache_node* node = my_thread->tlc_node;
    uint64_t version = my_thread->tlc_node_version;
    if (node && version)
        verse_heap_thread_local_cache_node_stop_local_allocators(node, version);

    stop_thread_allocators(my_thread);
}

void filc_thread_mark_roots(filc_thread* my_thread)
{
    static const bool verbose = false;
    
    assert_participates_in_pollchecks(my_thread);

    size_t index;
    for (index = my_thread->allocation_roots.size; index--;) {
        void* allocation_root = my_thread->allocation_roots.array[index];
        /* Allocation roots have to have the mark bit set without being put on any mark stack, since
           they have no outgoing references and they are not ready for scanning. */
        verse_heap_set_is_marked_relaxed(allocation_root, true);
    }

    filc_frame* frame;
    for (frame = my_thread->top_frame; frame; frame = frame->parent) {
        PAS_ASSERT(frame->origin);
        const filc_function_origin* function_origin = filc_origin_get_function_origin(frame->origin);
        PAS_ASSERT(function_origin);
        if (verbose) {
            pas_log("Marking roots for ");
            filc_origin_dump_all_inline(frame->origin, "; ", pas_log_stream);
            pas_log("\n");
        }
        PAS_ASSERT(function_origin->base.num_lowers_ish < UINT_MAX);
        for (index = function_origin->base.num_lowers_ish; index--;) {
            if (verbose)
                pas_log("Marking thread root %p\n", frame->lowers[index]);
            fugc_mark(&my_thread->mark_stack, filc_object_for_lower(frame->lowers[index]));
        }
    }

    filc_native_frame* native_frame;
    for (native_frame = my_thread->top_native_frame; native_frame; native_frame = native_frame->parent) {
        for (index = native_frame->size; index--;) {
            uintptr_t encoded_ptr = native_frame->array[index];
            if ((encoded_ptr & FILC_NATIVE_FRAME_PTR_MASK) == FILC_NATIVE_FRAME_TRACKED_PTR) {
                fugc_mark(
                    &my_thread->mark_stack,
                    (filc_object*)(encoded_ptr & ~FILC_NATIVE_FRAME_PTR_MASK));
            } else {
                PAS_TESTING_ASSERT(
                    (encoded_ptr & FILC_NATIVE_FRAME_PTR_MASK) == FILC_NATIVE_FRAME_BMALLOC_PTR);
            }
        }
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
            filc_object_dump(my_thread->mark_stack.objects[index], pas_log_stream);
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
}

static void dump_signals_mask(void)
{
    pas_log("Signals mask:");
    sigset_t oldset;
    pthread_sigmask(0, NULL, &oldset);
    for (int sig = 1; sig < 32; ++sig) {
        if (sigismember(&oldset, sig))
            pas_log(" %d", sig);
    }
    pas_log("\n");
}

static void signal_pizlonator(int signum)
{
    static const bool verbose = false;
    
    PAS_ASSERT((unsigned)signum <= FILC_MAX_USER_SIGNUM);
    filc_thread* thread = filc_get_my_thread();

    /* We're running on a thread that shouldn't be receiving signals or we're running in a thread
       that hasn't fully started.
       
       This shouldn't happen, because:
       
       - Service threads have all of our signals blocked from the start.
       
       - Newly created threads have signals blocked until they set the thread. */
    if (!thread) {
        pas_log("Received a user signal on non-user thread!\n");
        pas_log("Signum: %d\n", signum);
        dump_signals_mask();
    }
    PAS_ASSERT(thread);
    
    if ((thread->state & FILC_THREAD_STATE_ENTERED)) {
        /* For all we know the user asked for a mask that allows us to recurse, hence the lock-freedom. */
        for (;;) {
            uint64_t old_value = thread->num_deferred_signals[signum];
            if (pas_compare_and_swap_uint64_weak(
                    thread->num_deferred_signals + signum, old_value, old_value + 1))
                break;
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
    
    call_signal_handler(thread, signal_table[signum], signum);

    filc_exit(thread);
}

const filc_function_origin* filc_origin_get_function_origin(const filc_origin* origin)
{
    while (filc_origin_node_is_inline_frame(origin->origin_node))
        origin = &filc_origin_node_as_inline_frame(origin->origin_node)->origin;
    return filc_origin_node_as_function_origin(origin->origin_node);
}

const filc_origin* filc_origin_next_inline(const filc_origin* origin)
{
    if (filc_origin_node_is_inline_frame(origin->origin_node))
        return &filc_origin_node_as_inline_frame(origin->origin_node)->origin;
    return NULL;
}

void filc_origin_dump_self(const filc_origin* origin, pas_stream* stream)
{
    if (origin) {
        PAS_ASSERT(origin->origin_node);
        if (origin->origin_node->filename)
            pas_stream_printf(stream, "%s", origin->origin_node->filename);
        else
            pas_stream_printf(stream, "<somewhere>");
        if (origin->line) {
            pas_stream_printf(stream, ":%u", origin->line);
            if (origin->column)
                pas_stream_printf(stream, ":%u", origin->column);
        }
        if (origin->origin_node->function)
            pas_stream_printf(stream, ": %s", origin->origin_node->function);
    } else {
        /* FIXME: Maybe just assert that this doesn't happen? */
        pas_stream_printf(stream, "<null origin>");
    }
}

void filc_origin_dump_all_inline(const filc_origin* origin, const char* separator, pas_stream* stream)
{
    bool comma = false;
    for (; origin; origin = filc_origin_next_inline(origin)) {
        pas_stream_print_comma(stream, &comma, separator);
        filc_origin_dump_self(origin, stream);
    }
}

void filc_special_type_dump(filc_special_type type, pas_stream* stream)
{
    switch (type) {
    case FILC_SPECIAL_TYPE_NONE:
        pas_stream_printf(stream, "none");
        return;
    case FILC_SPECIAL_TYPE_THREAD:
        pas_stream_printf(stream, "thread");
        return;
    case FILC_SPECIAL_TYPE_PTR_TABLE:
        pas_stream_printf(stream, "ptr_table");
        return;
    case FILC_SPECIAL_TYPE_EXACT_PTR_TABLE:
        pas_stream_printf(stream, "exact_ptr_table");
        return;
    case FILC_SPECIAL_TYPE_FUNCTION:
        pas_stream_printf(stream, "function");
        return;
    case FILC_SPECIAL_TYPE_SIGNAL_HANDLER:
        pas_stream_printf(stream, "signal_handler");
        return;
    case FILC_SPECIAL_TYPE_PTR_TABLE_ARRAY:
        pas_stream_printf(stream, "ptr_table_array");
        return;
    case FILC_SPECIAL_TYPE_DL_HANDLE:
        pas_stream_printf(stream, "dl_handle");
        return;
    case FILC_SPECIAL_TYPE_JMP_BUF:
        pas_stream_printf(stream, "jmp_buf");
        return;
    default:
        pas_stream_printf(stream, "?%u", (unsigned)type);
        return;
    }
}

char* filc_special_type_to_new_string(filc_special_type type)
{
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);
    pas_string_stream stream;
    pas_string_stream_construct(&stream, &allocation_config);
    filc_special_type_dump(type, &stream.base);
    return pas_string_stream_take_string(&stream);
}

void filc_object_flags_dump_with_comma(filc_object_flags flags, bool* comma, pas_stream* stream)
{
    if (flags & FILC_OBJECT_FLAG_FREE) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "free");
    }
    if (filc_object_flags_is_special(flags)) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "special(");
        filc_special_type_dump(filc_object_flags_special_type(flags), stream);
        pas_stream_printf(stream, ")");
    }
    if (flags & FILC_OBJECT_FLAG_GLOBAL) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "global");
    }
    if (flags & FILC_OBJECT_FLAG_GLOBAL_AUX) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "global_aux");
    }
    if (flags & FILC_OBJECT_FLAG_MMAP) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "mmap");
    }
    if (flags & FILC_OBJECT_FLAG_READONLY) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "readonly");
    }
    if (flags & FILC_OBJECT_FLAG_SYSV_SHM) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "sysv_shm");
    }
    if (filc_object_flags_is_aligned(flags)) {
        pas_stream_print_comma(stream, comma, ",");
        pas_stream_printf(stream, "aligned(%zu)", filc_object_flags_alignment(flags));
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

void filc_object_dump(filc_object* object, pas_stream* stream)
{
    static const bool verbose = false;
    
    if (!object) {
        pas_stream_printf(stream, "<null>");
        return;
    }
    if (filc_object_is_special(object))
        pas_stream_printf(stream, "aux=%p", filc_object_aux_ptr(object));
    else {
        pas_stream_printf(stream, "%p,%p", filc_object_lower(object), filc_object_upper(object));
        if (filc_object_aux_ptr(object))
            pas_stream_printf(stream, ",aux=%p", filc_object_aux_ptr(object));
    }
    bool comma = true;
    filc_object_flags_dump_with_comma(filc_object_get_flags(object), &comma, stream);
}

void filc_ptr_dump(filc_ptr ptr, pas_stream* stream)
{
    pas_stream_printf(stream, "%p,", filc_ptr_ptr(ptr));
    filc_object_dump(filc_ptr_object(ptr), stream);
}

void filc_ptr_contents_dump(filc_ptr ptr, pas_stream* stream)
{
    filc_ptr_dump(ptr, stream);
    pas_stream_printf(stream, ":[");
    bool comma = false;
    filc_ptr cursor;
    for (cursor = filc_ptr_with_ptr(ptr, filc_ptr_lower(ptr));
         filc_ptr_ptr(cursor) < filc_ptr_upper(cursor);
         cursor = filc_ptr_with_offset(cursor, FILC_WORD_SIZE)) {
        pas_stream_print_comma(stream, &comma, " ");
        pas_stream_printf(stream, "+%zu:", (size_t)filc_ptr_offset(cursor));
        filc_ptr value = filc_load_ptr_with_manual_tracking(cursor, 0);
        if (filc_ptr_is_boxed_int(value)) {
            pas_stream_printf(stream, "%p", filc_ptr_ptr(value));
            continue;
        }
        filc_ptr_dump(value, stream);
    }
    pas_stream_printf(stream, "]");
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

char* filc_ptr_contents_to_new_string(filc_ptr ptr)
{
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);
    pas_string_stream stream;
    pas_string_stream_construct(&stream, &allocation_config);
    filc_ptr_contents_dump(ptr, &stream.base);
    return pas_string_stream_take_string(&stream);
}

void filc_store_ptr_atomic_with_ptr_pair_outline(filc_thread* my_thread,
                                                 void** ptr_ptr,
                                                 filc_lower_or_box* lower_or_box_ptr,
                                                 filc_ptr value)
{
    return filc_store_ptr_atomic_with_ptr_pair(my_thread, ptr_ptr, lower_or_box_ptr, value);
}

bool filc_weak_cas_ptr(filc_thread* my_thread, filc_ptr ptr, ptrdiff_t offset,
                       filc_ptr expected, filc_ptr new_value)
{
    ptr = filc_ptr_with_offset(ptr, offset);
    filc_check_native_access(ptr, sizeof(void*), filc_write_access);
    filc_lower_or_box* lower_or_box_ptr = filc_ptr_ensure_lower_or_box_ptr(my_thread, ptr);
    filc_store_barrier(my_thread, filc_ptr_object(new_value));
    for (;;) {
        filc_lower_or_box lower_or_box = filc_lower_or_box_load(lower_or_box_ptr);
        bool did_succeed;
        if (filc_lower_or_box_is_box(lower_or_box)) {
            did_succeed = filc_atomic_box_cas_weak_without_barrier(
                filc_lower_or_box_get_box(lower_or_box), expected, new_value);
        } else if (*(void**)filc_ptr_ptr(ptr) != filc_ptr_ptr(expected))
            return false;
        else {
            if (!filc_lower_or_box_cas_weak_unbarriered(
                    lower_or_box_ptr, lower_or_box,
                    filc_lower_or_box_create_box(
                        filc_atomic_box_create_for_ptr_store(my_thread, new_value))))
                continue;
            did_succeed = true;
        }
        if (did_succeed)
            *(void**)filc_ptr_ptr(ptr) = filc_ptr_ptr(new_value);
        return did_succeed;
    }
}

filc_ptr filc_strong_cas_ptr_with_manual_tracking(
    filc_thread* my_thread, filc_ptr ptr, ptrdiff_t offset, filc_ptr expected, filc_ptr new_value)
{
    ptr = filc_ptr_with_offset(ptr, offset);
    filc_check_native_access(ptr, sizeof(void*), filc_write_access);
    filc_lower_or_box* lower_or_box_ptr = filc_ptr_ensure_lower_or_box_ptr(my_thread, ptr);
    filc_store_barrier(my_thread, filc_ptr_object(new_value));
    for (;;) {
        filc_lower_or_box lower_or_box = filc_lower_or_box_load(lower_or_box_ptr);
        filc_ptr old_value;
        if (filc_lower_or_box_is_box(lower_or_box)) {
            old_value = filc_atomic_box_cas_strong_without_barrier(
                filc_lower_or_box_get_box(lower_or_box), expected, new_value);
        } else {
            void* old_ptr = *(void**)filc_ptr_ptr(ptr);
            if (old_ptr != filc_ptr_ptr(expected)) {
                old_value = filc_ptr_create_with_lower_and_ptr_and_manual_tracking(
                    filc_lower_or_box_get_lower(lower_or_box), old_ptr);
            } else {
                if (!filc_lower_or_box_cas_weak_unbarriered(
                        lower_or_box_ptr, lower_or_box,
                        filc_lower_or_box_create_box(
                            filc_atomic_box_create_for_ptr_store(my_thread, new_value))))
                    continue;
                old_value = filc_ptr_create_with_lower_and_ptr_and_manual_tracking(
                    filc_lower_or_box_get_lower(lower_or_box), filc_ptr_ptr(expected));
            }
        }
        if (filc_ptr_ptr(old_value) == filc_ptr_ptr(expected))
            *(void**)filc_ptr_ptr(ptr) = filc_ptr_ptr(new_value);
        return old_value;
    }
}

filc_ptr filc_xchg_ptr_with_manual_tracking(
    filc_thread* my_thread, filc_ptr ptr, ptrdiff_t offset, filc_ptr new_value)
{
    for (;;) {
        filc_ptr result = filc_load_ptr_with_manual_tracking(ptr, offset);
        if (filc_weak_cas_ptr(my_thread, ptr, offset, result, new_value))
            return result;
    }
}

void filc_thread_ensure_cc_outline_buffer_slow(filc_thread* my_thread, size_t size)
{
    PAS_ASSERT(size > my_thread->cc_outline_size);
    PAS_ASSERT(pas_is_aligned(size, FILC_WORD_SIZE));
    size = pas_round_up_to_power_of_2(size, FILC_CC_ALIGNMENT);
    bmalloc_deallocate(my_thread->cc_outline_buffer);
    bmalloc_deallocate(my_thread->cc_outline_aux_buffer);
    my_thread->cc_outline_buffer = bmalloc_allocate_with_alignment(size, FILC_CC_ALIGNMENT);
    my_thread->cc_outline_aux_buffer = bmalloc_allocate_with_alignment(size, FILC_CC_ALIGNMENT);
    pas_zero_memory(my_thread->cc_outline_buffer, size);
    pas_zero_memory(my_thread->cc_outline_aux_buffer, size);
    my_thread->cc_outline_size = size;
}

void filc_cc_cursor_dump(filc_cc_cursor cursor, pas_stream* stream)
{
    pas_stream_printf(stream, "%zu,%zu", cursor.offset, cursor.size);
}

char* filc_cc_cursor_to_new_string(filc_cc_cursor cursor)
{
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);
    pas_string_stream stream;
    pas_string_stream_construct(&stream, &allocation_config);
    filc_cc_cursor_dump(cursor, &stream.base);
    return pas_string_stream_take_string(&stream);
}

PAS_NEVER_INLINE void filc_store_barrier_slow(filc_thread* my_thread, filc_object* object)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    fugc_mark(&my_thread->mark_stack, object);
}

PAS_NEVER_INLINE void filc_store_barrier_for_lower_slow(filc_thread* my_thread, void* lower)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    fugc_mark(&my_thread->mark_stack, filc_object_for_lower_not_null(lower));
}

PAS_NO_RETURN PAS_NEVER_INLINE void filc_check_native_access_fail(filc_ptr ptr,
                                                                  size_t size_and_alignment,
                                                                  filc_access_kind kind)
{
    PAS_ASSERT(pas_is_power_of_2(size_and_alignment));
    PAS_ASSERT(size_and_alignment <= FILC_WORD_SIZE);
    FILC_CHECK(
        filc_ptr_lower(ptr),
        NULL,
        "cannot %s pointer with null object (ptr = %s).",
        filc_access_kind_get_string(kind), filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        pas_is_aligned((uintptr_t)filc_ptr_ptr(ptr), size_and_alignment),
        NULL,
        "alignment requirement of %zu bytes not met; in this case ptr %% %zu = %zu (ptr = %s).",
        size_and_alignment, size_and_alignment, (size_t)filc_ptr_ptr(ptr) % size_and_alignment,
        filc_ptr_to_new_string(ptr));
    if (kind == filc_write_access) {
        FILC_CHECK(
            !(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_READONLY),
            NULL,
            "cannot write to read-only object (ptr = %s).",
            filc_ptr_to_new_string(ptr));
    }
    /* At this point we know that we have to be out of bounds. */
    FILC_CHECK(
        filc_ptr_ptr(ptr) >= filc_ptr_lower(ptr),
        NULL,
        "cannot %s pointer with ptr < lower (ptr = %s).", 
        filc_access_kind_get_string(kind), filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        filc_ptr_ptr(ptr) < filc_ptr_upper(ptr),
        NULL,
        "cannot %s pointer with ptr >= upper (ptr = %s).",
        filc_access_kind_get_string(kind), filc_ptr_to_new_string(ptr));

    PAS_UNREACHABLE();
}

/* Just checks bounds, but assumes that you do a type check or at least an accessibility (not free
   or special) check. */
#define CHECK_BOUNDS_FAST(raw_ptr, lower, upper, count, fail) do { \
        char* my_raw_ptr = (raw_ptr); \
        char* my_upper = (upper); \
        if (my_raw_ptr < (lower) || \
            my_raw_ptr >= my_upper || \
            (count) > (size_t)(my_upper - my_raw_ptr)) \
            fail; \
    } while (false)

#define CHECK_ACCESSIBLE_FAST(object, fail) do { \
        if ((filc_object_get_flags(object) & (FILC_OBJECT_FLAG_FREE | \
                                              FILC_OBJECT_FLAGS_SPECIAL_MASK))) \
            fail; \
    } while (false)

/* Also checks that the object isn't free or special, just because it's cheap to do such a check. */
#define CHECK_WRITE_FAST(object, fail) do { \
        if ((filc_object_get_flags(object) & (FILC_OBJECT_FLAG_READONLY | \
                                              FILC_OBJECT_FLAG_FREE | \
                                              FILC_OBJECT_FLAGS_SPECIAL_MASK))) \
            fail; \
    } while (false)

void filc_check_access_special(filc_ptr ptr, filc_special_type special_type)
{
    PAS_TESTING_ASSERT(filc_special_type_is_valid(special_type));
    
    if (PAS_ENABLE_TESTING)
        filc_validate_ptr(ptr, NULL);

    /* FIXME: Optimize this so the fact path is inline and doesn't have the FILC_CHECK logic. */

    FILC_CHECK(
        filc_ptr_object(ptr),
        NULL,
        "cannot access pointer with null object (ptr = %s).",
        filc_ptr_to_new_string(ptr));

    /* This check is only to make the error reporting better, it's totally subsumed by the next
       check. */
    FILC_CHECK(
        filc_object_is_special(filc_ptr_object(ptr)),
        NULL,
        "cannot access pointer as %s, object isn't even special (pts = %s).",
        filc_special_type_to_new_string(special_type), filc_ptr_to_new_string(ptr));
    
    FILC_CHECK(
        filc_object_special_type(filc_ptr_object(ptr)) == special_type,
        NULL,
        "cannot access pointer as %s, object has wrong special type (ptr = %s).",
        filc_special_type_to_new_string(special_type), filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        filc_ptr_ptr(ptr) == filc_ptr_aux_ptr(ptr),
        NULL,
        "cannot access pointer as %s with ptr != aux (ptr = %s).", 
        filc_special_type_to_new_string(special_type), filc_ptr_to_new_string(ptr));
}

PAS_NO_RETURN void filc_cc_args_check_failure(size_t actual_size, size_t expected_size,
                                              const filc_origin* origin)
{
    filc_safety_panic(origin, "argument size mismatch (actual = %zu, expected = %zu).",
                      actual_size, expected_size);
}

PAS_NO_RETURN void filc_cc_rets_check_failure(size_t actual_size, size_t expected_size,
                                              const filc_origin* origin)
{
    filc_safety_panic(origin, "return size mismatch (actual = %zu, expected = %zu).",
                      actual_size, expected_size);
}

PAS_API PAS_NO_RETURN void filc_stack_overflow_failure_impl(void)
{
    filc_safety_panic(NULL, "stack overflow.");
}

static void check_not_free(filc_ptr ptr)
{
    FILC_CHECK(
        !(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_FREE),
        NULL,
        "cannot access pointer to free object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
}

static void check_object_accessible(filc_object* object)
{
    FILC_CHECK(
        !(filc_object_get_flags(object) & (FILC_OBJECT_FLAG_FREE | FILC_OBJECT_FLAGS_SPECIAL_MASK)),
        NULL,
        "cannot access pointer to free or special object (object = %s).",
        filc_object_to_new_string(object));
}

static void check_accessible(filc_ptr ptr)
{
    FILC_CHECK(
        !(filc_object_get_flags(filc_ptr_object(ptr)) & (FILC_OBJECT_FLAG_FREE |
                                                         FILC_OBJECT_FLAGS_SPECIAL_MASK)),
        NULL,
        "cannot access pointer to free or special object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
}

static PAS_ALWAYS_INLINE filc_ptr get_next_bytes_for_va_arg_impl(
    filc_ptr ptr_ptr, size_t size, size_t alignment)
{
    filc_ptr ptr_value;
    uintptr_t ptr_as_int;

    /* In a perfect world, we'd track this pointer because logically we're doing a ptr store later on,
       and the ptr store may exit.
       
       But we expect this code to be a fast path, so we skip tracking and do something a bit shady.
       
       We load without tracking, and then avoid exiting by storing just the raw ptr back rather than
       storing the whole pointer. This totally avoids the exit. It's correct because if va_list is
       being used correctly then the capability will still be the same. At worst, if the user stores a
       different capability, then this will result in the user getting a trap.
       
       Some alternatives that I could have done:
       
       - I could push a native frame and do the tracking. Maybe it's not even a slowdown to do that.
       
       - I could have a special place in filc_thread to stash capabilities that are need to be tracked
         cheaply.
       
       - I could have filc_store_ptr track the value part of the pointer on the slow path that
         allocates the aux. This would probably be even more shady, in practive, than what I'm
         doing. */
    ptr_value = filc_load_ptr_with_manual_tracking(ptr_ptr, 0);
    ptr_as_int = (uintptr_t)filc_ptr_ptr(ptr_value);
    ptr_as_int = pas_round_up_to_power_of_2(ptr_as_int, alignment);

    filc_ptr result = filc_ptr_with_ptr(ptr_value, (void*)ptr_as_int);
    filc_check_read(result, size); /* Doesn't have to be a check_aligned_read because we already
                                      forced alignment. */

    filc_check_native_access(ptr_ptr, sizeof(void*), filc_write_access);
    *(void**)filc_ptr_ptr(ptr_ptr) = (char*)ptr_as_int + size;

    return result;
}

void* filc_get_next_bytes_for_va_arg(filc_ptr ptr_ptr, size_t size, size_t alignment)
{
    return filc_ptr_ptr(get_next_bytes_for_va_arg_impl(ptr_ptr, size, alignment));
}

filc_ptr_pair filc_get_next_ptr_bytes_for_va_arg(filc_ptr ptr_ptr, size_t size, size_t alignment)
{
    filc_ptr result_ptr = get_next_bytes_for_va_arg_impl(ptr_ptr, size, alignment);

    filc_ptr_pair result;
    result.raw_ptr = filc_ptr_ptr(result_ptr);

    char* aux_ptr = filc_ptr_aux_ptr(result_ptr);
    if (!aux_ptr)
        result.lower_or_box_ptr = NULL;
    else
        result.lower_or_box_ptr = (filc_lower_or_box*)(aux_ptr + filc_ptr_offset(result_ptr));
    
    return result;
}

char* filc_object_ensure_aux_ptr_slow(filc_thread* my_thread, filc_object* object)
{
    static const bool verbose = false;
    static const bool not_atomic_hack = false;
    if (verbose) {
        pas_log("ensuring aux for ");
        filc_object_dump(object, pas_log_stream);
        pas_log("\n");
    }
    PAS_TESTING_ASSERT(object);
    PAS_TESTING_ASSERT(object != &filc_free_singleton);
    size_t size = filc_object_size_not_null(object);
    /* When freeing an object, there's a window of time when we might see the size being zero, but
       the free bit isn't set yet. There's no universe where we'd want to ensure aux for an object
       whose size was legitimately zero. Therefore, we do this check to guard against the race where
       we would succeed at CASing the aux into the object but allocate a zero-sized aux, resulting in
       access failures on this newly created aux (since in that case the rest of the access would have
       presumably already checked that it's in bounds of whatever the size was before it was
       freed). */
    FILC_CHECK(
        size,
        NULL,
        "attempt to create aux for object with zero size %s.\n",
        filc_object_to_new_string(object));
    char* aux_ptr = filc_thread_allocate(my_thread, size);
    if (verbose)
        pas_log("allocated aux at %p with size %zu, ending at %p\n", aux_ptr, size, aux_ptr + size);
    if (PAS_LIKELY(size <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS))
        filc_memset_small_word(aux_ptr, 0, size);
    else {
        /* It's wild that we have to do this, but it's really necessary, and it means that anytime
           we do a store, it's a safepoint.
        
           Fortunately, any object referenced from roots is guaranteed to be kept alive along with its
           aux. even if this exit happens, anything the compiler had been able to prove about objects
           referenced from locals will continue to be true.
           
           The only thing that the compiler can't reason about at all are barriers, but that was
           already true due to the fantastic properties of Phil's concurrent marking (even if you just
           allocated an object you still need a barrier). */
        filc_exit_with_allocation_root(my_thread, aux_ptr);
        memset(aux_ptr, 0, size);
        filc_enter_with_allocation_root(my_thread, aux_ptr);
    }
    if (PAS_UNLIKELY(filc_is_marking))
        verse_heap_set_is_marked_relaxed(aux_ptr, true);
    for (;;) {
        uintptr_t aux = object->aux;
        PAS_ASSERT(!(filc_aux_get_flags(aux) & FILC_OBJECT_FLAGS_SPECIAL_MASK));
        if (filc_aux_get_ptr(aux)) {
            if (verbose) {
                pas_log("object already has aux at %p: ", (void*)aux);
                filc_object_dump(object, pas_log_stream);
                pas_log("\n");
            }
            return filc_aux_get_ptr(aux);
        }
        FILC_CHECK(
            !(filc_aux_get_flags(aux) & FILC_OBJECT_FLAG_FREE),
            NULL,
            "attempt to create aux for free object %s.\n",
            filc_object_to_new_string(object));
        if (not_atomic_hack) {
            object->aux = filc_aux_create(filc_aux_get_flags(aux), aux_ptr);
            return aux_ptr;
        }
        if (pas_compare_and_swap_uintptr_weak(
                &object->aux, aux, filc_aux_create(filc_aux_get_flags(aux), aux_ptr))) {
            if (verbose) {
                pas_log("created aux at %p: ", aux_ptr);
                filc_object_dump(object, pas_log_stream);
                pas_log("\n");
            }
            return aux_ptr;
        }
    }
}

char* filc_object_ensure_aux_ptr_outline(filc_thread* my_thread, filc_object* object)
{
    return filc_object_ensure_aux_ptr(my_thread, object);
}

filc_atomic_box* filc_atomic_box_create_for_ptr_store(filc_thread* my_thread, filc_ptr value)
{
    filc_atomic_box* result = filc_thread_allocate(my_thread, sizeof(filc_atomic_box));
    /* We know that the ptr value is already barriered. */
    result->ptr = value;
    if (PAS_UNLIKELY(filc_is_marking))
        verse_heap_set_is_marked_relaxed(result, true);
    pas_store_store_fence();
    return result;
}

filc_object* filc_allocate_special_early(size_t size, size_t alignment,
                                         filc_special_type special_type)
{
    /* NOTE: This cannot assert anything about the Fil-C thread because we do use this before any
       threads have been created. */

    /* NOTE: This must not exit, because we might hold rando locks while calling into this. */

    PAS_ASSERT(filc_special_type_is_valid(special_type));

    pas_heap* heap;
    if (filc_special_type_has_destructor(special_type))
        heap = fugc_destructor_heap;
    else
        heap = fugc_default_heap;

    size_t total_size;
    size_t base_size;
    PAS_ASSERT(FILC_MINALIGN == sizeof(filc_object));
    if (alignment > FILC_MINALIGN)
        base_size = alignment;
    else
        base_size = sizeof(filc_object);
    PAS_ASSERT(!pas_add_uintptr_overflow(base_size, size, &total_size));

    void* allocation = verse_heap_allocate_with_alignment(heap, total_size, alignment);
    filc_log_align log_align;
    filc_object* result;
    if (alignment > FILC_MINALIGN) {
        filc_alignment_header_construct((filc_alignment_header*)allocation, alignment);
        result = (filc_object*)((char*)allocation + alignment) - 1;
        log_align = pas_log2(alignment);
    } else {
        result = (filc_object*)allocation;
        log_align = 0;
    }
    filc_object_flags object_flags = filc_object_flags_create(0, special_type, log_align);
    result->upper = filc_object_lower_not_null(result);
    result->aux = filc_aux_create(object_flags, filc_object_lower(result));
    pas_zero_memory(filc_object_lower(result), size);
    pas_store_store_fence();

    return result;
}

filc_object* filc_allocate_special(filc_thread* my_thread, size_t size, size_t alignment,
                                   filc_special_type special_type)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    return filc_allocate_special_early(size, alignment, special_type);
}

static PAS_NEVER_INLINE PAS_NO_RETURN void size_too_big(size_t size)
{
    filc_safety_panic(NULL, "attempt to allocate object that is too big (size = %zu).", size);
}

static PAS_ALWAYS_INLINE void prepare_allocate_object(size_t* size)
{
    if (PAS_UNLIKELY(*size > FILC_MAX_ALLOCATION_SIZE))
        size_too_big(*size);
    size_t original_size = *size;
    *size = pas_round_up_to_power_of_2(*size, FILC_MINALIGN);
    PAS_TESTING_ASSERT(*size >= original_size);
}

filc_object* filc_allocate_special_with_existing_payload(
    filc_thread* my_thread, void* payload, filc_special_type special_type)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    PAS_ASSERT(special_type == FILC_SPECIAL_TYPE_FUNCTION ||
               special_type == FILC_SPECIAL_TYPE_DL_HANDLE);

    filc_object* result = (filc_object*)filc_thread_allocate(my_thread, sizeof(filc_object));
    result->upper = filc_object_lower_not_null(result);
    result->aux = filc_aux_create(
        filc_object_flags_create(0, special_type, 0),
        payload);
    pas_store_store_fence();
    return result;
}

static PAS_ALWAYS_INLINE void prepare_allocate(
    size_t* size, size_t alignment, size_t* offset_to_payload, size_t* total_size)
{
    PAS_ASSERT(sizeof(filc_object) == FILC_MINALIGN);
    prepare_allocate_object(size);
    *offset_to_payload = pas_max_uintptr(alignment, sizeof(filc_object));
    *total_size = *size + *offset_to_payload;
    PAS_TESTING_ASSERT(*total_size > *size);
}

static PAS_ALWAYS_INLINE filc_object* initialize_object_header(
    void* allocation, size_t size, size_t alignment, size_t offset_to_payload,
    filc_object_flags object_flags, char* aux_ptr)
{
    static const bool verbose = false;
    if (verbose) {
        pas_log("initializing new object with allocation at %p, payload at %p, size %zu, end at %p\n",
                allocation, (char*)allocation + offset_to_payload, size,
                (char*)allocation + offset_to_payload + size);
    }
    PAS_TESTING_ASSERT(pas_is_aligned(size, FILC_WORD_SIZE));
    PAS_TESTING_ASSERT(!filc_object_flags_is_special(object_flags));
    PAS_TESTING_ASSERT(!filc_object_flags_is_aligned(object_flags));
    if (alignment > sizeof(filc_object)) {
        PAS_TESTING_ASSERT(offset_to_payload > sizeof(filc_object));
        PAS_TESTING_ASSERT(offset_to_payload == alignment);
        filc_alignment_header_construct((filc_alignment_header*)allocation, alignment);
    }
    filc_object* result = filc_object_for_lower_not_null((char*)allocation + offset_to_payload);
    result->upper = (char*)(result + 1) + size;
    if (alignment > FILC_MINALIGN) {
        object_flags = filc_object_flags_create(
            object_flags, FILC_SPECIAL_TYPE_NONE, pas_log2(alignment));
        PAS_TESTING_ASSERT(filc_object_flags_alignment(object_flags) == alignment);
    }
    result->aux = filc_aux_create(object_flags, aux_ptr);
    return result;
}

static PAS_NEVER_INLINE filc_object* finish_allocate_large(
    filc_thread* my_thread, filc_object* result, size_t size)
{
    filc_exit_with_allocation_root(my_thread, filc_object_mark_base(result));
    
    pas_zero_memory(filc_object_lower(result), size);
    
    pas_store_store_fence();
    
    filc_enter_with_allocation_root(my_thread, filc_object_mark_base(result));
    return result;
}

static PAS_ALWAYS_INLINE filc_object* finish_allocate_small(filc_object* result, size_t size)
{
    filc_memset_small_word(filc_object_lower(result), 0, size);
    pas_store_store_fence();
    return result;
}

static PAS_ALWAYS_INLINE filc_object* finish_allocate(
    filc_thread* my_thread, void* allocation, size_t size, size_t alignment,
    size_t offset_to_payload, filc_object_flags object_flags)
{
    filc_object* result = initialize_object_header(
        allocation, size, alignment, offset_to_payload, object_flags, NULL);
    if (PAS_UNLIKELY(size > FILC_MAX_BYTES_BETWEEN_POLLCHECKS))
        return finish_allocate_large(my_thread, result, size);
    return finish_allocate_small(result, size);
}

static PAS_ALWAYS_INLINE filc_object* allocate_impl(
    filc_thread* my_thread, size_t size, filc_object_flags object_flags)
{
    static const bool verbose = false;
    
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);
    PAS_TESTING_ASSERT(!(object_flags & FILC_OBJECT_FLAG_MMAP));

    if (verbose) {
        pas_log("Allocating %zu bytes\n", size);
        filc_thread_dump_stack(my_thread, pas_log_stream);
    }

    size_t num_words;
    size_t offset_to_payload;
    size_t total_size;
    prepare_allocate(&size, FILC_WORD_SIZE, &offset_to_payload, &total_size);
    return finish_allocate(
        my_thread, filc_thread_allocate(my_thread, total_size),
        size, FILC_WORD_SIZE, offset_to_payload, object_flags);
}

filc_object* filc_allocate(filc_thread* my_thread, size_t size)
{
    return allocate_impl(my_thread, size, 0);
}

static PAS_ALWAYS_INLINE filc_object* allocate_aligned_impl(
    filc_thread* my_thread, size_t size, size_t alignment, filc_object_flags object_flags)
{
    static const bool verbose = false;
    
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    if (verbose) {
        pas_log("Allocating %zu bytes with %zu alignment\n", size, alignment);
        filc_thread_dump_stack(my_thread, pas_log_stream);
    }

    pas_heap* heap;
    if ((object_flags & FILC_OBJECT_FLAG_MMAP)) {
        PAS_TESTING_ASSERT(alignment == pas_page_malloc_alignment());
        heap = fugc_destructor_heap;
    } else
        heap = fugc_default_heap;

    alignment = pas_max_uintptr(alignment, FILC_WORD_SIZE);
    size_t num_words;
    size_t offset_to_payload;
    size_t total_size;
    prepare_allocate(&size, alignment, &offset_to_payload, &total_size);
    return finish_allocate(
        my_thread, verse_heap_allocate_with_alignment(heap, total_size, alignment),
        size, alignment, offset_to_payload, object_flags);
}

filc_object* filc_allocate_with_alignment(filc_thread* my_thread, size_t size, size_t alignment)
{
    return allocate_aligned_impl(my_thread, size, alignment, 0);
}

static filc_object* finish_reallocate(
    filc_thread* my_thread, void* allocation, filc_object* old_object, size_t new_size,
    size_t alignment, size_t offset_to_payload)
{
    static const bool verbose = false;

    if (verbose)
        pas_log("new_size = %zu\n", new_size);
    
    check_object_accessible(old_object);

    size_t old_size = filc_object_size_not_null(old_object);
    char* old_aux_ptr = filc_object_aux_ptr(old_object);

    size_t common_size = pas_min_uintptr(new_size, old_size);
    char* new_aux_ptr = NULL;
    if (old_aux_ptr) {
        new_aux_ptr = filc_thread_allocate(my_thread, new_size);
        if (PAS_UNLIKELY(filc_is_marking)) {
            /* It's posssible that the allocation of the object observed that we're in black
               allocation mode, but the allocation of the aux didn't, because of how TLCs work. So,
               we need to mark the aux if marking is set, just like allocating the aux in
               filc_object_ensure_aux_ptr_slow. */
            verse_heap_set_is_marked_relaxed(new_aux_ptr, true);
        }
    }

    filc_object* result = initialize_object_header(
        allocation, new_size, alignment, offset_to_payload, 0, new_aux_ptr);
    if (verbose)
        pas_log("old_object = %p, result = %p\n", old_object, result);
    if (new_size > FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        if (new_aux_ptr)
            filc_push_allocation_root(my_thread, new_aux_ptr);
        filc_exit_with_allocation_root(my_thread, allocation);
    }
    memcpy(filc_object_lower(result), filc_object_lower(old_object), common_size);
    if (new_size > common_size)
        pas_zero_memory((char*)filc_object_lower(result) + common_size, new_size - common_size);
    if (new_aux_ptr)
        pas_zero_memory(new_aux_ptr, new_size);
    if (new_size > FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        filc_enter_with_allocation_root(my_thread, allocation);
        if (new_aux_ptr)
            filc_pop_allocation_root(my_thread, new_aux_ptr);
        FILC_CHECK(
            !(filc_object_get_flags(old_object) & FILC_OBJECT_FLAG_FREE),
            NULL,
            "source object became free during realloc (object = %s).",
            filc_object_to_new_string(old_object));
    }
    /* FIXME: We could conditionalize this. */
    filc_thread_track_object(my_thread, result);
    /* FIXME: This could be optimized the same way that memmove is. */
    if (new_aux_ptr) {
        size_t offset;
        PAS_ASSERT(pas_is_aligned(common_size, sizeof(filc_lower_or_box)));
        for (offset = 0; offset < common_size; offset += sizeof(filc_lower_or_box)) {
            filc_lower_or_box* old_lower_or_box_ptr = (filc_lower_or_box*)(old_aux_ptr + offset);
            filc_lower_or_box* new_lower_or_box_ptr = (filc_lower_or_box*)(new_aux_ptr + offset);
            filc_lower_or_box lower_or_box = filc_lower_or_box_load_unfenced(old_lower_or_box_ptr);
            void* lower = filc_lower_or_box_extract_lower(lower_or_box);
            filc_store_barrier(my_thread, filc_object_for_lower(lower));
            filc_lower_or_box_store_unfenced_unbarriered(
                new_lower_or_box_ptr, filc_lower_or_box_create_lower(lower));
            if (PAS_UNLIKELY(filc_pollcheck(my_thread, NULL))) {
                FILC_CHECK(
                    !(filc_object_get_flags(old_object) & FILC_OBJECT_FLAG_FREE),
                    NULL,
                    "source object became free during realloc (object = %s).",
                    filc_object_to_new_string(old_object));
            }
        }
    }

    pas_store_store_fence();
    filc_free(old_object);

    return result;
}

filc_object* filc_reallocate(filc_thread* my_thread, filc_object* object, size_t new_size)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    size_t offset_to_payload;
    size_t total_size;
    prepare_allocate(&new_size, FILC_WORD_SIZE, &offset_to_payload, &total_size);
    return finish_reallocate(
        my_thread, filc_thread_allocate(my_thread, total_size),
        object, new_size, FILC_WORD_SIZE, offset_to_payload);
}

filc_object* filc_reallocate_with_alignment(filc_thread* my_thread, filc_object* object,
                                            size_t new_size, size_t alignment)
{
    PAS_TESTING_ASSERT(my_thread == filc_get_my_thread());
    PAS_TESTING_ASSERT(my_thread->state & FILC_THREAD_STATE_ENTERED);

    alignment = pas_max_uintptr(alignment, FILC_WORD_SIZE);
    size_t offset_to_payload;
    size_t total_size;
    prepare_allocate(&new_size, alignment, &offset_to_payload, &total_size);
    return finish_reallocate(
        my_thread, verse_heap_allocate_with_alignment(fugc_default_heap, total_size, alignment),
        object, new_size, alignment, offset_to_payload);
}

void filc_free(filc_object* object)
{
    static const bool verbose = false;
    if (verbose) {
        pas_log("freeing ");
        filc_object_dump(object, pas_log_stream);
        pas_log("\n");
    }
    PAS_TESTING_ASSERT(!filc_object_is_special(object)); /* We could allow freeing special objects
                                                            at some point. */
    PAS_TESTING_ASSERT(!(filc_object_get_flags(object) & FILC_OBJECT_FLAG_GLOBAL));
    object->upper = filc_object_lower_not_null(object);
    for (;;) {
        uintptr_t aux = object->aux;
        FILC_CHECK(
            !(filc_aux_get_flags(aux) & FILC_OBJECT_FLAG_FREE),
            NULL,
            "cannot free already free object %s.",
            filc_object_to_new_string(object));
        /* FIXME: This CAS could be relaxed! */
        if (pas_compare_and_swap_uintptr_weak(
                &object->aux, aux,
                filc_aux_create(filc_aux_get_flags(aux) | FILC_OBJECT_FLAG_FREE,
                                filc_aux_get_ptr(aux))))
            break;
    }
}

static size_t num_ptrtables = 0;

filc_ptr_table* filc_ptr_table_create(filc_thread* my_thread)
{
    filc_ptr_table* result = (filc_ptr_table*)filc_object_special_payload_with_manual_tracking(
        filc_allocate_special(my_thread, sizeof(filc_ptr_table), 1, FILC_SPECIAL_TYPE_PTR_TABLE));

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
                filc_flight_ptr_store(
                    my_thread, new_array->ptrs + index,
                    filc_flight_ptr_load_with_manual_tracking(ptr_table->array->ptrs + index));
            }

            new_array->num_entries = ptr_table->array->num_entries;
            ptr_table->array = new_array;
        }

        PAS_ASSERT(ptr_table->array->num_entries < ptr_table->array->capacity);
        result = ptr_table->array->num_entries++;
    }

    PAS_ASSERT(result < ptr_table->array->num_entries);
    PAS_ASSERT(result < ptr_table->array->capacity);
    filc_flight_ptr_store(my_thread, &add_result.entry->key, ptr);
    add_result.entry->value = result;
    filc_flight_ptr_store(my_thread, ptr_table->array->ptrs + result, ptr);
    return (result + FILC_PTR_TABLE_OFFSET) << FILC_PTR_TABLE_SHIFT;
}

uintptr_t filc_ptr_table_encode(filc_thread* my_thread, filc_ptr_table* ptr_table, filc_ptr ptr)
{
    if (!filc_ptr_ptr(ptr) || !filc_ptr_object(ptr)
        || (filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_FREE))
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

    filc_ptr result = filc_flight_ptr_load_with_manual_tracking(array->ptrs + index);
    if (!filc_ptr_ptr(result))
        return filc_ptr_forge_null();

    PAS_TESTING_ASSERT(filc_ptr_object(result));
    if (filc_object_get_flags(filc_ptr_object(result)) & FILC_OBJECT_FLAG_FREE)
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
        if (filc_object_get_flags(filc_ptr_object(entry.key)) & FILC_OBJECT_FLAG_FREE)
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
        filc_ptr ptr = filc_flight_ptr_load_with_manual_tracking(ptr_table->array->ptrs + index);
        if (!filc_ptr_ptr(ptr))
            continue;
        if (!(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_FREE))
            continue;
        if (ptr_table->num_free_indices >= ptr_table->free_indices_capacity) {
            PAS_ASSERT(ptr_table->num_free_indices == ptr_table->free_indices_capacity);

            size_t new_free_indices_capacity = ptr_table->free_indices_capacity << 1;
            PAS_ASSERT(new_free_indices_capacity > ptr_table->free_indices_capacity);

            uintptr_t* new_free_indices =
                bmalloc_allocate(sizeof(uintptr_t) * new_free_indices_capacity);
            memcpy(new_free_indices, ptr_table->free_indices,
                   sizeof(uintptr_t) * ptr_table->num_free_indices);

            bmalloc_deallocate(ptr_table->free_indices);
            ptr_table->free_indices = new_free_indices;
            ptr_table->free_indices_capacity = new_free_indices_capacity;
        }
        PAS_ASSERT(ptr_table->num_free_indices < ptr_table->free_indices_capacity);
        ptr_table->free_indices[ptr_table->num_free_indices++] = index;
        filc_flight_ptr_store_without_barrier(ptr_table->array->ptrs + index, filc_ptr_forge_null());
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
        filc_object_special_payload_with_manual_tracking(
            filc_allocate_special(my_thread, total_size, 1, FILC_SPECIAL_TYPE_PTR_TABLE_ARRAY));
    result->capacity = capacity;

    return result;
}

void filc_ptr_table_array_mark_outgoing_ptrs(filc_ptr_table_array* array, filc_object_array* stack)
{
    size_t index;
    for (index = array->num_entries; index--;) {
        fugc_mark(stack, filc_ptr_object(
                      filc_flight_ptr_load_with_manual_tracking(array->ptrs + index)));
    }
}

filc_exact_ptr_table* filc_exact_ptr_table_create(filc_thread* my_thread)
{
    filc_exact_ptr_table* result = (filc_exact_ptr_table*)
        filc_object_special_payload_with_manual_tracking(
            filc_allocate_special(
                my_thread, sizeof(filc_exact_ptr_table), 1, FILC_SPECIAL_TYPE_EXACT_PTR_TABLE));

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
        || (filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_FREE))
        return (uintptr_t)filc_ptr_ptr(ptr);
    
    pas_allocation_config allocation_config;
    bmalloc_initialize_allocation_config(&allocation_config);

    filc_uintptr_ptr_hash_map_entry decode_entry;
    decode_entry.key = (uintptr_t)filc_ptr_ptr(ptr);
    filc_flight_ptr_store(my_thread, &decode_entry.value, ptr);
    
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
        if (filc_object_get_flags(filc_ptr_object(entry.value)) & FILC_OBJECT_FLAG_FREE)
            continue;
        fugc_mark(stack, filc_ptr_object(entry.value));
        filc_uintptr_ptr_hash_map_add_new(&new_decode_map, entry, NULL, &allocation_config);
    }
    filc_uintptr_ptr_hash_map_destruct(&ptr_table->decode_map, &allocation_config);
    ptr_table->decode_map = new_decode_map;
    
    pas_lock_unlock(&ptr_table->lock);
}

filc_ptr filc_native_zgc_alloc(filc_thread* my_thread, size_t size)
{
    return filc_ptr_create_with_object_and_manual_tracking(filc_allocate(my_thread, size));
}

filc_ptr filc_native_zgc_aligned_alloc(filc_thread* my_thread, size_t alignment, size_t size)
{
    return filc_ptr_create_with_object_and_manual_tracking(
        filc_allocate_with_alignment(my_thread, size, alignment));
}

static filc_object* object_for_deallocate(filc_ptr ptr)
{
    FILC_CHECK(
        filc_ptr_object(ptr),
        NULL,
        "cannot free ptr with no object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        !filc_object_is_special(filc_ptr_object(ptr)),
        NULL,
        "cannot free special object %s.",
        filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        !(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_GLOBAL),
        NULL,
        "cannot free global object %s.",
        filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        !(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_MMAP),
        NULL,
        "cannot free mmap object %s.",
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
    return filc_ptr_create_with_object_and_manual_tracking(
        filc_reallocate(my_thread, object_for_deallocate(old_ptr), size));
}

filc_ptr filc_native_zgc_aligned_realloc(filc_thread* my_thread,
                                         filc_ptr old_ptr, size_t alignment, size_t size)
{
    if (!filc_ptr_ptr(old_ptr))
        return filc_native_zgc_aligned_alloc(my_thread, alignment, size);
    return filc_ptr_create_with_object_and_manual_tracking(
        filc_reallocate_with_alignment(my_thread, object_for_deallocate(old_ptr), alignment, size));
}

void filc_native_zgc_free(filc_thread* my_thread, filc_ptr ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    if (!filc_ptr_ptr(ptr))
        return;
    filc_free(object_for_deallocate(ptr));
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
    return filc_ptr_object(ptr)
        && !(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_FREE);
}

filc_ptr filc_native_zptrtable_new(filc_thread* my_thread)
{
    return filc_ptr_for_special_payload_with_manual_tracking(filc_ptr_table_create(my_thread));
}

size_t filc_native_zptrtable_encode(filc_thread* my_thread, filc_ptr table_ptr, filc_ptr ptr)
{
    filc_check_access_special(table_ptr, FILC_SPECIAL_TYPE_PTR_TABLE);
    return filc_ptr_table_encode(my_thread, (filc_ptr_table*)filc_ptr_ptr(table_ptr), ptr);
}

filc_ptr filc_native_zptrtable_decode(filc_thread* my_thread, filc_ptr table_ptr, size_t encoded_ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_check_access_special(table_ptr, FILC_SPECIAL_TYPE_PTR_TABLE);
    return filc_ptr_table_decode_with_manual_tracking(
        (filc_ptr_table*)filc_ptr_ptr(table_ptr), encoded_ptr);
}

filc_ptr filc_native_zexact_ptrtable_new(filc_thread* my_thread)
{
    return filc_ptr_for_special_payload_with_manual_tracking(filc_exact_ptr_table_create(my_thread));
}

size_t filc_native_zexact_ptrtable_encode(filc_thread* my_thread, filc_ptr table_ptr, filc_ptr ptr)
{
    filc_check_access_special(table_ptr, FILC_SPECIAL_TYPE_EXACT_PTR_TABLE);
    return filc_exact_ptr_table_encode(my_thread, (filc_exact_ptr_table*)filc_ptr_ptr(table_ptr), ptr);
}

filc_ptr filc_native_zexact_ptrtable_decode(filc_thread* my_thread, filc_ptr table_ptr,
                                            size_t encoded_ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    filc_check_access_special(table_ptr, FILC_SPECIAL_TYPE_EXACT_PTR_TABLE);
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
    if (object == &filc_free_singleton) {
        FILC_ASSERT(!filc_object_size_not_null(object), origin);
        FILC_ASSERT(object->aux == filc_aux_create(FILC_OBJECT_FLAG_FREE |
                                                   FILC_OBJECT_FLAG_GLOBAL |
                                                   FILC_OBJECT_FLAG_READONLY,
                                                   NULL), origin);
        return;
    }

    if (filc_object_is_special(object)) {
        FILC_CHECK(
            !filc_object_size_not_null(object),
            origin,
            "special object has non-null size (object = %s, object at %p, upper = %p)",
            filc_object_to_new_string(object), object, object->upper);
        FILC_ASSERT(filc_special_type_is_valid(filc_object_special_type(object)), origin);
        if (filc_object_special_type(object) != FILC_SPECIAL_TYPE_FUNCTION &&
            filc_object_special_type(object) != FILC_SPECIAL_TYPE_DL_HANDLE) {
            FILC_ASSERT(pas_is_aligned(
                            (uintptr_t)filc_object_special_payload_with_manual_tracking(object),
                            FILC_WORD_SIZE), origin);
            FILC_ASSERT(filc_object_aux_ptr(object)
                        == filc_object_special_payload_with_manual_tracking(object), origin);
        }
        return;
    }

    FILC_ASSERT(pas_is_aligned((uintptr_t)filc_object_lower(object), FILC_WORD_SIZE), origin);
    FILC_ASSERT(pas_is_aligned((uintptr_t)filc_object_upper(object), FILC_WORD_SIZE), origin);
    FILC_ASSERT(filc_object_upper(object) >= filc_object_lower(object), origin);
}

void filc_validate_ptr(filc_ptr ptr, const filc_origin* origin)
{
    if (filc_ptr_is_boxed_int(ptr))
        return;
    
    filc_validate_object(filc_ptr_object(ptr), origin);
}

filc_ptr filc_native_zptr_to_new_string(filc_thread* my_thread, filc_ptr ptr)
{
    char* str = filc_ptr_to_new_string(ptr);
    filc_ptr result = filc_strdup(my_thread, str);
    bmalloc_deallocate(str);
    return result;
}

filc_ptr filc_native_zptr_contents_to_new_string(filc_thread* my_thread, filc_ptr ptr)
{
    char* str = filc_ptr_contents_to_new_string(ptr);
    filc_ptr result = filc_strdup(my_thread, str);
    bmalloc_deallocate(str);
    return result;
}

PAS_NEVER_INLINE PAS_NO_RETURN void filc_check_aligned_access_fail(
    filc_ptr ptr, size_t bytes, size_t alignment, filc_access_kind kind)
{
    filc_validate_ptr(ptr, NULL);

    PAS_ASSERT(bytes);

    FILC_CHECK(
        pas_is_aligned((uintptr_t)filc_ptr_ptr(ptr), alignment),
        NULL,
        "alignment requirement of %zu bytes not met during %s; in this case ptr %% %zu = %zu "
        "(ptr = %s).",
        filc_access_kind_get_string(kind), alignment, alignment,
        (size_t)filc_ptr_ptr(ptr) % alignment, filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        filc_ptr_object(ptr),
        NULL,
        "cannot %s pointer with null object (ptr = %s).",
        filc_access_kind_get_string(kind), filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        !(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_FREE),
        NULL,
        "cannot %s pointer to free object (ptr = %s).",
        filc_access_kind_get_string(kind), filc_ptr_to_new_string(ptr));
    
    FILC_CHECK(
        !(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAGS_SPECIAL_MASK),
        NULL,
        "cannot %s pointer to special object (ptr = %s).",
        filc_access_kind_get_string(kind), filc_ptr_to_new_string(ptr));
    
    FILC_CHECK(
        filc_ptr_ptr(ptr) >= filc_ptr_lower(ptr),
        NULL,
        "cannot %s pointer with ptr < lower (ptr = %s).", 
        filc_access_kind_get_string(kind), filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        filc_ptr_ptr(ptr) < filc_ptr_upper(ptr),
        NULL,
        "cannot %s pointer with ptr >= upper (ptr = %s).",
        filc_access_kind_get_string(kind), filc_ptr_to_new_string(ptr));

    FILC_CHECK(
        bytes <= (uintptr_t)((char*)filc_ptr_upper(ptr) - (char*)filc_ptr_ptr(ptr)),
        NULL,
        "cannot %s %zu bytes when upper - ptr = %zu (ptr = %s).",
        filc_access_kind_get_string(kind), bytes,
        (size_t)((char*)filc_ptr_upper(ptr) - (char*)filc_ptr_ptr(ptr)),
        filc_ptr_to_new_string(ptr));

    if (kind == filc_write_access) {
        FILC_CHECK(
            !(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_READONLY),
            NULL,
            "cannot write to read-only object (ptr = %s).",
            filc_ptr_to_new_string(ptr));
    }
    
    PAS_UNREACHABLE();
}

PAS_NO_RETURN static void finish_panicking(const char* kind_string)
{
    if (exit_on_panic) {
        pas_log("[%d] filc panic: %s\n", pas_getpid(), kind_string);
        _exit(42);
        PAS_ASSERT(!"Should not be reached");
    }
    pas_panic("%s\n", kind_string);
}

PAS_NO_RETURN static void optimized_check_fail_impl(const filc_origin* scheduled_origin,
                                                    const filc_origin*const* semantic_origins)
{
    filc_thread* my_thread = filc_get_my_thread();
    size_t index;
    for (index = 0; semantic_origins[index]; ++index) {
        pas_log("semantic origin:\n");
        filc_origin_dump_all_inline_default(semantic_origins[index], pas_log_stream);
    }
    PAS_ASSERT(my_thread->top_frame);
    if (scheduled_origin) {
        my_thread->top_frame->origin = scheduled_origin;
        pas_log("check scheduled at:\n");
    } else
        pas_log("check scheduled at (uncertain):\n");
    filc_thread_dump_stack(my_thread, pas_log_stream);
    finish_panicking("thwarted a futile attempt to violate memory safety.");
}

PAS_NEVER_INLINE PAS_NO_RETURN void filc_optimized_access_check_fail(
    filc_ptr ptr, const filc_optimized_access_check_origin* check_origin)
{
    pas_log("filc safety error: ");
    if (!filc_ptr_object(ptr)) {
        pas_log("cannot %s pointer with null object.\n",
                check_origin->needs_write ? "write" : "read");
    } else if ((filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_FREE)) {
        pas_log("cannot %s pointer to free object.\n",
                check_origin->needs_write ? "write" : "read");
    } else if ((filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAGS_SPECIAL_MASK)) {
        pas_log("cannot %s pointer to special object.\n",
                check_origin->needs_write ? "write" : "read");
    } else if (check_origin->size && filc_ptr_ptr(ptr) < filc_ptr_lower(ptr)) {
        pas_log("cannot %s pointer with ptr < lower.\n",
                check_origin->needs_write ? "write" : "read");
    } else if (check_origin->size && filc_ptr_ptr(ptr) >= filc_ptr_upper(ptr)) {
        pas_log("cannot %s pointer with ptr >= upper.\n",
                check_origin->needs_write ? "write" : "read");
    } else if (check_origin->size > (char*)filc_ptr_upper(ptr) - (char*)filc_ptr_ptr(ptr)) {
        pas_log("cannot %s %zu bytes when upper - ptr = %zu.\n",
                check_origin->needs_write ? "write" : "read", (size_t)check_origin->size,
                (size_t)((char*)filc_ptr_upper(ptr) - (char*)filc_ptr_ptr(ptr)));
    } else if (check_origin->needs_write &&
               (filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_READONLY))
        pas_log("cannot write to read-only object.\n");
    else if (!pas_is_aligned((uintptr_t)filc_ptr_ptr(ptr) + (uintptr_t)check_origin->alignment_offset,
                             check_origin->alignment))
        pas_log("alignment requirement of %u bytes not met.\n", (unsigned)check_origin->alignment);
    else
        PAS_ASSERT(!"Should not be reached");
    pas_log("    pointer: ");
    filc_ptr_dump(ptr, pas_log_stream);
    pas_log("\n");
    pas_log("    expected");
    if (check_origin->size) {
        pas_log(" %zu %sbytes",
                (size_t)check_origin->size, check_origin->needs_write ? "writable " : "");
    } else if (check_origin->needs_write)
        pas_log(" writable capability");
    else
        pas_log(" valid capability");
    PAS_ASSERT(check_origin->alignment >= 1);
    PAS_ASSERT(check_origin->alignment <= FILC_WORD_SIZE);
    PAS_ASSERT(pas_is_power_of_2(check_origin->alignment));
    PAS_ASSERT(check_origin->alignment_offset < check_origin->alignment);
    if (check_origin->alignment > 1) {
        pas_log(" with ptr aligned to %zu bytes", (size_t)check_origin->alignment);
        if (check_origin->alignment_offset)
            pas_log(" at offset %zu", (size_t)check_origin->alignment_offset);
    }
    pas_log(".\n");
    optimized_check_fail_impl(check_origin->scheduled_origin, check_origin->semantic_origins);
}

PAS_NEVER_INLINE PAS_NO_RETURN void filc_optimized_alignment_contradiction(
    filc_ptr ptr, const filc_optimized_alignment_contradiction_origin* contradiction_origin)
{
    pas_log("filc safety error: alignment contradiction.\n");
    pas_log("    pointer: ");
    filc_ptr_dump(ptr, pas_log_stream);
    pas_log("\n");
    pas_log("required alignments:\n");
    /* Gotta have at least two alignments for there to have been a contradiction! */
    PAS_ASSERT(contradiction_origin->alignments[0].alignment);
    PAS_ASSERT(contradiction_origin->alignments[1].alignment);
    size_t index;
    for (index = 0; contradiction_origin->alignments[index].alignment; ++index) {
        pas_log("    align to %zu at offset %zu.\n",
                (size_t)contradiction_origin->alignments[index].alignment,
                (size_t)contradiction_origin->alignments[index].alignment_offset);
    }
    optimized_check_fail_impl(contradiction_origin->scheduled_origin,
                              contradiction_origin->semantic_origins);
}

void filc_check_function_call(filc_ptr ptr)
{
    filc_check_access_special(ptr, FILC_SPECIAL_TYPE_FUNCTION);
}

PAS_NO_RETURN void filc_check_function_call_fail(filc_ptr ptr)
{
    filc_check_function_call(ptr);
    PAS_UNREACHABLE();
}

static void fix_origin(const filc_origin* origin)
{
    filc_thread* my_thread = filc_get_my_thread();
    if (origin && my_thread->top_frame)
        my_thread->top_frame->origin = origin;
}

static void check_mmap(filc_ptr ptr)
{
    filc_object* object = filc_ptr_object(ptr);
    PAS_ASSERT(object); /* To use this function you should have already checked that the ptr is
                           accessible. */
    FILC_CHECK(
        filc_object_get_flags(object) & FILC_OBJECT_FLAG_MMAP,
        NULL,
        "cannot perform this operation on something that was not mmapped (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        !(filc_object_get_flags(object) & FILC_OBJECT_FLAG_FREE),
        NULL,
        "cannot perform this operation on a free object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
}

void filc_memset_with_exit(filc_thread* my_thread, void* ptr, unsigned value, size_t bytes)
{
    if (bytes <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        filc_memset_small(ptr, value, bytes);
        return;
    }
    filc_exit(my_thread);
    memset(ptr, value, bytes);
    filc_enter(my_thread);
}

void filc_memcpy_with_exit(filc_thread* my_thread, void* dst, const void* src, size_t bytes)
{
    if (bytes <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        memcpy(dst, src, bytes);
        return;
    }
    filc_exit(my_thread);
    memcpy(dst, src, bytes);
    filc_enter(my_thread);
}

void filc_memmove_with_exit(filc_thread* my_thread, void* dst, const void* src, size_t bytes)
{
    if (bytes <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        memmove(dst, src, bytes);
        return;
    }
    filc_exit(my_thread);
    memmove(dst, src, bytes);
    filc_enter(my_thread);
}

void filc_low_level_ptr_safe_bzero_with_exit(filc_thread* my_thread, void* raw_ptr, size_t bytes)
{
    if (bytes <= FILC_MAX_BYTES_BETWEEN_POLLCHECKS) {
        filc_low_level_ptr_safe_bzero(raw_ptr, bytes);
        return;
    }
    filc_exit(my_thread);
    filc_low_level_ptr_safe_bzero(raw_ptr, bytes);
    filc_enter(my_thread);
}

PAS_NO_RETURN PAS_NEVER_INLINE static void memset_fail(
    filc_ptr ptr, size_t count, const filc_origin* passed_origin)
{
    static const bool verbose = false;
    char* raw_ptr;

    filc_thread* my_thread = filc_get_my_thread();

    fix_origin(passed_origin);

    FILC_DEFINE_FRAME("memset");
    filc_push_frame(my_thread, frame);

    raw_ptr = filc_ptr_ptr(ptr);
    
    if (verbose)
        pas_log("count = %zu\n", count);

    filc_check_access(ptr, count, filc_write_access);
    PAS_UNREACHABLE();
}

PAS_ALWAYS_INLINE static void nuke_aux_entry(char* aux_ptr, size_t offset)
{
    filc_lower_or_box* lower_or_box_ptr = (filc_lower_or_box*)(aux_ptr + offset);
    filc_lower_or_box_store_unfenced_unbarriered(
        lower_or_box_ptr, filc_lower_or_box_create_lower(NULL));
}

PAS_ALWAYS_INLINE static void nuke_aux_range(filc_thread* my_thread, char* aux_ptr,
                                             size_t aligned_start_offset, size_t aligned_end_offset,
                                             filc_size_mode size_mode)
{
    if (size_mode == filc_large_size)
        filc_exit(my_thread);

    size_t offset;
    for (offset = aligned_start_offset; offset < aligned_end_offset; offset += FILC_WORD_SIZE)
        nuke_aux_entry(aux_ptr, offset);
    
    if (size_mode == filc_large_size)
        filc_enter(my_thread);
}

PAS_ALWAYS_INLINE static void memset_impl_specialized(filc_thread* my_thread, filc_ptr ptr,
                                                      unsigned value, size_t count,
                                                      filc_size_mode size_mode,
                                                      const filc_origin* origin)
{
    PAS_TESTING_ASSERT(count);

    char* raw_ptr = filc_ptr_ptr(ptr);
    filc_object* object = filc_ptr_object(ptr);

    if (!object)
        memset_fail(ptr, count, origin);

    char* lower = (char*)filc_object_lower(object);
    char* upper = (char*)filc_object_upper(object);
    CHECK_BOUNDS_FAST(raw_ptr, lower, upper, count, memset_fail(ptr, count, origin));
    CHECK_WRITE_FAST(object, memset_fail(ptr, count, origin));

    if (size_mode == filc_small_size)
        filc_memset_small(raw_ptr, value, count);
    else
        filc_memset_with_exit(my_thread, raw_ptr, value, count);

    char* aux_ptr = filc_object_aux_ptr(object);
    if (!aux_ptr)
        return;

    size_t start_offset = raw_ptr - lower;
    size_t end_offset = start_offset + count;
    size_t aligned_start_offset = pas_round_down_to_power_of_2(start_offset, FILC_WORD_SIZE);
    size_t aligned_end_offset = pas_round_up_to_power_of_2(end_offset, FILC_WORD_SIZE);
    nuke_aux_range(my_thread, aux_ptr, aligned_start_offset, aligned_end_offset, size_mode);
}

PAS_NEVER_INLINE static void memset_large(filc_thread* my_thread, filc_ptr ptr, unsigned value,
                                          size_t count, const filc_origin* origin)
{
    memset_impl_specialized(my_thread, ptr, value, count, filc_large_size, origin);
}

/* Assumes ptr is tracked by GC. */
PAS_ALWAYS_INLINE static void memset_impl(filc_thread* my_thread, filc_ptr ptr, unsigned value,
                                          size_t count, const filc_origin* origin)
{
    if (!count)
        return;
    
    if (count > FILC_MAX_BYTES_FOR_SMALL_CASE) {
        memset_large(my_thread, ptr, value, count, origin);
        return;
    }

    memset_impl_specialized(my_thread, ptr, value, count, filc_small_size, origin);
}

void filc_memset(filc_thread* my_thread, filc_ptr ptr, unsigned value, size_t count,
                 const filc_origin* origin)
{
    memset_impl(my_thread, ptr, value, count, origin);
}

PAS_NO_RETURN PAS_NEVER_INLINE void memmove_fail(filc_ptr dst, filc_ptr src, size_t count,
                                                 const filc_origin* passed_origin)
{
    static const bool verbose = false;

    filc_thread* my_thread = filc_get_my_thread();

    if (verbose) {
        pas_log("Doing memmove fail to %s from %s with count %zu\n",
                filc_ptr_to_new_string(dst),
                filc_ptr_to_new_string(src),
                count);
        filc_thread_dump_stack(my_thread, pas_log_stream);
    }

    fix_origin(passed_origin);

    FILC_DEFINE_FRAME("memmove");
    filc_push_frame(my_thread, frame);

    filc_check_access(dst, count, filc_write_access);
    filc_check_access(src, count, filc_read_access);
    PAS_UNREACHABLE();
}

enum memmove_smidgen_part {
    memmove_lower_smidgen,
    memmove_upper_smidgen
};

typedef enum memmove_smidgen_part memmove_smidgen_part;

static PAS_ALWAYS_INLINE void memmove_smidgen(memmove_smidgen_part part, char* dst_aux_ptr,
                                              size_t dst_start_offset, size_t dst_end_offset,
                                              bool has_dst_aux)
{
    /* This is hella subtle. has_dst_aux means: did we have a dst_aux at the start of the memmove?
       If we didn't have one at the start, then there's no reason to nuke the smidgen, even if we
       since created one. Simple! */
    if (!has_dst_aux)
        return;
    switch (part) {
    case memmove_lower_smidgen:
        if (pas_is_aligned(dst_start_offset, FILC_WORD_SIZE))
            return;
        nuke_aux_entry(dst_aux_ptr, pas_round_down_to_power_of_2(dst_start_offset, FILC_WORD_SIZE));
        return;

    case memmove_upper_smidgen:
        if (pas_is_aligned(dst_end_offset, FILC_WORD_SIZE))
            return;
        nuke_aux_entry(dst_aux_ptr, pas_round_down_to_power_of_2(dst_end_offset, FILC_WORD_SIZE));
        return;
    }
    PAS_ASSERT(!"Bad part");
}

PAS_ALWAYS_INLINE static void memmove_aux_loop_body(filc_thread* my_thread,
                                                    char** dst_aux_ptr,
                                                    char* src_aux_ptr,
                                                    size_t dst_start_offset,
                                                    size_t src_start_offset,
                                                    size_t dst_end_offset,
                                                    bool do_barrier,
                                                    bool has_dst_aux,
                                                    filc_object* dst_object,
                                                    filc_object* src_object,
                                                    bool is_up,
                                                    const filc_origin* passed_origin)
{
    PAS_TESTING_ASSERT(dst_end_offset >= dst_start_offset);
    PAS_TESTING_ASSERT(pas_is_aligned(dst_start_offset, FILC_WORD_SIZE));
    PAS_TESTING_ASSERT(pas_is_aligned(src_start_offset, FILC_WORD_SIZE));
    PAS_TESTING_ASSERT(pas_is_aligned(dst_end_offset, FILC_WORD_SIZE));
    PAS_TESTING_ASSERT(src_aux_ptr);
    PAS_TESTING_ASSERT(pas_is_aligned((uintptr_t)src_aux_ptr, FILC_WORD_SIZE));
    size_t dst_offset;
    size_t src_offset;
    if (is_up) {
        dst_offset = dst_start_offset;
        src_offset = src_start_offset;
    } else {
        dst_offset = dst_end_offset;
        src_offset = src_start_offset + (dst_end_offset - dst_start_offset);
    }
    for (;;) {
        if (is_up) {
            if (dst_offset >= dst_end_offset) {
                PAS_TESTING_ASSERT(dst_offset == dst_end_offset);
                return;
            }
        } else {
            if (dst_offset <= dst_start_offset) {
                PAS_TESTING_ASSERT(dst_offset == dst_start_offset);
                return;
            }
            dst_offset -= FILC_WORD_SIZE;
            src_offset -= FILC_WORD_SIZE;
        }
        filc_lower_or_box src_lower_or_box = filc_lower_or_box_load_unfenced(
            (filc_lower_or_box*)(src_aux_ptr + src_offset));
        void* lower = filc_lower_or_box_extract_lower(src_lower_or_box);
        bool do_store = true;
        if (!has_dst_aux && !*dst_aux_ptr) {
            if (lower) {
                *dst_aux_ptr = filc_object_ensure_aux_ptr(my_thread, dst_object);

                /* We could have exited so recheck that the objects aren't free. Note that checking the dst_object is
                   optional for memory safety (if it's free then it doesn't matter if we copy dangling pointers into it). */
                FILC_CHECK(
                    !(filc_object_get_flags(dst_object) & FILC_OBJECT_FLAG_FREE),
                    passed_origin,
                    "destination object became free during memmove (object = %s).",
                    filc_object_to_new_string(dst_object));
                FILC_CHECK(
                    !(filc_object_get_flags(src_object) & FILC_OBJECT_FLAG_FREE),
                    passed_origin,
                    "source object became free during memmove (object = %s).",
                    filc_object_to_new_string(src_object));
            } else
                do_store = false;
        }
        if (do_store) {
            if (!has_dst_aux) {
                /* FIXME: Could optimize the no-dst-aux case to have two loops; one for before when
                   we create the aux and one for after.
                
                   Then, we could do the same barrier specialization that the other paths use.
                
                   But I suspect that this isn't a sufficiently interesting case to worry about for
                   now, and it would not be a super trivial optimization. */
                filc_store_barrier(my_thread, filc_object_for_lower(lower));
            } else if (do_barrier && lower)
                fugc_mark(&my_thread->mark_stack, filc_object_for_lower_not_null(lower));
            filc_lower_or_box* dst_lower_or_box_ptr = (filc_lower_or_box*)(*dst_aux_ptr + dst_offset);
            filc_lower_or_box_store_unfenced_unbarriered(
                dst_lower_or_box_ptr, filc_lower_or_box_create_lower(lower));
        }
        if (is_up) {
            dst_offset += FILC_WORD_SIZE;
            src_offset += FILC_WORD_SIZE;
        }
    }
}

PAS_ALWAYS_INLINE static void memmove_aux_loop(filc_thread* my_thread,
                                               char* dst_aux_ptr,
                                               char* src_aux_ptr,
                                               size_t dst_start_offset,
                                               size_t src_start_offset,
                                               size_t dst_end_offset,
                                               filc_size_mode size_mode,
                                               bool do_barrier,
                                               bool has_dst_aux,
                                               filc_object* dst_object,
                                               filc_object* src_object,
                                               bool is_up,
                                               const filc_origin* passed_origin)
{
    dst_start_offset = pas_round_up_to_power_of_2(dst_start_offset, FILC_WORD_SIZE);
    src_start_offset = pas_round_up_to_power_of_2(src_start_offset, FILC_WORD_SIZE);
    dst_end_offset = pas_round_down_to_power_of_2(dst_end_offset, FILC_WORD_SIZE);

    if (dst_end_offset <= dst_start_offset)
        return;
    
    switch (size_mode) {
    case filc_small_size:
        memmove_aux_loop_body(my_thread, &dst_aux_ptr, src_aux_ptr,
                              dst_start_offset, src_start_offset, dst_end_offset,
                              do_barrier, has_dst_aux, dst_object, src_object, is_up, passed_origin);
        break;

    case filc_large_size: {
        PAS_TESTING_ASSERT(dst_object);
        PAS_TESTING_ASSERT(src_object);
        size_t dst_offset;
        size_t src_offset;
        if (is_up) {
            dst_offset = dst_start_offset;
            src_offset = src_start_offset;
        } else {
            dst_offset = dst_end_offset;
            src_offset = src_start_offset + (dst_end_offset - dst_start_offset);
        }
        for (;;) {
            size_t current_dst_start_offset;
            size_t current_dst_end_offset;
            size_t current_src_start_offset;
            PAS_ASSERT(pas_is_aligned(FILC_MAX_BYTES_BETWEEN_POLLCHECKS, FILC_WORD_SIZE));
            if (is_up) {
                if (dst_offset >= dst_end_offset) {
                    PAS_TESTING_ASSERT(dst_offset == dst_end_offset);
                    break;
                }
                current_dst_start_offset = dst_offset;
                current_dst_end_offset = pas_min_uintptr(
                    dst_offset + FILC_MAX_BYTES_BETWEEN_POLLCHECKS,
                    dst_end_offset);
                current_src_start_offset = src_offset;
                dst_offset = current_dst_end_offset;
                src_offset += current_dst_end_offset - current_dst_start_offset;
            } else {
                if (dst_offset <= dst_start_offset) {
                    PAS_TESTING_ASSERT(dst_offset == dst_start_offset);
                    break;
                }
                size_t step = pas_min_uintptr(FILC_MAX_BYTES_BETWEEN_POLLCHECKS,
                                              dst_offset - dst_start_offset);
                current_dst_end_offset = dst_offset;
                current_dst_start_offset = dst_offset - step;
                current_src_start_offset = src_offset - step;
                dst_offset -= step;
                src_offset -= step;
            }
            PAS_TESTING_ASSERT(pas_is_aligned(current_dst_start_offset, FILC_WORD_SIZE));
            PAS_TESTING_ASSERT(pas_is_aligned(current_dst_end_offset, FILC_WORD_SIZE));
            PAS_TESTING_ASSERT(pas_is_aligned(current_src_start_offset, FILC_WORD_SIZE));
            if (!has_dst_aux && PAS_UNLIKELY(filc_is_marking)) {
                bool do_barrier = true;
                memmove_aux_loop_body(my_thread, &dst_aux_ptr, src_aux_ptr,
                                      current_dst_start_offset, current_src_start_offset,
                                      current_dst_end_offset,
                                      do_barrier, has_dst_aux, dst_object, src_object, is_up, passed_origin);
            } else {
                bool do_barrier = false;
                memmove_aux_loop_body(my_thread, &dst_aux_ptr, src_aux_ptr,
                                      current_dst_start_offset, current_src_start_offset,
                                      current_dst_end_offset,
                                      do_barrier, has_dst_aux, dst_object, src_object, is_up, passed_origin);
            }
            if (PAS_UNLIKELY(filc_pollcheck(my_thread, passed_origin))) {
                /* NOTE: The destination object check isn't strictly necessary for memory safety. */
                FILC_CHECK(
                    !(filc_object_get_flags(dst_object) & FILC_OBJECT_FLAG_FREE),
                    passed_origin,
                    "destination object became free during memmove (object = %s).",
                    filc_object_to_new_string(dst_object));
                FILC_CHECK(
                    !(filc_object_get_flags(src_object) & FILC_OBJECT_FLAG_FREE),
                    passed_origin,
                    "source object became free during memmove (object = %s).",
                    filc_object_to_new_string(src_object));
            }
        }
        break;
    } }
}

PAS_ALWAYS_INLINE static void memmove_aux_direction_specialized(filc_thread* my_thread,
                                                                char* dst_aux_ptr,
                                                                char* src_aux_ptr,
                                                                size_t dst_start_offset,
                                                                size_t src_start_offset,
                                                                size_t dst_end_offset,
                                                                filc_size_mode size_mode,
                                                                bool do_barrier,
                                                                bool has_dst_aux,
                                                                filc_object* dst_object,
                                                                filc_object* src_object,
                                                                bool is_up,
                                                                const filc_origin* passed_origin)
{
    memmove_smidgen(is_up ? memmove_lower_smidgen : memmove_upper_smidgen,
                    dst_aux_ptr, dst_start_offset, dst_end_offset, has_dst_aux);

    memmove_aux_loop(my_thread, dst_aux_ptr, src_aux_ptr, dst_start_offset, src_start_offset,
                     dst_end_offset, size_mode, do_barrier, has_dst_aux, dst_object, src_object,
                     is_up, passed_origin);

    memmove_smidgen(is_up ? memmove_upper_smidgen : memmove_lower_smidgen,
                    dst_aux_ptr, dst_start_offset, dst_end_offset, has_dst_aux);
}

PAS_ALWAYS_INLINE static void memmove_aux_barrier_specialized(filc_thread* my_thread,
                                                              char* dst_aux_ptr,
                                                              char* src_aux_ptr,
                                                              size_t dst_start_offset,
                                                              size_t src_start_offset,
                                                              size_t dst_end_offset,
                                                              filc_size_mode size_mode,
                                                              bool do_barrier,
                                                              bool has_dst_aux,
                                                              filc_object* dst_object,
                                                              filc_object* src_object,
                                                              const filc_origin* passed_origin)
{
    // NOTE: If dst_aux_ptr != src_aux_ptr, then we could use either variant.
    if (dst_aux_ptr + dst_start_offset < src_aux_ptr + src_start_offset || !has_dst_aux) {
        bool is_up = true;
        memmove_aux_direction_specialized(my_thread, dst_aux_ptr, src_aux_ptr,
                                          dst_start_offset, src_start_offset, dst_end_offset,
                                          size_mode, do_barrier, has_dst_aux, dst_object, src_object,
                                          is_up, passed_origin);
        return;
    }
    bool is_up = false;
    memmove_aux_direction_specialized(my_thread, dst_aux_ptr, src_aux_ptr,
                                      dst_start_offset, src_start_offset, dst_end_offset,
                                      size_mode, do_barrier, has_dst_aux, dst_object, src_object,
                                      is_up, passed_origin);
}

PAS_NEVER_INLINE static void memmove_aux_small_barrier(filc_thread* my_thread,
                                                       char* dst_aux_ptr,
                                                       char* src_aux_ptr,
                                                       size_t dst_start_offset,
                                                       size_t src_start_offset,
                                                       size_t dst_end_offset)
{
    bool do_barrier = true;
    bool has_dst_aux = true;
    memmove_aux_barrier_specialized(my_thread, dst_aux_ptr, src_aux_ptr,
                                    dst_start_offset, src_start_offset, dst_end_offset,
                                    filc_small_size, do_barrier, has_dst_aux, NULL, NULL, NULL);
}

PAS_NEVER_INLINE static void memmove_aux_small_no_dst(filc_thread* my_thread,
                                                      char* dst_aux_ptr,
                                                      char* src_aux_ptr,
                                                      size_t dst_start_offset,
                                                      size_t src_start_offset,
                                                      size_t dst_end_offset,
                                                      filc_object* dst_object,
                                                      filc_object* src_object,
                                                      const filc_origin* passed_origin)
{
    bool do_barrier = false;
    bool has_dst_aux = false;
    memmove_aux_barrier_specialized(my_thread, dst_aux_ptr, src_aux_ptr,
                                    dst_start_offset, src_start_offset, dst_end_offset,
                                    filc_small_size, do_barrier, has_dst_aux, dst_object, src_object, passed_origin);
}

PAS_NEVER_INLINE static void memmove_aux_large_no_dst(filc_thread* my_thread,
                                                      char* dst_aux_ptr,
                                                      char* src_aux_ptr,
                                                      size_t dst_start_offset,
                                                      size_t src_start_offset,
                                                      size_t dst_end_offset,
                                                      filc_object* dst_object,
                                                      filc_object* src_object,
                                                      const filc_origin* passed_origin)
{
    bool do_barrier = false;
    bool has_dst_aux = false;
    memmove_aux_barrier_specialized(my_thread, dst_aux_ptr, src_aux_ptr,
                                    dst_start_offset, src_start_offset, dst_end_offset,
                                    filc_large_size, do_barrier, has_dst_aux, dst_object, src_object,
                                    passed_origin);
}

/* Assumes that the dst/src are tracked by GC. Assumes that count is nonzero. */
PAS_ALWAYS_INLINE static void memmove_impl_size_specialized(filc_thread* my_thread, filc_ptr dst,
                                                            filc_ptr src, size_t count,
                                                            filc_size_mode size_mode,
                                                            const filc_origin* origin)
{
    static const bool verbose = false;

    if (verbose) {
        pas_log("Doing memmove to %s from %s with count %zu\n",
                filc_ptr_to_new_string(dst),
                filc_ptr_to_new_string(src),
                count);
        filc_thread_dump_stack(my_thread, pas_log_stream);
    }

    PAS_TESTING_ASSERT(count);

    filc_object* dst_object = filc_ptr_object(dst);
    filc_object* src_object = filc_ptr_object(src);

    if (!dst_object || !src_object)
        memmove_fail(dst, src, count, origin);

    char* dst_start = filc_ptr_ptr(dst);
    char* src_start = filc_ptr_ptr(src);

    char* dst_lower = (char*)filc_object_lower(dst_object);
    char* dst_upper = (char*)filc_object_upper(dst_object);
    char* src_lower = (char*)filc_object_lower(src_object);
    char* src_upper = (char*)filc_object_upper(src_object);

    CHECK_BOUNDS_FAST(dst_start, dst_lower, dst_upper, count, memmove_fail(dst, src, count, origin));
    CHECK_BOUNDS_FAST(src_start, src_lower, src_upper, count, memmove_fail(dst, src, count, origin));
    CHECK_WRITE_FAST(dst_object, memmove_fail(dst, src, count, origin));

    if (size_mode == filc_small_size)
        filc_memmove_small(dst_start, src_start, count);
    else {
        filc_memmove_with_exit(my_thread, dst_start, src_start, count);
        CHECK_ACCESSIBLE_FAST(src_object, memmove_fail(dst, src, count, origin));
        CHECK_ACCESSIBLE_FAST(dst_object, memmove_fail(dst, src, count, origin));
    }

    /* Here are the cases in descending order of nastiness:
       
       - Target doesn't have an aux, but the source does, and the offsets are in phase. In that case,
         we have to scan through the source aux until we find a non-NULL lower, and then allocate the
         target aux. Allocating the aux might exit, so the barrier state might change.

       - We have to barrier, both have aux, and the offsets are in phase.

       - We don't have to barrier, both have aux, and the offsets are in phase.

       - The destination has aux, but either the source doesn't or the offsets are out of phase.
         Doesn't matter if we have a barrier in this case. We just nuke the dst aux range.

       - Neither has aux. Or, source has aux, the destination does not, but the offsets are out of
         phase. Then there's just nothing to do. Best case ever! */
    char* dst_aux_ptr = filc_object_aux_ptr(dst_object);
    char* src_aux_ptr = filc_object_aux_ptr(src_object);
    size_t dst_start_offset = dst_start - dst_lower;
    size_t src_start_offset = src_start - src_lower;
    size_t dst_end_offset = dst_start_offset + count;
    if (dst_aux_ptr) {
        if (!src_aux_ptr || (pas_modulo_power_of_2((uintptr_t)dst_start, FILC_WORD_SIZE) !=
                             pas_modulo_power_of_2((uintptr_t)src_start, FILC_WORD_SIZE))) {
            nuke_aux_range(my_thread, dst_aux_ptr,
                           pas_round_down_to_power_of_2(dst_start_offset, FILC_WORD_SIZE),
                           pas_round_up_to_power_of_2(dst_end_offset, FILC_WORD_SIZE),
                           size_mode);
            return;
        }
        if (size_mode == filc_large_size || PAS_LIKELY(!filc_is_marking)) {
            /* NOTE: do_barrier is ignored if we're in large_size. */
            bool do_barrier = false;
            bool has_dst_aux = true;
            memmove_aux_barrier_specialized(my_thread, dst_aux_ptr, src_aux_ptr,
                                            dst_start_offset, src_start_offset, dst_end_offset,
                                            size_mode, do_barrier, has_dst_aux, dst_object,
                                            src_object, origin);
            return;
        }
        memmove_aux_small_barrier(my_thread, dst_aux_ptr, src_aux_ptr,
                                  dst_start_offset, src_start_offset, dst_end_offset);
        return;
    }
    if (src_aux_ptr && (pas_modulo_power_of_2((uintptr_t)dst_start, FILC_WORD_SIZE) ==
                        pas_modulo_power_of_2((uintptr_t)src_start, FILC_WORD_SIZE))) {
        if (size_mode == filc_small_size) {
            memmove_aux_small_no_dst(my_thread, dst_aux_ptr, src_aux_ptr,
                                     dst_start_offset, src_start_offset, dst_end_offset,
                                     dst_object, src_object, origin);
            return;
        }
        memmove_aux_large_no_dst(my_thread, dst_aux_ptr, src_aux_ptr,
                                 dst_start_offset, src_start_offset, dst_end_offset,
                                 dst_object, src_object, origin);
        return;
    }
}

PAS_NEVER_INLINE void memmove_large(filc_thread* my_thread, filc_ptr dst, filc_ptr src, size_t count,
                                    const filc_origin* origin)
{
    memmove_impl_size_specialized(my_thread, dst, src, count, filc_large_size, origin);
}

PAS_ALWAYS_INLINE void memmove_impl(filc_thread* my_thread, filc_ptr dst, filc_ptr src, size_t count,
                                    const filc_origin* origin)
{
    if (!count)
        return;
    if (PAS_LIKELY(count < FILC_MAX_BYTES_BETWEEN_POLLCHECKS))
        memmove_impl_size_specialized(my_thread, dst, src, count, filc_small_size, origin);
    else
        memmove_large(my_thread, dst, src, count, origin);
}

void filc_memmove(filc_thread* my_thread, filc_ptr dst, filc_ptr src, size_t count,
                  const filc_origin* passed_origin)
{
    static const bool verbose = false;
    if (verbose) {
        pas_log("filc_memmove: dst = ");
        filc_ptr_dump(dst, pas_log_stream);
        pas_log(", src = ");
        filc_ptr_dump(src, pas_log_stream);
        pas_log(", count = %zu, origin = ", count);
        filc_origin_dump_all_inline(passed_origin, "/", pas_log_stream);
        pas_log("\n");
    }
    memmove_impl(my_thread, dst, src, count, passed_origin);
}

static filc_ptr promote_cc_to_heap(filc_thread* my_thread, size_t size)
{
    PAS_ASSERT(size <= filc_thread_cc_total_size(my_thread));

    if (!size)
        return filc_ptr_forge_null();

    /* The calling convention requires that the CC size is always a multiple of word size. */
    PAS_ASSERT(pas_is_aligned(size, FILC_WORD_SIZE));

    filc_object* result_object = allocate_impl(my_thread, size, FILC_OBJECT_FLAG_READONLY);
    filc_thread_track_object(my_thread, result_object);

    size_t offset;
    for (offset = 0; offset < size; offset += FILC_WORD_SIZE) {
        *(filc_word*)((char*)filc_object_lower(result_object) + offset) =
            *(filc_word*)filc_thread_cc_slot_at_offset(my_thread, offset);
    }
    
    for (offset = 0; offset < size; offset += FILC_WORD_SIZE) {
        void* lower = filc_lower_or_box_get_lower(
            filc_lower_or_box_load_unfenced(
                filc_thread_cc_aux_slot_at_offset(my_thread, offset)));
        if (lower) {
            /* FIXME: We should do all of this tracking before we trigger aux creation.
               
               Also, we shouldn't do the tracking at all when we're promoting args to heap, since the
               args are already tracked by the calleer. */
            filc_thread_track_object(my_thread, filc_object_for_lower(lower));
            char* aux_ptr = filc_object_ensure_aux_ptr(my_thread, result_object);
            filc_store_barrier(my_thread, filc_object_for_lower(lower));
            filc_lower_or_box_store_unfenced_unbarriered(
                (filc_lower_or_box*)(aux_ptr + offset),
                filc_lower_or_box_create_lower(lower));
        }
    }

    return filc_ptr_create_with_object_and_manual_tracking(result_object);
}

static size_t demote_cc_from_heap(filc_thread* my_thread, filc_ptr ptr, const filc_origin* origin,
                                  bool do_tracking)
{
    if (!filc_ptr_ptr(ptr))
        return 0;
    
    FILC_CHECK(
        filc_ptr_object(ptr),
        origin,
        "cannot access ptr with null object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        !(filc_object_get_flags(filc_ptr_object(ptr)) & FILC_OBJECT_FLAG_FREE),
        origin,
        "cannot access free object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        !filc_object_is_special(filc_ptr_object(ptr)),
        origin,
        "cannot access special object (ptr = %s).",
        filc_ptr_to_new_string(ptr));
    FILC_CHECK(
        pas_is_aligned((uintptr_t)filc_ptr_ptr(ptr), FILC_WORD_SIZE),
        origin,
        "alignment requirement of %zu bytes not met; in this case ptr %% %zu = %zu (ptr = %s).",
        FILC_WORD_SIZE, FILC_WORD_SIZE, (size_t)filc_ptr_ptr(ptr) % FILC_WORD_SIZE,
        filc_ptr_to_new_string(ptr));

    size_t available = filc_ptr_available(ptr);
    if (!available)
        return 0;

    PAS_ASSERT(filc_ptr_ptr(ptr));
    PAS_ASSERT(pas_is_aligned(available, FILC_WORD_SIZE));
    
    filc_thread_ensure_cc_total_buffer(my_thread, available);

    size_t offset;
    for (offset = 0; offset < available; offset += FILC_WORD_SIZE) {
        *(filc_word*)filc_thread_cc_slot_at_offset(my_thread, offset) =
            *(filc_word*)((char*)filc_ptr_ptr(ptr) + offset);
    }

    char* aux_ptr = filc_ptr_aux_ptr(ptr);
    for (offset = 0; offset < available; offset += FILC_WORD_SIZE) {
        void* lower;
        if (aux_ptr) {
            lower = filc_lower_or_box_extract_lower(
                filc_lower_or_box_load_unfenced((filc_lower_or_box*)(
                                                    aux_ptr + filc_ptr_offset(ptr) + offset)));
            if (do_tracking)
                filc_thread_track_object(my_thread, filc_object_for_lower(lower));
        } else
            lower = NULL;
        filc_lower_or_box_store_unfenced_unbarriered(
            filc_thread_cc_aux_slot_at_offset(my_thread, offset),
            filc_lower_or_box_create_lower(lower));
    }

    return available;
}

filc_ptr filc_promote_args_to_heap(filc_thread* my_thread, size_t size)
{
    filc_native_frame native_frame;
    filc_push_native_frame(my_thread, &native_frame);

    filc_ptr result = promote_cc_to_heap(my_thread, size);

    filc_pop_native_frame(my_thread, &native_frame);

    return result;
}

size_t filc_prepare_to_return_with_data(filc_thread* my_thread, filc_ptr rets,
                                        const filc_origin* origin)
{
    bool do_tracking = false; /* It's not necessary for the function doing the returning to track
                                 pointers that it returns, unless it exits before returning, since
                                 it's the caller's responsibility to track those pointers. */
    return demote_cc_from_heap(my_thread, rets, origin, do_tracking);
}

filc_ptr filc_native_zcall(filc_thread* my_thread, filc_ptr callee_ptr, filc_ptr args_ptr)
{
    bool do_tracking = true;
    size_t arg_size = demote_cc_from_heap(my_thread, args_ptr, NULL, do_tracking);
    
    filc_check_function_call(callee_ptr);

    filc_lock_top_native_frame(my_thread);
    pizlonated_return_value result =
        ((pizlonated_function)filc_ptr_ptr(callee_ptr))(my_thread, arg_size);
    PAS_ASSERT(!result.has_exception);
    filc_unlock_top_native_frame(my_thread);

    return promote_cc_to_heap(my_thread, result.return_size);
}

void filc_native_zmemset(filc_thread* my_thread, filc_ptr dst_ptr, unsigned value, size_t count)
{
    memset_impl(my_thread, dst_ptr, value, count, NULL);
}

void filc_native_zmemmove(filc_thread* my_thread, filc_ptr dst_ptr, filc_ptr src_ptr, size_t count)
{
    memmove_impl(my_thread, dst_ptr, src_ptr, count, NULL);
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
    filc_check_access(str, 1, filc_read_access);
    available = filc_ptr_available(str);
    length = strnlen((char*)filc_ptr_ptr(str), available);
    FILC_ASSERT(length < available, NULL);
    FILC_ASSERT(length + 1 <= available, NULL);

    return finish_check_and_get_new_str((char*)filc_ptr_ptr(str), length);
}

char* filc_check_and_get_new_str_for_valid_range(char* base, size_t size)
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

char* filc_check_and_get_tmp_str_for_valid_range(filc_thread* my_thread, char* base, size_t size)
{
    char* result = filc_check_and_get_new_str_for_valid_range(base, size);
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
    filc_ptr result = filc_ptr_create_with_object(my_thread, filc_allocate(my_thread, size));
    filc_memcpy_with_exit(my_thread, filc_ptr_ptr(result), str, size);
    return result;
}

filc_global_initialization_context* filc_global_initialization_context_create(
    filc_global_initialization_context* parent)
{
    static const bool verbose = false;
    
    filc_global_initialization_context* result;

    if ((char*)&result < (char*)filc_get_my_thread()->stack_limit)
        filc_stack_overflow_failure_impl();

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
    PAS_ASSERT(filc_object_get_flags(object) & FILC_OBJECT_FLAG_GLOBAL);

    if (verbose) {
        pas_log("dealing with context = %p, pizlonated_gptr = %p, object = %p\n",
                context, pizlonated_gptr, object);
    }

    filc_ptr gptr_value = filc_flight_ptr_load_atomic_unfenced_with_manual_tracking(pizlonated_gptr);
    if (filc_ptr_ptr(gptr_value)) {
        PAS_ASSERT(filc_ptr_object(gptr_value));
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

    PAS_ASSERT(!filc_ptr_object(gptr_value));

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
        filc_flight_ptr_store_atomic_unfenced_without_barrier(
            pizlonated_gptr, filc_ptr_create_with_object_and_manual_tracking(object));
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
        filc_ptr result = ((pizlonated_linker_stub)target)(context);
        PAS_ASSERT(filc_ptr_object(result));
        PAS_ASSERT(filc_ptr_ptr(result));
        return result;
    }
    case filc_expr_constant: {
        filc_constexpr_node* node = (filc_constexpr_node*)target;
        switch (node->opcode) {
        case filc_constexpr_add_ptr_immediate:
            return filc_ptr_with_offset(
                get_constant_value(node->left_kind, node->left_target, context),
                node->right_value);
        }
        PAS_ASSERT(!"Bad constexpr opcode");
    } }
    PAS_ASSERT(!"Bad relocation kind");
    return filc_ptr_forge_null();
}

void filc_execute_constant_relocations(
    filc_object* constant, filc_constant_relocation* relocations, size_t num_relocations,
    filc_global_initialization_context* context)
{
    static const bool verbose = false;
    size_t index;
    PAS_ASSERT(constant);
    PAS_ASSERT(context);
    if (verbose)
        pas_log("Executing constant relocations!\n");
    /* Nothing here needs to be atomic, since the constant doesn't become visible to the universe
       until the initialization context is destroyed. */
    char* payload_ptr = (char*)filc_object_lower(constant);
    char* aux_ptr = filc_object_aux_ptr(constant);
    PAS_ASSERT(payload_ptr);
    PAS_ASSERT(aux_ptr);
    for (index = num_relocations; index--;) {
        filc_constant_relocation* relocation;
        relocation = relocations + index;
        PAS_ASSERT(pas_is_aligned(relocation->offset, FILC_WORD_SIZE));
        filc_ptr value = get_constant_value(relocation->kind, relocation->target, context);
        void** raw_ptr_ptr = (void**)(payload_ptr + relocation->offset);
        filc_lower_or_box* lower_or_box_ptr = (filc_lower_or_box*)(aux_ptr + relocation->offset);
        PAS_ASSERT(!*raw_ptr_ptr);
        PAS_ASSERT(filc_lower_or_box_is_null(filc_lower_or_box_load_unfenced(lower_or_box_ptr)));
        *raw_ptr_ptr = filc_ptr_ptr(value);
        filc_lower_or_box_store_unfenced_unbarriered(
            lower_or_box_ptr, filc_lower_or_box_create_lower(filc_ptr_lower(value)));
    }
}

static bool did_run_deferred_global_ctors = false;
static pizlonated_function* deferred_global_ctors = NULL; 
static size_t num_deferred_global_ctors = 0;
static size_t deferred_global_ctors_capacity = 0;

static void run_global_ctor(filc_thread* my_thread, pizlonated_function global_ctor)
{
    if (!run_global_ctors) {
        pas_log("filc: skipping global ctor.\n");
        return;
    }

    FILC_DEFINE_FRAME("run_global_ctor");
    filc_push_frame(my_thread, frame);

    filc_call_user_void(my_thread, global_ctor);

    filc_pop_frame(my_thread, frame);
}

void filc_defer_or_run_global_ctor(pizlonated_function global_ctor)
{
    if (did_run_deferred_global_ctors) {
        filc_thread* my_thread = filc_get_my_thread();
        
        filc_enter(my_thread);
        run_global_ctor(my_thread, global_ctor);
        filc_exit(my_thread);
        return;
    }

    if (num_deferred_global_ctors >= deferred_global_ctors_capacity) {
        pizlonated_function* new_deferred_global_ctors;
        size_t new_deferred_global_ctors_capacity;

        PAS_ASSERT(num_deferred_global_ctors == deferred_global_ctors_capacity);

        new_deferred_global_ctors_capacity = (deferred_global_ctors_capacity + 1) * 2;
        new_deferred_global_ctors = (pizlonated_function*)bmalloc_allocate(
            new_deferred_global_ctors_capacity * sizeof(pizlonated_function));

        memcpy(new_deferred_global_ctors, deferred_global_ctors,
               num_deferred_global_ctors * sizeof(pizlonated_function));

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

void filc_run_global_dtor(pizlonated_function global_dtor)
{
    if (!run_global_dtors) {
        pas_log("filc: skipping global dtor.\n");
        return;
    }
    
    filc_thread* my_thread = filc_get_my_thread();
    
    filc_enter(my_thread);

    FILC_DEFINE_FRAME("run_global_dtor");
    filc_push_frame(my_thread, frame);

    filc_call_user_void(my_thread, global_dtor);

    filc_pop_frame(my_thread, frame);
    filc_exit(my_thread);
}

void filc_native_zrun_deferred_global_ctors(filc_thread* my_thread)
{
    filc_run_deferred_global_ctors(my_thread);
}

void filc_origin_dump_all_inline_default(const filc_origin* origin, pas_stream* stream)
{
    pas_stream_printf(stream, "    ");
    filc_origin_dump_all_inline(origin, " (inlined)\n    ", stream);
    pas_stream_printf(stream, "\n");
}

void filc_thread_dump_stack(filc_thread* thread, pas_stream* stream)
{
    filc_frame* frame;
    for (frame = thread->top_frame; frame; frame = frame->parent)
        filc_origin_dump_all_inline_default(frame->origin, stream);
}

PAS_NEVER_INLINE PAS_NO_RETURN static void panic_impl(
    const filc_origin* origin, const char* prefix, const char* kind_string, const char* format,
    va_list args)
{
    fix_origin(origin);
    pas_log("%s: ", prefix);
    pas_vlog(format, args);
    pas_log("\n");
    filc_thread_dump_stack(filc_get_my_thread(), pas_log_stream);
    finish_panicking(kind_string);
}

PAS_NEVER_INLINE PAS_NO_RETURN void filc_safety_panic(
    const filc_origin* origin, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    panic_impl(
        origin, "filc safety error", "thwarted a futile attempt to violate memory safety.",
        format, args);
}

PAS_NEVER_INLINE PAS_NO_RETURN void filc_internal_panic(
    const filc_origin* origin, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    panic_impl(
        origin, "filc internal error", "internal Fil-C error (it's probably a bug).",
        format, args);
}

PAS_NEVER_INLINE PAS_NO_RETURN void filc_user_panic(
    const filc_origin* origin, const char* format, ...)
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

bool filc_native_zgc_is_stw(filc_thread* my_thread)
{
    PAS_UNUSED_PARAM(my_thread);
    return fugc_is_stw();
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

void filc_native_zdump_stack(filc_thread* my_thread)
{
    filc_thread_dump_stack(my_thread, pas_log_stream);
}

struct stack_frame_description {
    const char* function_name;
    const char* filename;
    unsigned line;
    unsigned column;
    bool can_throw;
    bool can_catch;
    bool is_inline;
    const char* personality_function;
    const char* eh_data;
};

typedef struct stack_frame_description stack_frame_description;

static void check_stack_frame_description(
    filc_ptr stack_frame_description_ptr, filc_access_kind access_kind)
{
    filc_check_access(
        stack_frame_description_ptr, sizeof(struct stack_frame_description), access_kind);
}

void filc_native_zstack_scan(filc_thread* my_thread, filc_ptr callback_ptr, filc_ptr arg_ptr)
{
    filc_check_function_call(callback_ptr);
    pizlonated_function callback = (pizlonated_function)filc_ptr_ptr(callback_ptr);

    filc_frame* my_frame = my_thread->top_frame;
    PAS_ASSERT(my_frame->origin);
    PAS_ASSERT(!strcmp(
                   filc_origin_node_as_function_origin(my_frame->origin->origin_node)->base.function,
                   "zstack_scan"));
    PAS_ASSERT(!strcmp(
                   filc_origin_node_as_function_origin(my_frame->origin->origin_node)->base.filename,
                   "<runtime>"));
    PAS_ASSERT(my_frame->parent);

    filc_frame* first_frame = my_frame->parent;
    filc_frame* current_frame;
    for (current_frame = first_frame; current_frame; current_frame = current_frame->parent) {
        const filc_origin* outer_origin = current_frame->origin;
        PAS_ASSERT(outer_origin);
        for (const filc_origin* origin = outer_origin;
             origin;
             origin = filc_origin_next_inline(origin)) {
            bool is_inline = filc_origin_node_is_inline_frame(origin->origin_node);
            filc_ptr description_ptr = filc_ptr_create_with_object(
                my_thread, filc_allocate(my_thread, sizeof(stack_frame_description)));
            check_stack_frame_description(description_ptr, filc_write_access);
            stack_frame_description* description =
                (stack_frame_description*)filc_ptr_ptr(description_ptr);
            filc_store_ptr_at(my_thread, description_ptr, &description->function_name,
                              filc_strdup(my_thread, origin->origin_node->function));
            filc_store_ptr_at(my_thread, description_ptr, &description->filename,
                              filc_strdup(my_thread, origin->origin_node->filename));
            description->line = origin->line;
            description->column = origin->column;
            description->is_inline = is_inline;
            if (is_inline) {
                description->can_throw = false;
                description->can_catch = false;
                filc_store_ptr_at(my_thread, description_ptr, &description->personality_function,
                                  filc_ptr_forge_null());
                filc_store_ptr_at(my_thread, description_ptr, &description->eh_data,
                                  filc_ptr_forge_null());
            } else {
                const filc_function_origin* function_origin =
                    filc_origin_node_as_function_origin(origin->origin_node);
                description->can_throw = function_origin->can_throw;
                description->can_catch = function_origin->can_catch;
                filc_ptr personality_function;
                bool has_personality;
                if (function_origin->personality_getter) {
                    has_personality = true;
                    personality_function = function_origin->personality_getter(NULL);
                } else {
                    has_personality = false;
                    personality_function = filc_ptr_forge_null();
                }
                filc_store_ptr_at(my_thread, description_ptr, &description->personality_function,
                                  personality_function);
                filc_ptr eh_data;
                filc_origin_with_eh* origin_with_eh = (filc_origin_with_eh*)outer_origin;
                if (has_personality && origin_with_eh->eh_data_getter)
                    eh_data = origin_with_eh->eh_data_getter(NULL);
                else
                    eh_data = filc_ptr_forge_null();
                filc_store_ptr_at(my_thread, description_ptr, &description->eh_data, eh_data);
            }

            if (!filc_call_user_bool_ptr_ptr(my_thread, callback, description_ptr, arg_ptr))
                return;
        }
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
    const void* language_specific_data;
    void* registers[FILC_NUM_UNWIND_REGISTERS];
};

typedef struct unwind_context unwind_context;

static void check_unwind_context(filc_ptr unwind_context_ptr, filc_access_kind access_kind)
{
    filc_check_access(unwind_context_ptr, sizeof(unwind_context), access_kind);
}

typedef unsigned long long unwind_exception_class;

struct unwind_exception {
    unwind_exception_class exception_class;
    pizlonated_function exception_cleanup;
};

typedef struct unwind_exception unwind_exception;

static void check_unwind_exception(filc_ptr unwind_exception_ptr, filc_access_kind access_kind)
{
    filc_check_access(unwind_exception_ptr, sizeof(unwind_exception), access_kind);
}

static unwind_reason_code call_personality(
    filc_thread* my_thread, filc_frame* current_frame, int version, unwind_action actions,
    filc_ptr exception_object_ptr, filc_ptr context_ptr)
{
    check_unwind_context(context_ptr, filc_write_access);
    unwind_context* context = (unwind_context*)filc_ptr_ptr(context_ptr);

    filc_origin_with_eh* origin_with_eh = (filc_origin_with_eh*)current_frame->origin;
    pizlonated_linker_stub eh_data_getter = origin_with_eh->eh_data_getter;
    filc_ptr eh_data;
    if (eh_data_getter)
        eh_data = eh_data_getter(NULL);
    else
        eh_data = filc_ptr_forge_null();
    filc_store_ptr_at(my_thread, context_ptr, &context->language_specific_data, eh_data);
    
    check_unwind_exception(exception_object_ptr, filc_read_access);
    unwind_exception* exception_object = (unwind_exception*)filc_ptr_ptr(exception_object_ptr);
    unwind_exception_class exception_class = exception_object->exception_class;

    filc_ptr personality_ptr =
        filc_origin_get_function_origin(current_frame->origin)->personality_getter(NULL);
    filc_thread_track_object(my_thread, filc_ptr_object(personality_ptr));
    filc_check_function_call(personality_ptr);

    return (unwind_reason_code)filc_call_user_eh_personality(
        my_thread, (pizlonated_function)filc_ptr_ptr(personality_ptr),
        version, actions, exception_class, exception_object_ptr, context_ptr);
}

filc_exception_and_int filc_native__Unwind_RaiseException(
    filc_thread* my_thread, filc_ptr exception_object_ptr)
{
    filc_ptr context_ptr = filc_ptr_create_with_object(
        my_thread, filc_allocate(my_thread, sizeof(unwind_context)));

    filc_frame* my_frame = my_thread->top_frame;
    PAS_ASSERT(my_frame->origin);
    PAS_ASSERT(!strcmp(
                   filc_origin_node_as_function_origin(my_frame->origin->origin_node)->base.function,
                   "_Unwind_RaiseException"));
    PAS_ASSERT(!strcmp(
                   filc_origin_node_as_function_origin(my_frame->origin->origin_node)->base.filename,
                   "<runtime>"));
    PAS_ASSERT(my_frame->parent);

    filc_frame* first_frame = my_frame->parent;
    filc_frame* current_frame;

    /* Phase 1 */
    for (current_frame = first_frame; current_frame; current_frame = current_frame->parent) {
        PAS_ASSERT(current_frame->origin);
        const filc_function_origin* function_origin =
            filc_origin_get_function_origin(current_frame->origin);
        
        if (!function_origin->can_catch)
            return filc_exception_and_int_with_int(unwind_reason_fatal_phase1_error);

        if (!function_origin->personality_getter) {
            if (!function_origin->can_throw)
                return filc_exception_and_int_with_int(unwind_reason_fatal_phase1_error);
            continue;
        }

        unwind_reason_code personality_result = call_personality(
            my_thread, current_frame, 1, unwind_action_search_phase, exception_object_ptr,
            context_ptr);
        if (personality_result == unwind_reason_handler_found) {
            my_thread->found_frame_for_unwind = current_frame;
            filc_flight_ptr_store(my_thread, &my_thread->unwind_context_ptr, context_ptr);
            filc_flight_ptr_store(my_thread, &my_thread->exception_object_ptr, exception_object_ptr);
            /* This triggers phase 2. */
            return filc_exception_and_int_with_exception();
        }

        if (personality_result == unwind_reason_continue_unwind && function_origin->can_throw)
            continue;

        return filc_exception_and_int_with_int(unwind_reason_fatal_phase1_error);
    }

    return filc_exception_and_int_with_int(unwind_reason_end_of_stack);
}

static bool landing_pad_impl(filc_thread* my_thread, filc_ptr context_ptr,
                             filc_ptr exception_object_ptr, filc_frame* found_frame,
                             filc_frame* current_frame)
{
    static const bool verbose = false;
    
    /* Middle of Phase 2 */
    
    PAS_ASSERT(current_frame->origin);

    /* If the frame didn't support catching, then we wouldn't have gotten here. Only frames that
       support unwinding call landing_pads. */
    const filc_function_origin* function_origin =
        filc_origin_get_function_origin(current_frame->origin);
    PAS_ASSERT(function_origin->can_catch);
    
    if (!function_origin->personality_getter)
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
        my_thread->unwind_registers[index] =
            filc_load_ptr_at(my_thread, context_ptr, context->registers + index);
        if (verbose) {
            pas_log("Unwind register %u: ", index);
            filc_ptr_dump(my_thread->unwind_registers[index], pas_log_stream);
            pas_log("\n");
        }
    }
    return true;
}

bool filc_landing_pad(filc_thread* my_thread)
{
    filc_frame* current_frame = my_thread->top_frame;
    PAS_ASSERT(current_frame);

    FILC_DEFINE_FRAME("landing_pad");
    filc_push_frame(my_thread, frame);
    PAS_ASSERT(current_frame == frame->parent);

    filc_native_frame native_frame;
    filc_push_native_frame(my_thread, &native_frame);

    filc_ptr context_ptr = filc_flight_ptr_load(my_thread, &my_thread->unwind_context_ptr);
    filc_ptr exception_object_ptr = filc_flight_ptr_load(my_thread, &my_thread->exception_object_ptr);
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
        const filc_function_origin* function_origin =
            filc_origin_get_function_origin(current_frame->origin);
        PAS_ASSERT(function_origin->can_catch);
        FILC_CHECK(
            function_origin->can_throw,
            NULL,
            "cannot unwind from landing pad, function claims not to throw.");
        FILC_CHECK(
            current_frame->parent,
            NULL,
            "cannot unwind from landing pad, reached end of stack.");
        FILC_CHECK(
            filc_origin_get_function_origin(current_frame->parent->origin)->can_catch,
            NULL,
            "cannot unwind from landing pad, parent frame doesn't support catching.");
    }

    filc_pop_native_frame(my_thread, &native_frame);
    filc_pop_frame(my_thread, frame);

    return result;
}

void filc_resume_unwind(filc_thread* my_thread, const filc_origin *origin)
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
        filc_origin_get_function_origin(current_frame->origin)->can_throw,
        NULL,
        "cannot resume unwinding, current frame claims not to throw.");
    FILC_CHECK(
        current_frame->parent,
        NULL,
        "cannot resume unwinding, reached end of stack.");
    FILC_CHECK(
        filc_origin_get_function_origin(current_frame->parent->origin)->can_catch,
        NULL,
        "cannot resume unwinding, parent frame doesn't support catching.");
}

static bool setjmp_saves_sigmask = false;

filc_jmp_buf* filc_jmp_buf_create(filc_thread* my_thread, filc_jmp_buf_kind kind, int value)
{
    PAS_ASSERT(kind == filc_jmp_buf_setjmp ||
               kind == filc_jmp_buf__setjmp ||
               kind == filc_jmp_buf_sigsetjmp);

    filc_frame* frame = my_thread->top_frame;
    PAS_ASSERT(frame->origin);
    const filc_function_origin* function_origin = filc_origin_get_function_origin(frame->origin);
    PAS_ASSERT(function_origin);
    PAS_ASSERT(function_origin->base.num_lowers_ish < UINT_MAX);
    
    filc_jmp_buf* result = (filc_jmp_buf*)filc_object_special_payload_with_manual_tracking(
        filc_allocate_special(my_thread,
                              PAS_OFFSETOF(filc_jmp_buf, lowers)
                              + filc_mul_size(function_origin->base.num_lowers_ish, sizeof(void*)),
                              1,
                              FILC_SPECIAL_TYPE_JMP_BUF));

    result->kind = kind; /* We save the kind because it lets us do a safety check, but that check isn't
                            needed for memory safety, so we could drop it. */
    result->saved_top_frame = frame;
    /* NOTE: We could possibly do more stuff to track the state of the top native frame, but we don't,
       because frames that create native frames don't setjmp. Basically, native code doesn't setjmp. */
    filc_native_frame_assert_locked(my_thread->top_native_frame);
    result->saved_top_native_frame = my_thread->top_native_frame;
    result->saved_allocation_roots_size = my_thread->allocation_roots.size;
    result->num_lowers = function_origin->base.num_lowers_ish;
    size_t index;
    for (index = function_origin->base.num_lowers_ish; index--;) {
        filc_store_barrier(my_thread, filc_object_for_lower(frame->lowers[index]));
        result->lowers[index] = frame->lowers[index];
    }

    PAS_ASSERT(
        result->num_lowers ==
        filc_origin_get_function_origin(result->saved_top_frame->origin)->base.num_lowers_ish);

    if ((kind == filc_jmp_buf_sigsetjmp && value) ||
        (kind == filc_jmp_buf_setjmp && setjmp_saves_sigmask)) {
        PAS_ASSERT(!pthread_sigmask(0, NULL, &result->sigmask));
        result->did_save_sigmask = true;
    }

    return result;
}

void filc_jmp_buf_mark_outgoing_ptrs(filc_jmp_buf* jmp_buf, filc_object_array* stack)
{
    size_t index;
    for (index = jmp_buf->num_lowers; index--;)
        fugc_mark(stack, filc_object_for_lower(jmp_buf->lowers[index]));
}

static void longjmp_impl(filc_thread* my_thread, filc_ptr jmp_buf_ptr, int value,
                         filc_jmp_buf_kind kind)
{
    PAS_ASSERT(kind == filc_jmp_buf_setjmp ||
               kind == filc_jmp_buf__setjmp ||
               kind == filc_jmp_buf_sigsetjmp);
    
    filc_check_access_special(jmp_buf_ptr, FILC_SPECIAL_TYPE_JMP_BUF);
    filc_jmp_buf* jmp_buf = (filc_jmp_buf*)filc_ptr_ptr(jmp_buf_ptr);

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
        const filc_function_origin* function_origin =
            filc_origin_get_function_origin(current_frame->origin);
        PAS_ASSERT(function_origin);
        PAS_ASSERT(function_origin->base.num_lowers_ish < UINT_MAX);
        PAS_ASSERT(function_origin->num_setjmps <= function_origin->base.num_lowers_ish);
        unsigned index;
        for (index = function_origin->num_setjmps; index-- && !found_frame;) {
            unsigned lower_index = function_origin->base.num_lowers_ish - 1 - index;
            PAS_ASSERT(lower_index < function_origin->base.num_lowers_ish);
            if (jmp_buf == current_frame->lowers[lower_index]) {
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
    my_thread->allocation_roots.size = jmp_buf->saved_allocation_roots_size;

    PAS_ASSERT(my_thread->top_frame == jmp_buf->saved_top_frame);
    PAS_ASSERT(filc_origin_get_function_origin(my_thread->top_frame->origin)->base.num_lowers_ish
               == jmp_buf->num_lowers);
    size_t index;
    for (index = jmp_buf->num_lowers; index--;)
        my_thread->top_frame->lowers[index] = jmp_buf->lowers[index];

    if (jmp_buf->did_save_sigmask)
        PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &jmp_buf->sigmask, NULL));
    
    _longjmp(jmp_buf->system_buf, value);
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

void filc_native_zmake_setjmp_save_sigmask(filc_thread* my_thread, bool save_sigmask)
{
    PAS_UNUSED_PARAM(my_thread);
    setjmp_saves_sigmask = save_sigmask;
}

static void cpuid_impl(unsigned leaf, unsigned count, filc_ptr eax_ptr, filc_ptr ebx_ptr,
                       filc_ptr ecx_ptr, filc_ptr edx_ptr)
{
    unsigned eax = leaf;
    unsigned ebx = 0;
    unsigned ecx = count;
    unsigned edx = 0;
    asm volatile("cpuid"
                 : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx));
    filc_check_write(eax_ptr, sizeof(int32_t));
    filc_check_write(ebx_ptr, sizeof(int32_t));
    filc_check_write(ecx_ptr, sizeof(int32_t));
    filc_check_write(edx_ptr, sizeof(int32_t));
    *(unsigned*)filc_ptr_ptr(eax_ptr) = eax;
    *(unsigned*)filc_ptr_ptr(ebx_ptr) = ebx;
    *(unsigned*)filc_ptr_ptr(ecx_ptr) = ecx;
    *(unsigned*)filc_ptr_ptr(edx_ptr) = edx;
}

void filc_native_zcpuid(filc_thread* my_thread, unsigned leaf, filc_ptr eax_ptr, filc_ptr ebx_ptr,
                        filc_ptr ecx_ptr, filc_ptr edx_ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    cpuid_impl(leaf, 0, eax_ptr, ebx_ptr, ecx_ptr, edx_ptr);
}

void filc_native_zcpuid_count(filc_thread* my_thread, unsigned leaf, unsigned count, filc_ptr eax_ptr,
                              filc_ptr ebx_ptr, filc_ptr ecx_ptr, filc_ptr edx_ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    cpuid_impl(leaf, count, eax_ptr, ebx_ptr, ecx_ptr, edx_ptr);
}

unsigned long filc_native_zxgetbv(filc_thread* my_thread)
{
    PAS_UNUSED_PARAM(my_thread);
    unsigned low;
    unsigned high;
    asm volatile("xgetbv"
                 : "=a"(low), "=d"(high)
                 : "c"(0));
    return (unsigned long)low | ((unsigned long)high << (unsigned long)32);
}

static pizlonated_function pizlonated_errno_handler;

void filc_native_zregister_sys_errno_handler(filc_thread* my_thread, filc_ptr errno_handler)
{
    PAS_UNUSED_PARAM(my_thread);
    FILC_CHECK(
        !pizlonated_errno_handler,
        NULL,
        "errno handler already registered.");
    filc_check_function_call(errno_handler);
    pizlonated_errno_handler = (pizlonated_function)filc_ptr_ptr(errno_handler);
}

static pizlonated_function pizlonated_dlerror_handler;

void filc_native_zregister_sys_dlerror_handler(filc_thread* my_thread, filc_ptr dlerror_handler)
{
    PAS_UNUSED_PARAM(my_thread);
    FILC_CHECK(
        !pizlonated_dlerror_handler,
        NULL,
        "dlerror handler already registered.");
    filc_check_function_call(dlerror_handler);
    pizlonated_dlerror_handler = (pizlonated_function)filc_ptr_ptr(dlerror_handler);
}

void filc_set_user_errno(int errno_value)
{
    FILC_CHECK(
        pizlonated_errno_handler,
        NULL,
        "errno handler not registered when trying to set errno = %d.", errno_value);
    filc_thread* my_thread = filc_get_my_thread();
    filc_call_user_void_int(my_thread, pizlonated_errno_handler, errno_value);
}

void filc_set_errno(int errno_value)
{
    int user_errno = filc_to_user_errno(errno_value);
    if (dump_errnos) {
        pas_log("[%d] Setting errno! System errno = %d, user errno = %d, system error = %s\n",
                getpid(), errno_value, user_errno, strerror(errno_value));
        filc_thread_dump_stack(filc_get_my_thread(), pas_log_stream);
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
    filc_call_user_void_ptr(my_thread, pizlonated_dlerror_handler, filc_strdup(my_thread, error));
}

void filc_extract_user_iovec_entry(filc_thread* my_thread, filc_ptr user_iov_entry_ptr,
                                   filc_ptr* user_iov_base, size_t* iov_len)
{
    filc_check_read(user_iov_entry_ptr, sizeof(struct iovec));
    struct iovec* user_iov_entry = (struct iovec*)filc_ptr_ptr(user_iov_entry_ptr);
    *user_iov_base = filc_load_ptr_at(my_thread, user_iov_entry_ptr, &user_iov_entry->iov_base);
    *iov_len = user_iov_entry->iov_len;
}

void filc_prepare_iovec_entry(filc_thread* my_thread, filc_ptr user_iov_entry_ptr,
                              struct iovec* iov_entry, filc_access_kind access_kind)
{
    filc_ptr user_iov_base;
    size_t iov_len;
    filc_extract_user_iovec_entry(my_thread, user_iov_entry_ptr, &user_iov_base, &iov_len);
    filc_check_access(user_iov_base, iov_len, access_kind);
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
            my_thread, filc_ptr_with_offset(user_iov, filc_mul_size(sizeof(struct iovec), index)),
            iov + index, access_kind);
    }
    return iov;
}

static bool is_reserved_fd(int fd)
{
    if (file_log_fd < 0)
        return false;
    return fd == file_log_fd;
}

static void check_fd(int fd)
{
    FILC_ASSERT(!is_reserved_fd(fd), NULL);
}

ssize_t filc_native_zsys_writev(filc_thread* my_thread, int fd, filc_ptr user_iov, int iovcnt)
{
    check_fd(fd);
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
    check_fd(fd);
    filc_check_write(buf, size);
    return FILC_SYSCALL(my_thread, read(fd, filc_ptr_ptr(buf), size));
}

ssize_t filc_native_zsys_readv(filc_thread* my_thread, int fd, filc_ptr user_iov, int iovcnt)
{
    check_fd(fd);
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
    check_fd(fd);
    filc_check_read(buf, size);
    return FILC_SYSCALL(my_thread, write(fd, filc_ptr_ptr(buf), size));
}

int filc_native_zsys_close(filc_thread* my_thread, int fd)
{
    if (is_reserved_fd(fd))
        return 0;
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
    check_fd(fd);
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
    static const bool verbose = false;
    if (verbose) {
        pas_log("%d: Exiting!\n", getpid());
        filc_thread_dump_stack(my_thread, pas_log_stream);
    }
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

int filc_native_zsys_open(filc_thread* my_thread, filc_ptr path_ptr, int flags,
                          filc_cc_cursor* args)
{
    static const bool verbose = false;
    unsigned mode = 0;
    if (flags >= 0 && (flags & O_CREAT))
        mode = filc_cc_cursor_get_next_unsigned(my_thread, args);
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    if (verbose)
        pas_log("doing an open with path = %s, flags = %d.\n", path, flags);
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

int filc_native_zsys_clock_gettime(filc_thread* my_thread, int clock_id, filc_ptr timespec_ptr)
{
    filc_check_write(timespec_ptr, sizeof(struct timespec));
    return FILC_SYSCALL(
        my_thread, clock_gettime(clock_id, (struct timespec*)filc_ptr_ptr(timespec_ptr)));
}

int filc_native_zsys_fstatat(
    filc_thread* my_thread, int fd, filc_ptr path_ptr, filc_ptr stat_ptr, int flag)
{
    check_fd(fd);
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_check_write(stat_ptr, sizeof(struct stat));
    return FILC_SYSCALL(my_thread, fstatat(fd, path, (struct stat*)filc_ptr_ptr(stat_ptr), flag));
}

int filc_native_zsys_fstat(filc_thread* my_thread, int fd, filc_ptr stat_ptr)
{
    check_fd(fd);
    filc_check_write(stat_ptr, sizeof(struct stat));
    return FILC_SYSCALL(my_thread, fstat(fd, (struct stat*)filc_ptr_ptr(stat_ptr)));
}

static bool from_user_sa_flags(int user_flags, int* flags)
{
    if ((user_flags & SA_SIGINFO))
        return false;
    *flags = user_flags;
    return true;
}

static int to_user_sa_flags(int sa_flags)
{
    return sa_flags;
}

static bool is_unsafe_signal_for_kill(int signum)
{
    /* We don't want userland to be able to raise/kill with signals that are used internally by the
       yolo libc. */
    switch (signum) {
    case SIGTIMER:
    case SIGCANCEL:
    case SIGSYNCCALL:
        return true;
    default:
        return false;
    }
}

static bool is_unsafe_signal_for_handlers(int signum)
{
    switch (signum) {
    case SIGILL:
    case SIGTRAP:
    case SIGBUS:
    case SIGSEGV:
    case SIGFPE:
        return true;
    default:
        return is_unsafe_signal_for_kill(signum);
    }
}

static bool is_special_signal_handler(void* handler)
{
    return handler == SIG_DFL || handler == SIG_IGN;
}

static bool is_user_special_signal_handler(void* handler)
{
    return is_special_signal_handler(handler);
}

static void* from_user_special_signal_handler(void* handler)
{
    PAS_ASSERT(is_user_special_signal_handler(handler));
    return handler;
}

static filc_ptr to_user_special_signal_handler(void* handler)
{
    PAS_ASSERT(is_special_signal_handler(handler));
    return filc_ptr_forge_invalid(handler);
}

int filc_native_zsys_sigaction(
    filc_thread* my_thread, int signum, filc_ptr act_ptr, filc_ptr oact_ptr)
{
    static const bool verbose = false;

    if (verbose)
        pas_log("[%d] sigaction on signum = %d\n", getpid(), signum);
    
    if (is_unsafe_signal_for_handlers(signum) && filc_ptr_ptr(act_ptr)) {
        filc_set_errno(ENOSYS);
        return -1;
    }
    if (filc_ptr_ptr(act_ptr))
        check_user_sigaction(act_ptr, filc_read_access);
    struct sigaction* user_act = (struct sigaction*)filc_ptr_ptr(act_ptr);
    struct sigaction* user_oact = (struct sigaction*)filc_ptr_ptr(oact_ptr);
    struct sigaction act;
    struct sigaction oact;
    if (user_act) {
        filc_from_user_sigset(&user_act->sa_mask, &act.sa_mask);
        filc_ptr user_handler = filc_load_ptr_at(my_thread, act_ptr, &user_act->sa_handler);
        if (is_user_special_signal_handler(filc_ptr_ptr(user_handler)))
            act.sa_handler = from_user_special_signal_handler(filc_ptr_ptr(user_handler));
        else {
            filc_check_function_call(user_handler);
            filc_signal_handler* handler = filc_object_special_payload(
                my_thread, filc_allocate_special(
                    my_thread, sizeof(filc_signal_handler), 1, FILC_SPECIAL_TYPE_SIGNAL_HANDLER));
            filc_flight_ptr_store(my_thread, &handler->function_ptr, user_handler);
            handler->mask = act.sa_mask;
            handler->user_signum = signum;
            pas_store_store_fence();
            PAS_ASSERT((unsigned)signum <= FILC_MAX_USER_SIGNUM);
            filc_store_barrier(my_thread, filc_object_for_special_payload(handler));
            signal_table[signum] = handler;
            act.sa_handler = signal_pizlonator;
        }
        if (!from_user_sa_flags(user_act->sa_flags, &act.sa_flags)) {
            if (verbose)
                pas_log("Bad sa_flags\n");
            filc_set_errno(EINVAL);
            return -1;
        }
        if (verbose)
            pas_log("restart = %s\n", (act.sa_flags & SA_RESTART) ? "yes" : "no");
    }
    if (user_oact)
        pas_zero_memory(&oact, sizeof(struct sigaction));
    filc_exit(my_thread);
    int result = sigaction(signum, user_act ? &act : NULL, user_oact ? &oact : NULL);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0) {
        if (verbose)
            pas_log("Got an actual errno from sigaction.\n");
        filc_set_errno(my_errno);
        return -1;
    }
    if (user_oact) {
        check_user_sigaction(oact_ptr, filc_write_access);
        if (is_unsafe_signal_for_handlers(signum))
            PAS_ASSERT(oact.sa_handler == SIG_DFL);
        if (is_special_signal_handler(oact.sa_handler)) {
            filc_store_ptr_at(my_thread, oact_ptr, &user_oact->sa_handler,
                              to_user_special_signal_handler(oact.sa_handler));
        } else {
            PAS_ASSERT(oact.sa_handler == signal_pizlonator);
            PAS_ASSERT((unsigned)signum <= FILC_MAX_USER_SIGNUM);
            /* FIXME: The signal_table entry should really be a filc_ptr so we can return it here. */
            filc_store_ptr_at(my_thread, oact_ptr, &user_oact->sa_handler,
                              filc_flight_ptr_load(
                                  my_thread, &signal_table[signum]->function_ptr));
        }
        filc_to_user_sigset(&oact.sa_mask, &user_oact->sa_mask);
        user_oact->sa_flags = to_user_sa_flags(oact.sa_flags);
    }
    return 0;
}

int filc_native_zsys_pipe(filc_thread* my_thread, filc_ptr fds_ptr)
{
    filc_check_write(fds_ptr, sizeof(int) * 2);
    return FILC_SYSCALL(my_thread, pipe((int*)filc_ptr_ptr(fds_ptr)));
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
        filc_check_write(readfds_ptr, sizeof(fd_set));
    if (filc_ptr_ptr(writefds_ptr))
        filc_check_write(writefds_ptr, sizeof(fd_set));
    if (filc_ptr_ptr(exceptfds_ptr))
        filc_check_write(exceptfds_ptr, sizeof(fd_set));
    if (filc_ptr_ptr(timeout_ptr))
        filc_check_write(timeout_ptr, sizeof(struct timeval));
    fd_set* readfds = (fd_set*)filc_ptr_ptr(readfds_ptr);
    fd_set* writefds = (fd_set*)filc_ptr_ptr(writefds_ptr);
    fd_set* exceptfds = (fd_set*)filc_ptr_ptr(exceptfds_ptr);
    struct timeval* timeout = (struct timeval*)filc_ptr_ptr(timeout_ptr);
    return FILC_SYSCALL(my_thread, select(nfds, readfds, writefds, exceptfds, timeout));
}

void filc_native_zsys_sched_yield(filc_thread* my_thread)
{
    filc_exit(my_thread);
    sched_yield();
    filc_enter(my_thread);
}

int filc_native_zsys_getrlimit(filc_thread* my_thread, int resource, filc_ptr rlim_ptr)
{
    filc_check_write(rlim_ptr, sizeof(struct rlimit));
    return FILC_SYSCALL(my_thread, getrlimit(resource, (struct rlimit*)filc_ptr_ptr(rlim_ptr)));
}

unsigned filc_native_zsys_umask(filc_thread* my_thread, unsigned mask)
{
    filc_exit(my_thread);
    unsigned result = umask(mask);
    filc_enter(my_thread);
    return result;
}

int filc_native_zsys_getitimer(filc_thread* my_thread, int which, filc_ptr value_ptr)
{
    filc_check_write(value_ptr, sizeof(struct itimerval));
    return FILC_SYSCALL(my_thread, getitimer(which, (struct itimerval*)filc_ptr_ptr(value_ptr)));
}

int filc_native_zsys_setitimer(filc_thread* my_thread, int which, filc_ptr new_value_ptr,
                               filc_ptr old_value_ptr)
{
    if (filc_ptr_ptr(new_value_ptr))
        filc_check_read(new_value_ptr, sizeof(struct itimerval));
    if (filc_ptr_ptr(old_value_ptr))
        filc_check_write(old_value_ptr, sizeof(struct itimerval));
    return FILC_SYSCALL(my_thread, setitimer(which,
                                             (const struct itimerval*)filc_ptr_ptr(new_value_ptr),
                                             (struct itimerval*)filc_ptr_ptr(old_value_ptr)));
}

int filc_native_zsys_pause(filc_thread* my_thread)
{
    static const bool verbose = false;
    if (verbose) {
        pas_log("[%d] pausing!\n", getpid());
        dump_signals_mask();
        filc_thread_dump_stack(my_thread, pas_log_stream);

        struct sigaction sa;
        PAS_ASSERT(!sigaction(SIGCHLD, NULL, &sa));
        pas_log("SIGCHLD restart = %s\n", (sa.sa_flags & SA_RESTART) ? "yes" : "no");
        pas_log("SIGCHLD flags = 0x%x\n", sa.sa_flags);
    }
    filc_exit(my_thread);
    int result = pause();
    int my_errno = errno;
    filc_enter(my_thread);
    if (verbose)
        pas_log("[%d] pause returned.\n", getpid());
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
        filc_check_write(readfds_ptr, sizeof(fd_set));
    if (filc_ptr_ptr(writefds_ptr))
        filc_check_write(writefds_ptr, sizeof(fd_set));
    if (filc_ptr_ptr(exceptfds_ptr))
        filc_check_write(exceptfds_ptr, sizeof(fd_set));
    if (filc_ptr_ptr(timeout_ptr))
        filc_check_read(timeout_ptr, sizeof(struct timespec));
    if (filc_ptr_ptr(sigmask_ptr))
        filc_check_user_sigset(sigmask_ptr, filc_read_access);
    fd_set* readfds = (fd_set*)filc_ptr_ptr(readfds_ptr);
    fd_set* writefds = (fd_set*)filc_ptr_ptr(writefds_ptr);
    fd_set* exceptfds = (fd_set*)filc_ptr_ptr(exceptfds_ptr);
    struct timespec* timeout = (struct timespec*)filc_ptr_ptr(timeout_ptr);
    sigset_t* user_sigmask = (sigset_t*)filc_ptr_ptr(sigmask_ptr);
    sigset_t sigmask;
    if (user_sigmask)
        filc_from_user_sigset(user_sigmask, &sigmask);
    return FILC_SYSCALL(my_thread, pselect(nfds, readfds, writefds, exceptfds, timeout,
                                           user_sigmask ? &sigmask : NULL));
}

int filc_native_zsys_kill(filc_thread* my_thread, int pid, int sig)
{
    if (is_unsafe_signal_for_kill(sig)) {
        filc_set_errno(ENOSYS);
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

int filc_native_zsys_raise(filc_thread* my_thread, int sig)
{
    if (is_unsafe_signal_for_kill(sig)) {
        filc_set_errno(ENOSYS);
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
    check_fd(fd);
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
    check_fd(oldfd);
    check_fd(newfd);
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
    how = user_how;
    sigset_t* set;
    sigset_t* oldset;
    if (filc_ptr_ptr(user_set_ptr)) {
        filc_check_user_sigset(user_set_ptr, filc_read_access);
        set = alloca(sizeof(sigset_t));
        filc_from_user_sigset((sigset_t*)filc_ptr_ptr(user_set_ptr), set);
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
        filc_to_user_sigset(oldset, (sigset_t*)filc_ptr_ptr(user_oldset_ptr));
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
        open_new_log_file_if_necessary();
        /* We're in the child. Make sure that the thread list only contains the current thread and that
           the other threads know that they are dead due to fork. */
        for (thread = filc_first_thread; thread;) {
            filc_thread* next_thread = thread->next_thread;
            thread->prev_thread = NULL;
            thread->next_thread = NULL;
            if (thread != my_thread) {
                if (!thread->is_stopping)
                    stop_thread_allocators(thread);

                /* We can inspect the thread's TLC without any locks, since the thread is dead and
                   stopped. Also, start_thread (and other parts of the runtime) ensure that we only
                   call into libpas while entered - so the fact that we stop the world before
                   forking ensures that the dead thread is definitely not in the middle of a call
                   into libpas. */
                if (thread->tlc_node && thread->tlc_node->version == thread->tlc_node_version)
                    pas_thread_local_cache_destroy_remote_from_node(thread->tlc_node->cache);

                thread->forked = true;
                thread->tid = 0;
                thread->has_set_tid = true; /* Prevent anyone from waiting for this thread to set its
                                               tid. */
                thread->thread = PAS_NULL_SYSTEM_THREAD_ID;
            }
            pas_system_mutex_unlock(&thread->lock);
            thread = next_thread;
        }
        PAS_ASSERT(my_thread->has_set_tid);
        my_thread->tid = gettid(); /* The child now has a different tid than before. */
        filc_first_thread = my_thread;
        PAS_ASSERT(filc_first_thread == my_thread);
        PAS_ASSERT(!filc_first_thread->next_thread);
        PAS_ASSERT(!filc_first_thread->prev_thread);
    } else {
        for (thread = filc_first_thread; thread; thread = thread->next_thread)
            pas_system_mutex_unlock(&thread->lock);
    }
    filc_thread_list_lock_unlock();
    filc_resume_the_world();
    fugc_resume();
    pas_scavenger_resume();
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_waitpid(filc_thread* my_thread, int pid, filc_ptr status_ptr, int options)
{
    if (filc_ptr_ptr(status_ptr))
        filc_check_write(status_ptr, sizeof(int));
    return FILC_SYSCALL(my_thread, waitpid(pid, (int*)filc_ptr_ptr(status_ptr), options));
}

int filc_native_zsys_listen(filc_thread* my_thread, int sockfd, int backlog)
{
    check_fd(sockfd);
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
        if (!filc_ptr_ptr(filc_load_ptr_with_manual_tracking(array_ptr, 0)))
            return result;
        array_ptr = filc_ptr_with_offset(array_ptr, sizeof(void*));
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
            my_thread, filc_load_ptr(my_thread, user_array_ptr, index * sizeof(void*)));
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

int filc_native_zsys_nanosleep(filc_thread* my_thread, filc_ptr req_ptr, filc_ptr rem_ptr)
{
    if (filc_ptr_ptr(req_ptr))
        filc_check_read(req_ptr, sizeof(struct timespec));
    if (filc_ptr_ptr(rem_ptr))
        filc_check_write(rem_ptr, sizeof(struct timespec));
    return FILC_SYSCALL(my_thread, nanosleep((const struct timespec*)filc_ptr_ptr(req_ptr),
                                             (struct timespec*)filc_ptr_ptr(rem_ptr)));
}

long filc_native_zsys_readlink(filc_thread* my_thread, filc_ptr path_ptr, filc_ptr buf_ptr,
                               size_t bufsize)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_check_write(buf_ptr, bufsize);
    filc_exit(my_thread);
    long result = readlink(path, (char*)filc_ptr_ptr(buf_ptr), bufsize);
    int my_errno = errno;
    filc_enter(my_thread);
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
    if ((user_prot & PROT_EXEC))
        return false;
    *prot = user_prot;
    return true;
}

static bool from_user_mmap_flags(int user_flags, int* flags)
{
    if ((user_flags & MAP_FIXED))
        return false;
    *flags = user_flags;
    return true;
}

static filc_ptr mmap_error_result(void)
{
    return filc_ptr_forge_invalid((void*)(intptr_t)-1);
}

filc_ptr filc_native_zsys_mmap(filc_thread* my_thread, filc_ptr address, size_t length, int prot,
                               int flags, int fd, long offset)
{
    check_fd(fd);
    static const bool verbose = false;
    if (verbose) {
        pas_log("doing an mmap with address = %p, length = %zu, prot = %d, flags = %d, fd = %d, "
                "offset = %ld.\n", filc_ptr_ptr(address), length, prot, flags, fd, offset);
        filc_thread_dump_stack(my_thread, pas_log_stream);
    }
    length = pas_round_up_to_power_of_2(length, pas_page_malloc_alignment());
    if (filc_ptr_ptr(address)) {
        filc_check_write(address, length);
        check_mmap(address);
        if (!(flags & MAP_FIXED)) {
            filc_set_errno(EINVAL);
            return mmap_error_result();
        }
    } else if ((flags & MAP_FIXED)) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }
    if ((prot & PROT_EXEC)) {
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
    if (!filc_ptr_ptr(address)) {
        address = filc_ptr_create_with_object(
            my_thread, allocate_aligned_impl(
                my_thread, length, pas_page_malloc_alignment(), FILC_OBJECT_FLAG_MMAP));
        flags |= MAP_FIXED;
    }
    if (verbose) {
        pas_log("mapping %p...%p (%zu bytes).\n",
                filc_ptr_ptr(address), (char*)filc_ptr_ptr(address) + length, length);
    }
    filc_exit(my_thread);
    void* raw_result = mmap(filc_ptr_ptr(address), length, prot, flags, fd, offset);
    int my_errno = errno;
    filc_enter(my_thread);
    if (raw_result == (void*)(intptr_t)-1) {
        filc_set_errno(my_errno);
        return mmap_error_result();
    }
    filc_check_write(address, length); /* Make sure someone didn't munmap concurrently. */
    PAS_ASSERT(raw_result);
    PAS_ASSERT(raw_result == filc_ptr_ptr(address));
    return address;
}

void filc_unmap(void* ptr, size_t size)
{
    PAS_ASSERT(pas_is_aligned((uintptr_t)ptr, pas_page_malloc_alignment()));
    PAS_ASSERT(pas_is_aligned(size, pas_page_malloc_alignment()));
    if (!size)
        return;
    void* result_ptr = mmap(ptr, size,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON | MAP_FIXED,
                            -1, 0);
    PAS_ASSERT(result_ptr == ptr);
}

int filc_native_zsys_munmap(filc_thread* my_thread, filc_ptr address, size_t length)
{
    static const bool verbose = false;
    if (!length)
        return 0;
    length = pas_round_up_to_power_of_2(length, pas_page_malloc_alignment());
    filc_check_write(address, length);
    check_mmap(address);
    if (!pas_is_aligned((uintptr_t)filc_ptr_ptr(address), pas_page_malloc_alignment())) {
        filc_set_errno(EINVAL);
        return -1;
    }
    if (verbose) {
        pas_log("unmapping %p...%p (%zu bytes).\n",
                filc_ptr_ptr(address), (char*)filc_ptr_ptr(address) + length, length);
    }
    if (filc_ptr_ptr(address) == filc_ptr_lower(address) &&
        length == filc_ptr_available(address))
        filc_free(filc_ptr_object(address));
    filc_exit(my_thread);
    filc_unmap(filc_ptr_ptr(address), length);
    filc_enter(my_thread);
    return 0;
}

int filc_native_zsys_ftruncate(filc_thread* my_thread, int fd, long length)
{
    check_fd(fd);
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
    filc_check_write(buf_ptr, size);
    filc_exit(my_thread);
    char* result = getcwd((char*)filc_ptr_ptr(buf_ptr), size);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == (char*)filc_ptr_ptr(buf_ptr));
    if (!result)
        filc_set_errno(my_errno);
    if (!result)
        return filc_ptr_forge_null();
    return buf_ptr;
}

static bool from_user_dlopen_flags(int user_flags, int* flags)
{
    *flags = user_flags;
    return true;
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
    return filc_ptr_create_with_special_object_and_manual_tracking(
        filc_allocate_special_with_existing_payload(my_thread, handle, FILC_SPECIAL_TYPE_DL_HANDLE));
}

filc_ptr filc_native_zsys_dlsym(filc_thread* my_thread, filc_ptr handle_ptr, filc_ptr symbol_ptr)
{
    filc_check_access_special(handle_ptr, FILC_SPECIAL_TYPE_DL_HANDLE);
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

int filc_native_zsys_faccessat(filc_thread* my_thread, int dirfd, filc_ptr pathname_ptr, int mode,
                               int flags)
{
    check_fd(dirfd);
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
    filc_from_user_sigset((sigset_t*)filc_ptr_ptr(sigmask_ptr), &sigmask);
    filc_exit(my_thread);
    int signum;
    int result = sigwait(&sigmask, &signum);
    filc_enter(my_thread);
    if (result)
        return result;
    filc_check_write(sig_ptr, sizeof(int));
    *(int*)filc_ptr_ptr(sig_ptr) = signum;
    return 0;
}

int filc_native_zsys_fsync(filc_thread* my_thread, int fd)
{
    check_fd(fd);
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
    check_fd(fd);
    int how;
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

int filc_native_zsys_futimens(filc_thread* my_thread, int fd, filc_ptr times_ptr)
{
    check_fd(fd);
    filc_check_read(times_ptr, sizeof(struct timespec) * 2);
    return FILC_SYSCALL(my_thread, futimens(fd, (const struct timespec*)filc_ptr_ptr(times_ptr)));
}

int filc_native_zsys_utimensat(filc_thread* my_thread, int dirfd, filc_ptr path_ptr,
                               filc_ptr times_ptr, int flags)
{
    check_fd(dirfd);
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    if (filc_ptr_ptr(times_ptr))
        filc_check_read(times_ptr, sizeof(struct timespec) * 2);
    return FILC_SYSCALL(my_thread, utimensat(dirfd, path,
                                             (const struct timespec*)filc_ptr_ptr(times_ptr), flags));
}

int filc_native_zsys_fchown(filc_thread* my_thread, int fd, unsigned uid, unsigned gid)
{
    check_fd(fd);
    return FILC_SYSCALL(my_thread, fchown(fd, uid, gid));
}

int filc_native_zsys_fchownat(filc_thread* my_thread, int fd, filc_ptr pathname_ptr, unsigned uid,
                              unsigned gid, int flags)
{
    check_fd(fd);
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    return FILC_SYSCALL(my_thread, fchownat(fd, pathname, uid, gid, flags));
}

int filc_native_zsys_fchdir(filc_thread* my_thread, int fd)
{
    check_fd(fd);
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
    filc_check_write(addr_ptr, len);
    check_mmap(addr_ptr);
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
    filc_check_write(list_ptr, total_size);
    filc_exit(my_thread);
    PAS_ASSERT(sizeof(gid_t) == sizeof(unsigned));
    int result = getgroups(size, (gid_t*)filc_ptr_ptr(list_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
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
    check_fd(fd);
    filc_check_write(buf_ptr, nbytes);
    return FILC_SYSCALL(my_thread, pread(fd, filc_ptr_ptr(buf_ptr), nbytes, offset));
}

long filc_native_zsys_preadv(filc_thread* my_thread, int fd, filc_ptr user_iov_ptr, int iovcnt,
                             long offset)
{
    check_fd(fd);
    struct iovec* iov = filc_prepare_iovec(my_thread, user_iov_ptr, iovcnt, filc_write_access);
    return FILC_SYSCALL(my_thread, preadv(fd, iov, iovcnt, offset));
}

long filc_native_zsys_pwrite(filc_thread* my_thread, int fd, filc_ptr buf_ptr, size_t nbytes,
                             long offset)
{
    check_fd(fd);
    filc_check_read(buf_ptr, nbytes);
    return FILC_SYSCALL(my_thread, pwrite(fd, filc_ptr_ptr(buf_ptr), nbytes, offset));
}

long filc_native_zsys_pwritev(filc_thread* my_thread, int fd, filc_ptr user_iov_ptr, int iovcnt,
                              long offset)
{
    check_fd(fd);
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
    filc_check_access(addr_ptr, len, filc_read_access);
    check_mmap(addr_ptr);
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

int filc_native_zsys_mlockall(filc_thread* my_thread, int flags)
{
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
    filc_to_user_sigset(&set, (sigset_t*)filc_ptr_ptr(set_ptr));
    return 0;
}

int filc_native_zsys_truncate(filc_thread* my_thread, filc_ptr path_ptr, long length)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    return FILC_SYSCALL(my_thread, truncate(path, length));
}

int filc_native_zsys_linkat(filc_thread* my_thread, int fd1, filc_ptr path1_ptr, int fd2,
                            filc_ptr path2_ptr, int flags)
{
    check_fd(fd1);
    check_fd(fd2);
    char* path1 = filc_check_and_get_tmp_str(my_thread, path1_ptr);
    char* path2 = filc_check_and_get_tmp_str(my_thread, path2_ptr);
    return FILC_SYSCALL(my_thread, linkat(fd1, path1, fd2, path2, flags));
}

int filc_native_zsys_chmod(filc_thread* my_thread, filc_ptr pathname_ptr, unsigned mode)
{
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    return FILC_SYSCALL(my_thread, chmod(pathname, mode));
}

int filc_native_zsys_lchmod(filc_thread* my_thread, filc_ptr pathname_ptr, unsigned mode)
{
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    return FILC_SYSCALL(my_thread, lchmod(pathname, mode));
}

int filc_native_zsys_fchmod(filc_thread* my_thread, int fd, unsigned mode)
{
    check_fd(fd);
    return FILC_SYSCALL(my_thread, fchmod(fd, mode));
}

int filc_native_zsys_mkfifo(filc_thread* my_thread, filc_ptr path_ptr, unsigned mode)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    return FILC_SYSCALL(my_thread, mkfifo(path, mode));
}

int filc_native_zsys_mkdirat(filc_thread* my_thread, int dirfd, filc_ptr pathname_ptr,
                             unsigned mode)
{
    check_fd(dirfd);
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    return FILC_SYSCALL(my_thread, mkdirat(dirfd, pathname, mode));
}

int filc_native_zsys_mkdir(filc_thread* my_thread, filc_ptr pathname_ptr, unsigned mode)
{
    char* pathname = filc_check_and_get_tmp_str(my_thread, pathname_ptr);
    return FILC_SYSCALL(my_thread, mkdir(pathname, mode));
}

int filc_native_zsys_fchmodat(filc_thread* my_thread, int fd, filc_ptr path_ptr, unsigned mode,
                              int flag)
{
    check_fd(fd);
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    return FILC_SYSCALL(my_thread, fchmodat(fd, path, mode, flag));
}

int filc_native_zsys_unlinkat(filc_thread* my_thread, int dirfd, filc_ptr path_ptr, int flag)
{
    check_fd(dirfd);
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    return FILC_SYSCALL(my_thread, unlinkat(dirfd, path, flag));
}

int filc_to_user_errno(int errno_value)
{
    return errno_value;
}

void filc_from_user_sigset(sigset_t* user_sigset,
                           sigset_t* sigset)
{
    memcpy(sigset, user_sigset, sizeof(sigset_t));
    PAS_ASSERT(!sigdelsetyolo(sigset, SIGTIMER));
    PAS_ASSERT(!sigdelsetyolo(sigset, SIGCANCEL));
    PAS_ASSERT(!sigdelsetyolo(sigset, SIGSYNCCALL));
}

void filc_to_user_sigset(sigset_t* sigset, sigset_t* user_sigset)
{
    memcpy(user_sigset, sigset, sizeof(sigset_t));
}

typedef struct {
    int fd;
    int request;
    int result;
} ioctl_data;

static void ioctl_callback(void* guarded_arg, void* user_arg)
{
    ioctl_data* data = (ioctl_data*)user_arg;
    data->result = ioctl(data->fd, data->request, guarded_arg);
}

int filc_native_zsys_ioctl(filc_thread* my_thread, int fd, int request, filc_cc_cursor* args)
{
    check_fd(fd);
    filc_ptr arg_ptr;
    if (filc_cc_cursor_has_next(args, sizeof(void*)))
        arg_ptr = filc_cc_cursor_get_next_ptr(my_thread, args);
    else
        arg_ptr = filc_ptr_forge_null();

    ioctl_data data;
    data.fd = fd;
    data.request = request;
    filc_call_syscall_with_guarded_ptr(my_thread, arg_ptr, ioctl_callback, &data);
    return data.result;
}

int filc_native_zsys_socket(filc_thread* my_thread, int domain, int type, int protocol)
{
    filc_exit(my_thread);
    int result = socket(domain, type, protocol);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setsockopt(filc_thread* my_thread, int sockfd, int level, int optname,
                                filc_ptr optval_ptr, unsigned optlen)
{
    check_fd(sockfd);
    filc_check_read(optval_ptr, optlen);
    filc_exit(my_thread);
    int result = setsockopt(sockfd, level, optname, filc_ptr_ptr(optval_ptr), optlen);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_bind(filc_thread* my_thread, int sockfd, filc_ptr addr_ptr, unsigned addrlen)
{
    check_fd(sockfd);
    filc_check_read(addr_ptr, addrlen);
    filc_exit(my_thread);
    int result = bind(sockfd, (const struct sockaddr*)filc_ptr_ptr(addr_ptr), addrlen);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_connect(filc_thread* my_thread, int sockfd, filc_ptr addr_ptr, unsigned addrlen)
{
    check_fd(sockfd);
    filc_check_read(addr_ptr, addrlen);
    filc_exit(my_thread);
    int result = connect(sockfd, (const struct sockaddr*)filc_ptr_ptr(addr_ptr), addrlen);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_getsockname(filc_thread* my_thread, int sockfd, filc_ptr addr_ptr,
                                 filc_ptr addrlen_ptr)
{
    check_fd(sockfd);
    filc_check_write(addrlen_ptr, sizeof(unsigned));
    unsigned addrlen = *(unsigned*)filc_ptr_ptr(addrlen_ptr);

    filc_check_write(addr_ptr, addrlen);

    filc_exit(my_thread);
    int result = getsockname(sockfd, (struct sockaddr*)filc_ptr_ptr(addr_ptr), &addrlen);
    int my_errno = errno;
    filc_enter(my_thread);

    if (result < 0)
        filc_set_errno(my_errno);
    else
        *(unsigned*)filc_ptr_ptr(addrlen_ptr) = addrlen;
    return result;
}

int filc_native_zsys_getsockopt(filc_thread* my_thread, int sockfd, int level, int optname,
                                filc_ptr optval_ptr, filc_ptr optlen_ptr)
{
    check_fd(sockfd);
    filc_check_write(optlen_ptr, sizeof(unsigned));
    unsigned optlen = *(unsigned*)filc_ptr_ptr(optlen_ptr);

    filc_check_write(optval_ptr, optlen);

    filc_exit(my_thread);
    int result = getsockopt(sockfd, level, optname, filc_ptr_ptr(optval_ptr), &optlen);
    int my_errno = errno;
    filc_enter(my_thread);

    if (result < 0)
        filc_set_errno(my_errno);
    else
        *(unsigned*)filc_ptr_ptr(optlen_ptr) = optlen;
    return result;
}

int filc_native_zsys_getpeername(filc_thread* my_thread, int sockfd, filc_ptr addr_ptr,
                                 filc_ptr addrlen_ptr)
{
    check_fd(sockfd);
    filc_check_write(addrlen_ptr, sizeof(unsigned));
    unsigned addrlen = *(unsigned*)filc_ptr_ptr(addrlen_ptr);

    filc_check_write(addr_ptr, addrlen);

    filc_exit(my_thread);
    int result = getpeername(sockfd, (struct sockaddr*)filc_ptr_ptr(addr_ptr), &addrlen);
    int my_errno = errno;
    filc_enter(my_thread);

    if (result < 0)
        filc_set_errno(my_errno);
    else
        *(unsigned*)filc_ptr_ptr(addrlen_ptr) = addrlen;
    return result;
}

ssize_t filc_native_zsys_sendto(filc_thread* my_thread, int sockfd, filc_ptr buf_ptr, size_t len,
                                int flags, filc_ptr addr_ptr, unsigned addrlen)
{
    check_fd(sockfd);
    filc_check_read(buf_ptr, len);
    filc_check_read(addr_ptr, addrlen);
    filc_exit(my_thread);
    int result = sendto(sockfd, filc_ptr_ptr(buf_ptr), len, flags,
                        (const struct sockaddr*)filc_ptr_ptr(addr_ptr), addrlen);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

ssize_t filc_native_zsys_recvfrom(filc_thread* my_thread, int sockfd, filc_ptr buf_ptr, size_t len,
                                  int flags, filc_ptr addr_ptr, filc_ptr addrlen_ptr)
{
    check_fd(sockfd);
    filc_check_write(buf_ptr, len);
    unsigned* addrlen = NULL;
    if (filc_ptr_ptr(addrlen_ptr)) {
        addrlen = alloca(sizeof(unsigned));
        filc_check_write(addrlen_ptr, sizeof(unsigned));
        *addrlen = *(unsigned*)filc_ptr_ptr(addrlen_ptr);
        filc_check_write(addr_ptr, *addrlen);
    } else if (filc_ptr_ptr(addr_ptr)) {
        filc_set_errno(EINVAL);
        return -1;
    }
    PAS_ASSERT(!!addrlen == !!filc_ptr_ptr(addr_ptr));
    filc_exit(my_thread);
    int result = recvfrom(sockfd, filc_ptr_ptr(buf_ptr), len, flags,
                          (struct sockaddr*)filc_ptr_ptr(addr_ptr), addrlen);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    else if (addrlen)
        *(unsigned*)filc_ptr_ptr(addrlen_ptr) = *addrlen;
    return result;
}

int filc_native_zsys_accept(filc_thread* my_thread, int sockfd, filc_ptr addr_ptr, filc_ptr addrlen_ptr)
{
    check_fd(sockfd);
    filc_check_write(addrlen_ptr, sizeof(unsigned));
    unsigned addrlen = *(unsigned*)filc_ptr_ptr(addrlen_ptr);
    filc_check_write(addr_ptr, addrlen);
    filc_exit(my_thread);
    int result = accept(sockfd, (struct sockaddr*)filc_ptr_ptr(addr_ptr), &addrlen);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    else
        *(unsigned*)filc_ptr_ptr(addrlen_ptr) = addrlen;
    return result;
}

int filc_native_zsys_accept4(filc_thread* my_thread, int sockfd, filc_ptr addr_ptr, filc_ptr addrlen_ptr, int flg)
{
    check_fd(sockfd);
    filc_check_write(addrlen_ptr, sizeof(unsigned));
    unsigned addrlen = *(unsigned*)filc_ptr_ptr(addrlen_ptr);
    filc_check_write(addr_ptr, addrlen);
    filc_exit(my_thread);
    int result = accept4(sockfd, (struct sockaddr*)filc_ptr_ptr(addr_ptr), &addrlen, flg);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    else
        *(unsigned*)filc_ptr_ptr(addrlen_ptr) = addrlen;
    return result;
}

int filc_native_zsys_socketpair(filc_thread* my_thread, int domain, int type, int protocol,
                                filc_ptr sv_ptr)
{
    filc_check_write(sv_ptr, sizeof(int) * 2);
    filc_exit(my_thread);
    int result = socketpair(domain, type, protocol, (int*)filc_ptr_ptr(sv_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

static void check_msghdr(filc_ptr ptr, filc_access_kind access_kind)
{
    filc_check_access(ptr, sizeof(struct msghdr), access_kind);
}

static void from_user_msghdr_base(filc_thread* my_thread, filc_ptr user_msghdr_ptr,
                                  struct msghdr* msghdr, filc_access_kind access_kind)
{
    struct msghdr* user_msghdr = (struct msghdr*)filc_ptr_ptr(user_msghdr_ptr);
    
    pas_zero_memory(msghdr, sizeof(struct msghdr));

    int iovlen = user_msghdr->msg_iovlen;
    msghdr->msg_iov = filc_prepare_iovec(
        my_thread, filc_load_ptr_at(my_thread, user_msghdr_ptr, &user_msghdr->msg_iov), iovlen,
        access_kind);
    msghdr->msg_iovlen = iovlen;

    msghdr->msg_flags = user_msghdr->msg_flags;
}

static void from_user_msghdr_impl(filc_thread* my_thread, filc_ptr user_msghdr_ptr,
                                  struct msghdr* msghdr, filc_access_kind access_kind)
{
    from_user_msghdr_base(my_thread, user_msghdr_ptr, msghdr, access_kind);

    struct msghdr* user_msghdr = (struct msghdr*)filc_ptr_ptr(user_msghdr_ptr);
    
    unsigned msg_namelen = user_msghdr->msg_namelen;
    if (msg_namelen) {
        filc_ptr msg_name = filc_load_ptr_at(my_thread, user_msghdr_ptr, &user_msghdr->msg_name);
        filc_check_access(msg_name, msg_namelen, access_kind);
        msghdr->msg_name = filc_ptr_ptr(msg_name);
        msghdr->msg_namelen = msg_namelen;
    }

    unsigned user_controllen = user_msghdr->msg_controllen;
    if (user_controllen) {
        filc_ptr user_control = filc_load_ptr_at(my_thread, user_msghdr_ptr,
                                                 &user_msghdr->msg_control);
        filc_check_access(user_control, user_controllen, access_kind);
        msghdr->msg_control = filc_ptr_ptr(user_control);
        msghdr->msg_controllen = user_controllen;
    }
}

static void from_user_msghdr_for_send(filc_thread* my_thread, filc_ptr user_msghdr_ptr,
                                      struct msghdr* msghdr)
{
    from_user_msghdr_impl(my_thread, user_msghdr_ptr, msghdr, filc_read_access);
}

static void from_user_msghdr_for_recv(filc_thread* my_thread, filc_ptr user_msghdr_ptr,
                                      struct msghdr* msghdr)
{
    from_user_msghdr_impl(my_thread, user_msghdr_ptr, msghdr, filc_write_access);
}

static void to_user_msghdr_for_recv(struct msghdr* msghdr, filc_ptr user_msghdr_ptr)
{
    struct msghdr* user_msghdr = (struct msghdr*)filc_ptr_ptr(user_msghdr_ptr);
    
    user_msghdr->msg_namelen = msghdr->msg_namelen;
    user_msghdr->msg_controllen = msghdr->msg_controllen;
    user_msghdr->msg_flags = msghdr->msg_flags;
}

ssize_t filc_native_zsys_sendmsg(filc_thread* my_thread, int sockfd, filc_ptr msg_ptr, int flags)
{
    check_fd(sockfd);
    static const bool verbose = false;
    
    if (verbose)
        pas_log("In sendmsg\n");

    check_msghdr(msg_ptr, filc_read_access);
    struct msghdr msg;
    from_user_msghdr_for_send(my_thread, msg_ptr, &msg);
    if (verbose)
        pas_log("Got this far\n");
    filc_exit(my_thread);
    if (verbose)
        pas_log("Actually doing sendmsg\n");
    ssize_t result = sendmsg(sockfd, &msg, flags);
    int my_errno = errno;
    if (verbose)
        pas_log("sendmsg result = %ld\n", (long)result);
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

ssize_t filc_native_zsys_recvmsg(filc_thread* my_thread, int sockfd, filc_ptr msg_ptr, int flags)
{
    check_fd(sockfd);
    static const bool verbose = false;

    check_msghdr(msg_ptr, filc_write_access);
    struct msghdr msg;
    from_user_msghdr_for_recv(my_thread, msg_ptr, &msg);
    filc_exit(my_thread);
    if (verbose) {
        pas_log("Actually doing recvmsg\n");
        pas_log("msg.msg_iov = %p\n", msg.msg_iov);
        pas_log("msg.msg_iovlen = %d\n", msg.msg_iovlen);
        int index;
        for (index = 0; index < (int)msg.msg_iovlen; ++index)
            pas_log("msg.msg_iov[%d].iov_len = %zu\n", index, msg.msg_iov[index].iov_len);
    }
    ssize_t result = recvmsg(sockfd, &msg, flags);
    int my_errno = errno;
    if (verbose)
        pas_log("recvmsg result = %ld\n", (long)result);
    filc_enter(my_thread);
    if (result < 0) {
        if (verbose)
            pas_log("recvmsg failed: %s\n", strerror(errno));
        filc_set_errno(my_errno);
    } else
        to_user_msghdr_for_recv(&msg, msg_ptr);
    return result;

einval:
    filc_set_errno(EINVAL);
    return -1;
}

int filc_native_zsys_fcntl(filc_thread* my_thread, int fd, int cmd, filc_cc_cursor* args)
{
    check_fd(fd);
    static const bool verbose = false;
    
    bool have_arg_int = false;
    bool have_arg_flock = false;
    bool have_arg_uint64_ptr = false;
    bool have_arg_f_owner_ex = false;
    bool have_arg_uids = false;
    filc_access_kind ptr_access_kind = filc_read_access;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
    case F_SETFD:
    case F_SETFL:
    case F_SETOWN:
    case F_ADD_SEALS:
    case F_SETLEASE:
    case F_NOTIFY:
    case F_SETPIPE_SZ:
    case F_SETSIG:
        have_arg_int = true;
        break;
    case F_GETLK:
    case F_OFD_GETLK:
        have_arg_flock = true;
        ptr_access_kind = filc_write_access;
        break;
    case F_SETLK:
    case F_SETLKW:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
    case F_CANCELLK:
        have_arg_flock = true;
        break;
    case F_GET_RW_HINT:
    case F_GET_FILE_RW_HINT:
        have_arg_uint64_ptr = true;
        ptr_access_kind = filc_write_access;
        break;
    case F_SET_RW_HINT:
    case F_SET_FILE_RW_HINT:
        have_arg_uint64_ptr = true;
        break;
    case F_SETOWN_EX:
        have_arg_f_owner_ex = true;
        break;
    case F_GETOWN_EX:
        have_arg_f_owner_ex = true;
        ptr_access_kind = filc_write_access;
        break;
    case F_GETOWNER_UIDS:
        have_arg_uids = true;
        ptr_access_kind = filc_write_access;
        break;
    case F_GETFD:
    case F_GETFL:
    case F_GETOWN:
    case F_GET_SEALS:
    case F_GETLEASE:
    case F_GETPIPE_SZ:
    case F_GETSIG:
        break;
    default:
        if (verbose)
            pas_log("no cmd!\n");
        filc_set_errno(EINVAL);
        return -1;
    }
    int arg_int = 0;
    void* arg_ptr = NULL;
    if (have_arg_int)
        arg_int = filc_cc_cursor_get_next_int(my_thread, args);
    else if (have_arg_flock) {
        filc_ptr flock_ptr = filc_cc_cursor_get_next_ptr(my_thread, args);
        filc_check_access(flock_ptr, sizeof(struct flock), ptr_access_kind);
        arg_ptr = filc_ptr_ptr(flock_ptr);
    } else if (have_arg_uint64_ptr) {
        filc_ptr uint64_ptr = filc_cc_cursor_get_next_ptr(my_thread, args);
        filc_check_access(uint64_ptr, sizeof(uint64_t), ptr_access_kind);
        arg_ptr = filc_ptr_ptr(uint64_ptr);
    } else if (have_arg_f_owner_ex) {
        filc_ptr f_owner_ex_ptr = filc_cc_cursor_get_next_ptr(my_thread, args);
        filc_check_access(f_owner_ex_ptr, sizeof(struct f_owner_ex), ptr_access_kind);
        arg_ptr = filc_ptr_ptr(f_owner_ex_ptr);
    } else if (have_arg_uids) {
        filc_ptr uids_ptr = filc_cc_cursor_get_next_ptr(my_thread, args);
        filc_check_access(uids_ptr, sizeof(uid_t) * 2, ptr_access_kind);
        arg_ptr = filc_ptr_ptr(uids_ptr);
    }
    if (verbose)
        pas_log("so far so good.\n");
    int result;
    filc_exit(my_thread);
    if (have_arg_int)
        result = fcntl(fd, cmd, arg_int);
    else if (arg_ptr)
        result = fcntl(fd, cmd, arg_ptr);
    else
        result = fcntl(fd, cmd);
    int my_errno = errno;
    filc_enter(my_thread);
    if (verbose)
        pas_log("result = %d\n", result);
    if (result == -1) {
        if (verbose)
            pas_log("error = %s\n", strerror(my_errno));
        filc_set_errno(my_errno);
    }
    return result;
}

int filc_native_zsys_poll(filc_thread* my_thread, filc_ptr pollfds_ptr, unsigned long nfds,
                          int timeout)
{
    filc_check_write(pollfds_ptr, filc_mul_size(sizeof(struct pollfd), nfds));
    struct pollfd* pollfds = (struct pollfd*)filc_ptr_ptr(pollfds_ptr);
    filc_exit(my_thread);
    int result = poll(pollfds, nfds, timeout);
    int my_errno = errno;
    filc_enter(my_thread);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_acct(filc_thread* my_thread, filc_ptr file_ptr)
{
    char* file = filc_check_and_get_tmp_str(my_thread, file_ptr);
    filc_exit(my_thread);
    int result = acct(file);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setgroups(filc_thread* my_thread, size_t size, filc_ptr list_ptr)
{
    static const bool verbose = false;
    
    size_t total_size;
    FILC_CHECK(
        !pas_mul_uintptr_overflow(sizeof(unsigned), size, &total_size),
        NULL,
        "size argument too big, causes overflow; size = %zu.",
        size);
    filc_check_read(list_ptr, total_size);
    filc_exit(my_thread);
    PAS_ASSERT(sizeof(gid_t) == sizeof(unsigned));
    if (verbose) {
        pas_log("size = %zu\n", size);
        pas_log("NGROUPS_MAX = %zu\n", (size_t)NGROUPS_MAX);
    }
    int result = setgroups(size, (gid_t*)filc_ptr_ptr(list_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    if (verbose)
        pas_log("result = %d, error = %s\n", result, strerror(my_errno));
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_madvise(filc_thread* my_thread, filc_ptr ptr, size_t length, int advice)
{
    filc_check_write(ptr, length);
    check_mmap(ptr);
    return FILC_SYSCALL(my_thread, madvise(filc_ptr_ptr(ptr), length, advice));
}

int filc_native_zsys_mincore(filc_thread* my_thread, filc_ptr addr, size_t len, filc_ptr vec_ptr)
{
    filc_check_write(
        vec_ptr,
        pas_round_up_to_power_of_2(
            len, pas_page_malloc_alignment()) >> pas_page_malloc_alignment_shift());
    filc_exit(my_thread);
    int result = mincore(filc_ptr_ptr(addr), len, (unsigned char*)filc_ptr_ptr(vec_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_getpriority(filc_thread* my_thread, int which, int who)
{
    filc_exit(my_thread);
    errno = 0;
    int result = getpriority(which, who);
    int my_errno = errno;
    filc_enter(my_thread);
    if (my_errno)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_setpriority(filc_thread* my_thread, int which, int who, int prio)
{
    filc_exit(my_thread);
    int result = setpriority(which, who, prio);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_gettimeofday(filc_thread* my_thread, filc_ptr tp_ptr, filc_ptr tzp_ptr)
{
    if (filc_ptr_ptr(tp_ptr))
        filc_check_write(tp_ptr, sizeof(struct timeval));
    if (filc_ptr_ptr(tzp_ptr))
        filc_check_write(tzp_ptr, sizeof(struct timezone));
    filc_exit(my_thread);
    int result = gettimeofday((struct timeval*)filc_ptr_ptr(tp_ptr),
                              (struct timezone*)filc_ptr_ptr(tzp_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_settimeofday(filc_thread* my_thread, filc_ptr tp_ptr, filc_ptr tzp_ptr)
{
    if (filc_ptr_ptr(tp_ptr))
        filc_check_read(tp_ptr, sizeof(struct timeval));
    if (filc_ptr_ptr(tzp_ptr))
        filc_check_read(tzp_ptr, sizeof(struct timezone));
    filc_exit(my_thread);
    int result = settimeofday((const struct timeval*)filc_ptr_ptr(tp_ptr),
                              (const struct timezone*)filc_ptr_ptr(tzp_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_getrusage(filc_thread* my_thread, int who, filc_ptr rusage_ptr)
{
    filc_check_write(rusage_ptr, sizeof(struct rusage));
    filc_exit(my_thread);
    int result = getrusage(who, (struct rusage*)filc_ptr_ptr(rusage_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_flock(filc_thread* my_thread, int fd, int operation)
{
    check_fd(fd);
    filc_exit(my_thread);
    int result = flock(fd, operation);
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

static int utimes_impl(filc_thread* my_thread, filc_ptr path_ptr, filc_ptr times_ptr,
                       int (*actual_utimes)(const char*, const struct timeval*))
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    if (filc_ptr_ptr(times_ptr))
        filc_check_read(times_ptr, sizeof(struct timeval) * 2);
    filc_exit(my_thread);
    int result = actual_utimes(path, (const struct timeval*)filc_ptr_ptr(times_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_utimes(filc_thread* my_thread, filc_ptr path_ptr, filc_ptr times_ptr)
{
    return utimes_impl(my_thread, path_ptr, times_ptr, utimes);
}

int filc_native_zsys_lutimes(filc_thread* my_thread, filc_ptr path_ptr, filc_ptr times_ptr)
{
    return utimes_impl(my_thread, path_ptr, times_ptr, lutimes);
}

int filc_native_zsys_adjtime(filc_thread* my_thread, filc_ptr delta_ptr, filc_ptr olddelta_ptr)
{
    if (filc_ptr_ptr(delta_ptr))
        filc_check_read(delta_ptr, sizeof(struct timeval));
    if (filc_ptr_ptr(olddelta_ptr))
        filc_check_write(olddelta_ptr, sizeof(struct timeval));
    filc_exit(my_thread);
    int result = adjtime((const struct timeval*)filc_ptr_ptr(delta_ptr),
                         (struct timeval*)filc_ptr_ptr(olddelta_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

long filc_native_zsys_pathconf(filc_thread* my_thread, filc_ptr path_ptr, int name)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    return FILC_SYSCALL(my_thread, pathconf(path, name));
}

long filc_native_zsys_fpathconf(filc_thread* my_thread, int fd, int name)
{
    check_fd(fd);
    return FILC_SYSCALL(my_thread, fpathconf(fd, name));
}

int filc_native_zsys_setrlimit(filc_thread* my_thread, int resource, filc_ptr rlp_ptr)
{
    filc_check_read(rlp_ptr, sizeof(struct rlimit));
    filc_exit(my_thread);
    int result = setrlimit(resource, (struct rlimit*)filc_ptr_ptr(rlp_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

/* It's totally a goal to implement SysV IPC, including the shared memory parts. I just don't have to,
   yet. */
int filc_native_zsys_semget(filc_thread* my_thread, int key, int nsems, int flag)
{
    return FILC_SYSCALL(my_thread, semget(key, nsems, flag));
}

int filc_native_zsys_semctl(filc_thread* my_thread, int semid, int semnum, int cmd,
                            filc_cc_cursor* args)
{
    switch (cmd) {
    case IPC_STAT:
    case IPC_SET:
    case SEM_STAT: {
        filc_ptr semid_ds_ptr = filc_cc_cursor_get_next_ptr(my_thread, args);
        filc_check_write(semid_ds_ptr, sizeof(struct semid_ds));
        return FILC_SYSCALL(
            my_thread, semctl(semid, semnum, cmd, (struct semid_ds*)filc_ptr_ptr(semid_ds_ptr)));
    }

    case IPC_RMID:
    case GETNCNT:
    case GETPID:
    case GETVAL:
    case GETZCNT:
        return FILC_SYSCALL(my_thread, semctl(semid, semnum, cmd));

    case IPC_INFO:
    case SEM_INFO:
    case SEM_STAT_ANY: {
        filc_ptr seminfo_ptr = filc_cc_cursor_get_next_ptr(my_thread, args);
        filc_check_write(seminfo_ptr, sizeof(struct seminfo));
        return FILC_SYSCALL(
            my_thread, semctl(semid, semnum, cmd, (struct seminfo*)filc_ptr_ptr(seminfo_ptr)));
    }

    case GETALL:
    case SETALL:
        filc_set_errno(ENOSYS);
        return -1;

    case SETVAL: {
        int val = filc_cc_cursor_get_next_int(my_thread, args);
        return FILC_SYSCALL(my_thread, semctl(semid, semnum, cmd, val));
    }

    default:
        filc_set_errno(EINVAL);
        return -1;
    }
}

int filc_native_zsys_semop(filc_thread* my_thread, int semid, filc_ptr array_ptr, size_t nops)
{
    filc_check_write(array_ptr, filc_mul_size(nops, sizeof(struct sembuf)));
    return FILC_SYSCALL(my_thread, semop(semid, (struct sembuf*)filc_ptr_ptr(array_ptr), nops));
}

int filc_native_zsys_semtimedop(filc_thread* my_thread, int semid, filc_ptr array_ptr, size_t nops,
                                filc_ptr ts_ptr)
{
    if (filc_ptr_ptr(ts_ptr))
        filc_check_read(ts_ptr, sizeof(struct timespec));
    filc_check_write(array_ptr, filc_mul_size(nops, sizeof(struct sembuf)));
    return FILC_SYSCALL(
        my_thread, semtimedop(semid, (struct sembuf*)filc_ptr_ptr(array_ptr), nops,
                              (const struct timespec*)filc_ptr_ptr(ts_ptr)));
}

int filc_native_zsys_shmget(filc_thread* my_thread, int key, size_t size, int flag)
{
    return FILC_SYSCALL(my_thread, shmget(key, size, flag));
}

int filc_native_zsys_shmctl(filc_thread* my_thread, int shmid, int cmd, filc_ptr buf_ptr)
{
    if (filc_ptr_ptr(buf_ptr))
        filc_check_write(buf_ptr, sizeof(struct shmid_ds));
    return FILC_SYSCALL(my_thread, shmctl(shmid, cmd, (struct shmid_ds*)filc_ptr_ptr(buf_ptr)));
}

filc_ptr filc_native_zsys_shmat(filc_thread* my_thread, int shmid, filc_ptr addr_ptr, int flag)
{
    size_t length;
    filc_ptr result = mmap_error_result();

    if (filc_ptr_ptr(addr_ptr)) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }

    /* We need to know the size of the shared memory before we attach it for real. We can ask for the
       size using semctl, but the trouble with that is that any other process could delete the shmid
       and presumably cause it to get reused (with a different size) in between when we ask for the
       size and when we rely on the size.
       
       So, we "lock" the shmid by doing a dummy shmat, and then shmdt'ing the dummy.
       
       Gross! */
    void* dummy = FILC_SYSCALL(my_thread, shmat(shmid, NULL, SHM_RDONLY));
    PAS_ASSERT(dummy);
    if (dummy == (void*)(intptr_t)-1)
        return mmap_error_result();

    struct shmid_ds stat;
    if (FILC_SYSCALL(my_thread, shmctl(shmid, IPC_STAT, &stat)) < 0)
        goto done;
    
    length = pas_round_up_to_power_of_2(stat.shm_segsz, pas_page_malloc_alignment());
    
    /* And now we hold the dummy attachment until we make the real attachment, so that the length we
       got from it remains valid. */
    
    addr_ptr = filc_ptr_create_with_object(
        my_thread, allocate_aligned_impl(
            my_thread, length, pas_page_malloc_alignment(),
            FILC_OBJECT_FLAG_MMAP | FILC_OBJECT_FLAG_SYSV_SHM));

    void* raw_result = FILC_SYSCALL(
        my_thread, shmat(shmid, filc_ptr_ptr(addr_ptr), flag | SHM_REMAP));

    PAS_ASSERT(raw_result == (void*)(intptr_t)-1 || raw_result == filc_ptr_ptr(addr_ptr));
    if (raw_result == filc_ptr_ptr(addr_ptr)) {
        filc_check_write(addr_ptr, length); /* Make sure someone didn't munmap concurrently. */
        result = addr_ptr;
    }
    
done:
    filc_exit(my_thread);
    shmdt(dummy);
    filc_enter(my_thread);
    return result;
}

int filc_native_zsys_shmdt(filc_thread* my_thread, filc_ptr addr_ptr)
{
    FILC_CHECK(
        filc_ptr_ptr(addr_ptr) == filc_ptr_lower(addr_ptr),
        NULL,
        "attempt to shmdt a pointer not to the base of the capability (ptr = %s).",
        filc_ptr_to_new_string(addr_ptr));
    size_t available = filc_ptr_available(addr_ptr);
    FILC_CHECK(
        available,
        NULL,
        "attempt to shmdt a pointer with zero bytes available (ptr = %s).",
        filc_ptr_to_new_string(addr_ptr));
    filc_check_write(addr_ptr, available);
    check_mmap(addr_ptr);
    FILC_CHECK(
        filc_object_get_flags(filc_ptr_object(addr_ptr)) & FILC_OBJECT_FLAG_SYSV_SHM,
        NULL,
        "attempt to shmdt a pointer not to System V IPC shared memory (ptr = %s).",
        filc_ptr_to_new_string(addr_ptr));
    PAS_ASSERT(pas_is_aligned(available, pas_page_malloc_alignment()));
    filc_free(filc_ptr_object(addr_ptr));
    filc_exit(my_thread);
    filc_unmap(filc_ptr_ptr(addr_ptr), available);
    filc_enter(my_thread);
    return 0;
}

int filc_native_zsys_msgget(filc_thread* my_thread, int key, int msgflg)
{
    return FILC_SYSCALL(my_thread, msgget(key, msgflg));
}

int filc_native_zsys_msgctl(filc_thread* my_thread, int msgid, int cmd, filc_ptr buf_ptr)
{
    if (filc_ptr_ptr(buf_ptr))
        filc_check_write(buf_ptr, sizeof(struct msqid_ds));
    return FILC_SYSCALL(my_thread, msgctl(msgid, cmd, (struct msqid_ds*)filc_ptr_ptr(buf_ptr)));
}

long filc_native_zsys_msgrcv(filc_thread* my_thread, int msgid, filc_ptr msgp_ptr, size_t msgsz,
                             long msgtyp, int msgflg)
{
    filc_check_write(msgp_ptr, PAS_OFFSETOF(struct msgbuf, mtext) + msgsz);
    return FILC_SYSCALL(my_thread, msgrcv(msgid, filc_ptr_ptr(msgp_ptr), msgsz, msgtyp, msgflg));
}

int filc_native_zsys_msgsnd(filc_thread* my_thread, int msgid, filc_ptr msgp_ptr, size_t msgsz,
                            int msgflg)
{
    filc_check_read(msgp_ptr, PAS_OFFSETOF(struct msgbuf, mtext) + msgsz);
    return FILC_SYSCALL(my_thread, msgsnd(msgid, filc_ptr_ptr(msgp_ptr), msgsz, msgflg));
}

int filc_native_zsys_futimes(filc_thread* my_thread, int fd, filc_ptr times_ptr)
{
    check_fd(fd);
    if (filc_ptr_ptr(times_ptr))
        filc_check_read(times_ptr, sizeof(struct timeval) * 2);
    return FILC_SYSCALL(my_thread, futimes(fd, (const struct timeval*)filc_ptr_ptr(times_ptr)));
}

int filc_native_zsys_futimesat(filc_thread* my_thread, int fd, filc_ptr path_ptr, filc_ptr times_ptr)
{
    check_fd(fd);
    if (filc_ptr_ptr(times_ptr))
        filc_check_read(times_ptr, sizeof(struct timeval) * 2);
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    return FILC_SYSCALL(my_thread, futimesat(fd, path,
                                             (const struct timeval*)filc_ptr_ptr(times_ptr)));
}

int filc_native_zsys_clock_settime(filc_thread* my_thread, int clock_id, filc_ptr tp_ptr)
{
    filc_check_read(tp_ptr, sizeof(struct timespec));
    filc_exit(my_thread);
    int result = clock_settime(clock_id, (struct timespec*)filc_ptr_ptr(tp_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_clock_getres(filc_thread* my_thread, int clock_id, filc_ptr tp_ptr)
{
    filc_check_write(tp_ptr, sizeof(struct timespec));
    filc_exit(my_thread);
    int result = clock_getres(clock_id, (struct timespec*)filc_ptr_ptr(tp_ptr));
    int my_errno = errno;
    filc_enter(my_thread);
    PAS_ASSERT(!result || result == -1);
    if (result < 0)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_issetugid(filc_thread* my_thread)
{
    filc_exit(my_thread);
    int result = issetugid();
    filc_enter(my_thread);
    return result;
}

int filc_native_zsys_getresgid(filc_thread* my_thread, filc_ptr rgid_ptr, filc_ptr egid_ptr,
                               filc_ptr sgid_ptr)
{
    filc_check_write(rgid_ptr, sizeof(gid_t));
    filc_check_write(egid_ptr, sizeof(gid_t));
    filc_check_write(sgid_ptr, sizeof(gid_t));
    return FILC_SYSCALL(my_thread, getresgid((gid_t*)filc_ptr_ptr(rgid_ptr),
                                             (gid_t*)filc_ptr_ptr(egid_ptr),
                                             (gid_t*)filc_ptr_ptr(sgid_ptr)));
}

int filc_native_zsys_getresuid(filc_thread* my_thread, filc_ptr ruid_ptr, filc_ptr euid_ptr,
                               filc_ptr suid_ptr)
{
    filc_check_write(ruid_ptr, sizeof(uid_t));
    filc_check_write(euid_ptr, sizeof(uid_t));
    filc_check_write(suid_ptr, sizeof(uid_t));
    return FILC_SYSCALL(my_thread, getresuid((uid_t*)filc_ptr_ptr(ruid_ptr),
                                             (uid_t*)filc_ptr_ptr(euid_ptr),
                                             (uid_t*)filc_ptr_ptr(suid_ptr)));
}

int filc_native_zsys_setresgid(filc_thread* my_thread, unsigned rgid, unsigned egid, unsigned sgid)
{
    return FILC_SYSCALL(my_thread, setresgid(rgid, egid, sgid));
}

int filc_native_zsys_setresuid(filc_thread* my_thread, unsigned ruid, unsigned euid, unsigned suid)
{
    return FILC_SYSCALL(my_thread, setresuid(ruid, euid, suid));
}

int filc_native_zsys_sched_setparam(filc_thread* my_thread, int pid, filc_ptr param_buf)
{
    filc_check_read(param_buf, sizeof(struct sched_param));
    return FILC_SYSCALL(
        my_thread, sched_setparam(pid, (const struct sched_param*)filc_ptr_ptr(param_buf)));
}

int filc_native_zsys_sched_getparam(filc_thread* my_thread, int pid, filc_ptr param_buf)
{
    filc_check_write(param_buf, sizeof(struct sched_param));
    return FILC_SYSCALL(my_thread, sched_getparam(pid, (struct sched_param*)filc_ptr_ptr(param_buf)));
}

int filc_native_zsys_sched_setscheduler(filc_thread* my_thread, int pid, int policy,
                                        filc_ptr param_buf)
{
    filc_check_read(param_buf, sizeof(struct sched_param));
    return FILC_SYSCALL(
        my_thread, sched_setscheduler(pid, policy,
                                      (const struct sched_param*)filc_ptr_ptr(param_buf)));
}

int filc_native_zsys_sched_getscheduler(filc_thread* my_thread, int pid)
{
    return FILC_SYSCALL(my_thread, sched_getscheduler(pid));
}

int filc_native_zsys_sched_get_priority_min(filc_thread* my_thread, int policy)
{
    return FILC_SYSCALL(my_thread, sched_get_priority_min(policy));
}

int filc_native_zsys_sched_get_priority_max(filc_thread* my_thread, int policy)
{
    return FILC_SYSCALL(my_thread, sched_get_priority_max(policy));
}

int filc_native_zsys_sched_rr_get_interval(filc_thread* my_thread, int pid, filc_ptr interval_ptr)
{
    filc_check_write(interval_ptr, sizeof(struct timespec));
    return FILC_SYSCALL(
        my_thread, sched_rr_get_interval(pid, (struct timespec*)filc_ptr_ptr(interval_ptr)));
}

int filc_native_zsys_eaccess(filc_thread* my_thread, filc_ptr path_ptr, int mode)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    return FILC_SYSCALL(my_thread, eaccess(path, mode));
}

int filc_native_zsys_fexecve(filc_thread* my_thread, int fd, filc_ptr argv_ptr, filc_ptr envp_ptr)
{
    check_fd(fd);
    char** argv = filc_check_and_get_null_terminated_string_array(my_thread, argv_ptr);
    char** envp = filc_check_and_get_null_terminated_string_array(my_thread, envp_ptr);
    return FILC_SYSCALL(my_thread, fexecve(fd, argv, envp));
}

int filc_native_zsys_isatty(filc_thread* my_thread, int fd)
{
    check_fd(fd);
    filc_exit(my_thread);
    errno = 0;
    int result = isatty(fd);
    int my_errno = errno;
    filc_enter(my_thread);
    if (!result && my_errno)
        filc_set_errno(my_errno);
    return result;
}

int filc_native_zsys_uname(filc_thread* my_thread, filc_ptr buf_ptr)
{
    filc_check_write(buf_ptr, sizeof(struct utsname));
    return FILC_SYSCALL(my_thread, uname((struct utsname*)filc_ptr_ptr(buf_ptr)));
}

int filc_native_zsys_sendfile(filc_thread* my_thread, int out_fd, int in_fd, filc_ptr offset_ptr,
                              size_t count)
{
    check_fd(out_fd);
    check_fd(in_fd);
    if (filc_ptr_ptr(offset_ptr)) {
        PAS_ASSERT(sizeof(long) == sizeof(off_t));
        filc_check_write(offset_ptr, sizeof(off_t));
    }
    return FILC_SYSCALL(my_thread, sendfile(out_fd, in_fd, (off_t*)filc_ptr_ptr(offset_ptr), count));
}

void filc_native_zsys_futex_wake(filc_thread* my_thread, filc_ptr addr_ptr, int cnt, int priv)
{
    filc_exit(my_thread);
    futex_wake((volatile int*)filc_ptr_ptr(addr_ptr), cnt, priv);
    filc_enter(my_thread);
}

void filc_native_zsys_futex_wait(filc_thread* my_thread, filc_ptr addr_ptr, int val, int priv)
{
    filc_check_read(addr_ptr, sizeof(int));
    filc_exit(my_thread);
    futex_wait((volatile int*)filc_ptr_ptr(addr_ptr), val, priv);
    filc_enter(my_thread);
}

int filc_native_zsys_futex_timedwait(filc_thread* my_thread, filc_ptr addr_ptr, int val, int clock_id,
                                     filc_ptr timeout_ptr, int priv)
{
    filc_check_read(addr_ptr, sizeof(int));
    if (filc_ptr_ptr(timeout_ptr))
        filc_check_read(timeout_ptr, sizeof(struct timespec));
    filc_exit(my_thread);
    int result = futex_timedwait((volatile int*)filc_ptr_ptr(addr_ptr), val, clock_id,
                                 (const struct timespec*)filc_ptr_ptr(timeout_ptr), priv);
    filc_enter(my_thread);
    return result;
}

int filc_native_zsys_futex_unlock_pi(filc_thread* my_thread, filc_ptr addr_ptr, int priv)
{
    filc_check_write(addr_ptr, sizeof(int));
    filc_exit(my_thread);
    int result = futex_unlock_pi((volatile int*)filc_ptr_ptr(addr_ptr), priv);
    filc_enter(my_thread);
    return result;
}

int filc_native_zsys_futex_lock_pi(filc_thread* my_thread, filc_ptr addr_ptr, int priv,
                                   filc_ptr timeout_ptr)
{
    filc_check_write(addr_ptr, sizeof(int));
    if (filc_ptr_ptr(timeout_ptr))
        filc_check_read(timeout_ptr, sizeof(struct timespec));
    filc_exit(my_thread);
    int result = futex_lock_pi((volatile int*)filc_ptr_ptr(addr_ptr), priv,
                               (const struct timespec*)filc_ptr_ptr(timeout_ptr));
    filc_enter(my_thread);
    return result;
}

void filc_native_zsys_futex_requeue(filc_thread* my_thread, filc_ptr addr_ptr, int priv,
                                    int wake_count, int requeue_count, filc_ptr addr2_ptr)
{
    filc_exit(my_thread);
    futex_requeue((volatile int*)filc_ptr_ptr(addr_ptr), priv, wake_count, requeue_count,
                  (volatile int*)filc_ptr_ptr(addr2_ptr));
    filc_enter(my_thread);
}

int filc_native_zsys_getdents(filc_thread* my_thread, int fd, filc_ptr dirent_ptr, size_t size)
{
    check_fd(fd);
    filc_check_write(dirent_ptr, size);
    return FILC_SYSCALL(my_thread, getdents(fd, (struct dirent*)filc_ptr_ptr(dirent_ptr), size));
}

long filc_native_zsys_getrandom(filc_thread* my_thread, filc_ptr buf_ptr, size_t buflen,
                                unsigned flags)
{
    filc_check_write(buf_ptr, buflen);
    return FILC_SYSCALL(my_thread, getrandom(filc_ptr_ptr(buf_ptr), buflen, flags));
}

int filc_native_zsys_epoll_create1(filc_thread* my_thread, int flags)
{
    return FILC_SYSCALL(my_thread, epoll_create1(flags));
}

struct user_epoll_event {
	uint32_t events;
	epoll_data_t data;
};

static struct epoll_event* from_user_epoll_event(filc_thread* my_thread, filc_ptr ev_ptr)
{
    if (!filc_ptr_ptr(ev_ptr))
        return NULL;
    struct epoll_event* ev = filc_bmalloc_allocate_tmp(my_thread, sizeof(struct epoll_event));
    filc_check_read(ev_ptr, sizeof(struct user_epoll_event));
    struct user_epoll_event* user_ev = (struct user_epoll_event*)filc_ptr_ptr(ev_ptr);
    ev->events = user_ev->events;
    memcpy(&ev->data, &user_ev->data, sizeof(epoll_data_t));
    return ev;
}

static struct epoll_event* make_epoll_events(filc_thread* my_thread, int maxevents)
{
    return filc_bmalloc_allocate_tmp(my_thread, filc_mul_size(maxevents, sizeof(struct epoll_event)));
}

static int to_user_epoll_events(int result, struct epoll_event* evs, filc_ptr evs_ptr)
{
    if (result < 0)
        return result;
    filc_check_write(evs_ptr, filc_mul_size(result, sizeof(struct user_epoll_event)));
    struct user_epoll_event* user_evs = (struct user_epoll_event*)filc_ptr_ptr(evs_ptr);
    int index;
    for (index = result; index--;) {
        user_evs[index].events = evs[index].events;
        memcpy(&user_evs[index].data, &evs[index].data, sizeof(epoll_data_t));
    }
    return result;
}

int filc_native_zsys_epoll_ctl(filc_thread* my_thread, int epfd, int op, int fd, filc_ptr event_ptr)
{
    check_fd(epfd);
    check_fd(fd);
    struct epoll_event* ev = from_user_epoll_event(my_thread, event_ptr);
    return FILC_SYSCALL(my_thread, epoll_ctl(epfd, op, fd, ev));
}

int filc_native_zsys_epoll_wait(filc_thread* my_thread, int epfd, filc_ptr events_ptr, int maxevents,
                                int timeout)
{
    check_fd(epfd);
    struct epoll_event* evs = make_epoll_events(my_thread, maxevents);
    return to_user_epoll_events(
        FILC_SYSCALL(my_thread, epoll_wait(epfd, evs, maxevents, timeout)),
        evs, events_ptr);
}

int filc_native_zsys_epoll_pwait(filc_thread* my_thread, int epfd, filc_ptr events_ptr, int maxevents,
                                 int timeout, filc_ptr sigmask_ptr)
{
    check_fd(epfd);
    sigset_t* sigmask = NULL;
    if (filc_ptr_ptr(sigmask_ptr)) {
        filc_check_user_sigset(sigmask_ptr, filc_read_access);
        sigmask = alloca(sizeof(sigset_t));
        filc_from_user_sigset((sigset_t*)filc_ptr_ptr(sigmask_ptr), sigmask);
    }
    struct epoll_event* evs = make_epoll_events(my_thread, maxevents);
    return to_user_epoll_events(
        FILC_SYSCALL(my_thread, epoll_pwait(epfd, evs, maxevents, timeout, sigmask)),
        evs, events_ptr);
}

int filc_native_zsys_sysinfo(filc_thread* my_thread, filc_ptr info_ptr)
{
    filc_check_write(info_ptr, sizeof(struct sysinfo));
    return FILC_SYSCALL(my_thread, sysinfo((struct sysinfo*)filc_ptr_ptr(info_ptr)));
}

int filc_native_zsys_sched_setaffinity(filc_thread* my_thread, int tid, size_t size, filc_ptr set_ptr)
{
    filc_check_read(set_ptr, size);
    return FILC_SYSCALL(
        my_thread, sched_setaffinity(tid, size, (const cpu_set_t*)filc_ptr_ptr(set_ptr)));
}

int filc_native_zsys_sched_getaffinity(filc_thread* my_thread, int tid, size_t size, filc_ptr set_ptr)
{
    filc_check_write(set_ptr, size);
    return FILC_SYSCALL(my_thread, sched_getaffinity(tid, size, (cpu_set_t*)filc_ptr_ptr(set_ptr)));
}

int filc_native_zsys_posix_fadvise(filc_thread* my_thread, int fd, long base, long len, int advice)
{
    check_fd(fd);
    return FILC_SYSCALL(my_thread, posix_fadvise(fd, base, len, advice));
}

int filc_native_zsys_ppoll(filc_thread* my_thread, filc_ptr fds_ptr, unsigned long nfds,
                           filc_ptr to_ptr, filc_ptr mask_ptr)
{
    filc_check_write(fds_ptr, filc_mul_size(sizeof(struct pollfd), nfds));
    if (filc_ptr_ptr(to_ptr))
        filc_check_read(to_ptr, sizeof(struct timespec));
    sigset_t* sigmask = NULL;
    if (filc_ptr_ptr(mask_ptr)) {
        filc_check_user_sigset(mask_ptr, filc_read_access);
        sigmask = alloca(sizeof(sigset_t));
        filc_from_user_sigset((sigset_t*)filc_ptr_ptr(mask_ptr), sigmask);
    }
    return FILC_SYSCALL(my_thread, ppoll((struct pollfd*)filc_ptr_ptr(fds_ptr), nfds,
                                         (const struct timespec*)filc_ptr_ptr(to_ptr), sigmask));
}

int filc_native_zsys_wait4(filc_thread* my_thread, int pid, filc_ptr status_ptr, int options,
                           filc_ptr ru_ptr)
{
    if (filc_ptr_ptr(status_ptr))
        filc_check_write(status_ptr, sizeof(int));
    if (filc_ptr_ptr(ru_ptr))
        filc_check_write(ru_ptr, sizeof(struct rusage));
    return FILC_SYSCALL(my_thread, wait4(pid, (int*)filc_ptr_ptr(status_ptr), options,
                                         (struct rusage*)filc_ptr_ptr(ru_ptr)));
}

int filc_native_zsys_sigsuspend(filc_thread* my_thread, filc_ptr mask_ptr)
{
    sigset_t mask;
    filc_check_user_sigset(mask_ptr, filc_read_access);
    filc_from_user_sigset((sigset_t*)filc_ptr_ptr(mask_ptr), &mask);
    return FILC_SYSCALL(my_thread, sigsuspend(&mask));
}

int filc_native_zsys_prctl(filc_thread* my_thread, int option, filc_cc_cursor* args)
{
    switch (option) {
    case PR_SET_DUMPABLE: {
        unsigned long value = filc_cc_cursor_get_next_unsigned_long(my_thread, args);
        return FILC_SYSCALL(my_thread, prctl(PR_SET_DUMPABLE, value));
    }

    case PR_SET_NO_NEW_PRIVS: {
        unsigned long value1 = filc_cc_cursor_get_next_unsigned_long(my_thread, args);
        unsigned long value2 = filc_cc_cursor_get_next_unsigned_long(my_thread, args);
        unsigned long value3 = filc_cc_cursor_get_next_unsigned_long(my_thread, args);
        unsigned long value4 = filc_cc_cursor_get_next_unsigned_long(my_thread, args);
        return FILC_SYSCALL(my_thread, prctl(PR_SET_NO_NEW_PRIVS, value1, value2, value3, value4));
    }
    
    default:
        filc_set_errno(ENOSYS);
        return -1;
    }
}

int filc_native_zsys_eventfd(filc_thread* my_thread, unsigned initval, int flags)
{
    return FILC_SYSCALL(my_thread, eventfd(initval, flags));
}

long filc_native_zsys_listxattr(filc_thread* my_thread, filc_ptr path_ptr, filc_ptr list_ptr,
                                size_t size)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_check_write(list_ptr, size);
    return FILC_SYSCALL(my_thread, listxattr(path, (char*)filc_ptr_ptr(list_ptr), size));
}

long filc_native_zsys_llistxattr(filc_thread* my_thread, filc_ptr path_ptr, filc_ptr list_ptr,
                                 size_t size)
{
    char* path = filc_check_and_get_tmp_str(my_thread, path_ptr);
    filc_check_write(list_ptr, size);
    return FILC_SYSCALL(my_thread, llistxattr(path, (char*)filc_ptr_ptr(list_ptr), size));
}

long filc_native_zsys_flistxattr(filc_thread* my_thread, int fd, filc_ptr list_ptr, size_t size)
{
    check_fd(fd);
    filc_check_write(list_ptr, size);
    return FILC_SYSCALL(my_thread, flistxattr(fd, (char*)filc_ptr_ptr(list_ptr), size));
}

int filc_native_zsys_landlock_create_ruleset(filc_thread* my_thread, filc_ptr attr_ptr, size_t size,
                                             unsigned flags)
{
    filc_check_read(attr_ptr, size);
    return FILC_SYSCALL(my_thread, syscall(SYS_landlock_create_ruleset, filc_ptr_ptr(attr_ptr), size,
                                           flags));
}

int filc_native_zsys_landlock_add_rule(filc_thread* my_thread, int fd, int rule_type,
                                       filc_ptr rule_attr_ptr, unsigned flags)
{
    check_fd(fd);
    switch (rule_type) {
    case LANDLOCK_RULE_PATH_BENEATH:
        filc_check_read(rule_attr_ptr, sizeof(struct landlock_path_beneath_attr));
        break;

    default:
        filc_set_errno(ENOSYS);
        return -1;
    }
    return FILC_SYSCALL(my_thread, syscall(SYS_landlock_add_rule, fd, rule_type,
                                           filc_ptr_ptr(rule_attr_ptr), flags));
}

int filc_native_zsys_landlock_restrict_self(filc_thread* my_thread, int fd, unsigned flags)
{
    check_fd(fd);
    return FILC_SYSCALL(my_thread, syscall(SYS_landlock_restrict_self, fd, flags));
}

int filc_native_zsys_perf_event_open(filc_thread* my_thread, filc_ptr attr_ptr, int pid, int cpu,
                                     int fd, unsigned long flags)
{
    check_fd(fd);
    filc_ptr size_ptr = filc_ptr_with_offset(attr_ptr, PAS_OFFSETOF(struct perf_event_attr, size));
    filc_check_read(size_ptr, sizeof(unsigned));
    unsigned size = *(unsigned*)filc_ptr_ptr(size_ptr);
    /* I don't know if the kernel might write to the attr, so I assume it will. */
    filc_check_write(attr_ptr, size);
    return FILC_SYSCALL(my_thread, syscall(SYS_perf_event_open, filc_ptr_ptr(attr_ptr), pid, cpu, fd,
                                           flags));
}

filc_ptr filc_native_zsys_mremap(filc_thread* my_thread, filc_ptr old_address_ptr, size_t old_size,
                                 size_t new_size, int flags, filc_ptr new_address_ptr)
{
    static const bool verbose = false;
    
    /* NOTE: Because of how we handle private mappings (see comment below), we have to catch all cases
       of EINVAL proactively. */
    
    if (!pas_is_aligned((uintptr_t)filc_ptr_ptr(old_address_ptr), pas_page_malloc_alignment()) ||
        !pas_is_aligned((uintptr_t)filc_ptr_ptr(new_address_ptr), pas_page_malloc_alignment())) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }
    
    if ((flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }

    /* We don't support DONTUNMAP, at least not yet. I'll have to think through the memory safety
       implications. */
    if ((flags & MREMAP_DONTUNMAP)) {
        filc_set_errno(ENOSYS);
        return mmap_error_result();
    }

    if (!old_size && !(flags & MREMAP_MAYMOVE)) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }

    if (!new_size) {
        filc_set_errno(EINVAL);
        return mmap_error_result();
    }

    old_size = pas_round_up_to_power_of_2(old_size, pas_page_malloc_alignment());
    new_size = pas_round_up_to_power_of_2(new_size, pas_page_malloc_alignment());

    /* If we're mremaping with !old_size then we still need the pointer to point at a page in the
       mmap, and since the pointer must be page-aligned, then it follows that there must always be
       at least a page available. */
    filc_check_write(old_address_ptr, pas_max_uintptr(old_size, pas_page_malloc_alignment()));
    check_mmap(old_address_ptr);

    if ((flags & MREMAP_FIXED)) {
        if (!(flags & MREMAP_MAYMOVE)) {
            filc_set_errno(EINVAL);
            return mmap_error_result();
        }
        filc_check_write(new_address_ptr, new_size);
        check_mmap(new_address_ptr);
    } else if ((flags & MREMAP_MAYMOVE)) {
        new_address_ptr = filc_ptr_create_with_object(
            my_thread, allocate_aligned_impl(
                my_thread, new_size, pas_page_malloc_alignment(), FILC_OBJECT_FLAG_MMAP));
    } else {
        if (new_size > filc_ptr_available(old_address_ptr)) {
            filc_set_errno(ENOMEM);
            return mmap_error_result();
        }
        new_address_ptr = old_address_ptr;
    }

    if (verbose) {
        pas_log("mremapping from %p...%p (%zu bytes) to %p...%p (%zu bytes).\n",
                filc_ptr_ptr(old_address_ptr), (char*)filc_ptr_ptr(old_address_ptr) + old_size,
                old_size,
                filc_ptr_ptr(new_address_ptr), (char*)filc_ptr_ptr(new_address_ptr) + new_size,
                new_size);
    }

    /* We cannot just simply mremap, because that will kill the old mapping. We cannot kill the old
       mapping, because if we do, we immediately create a possibility of a memory safety bug where
       some other thread asks the GC for memory and the GC mmaps something in the hole created by the
       mremap.
       
       For shared mappings, I think the fix is to always pas zero as the old_size to mremap.
       
       For private mappings, we'll have an answer eventually that involves doing something smart if
       mremap fails. Or something. See big ass comment below.
    
       Wacky! */

    /* FIXME: It would be great to filc_free() the old mapping. We should do that right before
       filc_unmapping, and we need to be entered to do it. */
    
    filc_exit(my_thread);
    void* result;
    if ((flags & MREMAP_MAYMOVE)) {
        result = mremap(filc_ptr_ptr(old_address_ptr), 0, new_size,
                        MREMAP_MAYMOVE | MREMAP_FIXED, filc_ptr_ptr(new_address_ptr));
        PAS_ASSERT(result == filc_ptr_ptr(new_address_ptr) || result == (void*)(intptr_t)-1);
        if (result == filc_ptr_ptr(new_address_ptr)) {
            /* We did a mremap with old_size == 0, so now we have to unmap the old mapping to simulate
               mremap with old_size != 0. */
            filc_unmap(filc_ptr_ptr(old_address_ptr), old_size);
        } else {
            PAS_ASSERT(result == (void*)(intptr_t)-1);
            if (errno == EINVAL) {
                /* We must have a private mapping.
                 
                   We could handle this, but it'll be super weird.
                
                   Things that don't work:
                
                   - We can't just create a new mapping. For anonymous mappings, we wouldn't be
                     creating a copy of the mapping with the right protections, and then we'd have to
                     copy the contents and that sounds expensive. For private mappings, we wouldn't
                     (easily or even practically) figure out what file to map. The file descriptor is
                     possibly closed by now.
                
                   - We can't stop the world and call mremap, and then refill the unmapped part with
                     a private fixed anonymous mapping. In a stopped world, any thread may still be
                     executing any code in yolomusl as well as some subset of code in libpas. Maybe
                     the libpas code we're executing won't call mmap, but I feel like I'd have to do
                     an audit to be sure. Weirdly, the runtime's implementation of user mmap would be
                     fine (it's calling mmap fixed after allocating memory while entered). The
                     yolomusl code could be executing mmap, for example in the implementation of
                     pthread_create(). There may be other places. So, stopping the world isn't enough.
                
                   - Switch the munmap implementation back to soft handshaking, and have mmap objects
                     be allocated directly with mmap, not under verse heap control. Make it so they
                     have a special mark bit, not managed by the verse heap. In that world, it's fine
                     if mremap deletes the mapping. We would have handshaked first to make sure that
                     everyone knows it's free. But this doesn't work for mremap of a subset of the
                     allocated mapping. It also doesn't work for munmap of a subset of the mapping.
                     So, that seems like a regression.

                   Something that might work:
                
                   - Give yolomusl a lock that it locks around mmap/mremap calls. Then, in here, we
                     grab that lock, call mremap, and then immediately mmap a fixed anonymous mapping
                     in the hole we created.
                
                   However, mremap is mostly just used in:
                
                   - Memory management implementations. We expect folks to just use zgc_alloc/zgc_free
                     because they're just calling malloc/free, and if they write their own malloc,
                     then that's bad for them (they might panic, and if they don't, they're getting
                     suboptimal protection). So, we don't expect to see these kinds of calls.
                
                   - Shared file mapping scenarios. This is when mremap is really the most useful. We
                     handle that case!
                   
                   In other words, I cannot think of (and haven't seen) a use case for CoW mappings
                   (i.e. private mapping that isn't anonymous).
                
                   But if I ever find a use of mremap for private mappings (either anonymous or CoW)
                   then I guess I'll have to add the yolomusl lock, or else think of something
                   smarter. */
                errno = ENOSYS;
            }
        }
    } else {
        PAS_ASSERT(!(flags & MREMAP_MAYMOVE));
        PAS_ASSERT(new_size <= filc_ptr_available(old_address_ptr));
        PAS_ASSERT(filc_ptr_ptr(new_address_ptr) == filc_ptr_ptr(old_address_ptr));
        if (new_size > old_size) {
            /* This is the simplest case. We know that we can enlarge the mapping in place, so there
               isn't anything being unmapped. This is the one case where we can just directly use
               mremap.
            
               Except we can't. This case never works. We'll only come here if we had previously
               shrunk the mapping, which means that the space beyond the mapping is mapped anonymous
               private. So, this mremap always fails with ENOMEM. */
            result = mremap(filc_ptr_ptr(old_address_ptr), old_size, new_size, 0);
        } else {
            PAS_ASSERT(old_size > new_size);
            filc_unmap((char*)filc_ptr_ptr(old_address_ptr) + new_size, old_size - new_size);
            result = filc_ptr_ptr(new_address_ptr);
        }
    }
    PAS_ASSERT(result == filc_ptr_ptr(new_address_ptr) || result == (void*)(intptr_t)-1);
    int my_errno = errno;
    filc_enter(my_thread);

    if (result == filc_ptr_ptr(new_address_ptr)) {
        filc_check_write(new_address_ptr, new_size); /* Make sure someone didn't munmap
                                                        concurrently. */
        return new_address_ptr;
    }

    PAS_ASSERT(result == (void*)(uintptr_t)-1);

    filc_set_errno(errno);
    return mmap_error_result();
}

int filc_native_zsys_signalfd(filc_thread* my_thread, int fd, filc_ptr mask_ptr, int flags)
{
    filc_check_user_sigset(mask_ptr, filc_read_access);
    sigset_t set;
    filc_from_user_sigset((sigset_t*)filc_ptr_ptr(mask_ptr), &set);
    int sig;
    for (sig = 1; sig <= FILC_MAX_USER_SIGNUM; ++sig) {
        if (is_unsafe_signal_for_handlers(sig))
            PAS_ASSERT(!sigdelsetyolo(&set, sig));
    }
    return FILC_SYSCALL(my_thread, signalfd(fd, &set, flags));
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
    check_zthread(thread_ptr);
    filc_thread* thread = (filc_thread*)filc_ptr_ptr(thread_ptr);
    if (thread->has_set_tid)
        return thread->tid;
    filc_exit(my_thread);
    pas_system_mutex_lock(&thread->lock);
    while (!thread->has_set_tid)
        pas_system_condition_wait(&thread->cond, &thread->lock);
    pas_system_mutex_unlock(&thread->lock);
    filc_enter(my_thread);
    return thread->tid;
}

unsigned filc_native_zthread_self_id(filc_thread* my_thread)
{
    PAS_ASSERT(my_thread->tid);
    PAS_ASSERT(my_thread->has_set_tid);
    return my_thread->tid;
}

filc_ptr filc_native_zthread_get_cookie(filc_thread* my_thread, filc_ptr thread_ptr)
{
    PAS_UNUSED_PARAM(my_thread);
    check_zthread(thread_ptr);
    filc_thread* thread = (filc_thread*)filc_ptr_ptr(thread_ptr);
    return filc_flight_ptr_load_with_manual_tracking(&thread->cookie_ptr);
}

void filc_native_zthread_set_self_cookie(filc_thread* my_thread, filc_ptr cookie_ptr)
{
    static const bool verbose = false;
    if (verbose) {
        pas_log("setting cookie: ");
        filc_ptr_dump(cookie_ptr, pas_log_stream);
        pas_log("\n");
    }
    filc_flight_ptr_store(my_thread, &my_thread->cookie_ptr, cookie_ptr);
    if (verbose) {
        pas_log("cookie lower: %p\n", my_thread->cookie_ptr.lower);
        pas_log("cookie ptr: %p\n", my_thread->cookie_ptr.ptr);
    }
}

void filc_native_zthread_exit(filc_thread* thread, filc_ptr result)
{
    static const bool verbose = false;

    unsigned tid = thread->tid;
    PAS_ASSERT(tid);
    PAS_ASSERT(tid == (unsigned)gettid());
    
    if (verbose)
        pas_log("thread %u main function returned\n", tid);

    pas_system_mutex_lock(&thread->lock);
    PAS_ASSERT(!thread->has_stopped);
    PAS_ASSERT(thread->thread);
    PAS_ASSERT(thread->thread == pthread_self());
    filc_flight_ptr_store(thread, &thread->result_ptr, result);
    pas_system_mutex_unlock(&thread->lock);

    thread->top_frame = NULL;
    while (thread->top_native_frame) {
        if (thread->top_native_frame->locked)
            filc_unlock_top_native_frame(thread);
        filc_pop_native_frame(thread, thread->top_native_frame);
    }
    thread->allocation_roots.size = 0;

    sigset_t set;
    pas_reasonably_fill_sigset(&set);
    if (verbose)
        pas_log("%s: blocking signals\n", __PRETTY_FUNCTION__);
    PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &set, NULL));

    fugc_donate(&thread->mark_stack);
    filc_thread_stop_allocators(thread);
    thread->tid = 0;
    thread->is_stopping = true;
    filc_thread_undo_create(thread);
    pas_thread_local_cache_destroy(pas_lock_is_not_held);
    bmalloc_deallocate(thread->cc_outline_buffer);
    bmalloc_deallocate(thread->cc_outline_aux_buffer);
    filc_exit(thread);

    pas_system_mutex_lock(&thread->lock);
    PAS_ASSERT(!thread->has_stopped);
    PAS_ASSERT(thread->thread);
    PAS_ASSERT(!(thread->state & FILC_THREAD_STATE_ENTERED));
    PAS_ASSERT(!(thread->state & FILC_THREAD_STATE_DEFERRED_SIGNAL));
    size_t index;
    for (index = FILC_MAX_USER_SIGNUM + 1; index--;)
        PAS_ASSERT(!thread->num_deferred_signals[index]);
    thread->thread = PAS_NULL_SYSTEM_THREAD_ID;
    thread->has_stopped = true;
    pas_system_condition_broadcast(&thread->cond);
    pas_system_mutex_unlock(&thread->lock);
    
    if (verbose)
        pas_log("thread %u disposing\n", tid);

    filc_thread_dispose(thread);

    /* At this point, the GC no longer sees this thread except if the user is holding references to it.
       And since we're exited, the GC could run at any time. So the thread might be alive or it might be
       dead - we don't know. */

    pthread_exit(NULL);

    PAS_ASSERT(!"Should not get here");
}

static void* start_thread(void* arg)
{
    static const bool verbose = false;
    
    filc_thread* thread;

    thread = (filc_thread*)arg;

    set_stack_limit(thread);

    pas_system_mutex_lock(&thread->lock);
    unsigned tid = gettid();
    thread->tid = tid;
    thread->has_set_tid = true;
    pas_system_condition_broadcast(&thread->cond);
    pas_system_mutex_unlock(&thread->lock);

    if (verbose)
        pas_log("thread %u (%p) starting\n", tid, thread);

    PAS_ASSERT(thread->has_started);
    PAS_ASSERT(!thread->has_stopped);
    PAS_ASSERT(!thread->error_starting);

    PAS_ASSERT(!pthread_setspecific(filc_thread_key, thread));
    PAS_ASSERT(!thread->thread);
    pas_fence();
    thread->thread = pthread_self();

    PAS_ASSERT(!pthread_detach(thread->thread));

    PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &thread->initial_blocked_sigs, NULL));
    
    filc_enter(thread);

    thread->tlc_node = verse_heap_get_thread_local_cache_node();
    thread->tlc_node_version = pas_thread_local_cache_node_version(thread->tlc_node);

    FILC_DEFINE_FRAME("start_thread");
    filc_push_frame(thread, frame);

    filc_native_frame native_frame;
    filc_push_native_frame(thread, &native_frame);

    filc_ptr arg_ptr = filc_flight_ptr_load(thread, &thread->arg_ptr);
    filc_flight_ptr_store(thread, &thread->arg_ptr, filc_ptr_forge_null());

    if (verbose)
        pas_log("thread %u calling main function\n", tid);

    filc_ptr result = filc_call_user_ptr_ptr(thread, thread->thread_main, arg_ptr);

    filc_native_zthread_exit(thread, result);

    PAS_ASSERT(!"Should not get here");
    return NULL;
}

filc_ptr filc_native_zthread_create(filc_thread* my_thread, filc_ptr callback_ptr, filc_ptr arg_ptr)
{
    filc_check_function_call(callback_ptr);
    filc_thread* thread = filc_thread_create_with_manual_tracking();
    filc_thread_track_object(my_thread, filc_object_for_special_payload(thread));
    pas_system_mutex_lock(&thread->lock);
    /* I don't see how this could ever happen. */
    PAS_ASSERT(!thread->thread);
    PAS_ASSERT(filc_ptr_is_totally_null(thread->arg_ptr));
    PAS_ASSERT(filc_ptr_is_totally_null(thread->result_ptr));
    PAS_ASSERT(filc_ptr_is_totally_null(thread->cookie_ptr));
    thread->thread_main = (pizlonated_function)filc_ptr_ptr(callback_ptr);
    filc_flight_ptr_store(my_thread, &thread->arg_ptr, arg_ptr);
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
        thread->has_set_tid = true;
        pas_system_condition_broadcast(&thread->cond);
        pas_system_mutex_unlock(&thread->lock);
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
        filc_check_write(result_ptr, sizeof(void*));
        filc_store_ptr(my_thread, result_ptr, 0,
                       filc_flight_ptr_load(my_thread, &thread->result_ptr));
    }
    return true;
}

void filc_thread_destroy_space_with_guard_page(filc_thread* my_thread)
{
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
}

char* filc_thread_get_end_of_space_with_guard_page_with_size(filc_thread* my_thread,
                                                             size_t desired_size)
{
    static const bool verbose = false;
    if (!desired_size)
        desired_size = 1;
    PAS_ASSERT(my_thread->guard_page >= my_thread->space_with_guard_page);
    if ((size_t)(my_thread->guard_page - my_thread->space_with_guard_page) >= desired_size) {
        PAS_ASSERT(my_thread->space_with_guard_page);
        PAS_ASSERT(my_thread->guard_page);
        if (verbose)
            pas_log("returning existing guard page: %p\n", my_thread->guard_page);
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
    if (verbose) {
        pas_log("created new space with guard page: %p, guard page: %p\n",
                my_thread->space_with_guard_page,
                my_thread->guard_page);
    }
    return (char*)result.result + size;
}

void filc_call_syscall_with_guarded_ptr(filc_thread* my_thread,
                                        filc_ptr arg_ptr,
                                        void (*syscall_callback)(void* guarded_arg,
                                                                 void* user_arg),
                                        void* user_arg)
{
    static const bool verbose = false;
    
    static const uintptr_t min_address = (uintptr_t)1 << (uintptr_t)32;

    /* It's possible that someone is calling an ioctl that takes an int or long argument. But, we
       don't know if the ioctl will actually interpret the argument as an int or long - it might
       interpreter it as a pointer!
       
       So, we only allow passing an integer through to ioctl if we know that it cannot possibly be a
       valid pointer in userspace.
       
       If we ever find ioctls that require integers bigger than min_address, then they'd have to be
       special case by our wrapping. */
    if (filc_ptr_ptr(arg_ptr) < (void*)min_address) {
        PAS_ASSERT(!filc_ptr_object(arg_ptr) || filc_ptr_ptr(arg_ptr) < filc_ptr_lower(arg_ptr));
        filc_exit(my_thread);
        errno = 0;
        syscall_callback(filc_ptr_ptr(arg_ptr), user_arg);
        int my_errno = errno;
        filc_enter(my_thread);
        if (my_errno)
            filc_set_errno(my_errno);
        return;
    }

    size_t extent_limit = 128;
    char* base = (char*)filc_ptr_ptr(arg_ptr);
    filc_object* object = filc_ptr_object(arg_ptr);
    size_t maximum_extent = filc_ptr_available(arg_ptr);
    if (verbose)
        pas_log("maximum_extent = %zu\n", maximum_extent);
    filc_check_read(arg_ptr, maximum_extent);
    for(;;) {
        size_t limited_extent = pas_min_uintptr(maximum_extent, extent_limit);
        if (verbose)
            pas_log("limited_extent = %zu\n", limited_extent);

        char* input_copy = bmalloc_allocate(limited_extent);
        memcpy(input_copy, filc_ptr_ptr(arg_ptr), limited_extent);

        char* start_of_space =
            filc_thread_get_end_of_space_with_guard_page_with_size(my_thread, limited_extent)
            - limited_extent;
        if (verbose)
            pas_log("start_of_space = %p\n", start_of_space);
        memcpy(start_of_space, input_copy, limited_extent);

        filc_exit(my_thread);
        errno = 0;
        syscall_callback(start_of_space, user_arg);
        int my_errno = errno;
        filc_enter(my_thread);
        if (my_errno != EFAULT) {
            if (!my_errno) {
                size_t index;
                bool is_same = true;
                for (index = 0; index < limited_extent; ++index) {
                    if (start_of_space[index] != input_copy[index]) {
                        is_same = false;
                        break;
                    }
                }
                if (!is_same) {
                    filc_check_write(arg_ptr, limited_extent);
                    memcpy(base, start_of_space, limited_extent);
                }
            }
            if (my_errno)
                filc_set_errno(my_errno);
            bmalloc_deallocate(input_copy);
            return;
        }

        bmalloc_deallocate(input_copy);
        if (extent_limit > limited_extent) {
            filc_safety_panic(
                NULL,
                "was only able to pass %zu int bytes to the syscall but the kernel wanted more "
                "(arg ptr = %s).",
                limited_extent, filc_ptr_to_new_string(arg_ptr));
        }
        PAS_ASSERT(!pas_mul_uintptr_overflow(extent_limit, 2, &extent_limit));
    }
}

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

