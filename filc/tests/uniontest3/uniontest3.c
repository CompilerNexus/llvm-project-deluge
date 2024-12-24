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

#include <stdfil.h>
#include <stdbool.h>
#include <string.h>

enum config_type
{
    PGC_BOOL,
    PGC_INT,
    PGC_REAL,
    PGC_STRING,
    PGC_ENUM,
};

union config_var_val
{
    bool boolval;
    int intval;
    double realval[4];
    char* stringval;
    int enumval;
};

typedef struct config_var_value
{
    union config_var_val val;
    void* extra;
} config_var_value;

__attribute__((__noinline__)) void do_things(config_var_value* in_ptr,
                                             config_var_value* out_ptr,
                                             enum config_type type)
{
    config_var_value in = *in_ptr;
    config_var_value out;
    switch (type) {
    case PGC_BOOL:
        out.val.boolval = !in.val.boolval;
        break;
    case PGC_INT:
        out.val.intval = in.val.intval + 666;
        break;
    case PGC_REAL:
        out.val.realval[3] = in.val.realval[3] * 4.2;
        break;
    case PGC_STRING:
        out.val.stringval = zasprintf("hello %s", in.val.stringval);
        break;
    case PGC_ENUM:
        out.val.enumval = in.val.enumval + 42;
        break;
    default:
        ZASSERT(!"Should not get here");
        break;
    }
    *out_ptr = out;
}

int main()
{
    config_var_value in;
    config_var_value out;

    in.val.boolval = true;
    do_things(&in, &out, PGC_BOOL);
    ZASSERT(!out.val.boolval);

    in.val.boolval = false;
    do_things(&in, &out, PGC_BOOL);
    ZASSERT(out.val.boolval);

    in.val.intval = 1410;
    do_things(&in, &out, PGC_INT);
    ZASSERT(out.val.intval == 1410 + 666);

    in.val.realval[3] = 2.4;
    do_things(&in, &out, PGC_REAL);
    ZASSERT(out.val.realval[3] == 2.4 * 4.2);

    in.val.stringval = "world";
    do_things(&in, &out, PGC_STRING);
    ZASSERT(!strcmp(out.val.stringval, "hello world"));

    in.val.enumval = 666;
    do_things(&in, &out, PGC_ENUM);
    ZASSERT(out.val.enumval == 666 + 42);

    return 0;
}

