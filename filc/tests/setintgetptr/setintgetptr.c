#include <stdlib.h>
#include <stdio.h>

int main()
{
    int* ptr = malloc(100);
    *ptr = 42;
    printf("tak\n");
    char** ptr2 = (char**)ptr;
    printf("%s\n", *ptr2);
    printf("nie\n");
    return 0;
}

