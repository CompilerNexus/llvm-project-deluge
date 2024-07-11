#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(exp) do { \
    if ((exp)) \
        break; \
    fprintf(stderr, "%s:%d: %s: assertion %s failed.\n", \
            __FILE__, __LINE__, __PRETTY_FUNCTION__, #exp); \
    abort(); \
} while (0)

static const size_t num_nodes = 40000;
static const size_t repeat = 10;

struct foo;
typedef struct foo foo;

struct foo {
    foo* other;
    char* string;
};

static foo* root;

static void add_node(void)
{
    foo* node = (foo*)malloc(sizeof(foo));
    node->other = root;
    asprintf(&node->string, "node = %p", node);
    root = node;
}

static void build(void)
{
    size_t index;
    for (index = num_nodes; index--;)
        add_node();
}

static void remove_half(void)
{
    foo** ptr;
    size_t index;
    for (ptr = &root, index = 0; *ptr; index++) {
        if ((index & 1))
            ptr = &(*ptr)->other;
        else
            *ptr = (*ptr)->other;
    }
}

static void add_half(void)
{
    size_t index;
    for (index = num_nodes / 2; index--;)
        add_node();
}

static void verify(void)
{
    foo* node;
    for (node = root; node; node = node->other) {
        char* str;
        asprintf(&str, "node = %p", node);
        ASSERT(!strcmp(node->string, str));
    }
}

int main()
{
    build();
    verify();
    size_t index;
    for (index = 1; index <= repeat; ++index) {
        printf("Iteration %zu\n", index);
        remove_half();
        verify();
        add_half();
        verify();
    }
    printf("Success!\n");
    return 0;
}

