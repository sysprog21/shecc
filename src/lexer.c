/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>

#include "defs.h"
#include "globals.c"

/* Hash table constants */
#define NUM_DIRECTIVES 11
#define NUM_KEYWORDS 15

/* Preprocessor directive hash table using existing shecc hashmap */
hashmap_t *DIRECTIVE_MAP = NULL;
/* C keywords hash table */
hashmap_t *KEYWORD_MAP = NULL;
/* Token arrays for cleanup */
token_t *directive_tokens_storage = NULL;
token_t *keyword_tokens_storage = NULL;

void lex_init_directives()
{
    if (DIRECTIVE_MAP)
        return;

    DIRECTIVE_MAP = hashmap_create(16); /* Small capacity for directives */

    /* Initialization using indexed for-loop */
    directive_tokens_storage =
        arena_alloc(GENERAL_ARENA, NUM_DIRECTIVES * sizeof(token_t));

    char *names[NUM_DIRECTIVES];
    token_t token_values[NUM_DIRECTIVES];

    /* Populate arrays using index-based assignments for compatibility */
    names[0] = "#define";
    token_values[0] = T_cppd_define;
    names[1] = "#elif";
    token_values[1] = T_cppd_elif;
    names[2] = "#else";
    token_values[2] = T_cppd_else;
    names[3] = "#endif";
    token_values[3] = T_cppd_endif;
    names[4] = "#error";
    token_values[4] = T_cppd_error;
    names[5] = "#if";
    token_values[5] = T_cppd_if;
    names[6] = "#ifdef";
    token_values[6] = T_cppd_ifdef;
    names[7] = "#ifndef";
    token_values[7] = T_cppd_ifndef;
    names[8] = "#include";
    token_values[8] = T_cppd_include;
    names[9] = "#pragma";
    token_values[9] = T_cppd_pragma;
    names[10] = "#undef";
    token_values[10] = T_cppd_undef;

    /* hashmap insertion */
    for (int i = 0; i < NUM_DIRECTIVES; i++) {
        directive_tokens_storage[i] = token_values[i];
        hashmap_put(DIRECTIVE_MAP, names[i], &directive_tokens_storage[i]);
    }
}

void lex_init_keywords()
{
    if (KEYWORD_MAP)
        return;

    KEYWORD_MAP = hashmap_create(32); /* Capacity for keywords */

    /* Initialization using indexed for-loop */
    keyword_tokens_storage =
        arena_alloc(GENERAL_ARENA, NUM_KEYWORDS * sizeof(token_t));

    char *names[NUM_KEYWORDS];
    token_t token_values[NUM_KEYWORDS];

    /* Populate arrays using index-based assignments for compatibility */
    names[0] = "if";
    token_values[0] = T_if;
    names[1] = "while";
    token_values[1] = T_while;
    names[2] = "for";
    token_values[2] = T_for;
    names[3] = "do";
    token_values[3] = T_do;
    names[4] = "else";
    token_values[4] = T_else;
    names[5] = "return";
    token_values[5] = T_return;
    names[6] = "typedef";
    token_values[6] = T_typedef;
    names[7] = "enum";
    token_values[7] = T_enum;
    names[8] = "struct";
    token_values[8] = T_struct;
    names[9] = "sizeof";
    token_values[9] = T_sizeof;
    names[10] = "switch";
    token_values[10] = T_switch;
    names[11] = "case";
    token_values[11] = T_case;
    names[12] = "break";
    token_values[12] = T_break;
    names[13] = "default";
    token_values[13] = T_default;
    names[14] = "continue";
    token_values[14] = T_continue;

    /* hashmap insertion */
    for (int i = 0; i < NUM_KEYWORDS; i++) {
        keyword_tokens_storage[i] = token_values[i];
        hashmap_put(KEYWORD_MAP, names[i], &keyword_tokens_storage[i]);
    }
}

/* Hash table lookup for preprocessor directives */
token_t lookup_directive(char *token)
{
    if (!DIRECTIVE_MAP)
        lex_init_directives();

    token_t *result = hashmap_get(DIRECTIVE_MAP, token);
    if (result)
        return *result;

    return T_identifier;
}

/* Hash table lookup for C keywords */
token_t lookup_keyword(char *token)
{
    if (!KEYWORD_MAP)
        lex_init_keywords();

    token_t *result = hashmap_get(KEYWORD_MAP, token);
    if (result)
        return *result;

    return T_identifier;
}

/* Cleanup function for lexer hashmaps */
void lexer_cleanup()
{
    if (DIRECTIVE_MAP) {
        hashmap_free(DIRECTIVE_MAP);
        DIRECTIVE_MAP = NULL;
    }

    if (KEYWORD_MAP) {
        hashmap_free(KEYWORD_MAP);
        KEYWORD_MAP = NULL;
    }

    /* Token storage arrays are allocated from GENERAL_ARENA and will be
     * automatically freed when the arena is freed in global_release().
     * No need to explicitly free them here.
     */
    directive_tokens_storage = NULL;
    keyword_tokens_storage = NULL;
}

bool is_whitespace(char c)
{
    return c == ' ' || c == '\t';
}

char peek_char(int offset);

/* is it backslash-newline? */
bool is_linebreak(char c)
{
    return c == '\\' && peek_char(1) == '\n';
}

bool is_newline(char c)
{
    return c == '\r' || c == '\n';
}

/* is it alphabet, number or '_'? */
bool is_alnum(char c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || (c == '_'));
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool is_numeric(char buffer[])
{
    bool hex = false;
    int size = strlen(buffer);

    if (size > 2 && buffer[0] == '0' && (buffer[1] | 32) == 'x')
        hex = true;

    for (int i = hex ? 2 : 0; i < size; i++) {
        if (hex && !is_hex(buffer[i]))
            return false;
        if (!hex && !is_digit(buffer[i]))
            return false;
    }
    return true;
}

void skip_whitespace(void)
{
    while (true) {
        if (is_linebreak(next_char)) {
            SOURCE->size += 2;
            next_char = SOURCE->elements[SOURCE->size];
            continue;
        }
        if (is_whitespace(next_char) ||
            (skip_newline && is_newline(next_char))) {
            SOURCE->size++;
            next_char = SOURCE->elements[SOURCE->size];
            continue;
        }
        break;
    }
}

char read_char(bool is_skip_space)
{
    SOURCE->size++;
    next_char = SOURCE->elements[SOURCE->size];
    if (is_skip_space)
        skip_whitespace();
    return next_char;
}

char peek_char(int offset)
{
    return SOURCE->elements[SOURCE->size + offset];
}

/* Lex next token and returns its token type. Parameter 'aliasing' is used for
 * disable preprocessor aliasing on identifier tokens.
 */
token_t lex_token_internal(bool aliasing)
{
    token_str[0] = 0;

    /* partial preprocessor */
    if (next_char == '#') {
        int i = 0;

        do {
            token_str[i++] = next_char;
        } while (is_alnum(read_char(false)));
        token_str[i] = 0;
        skip_whitespace();

        token_t directive = lookup_directive(token_str);
        if (directive != T_identifier)
            return directive;
        error("Unknown directive");
    }

    if (next_char == '/') {
        read_char(true);

        /* C-style comments */
        if (next_char == '*') {
            /* in a comment, skip until end */
            do {
                read_char(false);
                if (next_char == '*') {
                    read_char(false);
                    if (next_char == '/') {
                        read_char(true);
                        return lex_token_internal(aliasing);
                    }
                }
            } while (next_char);

            if (!next_char)
                error("Unenclosed C-style comment");
            return lex_token_internal(aliasing);
        }

        /* C++-style comments */
        if (next_char == '/') {
            do {
                read_char(false);
            } while (next_char && !is_newline(next_char));
            return lex_token_internal(aliasing);
        }

        if (next_char == '=') {
            read_char(true);
            return T_divideeq;
        }

        return T_divide;
    }

    if (is_digit(next_char)) {
        int i = 0;
        token_str[i++] = next_char;
        read_char(false);

        if (token_str[0] == '0' && ((next_char | 32) == 'x')) {
            /* Hexadecimal: starts with 0x or 0X */
            token_str[i++] = next_char;

            read_char(false);
            if (!is_hex(next_char))
                error("Invalid hex literal: expected hex digit after 0x");

            do {
                token_str[i++] = next_char;
            } while (is_hex(read_char(false)));

        } else if (token_str[0] == '0') {
            /* Octal: starts with 0 but not followed by 'x' */
            while (is_digit(next_char)) {
                if (next_char >= '8')
                    error("Invalid octal digit: must be in range 0-7");
                token_str[i++] = next_char;
                read_char(false);
            }

        } else {
            /* Decimal */
            while (is_digit(next_char)) {
                token_str[i++] = next_char;
                read_char(false);
            }
        }

        token_str[i] = 0;
        skip_whitespace();
        return T_numeric;
    }
    if (next_char == '(') {
        read_char(true);
        return T_open_bracket;
    }
    if (next_char == ')') {
        read_char(true);
        return T_close_bracket;
    }
    if (next_char == '{') {
        read_char(true);
        return T_open_curly;
    }
    if (next_char == '}') {
        read_char(true);
        return T_close_curly;
    }
    if (next_char == '[') {
        read_char(true);
        return T_open_square;
    }
    if (next_char == ']') {
        read_char(true);
        return T_close_square;
    }
    if (next_char == ',') {
        read_char(true);
        return T_comma;
    }
    if (next_char == '^') {
        read_char(true);

        if (next_char == '=') {
            read_char(true);
            return T_xoreq;
        }

        return T_bit_xor;
    }
    if (next_char == '~') {
        read_char(true);
        return T_bit_not;
    }
    if (next_char == '"') {
        int i = 0;
        int special = 0;

        while ((read_char(false) != '"') || special) {
            if ((i > 0) && (token_str[i - 1] == '\\')) {
                if (next_char == 'n')
                    token_str[i - 1] = '\n';
                else if (next_char == '"')
                    token_str[i - 1] = '"';
                else if (next_char == 'r')
                    token_str[i - 1] = '\r';
                else if (next_char == '\'')
                    token_str[i - 1] = '\'';
                else if (next_char == 't')
                    token_str[i - 1] = '\t';
                else if (next_char == '\\')
                    token_str[i - 1] = '\\';
                else if (next_char == '0')
                    token_str[i - 1] = '\0';
                else
                    abort();
            } else {
                token_str[i++] = next_char;
            }
            if (next_char == '\\')
                special = 1;
            else
                special = 0;
        }
        token_str[i] = 0;
        read_char(true);
        return T_string;
    }
    if (next_char == '\'') {
        read_char(false);
        if (next_char == '\\') {
            read_char(false);
            if (next_char == 'n')
                token_str[0] = '\n';
            else if (next_char == 'r')
                token_str[0] = '\r';
            else if (next_char == '\'')
                token_str[0] = '\'';
            else if (next_char == '"')
                token_str[0] = '"';
            else if (next_char == 't')
                token_str[0] = '\t';
            else if (next_char == '\\')
                token_str[0] = '\\';
            else if (next_char == '0')
                token_str[0] = '\0';
            else
                abort();
        } else {
            token_str[0] = next_char;
        }
        token_str[1] = 0;
        if (read_char(true) != '\'')
            abort();
        read_char(true);
        return T_char;
    }
    if (next_char == '*') {
        read_char(true);

        if (next_char == '=') {
            read_char(true);
            return T_asteriskeq;
        }

        return T_asterisk;
    }
    if (next_char == '&') {
        read_char(false);
        if (next_char == '&') {
            read_char(true);
            return T_log_and;
        }
        if (next_char == '=') {
            read_char(true);
            return T_andeq;
        }
        skip_whitespace();
        return T_ampersand;
    }
    if (next_char == '|') {
        read_char(false);
        if (next_char == '|') {
            read_char(true);
            return T_log_or;
        }
        if (next_char == '=') {
            read_char(true);
            return T_oreq;
        }
        skip_whitespace();
        return T_bit_or;
    }
    if (next_char == '<') {
        read_char(false);
        if (next_char == '=') {
            read_char(true);
            return T_le;
        }
        if (next_char == '<') {
            read_char(true);

            if (next_char == '=') {
                read_char(true);
                return T_lshifteq;
            }

            return T_lshift;
        }
        skip_whitespace();
        return T_lt;
    }
    if (next_char == '%') {
        read_char(true);

        if (next_char == '=') {
            read_char(true);
            return T_modeq;
        }

        return T_mod;
    }
    if (next_char == '>') {
        read_char(false);
        if (next_char == '=') {
            read_char(true);
            return T_ge;
        }
        if (next_char == '>') {
            read_char(true);

            if (next_char == '=') {
                read_char(true);
                return T_rshifteq;
            }

            return T_rshift;
        }
        skip_whitespace();
        return T_gt;
    }
    if (next_char == '!') {
        read_char(false);
        if (next_char == '=') {
            read_char(true);
            return T_noteq;
        }
        skip_whitespace();
        return T_log_not;
    }
    if (next_char == '.') {
        read_char(false);
        if (next_char == '.') {
            read_char(false);
            if (next_char == '.') {
                read_char(true);
                return T_elipsis;
            }
            abort();
        }
        skip_whitespace();
        return T_dot;
    }
    if (next_char == '-') {
        read_char(true);
        if (next_char == '>') {
            read_char(true);
            return T_arrow;
        }
        if (next_char == '-') {
            read_char(true);
            return T_decrement;
        }
        if (next_char == '=') {
            read_char(true);
            return T_minuseq;
        }
        skip_whitespace();
        return T_minus;
    }
    if (next_char == '+') {
        read_char(false);
        if (next_char == '+') {
            read_char(true);
            return T_increment;
        }
        if (next_char == '=') {
            read_char(true);
            return T_pluseq;
        }
        skip_whitespace();
        return T_plus;
    }
    if (next_char == ';') {
        read_char(true);
        return T_semicolon;
    }
    if (next_char == '?') {
        read_char(true);
        return T_question;
    }
    if (next_char == ':') {
        read_char(true);
        return T_colon;
    }
    if (next_char == '=') {
        read_char(false);
        if (next_char == '=') {
            read_char(true);
            return T_eq;
        }
        skip_whitespace();
        return T_assign;
    }

    if (is_alnum(next_char)) {
        char *alias;
        int i = 0;
        do {
            token_str[i++] = next_char;
        } while (is_alnum(read_char(false)));
        token_str[i] = 0;
        skip_whitespace();

        token_t keyword = lookup_keyword(token_str);
        if (keyword != T_identifier)
            return keyword;

        if (aliasing) {
            alias = find_alias(token_str);
            if (alias) {
                /* FIXME: comparison with string "bool" is a temporary hack */
                token_t t;

                if (is_numeric(alias)) {
                    t = T_numeric;
                } else if (!strcmp(alias, "_Bool")) {
                    t = T_identifier;
                } else {
                    t = T_string;
                }

                strcpy(token_str, alias);
                return t;
            }
        }

        return T_identifier;
    }

    /* This only happens when parsing a macro. Move to the token after the
     * macro definition or return to where the macro has been called.
     */
    if (next_char == '\n') {
        if (macro_return_idx) {
            SOURCE->size = macro_return_idx;
            next_char = SOURCE->elements[SOURCE->size];
        } else
            next_char = read_char(true);
        return lex_token_internal(aliasing);
    }

    if (next_char == 0)
        return T_eof;

    error("Unrecognized input");

    /* Unreachable, but we need an explicit return for non-void method. */
    return T_eof;
}

/* Lex next token and returns its token type. To disable aliasing on next
 * token, use 'lex_token_internal'.
 */
token_t lex_token(void)
{
    return lex_token_internal(true);
}

/* Skip the content. We only need the index where the macro body begins. */
void skip_macro_body(void)
{
    while (!is_newline(next_char))
        next_token = lex_token();

    skip_newline = true;
    next_token = lex_token();
}

/* Accepts next token if token types are matched. */
bool lex_accept_internal(token_t token, bool aliasing)
{
    if (next_token == token) {
        next_token = lex_token_internal(aliasing);
        return true;
    }

    return false;
}

/* Accepts next token if token types are matched. To disable aliasing on next
 * token, use 'lex_accept_internal'.
 */
bool lex_accept(token_t token)
{
    return lex_accept_internal(token, 1);
}

/* Peeks next token and copy token's literal to value if token types are
 * matched.
 */
bool lex_peek(token_t token, char *value)
{
    if (next_token == token) {
        if (!value)
            return true;
        strcpy(value, token_str);
        return true;
    }
    return false;
}

/* Strictly match next token with given token type and copy token's literal to
 * value.
 */
void lex_ident_internal(token_t token, char *value, bool aliasing)
{
    if (next_token != token)
        error("Unexpected token");
    strcpy(value, token_str);
    next_token = lex_token_internal(aliasing);
}

/* Strictly match next token with given token type and copy token's literal to
 * value. To disable aliasing on next token, use 'lex_ident_internal'.
 */
void lex_ident(token_t token, char *value)
{
    lex_ident_internal(token, value, true);
}

/* Strictly match next token with given token type. */
void lex_expect_internal(token_t token, bool aliasing)
{
    if (next_token != token)
        error("Unexpected token");
    next_token = lex_token_internal(aliasing);
}

/* Strictly match next token with given token type. To disable aliasing on next
 * token, use 'lex_expect_internal'.
 */
void lex_expect(token_t token)
{
    lex_expect_internal(token, true);
}
