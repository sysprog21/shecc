# C99 Compliance Status

shecc implements a subset of C99 suitable for self-hosting and systems programming,
prioritizing simplicity, educational value, and minimal dependencies over full standard compliance.
This document tracks compliance gaps and non-standard behaviors.

## Implemented Features

### Core Language
- Basic types: `int`, `short`, `char`, `void`, `_Bool`
- Structures and unions with nested definitions
- Enumerations with automatic value assignment
- Function definitions and declarations
- Arrays (single and multi-dimensional)
- Pointers and pointer arithmetic (fully C99-compliant)
- Type definitions (`typedef`)

### Control Flow
- `if`/`else` statements
- `goto` and label statements
- `while`, `do-while`, `for` loops
- `switch`/`case`/`default` statements
- `break`, `continue`, `return` statements

### Operators
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`
- Logical: `&&`, `||`, `!`
- Relational: `<`, `>`, `<=`, `>=`, `==`, `!=`
- Assignment: `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`, `|=`, `^=`
- Increment/decrement: `++`, `--` (prefix and postfix)
- Conditional: `? :`
- Member access: `.`, `->`
- Address/dereference: `&`, `*`

### Preprocessor (Partial)
- `#define` for object-like and function-like macros
- `#ifdef`, `#ifndef`, `#if`, `#elif`, `#else`, `#endif`
- `#undef` for macro removal
- `defined()` operator
- `__VA_ARGS__` for variadic macros

## Missing Features

### Storage Classes & Qualifiers

| Feature | Status | Impact |
|---------|--------|--------|
| `static` | Not implemented | No internal linkage or persistent local variables |
| `extern` | Not implemented | No external linkage declarations |
| `register` | Not implemented | No register hint optimization |
| `auto` | Not implemented | Default storage class (implicit) |
| `const` | Parsed but ignored | No read-only enforcement |
| `volatile` | Not implemented | No volatile semantics |
| `restrict` | Not implemented | No pointer aliasing optimization |
| `inline` | Not implemented | No function inlining |

### Type System

| Feature | Status | Notes |
|---------|--------|-------|
| `long` | Missing | Only 4-byte integers |
| `long long` | Missing | No 64-bit integers |
| `unsigned` | Missing | All integers are signed |
| `signed` | Missing | Implicit for integers |
| `float` | Missing | No floating-point support |
| `double` | Missing | No floating-point support |
| `long double` | Missing | No floating-point support |
| Bit-fields | Missing | Cannot pack struct members |

### Literals & Constants

| Feature | Status | Current Behavior |
|---------|--------|-----------------|
| Integer suffixes (`u`, `l`, `ll`) | Not parsed | All literals are `int` |
| Wide characters (`L'c'`) | Not supported | Single-byte only |
| Wide strings (`L"..."`) | Not supported | Single-byte only |
| Multi-character constants | Not supported | Single character only |
| Universal characters (`\u`, `\U`) | Not supported | ASCII only |
| Hex escapes (`\x...`) | Limited | Max 2 hex digits |

### Preprocessor Gaps

| Feature | Status | Description |
|---------|--------|-------------|
| `#include` | Parsed only | No file inclusion |
| Token pasting (`##`) | Missing | Cannot concatenate tokens |
| Stringizing (`#`) | Missing | Cannot convert to string |
| `__FILE__` | Missing | No file name macro |
| `__LINE__` | Missing | No line number macro |
| `__DATE__` | Missing | No compile date |
| `__TIME__` | Missing | No compile time |
| `__STDC__` | Missing | No standard compliance indicator |
| `#pragma` | Ignored | Accepted but no effect |

### Advanced Features

| Feature | Status | Description |
|---------|--------|-------------|
| Designated initializers | Missing | No `.field = value` syntax |
| Compound literals | Partial | Limited support |
| Flexible array members | Missing | No `[]` at struct end |
| Variable-length arrays | Missing | No runtime-sized arrays |
| `_Complex` | Missing | No complex numbers |
| `_Imaginary` | Missing | No imaginary numbers |
| `_Static_assert` | Missing | No compile-time assertions |
| `_Alignof` | Missing | No alignment queries |
| `_Alignas` | Missing | No alignment specification |
| `_Generic` | Missing | No generic selection |

## Non-Standard Behaviors

### GNU Extensions
- Binary literals: `0b101010`
- Escape sequence: `\e` for ESC character
- `void*` arithmetic (treated as `char*`)
- `sizeof(void)` returns 0 (should be error)
- Computed goto

### Implementation-Specific
- Array compound literals in scalar context use first element
- String literals are modifiable (stored in `.data`, not `.rodata`)
- No strict aliasing rules
- Left-to-right evaluation order (not always guaranteed in C99)
