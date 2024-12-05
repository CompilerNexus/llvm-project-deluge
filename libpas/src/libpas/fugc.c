/*
 * Copyright (c) 2024 Epic Games, Inc. All Rights Reserved.
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

#include "fugc.h"
#include "pas_fd_stream.h"
#include "verse_heap_mark_bits_page_commit_controller.h"
#include "verse_heap_object_set_inlines.h"
#include <signal.h>

#if PAS_ENABLE_FILC

/* FUGC: Fil's Unbelievable Garbage Collector!
   
   This implements Phil's Concurrent Marking (aka on-the-fly grey-stack Dijkstra with a soft
   handshake fixpoint) with verse_heap SIMD turbosweep based on libpas.
   
   It's a simple but effective algorithm. There is no stop-the-world unless we wanted to have such
   a thing for debugging (i.e. if there's a GC bug, we might want to run this in stop-the-world
   mode to triage if the bug has to do with concurrency or not). Also maybe someday we'll want to
   add some thread stoppage for GC pacing (in case the mutator out-allocates us).
   
   It would be easy to make the collector loop parallel, but it isn't, yet.
   
   It would be possible to add a concurrent nursery GC, which would have the dual effect of reducing
   floating garbage and increasing mutator throughput.
   
   It's a nonmoving GC, but it redirects ptrs to free objects to the free singleton, which enables
   freed objects to definitely be freed. Except, it won't redirect ptrs from certain roots (like
   ones coming from the stack).
   
   This GC can be suspended and resumed for the purposes of fork(). Suspension can happen while the
   GC is active. That's made possible thanks to polling collector_suspend_requested and structuring
   the GC loop as a state machine.

   There are no destructors, except for internal use. There are no weak references. No weak maps,
   either. This GC is just here so we don't have to trust the C-hacking Cowboy (TM) when they call
   free(). But the fact that free() flags the object for killage (because of the redirect-ptr-to-
   free-object thing) means that we don't have to worry about GC-induced leaks in C programs that
   use free() like they would have if they were written against a legacy malloc.

   This code looks simple, but it totally isn't. It's relying on the filc_runtime and the
   FilPizlonator pass to do a lot of heavy lifting for us. And it's relying on the excellent
   verse_heap that I wrote for the Verse VM, which in turn relies on the excellent libpas malloc,
   which I wrote for WebKit. */

pas_heap* fugc_default_heap;
pas_heap* fugc_destructor_heap;
verse_heap_object_set* fugc_destructor_set;
verse_heap_object_set* fugc_scribble_set;

static pas_system_mutex collector_thread_state_lock;
static pas_system_condition collector_thread_state_cond;
static bool collector_thread_is_running = false;

#define COLLECTOR_CONTROL_REQUEST_SUSPEND 1u
#define COLLECTOR_CONTROL_REQUEST_HANDSHAKE 2u

static unsigned collector_control_request = 0;

static unsigned collector_suspend_count = 0;

static uint64_t completed_cycle;
static uint64_t requested_cycle;

static filc_object_array global_stack;
static filc_object_array local_stack;
static pas_lock global_stack_lock;

static size_t destruct_size = SIZE_MAX;
static size_t destruct_index = SIZE_MAX;

static size_t scribble_size = SIZE_MAX;
static size_t scribble_index = SIZE_MAX;

static size_t sweep_size = SIZE_MAX;
static size_t sweep_index = SIZE_MAX;

static size_t live_bytes_at_start = SIZE_MAX;
static size_t live_bytes_before_sweeping = SIZE_MAX;

static size_t minimum_threshold;

static double overall_start_time;
static double mark_end_time;
static double overall_end_time;

#define VERBOSE_HANDSHAKE_STACKS 6
#define VERBOSE_HANDSHAKES 5
#define VERBOSE_PHASES 4
#define VERBOSE_BEGIN 3
#define VERBOSE_BREAKDOWN 2
#define VERBOSE_CYCLES 1

static unsigned verbose;
static bool should_stop_the_world;
static bool should_scribble;
static bool scribble_concurrently;
static bool rage_mode;

enum collector_state {
    collector_waiting,
    collector_marking,
    collector_destructing,
    collector_scribbling,
    collector_sweeping
};

typedef enum collector_state collector_state;

static collector_state current_collector_state = collector_waiting;

static const char* pollcheck_message_for_thread(filc_thread* thread)
{
    if (filc_get_my_thread() == thread)
        return "by mutator";
    PAS_ASSERT(!filc_get_my_thread());
    return "by collector";
}

static pas_lock dump_handshake_lock = PAS_LOCK_INITIALIZER;

static void dump_handshake(filc_thread* thread, const char* handshake_name)
{
    if (verbose >= VERBOSE_HANDSHAKES) {
        if (verbose >= VERBOSE_HANDSHAKE_STACKS)
            pas_lock_lock(&dump_handshake_lock);
        pas_log("[%d] fugc: %s handshake with thread %u (%p) %s\n",
                pas_getpid(), handshake_name, thread->tid, thread, pollcheck_message_for_thread(thread));
        if (verbose >= VERBOSE_HANDSHAKE_STACKS) {
            filc_thread_dump_stack(thread, pas_log_stream);
            pas_lock_unlock(&dump_handshake_lock);
        }
    }
}

static void no_op_pollcheck_callback(filc_thread* thread, void* arg)
{
    PAS_ASSERT(thread);
    PAS_ASSERT(!arg);
    dump_handshake(thread, "no_op");
}

static void stop_allocators_pollcheck_callback(filc_thread* thread, void* arg)
{
    PAS_ASSERT(!arg);
    dump_handshake(thread, "stop_allocators");
    filc_thread_stop_allocators(thread);
}

static void marking_pollcheck_callback(filc_thread* thread, void* arg)
{
    PAS_ASSERT(!arg);
    dump_handshake(thread, "marking");
    filc_thread_stop_allocators(thread);
    filc_thread_mark_roots(thread);
    filc_thread_donate(thread);
}

static void after_marking_pollcheck_callback(filc_thread* thread, void* arg)
{
    PAS_ASSERT(!arg);
    dump_handshake(thread, "after_marking");
    filc_thread_stop_allocators(thread);
    filc_thread_sweep_mark_stack(thread);
}

static void mark_outgoing_signal_handler_ptrs(filc_object_array* stack, filc_signal_handler* signal_handler)
{
    /* I guess that instead, we could just assert that this thing is global. But, like, whatever. */
    fugc_mark_or_free_flight(stack, &signal_handler->function_ptr);
}

static void mark_outgoing_special_ptrs(filc_object_array* stack, filc_object* object)
{
    PAS_TESTING_ASSERT(filc_object_is_special(object));
    filc_special_type special_type = filc_object_special_type(object);
    switch (special_type) {
    case FILC_SPECIAL_TYPE_FUNCTION:
    case FILC_SPECIAL_TYPE_DL_HANDLE:
        break;
    case FILC_SPECIAL_TYPE_SIGNAL_HANDLER:
        mark_outgoing_signal_handler_ptrs(
            stack, (filc_signal_handler*)filc_object_special_payload_with_manual_tracking(object));
        break;
    case FILC_SPECIAL_TYPE_THREAD:
        filc_thread_mark_outgoing_ptrs(
            (filc_thread*)filc_object_special_payload_with_manual_tracking(object), stack);
        break;
    case FILC_SPECIAL_TYPE_PTR_TABLE:
        filc_ptr_table_mark_outgoing_ptrs(
            (filc_ptr_table*)filc_object_special_payload_with_manual_tracking(object), stack);
        break;
    case FILC_SPECIAL_TYPE_PTR_TABLE_ARRAY:
        filc_ptr_table_array_mark_outgoing_ptrs(
            (filc_ptr_table_array*)filc_object_special_payload_with_manual_tracking(object), stack);
        break;
    case FILC_SPECIAL_TYPE_JMP_BUF:
        filc_jmp_buf_mark_outgoing_ptrs(
            (filc_jmp_buf*)filc_object_special_payload_with_manual_tracking(object), stack);
        break;
    case FILC_SPECIAL_TYPE_EXACT_PTR_TABLE:
        filc_exact_ptr_table_mark_outgoing_ptrs(
            (filc_exact_ptr_table*)filc_object_special_payload_with_manual_tracking(object), stack);
        break;
    default:
        pas_log("Got a bad special ptr type: ");
        filc_special_type_dump(special_type, pas_log_stream);
        pas_log("\n");
        pas_log("Object: ");
        filc_object_dump(object, pas_log_stream);
        pas_log("\n");
        PAS_ASSERT(!"Bad special word type");
        break;
    }
}

static void mark_outgoing_ptrs(filc_object_array* stack, filc_object* object)
{
    static const bool verbose = false;
    if (verbose)
        pas_log("Marking outgoing objects from %p\n", object);
    if (filc_object_is_special(object)) {
        mark_outgoing_special_ptrs(stack, object);
        return;
    }

    /* It's unusual for an object without an aux ptr to be placed on the mark stack, but we forgive
       cases like this anyway, since it might happen for globals. */
    char* aux_ptr = filc_object_aux_ptr(object);
    if (PAS_UNLIKELY(!aux_ptr))
        return;
    if (!(filc_object_get_flags(object) & FILC_OBJECT_FLAG_GLOBAL_AUX))
        verse_heap_set_is_marked_relaxed(aux_ptr, true);
    /* The only way for the aux to already be marked is if it's black, but then that means that all of
       the things it points to are already marked (either black-allocated atomic boxes or things
       marked with the store barrier).
    
       So, a possible optimization would be to skip this loop if the aux is already marked. */
    size_t size = filc_object_size_not_null(object);
    size_t offset;
    PAS_ASSERT(sizeof(filc_lower_or_box) == FILC_WORD_SIZE);
    PAS_ASSERT(sizeof(filc_lower_or_box) == sizeof(void*));
    for (offset = 0; offset < size; offset += sizeof(filc_lower_or_box)) {
        filc_lower_or_box* lower_or_box_ptr = (filc_lower_or_box*)(aux_ptr + offset);
        fugc_mark_or_free_lower_or_box(stack, lower_or_box_ptr);
    }
}

static void destruct_object_callback(void* allocation, void* arg)
{
    static const bool verbose = false;
    PAS_ASSERT(!arg);
    if (verbose) {
        pas_log("destruct_object: allocation = %p, starts with = %p\n",
                allocation, *(void**)allocation);
    }
    filc_object* object = filc_allocation_get_object(allocation);
    if (filc_object_get_flags(object) & FILC_OBJECT_FLAG_MMAP) {
        if (verbose)
            pas_log("Deleting mmap at %p\n", filc_object_lower(object));
        if (filc_object_get_flags(object) & FILC_OBJECT_FLAG_FREE)
            return;
        filc_unmap(filc_object_lower(object), filc_object_size(object));
        return;
    }
    if (verbose)
        pas_log("object = %p\n", object);
    PAS_ASSERT(filc_object_is_special(object));
    switch (filc_object_special_type(object)) {
    case FILC_SPECIAL_TYPE_THREAD:
        filc_thread_destruct((filc_thread*)filc_object_special_payload_with_manual_tracking(object));
        break;
    case FILC_SPECIAL_TYPE_PTR_TABLE:
        filc_ptr_table_destruct(
            (filc_ptr_table*)filc_object_special_payload_with_manual_tracking(object));
        break;
    case FILC_SPECIAL_TYPE_EXACT_PTR_TABLE:
        filc_exact_ptr_table_destruct(
            (filc_exact_ptr_table*)filc_object_special_payload_with_manual_tracking(object));
        break;
    default:
        PAS_ASSERT(!"Encountered object in destructor space that should not have destructor.");
        break;
    }
}

static void scribble_object_callback(void* allocation, void* arg)
{
    static const bool verbose = false;
    PAS_ASSERT(!arg);
    if (verbose) {
        pas_log("scribble_object: allocation = %p, starts with = %p\n",
                allocation, *(void**)allocation);
    }
    void* start = (void*)verse_heap_find_allocated_object_start((uintptr_t)allocation);
    PAS_ASSERT(start <= allocation);
    size_t size = verse_heap_get_allocation_size((uintptr_t)start);
    PAS_ASSERT(size);
    memset(start, 0xac, size);
}

static void wait_and_start_marking(void)
{
    PAS_ASSERT(!filc_is_marking);
    PAS_ASSERT(current_collector_state == collector_waiting);
    PAS_ASSERT(completed_cycle <= requested_cycle);

    if (verbose >= VERBOSE_PHASES) {
        pas_log("[%d] fugc: waiting with threshold %zu bytes, currently at %zu.\n",
                pas_getpid(), verse_heap_live_bytes_trigger_threshold, verse_heap_live_bytes);
    }
    
    while (completed_cycle == requested_cycle
           && verse_heap_live_bytes < verse_heap_live_bytes_trigger_threshold
           && !rage_mode) {
        pas_system_mutex_lock(&collector_thread_state_lock);
        PAS_ASSERT(completed_cycle <= requested_cycle);
        while (completed_cycle == requested_cycle
               && verse_heap_live_bytes < verse_heap_live_bytes_trigger_threshold
               && !rage_mode
               && !collector_control_request)
            pas_system_condition_wait(&collector_thread_state_cond, &collector_thread_state_lock);
        pas_system_mutex_unlock(&collector_thread_state_lock);
        
        if (collector_control_request)
            return;
        
        PAS_ASSERT(completed_cycle <= requested_cycle);
    }

    if (verbose >= VERBOSE_CYCLES)
        overall_start_time = pas_get_time_in_milliseconds();

    if (should_stop_the_world)
        filc_stop_the_world();
    
    pas_system_mutex_lock(&collector_thread_state_lock);
    PAS_ASSERT(completed_cycle <= requested_cycle);
    if (completed_cycle == requested_cycle)
        requested_cycle++;
    PAS_ASSERT(completed_cycle <= requested_cycle);
    pas_system_mutex_unlock(&collector_thread_state_lock);

    PAS_ASSERT(live_bytes_at_start == SIZE_MAX);
    live_bytes_at_start = verse_heap_live_bytes;

    if (verbose >= VERBOSE_PHASES) {
        pas_log("[%d] fugc: starting cycle %" PRIu64 " with %zu live bytes\n",
                pas_getpid(), completed_cycle + 1, live_bytes_at_start);
    } else if (verbose >= VERBOSE_BEGIN) {
        pas_log("[%d] fugc: starting cycle %" PRIu64 " with %zu kb\n",
                pas_getpid(), completed_cycle + 1, live_bytes_at_start / 1024);
    }

    verse_heap_mark_bits_page_commit_controller_lock();
    filc_is_marking = true;
    filc_soft_handshake(no_op_pollcheck_callback, NULL);
    
    verse_heap_start_allocating_black_before_handshake();
    filc_soft_handshake(stop_allocators_pollcheck_callback, NULL);

    filc_object_array_construct(&local_stack);
    /* FIXME: You could imagine this being a place we can suspend. */
    filc_mark_global_roots(&local_stack);

    current_collector_state = collector_marking;
}

static void mark_and_start_destructing(void)
{
    if (verbose >= VERBOSE_PHASES)
        pas_log("[%d] fugc: marking\n", pas_getpid());

    PAS_ASSERT(filc_is_marking);
    PAS_ASSERT(current_collector_state == collector_marking);

    for (;;) {
        filc_soft_handshake(marking_pollcheck_callback, NULL);
        
        pas_lock_lock(&global_stack_lock);
        filc_object_array_pop_all_from_and_push_to(&global_stack, &local_stack);
        pas_lock_unlock(&global_stack_lock);
        
        if (!local_stack.num_objects)
            break;
        
        filc_object* object;
        while (!collector_control_request && (object = filc_object_array_pop(&local_stack)))
            mark_outgoing_ptrs(&local_stack, object);

        if (collector_control_request)
            return;
    }
    
    filc_is_marking = false;
    
    if (verbose >= VERBOSE_CYCLES)
        mark_end_time = pas_get_time_in_milliseconds();

    PAS_ASSERT(destruct_size == SIZE_MAX);
    PAS_ASSERT(destruct_index == SIZE_MAX);
    verse_heap_object_set_start_iterate_before_handshake(fugc_destructor_set);
    filc_soft_handshake(after_marking_pollcheck_callback, NULL);
    destruct_size = verse_heap_object_set_start_iterate_after_handshake(fugc_destructor_set);
    destruct_index = 0;

    current_collector_state = collector_destructing;
}

static void destruct_and_start_scribbling(void)
{
    if (verbose >= VERBOSE_PHASES) {
        pas_log("[%d] fugc: marking took %lf ms; destructing\n",
                pas_getpid(), mark_end_time - overall_start_time);
    }

    PAS_ASSERT(!filc_is_marking);
    PAS_ASSERT(current_collector_state == collector_destructing);

    for (; destruct_index < destruct_size; destruct_index += 10) {
        if (collector_control_request)
            return;
        size_t next_destruct_index = pas_min_uintptr(destruct_index + 10, destruct_size);
        verse_heap_object_set_iterate_range_inline(
            fugc_destructor_set, destruct_index, next_destruct_index,
            verse_heap_iterate_unmarked, destruct_object_callback, NULL);
    }

    destruct_index = SIZE_MAX;
    destruct_size = SIZE_MAX;

    verse_heap_object_set_end_iterate(fugc_destructor_set);

    if (should_scribble) {
        PAS_ASSERT(scribble_size == SIZE_MAX);
        PAS_ASSERT(scribble_index == SIZE_MAX);
        verse_heap_object_set_start_iterate_before_handshake(fugc_scribble_set);
        filc_soft_handshake(stop_allocators_pollcheck_callback, NULL);
        scribble_size = verse_heap_object_set_start_iterate_after_handshake(fugc_scribble_set);
        scribble_index = 0;
    }

    current_collector_state = collector_scribbling;
}

static void scribble_and_start_sweeping(void)
{
    if (verbose >= VERBOSE_PHASES)
        pas_log("[%d] fugc: scribbling\n", pas_getpid());
        
    PAS_ASSERT(!filc_is_marking);
    PAS_ASSERT(current_collector_state == collector_scribbling);

    /* Scribbling is a debug mode for catching GC bugs by overwriting dead objects with garbage. */
    if (should_scribble) {
        /* By default, we stop the world to scribble, because scribbling concurrently means positive
           feedback in the concurrent GC. It can easily lead to memory exhaustion. */
        if (!scribble_concurrently)
            filc_stop_the_world();
        
        for (; scribble_index < scribble_size; scribble_index += 10) {
            if (collector_control_request) {
                if (!scribble_concurrently)
                    filc_resume_the_world();
                return;
            }
            size_t next_scribble_index = pas_min_uintptr(scribble_index + 10, scribble_size);
            verse_heap_object_set_iterate_range_inline(
                fugc_scribble_set, scribble_index, next_scribble_index,
                verse_heap_iterate_unmarked, scribble_object_callback, NULL);
        }
        
        scribble_index = SIZE_MAX;
        scribble_size = SIZE_MAX;
        
        verse_heap_object_set_end_iterate(fugc_scribble_set);

        if (!scribble_concurrently)
            filc_resume_the_world();
    }

    PAS_ASSERT(live_bytes_before_sweeping == SIZE_MAX);
    live_bytes_before_sweeping = verse_heap_live_bytes;

    verse_heap_start_sweep_before_handshake();
    filc_soft_handshake(stop_allocators_pollcheck_callback, NULL);
    PAS_ASSERT(!global_stack.num_objects);
    PAS_ASSERT(!local_stack.num_objects);
    filc_object_array_reset(&global_stack);
    filc_object_array_destruct(&local_stack);

    PAS_ASSERT(sweep_size == SIZE_MAX);
    PAS_ASSERT(sweep_index == SIZE_MAX);
    sweep_size = verse_heap_start_sweep_after_handshake();
    sweep_index = 0;

    current_collector_state = collector_sweeping;
}

static void sweep_and_end(void)
{
    if (verbose >= VERBOSE_PHASES)
        pas_log("[%d] fugc: sweeping\n", pas_getpid());

    PAS_ASSERT(!filc_is_marking);
    PAS_ASSERT(current_collector_state == collector_sweeping);
    
    for (; sweep_index < sweep_size; sweep_index += 10) {
        if (collector_control_request)
            return;
        size_t next_sweep_index = pas_min_uintptr(sweep_index + 10, sweep_size);
        verse_heap_sweep_range(sweep_index, next_sweep_index);
    }

    sweep_index = SIZE_MAX;
    sweep_size = SIZE_MAX;
    
    verse_heap_end_sweep();
    verse_heap_mark_bits_page_commit_controller_unlock();
    
    pas_system_mutex_lock(&collector_thread_state_lock);
    completed_cycle++;
    /* It's unusual but possible that we sweep more bytes than we thought were live, because it's
       possible for new objects to be allocated after we snapshot live bytes and then for those to
       be swept. */
    size_t surviving_bytes;
    if (verse_heap_swept_bytes > live_bytes_at_start)
        surviving_bytes = 0;
    else
        surviving_bytes = live_bytes_at_start - verse_heap_swept_bytes;
    if (verbose >= VERBOSE_CYCLES) {
        overall_end_time = pas_get_time_in_milliseconds();
        if (verbose >= VERBOSE_PHASES) {
            pas_log("[%d] fugc: destructing and sweeping took %lf ms; completed cycle %" PRIu64
                    " in %lf ms, swept %zu bytes, "
                    "survived %zu bytes, have %zu live bytes\n",
                    pas_getpid(), overall_end_time - mark_end_time, completed_cycle,
                    overall_end_time - overall_start_time,
                    verse_heap_swept_bytes, surviving_bytes, verse_heap_live_bytes);
        } else if (verbose >= VERBOSE_BREAKDOWN) {
            pas_log("[%d] fugc: %zu kb -> %zu kb -> %zu kb + %zu kb (floated) in %.3lf ms "
                    "(%.0lf%% marking)\n",
                    pas_getpid(), live_bytes_at_start / 1024, live_bytes_before_sweeping / 1024,
                    surviving_bytes / 1024,
                    verse_heap_live_bytes > surviving_bytes
                    ? (verse_heap_live_bytes - surviving_bytes) / 1024 : 0,
                    overall_end_time - overall_start_time,
                    100. * (mark_end_time - overall_start_time)
                    / (overall_end_time - overall_start_time));
        } else {
            pas_log("[%d] fugc: %zu kb -> %zu kb in %.3lf ms\n",
                    pas_getpid(), live_bytes_at_start / 1024, surviving_bytes / 1024,
                    overall_end_time - overall_start_time);
        }
    }
    live_bytes_at_start = SIZE_MAX;
    live_bytes_before_sweeping = SIZE_MAX;
    size_t proposed_threshold = (size_t)(surviving_bytes * 1.5);
    PAS_ASSERT(proposed_threshold >= surviving_bytes);
    PAS_ASSERT(!surviving_bytes || proposed_threshold < surviving_bytes * 2);
    verse_heap_live_bytes_trigger_threshold = pas_max_uintptr(proposed_threshold, minimum_threshold);
    pas_system_condition_broadcast(&collector_thread_state_cond);
    pas_system_mutex_unlock(&collector_thread_state_lock);

    if (should_stop_the_world)
        filc_resume_the_world();

    current_collector_state = collector_waiting;
}

static pas_thread_return_type collector_thread(void* arg)
{
    PAS_ASSERT(!arg);
    
    PAS_ASSERT(collector_thread_is_running);
    
    if (verbose >= VERBOSE_PHASES)
        pas_log("[%d] fugc: thread started\n", pas_getpid());

    while (!(collector_control_request & COLLECTOR_CONTROL_REQUEST_SUSPEND)) {
        if ((collector_control_request & COLLECTOR_CONTROL_REQUEST_HANDSHAKE)) {
            if (verbose >= VERBOSE_PHASES)
                pas_log("[%d] fugc: handshaking with mutator\n", pas_getpid());
            pas_system_mutex_lock(&collector_thread_state_lock);
            collector_control_request &= ~COLLECTOR_CONTROL_REQUEST_HANDSHAKE;
            pas_system_condition_broadcast(&collector_thread_state_cond);
            pas_system_mutex_unlock(&collector_thread_state_lock);
        }
        switch (current_collector_state) {
        case collector_waiting:
            wait_and_start_marking();
            continue;
        case collector_marking:
            mark_and_start_destructing();
            continue;
        case collector_destructing:
            destruct_and_start_scribbling();
            continue;
        case collector_scribbling:
            scribble_and_start_sweeping();
            continue;
        case collector_sweeping:
            sweep_and_end();
            continue;
        }
        PAS_ASSERT(!"Invalid collector state");
    }

    if (verbose >= VERBOSE_PHASES)
        pas_log("[%d] fugc: thread stopping\n", pas_getpid());

    pas_thread_local_cache_destroy(pas_lock_is_not_held);

    pas_system_mutex_lock(&collector_thread_state_lock);
    collector_thread_is_running = false;
    pas_system_condition_broadcast(&collector_thread_state_cond);
    pas_system_mutex_unlock(&collector_thread_state_lock);
    
    return PAS_THREAD_RETURN_VALUE;
}

static void trigger_callback(void)
{
    pas_system_mutex_lock(&collector_thread_state_lock);
    pas_system_condition_broadcast(&collector_thread_state_cond);
    pas_system_mutex_unlock(&collector_thread_state_lock);
}

static void create_thread(void)
{
    sigset_t fullset;
    pas_reasonably_fill_sigset(&fullset);
    sigset_t oldset;
    PAS_ASSERT(!pthread_sigmask(SIG_BLOCK, &fullset, &oldset));
    pas_create_detached_thread(collector_thread, NULL);
    PAS_ASSERT(!pthread_sigmask(SIG_SETMASK, &oldset, NULL));
}

void fugc_initialize_heaps(void)
{
    minimum_threshold = filc_get_size_env("FUGC_MIN_THRESHOLD", 1024 * 1024);
    verse_heap_live_bytes_trigger_threshold = minimum_threshold;
    verse_heap_live_bytes_trigger_callback = trigger_callback;

    verbose = filc_get_unsigned_env("FUGC_VERBOSE", 0);
    should_stop_the_world = filc_get_bool_env("FUGC_STW", false);
    should_scribble = filc_get_bool_env("FUGC_SCRIBBLE", false);
    scribble_concurrently = filc_get_bool_env("FUGC_SCRIBBLE_CONCURRENTLY", false);
    if (scribble_concurrently)
        should_scribble = true;
    rage_mode = filc_get_bool_env("FUGC_RAGE_MODE", false);

    fugc_default_heap = verse_heap_create(1, 0, 0);
    fugc_destructor_heap = verse_heap_create(1, 0, 0);
    fugc_destructor_set = verse_heap_object_set_create();
    verse_heap_add_to_set(fugc_destructor_heap, fugc_destructor_set);

    if (should_scribble) {
        fugc_scribble_set = verse_heap_object_set_create();
        verse_heap_add_to_set(fugc_default_heap, fugc_scribble_set);
        verse_heap_add_to_set(fugc_destructor_heap, fugc_scribble_set);
    }
    
    verse_heap_did_become_ready_for_allocation();
}

void fugc_initialize_collector(void)
{
    pas_system_mutex_construct(&collector_thread_state_lock);
    pas_system_condition_construct(&collector_thread_state_cond);
    filc_object_array_construct(&global_stack);
    pas_lock_construct(&global_stack_lock);

    if (verbose >= VERBOSE_PHASES) {
        pas_log("[%d] fugc: initializing GC with %zu live bytes.\n",
                pas_getpid(), verse_heap_live_bytes);
    }

    collector_thread_is_running = true;
    create_thread();
}

void fugc_suspend(void)
{
    pas_system_mutex_lock(&collector_thread_state_lock);
    if (collector_suspend_count++) {
        pas_system_mutex_unlock(&collector_thread_state_lock);
        return;
    }
    PAS_ASSERT(collector_thread_is_running);
    PAS_ASSERT(!(collector_control_request & COLLECTOR_CONTROL_REQUEST_SUSPEND));
    collector_control_request |= COLLECTOR_CONTROL_REQUEST_SUSPEND;
    pas_system_condition_broadcast(&collector_thread_state_cond);
    while (collector_thread_is_running)
        pas_system_condition_wait(&collector_thread_state_cond, &collector_thread_state_lock);
    pas_system_mutex_unlock(&collector_thread_state_lock);
}

void fugc_resume(void)
{
    pas_system_mutex_lock(&collector_thread_state_lock);
    if (--collector_suspend_count) {
        pas_system_mutex_unlock(&collector_thread_state_lock);
        return;
    }
    PAS_ASSERT(!collector_thread_is_running);
    PAS_ASSERT(collector_control_request & COLLECTOR_CONTROL_REQUEST_SUSPEND);
    collector_control_request &= ~COLLECTOR_CONTROL_REQUEST_SUSPEND;
    collector_thread_is_running = true;
    create_thread();
    pas_system_mutex_unlock(&collector_thread_state_lock);
}

void fugc_handshake(void)
{
    pas_system_mutex_lock(&collector_thread_state_lock);
    PAS_ASSERT(completed_cycle <= requested_cycle);
    if (requested_cycle == completed_cycle) {
        pas_system_mutex_unlock(&collector_thread_state_lock);
        return;
    }
    collector_control_request |= COLLECTOR_CONTROL_REQUEST_HANDSHAKE;
    pas_system_condition_broadcast(&collector_thread_state_cond);
    while ((collector_control_request & COLLECTOR_CONTROL_REQUEST_HANDSHAKE)
           && collector_thread_is_running)
        pas_system_condition_wait(&collector_thread_state_cond, &collector_thread_state_lock);
    pas_system_mutex_unlock(&collector_thread_state_lock);
}

void fugc_donate(filc_object_array* mark_stack)
{
    if (!mark_stack->num_objects)
        return;
    pas_lock_lock(&global_stack_lock);
    PAS_ASSERT(filc_is_marking);
    filc_object_array_pop_all_from_and_push_to(mark_stack, &global_stack);
    pas_lock_unlock(&global_stack_lock);
}

static uint64_t request_impl(uint64_t offset)
{
    pas_system_mutex_lock(&collector_thread_state_lock);
    PAS_ASSERT(completed_cycle <= requested_cycle);
    if (requested_cycle + offset >= completed_cycle)
        requested_cycle++;
    PAS_ASSERT(completed_cycle <= requested_cycle);
    size_t result = requested_cycle;
    pas_system_condition_broadcast(&collector_thread_state_cond);
    pas_system_mutex_unlock(&collector_thread_state_lock);
    return result;
}

uint64_t fugc_request(void)
{
    return request_impl(0);
}

uint64_t fugc_request_fresh(void)
{
    return request_impl(1);
}

void fugc_wait(uint64_t cycle)
{
    pas_system_mutex_lock(&collector_thread_state_lock);
    while (completed_cycle < cycle)
        pas_system_condition_wait(&collector_thread_state_cond, &collector_thread_state_lock);
    pas_system_mutex_unlock(&collector_thread_state_lock);
}

bool fugc_is_stw(void)
{
    return should_stop_the_world;
}

void fugc_dump_setup(void)
{
    pas_log("    fugc minimum threshold: %zu\n", minimum_threshold);
    pas_log("    fugc verbose level: %u\n", verbose);
    pas_log("    fugc stop the world: %s\n", should_stop_the_world ? "yes" : "no");
    pas_log("    fugc scribble: %s\n",
            should_scribble
            ? (scribble_concurrently ? "yes, concurrently" : "yes")
            : "no");
    pas_log("    fugc rage mode: %s\n", rage_mode ? "yes" : "no");
}

#endif /* PAS_ENABLE_FILC */

#endif /* LIBPAS_ENABLED */

