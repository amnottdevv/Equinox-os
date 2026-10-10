# mtcc language reference

`mtcc` is the Equinox OS native C compiler: a single-pass
**lexer → parser → direct x86-32 code generator** that produces flat
ring-3 images (`.mrp`) or static ELF32, and that runs *inside* the OS
itself as well as on a host for testing.

It implements a **deliberate subset of C**. The subset is not a
reduction for its own sake — every included feature is there because
the in-OS userland (libc, tools, the compiler itself) needs it, and
every excluded feature is one whose cost in code size, compiler
complexity or runtime footprint outweighed its benefit on a 256 MB
hobby kernel.

This page is the normative reference for that subset. The syscall
surface a program can call is documented separately in
[MTCC_API.md](MTCC_API.md); the multi-file build and `.ruf` recipe
system in [SELF_HOSTING.md](SELF_HOSTING.md).

---

## 1. Types

| Type | Size | Notes |
| --- | --- | --- |
| `void` | — | return type only |
| `int` | 4 bytes | signed 32-bit (see *integer semantics*) |
| `char` | 1 byte | signed byte; `char[n]` arrays are byte arrays |
| `T*` | 4 bytes | pointer, one or two levels |
| `T[n]` | `n × elem` | one-dimensional array |
| `struct` / `union` | sum / max | see *Structures and unions* |
| `enum` | 4 bytes | enumerators are `int` constants |

Pointer arithmetic scales by the element size: on a `struct Point*`
where `struct Point` occupies 8 bytes, `p + 1` advances 8 bytes. This
holds for `++`, `--`, `+=`, `-=` and index expressions.

### Integer semantics

`int` is **processed as signed** throughout. There is no `unsigned`
keyword, no `long`, no `short`. The libc's `printf` does provide `%u`
which reinterprets the bits as unsigned decimal, and `printint()` is
unsigned-aware — but the compiler itself applies signed comparison and
signed division. Code that needs unsigned behaviour must mask or
reinterpret explicitly.

### Not available

`float`, `double`, `long double`, `long`, `short`, `unsigned`, and
Casts such as `(int)x` are rejected. There is no floating-point
support anywhere in the toolchain or the kernel runtime.

---

## 2. Structures and unions

mtcc has full `struct` and `union` support. This is the most
substantially expanded area of the compiler.

### Definition

```c
struct Point { int x; int y; };

union Mixed { int i; char c[4]; };

/* nested */
struct Box { struct Point tl; struct Point br; };

/* anonymous (no tag) */
struct { int id; char name[16]; } rec;
```

Definitions may appear at file scope or inside a block. Nested
structs are supported by reference — the inner type carries an index
into the compiler's definition table rather than being copied.

### Layout rule

Every field begins on a **4-byte boundary**, and a struct's size is
the sum of its (4-aligned) fields. A `union` places all fields at
offset 0 and its size is the 4-aligned size of its largest field.

This is deliberately simpler — and more wasteful — than the C ABI's
natural alignment. A `struct { char c; int n; }` occupies 8 bytes,
not 5. The tradeoff is intentional: a correctness-first layout that
is trivial to compute is worth a few wasted bytes on a compiler whose
primary job is to stay small and predictable.

### Access

```c
struct Point p;
p.x = 3;                 /* member, by value */
struct Point* q = &p;
q->y = 7;                /* member through pointer */
```

Both `.` and `->` are supported. The compiler tracks whether an
expression's register holds an object's *address* rather than its
value (the `is_ref` flag), which is what allows `q->y = 7` and
`arr[1].x = 5` to work without an explicit load.

### Passing and returning

**Structs and unions are passed and returned by pointer, never by
value.** A by-value parameter or return type is a hard compile error
with a message that says so:

```text
[ERROR] file.c:12: struct/union return by value not supported (use a pointer)
```

This restriction is not an oversight. Passing a struct by value on
i386 means copying a caller-computed object into a callee frame
through a hidden pointer; supporting it costs a calling-convention
path that nothing in the in-OS userland currently needs. The
alternative — `void get_point(struct Point* out)` — is explicit about
the copy and costs one `mov`.

```c
/* correct: fill through a pointer */
void make_point(struct Point* out) { out->x = 1; out->y = 2; }

/* correct: borrow a pointer */
int width(struct Box* b) { return b->br.x - b->tl.x; }
```

### Copy, arrays and initializers

Whole-struct assignment is a member-wise copy and is supported:

```c
struct Point a; a.x = 1; a.y = 2;
struct Point b; b = a;          /* b == {1, 2} */
```

Arrays of structs index and address normally:

```c
struct Point pts[3];
pts[0].y = 10;
struct Point* first = &pts[0];
```

`{…}` initializers work for structs, unions and enums, at **global
and local scope, nested, and for arrays of struct**. A union
initializer takes its first member.

```c
struct Point g = {3, 5};              /* global */

int run(void) {
    struct Point l = {7, 9};          /* local */
    struct Box   b = {{0, 0}, {10, 20}};   /* nested */
    struct Point table[2] = {{1, 1}, {2, 2}};  /* array of struct */
    ...
}
```

### Limits

| Resource | Cap |
| --- | --- |
| Distinct struct/union definitions | 32 |
| Total fields across all definitions | 512 |
| Member / tag name length | 31 characters |

---

## 3. Enumerations and typedef

```c
enum Color { RED, GREEN, BLUE };        /* 0, 1, 2 */
enum Flags { F_ONE = 1, F_TWO = 2 };   /* explicit values */

typedef struct Point Pt;                /* alias, incl. struct tags */
Pt a; a.x = 1;
```

`enum` constants become integer constants usable anywhere a constant
expression is. `typedef` creates a name alias and works for struct
tags, which is what makes the `Pt`-style shorthand possible.

---

## 4. Statements

| Statement | Notes |
| --- | --- |
| `if` / `else` | standard |
| `while` | standard |
| `do` … `while` | standard |
| `for` | may declare a variable in the initializer: `for (int i = 0; i < n; i++)` |
| `switch` / `case` / `default` | see the constraints below |
| `break` | leaves loop or switch |
| `continue` | next loop iteration — **rejected inside a `switch`** |
| `return` | with or without a value |
| `{ … }` | blocks; declarations may appear at the start of any block |

### `switch` constraints

`case` labels must be constant expressions. Two ordering rules matter:

- **`default` should be the last label.** A `case` that matches a
  value and appears *after* `default` in the source does not suppress
  the default body.
- `continue` inside a `switch` is rejected — because the generated
  jump has no enclosing loop to continue.

`fall-through` between cases works as in C.

---

## 5. Expressions and operators

**Assignment:** `=` `+=` `-=` `*=` `/=` `%=` `<<=` `>>=` `&=` `|=` `^=`

**Arithmetic:** `+` `-` `*` `/` `%` (integer)

**Bitwise:** `<<` `>>` `&` `|` `^` `~`

**Logical:** `&&` `||` `!`

**Relational:** `==` `!=` `<` `>` `<=` `>=`

**Unary:** `-` `+` `*` (dereference) `&` (address-of) `++` `--`
(pre- and post-fix, element-scaled on pointers)

**Ternary:** `cond ? a : b`

**Member access:** `.` and `->`

Precedence and short-circuit evaluation follow C. Pointer `++`/`--`
and `+=`/`-=` scale by the element size.

---

## 6. Functions

```c
int add(int a, int b) { return a + b; }     /* definition */

int sum(int* v, int n);                     /* forward prototype */

int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }  /* recursion */
```

- **Maximum 8 parameters.**
- **Default parameter values** are supported in prototypes (a call
  that omits the argument uses the declared default).
- Forward prototypes allow mutual recursion and any call order.
- Calling a function that was *declared but never defined* fails the
  build and reports the line number of the first call site.
- Recursion uses the per-task stack; there is no heap-allocated
  frame.

### What is not available

Function pointers are not supported — a function's address cannot be
taken and stored. Indirection through a function table must be
replaced by an explicit `switch` over an integer selector. `T**`
(pointer-to-pointer) in array contexts, `goto`, `static` locals, and
variadic functions are likewise rejected.

---

## 7. Globals and storage

```c
int counter;                        /* zero-initialized */
int table[8];                       /* zero-initialized */
int init = 42;                      /* constant initializer */
int list[3] = {1, 2, 3};            /* list initializer */
char* name = "equinox";             /* string initializer */
extern int shared;                  /* defined in another file */
```

Globals live in a **zero-initialized data area**; scalars and
one-dimensional arrays take constant, list and string initializers.
`extern` declares a global defined in another translation unit — this
is the mechanism behind multi-file builds.

Local variables are frame-allocated and must be declared before use
(the compiler is single-pass). There are no `static` locals.

---

## 8. Preprocessor

mtcc ships a **mini preprocessor** (not a full CPP):

| Directive | Behaviour |
| --- | --- |
| `#include <morph.h>` | splice the named built-in prelude into the unit |
| `#include <stdio.h>` / `<stdlib.h>` / `<string.h>` | aliases of `morph.h` |
| `#include <multitasking.h>` | the task API prelude |
| `#include <fileio.h>` | the file-descriptor API prelude |
| `#include "local.h"` | include a real file from RAMFS (nesting max 8) |
| `#define NAME value` | object-like macro (text substitution) |
| `#undef NAME` | remove a definition |
| `#ifdef` / `#ifndef` / `#else` / `#endif` | conditional compilation |

Function-like macros — `#define SQ(x) ((x)*(x))` — are **rejected**
with a clear error rather than silently mis-expanding.

The preludes are *textually spliced*, so they never ship as files on
the volume; the source of truth for the syscall ABI is the prelude
text itself (see [MTCC_API.md](MTCC_API.md)).

---

## 9. Compilation and output

```sh
mtcc program.c          # compile and run immediately (tcc -run style)
mtcc -c program.c       # compile to program.mrp
mtcc -make build.ruf    # multi-file build driven by a .ruf recipe
mtcc -c -format elf -o out prog.c   # static ELF32 instead of MRP
```

| Flag | Meaning |
| --- | --- |
| `-c` | compile only, do not run |
| `-format mrp\|elf` | output format (default `mrp`) |
| `-o <name>` | output base name |
| `-make <file.ruf>` | recipe-driven multi-file build |
| `-q` | quiet (suppress the success line) |
| `--lib` | library mode: check-compile without a `main` |
| `-multiple-files` | keep per-file objects |
| `--debug` / `-d` | verbose compiler information |

The default output is an **MRP1** flat ring-3 image with an 18-byte
header, loaded into a per-task arena with demand paging. With
`-format elf` the output is a **static ELF32** image instead. `.ruf`
recipes, the `set` config hook and the multi-file link model are
documented in [SELF_HOSTING.md](SELF_HOSTING.md).

The default output format, flags and the tool used for spawning can
be changed live through [`.config/mtcc.ecf`](ECF.md#configmtccecf-default-mtcc)
— the compiler reads its defaults on every invocation, so an edit
takes effect on the next build without a reboot or a recompile.

---

## 10. Compiler limits

| Resource | Cap |
| --- | --- |
| Parameters per function | 8 |
| Local variables per function | 256 |
| Distinct struct/union definitions | 32 |
| Total struct/union fields | 512 |
| `typedef` aliases | 64 |
| Named constants (`enum`, `#define`) | 128 |
| String literals | 512 |
| Identifier length | 31 characters |
| `#include "..."` nesting | 8 |

These are compile-time constants in `mtcc.c`; exceeding one produces
an explicit error rather than silent truncation.

---

## 11. Summary of what is *not* supported

Stated plainly so there are no false expectations:

- **`struct` / `union` passed or returned by value** — use a pointer.
- Floating point of any kind (`float`, `double`).
- The `unsigned`, `long`, `short` keywords — `int` is signed.
- `sizeof` (there is no `sizeof` operator).
- Casts such as `(int)x`.
- 2-D arrays.
- `goto`.
- Variadic functions and function-like macros.
- `static` locals.
- Function pointers.
- `T**` (pointer-to-pointer) in array contexts.
- `continue` inside a `switch`.
- `case` labels placed after `default`.

Everything else in the tables above is supported.

---

## 12. Canonical examples

| Program | Demonstrates |
| --- | --- |
| `src/test/hello.c` | the minimum: `morph.h` + `print` |
| `src/test/struct.c` | `struct`/`union`, pointers, arrays of struct, nesting, copy |
| `src/test/sinit.c` | `struct`/`union` `{…}` initializers — global, local, nested |
| `src/test/swenum.c` | `enum` constants and `switch`/`case`/`default` |
| `src/test/mf_main.c` + `mf_helper.c` + `mf_shared.h` | multi-file compile/link, `extern`, relative `#include "x.h"` |
| `src/test/morphio.c` | the full v1 file API |
| `src/test/multitask.c` | the task API |

---

## See also

- [MTCC_API.md](MTCC_API.md) — the preludes and syscall surface
- [SELF_HOSTING.md](SELF_HOSTING.md) — multi-file builds, `.ruf` recipes, libc
- [MRP.md](MRP.md) — the `.mrp` image format
- [ECF.md](ECF.md) — live compiler defaults via `.config/mtcc.ecf`
