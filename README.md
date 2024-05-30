# Fil-C 0.666

This is a spare-time personal project that I'm doing for fun. It's called
Fil-C.

What is it? It's a totally memory-safe version of C and C++. All memory safety
errors are caught as Fil-C panics. Fil-C achieves this using a combination of
real-time garbage collection and monotonic capabilities (all pointers are 128
bits and have of those bits are under strict GC control). Every fundamental C
operation (as seen in LLVM IR) is checked against the capability. Fil-C has no
`unsafe` escape hatch of any kind.

What is it not? It's not production-tested. There are probably things I've
missed. It's slow (I haven't done any optimizations yet). And, it has no ABI
compatibility with classic C or C++.

## Requirements

Fil-C only works on Apple Silicon Macs and FreeBSD 14 on X86_64, for now.

## Getting Started

If you downloaded Fil-C binaries, run:

    ./setup.sh

If you downloaded Fil-C source, run:

    ./setup_gits.sh
    ./build_all.sh

Then you'll be able to use Fil-C from within this directory. There is no way to
"install" it, because that would be a crazy thing to do for such an
experimental piece of software!

## Things That Work

Included in the binary distribution (or if you build from source using
build_all.sh) is:

- Memory-safe SSH client and server.

- Memory-safe curl.

- Memory-safe libz and openssl.

- Memory-safe libc (based on musl) and libc++ (based on LLVM's libc++).

- A version of clang that you can use to compile C or C++ programs.

Fil-C really does catch all of the stuff that makes memory safety in C hard,
like:

- Out-of-bounds on the heap or stack.

- Use-after free (also heap or stack).

- Type confusion between pointers and non-pointers.

- Type errors arising from linking.

- Type errors arising from misuse of va_lists.

- Lots of other stuff.

Fil-C supports tricky features like threads, signal handling, and even mmap.

## Things That Don't Work

Fil-C is not a complete product. Lots of stuff isn't done!

- Fil-C doesn't catch stack exhaustion (i.e. recursing until you run out of
  stack).

- Fil-C doesn't do anything about UB not related to memory, yet. For example,
  dividing by zero may take the compiler down a weird path.

- C++ exception handling. Also, RTTI probably doesn't work yet.

- I haven't even done the most obvious performance optimizations. Fil-C is
  super slow right now!

- Probably other stuff, too!

These are all things that can be fixed. They just haven't been, yet.

## Learn More

The best write-up of Fil-C is
[the manifesto](https://github.com/pizlonator/llvm-project-deluge/blob/deluge/Manifesto.md).

You can also e-mail me: pizlo@mac.com

And follow my rants on [Twitter](https://x.com/filpizlo).

I don't have a bug database or anything like that, so bothering me on Twitter
or over e-mail is your best bet, probably. And I might ignore you anyway.

No guarantees!

