/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */
#include "../config"
#include "defs.h"
#include "globals.c"

source_location_t synth_built_in_loc;
hashmap_t *PRAGMA_ONCE;
hashmap_t *MACROS;

token_t *pp_lex_skip_space(token_t *tk)
{
    while (tk->next &&
           (tk->next->kind == T_whitespace || tk->next->kind == T_tab))
        tk = tk->next;
    return tk;
}

/* Whether @tk is whitespace, a tab or a newline. */
bool pp_is_layout(const token_t *tk)
{
    return tk->kind == T_whitespace || tk->kind == T_newline ||
           tk->kind == T_tab;
}

/* The first token after @tk that is not layout, or NULL at the end. */
token_t *pp_next_significant(token_t *tk)
{
    const token_t *before = pp_lex_skip_space(tk);

    return before->next;
}

token_t *pp_lex_next_token(token_t *tk, bool skip_space)
{
    if (skip_space)
        tk = pp_lex_skip_space(tk);
    return tk->next;
}

bool pp_lex_peek_token(token_t *tk, token_kind_t kind, bool skip_space)
{
    if (skip_space)
        tk = pp_lex_skip_space(tk);
    return tk->next && tk->next->kind == kind;
}

token_t *pp_lex_expect_token(token_t *tk, token_kind_t kind, bool skip_space)
{
    if (skip_space)
        tk = pp_lex_skip_space(tk);
    if (tk->next) {
        if (tk->next->kind == kind)
            return pp_lex_next_token(tk, false);

        error_at("Unexpected token kind", &tk->next->location);
    }

    error_at("Expect token after this token", &tk->location);
    return tk;
}

/* Copies and isolate the given copied token */
token_t *copy_token(const token_t *tk)
{
    /* The copy overwrites every byte, so zeroing the allocation first would be
     * wasted work -- and this runs once per token of every macro expansion.
     */
    token_t *new_tk = arena_alloc(TOKEN_ARENA, sizeof(token_t));
    memcpy(new_tk, tk, sizeof(token_t));
    new_tk->next = NULL;
    return new_tk;
}

typedef struct macro {
    char *name;
    int param_num;
    token_t *param_names[MAX_PARAMS];
    token_t *replacement;
    bool is_function_like;
    bool is_variadic;
    token_t *variadic_tk;
    bool is_disabled;
    /* build-in function-like macro handler */
    token_t *(*handler)(token_t *);
} macro_t;

bool is_macro_defined(char *name)
{
    const macro_t *macro = hashmap_get(MACROS, name);

    return macro && !macro->is_disabled;
}

/* The freestanding runtime supplies these declarations internally, so their
 * standard headers intentionally remain optional when no implementation search
 * path is configured. All other angle headers must resolve through -I.
 */
bool is_builtin_system_header(const char *name)
{
    return !strcmp(name, "assert.h") || !strcmp(name, "ctype.h") ||
           !strcmp(name, "errno.h") || !strcmp(name, "limits.h") ||
           !strcmp(name, "stdarg.h") || !strcmp(name, "stdbool.h") ||
           !strcmp(name, "stddef.h") || !strcmp(name, "stdint.h") ||
           !strcmp(name, "signal.h") || !strcmp(name, "wchar.h") ||
           !strcmp(name, "stdio.h") || !strcmp(name, "stdlib.h") ||
           !strcmp(name, "string.h") || !strcmp(name, "sys/stat.h");
}

void define_builtin_object_macro(const char *name,
                                 token_kind_t kind,
                                 const char *replacement)
{
    macro_t *macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));

    macro->name = intern_string((char *) name);
    macro->replacement =
        new_token(kind, &synth_built_in_loc, strlen(replacement));
    macro->replacement->literal = intern_string((char *) replacement);
    hashmap_put(MACROS, macro->name, macro);
}

/* Integer minimum macros are preprocessing token sequences rather than one
 * negative literal token: this preserves C's ordinary unary-minus spelling for
 * INT_MIN and LLONG_MIN after macro expansion.
 */
void define_builtin_negative_macro(const char *name, const char *magnitude)
{
    macro_t *macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    token_t *minus = new_token(T_minus, &synth_built_in_loc, 1);
    token_t *numeric =
        new_token(T_numeric, &synth_built_in_loc, strlen(magnitude));

    macro->name = intern_string((char *) name);
    minus->literal = "-";
    numeric->literal = intern_string((char *) magnitude);
    minus->next = numeric;
    macro->replacement = minus;
    hashmap_put(MACROS, macro->name, macro);
}

token_t *append_builtin_macro_token(token_t **replacement,
                                    token_t **tail,
                                    token_kind_t kind,
                                    const char *literal);

/* C99's integer construction macros append a target representation suffix to
 * their single integer-token argument. The preprocessor's ordinary ## path
 * rescans the joined spelling, so the replacement remains a numeric token.
 */
void define_builtin_integer_construction_macro(const char *name,
                                               const char *suffix)
{
    macro_t *macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    token_t *tail = NULL;

    macro->name = intern_string((char *) name);
    macro->is_function_like = true;
    macro->param_num = 1;
    macro->param_names[0] = new_token(T_identifier, &synth_built_in_loc, 5);
    macro->param_names[0]->literal = "value";
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "value");
    if (suffix[0]) {
        append_builtin_macro_token(&macro->replacement, &tail, T_hashhash,
                                   "##");
        append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                                   suffix);
    }
    hashmap_put(MACROS, macro->name, macro);
}

token_t *append_builtin_macro_token(token_t **replacement,
                                    token_t **tail,
                                    token_kind_t kind,
                                    const char *literal)
{
    token_t *token = new_token(kind, &synth_built_in_loc, strlen(literal));

    token->literal = intern_string((char *) literal);
    if (*tail) {
        token_t *previous = *tail;

        previous->next = token;
    } else
        *replacement = token;
    *tail = token;
    return token;
}

/* Spell signed minima as subtraction from a representable maximum. This keeps
 * the macro's expression type equal to the represented C type.
 */
void define_builtin_minimum_macro(const char *name,
                                  const char *maximum,
                                  const char *one)
{
    macro_t *macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    token_t *tail = NULL;

    macro->name = intern_string((char *) name);
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_minus, "-");
    append_builtin_macro_token(&macro->replacement, &tail, T_numeric, maximum);
    append_builtin_macro_token(&macro->replacement, &tail, T_minus, "-");
    append_builtin_macro_token(&macro->replacement, &tail, T_numeric, one);
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    hashmap_put(MACROS, macro->name, macro);
}

/* offsetof is specified as a function-like macro. Route its type/member
 * operands to the parser so record layout, rather than a null-pointer member
 * expression, determines the result.
 */
void install_stddef_offsetof_macro(void)
{
    macro_t *macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    token_t *tail = NULL;

    macro->name = intern_string("offsetof");
    macro->is_function_like = true;
    macro->param_num = 2;
    macro->param_names[0] = new_token(T_identifier, &synth_built_in_loc, 4);
    macro->param_names[0]->literal = "type";
    macro->param_names[1] = new_token(T_identifier, &synth_built_in_loc, 6);
    macro->param_names[1]->literal = "member";

    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "__builtin_offsetof");
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "type");
    append_builtin_macro_token(&macro->replacement, &tail, T_comma, ",");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "member");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    hashmap_put(MACROS, macro->name, macro);
}

/* shecc currently has signed plain char, 16-bit short, 32-bit int/long, and
 * 64-bit long long on every target. Keep <limits.h> tied to those actual
 * language types rather than to the host compiler's ABI.
 */
void install_limits_header(void)
{
    define_builtin_object_macro("CHAR_BIT", T_numeric, "8");
    define_builtin_object_macro("SCHAR_MAX", T_numeric, "127");
    define_builtin_object_macro("UCHAR_MAX", T_numeric, "255U");
    define_builtin_object_macro("CHAR_MAX", T_numeric, "127");
    define_builtin_object_macro("SHRT_MAX", T_numeric, "32767");
    define_builtin_object_macro("USHRT_MAX", T_numeric, "65535U");
    define_builtin_object_macro("INT_MAX", T_numeric, "2147483647");
    define_builtin_object_macro("UINT_MAX", T_numeric, "4294967295U");
    define_builtin_object_macro("LONG_MAX", T_numeric, "2147483647L");
    define_builtin_object_macro("ULONG_MAX", T_numeric, "4294967295UL");
    define_builtin_object_macro("LLONG_MAX", T_numeric,
                                "9223372036854775807LL");
    define_builtin_object_macro("ULLONG_MAX", T_numeric,
                                "18446744073709551615ULL");
    define_builtin_object_macro("MB_LEN_MAX", T_numeric, "1");

    define_builtin_negative_macro("SCHAR_MIN", "128");
    define_builtin_negative_macro("CHAR_MIN", "128");
    define_builtin_negative_macro("SHRT_MIN", "32768");
    define_builtin_minimum_macro("INT_MIN", "2147483647", "1");
    define_builtin_minimum_macro("LONG_MIN", "2147483647L", "1L");
    define_builtin_minimum_macro("LLONG_MIN", "9223372036854775807LL", "1LL");
}

/* A C null pointer constant may be the integer constant expression 0. */
void install_stddef_header(void)
{
    define_builtin_object_macro("NULL", T_numeric, "0");
    install_stddef_offsetof_macro();
}

/* <stdint.h>'s typedefs are parser-provided target types; install the target
 * macro surface alongside them when the header is included.
 */
void install_stdint_header(void)
{
    define_builtin_negative_macro("INT8_MIN", "128");
    define_builtin_object_macro("INT8_MAX", T_numeric, "127");
    define_builtin_object_macro("UINT8_MAX", T_numeric, "255U");
    define_builtin_negative_macro("INT16_MIN", "32768");
    define_builtin_object_macro("INT16_MAX", T_numeric, "32767");
    define_builtin_object_macro("UINT16_MAX", T_numeric, "65535U");
    define_builtin_minimum_macro("INT32_MIN", "2147483647", "1");
    define_builtin_object_macro("INT32_MAX", T_numeric, "2147483647");
    define_builtin_object_macro("UINT32_MAX", T_numeric, "4294967295U");
    define_builtin_minimum_macro("INT64_MIN", "9223372036854775807LL", "1LL");
    define_builtin_object_macro("INT64_MAX", T_numeric,
                                "9223372036854775807LL");
    define_builtin_object_macro("UINT64_MAX", T_numeric,
                                "18446744073709551615ULL");
    define_builtin_minimum_macro("INTMAX_MIN", "9223372036854775807LL", "1LL");
    define_builtin_object_macro("INTMAX_MAX", T_numeric,
                                "9223372036854775807LL");
    define_builtin_object_macro("UINTMAX_MAX", T_numeric,
                                "18446744073709551615ULL");
    define_builtin_minimum_macro("SIG_ATOMIC_MIN", "2147483647", "1");
    define_builtin_object_macro("SIG_ATOMIC_MAX", T_numeric, "2147483647");
    define_builtin_minimum_macro("WCHAR_MIN", "2147483647", "1");
    define_builtin_object_macro("WCHAR_MAX", T_numeric, "2147483647");
    define_builtin_object_macro("WINT_MIN", T_numeric, "0U");
    define_builtin_object_macro("WINT_MAX", T_numeric, "4294967295U");
    define_builtin_negative_macro("INT_LEAST8_MIN", "128");
    define_builtin_object_macro("INT_LEAST8_MAX", T_numeric, "127");
    define_builtin_object_macro("UINT_LEAST8_MAX", T_numeric, "255U");
    define_builtin_negative_macro("INT_LEAST16_MIN", "32768");
    define_builtin_object_macro("INT_LEAST16_MAX", T_numeric, "32767");
    define_builtin_object_macro("UINT_LEAST16_MAX", T_numeric, "65535U");
    define_builtin_minimum_macro("INT_LEAST32_MIN", "2147483647", "1");
    define_builtin_object_macro("INT_LEAST32_MAX", T_numeric, "2147483647");
    define_builtin_object_macro("UINT_LEAST32_MAX", T_numeric, "4294967295U");
    define_builtin_minimum_macro("INT_LEAST64_MIN", "9223372036854775807LL",
                                 "1LL");
    define_builtin_object_macro("INT_LEAST64_MAX", T_numeric,
                                "9223372036854775807LL");
    define_builtin_object_macro("UINT_LEAST64_MAX", T_numeric,
                                "18446744073709551615ULL");
    define_builtin_minimum_macro("INT_FAST8_MIN", "2147483647", "1");
    define_builtin_object_macro("INT_FAST8_MAX", T_numeric, "2147483647");
    define_builtin_object_macro("UINT_FAST8_MAX", T_numeric, "4294967295U");
    define_builtin_minimum_macro("INT_FAST16_MIN", "2147483647", "1");
    define_builtin_object_macro("INT_FAST16_MAX", T_numeric, "2147483647");
    define_builtin_object_macro("UINT_FAST16_MAX", T_numeric, "4294967295U");
    define_builtin_minimum_macro("INT_FAST32_MIN", "2147483647", "1");
    define_builtin_object_macro("INT_FAST32_MAX", T_numeric, "2147483647");
    define_builtin_object_macro("UINT_FAST32_MAX", T_numeric, "4294967295U");
    define_builtin_minimum_macro("INT_FAST64_MIN", "9223372036854775807LL",
                                 "1LL");
    define_builtin_object_macro("INT_FAST64_MAX", T_numeric,
                                "9223372036854775807LL");
    define_builtin_object_macro("UINT_FAST64_MAX", T_numeric,
                                "18446744073709551615ULL");
    define_builtin_integer_construction_macro("INT8_C", "");
    define_builtin_integer_construction_macro("UINT8_C", "");
    define_builtin_integer_construction_macro("INT16_C", "");
    define_builtin_integer_construction_macro("UINT16_C", "");
    define_builtin_integer_construction_macro("INT32_C", "");
    define_builtin_integer_construction_macro("UINT32_C", "U");
    define_builtin_integer_construction_macro("INT64_C", "LL");
    define_builtin_integer_construction_macro("UINT64_C", "ULL");
    define_builtin_integer_construction_macro("INTMAX_C", "LL");
    define_builtin_integer_construction_macro("UINTMAX_C", "ULL");
    if (PTR_SIZE == 8) {
        define_builtin_minimum_macro("INTPTR_MIN", "9223372036854775807LL",
                                     "1LL");
        define_builtin_object_macro("INTPTR_MAX", T_numeric,
                                    "9223372036854775807LL");
        define_builtin_object_macro("UINTPTR_MAX", T_numeric,
                                    "18446744073709551615ULL");
        define_builtin_minimum_macro("PTRDIFF_MIN", "9223372036854775807LL",
                                     "1LL");
        define_builtin_object_macro("PTRDIFF_MAX", T_numeric,
                                    "9223372036854775807LL");
        define_builtin_object_macro("SIZE_MAX", T_numeric,
                                    "18446744073709551615ULL");
    } else {
        define_builtin_minimum_macro("INTPTR_MIN", "2147483647L", "1L");
        define_builtin_object_macro("INTPTR_MAX", T_numeric, "2147483647L");
        define_builtin_object_macro("UINTPTR_MAX", T_numeric, "4294967295UL");
        define_builtin_minimum_macro("PTRDIFF_MIN", "2147483647L", "1L");
        define_builtin_object_macro("PTRDIFF_MAX", T_numeric, "2147483647L");
        define_builtin_object_macro("SIZE_MAX", T_numeric, "4294967295UL");
    }
}

/* stdbool.h is entirely macro-defined in C99. Supplying it here makes the
 * freestanding compiler usable with --no-libc too, rather than relying on the
 * private definitions prepended from lib/c.h.
 */
void install_stdbool_header(void)
{
    define_builtin_object_macro("bool", T_identifier, "_Bool");
    define_builtin_object_macro("true", T_numeric, "1");
    define_builtin_object_macro("false", T_numeric, "0");
    define_builtin_object_macro("__bool_true_false_are_defined", T_numeric,
                                "1");
}

/* iso646.h is likewise a pure C99 macro header. Keep the replacement token
 * kinds explicit so the operators retain their ordinary parser precedence.
 */
void install_iso646_header(void)
{
    define_builtin_object_macro("and", T_log_and, "&&");
    define_builtin_object_macro("and_eq", T_andeq, "&=");
    define_builtin_object_macro("bitand", T_ampersand, "&");
    define_builtin_object_macro("bitor", T_bit_or, "|");
    define_builtin_object_macro("compl", T_bit_not, "~");
    define_builtin_object_macro("not", T_log_not, "!");
    define_builtin_object_macro("not_eq", T_noteq, "!=");
    define_builtin_object_macro("or", T_log_or, "||");
    define_builtin_object_macro("or_eq", T_oreq, "|=");
    define_builtin_object_macro("xor", T_bit_xor, "^");
    define_builtin_object_macro("xor_eq", T_xoreq, "^=");
}

/* assert.h is a function-like macro header. Reinstall it on each inclusion so
 * an NDEBUG definition made before that inclusion controls its replacement.
 */
void install_assert_header(void)
{
    macro_t *macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    token_t *tail = NULL;

    macro->name = intern_string("assert");
    macro->is_function_like = true;
    macro->param_num = 1;
    macro->param_names[0] = new_token(T_identifier, &synth_built_in_loc, 4);
    macro->param_names[0]->literal = "expr";

    if (!is_macro_defined("NDEBUG")) {
        append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket,
                                   "(");
        append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket,
                                   "(");
        append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                                   "expr");
        append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                                   ")");
        append_builtin_macro_token(&macro->replacement, &tail, T_question, "?");
        append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket,
                                   "(");
        append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                                   "void");
        append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                                   ")");
        append_builtin_macro_token(&macro->replacement, &tail, T_numeric, "0");
        append_builtin_macro_token(&macro->replacement, &tail, T_colon, ":");
        append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                                   "__assert_fail");
        append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket,
                                   "(");
        append_builtin_macro_token(&macro->replacement, &tail, T_hash, "#");
        append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                                   "expr");
        append_builtin_macro_token(&macro->replacement, &tail, T_comma, ",");
        append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                                   "__FILE__");
        append_builtin_macro_token(&macro->replacement, &tail, T_comma, ",");
        append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                                   "__LINE__");
        append_builtin_macro_token(&macro->replacement, &tail, T_comma, ",");
        append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                                   "__func__");
        append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                                   ")");
        append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                                   ")");
    } else {
        /* C99 requires the disabled form to remain a void expression while not
         * evaluating its operand.
         */
        append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket,
                                   "(");
        append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                                   "void");
        append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                                   ")");
        append_builtin_macro_token(&macro->replacement, &tail, T_numeric, "0");
    }
    hashmap_put(MACROS, macro->name, macro);
}

macro_t *new_stdarg_macro(const char *name,
                          const char *first_param,
                          const char *second_param)
{
    macro_t *macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));

    macro->name = intern_string((char *) name);
    macro->is_function_like = true;
    macro->param_num = second_param ? 2 : 1;
    macro->param_names[0] =
        new_token(T_identifier, &synth_built_in_loc, strlen(first_param));
    macro->param_names[0]->literal = intern_string((char *) first_param);
    if (second_param) {
        macro->param_names[1] =
            new_token(T_identifier, &synth_built_in_loc, strlen(second_param));
        macro->param_names[1]->literal = intern_string((char *) second_param);
    }
    return macro;
}

/* The current ABI spills every variadic argument into a pointer-sized slot.
 * These macros deliberately cover integer and pointer arguments only; floating
 * argument promotion waits for the compiler's explicit FP ABI support.
 */
void install_stdarg_header(void)
{
    macro_t *macro;
    token_t *tail;

    define_builtin_object_macro("__VA_SLOT_WORDS", T_numeric,
                                PTR_SIZE == 8 ? "2" : "1");
    define_builtin_object_macro("__VA_SLOT_BYTES", T_numeric,
                                PTR_SIZE == 8 ? "8" : "4");

    macro = new_stdarg_macro("va_start", "ap", "last");
    tail = NULL;
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier, "ap");
    append_builtin_macro_token(&macro->replacement, &tail, T_assign, "=");
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "va_list");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    append_builtin_macro_token(&macro->replacement, &tail, T_ampersand, "&");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "last");
    append_builtin_macro_token(&macro->replacement, &tail, T_plus, "+");
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_sizeof, "sizeof");
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "last");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    append_builtin_macro_token(&macro->replacement, &tail, T_plus, "+");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "__VA_SLOT_BYTES");
    append_builtin_macro_token(&macro->replacement, &tail, T_minus, "-");
    append_builtin_macro_token(&macro->replacement, &tail, T_numeric, "1");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    append_builtin_macro_token(&macro->replacement, &tail, T_divide, "/");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "__VA_SLOT_BYTES");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    append_builtin_macro_token(&macro->replacement, &tail, T_asterisk, "*");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "__VA_SLOT_WORDS");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    hashmap_put(MACROS, macro->name, macro);

    macro = new_stdarg_macro("va_arg", "ap", "type");
    tail = NULL;
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "__builtin_va_arg");
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_ampersand, "&");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier, "ap");
    append_builtin_macro_token(&macro->replacement, &tail, T_comma, ",");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "type");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    hashmap_put(MACROS, macro->name, macro);

    macro = new_stdarg_macro("va_copy", "dest", "src");
    tail = NULL;
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "dest");
    append_builtin_macro_token(&macro->replacement, &tail, T_assign, "=");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier, "src");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    hashmap_put(MACROS, macro->name, macro);

    macro = new_stdarg_macro("va_end", "ap", NULL);
    tail = NULL;
    append_builtin_macro_token(&macro->replacement, &tail, T_open_bracket, "(");
    append_builtin_macro_token(&macro->replacement, &tail, T_identifier,
                               "void");
    append_builtin_macro_token(&macro->replacement, &tail, T_close_bracket,
                               ")");
    append_builtin_macro_token(&macro->replacement, &tail, T_numeric, "0");
    hashmap_put(MACROS, macro->name, macro);
}

/* An angle header is lexed as ordinary preprocessing tokens. Recover its
 * spelling from the immutable source buffer so dotted and slash-separated names
 * do not need special punctuation reconstruction here. That buffer holds the
 * physical bytes, so read it through translation phases 1 and 2: a trigraph
 * stands for its character and a backslash-newline joins the name.
 */
bool resolve_angle_include(token_t *open,
                           token_t *close,
                           char *resolved,
                           int resolved_size)
{
    strbuf_t *source = get_file_buf(open->location.physical_filename);
    int pos = open->location.pos + open->location.len;
    int end = close->location.pos;
    int len = 0;
    char name[MAX_LINE_LEN];

    while ((pos = skip_splices(source, pos)) < end) {
        if (len >= MAX_LINE_LEN - 1)
            error_at("Invalid #include <...> header name", &open->location);
        name[len++] = source_char_at(source, pos);
        pos += source_char_width(source, pos);
    }
    if (len <= 0)
        error_at("Invalid #include <...> header name", &open->location);
    name[len] = '\0';
    for (int i = 0; i < len; i++)
        if (name[i] == ' ' || name[i] == '\t')
            error_at("Whitespace is not permitted in #include <...>",
                     &open->location);

    for (int i = 0; i < include_dirs_idx; i++) {
        FILE *file;

        /* A truncated path would name some other file, or none, and let a
         * built-in header silently stand in for the one in this directory.
         */
        if (snprintf(resolved, resolved_size, "%s/%s", include_dirs[i], name) >=
            resolved_size)
            error_at("#include path is too long", &open->location);
        file = fopen(resolved, "rb");
        if (file) {
            fclose(file);
            return true;
        }
    }
    if (!strcmp(name, "stdbool.h")) {
        install_stdbool_header();
        return false;
    }
    if (!strcmp(name, "iso646.h")) {
        install_iso646_header();
        return false;
    }
    if (!strcmp(name, "limits.h")) {
        install_limits_header();
        return false;
    }
    if (!strcmp(name, "stddef.h")) {
        install_stddef_header();
        return false;
    }
    if (!strcmp(name, "stdint.h")) {
        install_stdint_header();
        return false;
    }
    if (!strcmp(name, "assert.h")) {
        install_assert_header();
        return false;
    }
    if (!strcmp(name, "stdarg.h")) {
        install_stdarg_header();
        return false;
    }
    if (is_builtin_system_header(name))
        return false;
    error_at("Angle header not found in -I search paths", &open->location);
    return false;
}

/* file_macro_handler is responsible for expanding built-in macro "__FILE__"
 * inplace with a string token with file's relative path's name literally
 */
token_t *file_macro_handler(token_t *tk)
{
    token_t *new_tk = copy_token(tk);
    new_tk->kind = T_string;
    new_tk->literal = tk->location.filename;
    memcpy(&new_tk->location, &tk->location, sizeof(source_location_t));
    return new_tk;
}

/* line_macro_handler is responsible for expanding built-in macro "__LINE__"
 * inplace with a string token with line number literally
 */
token_t *line_macro_handler(token_t *tk)
{
    char line[MAX_TOKEN_LEN];
    snprintf(line, MAX_TOKEN_LEN, "%d", tk->location.line);

    token_t *new_tk = copy_token(tk);
    new_tk->kind = T_numeric;
    new_tk->literal = intern_string(line);
    memcpy(&new_tk->location, &tk->location, sizeof(source_location_t));
    return new_tk;
}

/* C99 6.10.8 requires these strings to describe the translation time. The
 * generated configuration supplies that time once for all bootstrap stages;
 * consulting a clock while compiling would make stage 1 and stage 2 differ.
 *
 * A handler receives the outermost invocation, which is the name of whatever
 * macro led here rather than __DATE__ or __TIME__ itself, so each gets its own
 * handler instead of one that looks at the name.
 */
token_t *translation_timestamp_token(token_t *tk, char *literal)
{
    token_t *new_tk = copy_token(tk);

    new_tk->kind = T_string;
    new_tk->literal = literal;
    memcpy(&new_tk->location, &tk->location, sizeof(source_location_t));
    return new_tk;
}

token_t *date_macro_handler(token_t *tk)
{
    return translation_timestamp_token(tk, SHECC_TRANSLATION_DATE);
}

token_t *time_macro_handler(token_t *tk)
{
    return translation_timestamp_token(tk, SHECC_TRANSLATION_TIME);
}

/* Remap the remaining physical tokens from one source stream for #line.
 * Included files are preprocessed recursively from separate token streams, so
 * they retain their own logical locations.
 */
void pp_apply_line_directive(token_t *first,
                             char *original_filename,
                             char *logical_filename,
                             int line_delta)
{
    for (token_t *it = first; it && it->kind != T_eof; it = it->next) {
        if (strcmp(it->location.physical_filename, original_filename))
            continue;
        it->location.line += line_delta;
        if (logical_filename)
            it->location.filename = logical_filename;
    }
}

/* hide_set_t is used to track which macros have been expanded in the previous
 * expanding context, if so, it'll get added into hide set of context to prevent
 * endless recursion macro expansion.
 */
typedef struct hide_set {
    char *name;
    struct hide_set *next;
} hide_set_t;

hide_set_t *new_hide_set(char *name)
{
    hide_set_t *hs = arena_alloc(TOKEN_ARENA, sizeof(hide_set_t));
    hs->name = name;
    hs->next = NULL;
    return hs;
}

hide_set_t *hide_set_union(hide_set_t *hs1, hide_set_t *hs2)
{
    hide_set_t head;
    hide_set_t *cur = &head;

    for (; hs1; hs1 = hs1->next) {
        cur->next = new_hide_set(hs1->name);
        cur = cur->next;
    }
    cur->next = hs2;

    return head.next;
}

bool hide_set_contains(hide_set_t *hs, const char *name)
{
    for (; hs; hs = hs->next)
        if (!strcmp(hs->name, name))
            return true;
    return false;
}

typedef enum { CK_if_then, CK_elif_then, CK_else_then } cond_kind_t;

/* cond_incl_t is used as a stack-like context to track conditional macro
 * directives' expansion, and gives information to the expansion context to
 * process the token stream with correct behavior.
 */
typedef struct cond_incl {
    struct cond_incl *prev;
    cond_kind_t ctx;
    token_t *tk;
    bool included;
} cond_incl_t;

cond_incl_t *push_cond(cond_incl_t *ci, token_t *tk, bool included)
{
    cond_incl_t *cond = arena_alloc(TOKEN_ARENA, sizeof(cond_incl_t));
    cond->prev = ci;
    cond->ctx = CK_if_then;
    cond->tk = tk;
    cond->included = included;
    return cond;
}

/* preprocess_ctx_t is used to track various inforamtion when expanding token
 * stream, the context state may vary due to the current expanding object, but
 * in general case, it will tries to inherit parent context state if possible.
 *
 * Due to the standard that token stream are always ends with EOF token, the
 * default behavior is not to trim EOF token, but if the result requires EOF
 * token to be present, set trim_eof to true would suffice.
 */
typedef struct preprocess_ctx {
    hide_set_t *hide_set;

    /* The hide set of the invocation whose arguments macro_args holds. An
     * argument is replaced before it is substituted (C99 6.10.3.1), outside the
     * macro it is an argument to, so "A(A(1))" expands both.
     */
    hide_set_t *arg_hide_set;
    hashmap_t *macro_args;
    token_t *expanded_from;
    token_t *end_of_token; /* end of token stream of current context */
    bool trim_eof;
} preprocess_ctx_t;

token_t *pp_preprocess_internal(token_t *tk, preprocess_ctx_t *ctx);
char *token_to_string(token_t *tk, char *dest);

/* Macro expansion for #line must be confined to its operands: sending the live
 * stream through pp_preprocess_internal() could consume following source
 * directives. Copy through (but not including) the directive newline and append
 * a private EOF sentinel for a normal standalone rescan.
 */
token_t *pp_expand_line_operands(token_t *directive)
{
    token_t head;
    token_t *tail = &head;
    token_t *raw = directive->next;
    preprocess_ctx_t ctx;

    head.next = NULL;
    while (raw && raw->kind != T_newline && raw->kind != T_eof) {
        tail->next = copy_token(raw);
        tail = tail->next;
        raw = raw->next;
    }
    if (!raw || raw->kind != T_newline)
        error_at("Unterminated #line directive", &directive->location);

    tail->next = new_token(T_eof, &raw->location, 0);
    ctx.expanded_from = directive;
    ctx.hide_set = NULL;
    ctx.arg_hide_set = NULL;
    ctx.macro_args = NULL;
    ctx.trim_eof = false;
    return pp_preprocess_internal(head.next, &ctx);
}

void pp_read_line_operands(token_t *directive,
                           int *requested_line,
                           char **logical_filename)
{
    token_t head;
    token_t *cursor;
    token_t *expanded = pp_expand_line_operands(directive);

    head.next = expanded;
    cursor = &head;
    if (!pp_lex_peek_token(cursor, T_numeric, true))
        error_at("#line requires a decimal line number", &directive->location);
    cursor = pp_lex_next_token(cursor, true);

    /* C99 6.10.4 takes a digit sequence, which is decimal even with a leading
     * zero and admits neither a prefix nor a suffix, naming a line from 1 to
     * 2147483647.
     */
    *requested_line = 0;
    for (int i = 0; cursor->literal[i]; i++) {
        int digit = cursor->literal[i] - '0';

        if (digit < 0 || digit > 9)
            error_at("#line requires a decimal line number", &cursor->location);
        if (*requested_line > (2147483647 - digit) / 10)
            error_at("#line number is out of range", &cursor->location);
        *requested_line = *requested_line * 10 + digit;
    }
    if (*requested_line <= 0)
        error_at("#line number must be positive", &cursor->location);
    if (pp_lex_peek_token(cursor, T_string, true)) {
        cursor = pp_lex_next_token(cursor, true);
        *logical_filename = intern_string(cursor->literal);
    }
    if (!pp_lex_peek_token(cursor, T_eof, true))
        error_at("Unexpected token in #line directive", &cursor->location);
}

int pp_get_operator_prio(opcode_t op)
{
    /* https://www.cs.uic.edu/~i109/Notes/COperatorPrecedenceTable.pdf */
    switch (op) {
    case OP_ternary:
        return 3;
    case OP_log_or:
        return 4;
    case OP_log_and:
        return 5;
    case OP_bit_or:
        return 6;
    case OP_bit_xor:
        return 7;
    case OP_bit_and:
        return 8;
    case OP_eq:
    case OP_neq:
        return 9;
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
        return 10;
    case OP_lshift:
    case OP_rshift:
        return 11;
    case OP_add:
    case OP_sub:
        return 12;
    case OP_mul:
    case OP_div:
    case OP_mod:
        return 13;
    default:
        return 0;
    }
}

int pp_get_unary_operator_prio(opcode_t op)
{
    switch (op) {
    case OP_add:
    case OP_sub:
    case OP_bit_not:
    case OP_log_not:
        return 14;
    default:
        return 0;
    }
}

/* Look ahead at the next operator. Infix parsing needs to inspect an operator's
 * precedence before it consumes it, while unary parsing consumes its operator
 * immediately.
 */
token_t *pp_get_operator(token_t *tk, opcode_t *op, bool consume)
{
    tk = pp_lex_skip_space(tk);

    if (!tk->next)
        error_at("Unexpected error when trying to evaulate constant operator",
                 &tk->location);

    switch (tk->next->kind) {
    case T_plus:
        op[0] = OP_add;
        break;
    case T_minus:
        op[0] = OP_sub;
        break;
    case T_bit_not:
        op[0] = OP_bit_not;
        break;
    case T_log_not:
        op[0] = OP_log_not;
        break;
    case T_asterisk:
        op[0] = OP_mul;
        break;
    case T_divide:
        op[0] = OP_div;
        break;
    case T_mod:
        op[0] = OP_mod;
        break;
    case T_lshift:
        op[0] = OP_lshift;
        break;
    case T_rshift:
        op[0] = OP_rshift;
        break;
    case T_log_and:
        op[0] = OP_log_and;
        break;
    case T_log_or:
        op[0] = OP_log_or;
        break;
    case T_eq:
        op[0] = OP_eq;
        break;
    case T_noteq:
        op[0] = OP_neq;
        break;
    case T_lt:
        op[0] = OP_lt;
        break;
    case T_le:
        op[0] = OP_leq;
        break;
    case T_gt:
        op[0] = OP_gt;
        break;
    case T_ge:
        op[0] = OP_geq;
        break;
    case T_ampersand:
        op[0] = OP_bit_and;
        break;
    case T_bit_or:
        op[0] = OP_bit_or;
        break;
    case T_bit_xor:
        op[0] = OP_bit_xor;
        break;
    case T_question:
        op[0] = OP_ternary;
        break;
    default:
        /* Maybe it's an operand, we immediately return here. */
        op[0] = OP_generic;
        return tk;
    }
    return consume ? pp_lex_next_token(tk, true) : tk;
}

/* C99 evaluates all integer constants in #if as intmax_t or uintmax_t. Keep the
 * bit pattern separate from its signedness so unsigned comparisons and shifts
 * do not accidentally use the host compiler's signed int semantics.
 */
typedef struct pp_integer {
    unsigned int lo;
    unsigned int hi;
    unsigned int is_unsigned;
    int enum_width;
} pp_integer_t;

/* The parser reuses this exact two-word evaluator for typed enum constant
 * expressions. Outside that narrowly scoped use, #if's standard rule still
 * treats unknown identifiers as zero.
 */
block_t *pp_integer_constant_scope = NULL;

void pp_enum_normalize(pp_integer_t *val)
{
    if (val->enum_width != 32)
        return;
    val->hi = val->is_unsigned || !(val->lo & 0x80000000U) ? 0 : ~0U;
}

void pp_enum_convert_width(pp_integer_t *val, int width, int is_unsigned)
{
    if (width == 64 && val->enum_width == 32 && !val->is_unsigned &&
        (val->lo & 0x80000000U))
        val->hi = ~0U;
    if (width == 32)
        val->hi = 0;
    val->enum_width = width;
    val->is_unsigned = is_unsigned;
    pp_enum_normalize(val);
}

void pp_enum_usual_arithmetic(pp_integer_t *lhs, pp_integer_t *rhs)
{
    int width =
        lhs->enum_width > rhs->enum_width ? lhs->enum_width : rhs->enum_width;
    int is_unsigned;

    if (width == 64)
        is_unsigned = (lhs->enum_width == 64 && lhs->is_unsigned) ||
                      (rhs->enum_width == 64 && rhs->is_unsigned);
    else
        is_unsigned = lhs->is_unsigned || rhs->is_unsigned;
    pp_enum_convert_width(lhs, width, is_unsigned);
    pp_enum_convert_width(rhs, width, is_unsigned);
}

void pp_set_boolean(pp_integer_t *val, int truth)
{
    val->lo = truth != 0;
    val->hi = 0;
    val->is_unsigned = false;
    val->enum_width = 32;
}

int pp_is_true(const pp_integer_t *val)
{
    return val->lo || val->hi;
}

int PP_USES_UNSIGNED(const pp_integer_t *lhs, const pp_integer_t *rhs)
{
    return lhs->is_unsigned || rhs->is_unsigned;
}

int pp_is_negative(const pp_integer_t *val)
{
    return !val->is_unsigned && (val->hi & 0x80000000U);
}

int pp_compare_unsigned(const pp_integer_t *lhs, const pp_integer_t *rhs)
{
    if (lhs->hi != rhs->hi)
        return lhs->hi > rhs->hi ? 1 : -1;
    if (lhs->lo != rhs->lo)
        return lhs->lo > rhs->lo ? 1 : -1;
    return 0;
}

void pp_negate(pp_integer_t *val)
{
    val->lo = ~val->lo;
    val->hi = ~val->hi;
    val->lo++;
    if (val->lo == 0)
        val->hi++;
}

void pp_add(pp_integer_t *lhs, const pp_integer_t *rhs)
{
    unsigned int old_lo = lhs->lo;

    lhs->lo += rhs->lo;
    lhs->hi += rhs->hi + (lhs->lo < old_lo);
}

void pp_subtract(pp_integer_t *lhs, const pp_integer_t *rhs)
{
    unsigned int old_lo = lhs->lo;

    lhs->lo -= rhs->lo;
    lhs->hi -= rhs->hi + (old_lo < rhs->lo);
}

void pp_shift_left_one(pp_integer_t *val)
{
    val->hi = (val->hi << 1) | (val->lo >> 31);
    val->lo <<= 1;
}

void pp_multiply_small(pp_integer_t *val, unsigned int amount)
{
    unsigned int original_lo = val->lo, original_hi = val->hi;

    val->lo = val->hi = 0;
    for (unsigned int i = 0; i < amount; i++) {
        unsigned int old_lo = val->lo;

        val->lo += original_lo;
        val->hi += original_hi + (val->lo < old_lo);
    }
}

int pp_literal_will_overflow(const pp_integer_t *value, int base, int digit)
{
    unsigned int limit_lo, limit_hi;
    int remainder;

    if (base == 2) {
        limit_lo = 0xffffffffU;
        limit_hi = 0x7fffffffU;
        remainder = 1;
    } else if (base == 8) {
        limit_lo = 0xffffffffU;
        limit_hi = 0x1fffffffU;
        remainder = 7;
    } else if (base == 10) {
        limit_lo = 0x99999999U;
        limit_hi = 0x19999999U;
        remainder = 5;
    } else {
        limit_lo = 0xffffffffU;
        limit_hi = 0x0fffffffU;
        remainder = 15;
    }
    return value->hi > limit_hi ||
           (value->hi == limit_hi && value->lo > limit_lo) ||
           (value->hi == limit_hi && value->lo == limit_lo &&
            digit > remainder);
}

int pp_numeric_suffix_is_valid(const char *suffix)
{
    int pos = 0;

    if ((suffix[pos] | 32) == 'u')
        pos++;
    if ((suffix[pos] | 32) == 'l') {
        pos++;
        if ((suffix[pos] | 32) == 'l')
            pos++;
    }
    if ((suffix[pos] | 32) == 'u')
        pos++;
    return suffix[pos] == '\0';
}

void pp_multiply(pp_integer_t *lhs, const pp_integer_t *rhs)
{
    pp_integer_t multiplicand, multiplier, product = {0};

    multiplicand.lo = lhs->lo;
    multiplicand.hi = lhs->hi;
    multiplicand.is_unsigned = lhs->is_unsigned;
    multiplier.lo = rhs->lo;
    multiplier.hi = rhs->hi;
    multiplier.is_unsigned = rhs->is_unsigned;

    for (int i = 0; i < 64; i++) {
        if (multiplier.lo & 1)
            pp_add(&product, &multiplicand);
        multiplier.lo = (multiplier.lo >> 1) | (multiplier.hi << 31);
        multiplier.hi >>= 1;
        pp_shift_left_one(&multiplicand);
    }
    product.is_unsigned = PP_USES_UNSIGNED(lhs, rhs);
    product.enum_width = lhs->enum_width;
    lhs->lo = product.lo;
    lhs->hi = product.hi;
    lhs->is_unsigned = product.is_unsigned;
    lhs->enum_width = product.enum_width;
}

void pp_shift_right_one(pp_integer_t *val, int arithmetic)
{
    unsigned int sign = arithmetic && (val->hi & 0x80000000U) ? 0x80000000U : 0;

    val->lo = (val->lo >> 1) | (val->hi << 31);
    val->hi = (val->hi >> 1) | sign;
}

void pp_divmod_unsigned(const pp_integer_t *numerator,
                        const pp_integer_t *denominator,
                        pp_integer_t *quotient,
                        pp_integer_t *remainder)
{
    quotient->lo = quotient->hi = remainder->lo = remainder->hi = 0;
    quotient->is_unsigned = remainder->is_unsigned = false;
    for (int i = 63; i >= 0; i--) {
        unsigned int bit = i >= 32 ? (numerator->hi >> (i - 32)) & 1
                                   : (numerator->lo >> i) & 1;

        pp_shift_left_one(remainder);
        remainder->lo |= bit;
        pp_shift_left_one(quotient);
        if (pp_compare_unsigned(remainder, denominator) >= 0) {
            pp_subtract(remainder, denominator);
            quotient->lo |= 1;
        }
    }
}

void pp_divmod(pp_integer_t *lhs, const pp_integer_t *rhs, int remainder)
{
    int use_unsigned = PP_USES_UNSIGNED(lhs, rhs);
    int enum_width = lhs->enum_width;
    int lhs_negative = !use_unsigned && pp_is_negative(lhs);
    int rhs_negative = !use_unsigned && pp_is_negative(rhs);
    pp_integer_t numerator, denominator, quotient, modulo;

    numerator.lo = lhs->lo;
    numerator.hi = lhs->hi;
    numerator.is_unsigned = lhs->is_unsigned;
    denominator.lo = rhs->lo;
    denominator.hi = rhs->hi;
    denominator.is_unsigned = rhs->is_unsigned;

    if (lhs_negative)
        pp_negate(&numerator);
    if (rhs_negative)
        pp_negate(&denominator);
    pp_divmod_unsigned(&numerator, &denominator, &quotient, &modulo);
    if (!remainder && lhs_negative != rhs_negative)
        pp_negate(&quotient);
    if (remainder && lhs_negative)
        pp_negate(&modulo);
    if (remainder) {
        lhs->lo = modulo.lo;
        lhs->hi = modulo.hi;
    } else {
        lhs->lo = quotient.lo;
        lhs->hi = quotient.hi;
    }
    lhs->is_unsigned = use_unsigned;
    lhs->enum_width = enum_width;
}

void pp_parse_integer_literal(token_t *tk, pp_integer_t *val)
{
    const char *literal = tk->literal;
    int i = 0, base = 10;
    int is_decimal;
    pp_integer_t parsed = {0};

    if (literal[0] == '0') {
        if ((literal[1] | 32) == 'x') {
            base = 16;
            i = 2;
        } else if ((literal[1] | 32) == 'b') {
            base = 2;
            i = 2;
        } else if (literal[1] && (literal[1] | 32) != 'u' &&
                   (literal[1] | 32) != 'l') {
            base = 8;
            i = 1;
        }
    }
    is_decimal = base == 10;
    while (literal[i] && (literal[i] | 32) != 'u' && (literal[i] | 32) != 'l') {
        int digit;
        char c = literal[i++];

        if (isdigit(c))
            digit = c - '0';
        else {
            c |= 32;
            digit = c - 'a' + 10;
        }
        if (digit < 0 || digit >= base)
            error_at("Invalid digit in integer constant", &tk->location);
        if (pp_literal_will_overflow(&parsed, base, digit))
            error_at("Integer constant exceeds uintmax_t", &tk->location);
        pp_multiply_small(&parsed, base);
        pp_integer_t addend = {digit, 0, false, 0};

        pp_add(&parsed, &addend);
    }
    val->is_unsigned = false;
    const char *suffix = literal + i;
    while (literal[i]) {
        if (literal[i] == 'u' || literal[i] == 'U') {
            val->is_unsigned = true;
        } else if (literal[i] != 'l' && literal[i] != 'L')
            error_at("Invalid integer constant suffix", &tk->location);
        i++;
    }
    if (!pp_numeric_suffix_is_valid(suffix))
        error_at("Invalid integer constant suffix", &tk->location);
    if (!val->is_unsigned && (parsed.hi & 0x80000000U)) {
        if (is_decimal)
            error_at("Integer constant exceeds intmax_t", &tk->location);
        val->is_unsigned = true;
    }
    val->lo = parsed.lo;
    val->hi = parsed.hi;
    if (pp_integer_constant_scope) {
        int long_count = 0;

        for (int j = 0; suffix[j]; j++)
            if ((suffix[j] | 32) == 'l')
                long_count++;
        if (long_count >= 2) {
            val->enum_width = 64;
        } else if (val->is_unsigned) {
            val->enum_width = parsed.hi ? 64 : 32;
        } else if (!is_decimal && !parsed.hi && parsed.lo > 0x7fffffffU) {
            /* In this ILP32 model, nondecimal constants select unsigned int or
             * unsigned long before they reach signed long long.
             */
            val->is_unsigned = true;
            val->enum_width = 32;
        } else {
            val->enum_width = parsed.hi || parsed.lo > 0x7fffffffU ? 64 : 32;
        }
        pp_enum_normalize(val);
    }
}

token_t *pp_read_constant_infix_expr(int precedence,
                                     token_t *tk,
                                     pp_integer_t *val,
                                     bool evaluate);

/* Expand one function-like macro invocation in a #if token stream. The normal
 * preprocessor already owns argument substitution, rescanning, and hide-set
 * handling; give it only this balanced invocation so it cannot consume the rest
 * of the directive.
 */
token_t *pp_expand_function_macro_in_constant_expr(token_t *before)
{
    token_t head;
    token_t *tail = &head;
    token_t *first = before->next;
    token_t *end = first;
    int bracket_depth = 0;
    preprocess_ctx_t ctx;

    head.next = NULL;
    while (end) {
        if (end->kind == T_open_bracket)
            bracket_depth++;
        else if (end->kind == T_close_bracket && --bracket_depth == 0)
            break;
        end = end->next;
    }
    if (!end)
        error_at("Unterminated function-like macro invocation",
                 &first->location);

    for (token_t *cur = first;; cur = cur->next) {
        tail->next = copy_token(cur);
        tail = tail->next;
        if (cur == end)
            break;
    }
    tail->next = NULL;

    ctx.expanded_from = first;
    ctx.hide_set = NULL;
    ctx.arg_hide_set = NULL;
    ctx.macro_args = NULL;
    ctx.trim_eof = true;
    token_t *expanded = pp_preprocess_internal(head.next, &ctx);
    token_t *after = end->next;

    before->next = expanded;
    if (expanded)
        ctx.end_of_token->next = after;
    else
        before->next = after;
    return before;
}

token_t *pp_read_constant_expr_operand(token_t *tk,
                                       pp_integer_t *val,
                                       bool evaluate)
{
    opcode_t unary;

    tk = pp_get_operator(tk, &unary, false);
    if (pp_get_unary_operator_prio(unary)) {
        tk = pp_lex_next_token(tk, true);
        tk = pp_read_constant_expr_operand(tk, val, evaluate);
        if (evaluate) {
            if (unary == OP_sub)
                pp_negate(val);
            else if (unary == OP_bit_not)
                val->lo = ~val->lo, val->hi = ~val->hi;
            else if (unary == OP_log_not)
                pp_set_boolean(val, !pp_is_true(val));
        }
        return tk;
    }

    if (pp_lex_peek_token(tk, T_floating, true))
        error_at("Floating constant is not permitted in #if expression",
                 &tk->location);

    /* Parser-side enum evaluation shares this token walker, but unlike #if it
     * admits C's sizeof integer constant expressions. Reuse the parser's
     * unevaluated sizeof reader and leave both cursors at the consumed operand.
     */
    if (pp_integer_constant_scope && pp_lex_peek_token(tk, T_sizeof, true)) {
        cur_token = tk;
        lex_expect(T_sizeof);
        val->lo = read_sizeof_constant(pp_integer_constant_scope);
        val->hi = 0;
        val->is_unsigned = false;
        val->enum_width = 32;
        return cur_token;
    }

    if (pp_lex_peek_token(tk, T_numeric, true)) {
        tk = pp_lex_next_token(tk, true);
        pp_parse_integer_literal(tk, val);
        return tk;
    }

    if (pp_lex_peek_token(tk, T_char, true) ||
        pp_lex_peek_token(tk, T_wchar, true)) {
        char unescaped[MAX_TOKEN_LEN];

        tk = pp_lex_next_token(tk, true);
        if (unescape_string(tk->literal, unescaped, MAX_TOKEN_LEN) < 0)
            error_at("Invalid escape sequence", &tk->location);
        int character;

        if (tk->kind != T_wchar)
            character = parse_character_constant(tk->literal);
        else if (!wide_character_constant(tk->literal, &character))
            error_at("Invalid wide character escape sequence", &tk->location);

        /* A character constant has type int, so a typed enumerator or case
         * label gives it the int width a numeric operand gets.
         */
        val->lo = character;
        val->hi = character < 0 ? ~0U : 0;
        val->is_unsigned = false;
        val->enum_width = 32;
        return tk;
    }

    if (pp_lex_peek_token(tk, T_open_bracket, true)) {
        tk = pp_lex_next_token(tk, true);
        tk = pp_read_constant_infix_expr(0, tk, val, evaluate);
        tk = pp_lex_expect_token(tk, T_close_bracket, true);
        return tk;
    }

    if (pp_lex_peek_token(tk, T_identifier, true)) {
        token_t *before_identifier = tk;

        tk = pp_lex_next_token(tk, true);

        /* offsetof is an integer constant as well. The parser's reader owns its
         * type and member syntax, so hand it the operand as for sizeof.
         */
        if (pp_integer_constant_scope &&
            !strcmp(tk->literal, "__builtin_offsetof")) {
            cur_token = before_identifier;
            val->lo = read_const_expr_operand(pp_integer_constant_scope);
            val->hi = 0;
            val->is_unsigned = false;
            val->enum_width = 32;
            return cur_token;
        }
        if (pp_integer_constant_scope) {
            constant_t *constant =
                find_scoped_constant(tk->literal, pp_integer_constant_scope);

            if (!constant)
                error_at("Identifier is not an integer constant",
                         &tk->location);
            val->lo = constant->value;
            val->hi = constant->value < 0 ? ~0U : 0;
            val->is_unsigned = false;
            val->enum_width = 32;
        } else if (!strcmp("defined", tk->literal)) {
            bool parenthesized = pp_lex_peek_token(tk, T_open_bracket, true);

            if (parenthesized)
                tk = pp_lex_next_token(tk, true);
            tk = pp_lex_expect_token(tk, T_identifier, true);
            pp_set_boolean(val, is_macro_defined(tk->literal));
            if (parenthesized)
                tk = pp_lex_expect_token(tk, T_close_bracket, true);
        } else {
            /* Any identifier in #if falls back to zero. */
            macro_t *macro = MACROS ? hashmap_get(MACROS, tk->literal) : NULL;

            /* Disallow function-like macro to be expanded */
            if (macro && macro->is_function_like) {
                if (pp_lex_peek_token(tk, T_open_bracket, true)) {
                    tk = pp_expand_function_macro_in_constant_expr(
                        before_identifier);
                    return pp_read_constant_expr_operand(tk, val, evaluate);
                }
            } else if (macro) {
                token_t *expanded_tk, *tmp;
                preprocess_ctx_t ctx;
                ctx.expanded_from = tk;
                ctx.hide_set = NULL;
                ctx.arg_hide_set = NULL;
                ctx.macro_args = NULL;
                ctx.trim_eof = false;
                expanded_tk = pp_preprocess_internal(macro->replacement, &ctx);
                if (expanded_tk) {
                    tmp = tk->next;
                    tk->next = expanded_tk;
                    ctx.end_of_token->next = tmp;
                }
                return pp_read_constant_expr_operand(tk, val, evaluate);
            }

            pp_set_boolean(val, false);
        }

        return tk;
    }

    /* Unable to identify next token, so we advance to next non-whitespace token
     * and report its location with error message.
     */
    tk = pp_lex_next_token(tk, true);
    error_at("Unexpected token while evaluating constant", &tk->location);
    return tk;
}

token_t *pp_read_constant_infix_expr(int precedence,
                                     token_t *tk,
                                     pp_integer_t *val,
                                     bool evaluate)
{
    pp_integer_t lhs = {0}, rhs = {0};
    int comparison;

    /* Evaluate unary expression first */
    opcode_t op;
    tk = pp_get_operator(tk, &op, true);
    int current_precedence = pp_get_unary_operator_prio(op);
    if (current_precedence != 0 && current_precedence >= precedence) {
        tk =
            pp_read_constant_infix_expr(current_precedence, tk, &lhs, evaluate);

        if (evaluate) {
            switch (op) {
            case OP_add:
                break;
            case OP_sub:
                pp_negate(&lhs);
                break;
            case OP_bit_not:
                lhs.lo = ~lhs.lo;
                lhs.hi = ~lhs.hi;
                break;
            case OP_log_not:
                pp_set_boolean(&lhs, !pp_is_true(&lhs));
                break;
            default: {
                source_location_t *loc =
                    tk->next ? &tk->next->location : &tk->location;

                error_at("Unexpected unary token while evaluating constant",
                         loc);
            }
            }
        }
    } else {
        tk = pp_read_constant_expr_operand(tk, &lhs, evaluate);
    }

    while (true) {
        tk = pp_get_operator(tk, &op, false);
        current_precedence = pp_get_operator_prio(op);

        if (current_precedence == 0 || current_precedence <= precedence)
            break;

        tk = pp_lex_next_token(tk, true);

        if (op == OP_ternary) {
            pp_integer_t if_true = {0}, if_false = {0};
            bool condition = pp_is_true(&lhs);

            tk = pp_read_constant_infix_expr(0, tk, &if_true,
                                             evaluate && condition);
            tk = pp_lex_expect_token(tk, T_colon, true);
            /* Conditional expressions are right-associative. */
            tk = pp_read_constant_infix_expr(current_precedence - 1, tk,
                                             &if_false, evaluate && !condition);
            if (pp_integer_constant_scope)
                pp_enum_usual_arithmetic(&if_true, &if_false);
            bool result_is_unsigned = PP_USES_UNSIGNED(&if_true, &if_false);
            if (evaluate) {
                pp_integer_t *selected = condition ? &if_true : &if_false;

                lhs.lo = selected->lo;
                lhs.hi = selected->hi;
                lhs.enum_width = selected->enum_width;
            } else
                lhs.lo = lhs.hi = 0;
            lhs.is_unsigned = result_is_unsigned;
            continue;
        }

        bool rhs_evaluate = evaluate;
        if (op == OP_log_and && !pp_is_true(&lhs))
            rhs_evaluate = false;
        if (op == OP_log_or && pp_is_true(&lhs))
            rhs_evaluate = false;
        tk = pp_read_constant_infix_expr(current_precedence, tk, &rhs,
                                         rhs_evaluate);

        if (pp_integer_constant_scope && op != OP_lshift && op != OP_rshift)
            pp_enum_usual_arithmetic(&lhs, &rhs);

        switch (op) {
        case OP_add:
        case OP_sub:
        case OP_mul:
        case OP_div:
        case OP_mod:
        case OP_bit_and:
        case OP_bit_or:
        case OP_bit_xor:
            lhs.is_unsigned = PP_USES_UNSIGNED(&lhs, &rhs);
            break;
        default:
            break;
        }

        if (evaluate) {
            switch (op) {
            case OP_add:
                pp_add(&lhs, &rhs);
                lhs.is_unsigned = PP_USES_UNSIGNED(&lhs, &rhs);
                break;
            case OP_sub:
                pp_subtract(&lhs, &rhs);
                lhs.is_unsigned = PP_USES_UNSIGNED(&lhs, &rhs);
                break;
            case OP_mul:
                pp_multiply(&lhs, &rhs);
                break;
            case OP_div:
                if (!pp_is_true(&rhs))
                    error_at("Division by zero in #if expression",
                             &tk->location);
                pp_divmod(&lhs, &rhs, false);
                break;
            case OP_mod:
                if (!pp_is_true(&rhs))
                    error_at("Modulo by zero in #if expression", &tk->location);
                pp_divmod(&lhs, &rhs, true);
                break;
            case OP_bit_and:
                lhs.lo &= rhs.lo;
                lhs.hi &= rhs.hi;
                lhs.is_unsigned = PP_USES_UNSIGNED(&lhs, &rhs);
                break;
            case OP_bit_or:
                lhs.lo |= rhs.lo;
                lhs.hi |= rhs.hi;
                lhs.is_unsigned = PP_USES_UNSIGNED(&lhs, &rhs);
                break;
            case OP_bit_xor:
                lhs.lo ^= rhs.lo;
                lhs.hi ^= rhs.hi;
                lhs.is_unsigned = PP_USES_UNSIGNED(&lhs, &rhs);
                break;
            case OP_lshift:
                if (rhs.hi ||
                    rhs.lo >= (unsigned int) (pp_integer_constant_scope
                                                  ? lhs.enum_width
                                                  : 64))
                    error_at("Shift count out of range in #if expression",
                             &tk->location);
                for (unsigned int i = 0; i < rhs.lo; i++) {
                    if (pp_integer_constant_scope && !lhs.is_unsigned &&
                        (lhs.enum_width == 32 ? lhs.lo & 0x40000000U
                                              : lhs.hi & 0x40000000U))
                        error_at("Enumerator value exceeds int range",
                                 &tk->location);
                    pp_shift_left_one(&lhs);
                }
                break;
            case OP_rshift:
                if (rhs.hi ||
                    rhs.lo >= (unsigned int) (pp_integer_constant_scope
                                                  ? lhs.enum_width
                                                  : 64))
                    error_at("Shift count out of range in #if expression",
                             &tk->location);
                for (unsigned int i = 0; i < rhs.lo; i++)
                    pp_shift_right_one(&lhs, !lhs.is_unsigned);
                break;
            case OP_gt:
                if (!lhs.is_unsigned && !rhs.is_unsigned &&
                    (lhs.hi >> 31) != (rhs.hi >> 31))
                    pp_set_boolean(&lhs, !(lhs.hi >> 31));
                else {
                    comparison = pp_compare_unsigned(&lhs, &rhs);
                    pp_set_boolean(&lhs, comparison > 0);
                }
                break;
            case OP_geq:
                if (!lhs.is_unsigned && !rhs.is_unsigned &&
                    (lhs.hi >> 31) != (rhs.hi >> 31))
                    pp_set_boolean(&lhs, !(lhs.hi >> 31));
                else {
                    comparison = pp_compare_unsigned(&lhs, &rhs);
                    pp_set_boolean(&lhs, comparison >= 0);
                }
                break;
            case OP_lt:
                if (!lhs.is_unsigned && !rhs.is_unsigned &&
                    (lhs.hi >> 31) != (rhs.hi >> 31))
                    pp_set_boolean(&lhs, lhs.hi >> 31);
                else {
                    comparison = pp_compare_unsigned(&lhs, &rhs);
                    pp_set_boolean(&lhs, comparison < 0);
                }
                break;
            case OP_leq:
                if (!lhs.is_unsigned && !rhs.is_unsigned &&
                    (lhs.hi >> 31) != (rhs.hi >> 31))
                    pp_set_boolean(&lhs, lhs.hi >> 31);
                else {
                    comparison = pp_compare_unsigned(&lhs, &rhs);
                    pp_set_boolean(&lhs, comparison <= 0);
                }
                break;
            case OP_eq:
                pp_set_boolean(&lhs, lhs.lo == rhs.lo && lhs.hi == rhs.hi);
                break;
            case OP_neq:
                pp_set_boolean(&lhs, lhs.lo != rhs.lo || lhs.hi != rhs.hi);
                break;
            case OP_log_and:
                pp_set_boolean(&lhs, pp_is_true(&lhs) && pp_is_true(&rhs));
                break;
            case OP_log_or:
                pp_set_boolean(&lhs, pp_is_true(&lhs) || pp_is_true(&rhs));
                break;
            default:
                error_at("Unexpected infix token while evaluating constant",
                         &tk->location);
            }
            if (pp_integer_constant_scope)
                pp_enum_normalize(&lhs);
        }
    }

    if (!evaluate)
        lhs.lo = lhs.hi = 0;
    val->lo = lhs.lo;
    val->hi = lhs.hi;
    val->is_unsigned = lhs.is_unsigned;
    val->enum_width = lhs.enum_width;
    return tk;
}

token_t *pp_read_constant_expr(token_t *tk, pp_integer_t *val)
{
    tk = pp_read_constant_infix_expr(0, tk, val, true);
    /* advance to fully consume constant expression */
    tk = pp_lex_next_token(tk, true);
    return tk;
}

/* Skip a group whose condition failed, returning the #elif, #else or #endif
 * that ends it. A conditional nested inside the group is skipped whole: its own
 * #elif and #else belong to it, and only its #endif brings the count back.
 */
token_t *pp_skip_cond_incl(token_t *tk)
{
    int depth = 0;

    for (; tk->kind != T_eof; tk = tk->next) {
        token_kind_t kind = tk->kind;

        if (kind == T_cppd_if || kind == T_cppd_ifdef ||
            kind == T_cppd_ifndef) {
            depth++;
            continue;
        }

        if (kind == T_cppd_endif) {
            if (!depth)
                break;
            depth--;
            continue;
        }

        if ((kind == T_cppd_elif || kind == T_cppd_else) && !depth)
            break;
    }
    return tk;
}

/* Spell an argument's tokens as a string literal, for '#'.
 *
 * T_string literals are stored with their escapes intact and unescaped later by
 * the parser, so a quote or a backslash coming from the argument has to be
 * escaped again here. Tokens are separated by a single space, with none at
 * either end.
 */
token_t *pp_stringify(token_t *arg, source_location_t *loc)
{
    char text[MAX_TOKEN_LEN], scratch[MAX_TOKEN_LEN];
    int n = 0;
    bool pending_space = false;

    for (token_t *tk = arg; tk; tk = tk->next) {
        if (pp_is_layout(tk)) {
            pending_space = n > 0;
            continue;
        }

        char *spelling = token_to_string(tk, scratch);
        if (!spelling)
            continue;

        if (pending_space) {
            if (n >= MAX_TOKEN_LEN - 1)
                error_at("Stringified argument too long", loc);
            text[n++] = ' ';
            pending_space = false;
        }

        for (int i = 0; spelling[i]; i++) {
            if (spelling[i] == '"' || spelling[i] == '\\') {
                if (n >= MAX_TOKEN_LEN - 1)
                    error_at("Stringified argument too long", loc);
                text[n++] = '\\';
            }
            if (n >= MAX_TOKEN_LEN - 1)
                error_at("Stringified argument too long", loc);
            text[n++] = spelling[i];
        }
    }
    text[n] = 0;

    token_t *out = new_token(T_string, loc, n);
    out->literal = arena_strdup(TOKEN_ARENA, text);
    return out;
}

/* C99's #error directive displays the rest of its directive line as a
 * diagnostic message. These are raw preprocessing tokens, not a macro
 * replacement list, so spell them directly and retain their source order.
 */
__noreturn void pp_error_directive(token_t *directive)
{
    char message[MAX_LINE_LEN], scratch[MAX_TOKEN_LEN];
    int len = 0;
    bool needs_space = false;
    source_location_t *loc = &directive->location;
    token_t *tk = directive;

    while (tk->next && tk->next->kind != T_newline && tk->next->kind != T_eof) {
        tk = pp_lex_next_token(tk, false);
        if (pp_is_layout(tk)) {
            needs_space = len > 0;
            continue;
        }

        char *spelling = token_to_string(tk, scratch);
        if (!spelling)
            continue;
        if (needs_space && len < MAX_LINE_LEN - 1)
            message[len++] = ' ';
        needs_space = true;
        loc = &tk->location;
        for (int i = 0; spelling[i] && len < MAX_LINE_LEN - 1; i++)
            message[len++] = spelling[i];
    }

    if (!len)
        strcpy(message, "#error");
    else
        message[len] = '\0';
    error_at(message, loc);
}

/* C99's _Pragma operator is processed after macro replacement. The compiler has
 * no standard pragma semantics, so its destringized directive is ignored just
 * like an unknown #pragma, except for "once", which #pragma honours as well;
 * consume the operator syntax so no tokens reach the parser. @owner is the
 * token in the source file, which for an operator that came out of a macro is
 * the invocation rather than the macro's definition.
 */
token_t *pp_pragma_operator(token_t *tk, token_t *owner)
{
    tk = pp_lex_expect_token(tk, T_open_bracket, true);
    tk = pp_lex_expect_token(tk, T_string, true);

    /* Destringizing only removes a backslash before '"' or '\\', neither of
     * which can spell "once", so the literal as stored is compared directly.
     */
    const char *p = tk->literal;
    int len;

    while (*p == ' ' || *p == '\t')
        p++;
    len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
        len--;
    if (len == 4 && !strncmp(p, "once", 4))
        hashmap_put(PRAGMA_ONCE, owner->location.physical_filename, NULL);

    return pp_lex_expect_token(tk, T_close_bracket, true);
}

/* Join two tokens into one, as '##' requires.
 *
 * Pasting is textual, so the result has to be scanned again: "a" and "1" give
 * the single identifier a1, not two tokens that happen to sit next to each
 * other. A join that does not scan as exactly one token is not a token at all,
 * and is reported rather than quietly splitting back in two.
 */
token_t *pp_paste_tokens(token_t *lhs, token_t *rhs, source_location_t *loc)
{
    char lbuf[MAX_TOKEN_LEN], rbuf[MAX_TOKEN_LEN], joined[MAX_TOKEN_LEN];
    const char *l = token_to_string(lhs, lbuf);
    const char *r = token_to_string(rhs, rbuf);

    if (!l || !r)
        error_at("Operand of '##' cannot be pasted", loc);
    if (strlen(l) + strlen(r) >= MAX_TOKEN_LEN)
        error_at("Pasted token too long", loc);

    strcpy(joined, l);
    strcat(joined, r);

    /* Hand the text to the lexer the way a source file is handed to it:
     * capacity marks the end of the input and size is the read cursor.
     */
    int len = strlen(joined);
    strbuf_t *buf = strbuf_create(len + 1);
    strbuf_puts(buf, joined);
    buf->elements[len] = 0;
    buf->capacity = len + 1;
    buf->size = 0;

    source_location_t scan_loc;
    memcpy(&scan_loc, loc, sizeof(source_location_t));
    /* A pasted '#' is never a directive, whatever column it came from. */
    lex_at_line_start = false;
    token_t *pasted = lex_token(buf, &scan_loc);
    bool whole = buf->size == len;
    strbuf_free(buf);

    if (!whole)
        error_at("'##' does not produce a single token", loc);

    pasted->next = NULL;
    memcpy(&pasted->location, loc, sizeof(source_location_t));
    pasted->location.len = len;
    return pasted;
}

/* Apply '#' and '##' to a macro's replacement list, before it is rescanned.
 *
 * Both operate on an argument as it was written rather than on its expansion,
 * so they cannot wait for the expansion loop: by the time that loop reaches a
 * parameter it has already expanded it. @args is NULL for an object-like macro,
 * which has no parameters to stringify but may still paste.
 *
 * An argument with no tokens in it is not "no argument": '#' spells it as the
 * empty string, and pasting against it leaves the other operand standing on its
 * own. Membership in @args, rather than a non-empty value, is what makes a name
 * a parameter.
 */
token_t *pp_subst_hash(token_t *rep, hashmap_t *args)
{
    token_t head;
    token_t *tail = &head, *tail_prev = NULL;

    /* Whether anything at all precedes a '##' here, and whether that something
     * was an argument that turned out to be empty.
     */
    bool lhs_present = false, lhs_empty = false;

    head.next = NULL;

    for (token_t *tk = rep; tk; tk = tk->next) {
        if (tk->kind == T_hash && args) {
            token_t *operand = pp_next_significant(tk);

            if (!operand || operand->kind != T_identifier ||
                !hashmap_contains(args, operand->literal))
                error_at("'#' must be followed by a macro parameter",
                         &tk->location);

            tail_prev = tail;
            tail->next = pp_stringify(hashmap_get(args, operand->literal),
                                      &tk->location);
            tail = tail->next;
            lhs_present = true;
            lhs_empty = false;
            tk = operand;
            continue;
        }

        if (tk->kind == T_hashhash) {
            token_t *operand = pp_next_significant(tk);

            if (!lhs_present || !operand)
                error_at("'##' needs a token on each side", &tk->location);

            /* GNU comma elision: in ", ## __VA_ARGS__" nothing is pasted. The
             * variadic argument follows the comma as written, and when it has
             * no tokens the comma is deleted instead.
             */
            if (args && tail->kind == T_comma &&
                operand->kind == T_identifier &&
                !strcmp(operand->literal, "__VA_ARGS__") &&
                hashmap_contains(args, operand->literal)) {
                token_t *comma_prev = tail_prev;
                bool any = false;

                for (token_t *t = hashmap_get(args, operand->literal); t;
                     t = t->next) {
                    if (pp_is_layout(t))
                        continue;
                    tail_prev = tail;
                    tail->next = copy_token(t);
                    tail = tail->next;
                    any = true;
                }
                if (!any) {
                    tail = comma_prev;
                    tail->next = NULL;
                    lhs_present = tail != &head;
                }
                lhs_empty = false;
                tk = operand;
                continue;
            }

            /* The right operand joins as written; a parameter contributes its
             * argument rather than its expansion.
             */
            token_t *rhs = operand;
            bool rhs_is_arg = false;
            if (args && operand->kind == T_identifier &&
                hashmap_contains(args, operand->literal)) {
                rhs = hashmap_get(args, operand->literal);
                rhs_is_arg = true;
            }

            /* An argument arrives with the spacing it was written with. The
             * join is between real tokens, so that spacing is not an operand.
             */
            while (rhs && pp_is_layout(rhs))
                rhs = rhs->next;

            if (rhs) {
                if (lhs_empty) {
                    /* Nothing to join to, so the right operand stands alone. */
                    tail_prev = tail;
                    tail->next = copy_token(rhs);
                    tail = tail->next;
                } else {
                    tail_prev->next = pp_paste_tokens(tail, rhs, &tk->location);
                    tail = tail_prev->next;
                }

                /* Only the first token of a multi-token argument is joined; the
                 * rest follow it. An argument is a list of its own, so it ends
                 * where the argument does. A literal operand is not: its
                 * successor is the next token of the replacement list, which
                 * the loop below still has to walk, so copying from here would
                 * emit the remainder of the macro body twice.
                 */
                if (rhs_is_arg) {
                    for (token_t *rest = rhs->next; rest; rest = rest->next) {
                        if (pp_is_layout(rest))
                            continue;
                        tail_prev = tail;
                        tail->next = copy_token(rest);
                        tail = tail->next;
                    }
                }
                lhs_empty = false;
            }
            tk = operand;
            continue;
        }

        /* A parameter the next '##' will join is substituted here, unexpanded
         * -- letting the expansion loop reach it would expand it first.
         */
        const token_t *after = pp_next_significant(tk);

        if (args && tk->kind == T_identifier && after &&
            after->kind == T_hashhash && hashmap_contains(args, tk->literal)) {
            token_t *arg = hashmap_get(args, tk->literal);
            bool any = false;

            for (token_t *t = arg; t; t = t->next) {
                if (pp_is_layout(t))
                    continue;
                tail_prev = tail;
                tail->next = copy_token(t);
                tail = tail->next;
                any = true;
            }
            lhs_present = true;
            lhs_empty = !any;
            continue;
        }

        /* Layout next to a '##' is not an operand either: the join is between
         * the tokens on each side of it.
         */
        if (pp_is_layout(tk) && after && after->kind == T_hashhash)
            continue;

        tail_prev = tail;
        tail->next = copy_token(tk);
        tail = tail->next;
        lhs_present = true;
        lhs_empty = false;
    }

    return head.next;
}

token_t *pp_preprocess_internal(token_t *tk, preprocess_ctx_t *ctx)
{
    token_t head;
    token_t *cur = &head;
    cond_incl_t *ci = NULL;

    /* A macro whose replacement list is empty -- "#define NDEBUG", or a
     * function-like macro that expands to nothing -- produces no tokens at all,
     * and both of the values returned below have to say so. Without the
     * initializer the result is whatever the stack held, and end_of_token below
     * would name this frame, which the caller splices onto after it has died.
     */
    head.next = NULL;

    while (tk) {
        macro_t *macro = NULL;

        switch (tk->kind) {
        case T_identifier: {
            token_t *macro_tk = tk;
            preprocess_ctx_t expansion_ctx;

            /* Initialize expansion context: inherit parent context and enable
             * EOF trimming for macro body expansion
             */
            expansion_ctx.expanded_from =
                ctx->expanded_from ? ctx->expanded_from : tk;
            expansion_ctx.macro_args = ctx->macro_args;
            expansion_ctx.arg_hide_set = ctx->arg_hide_set;
            expansion_ctx.trim_eof = true;

            token_t *macro_arg_replacement = NULL;

            /* Check if this identifier is a macro parameter (argument) If we're
             * currently expanding a macro body, parameters should be replaced
             * with their supplied arguments.
             *
             * Membership decides this, not a non-empty value: an argument with
             * no tokens in it still names a parameter, and substituting nothing
             * for it is what "M(a,)" means. Testing the value would leave the
             * parameter's own name standing in the output.
             *
             * '#' and '##' were already resolved by pp_subst_hash(), which had
             * to run before this expansion could reach their operands.
             */
            bool is_macro_param =
                ctx->macro_args &&
                hashmap_contains(ctx->macro_args, tk->literal);

            if (is_macro_param) {
                macro_arg_replacement =
                    hashmap_get(ctx->macro_args, tk->literal);
                if (macro_arg_replacement) {
                    /* Recursively expand the argument to handle nested macros
                     */
                    expansion_ctx.hide_set = ctx->arg_hide_set;
                    expansion_ctx.macro_args =
                        NULL; /* Don't take account of macro arguments, this
                                 might run into infinite loop
                                 */
                    macro_arg_replacement = pp_preprocess_internal(
                        macro_arg_replacement, &expansion_ctx);
                    if (macro_arg_replacement) {
                        cur->next = macro_arg_replacement;
                        cur = expansion_ctx.end_of_token;
                    }
                }
                tk = pp_lex_next_token(tk, false);
                continue;
            }

            if (!strcmp(tk->literal, "_Pragma")) {
                tk = pp_pragma_operator(tk, expansion_ctx.expanded_from);
                tk = pp_lex_next_token(tk, false);
                continue;
            }

            /* Prevent infinite recursion by checking hide set */
            if (hide_set_contains(ctx->hide_set, tk->literal))
                break;

            macro = hashmap_get(MACROS, tk->literal);

            /* Skips expansion if either macro doesn't exist or macro is diabled
             */
            if (!macro || macro->is_disabled)
                break;

            /* Handle built-in function-like macros (__FILE__, __LINE__) These
             * have special handlers that generate tokens directly
             */
            if (macro->handler) {
                cur->next = macro->handler(expansion_ctx.expanded_from);
                cur = cur->next;
                tk = pp_lex_next_token(tk, false);
                continue;
            }

            /* Check if this is a function-like macro invocation */
            if (macro->is_function_like &&
                pp_lex_peek_token(tk, T_open_bracket, true)) {
                token_t arg_head;
                token_t *arg_cur = &arg_head;
                int arg_idx = 0;
                int bracket_depth = 0;

                /* An argument with no tokens in it -- "M()" -- never assigns
                 * this, and the empty list has to read as empty rather than as
                 * whatever the stack happened to hold.
                 */
                arg_head.next = NULL;

                /* Add macro name to hide set to prevent re-expansion of itself
                 * during its own body expansion
                 */
                expansion_ctx.hide_set =
                    hide_set_union(ctx->hide_set, new_hide_set(tk->literal));
                expansion_ctx.arg_hide_set = ctx->hide_set;
                /* Create parameter mapping table for this macro invocation */
                expansion_ctx.macro_args = hashmap_create(8);

                tk = pp_lex_next_token(tk, true);

                /* Parse macro arguments until closing parenthesis
                 *
                 * Handles nested parentheses and comma-separated argument list
                 * by tracking the nested depth
                 */
                while (true) {
                    if (pp_lex_peek_token(tk, T_open_bracket, false))
                        bracket_depth++;
                    else if (pp_lex_peek_token(tk, T_close_bracket, false))
                        bracket_depth--;

                    /* Expand identifiers within macro arguments
                     *
                     * This handles cases like: MACRO(OTHER_MACRO) where
                     * OTHER_MACRO is itself expanded before being used as arg
                     */
                    if (expansion_ctx.macro_args &&
                        pp_lex_peek_token(tk, T_identifier, false)) {
                        token_t *arg_tk =
                            hashmap_get(ctx->macro_args, tk->next->literal);

                        if (arg_tk) {
                            preprocess_ctx_t arg_expansion_ctx;
                            arg_expansion_ctx.expanded_from = tk->next;
                            arg_expansion_ctx.hide_set = ctx->arg_hide_set;
                            arg_expansion_ctx.arg_hide_set = NULL;
                            arg_expansion_ctx.macro_args = NULL;
                            arg_tk = pp_preprocess_internal(arg_tk,
                                                            &arg_expansion_ctx);
                            tk = pp_lex_next_token(tk, false);
                            if (arg_tk) {
                                arg_cur->next = arg_tk;
                                arg_cur = arg_expansion_ctx.end_of_token;
                            }
                            continue;
                        }
                    }

                    /* Accumulate argument tokens until a delimiter.
                     *
                     * Only the outermost level delimits: a comma nested inside
                     * parentheses belongs to the argument, as in "M(f(a, b))",
                     * and so does the bracket that closes them. The argument
                     * list itself ends at the bracket that takes the depth
                     * below where it started.
                     */
                    bool delimits = bracket_depth < 0 ||
                                    (!bracket_depth &&
                                     pp_lex_peek_token(tk, T_comma, false));

                    if (!delimits) {
                        tk = pp_lex_next_token(tk, false);
                        arg_cur->next = copy_token(tk);
                        arg_cur = arg_cur->next;
                        continue;
                    }

                    token_t *param_tk;
                    bool empty_zero_arg_call = bracket_depth < 0 &&
                                               arg_idx == 0 && !arg_head.next &&
                                               macro->param_num == 0;

                    /* M() has no arguments when M declares none. A macro with
                     * one parameter, in contrast, receives one empty argument
                     * and must retain that distinction.
                     */
                    if (empty_zero_arg_call) {
                        if (macro->is_variadic)
                            hashmap_put(expansion_ctx.macro_args,
                                        macro->variadic_tk->literal, NULL);
                        /* Bind argument to corresponding parameter */
                    } else if (arg_idx < macro->param_num) {
                        param_tk = macro->param_names[arg_idx++];
                        hashmap_put(expansion_ctx.macro_args, param_tk->literal,
                                    arg_head.next);
                    } else {
                        /* Handle variadic macro overflow
                         *
                         * If macro takes __VA_ARGS__, excess arguments go there
                         */
                        if (macro->is_variadic) {
                            param_tk = macro->variadic_tk;

                            if (hashmap_contains(expansion_ctx.macro_args,
                                                 param_tk->literal)) {
                                /* Append to existing variadic args with comma
                                 * separator to preserve argument boundaries
                                 */
                                token_t *prev =
                                    hashmap_get(expansion_ctx.macro_args,
                                                param_tk->literal);

                                while (prev->next)
                                    prev = prev->next;

                                /* Borrows parameter's token location */
                                prev->next =
                                    new_token(T_comma, &param_tk->location, 1);
                                prev->next->next = arg_head.next;
                                prev = arg_cur;
                            } else {
                                hashmap_put(expansion_ctx.macro_args,
                                            param_tk->literal, arg_head.next);
                            }
                        } else {
                            error_at(
                                "Too many arguments supplied to macro "
                                "invocation",
                                &macro_tk->location);
                        }
                    }

                    /* Reset for next argument collection */
                    arg_cur = &arg_head;
                    arg_head.next = NULL;

                    if (pp_lex_peek_token(tk, T_comma, false)) {
                        tk = pp_lex_next_token(tk, false);
                        continue;
                    }

                    if (pp_lex_peek_token(tk, T_close_bracket, false)) {
                        tk = pp_lex_next_token(tk, false);
                        break;
                    }
                }

                if (arg_idx < macro->param_num)
                    error_at("Too few arguments supplied to macro invocation",
                             &macro_tk->location);

                /* F(1) for F(a, ...) supplies nothing past the named
                 * parameters. C99 wants at least one more argument, but gcc
                 * accepts the call, and __VA_ARGS__ has to be bound to the
                 * empty list for it rather than be left an ordinary name.
                 */
                if (macro->is_variadic &&
                    !hashmap_contains(expansion_ctx.macro_args,
                                      macro->variadic_tk->literal))
                    hashmap_put(expansion_ctx.macro_args,
                                macro->variadic_tk->literal, NULL);

                /* Expand macro body with collected arguments Replace parameter
                 * references with supplied argument tokens
                 */
                token_t *expanded = pp_preprocess_internal(
                    pp_subst_hash(macro->replacement, expansion_ctx.macro_args),
                    &expansion_ctx);
                if (expanded) {
                    cur->next = expanded;
                    cur = expansion_ctx.end_of_token;
                }

                hashmap_free(expansion_ctx.macro_args);
            } else {
                /* Handle object-like macro expansion (no parameters) Simply
                 * expand the replacement with current hide set plus this macro
                 * name added to prevent re-expansion
                 */
                expansion_ctx.hide_set =
                    hide_set_union(ctx->hide_set, new_hide_set(tk->literal));
                token_t *expanded = pp_preprocess_internal(
                    pp_subst_hash(macro->replacement, NULL), &expansion_ctx);
                if (expanded) {
                    cur->next = expanded;
                    cur = expansion_ctx.end_of_token;
                }
            }

            tk = pp_lex_next_token(tk, false);
            continue;
        }
        case T_hash:
        case T_hashhash:
            /* Every '#' a macro body owns is resolved by pp_subst_hash() before
             * that body is rescanned, so one arriving here is loose in ordinary
             * code.
             */
            error_at("'#' is only meaningful inside a macro definition",
                     &tk->location);
            break;
        case T_cppd_include: {
            char inclusion_path[MAX_LINE_LEN];
            token_stream_t *file_tks = NULL;
            token_t *include_tk = tk;
            bool angle_header = false;
            bool angle_form = false;
            preprocess_ctx_t inclusion_ctx;
            inclusion_ctx.hide_set = ctx->hide_set;
            inclusion_ctx.arg_hide_set = NULL;
            inclusion_ctx.expanded_from = NULL;
            inclusion_ctx.macro_args = NULL;
            inclusion_ctx.trim_eof = true;

            if (pp_lex_peek_token(tk, T_string, true)) {
                tk = pp_lex_next_token(tk, true);
                strcpy(inclusion_path, tk->literal);

                /* A header name may be supplied by an object-like macro. The
                 * replacement is rescanned here rather than treating the macro
                 * name as an angle include, and aliases may name another alias
                 * before finally producing the required string token.
                 */
            } else if (pp_lex_peek_token(tk, T_identifier, true)) {
                macro_t *aliases[MAX_TOKEN_LEN];
                int alias_count = 0;

                tk = pp_lex_next_token(tk, true);
                macro = hashmap_get(MACROS, tk->literal);
                while (macro && !macro->is_disabled &&
                       !macro->is_function_like && macro->replacement &&
                       !macro->replacement->next &&
                       macro->replacement->kind == T_identifier) {
                    for (int i = 0; i < alias_count; i++)
                        if (aliases[i] == macro)
                            error_at("cyclic macro expansion in #include",
                                     &tk->location);
                    if (alias_count == MAX_TOKEN_LEN)
                        error_at("#include macro alias chain is too deep",
                                 &tk->location);
                    aliases[alias_count++] = macro;
                    macro = hashmap_get(MACROS, macro->replacement->literal);
                }
                if (!macro || macro->is_disabled || macro->is_function_like ||
                    !macro->replacement)
                    error_at("#include macro must expand to a header name",
                             &tk->location);
                if (macro->replacement->kind == T_string &&
                    !macro->replacement->next) {
                    strcpy(inclusion_path, macro->replacement->literal);
                } else if (macro->replacement->kind == T_lt) {
                    token_t *open = macro->replacement;
                    token_t *close = open;

                    angle_form = true;
                    while (close && close->kind != T_gt)
                        close = close->next;
                    if (!close || close->next)
                        error_at("#include macro must expand to a header name",
                                 &tk->location);
                    inclusion_path[0] = '\0';
                    angle_header = resolve_angle_include(
                        open, close, inclusion_path, sizeof(inclusion_path));
                } else {
                    error_at("#include macro must expand to a header name",
                             &tk->location);
                }
            } else {
                tk = pp_lex_expect_token(tk, T_lt, true);
                token_t *open = tk;
                angle_form = true;
                inclusion_path[0] = '\0';
                while (!pp_lex_peek_token(tk, T_gt, false)) {
                    if (pp_lex_peek_token(tk, T_newline, false) ||
                        pp_lex_peek_token(tk, T_eof, false))
                        error_at("Unterminated #include <...>", &tk->location);
                    tk = pp_lex_next_token(tk, false);
                }

                token_t *close = pp_lex_next_token(tk, false);
                angle_header = resolve_angle_include(
                    open, close, inclusion_path, sizeof(inclusion_path));
                tk = close;
            }

            if (angle_form && !angle_header) {
                tk = pp_lex_expect_token(tk, T_newline, true);
                tk = pp_lex_next_token(tk, false);
                continue;
            }

            if (!angle_header) {
                /* Quoted headers are relative to the physical source path. */
                char path[MAX_LINE_LEN];
                const char *file = include_tk->location.physical_filename;
                int c = strlen(file) - 1;

                while (c > 0 && file[c] != '/')
                    c--;

                if (c) {
                    if (c >= MAX_LINE_LEN - 1)
                        c = MAX_LINE_LEN - 2;
                    memcpy(path, file, c);
                    path[c] = '\0';
                } else {
                    path[0] = '.';
                    path[1] = '\0';
                    c = 1;
                }

                snprintf(path + c, MAX_LINE_LEN - c, "/%s", inclusion_path);
                strncpy(inclusion_path, path, MAX_LINE_LEN - 1);
                inclusion_path[MAX_LINE_LEN - 1] = '\0';
            }

            tk = pp_lex_expect_token(tk, T_newline, true);
            tk = pp_lex_next_token(tk, false);

            file_tks = gen_file_token_stream(intern_string(inclusion_path));

            /* gen_file_token_stream() canonicalizes lexical components such as
             * "./" and "../". PRAGMA_ONCE is keyed by that same physical
             * filename, so test the canonical spelling rather than the raw
             * include directive.
             */
            if (hashmap_contains(PRAGMA_ONCE,
                                 file_tks->head->location.physical_filename))
                continue;

            token_t *included =
                pp_preprocess_internal(file_tks->head, &inclusion_ctx);
            if (included) {
                cur->next = included;
                cur = inclusion_ctx.end_of_token;
            }
            continue;
        }
        case T_cppd_define: {
            token_t *r_head = NULL, *r_tail = NULL, *r_cur;
            token_t *r_last = NULL; /* last token that is not white space */

            tk = pp_lex_expect_token(tk, T_identifier, true);
            macro = hashmap_get(MACROS, tk->literal);

            if (!macro) {
                macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
                macro->name = tk->literal;
            } else {
                /* Ensures that #undef effect is overwritten */
                macro->is_disabled = false;
            }

            /* A redefinition replaces the complete macro signature, not only
             * its replacement list.
             */
            macro->is_function_like = false;
            macro->is_variadic = false;
            macro->param_num = 0;
            macro->variadic_tk = NULL;

            if (pp_lex_peek_token(tk, T_open_bracket, false)) {
                /* function-like macro */
                macro->is_function_like = true;
                tk = pp_lex_next_token(tk, false);
                while (pp_lex_peek_token(tk, T_identifier, true)) {
                    tk = pp_lex_next_token(tk, true);
                    if (macro->param_num >= MAX_PARAMS)
                        error_at("Too many macro parameters", &tk->location);
                    macro->param_names[macro->param_num++] = copy_token(tk);

                    if (pp_lex_peek_token(tk, T_comma, true)) {
                        tk = pp_lex_next_token(tk, true);
                    }
                }

                if (pp_lex_peek_token(tk, T_elipsis, true)) {
                    tk = pp_lex_next_token(tk, true);
                    macro->is_variadic = true;
                    macro->variadic_tk = copy_token(tk);
                    macro->variadic_tk->literal = intern_string("__VA_ARGS__");
                }

                tk = pp_lex_expect_token(tk, T_close_bracket, true);
            }

            tk = pp_lex_skip_space(tk);
            while (!pp_lex_peek_token(tk, T_newline, false)) {
                if (pp_lex_peek_token(tk, T_backslash, false)) {
                    tk = pp_lex_expect_token(tk, T_backslash, false);

                    if (!pp_lex_peek_token(tk, T_newline, false))
                        error_at("Backslash and newline must not be separated",
                                 &tk->location);
                    else
                        tk = pp_lex_expect_token(tk, T_newline, false);
                    continue;
                }

                tk = pp_lex_next_token(tk, false);
                r_cur = copy_token(tk);
                r_cur->next = NULL;

                if (!r_head) {
                    r_head = r_cur;
                    r_tail = r_head;
                } else {
                    r_tail->next = r_cur;
                    r_tail = r_cur;
                }
                if (r_cur->kind != T_whitespace && r_cur->kind != T_tab)
                    r_last = r_cur;
            }

            /* White space after the replacement list is not part of it (C99
             * 6.10.3p7), and a list that kept it would no longer be the single
             * string that #include H looks for.
             */
            if (r_last)
                r_last->next = NULL;
            else
                r_head = NULL;

            tk = pp_lex_expect_token(tk, T_newline, false);
            tk = pp_lex_next_token(tk, false);
            macro->replacement = r_head;
            hashmap_put(MACROS, macro->name, macro);
            continue;
        }
        case T_cppd_undef: {
            tk = pp_lex_expect_token(tk, T_identifier, true);
            macro = hashmap_get(MACROS, tk->literal);

            if (macro) {
                macro->is_disabled = true;
            }

            tk = pp_lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_if: {
            token_t *cond_tk = tk;
            pp_integer_t condition;
            tk = pp_read_constant_expr(tk, &condition);
            ci = push_cond(ci, cond_tk, pp_is_true(&condition));

            if (!pp_is_true(&condition))
                tk = pp_skip_cond_incl(tk);
            continue;
        }
        case T_cppd_ifdef: {
            token_t *kw_tk = tk;
            tk = pp_lex_expect_token(tk, T_identifier, true);
            bool defined = is_macro_defined(tk->literal);

            ci = push_cond(ci, kw_tk, defined);
            if (!defined)
                tk = pp_skip_cond_incl(tk);
            else
                tk = pp_lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_ifndef: {
            token_t *kw_tk = tk;
            tk = pp_lex_expect_token(tk, T_identifier, true);
            bool defined = is_macro_defined(tk->literal);

            ci = push_cond(ci, kw_tk, !defined);
            if (defined)
                tk = pp_skip_cond_incl(tk);
            else
                tk = pp_lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_elif: {
            if (!ci || ci->ctx == CK_else_then)
                error_at("Stray #elif", &tk->location);
            pp_integer_t included;
            ci->ctx = CK_elif_then;
            tk = pp_read_constant_expr(tk, &included);

            if (!ci->included && pp_is_true(&included))
                ci->included = true;
            else
                tk = pp_skip_cond_incl(tk);
            continue;
        }
        case T_cppd_else: {
            if (!ci || ci->ctx == CK_else_then)
                error_at("Stray #else", &tk->location);
            ci->ctx = CK_else_then;
            tk = pp_lex_expect_token(tk, T_newline, true);

            if (ci->included)
                tk = pp_skip_cond_incl(tk);
            continue;
        }
        case T_cppd_endif: {
            if (!ci)
                error_at("Stray #endif", &tk->location);
            ci = ci->prev;
            tk = pp_lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_pragma: {
            if (pp_lex_peek_token(tk, T_identifier, true)) {
                tk = pp_lex_next_token(tk, true);

                if (!strcmp("once", tk->literal))
                    hashmap_put(PRAGMA_ONCE, tk->location.physical_filename,
                                NULL);
            }

            while (!pp_lex_peek_token(tk, T_newline, true))
                tk = pp_lex_next_token(tk, true);

            tk = pp_lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_line: {
            char *original_filename = tk->location.physical_filename;
            char *logical_filename = NULL;
            int requested_line;

            pp_read_line_operands(tk, &requested_line, &logical_filename);
            while (!pp_lex_peek_token(tk, T_newline, false))
                tk = pp_lex_next_token(tk, false);
            if (!pp_lex_peek_token(tk, T_newline, true))
                error_at("Unexpected token in #line directive", &tk->location);
            tk = pp_lex_expect_token(tk, T_newline, true);
            tk = pp_lex_next_token(tk, false);
            if (tk->kind != T_eof)
                pp_apply_line_directive(tk, original_filename, logical_filename,
                                        requested_line - tk->location.line);
            continue;
        }
        case T_cppd_error: {
            pp_error_directive(tk);
        }
        case T_cppd_unknown:
            error_at("Unsupported directive", &tk->location);
            break;
        case T_backslash: {
            /* This branch is designed to be failed since backslash should be
             * consumed by #define, and upon later expansion, it should not be
             * included previously while created by #define.
             */
            error_at("Backslash is not allowed here", &cur->location);
            break;
        }
        case T_eof: {
            if (ctx->trim_eof) {
                tk = pp_lex_next_token(tk, false);
                continue;
            }
            break;
        }
        default:
            break;
        }

        cur->next = copy_token(tk);
        cur = cur->next;
        tk = pp_lex_next_token(tk, false);
    }

    if (ci)
        error_at("Unterminated conditional directive", &ci->tk->location);

    /* NULL rather than '&head' when nothing was produced: the caller must skip
     * the splice entirely, and a stale read should fault rather than corrupt
     * the token list it is building.
     */
    ctx->end_of_token = cur == &head ? NULL : cur;
    return head.next;
}

/* Report whether a string literal payload ends inside a numeric escape that a
 * following digit would extend: a hexadecimal escape, whose digit run has no
 * limit, or an octal escape of fewer than three digits.
 *
 * Returns 16 or 8 for those, and 0 when the payload ends in anything else.
 */
int pp_string_open_escape_base(const char *text)
{
    for (int i = 0; text[i]; i++) {
        if (text[i] != '\\')
            continue;
        i++;
        if (text[i] == 'x') {
            while (isxdigit((unsigned char) text[i + 1]))
                i++;
            if (!text[i + 1])
                return 16;
        } else if (text[i] >= '0' && text[i] <= '7') {
            int digits = 1;

            while (digits < 3 && text[i + 1] >= '0' && text[i + 1] <= '7') {
                i++;
                digits++;
            }
            if (!text[i + 1] && digits < 3)
                return 8;
        } else if (!text[i]) {
            break;
        }
    }
    return 0;
}

/* Diagnose a finished narrow string literal whose hexadecimal escape does not
 * fit a byte. Called only for a T_string token, once no further literal can
 * join it.
 */
void pp_check_narrow_string(token_t *tk)
{
    if (hex_escape_exceeds_byte(tk->literal))
        error_at("Hexadecimal escape sequence out of range", &tk->location);
}

/* Drop the whitespace, tab and newline tokens from a fully preprocessed stream,
 * on the way into the parser.
 *
 * They carry no meaning to the parser, which never names those kinds, and every
 * token is created before this runs, so once they are gone the parser never
 * meets one again. That is what let the skip-over-layout walk in front of each
 * token access -- more than a million iterations over a self-compile -- be
 * removed outright. Preprocessed output (-E) still needs them to separate one
 * token from the next, so the stripping belongs here and not in preprocess()
 * itself.
 */
token_t *pp_strip_layout(token_t *tk)
{
    token_t head;
    token_t *cur = &head;

    head.next = NULL;
    for (; tk; tk = tk->next) {
        if (tk->kind == T_whitespace || tk->kind == T_newline ||
            tk->kind == T_tab)
            continue;

        /* C99 translation phase 6 concatenates adjacent string literal tokens
         * after macro expansion. Do it at the parser boundary: whitespace is
         * already irrelevant there, and this covers both source-adjacent and
         * macro-produced strings without changing -E's token spelling. When
         * either literal is wide, C99 6.4.5 makes the joined literal wide.
         */
        if (cur != &head && (cur->kind == T_string || cur->kind == T_wstring) &&
            (tk->kind == T_string || tk->kind == T_wstring)) {
            char combined[MAX_STRING_LEN];
            int left_len = strlen(cur->literal);
            int right_len = strlen(tk->literal);
            int base = pp_string_open_escape_base(cur->literal);
            char first = tk->literal[0];
            int respell = 0;

            /* Each literal's escapes end with that literal, but the payloads
             * are joined as spelled and decoded later. When the left payload
             * ends in an escape the right one's first digit would extend, spell
             * that digit as a complete three-digit octal escape, which nothing
             * can extend and which decodes to the same character.
             */
            if ((base == 16 && isxdigit((unsigned char) first)) ||
                (base == 8 && first >= '0' && first <= '7'))
                respell = 3;
            if (left_len + right_len + respell >= MAX_STRING_LEN)
                error_at("Concatenated string literal too long", &tk->location);
            const char *rest = tk->literal;

            memcpy(combined, cur->literal, left_len);
            if (respell) {
                combined[left_len++] = '\\';
                combined[left_len++] = '0' + ((first >> 6) & 7);
                combined[left_len++] = '0' + ((first >> 3) & 7);
                combined[left_len++] = '0' + (first & 7);
                rest++;
            }
            strcpy(combined + left_len, rest);
            cur->literal = intern_string(combined);
            cur->location.len += tk->location.len;
            if (tk->kind == T_wstring)
                cur->kind = T_wstring;
            continue;
        }
        if (cur != &head && cur->kind == T_string)
            pp_check_narrow_string(cur);
        cur->next = tk;
        cur = tk;
    }
    if (cur != &head && cur->kind == T_string)
        pp_check_narrow_string(cur);
    cur->next = NULL;
    return head.next;
}

token_t *preprocess(token_t *tk)
{
    preprocess_ctx_t ctx;
    ctx.hide_set = NULL;
    ctx.arg_hide_set = NULL;
    ctx.expanded_from = NULL;
    ctx.macro_args = NULL;
    ctx.trim_eof = false;

    /* Initialize built-in macros */
    PRAGMA_ONCE = hashmap_create(16);
    MACROS = hashmap_create(16);

    synth_built_in_loc.pos = 0;
    synth_built_in_loc.len = 1;
    synth_built_in_loc.column = 1;
    synth_built_in_loc.line = 1;
    synth_built_in_loc.filename = "<built-in>";

    macro_t *macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    macro->name = "__FILE__";
    macro->handler = file_macro_handler;
    hashmap_put(MACROS, "__FILE__", macro);

    macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    macro->name = "__LINE__";
    macro->handler = line_macro_handler;
    hashmap_put(MACROS, "__LINE__", macro);

    macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    macro->name = "__DATE__";
    macro->handler = date_macro_handler;
    hashmap_put(MACROS, "__DATE__", macro);

    macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    macro->name = "__TIME__";
    macro->handler = time_macro_handler;
    hashmap_put(MACROS, "__TIME__", macro);

    /* C99-required implementation macros. shecc supplies its own small runtime
     * rather than a complete hosted library, so advertise freestanding mode
     * while retaining the C99 language-version identifier.
     */
    macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    macro->name = "__STDC__";
    macro->replacement = new_token(T_numeric, &synth_built_in_loc, 1);
    macro->replacement->literal = "1";
    hashmap_put(MACROS, "__STDC__", macro);

    macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    macro->name = "__STDC_VERSION__";
    macro->replacement = new_token(T_numeric, &synth_built_in_loc, 7);
    macro->replacement->literal = "199901L";
    hashmap_put(MACROS, "__STDC_VERSION__", macro);

    macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    macro->name = "__STDC_HOSTED__";
    macro->replacement = new_token(T_numeric, &synth_built_in_loc, 1);
    macro->replacement->literal = "0";
    hashmap_put(MACROS, "__STDC_HOSTED__", macro);

    /* architecture defines */
    macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    macro->name = ARCH_PREDEFINED;
    macro->replacement = new_token(T_numeric, &synth_built_in_loc, 1);
    macro->replacement->literal = "1";
    hashmap_put(MACROS, ARCH_PREDEFINED, macro);

    /* shecc run-time defines */
    macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
    macro->name = "__SHECC__";
    macro->replacement = new_token(T_numeric, &synth_built_in_loc, 1);
    macro->replacement->literal = "1";
    hashmap_put(MACROS, "__SHECC__", macro);

    /* Tells the source being compiled that the embedded libc is not part of the
     * output, so the functions lib/c.c would have supplied -- '__syscall' above
     * all -- are unavailable and libc resolves through the PLT instead.
     */
    if (dynlink) {
        macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_t));
        macro->name = "__SHECC_DYNLINK__";
        macro->replacement = new_token(T_numeric, &synth_built_in_loc, 1);
        macro->replacement->literal = "1";
        hashmap_put(MACROS, "__SHECC_DYNLINK__", macro);
    }

    tk = pp_preprocess_internal(tk, &ctx);

    hashmap_free(MACROS);
    hashmap_free(PRAGMA_ONCE);
    return tk;
}

char *token_to_string(token_t *tk, char *dest)
{
    switch (tk->kind) {
    case T_eof:
        if (tk->next)
            error_at(
                "Internal error, token_to_string does not expect eof token in "
                "the middle of token stream",
                &tk->location);
        return NULL;
    case T_numeric:
    case T_floating:
        return tk->literal;
    case T_identifier:
        return tk->literal;
    case T_string:
        snprintf(dest, MAX_TOKEN_LEN, "\"%s\"", tk->literal);
        return dest;
    case T_wstring:
        snprintf(dest, MAX_TOKEN_LEN, "L\"%s\"", tk->literal);
        return dest;
    case T_char:
        snprintf(dest, MAX_TOKEN_LEN, "'%s'", tk->literal);
        return dest;
    case T_wchar:
        snprintf(dest, MAX_TOKEN_LEN, "L'%s'", tk->literal);
        return dest;
    case T_comma:
        return ",";
    case T_open_bracket:
        return "(";
    case T_close_bracket:
        return ")";
    case T_open_curly:
        return "{";
    case T_close_curly:
        return "}";
    case T_open_square:
        return "[";
    case T_close_square:
        return "]";
    case T_asterisk:
        return "*";
    case T_divide:
        return "/";
    case T_mod:
        return "%";
    case T_bit_or:
        return "|";
    case T_bit_xor:
        return "^";
    case T_bit_not:
        return "~";
    case T_log_and:
        return "&&";
    case T_log_or:
        return "||";
    case T_log_not:
        return "!";
    case T_lt:
        return "<";
    case T_gt:
        return ">";
    case T_le:
        return "<=";
    case T_ge:
        return ">=";
    case T_lshift:
        return "<<";
    case T_rshift:
        return ">>";
    case T_dot:
        return ".";
    case T_arrow:
        return "->";
    case T_plus:
        return "+";
    case T_minus:
        return "-";
    case T_minuseq:
        return "-=";
    case T_pluseq:
        return "+=";
    case T_asteriskeq:
        return "*=";
    case T_divideeq:
        return "/=";
    case T_modeq:
        return "%=";
    case T_lshifteq:
        return "<<=";
    case T_rshifteq:
        return ">>=";
    case T_xoreq:
        return "^=";
    case T_oreq:
        return "|=";
    case T_andeq:
        return "&=";
    case T_eq:
        return "==";
    case T_noteq:
        return "!=";
    case T_assign:
        return "=";
    case T_increment:
        return "++";
    case T_decrement:
        return "--";
    case T_question:
        return "?";
    case T_colon:
        return ":";
    case T_semicolon:
        return ";";
    case T_ampersand:
        return "&";
    case T_return:
        return "return";
    case T_if:
        return "if";
    case T_else:
        return "else";
    case T_while:
        return "while";
    case T_for:
        return "for";
    case T_do:
        return "do";
    case T_typedef:
        return "typedef";
    case T_enum:
        return "enum";
    case T_struct:
        return "struct";
    case T_union:
        return "union";
    case T_sizeof:
        return "sizeof";
    case T_elipsis:
        return "...";
    case T_switch:
        return "switch";
    case T_case:
        return "case";
    case T_break:
        return "break";
    case T_default:
        return "default";
    case T_continue:
        return "continue";
    case T_goto:
        return "goto";
    case T_const:
        return "const";
    case T_volatile:
        return "volatile";
    case T_static:
        return "static";
    case T_extern:
        return "extern";
    case T_register:
        return "register";
    case T_auto:
        return "auto";
    case T_restrict:
        return "restrict";
    case T_inline:
        return "inline";
    case T_signed:
        return "signed";
    case T_unsigned:
        return "unsigned";
    case T_long:
        return "long";
    case T_float:
        return "float";
    case T_double:
        return "double";
    case T_complex:
        return "_Complex";
    case T_imaginary:
        return "_Imaginary";
    case T_newline:
        return "\n";
    case T_backslash:
        error_at(
            "Internal error, backslash should be ommited after "
            "preprocessing",
            &tk->location);
        break;
    case T_whitespace: {
        int i = 0;
        for (; i < tk->location.len; i++)
            dest[i] = ' ';
        dest[i] = '\0';
        return dest;
    }
    case T_tab:
        return "\t";
    case T_start:
        /* FIXME: Unused token kind */
        break;
    case T_cppd_include:
    case T_cppd_define:
    case T_cppd_undef:
    case T_cppd_error:
    case T_cppd_if:
    case T_cppd_elif:
    case T_cppd_else:
    case T_cppd_endif:
    case T_cppd_ifdef:
    case T_cppd_ifndef:
    case T_cppd_pragma:
    case T_cppd_line:
    case T_cppd_unknown:
        error_at(
            "Internal error, preprocessor directives should be ommited "
            "after preprocessing",
            &tk->location);
        break;
    default:
        error_at("Unknown token kind", &tk->location);
        break;
    }

    return NULL;
}

void emit_preprocessed_token(token_t *tk)
{
    char token_buffer[MAX_TOKEN_LEN], *literal;

    while (tk) {
        literal = token_to_string(tk, token_buffer);

        if (literal)
            printf("%s", literal);

        tk = tk->next;
    }
}
