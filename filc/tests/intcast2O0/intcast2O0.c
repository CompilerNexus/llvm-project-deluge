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

#include <inttypes.h>
#include <stdfil.h>
#include <string.h>

#define TYPEALIGN(ALIGNVAL,LEN)                                         \
	(((uintptr_t) (LEN) + ((ALIGNVAL) - 1)) & ~((uintptr_t) ((ALIGNVAL) - 1)))

#define ALIGNOF_SHORT 2
#define ALIGNOF_INT 4
#define ALIGNOF_LONG 8
#define ALIGNOF_DOUBLE 8

#define SHORTALIGN(LEN)			TYPEALIGN(ALIGNOF_SHORT, (LEN))
#define INTALIGN(LEN)			TYPEALIGN(ALIGNOF_INT, (LEN))
#define LONGALIGN(LEN)			TYPEALIGN(ALIGNOF_LONG, (LEN))
#define DOUBLEALIGN(LEN)		TYPEALIGN(ALIGNOF_DOUBLE, (LEN))

#define TYPALIGN_INT 4
#define TYPALIGN_CHAR 1
#define TYPALIGN_DOUBLE 8
#define TYPALIGN_SHORT 2

#define AssertMacro(exp) ({ ZASSERT(exp); 0; })

#define att_align_nominal(cur_offset, attalign) \
( \
	((attalign) == TYPALIGN_INT) ? INTALIGN(cur_offset) : \
	 (((attalign) == TYPALIGN_CHAR) ? (uintptr_t) (cur_offset) : \
	  (((attalign) == TYPALIGN_DOUBLE) ? DOUBLEALIGN(cur_offset) : \
	   ( \
			AssertMacro((attalign) == TYPALIGN_SHORT), \
			SHORTALIGN(cur_offset) \
	   ))) \
)

__attribute__((__noinline__)) void* foo(void* ptr, int attalign)
{
    return (void*)att_align_nominal(ptr, attalign);
}

int main()
{
    const char* str = (const char*)foo("hello", ALIGNOF_SHORT);
    ZASSERT(!strcmp(str, "hello"));
    return 0;
}

