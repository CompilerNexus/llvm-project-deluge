int thingy(void)
{
    return 666;
}

extern __typeof(thingy) stuff __attribute__((__weak__, __alias__("thingy")));

int foo(void)
{
    return 42;
}

__asm__(".filc_alias foo, bar");

int blah(void)
{
    return 1410;
}

__asm__(".filc_weak_alias blah, bleh");

int fuzz(void)
{
    return 111;
}

int buzz(void);

int fizz(void)
{
    return buzz();
}

__asm__(".filc_weak_alias fuzz, buzz");

int wombat(int x, int y);

__asm__(".filc_weak_alias wombat, baz");

int red(int x, int y);
int blue(int x, int y);
__asm__(".filc_weak_alias red, blue");

extern int green;
extern int yellow;
__asm__(".filc_weak_alias green, yellow");


