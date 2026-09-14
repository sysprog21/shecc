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
- `switch`/`case`/`default` statements, including labels after ordinary
  statements and within nested compound blocks; strict C99 rejects duplicate
  case values and declarations immediately following an ordinary, case, or
  default label
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
- `#pragma once`, other `#pragma` options will be ignored
- `defined()` operator
- `__VA_ARGS__` for variadic macros
- `__FILE__`, `__LINE__` built-in macros

## Missing Features

### Storage Classes & Qualifiers

| Feature | Status | Impact |
|---------|--------|--------|
| `static` | Partial | File-scope internal linkage and persistent block-scope objects work, including C99 `for` initializers; cross-translation-unit linkage remains incomplete. |
| `extern` | Partial | File- and block-scope object declarations plus function prototypes bind to global declarations; remaining C99 forms need coverage. |
| `register` | Partial | Block-scope declarations and parameters lower as automatic objects and reject address-taking; no allocation hint is implemented. |
| `auto` | Supported | Block-scope declarations and C99 `for` initializers use ordinary automatic storage. |
| `const` | Supported | Enforced for direct and indirect lvalues; pointer-level conversions are checked. |
| `volatile` | Partial | Preserved through declarations and prevents key optimizations; exhaustive optimizer audit remains. |
| `restrict` | Partial | Accepted wherever C99 allows it and then ignored: the qualifier is not retained, so it drives no aliasing optimization. |
| `inline` | Partial | File-scope declarations/definitions are accepted; C99 linkage constraints remain incomplete. |

### Type System

| Feature | Status | Notes |
|---------|--------|-------|
| `long` | Partial | Distinct rank with the current 32-bit representation. |
| `long long` | Partial | Eight-byte values work on every target: 32-bit Arm and RISC-V keep them in register pairs and pass them as AAPCS32 and the RV32 calling convention require. Randomized testing still finds wide expressions the x64 and AArch64 backends miscompile. |
| `unsigned` | Supported | Unsigned char/short/int/long families, arithmetic, conversions, and ABI paths are implemented. |
| `signed` | Supported | Signed scalar spellings, including signed char, are distinct and parsed. |
| `float` | Missing | No floating-point support |
| `double` | Missing | No floating-point support |
| `long double` | Missing | No floating-point support |
| Bit-fields | Supported | `_Bool`, `int`, and `unsigned int` fields pack least-significant-bit first in their conventional allocation units; narrow unsigned fields receive C99 integer promotion. |

### Literals & Constants

| Feature | Status | Current Behavior |
|---------|--------|-----------------|
| Integer suffixes (`u`, `l`, `ll`) | Partial | Common suffix spellings and wide literals are parsed on every target; full candidate-type selection remains incomplete. |
| Wide characters (`L'c'`) | Supported | Lowered as the implementation's `int`-sized execution-wide-character representation. |
| Wide strings (`L"..."`) | Supported | Lowered as NUL-terminated `wchar_t` rodata; supported in expressions, `sizeof`, pointers, and compatible array initialization. |
| Multi-character constants | Supported | Implementation-defined left-to-right packing of up to four bytes. |
| Universal characters (`\u`, `\U`) | Partial | Narrow literals, identifiers, and wide character constants use the implementation's UTF-8 decoding; wide string literals decode to execution-wide-character units. |
| Hex escapes (`\x...`) | Supported | The full following hexadecimal run is consumed; a value that does not fit an `unsigned char` is rejected in narrow literals. |

### Preprocessor Gaps

| Feature | Status | Description |
|---------|--------|-------------|
| `#include` | Partial | Quoted includes and explicit `-I` angle-header search work; hosted C99 headers remain incomplete. |
| Token pasting (`##`) | Supported | Object- and function-like pastes are rescanned and diagnosed when invalid. |
| Stringizing (`#`) | Supported | Function-like macro arguments are stringized with C99 whitespace and escaping behavior. |
| `__DATE__` | Supported | Expands to the C99 date-character array shape. |
| `__TIME__` | Supported | Expands to the C99 time-character array shape. |
| `__STDC__` | Supported | Expands to integer constant `1`; `__STDC_HOSTED__` is also provided. |

### Advanced Features

| Feature | Status | Description |
|---------|--------|-------------|
| Designated initializers | Partial | Record and bounded-array designators work for local, static, and file-scope objects; higher-rank continuation cases remain incomplete. |
| Compound literals | Partial | Limited support |
| `sizeof` type names | Partial | Fixed arrays, pointer-to-array, array-of-pointer, and recursive function-pointer declarators are supported, including callback arrays and global constant expressions; arbitrary mixed derived declarators and a shared general type-name parser remain incomplete. |
| Flexible array members | Supported | Final `[]` struct members have zero fixed extent, support pointer-based element access, and enforce C99 placement constraints. |
| Variable-length arrays | Missing | No runtime-sized arrays |
| `_Complex` | Missing | No complex numbers |
| `_Imaginary` | Missing | No imaginary numbers |

## Non-Standard Behaviors

### GNU Extensions
- Binary literals: `0b101010`
- Escape sequence: `\e` for ESC character
- Computed goto
- Comma elision in variadic macros: `, ## __VA_ARGS__` drops the comma when no variadic argument is given

### Implementation-Specific
- Array compound literals in scalar context use first element
- String literals are modifiable (stored in `.data`, not `.rodata`)
- No strict aliasing rules
- Left-to-right evaluation order (not always guaranteed in C99)
