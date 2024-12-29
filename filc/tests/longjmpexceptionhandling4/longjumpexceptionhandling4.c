/* This test is based on PostgreSQL code.

PostgreSQL Database Management System
(formerly known as Postgres, then as Postgres95)

Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group

Portions Copyright (c) 1994, The Regents of the University of California

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose, without fee, and without a written agreement
is hereby granted, provided that the above copyright notice and this
paragraph and the following two paragraphs appear in all copies.

IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATIONS TO
PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS. */

#include <setjmp.h>
#include <stdfil.h>
#include <stdlib.h>
#include <stdbool.h>

#define PG_TRY(...)  \
	do { \
		sigjmp_buf *_save_exception_stack##__VA_ARGS__ = PG_exception_stack; \
		sigjmp_buf _local_sigjmp_buf##__VA_ARGS__; \
		bool _do_rethrow##__VA_ARGS__ = false; \
		if (sigsetjmp(_local_sigjmp_buf##__VA_ARGS__, 0) == 0) \
		{ \
			PG_exception_stack = &_local_sigjmp_buf##__VA_ARGS__

#define PG_CATCH(...)	\
		} \
		else \
		{ \
			PG_exception_stack = _save_exception_stack##__VA_ARGS__

#define PG_FINALLY(...) \
		} \
		else \
			_do_rethrow##__VA_ARGS__ = true; \
		{ \
			PG_exception_stack = _save_exception_stack##__VA_ARGS__

#define PG_END_TRY(...)  \
		} \
		if (_do_rethrow##__VA_ARGS__) \
				PG_RE_THROW(); \
		PG_exception_stack = _save_exception_stack##__VA_ARGS__; \
	} while (0)

#define PG_RE_THROW()  \
	pg_re_throw()

static sigjmp_buf *PG_exception_stack = NULL;

static __attribute__((__noinline__)) void pg_re_throw(void) __attribute__((__noreturn__))
{
    if (PG_exception_stack != NULL)
        siglongjmp(*PG_exception_stack, 1);
    else
        ZASSERT(!"Attempt to rethrow outside of try block.");
    ZASSERT(!"Should not get here");
}

static __attribute__((__noinline__)) void Throw(void) __attribute__((__noreturn__))
{
    pg_re_throw();
}

static bool did_do_catch = false;
static bool did_run_after_bar = false;

static __attribute__((__noinline__)) void bar(bool should_throw)
{
    if (should_throw)
        Throw();
}

static __attribute__((__noinline__)) void foo(bool should_throw)
{
    unsigned count;
    for (count = 100; count--;) {
        did_run_after_bar = false;
        did_do_catch = false;
        PG_TRY();
        {
            bar(should_throw && !count);
            did_run_after_bar = true;
        }
        PG_CATCH();
        {
            did_do_catch = true;
            PG_RE_THROW();
        }
        PG_END_TRY();
        if (count || !should_throw) {
            ZASSERT(did_run_after_bar);
            ZASSERT(!did_do_catch);
        } else
            ZASSERT(!"Should not get here");
    }
}

int main()
{
    unsigned count;
    for (count = 100; count--;) {
        bool did_catch = false;
        bool did_run_after_foo = false;
        PG_TRY();
        {
            foo(!count);
            did_run_after_foo = true;
        }
        PG_CATCH();
        {
            did_catch = true;
        }
        PG_END_TRY();
        if (count) {
            ZASSERT(!did_catch);
            ZASSERT(!did_do_catch);
            ZASSERT(did_run_after_bar);
            ZASSERT(did_run_after_foo);
        } else {
            ZASSERT(did_catch);
            ZASSERT(did_do_catch);
            ZASSERT(!did_run_after_bar);
            ZASSERT(!did_run_after_foo);
        }
    }
    return 0;
}

