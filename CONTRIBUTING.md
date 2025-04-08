# Contributing to `shecc`

:+1::tada: First off, thanks for taking the time to contribute! :tada::+1:

The following is a set of guidelines for contributing to [shecc](https://github.com/sysprog21/shecc)
hosted on GitHub. These are mostly guidelines, not rules.
Use your best judgment, and feel free to propose changes to this document in a pull request.

## Issues

This project uses GitHub Issues to track ongoing development, discuss project plans, and keep track of bugs.
Be sure to search for existing issues before you create another one.

Initially, it is advisable to create an issue on GitHub for bug reports, feature requests,
or substantial pull requests, as this offers a platform for discussion with both the community and project maintainers.

Engaging in a conversation through a GitHub issue before making a contribution is crucial to ensure the acceptance of your work.
We aim to prevent situations where significant effort is expended on a pull request that might not align with the project's design principles.
For example, it might turn out that the feature you propose is more suited as an independent module that complements this project,
in which case we would recommend that direction.

For minor corrections, such as typo fixes, small refactoring, or updates to documentation/comments,
filing an issue is not typically necessary.
What constitutes a "minor" fix involves discretion; however, examples include:
- Correcting spelling mistakes
- Minor code refactoring
- Updating or editing documentation and comments

Nevertheless, there may be instances where, upon reviewing your pull requests,
we might request an issue to be filed to facilitate discussion on broader design considerations.

Visit our [Issues page on GitHub](https://github.com/sysprog21/shecc/issues) to search and submit.

## Coding Convention

Contributions from developers across corporations, academia, and individuals are welcome.
However, participation requires adherence to fundamental ground rules:
* Code must strictly adhere to the established C coding style (refer to the guidelines below).
  While there is some flexibility in basic style, it is crucial to stick to the current coding standards.
  Complex algorithmic constructs without proper comments will not be accepted.
* External pull requests should include thorough documentation in the pull request comments for consideration.
* When composing documentation, code comments, and other materials in English,
  please adhere to the American English (`en_US`) dialect.
  This variant should be considered the standard for all documentation efforts.
  For instance, opt for "initialize" over "initialise" and "color" rather than "colour".

Software requirement: [clang-format](https://clang.llvm.org/docs/ClangFormat.html) version 18 or later.

This repository consistently contains an up-to-date `.clang-format` file with rules that match the explained ones.
For maintaining a uniform coding style, execute the command `clang-format -i *.{c,h}`.

## Coding Style for Modern C

This coding style is a variant of the [K&R style](https://en.wikipedia.org/wiki/Indentation_style#K&R).
Adhere to established practices while being open to innovation.
Maintain consistency, adopt the latest C standards,
and embrace modern compilers along with their advanced static analysis capabilities and sanitizers.

### Indentation

In this coding style guide, the use of 4 spaces for indentation instead of tabs is strongly enforced to ensure consistency.
Always apply a single space before and after comparison and assignment operators to maintain readable code.
Additionally, it is crucial to include a single space after every comma.
e.g.,
```c
for (int i = 0; i < 10; i++) {
    printf("%d\n", i);
    /* some operations */
}
```

The tab character (ASCII 0x9) should never appear within any source code file.
When indentation is needed in the source code, align using spaces instead.
The width of the tab character varies by text editor and programmer preference,
making consistent visual layout a continual challenge during code reviews and maintenance.

### Line length

All lines should typically remain within 80 characters, with longer lines wrapped as needed.
This practice is supported by several valid rationales:
* It encourages developers to write concise code.
* Smaller portions of information are easier for humans to process.
* It assists users of vi/vim (and potentially other editors) who use vertical splits.
* It is especially helpful for those who may want to print code on paper.

### Comments

Multi-line comments should have the opening and closing characters on separate lines,
with the content lines prefixed by a space and an asterisk (`*`) for alignment, e.g.,
```c
/*
 * This is a multi-line comment.
 */

/* One line comment. */
```

Use multi-line comments for more elaborate descriptions or before significant logical blocks of code.

Single-line comments should be written in C89 style:
```c
    return (uintptr_t) val;  /* return a bitfield */
```

Leave two spaces between the statement and the inline comment.
Avoid commenting out code directly.
Instead, use `#if 0` ... `#endif` when it is intentional.

All assumptions should be clearly explained in comments.
Use the following markers to highlight issues and make them searchable:
* `WARNING`: Alerts a maintainer to the risk of changing this code.
  e.g., a delay loop counter's terminal value was determined empirically and may need adjustment when the code is ported or the optimization level is tweaked.
* `NOTE`: Provides descriptive comments about the "why" of a chunk of code,
  as opposed to the "how" usually included in comments.
  e.g., a chunk of driver code may deviate from the datasheet due to a known erratum in the chip,
  or an assumption made by the original programmer is explained.
* `TODO`: Indicates an area of the code that is still under construction and explains what remains to be done.
  When appropriate, include an all-caps programmer name or set of initials before the word `TODO`.

Keep the documentation as close to the code as possible.

### Spacing and brackets

Ensure that the keywords `if`, `while`, `for`, `switch`, and `return` are always followed by a single space when there is additional code on the same line.
Follow these spacing guidelines:
* Place one space after the keyword in a conditional or loop.
* Do not use spaces around the parentheses in conditionals or loops.
* Insert one space before the opening curly bracket.

For example:
```c
do {
    /* some operations */
} while (condition);
```

Functions (their declarations or calls), `sizeof` operator or similar
macros shall not have a space after their name/keyword or around the
brackets, e.g.,
```c
unsigned total_len = offsetof(obj_t, items[n]);
unsigned obj_len = sizeof(obj_t);
```

Use brackets to avoid ambiguity and with operators such as `sizeof`,
but otherwise avoid redundant or excessive brackets.

Assignment operators (`=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `~=`, and `!=`) should always have a space before and after them.
For example:
```c
count += 1;
```

Binary operators (`+`, `-`, `*`, `/`, `%`, `<`, `<=`, `>`, `>=`, `==`, `!=`, `<<`, `>>`, `&`, `|`, `^`, `&&`, and `||`) should also be surrounded by spaces.
For example:
```c
current_conf = prev_conf | (1 << START_BIT);
```

Unary operators (`++`, `--`, `!`, and `~`) should be written without spaces between the operator and the operand.
For example:
```c
bonus++;
if (!play)
    return STATE_QUITE;
```

The ternary operator (`?` and `:`) should have spaces on both sides.
For example:
```c
uint32_t max(uint32_t a, uint32_t b)
{
    return (a > b) ? a : b;
}
```

Structure pointer (`->`) and member (`.`) operators should not have surrounding spaces.
Similarly, array subscript operators (`[` and `]`) and function call parentheses should be written without spaces around them.

### Parentheses

Avoid relying on C’s operator precedence rules, as they may not be immediately clear to those maintaining the code.
To ensure clarity, always use parentheses to enforce the correct execution order within a sequence of operations,
or break long statements into multiple lines if necessary.

When using logical AND (`&&`) and logical OR (`||`) operators, each operand should be enclosed in parentheses,
unless it is a single identifier or constant.
For example:
```c
if ((count > 100) && (detected == false)) {
    character = C_ASSASSIN;
}
```

### Variable names and declarations

Ensure that functions, variables, and comments are consistently named using English words.
Global variables should have descriptive names, while local variables can have shorter names.
It is important to strike a balance between being descriptive and concise.
Each variable's name should clearly reflect its purpose.

Use [snake_case](https://en.wikipedia.org/wiki/Snake_case) for naming conventions,
and avoid using "camelCase."
Additionally, do not use Hungarian notation or any other unnecessary prefixes or suffixes.

When declaring pointers, follow these spacing conventions:
```c
const char *name;  /* Const pointer; '*' with the name and a space before it */
conf_t * const cfg;  /* Pointer to const data; spaces around 'const' */
const uint8_t * const charmap;  /* Const pointer and const data */
const void * restrict key;  /* Const pointer that does not alias */
```

Local variables of the same type should be declared on the same line.
For example:
```c
void func(void)
{
    char a, b;  /* OK */

    char a;
    char b;     /* Incorrect: a variable with char type already exists. */
}
```

Always include a trailing comma in the last element of a structure initialization,
including its nested elements, to help `clang-format` correctly format the structure.
However, this comma can be omitted in very simple and short structures.
```c
typedef struct {
    int width, height;
} screen_t;

screen_t s = {
    .width = 640,
    .height = 480,   /* comma here */
}
```

### Type definitions

Declarations shall be on the same line, e.g.,
```c
typedef void (*dir_iter_t)(void *, const char *, struct dirent *);
```

_Typedef_ structures rather than pointers.  Note that structures can be kept
opaque if they are not dereferenced outside the translation unit where they
are defined.  Pointers can be _typedefed_ only if there is a very compelling
reason.

New types may be suffixed with `_t`.  Structure name, when used within the
translation unit, may be omitted, e.g.:

```c
typedef struct {
    unsigned if_index;
    unsigned addr_len;
    addr_t next_hop;
} route_info_t;
```

### Initialization

Do not initialize static and global variables to `0`; the compiler will do this.
When a variable is declared inside a function, it is not automatically initialized.

```c
static uint8_t a;  /* Global variable 'a' is set to 0 by the compiler */

void foo()
{
    /* 'b' is uninitialized and set to whatever happens to be in memory */
    uint8_t b;
    ...
}
```

Embrace C99 structure initialization where reasonable, e.g.,
```c
static const crypto_ops_t openssl_ops = {
    .create = openssl_crypto_create,
    .destroy = openssl_crypto_destroy,
    .encrypt = openssl_crypto_encrypt,
    .decrypt = openssl_crypto_decrypt,
    .hmac = openssl_crypto_hmac,
};
```

Embrace C99 array initialization, especially for the state machines, e.g.,
```c
static const uint8_t tcp_fsm[TCP_NSTATES][2][TCPFC_COUNT] = {
    [TCPS_CLOSED] = {
        [FLOW_FORW] = {
            /* Handshake (1): initial SYN. */
            [TCPFC_SYN] = TCPS_SYN_SENT,
        },
    },
    ...
}
```

Any pointer variable that does not have an initial address should be explicitly initialized to `NULL`.
This practice helps prevent undefined behavior caused by dereferencing uninitialized pointers.

In accordance with modern C standards (such as C99 and later), it is preferable to define local variables as needed,
rather than declaring them all at the beginning of a function.
Declaring variables close to their first use enhances code readability and helps maintain a clear, logical flow within the function.

Additionally, static analysis tools can be employed to scan the entire source code before each build,
providing warnings about variables that are used before being properly initialized.
This helps catch potential bugs early and ensures code quality and safety.

### Control structures

Try to make the control flow easy to follow.  Avoid long convoluted logic
expressions; try to split them where possible (into inline functions,
separate if-statements, etc).

The control structure keyword and the expression in the brackets should be
separated by a single space.  The opening curly bracket shall be in the
same line, also separated by a single space.  Example:

```c
    for (;;) {
        obj = get_first();
        while ((obj = get_next(obj))) {
            ...
        }
        if (done)
            break;
    }
```

Do not add inner spaces around the brackets. There should be one space after
the semicolon when `for` has expressions:
```c
    for (unsigned i = 0; i < __arraycount(items); i++) {
        ...
    }
```

#### Avoid unnecessary nesting levels

It is generally preferred to place the shorter clause (measured in lines of code) first in `if` and `else if` statements.
Long clauses can distract the reader from the core decision-making logic, making the code harder to follow.
By placing the shorter clause first, the decision path becomes clearer and easier to understand, which can help reduce bugs.

Avoid nesting `if`-`else` statements deeper than two levels.
Instead, consider using function calls or `switch` statements to simplify the logic and enhance readability.
Deeply nested `if`-`else` statements often indicate a complex and fragile state machine implementation,
which can be refactored into a safer and more maintainable structure.

For example, avoid this:
```c
int inspect(obj_t *obj)
{
    if (cond) {
        ...
        /* long code block */
        ...
        return 0;
    }
    return -1;
}
```

Instead, consider this approach:
```c
int inspect(obj_t *obj)
{
    if (!cond)
        return -1;
    ...
    return 0;
}
```

However, be careful not to make the logic more convoluted in an attempt to simplify nesting.

### `if` statements

Curly brackets and spacing follow the K&R style:
```c
    if (a == b) {
        ..
    } else if (a < b) {
        ...
    } else {
        ...
    }
```

Simple and succinct one-line if-statements may omit curly brackets:
```c
    if (!valid)
        return -1;
```

However, do prefer curly brackets with multi-line or more complex statements.
If one branch uses curly brackets, then all other branches shall use the
curly brackets too.

Wrap long conditions to the if-statement indentation adding extra 4 spaces:
```c
    if (some_long_expression &&
        another_expression) {
        ...
    }
```

#### Avoid redundant `else`

Avoid:
```c
    if (flag & F_FEATURE_X) {
        ...
        return 0;
    } else {
        return -1;
    }
```

Consider:
```c
    if (flag & F_FEATURE_X) {
        ...
        return 0;
    }
    return -1;
```

### `switch` statements

Switch statements should have the `case` blocks at the same indentation
level, e.g.:
```c
    switch (expr) {
    case A:
        ...
        break;
    case B:
        /* fallthrough */
    case C:
        ...
        break;
    }
```

If the case block does not break, then it is strongly recommended to add a
comment containing "fallthrough" to indicate it.  Modern compilers can also
be configured to require such comment (see gcc `-Wimplicit-fallthrough`).

### Function definitions

The opening and closing curly brackets shall also be in the separate lines (K&R style).

```c
ssize_t hex_write(FILE *stream, const void *buf, size_t len)
{
    ...
}
```

Do not use old style K&R style C definitions.

Introduced in C99, `restrict` is a pointer qualifier that informs the compiler no other pointer will access the same object during its lifetime,
enabling optimizations such as vectorization. Violating this assumption leads to undefined behavior.
Use `restrict` judiciously.

For function parameters, place one space after each comma, except at the end of a line.

### Function-like Macros

When using function-like macros (parameterized macros), adhere to the following guidelines:
- Enclose the entire macro body in parentheses.
- Surround each parameter usage with parentheses.
- Limit the use of each parameter to no more than once within the macro to avoid unintended side effects.
- Never include control flow statements (e.g., `return`) within a macro.
- If the macro involves multiple statements, encapsulate them within a `do`-`while (0)` construct.

For example:
```c
#define SET_POINT(p, x, y)      \
    do {                        \
        (p)->px = (x);          \
        (p)->py = (y);          \
    } while (0)
```

While the extensive use of parentheses, as shown above, helps minimize some risks,
it cannot prevent issues like unintended double increments from calls such as `MAX(i++, j++)`.

Other risks associated with macros include comparing signed and unsigned data or testing floating-point values.
Additionally, macros are not visible at runtime, making them impossible to step into with a debugger.
Therefore, use them with caution.

In general, macro names are typically written in all capitals, except in cases where readability is improved by using lowercase.
For example:
```
#define countof(a)   (size)(sizeof(a) / sizeof(*(a)))
#define lengthof(s)  (countof(s) - 1)
```

Although all capitals are generally preferred for constants,
lowercase can be used for function-like macros to improve readability.
These function-like macros do not share the same namespace concerns as other macros.

For example, consider the implementation of a simple memory allocator.
An arena can be represented by a memory buffer and an offset that begins at zero.
To allocate an object, record the pointer at the current offset,
advance the offset by the size of the object, and return the pointer.
Additional considerations, such as alignment and checking for available space, are also required.
```c
#define new(a, n, t)  alloc(a, n, sizeof(t), _Alignof(t))

typedef struct {
    char *begin, *end;
} arena_t;

void *alloc(arena_t *a, ptrdiff_t count, ptrdiff_t size, ptrdiff_t align)
{
    ptrdiff_t pad = -(uintptr_t)a->begin & (align - 1);
    assert(count < (a->end - a->begin - pad) / size);

    void *result = a->begin + pad;
    a->begin += pad + (count * size);
    return memset(result, 0, count * size);
}
```

Using the `new` macro helps prevent several common errors in C programs.
If types are mixed up, the compiler generates errors or warnings.
Moreover, naming a macro `new()` does not conflict with variables or fields named `new`,
because the macro form does not resemble a function call.

### Use `const` and `static` effectively

The `static` keyword should be used for any variables that do not need to be accessible outside the module where they are declared.
This is particularly important for global variables defined in C files.
Declaring variables and functions as `static` at the module level protects them from external access, reducing coupling between modules and improving encapsulation.

For functions that do not need to be accessible outside the module, use the `static` keyword.
This is especially important for private functions, where `static` should always be applied.

For example:
```c
static bool verify_range(uint16_t x, uint16_t y);
```

The `const` keyword is essential for several key purposes:
- Declaring variables that should not change after initialization.
- Defining fields within a `struct` that must remain immutable, such as those in memory-mapped I/O peripheral registers.
- Serving as a strongly typed alternative to `#define` for numerical constants.

For example, instead of using:
```c
#define MAX_SKILL_LEVEL (100U)
```

Use:
```c
const uint8_t max_skill_level = 100;
```

Maximizing the use of `const` provides the advantage of compiler-enforced protection against unintended modifications to data that should be read-only,
thereby enhancing code reliability and safety.

Additionally, when one of your function arguments is a pointer to data that will not be modified within the function,
you should use the `const` keyword.
This is particularly useful when comparing a character array with predefined strings without altering the array’s contents.

For example:
```c
static bool is_valid_cmd(const char *cmd);
```

### Object abstraction

Objects are often "simulated" by the C programmers with a `struct` and
its "public API".  To enforce the information hiding principle, it is a
good idea to define the structure in the source file (translation unit)
and provide only the _declaration_ in the header.  For example, `obj.c`:

```c
#include "obj.h"

struct obj {
    int value;
}

obj_t *obj_create(void)
{
    return calloc(1, sizeof(obj_t));
}

void obj_destroy(obj_t *obj)
{
    free(obj);
}
```

With an example `obj.h`:
```c
#ifndef _OBJ_H_
#define _OBJ_H_

typedef struct obj;

obj_t *obj_create(void);
void obj_destroy(obj_t *);

#endif
```

Such structuring will prevent direct access of the `obj_t` members outside
the `obj.c` source file.  The implementation (of such "class" or "module")
may be large and abstracted within separate source files.  In such case,
consider separating structures and "methods" into separate headers (think of
different visibility), for example `obj_impl.h` (private) and `obj.h` (public).

Consider `crypto_impl.h`:
```c
#ifndef _CRYPTO_IMPL_H_
#define _CRYPTO_IMPL_H_

#if !defined(__CRYPTO_PRIVATE)
#error "only to be used by the crypto modules"
#endif

#include "crypto.h"

typedef struct crypto {
    crypto_cipher_t cipher;
    void *key;
    size_t key_len;
    ...
}
...

#endif
```

And `crypto.h` (public API):

```c
#ifndef _CRYPTO_H_
#define _CRYPTO_H_

typedef struct crypto crypto_t;

crypto_t *crypto_create(crypto_cipher_t);
void crypto_destroy(crypto_t *);
...

#endif
```

### Use reasonable types

Use `unsigned` for general iterators; use `size_t` for general sizes; use
`ssize_t` to return a size which may include an error.  Of course, consider
possible overflows.

Avoid using fixed-width types like `uint8_t`, `uint16_t`, or other smaller integer types for general iterators or similar cases unless there is a specific need for size-constrained operations,
such as in fixed-width data processing or resource-limited environments.

C has rather peculiar _type promotion rules_ and unnecessary use of sub-word
types might contribute to a bug once in a while.

Boolean variables should be declared using the `bool` type.
Non-Boolean values should be converted to Boolean by using relational operators (e.g., `<` or `!=`) rather than by casting.

For example:
```c
#include <stdbool.h>
...
bool inside = (value < expected_range);
```

### Embrace portability

#### Byte-order

Do not assume x86 or little-endian architecture.  Use endian conversion
functions for operating the on-disk and on-the-wire structures or other
cases where it is appropriate.

#### Types

Do not assume a particular 32-bit or 64-bit architecture; for example, do not assume the size of `long` or `unsigned long`.
Instead, use `int64_t` or `uint64_t` for 8-byte integers.

Fixed-width types, such as `uint32_t`, are particularly useful when memory size is critical,
as in embedded systems, communication protocols requiring specific data sizes,
or when interacting with hardware registers that require precise bit-width operations.
In these scenarios, fixed-width types ensure consistent behavior across different platforms and compilers.

Do not assume `char` is signed; for example, on Arm architectures, it is unsigned by default.

Avoid defining bit-fields within signed integer types.
Additionally, do not use bitwise operators (such as `&`, `|`, `~`, `^`, `<<`, and `>>`) on signed integer data.
Refrain from combining signed and unsigned integers in comparisons or expressions, as this can lead to unpredictable results.

When using `#define` to declare decimal constants, append a `U` to ensure they are treated as unsigned.
For example:
```c
#define SOME_CONSTANT (6U)

uint16_t unsigned_a = 6;
int16_t  signed_b = -9;
if (unsigned_a + signed_b < 4) {
    /* This block might appear logically correct, as -9 + 6 is -3 */
    ...
}
/* but compilers with 16-bit int may legally interpret it as (0xFFFF – 9) + 6. */
```

It is important to note that certain aspects of manipulating binary data within signed integer containers are implementation-defined behaviors according to ISO C standards.
Additionally, mixing signed and unsigned integers can lead to data-dependent results, as demonstrated in the example above.

Use C99 macros for constant prefixes or formatting of the fixed-width types.

Use:
```c
#define SOME_CONSTANT (UINT64_C(1) << 48)
printf("val %" PRIu64 "\n", SOME_CONSTANT);
```

Do not use:
```c
#define SOME_CONSTANT (1ULL << 48)
printf("val %lld\n", SOME_CONSTANT);
```

#### Avoid unaligned access

Avoid assuming that unaligned access is safe.
It is not secure on architectures like Arm, POWER, and others.
Additionally, even on x86, unaligned access can be slower.

#### Structures and Unions

Care should be taken to prevent the compiler from inserting padding bytes within `struct` or `union` types,
as this can affect memory layout and portability.
To control padding and alignment, consider using structure packing techniques specific to your compiler.

Additionally, take precautions to ensure that the compiler does not alter the intended order of bits within bit-fields.
This is particularly important when working with hardware registers or communication protocols where bit order is crucial.

According to the C standard, the layout of structures, including padding and bit-field ordering,
is implementation-defined, meaning it can vary between different compilers and platforms.
Therefore, it is essential to verify that the structure's layout meets your expectations, especially when writing portable code.

For example:
```c
typedef struct {
    uint16_t count;         /* offset 0 */
    uint16_t max_count;     /* offset 2 */
    uint16_t unused0;       /* offset 4 */
    uint16_t enable    : 2; /* offset 6 bits 15-14 */
    uint16_t interrupt : 1; /* offset 6 bit 13     */
    uint16_t unused1   : 7; /* offset 6 bits 12-6  */
    uint16_t complete  : 1; /* offset 6 bit 5      */
    uint16_t unused2   : 4; /* offset 6 bits 4-1   */
    uint16_t periodic  : 1; /* offset 6 bit 0      */
} mytimer_t;

_Static_assert(sizeof(mytimer_t) == 8,
               "mytimer_t struct size incorrect (expected 8 bytes)");
```

To enhance portability, use standard-defined types (e.g., `uint16_t`, `uint32_t`) and avoid relying on compiler-specific behavior.
Where precise control over memory layout is required, such as in embedded systems or when interfacing with hardware,
always verify the structure size and layout using static assertions.

#### Avoid extreme portability

Unless programming for micro-controllers or exotic CPU architectures,
focus on the common denominator of the modern CPU architectures, avoiding
the very maximum portability which can make the code unnecessarily cumbersome.

Some examples:
- It is fair to assume `sizeof(int) == 4` since it is the case on all modern
mainstream architectures.  PDP-11 era is long gone.
- Using `1U` instead of `UINT32_C(1)` or `(uint32_t) 1` is also fine.
- It is fair to assume that `NULL` is matching `(uintptr_t) 0` and it is fair
to `memset()` structures with zero.  Non-zero `NULL` is for retro computing.

## Git Commit Style

Effective version control is critical to modern software development.
Git's powerful features—such as granular commits, branching,
and a versatile staging area—offer unparalleled flexibility.
However, this flexibility can sometimes lead to disorganized commit histories and
merge conflicts if not managed with clear, consistent practices.

By committing often, writing clear messages, and adhering to a common workflow,
developers can not only reduce the potential for errors but also simplify collaboration and future maintenance.
We encourage every team to tailor these best practices to their specific needs while striving
for a shared standard that promotes efficiency and code quality.

Below are the detailed guidelines that build on these principles.
* Group Related Changes Together:
  Each commit should encapsulate a single, coherent change.
  e.g., if you are addressing two separate bugs, create two distinct commits.
  This approach produces focused, small commits that simplify understanding, enable quick rollbacks,
  and foster efficient peer reviews.
  By taking advantage of Git’s staging area and selective file staging,
  you can craft granular commits that make collaboration smoother and more transparent.
* Commit Frequently:
  Making commits often ensures that your changes remain concise and logically grouped.
  Frequent commits not only help maintain a clean history but also allow you to share your progress with your teammates regularly.
  This regular sharing keeps everyone in sync,
  minimizes merge conflicts, and promotes a collaborative environment where integration happens seamlessly.
* Avoid Committing Work in Progress:
  Only commit code when a logical component is in a stable, ready-to-integrate state.
  Break your feature's development into manageable segments that reach a functional milestone quickly,
  so you can commit regularly without compromising quality.
  If you feel the urge to commit merely to clear your working directory for actions like switching branches or pulling changes,
  use Git's stash feature instead.
  This practice helps maintain a stable repository and ensures that your team reviews well-tested, coherent code.
* Test Your Code Before Committing:
  Before committing, ensure that your code has been thoroughly tested.
  Rather than assuming your changes are ready, run comprehensive tests to confirm they work as intended without unintended side effects.
  Testing is especially critical when sharing your code with others,
  as it maintains the overall stability of the project and builds trust among collaborators.
* Utilize Branches for Parallel Development:
  Branches are a powerful tool that enables developers to isolate different lines of work—whether you are developing new features,
  fixing bugs, or exploring innovative ideas.
  By using branches extensively, you can work on your tasks independently and merge only after careful review and testing.
  This not only keeps the main branch stable but also encourages collaborative code reviews and a more organized integration process.

Clear and descriptive commit messages are crucial for maintaining a transparent history of changes and for facilitating effective debugging and tracking.
Please adhere to the guidelines outlined in [How to Write a Git Commit Message](https://cbea.ms/git-commit/).
1. Separate the subject from the body with a blank line.
2. Limit the subject line to 50 characters.
3. Capitalize the subject line.
4. Do not end the subject line with a period.
5. Use the imperative mood in the subject line.
6. Wrap the body at 72 characters.
7. Use the body to explain what and why, not how.

An example (derived from Chris' blog post) looks like the following:
```text
Summarize changes in around 50 characters or less

More detailed explanatory text, if necessary. Wrap it to about 72
characters or so. In some contexts, the first line is treated as the
subject of the commit and the rest of the text as the body. The
blank line separating the summary from the body is critical (unless
you omit the body entirely); various tools like `log`, `shortlog`
and `rebase` can get confused if you run the two together.

Explain the problem that this commit is solving. Focus on why you
are making this change as opposed to how (the code explains that).
Are there side effects or other unintuitive consequences of this
change? Here's the place to explain them.

Further paragraphs come after blank lines.

- Bullet points are okay, too

- Typically a hyphen or asterisk is used for the bullet, preceded
  by a single space, with blank lines in between, but conventions
  vary here

If you use an issue tracker, put references to them at the bottom,
like this:
Close #123
```

Another illustration of effective practice.
```text
commit f1775422bb5a1aa6e79a685dfa7cb54a852b567b
Author: Jim Huang <jserv@ccns.ncku.edu.tw>
Date:   Mon Feb 24 13:08:32 2025 +0800

    Introduce CPU architecture filtering in scheduler
    
    In environments with mixed CPU architectures, it is crucial to ensure
    that an instance runs only on a host with a compatible CPU
    type—preventing, for example, a RISC-V instance from being scheduled on
    an Arm host.
    
    This new scheduler filter enforces that requirement by comparing an
    instance's architecture against the host's allowed architectures. For
    the libvirt driver, the host's guest capabilities are queried, and the
    permitted architectures are recorded in the permitted_instances_types
    list within the host's cpu_info dictionary.
    
    The filter systematically excludes hosts that do not support the
    instance's CPU architecture. Additionally, RISC-V has been added to the
    set of acceptable architectures for scheduling.
    
    Note that the CPU architecture filter is disabled by default.
```

The above is a clear, unformatted description provided in plain text.

In addition, this project expects contributors to follow these additional rules:
* If there is important, useful, or essential conversation or information,
  include a reference or copy it.
* Do not write single-word commits. Provide a descriptive subject.
* Avoid using abusive words.
* Avoid using backticks in commit subjects.
  Backticks can be easily confused with single quotes on some terminals,
  reducing readability. Plain text or single quotes provide sufficient clarity and emphasis.
* Avoid using parentheses in commit subjects.
  Excessive use of parentheses "()" can clutter the subject line,
  making it harder to quickly grasp the essential message.

Some conventions are automatically enforced by the [githooks](https://git-scm.com/docs/githooks).

## References
- [Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html)
- 1999, Brian W. Kernighan and Rob Pike, The Practice of Programming, Addison–Wesley.
- 1993, Bill Shannon, [C Style and Coding Standards for SunOS](https://devnull-cz.github.io/unix-linux-prog-in-c/cstyle.ms.pdf)
