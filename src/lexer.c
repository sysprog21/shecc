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
#define NUM_KEYWORDS 16

/* Token mapping structure for elegant initialization */
typedef struct {
    char *name;
    token_t token;
} token_mapping_t;

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

    /* Initialization using struct compound literals for elegance */
    directive_tokens_storage =
        arena_alloc(GENERAL_ARENA, NUM_DIRECTIVES * sizeof(token_t));

    /* Use array compound literal for directive mappings */
    token_mapping_t directives[] = {
        {"#define", T_cppd_define},   {"#elif", T_cppd_elif},
        {"#else", T_cppd_else},       {"#endif", T_cppd_endif},
        {"#error", T_cppd_error},     {"#if", T_cppd_if},
        {"#ifdef", T_cppd_ifdef},     {"#ifndef", T_cppd_ifndef},
        {"#include", T_cppd_include}, {"#pragma", T_cppd_pragma},
        {"#undef", T_cppd_undef},
    };

    /* hashmap insertion */
    for (int i = 0; i < NUM_DIRECTIVES; i++) {
        directive_tokens_storage[i] = directives[i].token;
        hashmap_put(DIRECTIVE_MAP, directives[i].name,
                    &directive_tokens_storage[i]);
    }
}

void lex_init_keywords()
{
    if (KEYWORD_MAP)
        return;

    KEYWORD_MAP = hashmap_create(32); /* Capacity for keywords */

    /* Initialization using struct compound literals for elegance */
    keyword_tokens_storage =
        arena_alloc(GENERAL_ARENA, NUM_KEYWORDS * sizeof(token_t));

    /* Use array compound literal for keyword mappings */
    token_mapping_t keywords[] = {
        {"if", T_if},
        {"while", T_while},
        {"for", T_for},
        {"do", T_do},
        {"else", T_else},
        {"return", T_return},
        {"typedef", T_typedef},
        {"enum", T_enum},
        {"struct", T_struct},
        {"sizeof", T_sizeof},
        {"switch", T_switch},
        {"case", T_case},
        {"break", T_break},
        {"default", T_default},
        {"continue", T_continue},
        {"union", T_union},
    };

    /* hashmap insertion */
    for (int i = 0; i < NUM_KEYWORDS; i++) {
        keyword_tokens_storage[i] = keywords[i].token;
        hashmap_put(KEYWORD_MAP, keywords[i].name, &keyword_tokens_storage[i]);
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
/* Token Memory Management Functions */

/* Initialize token pool for memory reuse */
void token_pool_init(void)
{
    if (TOKEN_POOL)
        return;

    TOKEN_POOL = arena_alloc(GENERAL_ARENA, sizeof(token_pool_t));
    if (TOKEN_POOL) {
        TOKEN_POOL->freelist = NULL;
        TOKEN_POOL->allocated_count = 0;
        TOKEN_POOL->reused_count = 0;
    }
}

/* Allocate or reuse a token from the pool */
token_info_t *token_pool_alloc(void)
{
    if (!TOKEN_POOL)
        token_pool_init();

    token_info_t *token;

    if (TOKEN_POOL->freelist) {
        /* Reuse from freelist */
        token = TOKEN_POOL->freelist;
        TOKEN_POOL->freelist = token->next;
        TOKEN_POOL->reused_count++;
    } else {
        /* Allocate new token */
        token = arena_alloc(GENERAL_ARENA, sizeof(token_info_t));
        TOKEN_POOL->allocated_count++;
    }

    /* Clear token data */
    token->type = T_eof;
    token->value[0] = '\0';
    /* Set location fields individually */
    token->location.line = current_location.line;
    token->location.column = current_location.column;
    token->location.filename = current_location.filename;
    token->next = NULL;

    return token;
}

/* Return token to freelist for reuse */
void token_pool_free(token_info_t *token)
{
    if (!token || !TOKEN_POOL)
        return;

    token->next = TOKEN_POOL->freelist;
    TOKEN_POOL->freelist = token;
}

/* Initialize token buffer for lookahead */
void token_buffer_init(void)
{
    if (TOKEN_BUFFER)
        return;

    TOKEN_BUFFER = arena_alloc(GENERAL_ARENA, sizeof(token_buffer_t));
    TOKEN_BUFFER->head = 0;
    TOKEN_BUFFER->tail = 0;
    TOKEN_BUFFER->count = 0;

    for (int i = 0; i < TOKEN_BUFFER_SIZE; i++)
        TOKEN_BUFFER->tokens[i] = NULL;
}

/* Add token to buffer */
void token_buffer_push(token_info_t *token)
{
    if (!TOKEN_BUFFER)
        token_buffer_init();

    if (TOKEN_BUFFER->count >= TOKEN_BUFFER_SIZE) {
        /* Buffer full, free oldest token */
        token_info_t *old = TOKEN_BUFFER->tokens[TOKEN_BUFFER->head];
        token_pool_free(old);
        TOKEN_BUFFER->head = (TOKEN_BUFFER->head + 1) % TOKEN_BUFFER_SIZE;
        TOKEN_BUFFER->count--;
    }

    TOKEN_BUFFER->tokens[TOKEN_BUFFER->tail] = token;
    TOKEN_BUFFER->tail = (TOKEN_BUFFER->tail + 1) % TOKEN_BUFFER_SIZE;
    TOKEN_BUFFER->count++;
}

/* Look ahead N tokens without consuming */
token_info_t *token_buffer_peek(int offset)
{
    if (!TOKEN_BUFFER || offset >= TOKEN_BUFFER->count)
        return NULL;

    int idx = (TOKEN_BUFFER->head + offset) % TOKEN_BUFFER_SIZE;
    return TOKEN_BUFFER->tokens[idx];
}

/* Update source location tracking */
void update_location(char c)
{
    if (c == '\n') {
        current_location.line++;
        current_location.column = 1;
    } else if (c == '\t') {
        current_location.column += 4; /* Assume 4-space tabs */
    } else {
        current_location.column++;
    }
}

/* Set current filename for error reporting */
void set_current_filename(char *filename)
{
    current_location.filename = filename;
    current_location.line = 1;
    current_location.column = 1;
}

/* Enhanced error reporting with location */
void error_with_location(char *msg, source_location_t *loc)
{
    if (loc && loc->filename) {
        printf("%s:%d:%d: error: %s\n", loc->filename, loc->line, loc->column,
               msg);
    } else if (loc) {
        printf("line %d, column %d: error: %s\n", loc->line, loc->column, msg);
    } else {
        printf("error: %s\n", msg);
    }
    abort();
}

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

    /* Token pool and buffer are also arena-allocated, no explicit free needed
     */
    TOKEN_POOL = NULL;
    TOKEN_BUFFER = NULL;
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

int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
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
    int pos = SOURCE->size;
    while (true) {
        /* Handle backslash-newline (line continuation) using local pos */
        if (next_char == '\\' && SOURCE->elements[pos + 1] == '\n') {
            pos += 2;
            next_char = SOURCE->elements[pos];
            continue;
        }
        if (is_whitespace(next_char) ||
            (skip_newline && is_newline(next_char))) {
            pos++;
            next_char = SOURCE->elements[pos];
            continue;
        }
        break;
    }
    SOURCE->size = pos;
}

char read_char(bool is_skip_space)
{
    SOURCE->size++;
    next_char = SOURCE->elements[SOURCE->size];
    /* TODO: Re-enable after self-hosting: update_location(next_char); */
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
            if (i >= MAX_TOKEN_LEN - 1)
                error("Token too long");
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
            int pos = SOURCE->size;
            do {
                /* advance one char */
                pos++;
                next_char = SOURCE->elements[pos];
                if (next_char == '*') {
                    /* look ahead */
                    pos++;
                    next_char = SOURCE->elements[pos];
                    if (next_char == '/') {
                        /* consume closing '/', then commit and skip trailing
                         * whitespaces
                         */
                        pos++;
                        next_char = SOURCE->elements[pos];
                        SOURCE->size = pos;
                        skip_whitespace();
                        return lex_token_internal(aliasing);
                    }
                }
            } while (next_char);

            SOURCE->size = pos;
            if (!next_char)
                error("Unenclosed C-style comment");
            return lex_token_internal(aliasing);
        }

        /* C++-style comments */
        if (next_char == '/') {
            int pos = SOURCE->size;
            do {
                pos++;
                next_char = SOURCE->elements[pos];
            } while (next_char && !is_newline(next_char));
            SOURCE->size = pos;
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
        if (i >= MAX_TOKEN_LEN - 1)
            error("Token too long");
        token_str[i++] = next_char;
        read_char(false);

        if (token_str[0] == '0' && ((next_char | 32) == 'x')) {
            /* Hexadecimal: starts with 0x or 0X */
            if (i >= MAX_TOKEN_LEN - 1)
                error("Token too long");
            token_str[i++] = next_char;

            read_char(false);
            if (!is_hex(next_char))
                error("Invalid hex literal: expected hex digit after 0x");

            do {
                if (i >= MAX_TOKEN_LEN - 1)
                    error("Token too long");
                token_str[i++] = next_char;
            } while (is_hex(read_char(false)));

        } else if (token_str[0] == '0' && ((next_char | 32) == 'b')) {
            /* Binary: starts with 0b or 0B */
            if (i >= MAX_TOKEN_LEN - 1)
                error("Token too long");
            token_str[i++] = next_char;

            read_char(false);
            if (next_char != '0' && next_char != '1')
                error("Invalid binary literal: expected 0 or 1 after 0b");

            do {
                if (i >= MAX_TOKEN_LEN - 1)
                    error("Token too long");
                token_str[i++] = next_char;
                read_char(false);
            } while (next_char == '0' || next_char == '1');

        } else if (token_str[0] == '0') {
            /* Octal: starts with 0 but not followed by 'x' or 'b' */
            while (is_digit(next_char)) {
                if (next_char >= '8')
                    error("Invalid octal digit: must be in range 0-7");
                if (i >= MAX_TOKEN_LEN - 1)
                    error("Token too long");
                token_str[i++] = next_char;
                read_char(false);
            }

        } else {
            /* Decimal */
            while (is_digit(next_char)) {
                if (i >= MAX_TOKEN_LEN - 1)
                    error("Token too long");
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
                else if (next_char == 'a')
                    token_str[i - 1] = '\a';
                else if (next_char == 'b')
                    token_str[i - 1] = '\b';
                else if (next_char == 'v')
                    token_str[i - 1] = '\v';
                else if (next_char == 'f')
                    token_str[i - 1] = '\f';
                else if (next_char == 'e') /* GNU extension: ESC character */
                    token_str[i - 1] = 27;
                else if (next_char == '?')
                    token_str[i - 1] = '?';
                else if (next_char == 'x') {
                    /* Hexadecimal escape sequence \xHH */
                    read_char(false);
                    if (!is_hex(next_char))
                        error("Invalid hex escape sequence");
                    int value = 0;
                    int count = 0;
                    while (is_hex(next_char) && count < 2) {
                        value = (value << 4) + hex_digit_value(next_char);
                        read_char(false);
                        count++;
                    }
                    token_str[i - 1] = value;
                    /* Back up one character as we read one too many */
                    SOURCE->size--;
                    next_char = SOURCE->elements[SOURCE->size];
                } else if (next_char >= '0' && next_char <= '7') {
                    /* Octal escape sequence \nnn */
                    int value = next_char - '0';
                    read_char(false);
                    if (next_char >= '0' && next_char <= '7') {
                        value = (value << 3) + (next_char - '0');
                        read_char(false);
                        if (next_char >= '0' && next_char <= '7') {
                            value = (value << 3) + (next_char - '0');
                        } else {
                            /* Back up one character */
                            SOURCE->size--;
                            next_char = SOURCE->elements[SOURCE->size];
                        }
                    } else {
                        /* Back up one character */
                        SOURCE->size--;
                        next_char = SOURCE->elements[SOURCE->size];
                    }
                    token_str[i - 1] = value;
                } else {
                    /* Handle unknown escapes gracefully */
                    token_str[i - 1] = next_char;
                }
            } else {
                if (i >= MAX_TOKEN_LEN - 1)
                    error("String literal too long");
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
            else if (next_char == 'a')
                token_str[0] = '\a';
            else if (next_char == 'b')
                token_str[0] = '\b';
            else if (next_char == 'v')
                token_str[0] = '\v';
            else if (next_char == 'f')
                token_str[0] = '\f';
            else if (next_char == 'e') /* GNU extension: ESC character */
                token_str[0] = 27;
            else if (next_char == '?')
                token_str[0] = '?';
            else if (next_char == 'x') {
                /* Hexadecimal escape sequence \xHH */
                read_char(false);
                if (!is_hex(next_char))
                    error("Invalid hex escape sequence");
                int value = 0;
                int count = 0;
                while (is_hex(next_char) && count < 2) {
                    value = (value << 4) + hex_digit_value(next_char);
                    read_char(false);
                    count++;
                }
                token_str[0] = value;
                /* Back up one character as we read one too many */
                SOURCE->size--;
                next_char = SOURCE->elements[SOURCE->size];
            } else if (next_char >= '0' && next_char <= '7') {
                /* Octal escape sequence \nnn */
                int value = next_char - '0';
                read_char(false);
                if (next_char >= '0' && next_char <= '7') {
                    value = (value << 3) + (next_char - '0');
                    read_char(false);
                    if (next_char >= '0' && next_char <= '7') {
                        value = (value << 3) + (next_char - '0');
                    } else {
                        /* Back up one character */
                        SOURCE->size--;
                        next_char = SOURCE->elements[SOURCE->size];
                    }
                } else {
                    /* Back up one character */
                    SOURCE->size--;
                    next_char = SOURCE->elements[SOURCE->size];
                }
                token_str[0] = value;
            } else {
                /* Handle unknown escapes gracefully */
                token_str[0] = next_char;
            }
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
            if (i >= MAX_TOKEN_LEN - 1)
                error("Token too long");
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

/* Enhanced lex_token that returns a full token_info structure */
token_info_t *lex_token_enhanced(bool aliasing)
{
    token_info_t *token = token_pool_alloc();

    /* Save location at start of token */
    int saved_line = current_location.line;
    int saved_column = current_location.column;
    char *saved_filename = current_location.filename;

    /* Get the token type using existing logic */
    token->type = lex_token_internal(aliasing);

    /* Copy token string value */
    strcpy(token->value, token_str);

    /* Restore saved location fields individually */
    token->location.line = saved_line;
    token->location.column = saved_column;
    token->location.filename = saved_filename;

    /* Add to buffer for lookahead capability */
    token_buffer_push(token);

    return token;
}

/* Lex next token and returns its token type. To disable aliasing on next
 * token, use 'lex_token_internal'.
 */
token_t lex_token(void)
{
    return lex_token_internal(true);
}

/* Advanced lookahead functions using token buffer */
bool lex_peek_ahead(int offset, token_t expected_type)
{
    token_info_t *future_token = token_buffer_peek(offset);
    return future_token && future_token->type == expected_type;
}

/* Check if next N tokens match a pattern */
bool lex_match_sequence(token_t *pattern, int count)
{
    for (int i = 0; i < count; i++) {
        if (!lex_peek_ahead(i, pattern[i]))
            return false;
    }
    return true;
}

/* Get token value at offset for lookahead inspection */
char *lex_peek_value(int offset)
{
    token_info_t *future_token = token_buffer_peek(offset);
    return future_token ? future_token->value : NULL;
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
