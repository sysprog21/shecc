/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */
#include <ctype.h>
#include <stdbool.h>

#include "defs.h"
#include "globals.c"

/* Hash table constants */
#define NUM_DIRECTIVES 11
#define NUM_KEYWORDS 18

/* Token mapping structure for elegant initialization */
typedef struct {
    char *name;
    token_kind_t token;
} token_mapping_t;

/* Preprocessor directive hash table using existing shecc hashmap */
hashmap_t *DIRECTIVE_MAP = NULL;
/* C keywords hash table */
hashmap_t *KEYWORD_MAP = NULL;
/* Token arrays for cleanup */
token_kind_t *directive_tokens_storage = NULL;
token_kind_t *keyword_tokens_storage = NULL;

void lex_init_directives()
{
    if (DIRECTIVE_MAP)
        return;

    DIRECTIVE_MAP = hashmap_create(16); /* Small capacity for directives */

    /* Initialization using struct compound literals for elegance */
    directive_tokens_storage =
        arena_alloc(GENERAL_ARENA, NUM_DIRECTIVES * sizeof(token_kind_t));

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
        arena_alloc(GENERAL_ARENA, NUM_KEYWORDS * sizeof(token_kind_t));

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
        {"goto", T_goto},
        {"union", T_union},
        {"const", T_const},
    };

    /* hashmap insertion */
    for (int i = 0; i < NUM_KEYWORDS; i++) {
        keyword_tokens_storage[i] = keywords[i].token;
        hashmap_put(KEYWORD_MAP, keywords[i].name, &keyword_tokens_storage[i]);
    }
}

/* Hash table lookup for preprocessor directives */
token_kind_t lookup_directive(char *token)
{
    if (!DIRECTIVE_MAP)
        lex_init_directives();

    token_kind_t *result = hashmap_get(DIRECTIVE_MAP, token);
    if (result)
        return *result;

    return T_identifier;
}

/* Hash table lookup for C keywords */
token_kind_t lookup_keyword(char *token)
{
    if (!KEYWORD_MAP)
        lex_init_keywords();

    token_kind_t *result = hashmap_get(KEYWORD_MAP, token);
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
     * automatically freed when the arena is freed in global_release(). No need
     * to explicitly free them here.
     */
    directive_tokens_storage = NULL;
    keyword_tokens_storage = NULL;
}

char peek_char(strbuf_t *buf, int offset)
{
    if (buf->size + offset >= buf->capacity)
        return '\0';
    return buf->elements[buf->size + offset];
}

char read_char(strbuf_t *buf)
{
    if (buf->size + 1 >= buf->capacity)
        return buf->elements[buf->capacity - 1];
    buf->size++;
    return buf->elements[buf->size];
}

/* Fill @dst with @len bytes of @f, returning how many arrived.
 *
 * The whole file is wanted and its size is already known, so it is asked for in
 * one piece. Reading it a line at a time instead cost a library call and a
 * second copy for every line of every source file -- and shecc's own libc has
 * no buffer behind fgets(), so each of those lines was a read(2) as well.
 */
#ifdef HOST_BUFFERED_STDIO
int file_read_all(FILE *f, char *dst, int len)
{
    if (len <= 0)
        return 0;
    return fread(dst, 1, len, f);
}
#else
int file_read_all(FILE *f, char *dst, int len)
{
    int got = 0;

    while (got < len) {
        int n = __syscall(__syscall_read, f, dst + got, len - got);
        if (n <= 0)
            return got;
        got += n;
    }
    return got;
}
#endif

strbuf_t *read_file(char *filename)
{
    FILE *f = fopen(filename, "rb");
    strbuf_t *src;

    if (!f) {
        printf("filename: %s\n", filename);
        fatal("source file cannot be found.");
    }

    fseek(f, 0, SEEK_END);
    int len = ftell(f);
    src = strbuf_create(len + 1);
    fseek(f, 0, SEEK_SET);

    src->size = file_read_all(f, src->elements, len);

    fclose(f);
    src->elements[len] = '\0';
    return src;
}

strbuf_t *get_file_buf(char *filename)
{
    strbuf_t *buf;

    if (!hashmap_contains(SRC_FILE_MAP, filename)) {
        buf = read_file(filename);
        hashmap_put(SRC_FILE_MAP, filename, buf);
    } else {
        buf = hashmap_get(SRC_FILE_MAP, filename);
    }

    return buf;
}

token_t *new_token(token_kind_t kind, source_location_t *loc, int len)
{
    /* Every field is written here, so the allocation does not need zeroing
     * first -- and tokens are the single largest source of allocations in the
     * compiler.
     */
    token_t *token = arena_alloc(TOKEN_ARENA, sizeof(token_t));
    token->kind = kind;
    token->literal = NULL;
    token->next = NULL;
    memcpy(&token->location, loc, sizeof(source_location_t));
    token->location.len = len;
    return token;
}

token_t *lex_token(strbuf_t *buf, source_location_t *loc)
{
    token_t *token;
    char token_buffer[MAX_TOKEN_LEN], ch = peek_char(buf, 0);

    loc->pos = buf->size;

    if (ch == '#') {
        /* Inside a macro replacement list '#' stringifies the parameter that
         * follows and '##' pastes its neighbours. Neither can be a directive,
         * which only exists at the start of a line.
         */
        if (peek_char(buf, 1) == '#') {
            read_char(buf);
            read_char(buf);
            token = new_token(T_hashhash, loc, 2);
            loc->column += 2;
            return token;
        }

        if (loc->column != 1) {
            read_char(buf);
            token = new_token(T_hash, loc, 1);
            loc->column++;
            return token;
        }

        int sz = 0;

        do {
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz;
                error_at("Token too long", loc);
            }
            token_buffer[sz++] = ch;
            ch = read_char(buf);
        } while (isalnum(ch) || ch == '_');
        token_buffer[sz] = '\0';

        token_kind_t directive_kind = lookup_directive(token_buffer);
        if (directive_kind == T_identifier) {
            loc->len = sz;
            error_at("Unsupported directive", loc);
        }

        token = new_token(directive_kind, loc, sz);
        loc->column += sz;
        return token;
    }

    if (ch == '\\') {
        read_char(buf);
        token = new_token(T_backslash, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '\n') {
        read_char(buf);
        token = new_token(T_newline, loc, 1);
        loc->line++;
        loc->column = 1;
        return token;
    }

    if (ch == '/') {
        ch = read_char(buf);

        if (ch == '*') {
            /* C-style comment */
            int pos = buf->size;
            do {
                /* advance one char */
                pos++;
                loc->column++;
                ch = buf->elements[pos];
                if (ch == '*') {
                    /* look ahead */
                    pos++;
                    loc->column++;
                    ch = buf->elements[pos];
                    if (ch == '/') {
                        /* consume closing '/', then commit and skip trailing
                         * whitespaces
                         */
                        pos++;
                        loc->column += 2;
                        buf->size = pos;
                        return lex_token(buf, loc);
                    }
                }

                if (ch == '\n') {
                    loc->line++;
                    loc->column = 1;
                }
            } while (ch);

            error_at("Unenclosed C-style comment", loc);
            return NULL;
        }

        if (ch == '/') {
            /* C++-style comment */
            int pos = buf->size;
            do {
                pos++;
                ch = buf->elements[pos];
            } while (ch && !is_newline(ch));
            loc->column += pos - buf->size + 1;
            buf->size = pos;
            return lex_token(buf, loc);
        }

        if (ch == '=') {
            ch = read_char(buf);
            token = new_token(T_divideeq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_divide, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ' ') {
        /* Compacts sequence of whitespace together */
        int sz = 1;

        while (read_char(buf) == ' ')
            sz++;

        token = new_token(T_whitespace, loc, sz);
        loc->column += sz;
        return token;
    }

    if (ch == '\t') {
        read_char(buf);
        token = new_token(T_tab, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '\0') {
        read_char(buf);
        token = new_token(T_eof, loc, 1);
        loc->column++;
        return token;
    }

    if (isdigit(ch)) {
        int sz = 0;
        token_buffer[sz++] = ch;
        ch = read_char(buf);

        if (token_buffer[0] == '0' && ((ch | 32) == 'x')) {
            /* Hexadecimal: starts with 0x or 0X */
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz;
                error_at("Token too long", loc);
            }
            token_buffer[sz++] = ch;

            ch = read_char(buf);
            if (!isxdigit(ch)) {
                loc->len = 3;
                error_at("Invalid hex literal: expected hex digit after 0x",
                         loc);
            }

            do {
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz;
                    error_at("Token too long", loc);
                }
                token_buffer[sz++] = ch;
                ch = read_char(buf);
            } while (isxdigit(ch));

        } else if (token_buffer[0] == '0' && ((ch | 32) == 'b')) {
            /* Binary literal: 0b or 0B */
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz;
                error_at("Token too long", loc);
            }
            token_buffer[sz++] = ch;

            ch = read_char(buf);
            if (ch != '0' && ch != '1') {
                loc->len = 3;
                error_at("Binary literal expects 0 or 1 after 0b", loc);
            }

            do {
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz;
                    error_at("Token too long", loc);
                }
                token_buffer[sz++] = ch;
                ch = read_char(buf);
            } while (ch == '0' || ch == '1');

        } else if (token_buffer[0] == '0') {
            /* Octal: starts with 0 but not followed by 'x' or 'b' */
            while (isdigit(ch)) {
                if (ch >= '8') {
                    loc->pos += sz;
                    loc->column += sz;
                    error_at("Invalid octal digit, must be in range 0-7", loc);
                }
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz;
                    error_at("Token too long", loc);
                }
                token_buffer[sz++] = ch;
                ch = read_char(buf);
            }

        } else {
            /* Decimal */
            while (isdigit(ch)) {
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz;
                    error_at("Token too long", loc);
                }
                token_buffer[sz++] = ch;
                ch = read_char(buf);
            }
        }

        token_buffer[sz] = '\0';
        token = new_token(T_numeric, loc, sz);
        token->literal = intern_string(token_buffer);
        loc->column += sz;
        return token;
    }

    if (ch == '(') {
        ch = read_char(buf);
        token = new_token(T_open_bracket, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ')') {
        ch = read_char(buf);
        token = new_token(T_close_bracket, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '{') {
        ch = read_char(buf);
        token = new_token(T_open_curly, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '}') {
        ch = read_char(buf);
        token = new_token(T_close_curly, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '[') {
        ch = read_char(buf);
        token = new_token(T_open_square, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ']') {
        ch = read_char(buf);
        token = new_token(T_close_square, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ',') {
        ch = read_char(buf);
        token = new_token(T_comma, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '^') {
        ch = read_char(buf);

        if (ch == '=') {
            ch = read_char(buf);
            token = new_token(T_xoreq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_bit_xor, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '~') {
        ch = read_char(buf);
        token = new_token(T_bit_not, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '"') {
        int sz = 0;
        bool special = false;

        ch = read_char(buf);
        while (ch != '"' || special) {
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz + 1;
                error_at("String literal too long", loc);
            }
            token_buffer[sz++] = ch;

            if (ch == '\\')
                special = true;
            else
                special = false;

            ch = read_char(buf);
        }
        token_buffer[sz] = '\0';

        read_char(buf);
        token = new_token(T_string, loc, sz + 2);
        token->literal = intern_string(token_buffer);
        loc->column += sz + 2;
        return token;
    }

    if (ch == '\'') {
        int sz = 0;
        bool escaped = false;

        ch = read_char(buf);
        if (ch == '\\') {
            token_buffer[sz++] = ch;
            ch = read_char(buf);

            do {
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz + 1;
                    error_at("Character literal too long", loc);
                }
                token_buffer[sz++] = ch;
                ch = read_char(buf);
                escaped = true;
            } while (ch && ch != '\'');
        } else {
            token_buffer[sz++] = ch;
        }
        token_buffer[sz] = '\0';

        if (!escaped)
            ch = read_char(buf);

        if (ch != '\'') {
            loc->len = 2;
            error_at("Unenclosed character literal", loc);
        }

        read_char(buf);
        token = new_token(T_char, loc, sz + 2);
        token->literal = intern_string(token_buffer);
        loc->column += sz + 2;
        return token;
    }

    if (ch == '*') {
        ch = read_char(buf);

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_asteriskeq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_asterisk, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '&') {
        ch = read_char(buf);

        if (ch == '&') {
            read_char(buf);
            token = new_token(T_log_and, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_andeq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_ampersand, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '|') {
        ch = read_char(buf);

        if (ch == '|') {
            read_char(buf);
            token = new_token(T_log_or, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_oreq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_bit_or, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '<') {
        ch = read_char(buf);

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_le, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '<') {
            ch = read_char(buf);

            if (ch == '=') {
                read_char(buf);
                token = new_token(T_lshifteq, loc, 3);
                loc->column += 3;
                return token;
            }

            token = new_token(T_lshift, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_lt, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '%') {
        ch = read_char(buf);

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_modeq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_mod, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '>') {
        ch = read_char(buf);

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_ge, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '>') {
            ch = read_char(buf);

            if (ch == '=') {
                read_char(buf);
                token = new_token(T_rshifteq, loc, 3);
                loc->column += 3;
                return token;
            }

            token = new_token(T_rshift, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_gt, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '!') {
        ch = read_char(buf);

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_noteq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_log_not, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '.') {
        ch = read_char(buf);

        if (ch == '.' && peek_char(buf, 1) == '.') {
            buf->size += 2;
            token = new_token(T_elipsis, loc, 3);
            loc->column += 3;
            return token;
        }

        token = new_token(T_dot, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '-') {
        ch = read_char(buf);

        if (ch == '>') {
            read_char(buf);
            token = new_token(T_arrow, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '-') {
            read_char(buf);
            token = new_token(T_decrement, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_minuseq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_minus, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '+') {
        ch = read_char(buf);

        if (ch == '+') {
            read_char(buf);
            token = new_token(T_increment, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_pluseq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_plus, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ';') {
        read_char(buf);
        token = new_token(T_semicolon, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '?') {
        read_char(buf);
        token = new_token(T_question, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ':') {
        read_char(buf);
        token = new_token(T_colon, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '=') {
        ch = read_char(buf);

        if (ch == '=') {
            read_char(buf);
            token = new_token(T_eq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_assign, loc, 1);
        loc->column++;
        return token;
    }

    if (isalnum(ch) || ch == '_') {
        int sz = 0;
        do {
            /* Bounded by the smallest buffer an identifier is ever copied into,
             * not by the token buffer's own size: lex_ident() and lex_peek()
             * strcpy into caller arrays of MAX_ID_LEN, so a longer name would
             * run off the end of one. Diagnosing it here is what keeps a long
             * identifier in the input from corrupting the compiler's stack.
             */
            if (sz >= MAX_ID_LEN - 1) {
                loc->len = sz;
                error_at("Identifier too long", loc);
            }
            token_buffer[sz++] = ch;
            ch = read_char(buf);
        } while (isalnum(ch) || ch == '_');
        token_buffer[sz] = 0;

        /* Fast path for common keywords - avoid hashmap lookup */
        token_kind_t kind = T_identifier;

        /* Check most common keywords inline based on token length and first
         * character.
         */
        switch (sz) {
        case 2: /* 2-letter keywords: if, do */
            if (token_buffer[0] == 'i' && token_buffer[1] == 'f')
                kind = T_if;
            else if (token_buffer[0] == 'd' && token_buffer[1] == 'o')
                kind = T_do;
            break;

        case 3: /* 3-letter keywords: for */
            if (token_buffer[0] == 'f' && token_buffer[1] == 'o' &&
                token_buffer[2] == 'r')
                kind = T_for;
            break;

        case 4: /* 4-letter keywords: else, enum, case */
            if (token_buffer[0] == 'e') {
                if (!memcmp(token_buffer, "else", 4))
                    kind = T_else;
                else if (!memcmp(token_buffer, "enum", 4))
                    kind = T_enum;
            } else if (!memcmp(token_buffer, "case", 4))
                kind = T_case;
            else if (!memcmp(token_buffer, "goto", 4))
                kind = T_goto;
            break;

        case 5: /* 5-letter keywords: while, break, union, const */
            if (token_buffer[0] == 'w' && !memcmp(token_buffer, "while", 5))
                kind = T_while;
            else if (token_buffer[0] == 'b' &&
                     !memcmp(token_buffer, "break", 5))
                kind = T_break;
            else if (token_buffer[0] == 'u' &&
                     !memcmp(token_buffer, "union", 5))
                kind = T_union;
            else if (token_buffer[0] == 'c' &&
                     !memcmp(token_buffer, "const", 5))
                kind = T_const;
            break;

        case 6: /* 6-letter keywords: return, struct, switch, sizeof */
            if (token_buffer[0] == 'r' && !memcmp(token_buffer, "return", 6))
                kind = T_return;
            else if (token_buffer[0] == 's') {
                if (!memcmp(token_buffer, "struct", 6))
                    kind = T_struct;
                else if (!memcmp(token_buffer, "switch", 6))
                    kind = T_switch;
                else if (!memcmp(token_buffer, "sizeof", 6))
                    kind = T_sizeof;
            }
            break;

        case 7: /* 7-letter keywords: typedef, default */
            if (!memcmp(token_buffer, "typedef", 7))
                kind = T_typedef;
            else if (!memcmp(token_buffer, "default", 7))
                kind = T_default;
            break;

        case 8: /* 8-letter keywords: continue */
            if (!memcmp(token_buffer, "continue", 8))
                kind = T_continue;
            break;

        default:
            /* Keywords longer than 8 chars or identifiers - use hashmap */
            break;
        }

        /* Fall back to the hashmap for anything the switch does not name. No
         * keyword is shorter than two characters or longer than eight, so a
         * name outside that range cannot be one and needs no lookup -- which is
         * most of the identifiers in a real program.
         */
        if (kind == T_identifier && sz >= 2 && sz <= 8)
            kind = lookup_keyword(token_buffer);

        token = new_token(kind, loc, sz);
        token->literal = intern_string(token_buffer);
        loc->column += sz;
        return token;
    }

    error_at("Unexpected token", loc);
    return NULL;
}

token_stream_t *gen_file_token_stream(char *filename)
{
    /* FIXME: We should normalize filename first to make cache works as expected
     */

    token_t head;
    token_t *cur = &head;
    token_stream_t *tks;

    /* initialie source location with the following configuration: pos is at 0,
     * len is 1 for reporting convenience, and the column and line number are
     * set to 1.
     */
    source_location_t loc = {0, 1, 1, 1, filename};
    strbuf_t *buf;

    tks = hashmap_get(TOKEN_CACHE, filename);

    /* Already cached, just return the computed token stream */
    if (tks)
        return tks;

    buf = get_file_buf(filename);

    /* Borrows strbuf_t#size to use as source index */
    buf->size = 0;

    while (buf->size < buf->capacity) {
        cur->next = lex_token(buf, &loc);
        cur = cur->next;

        if (cur->kind == T_eof)
            break;
    }

    if (!head.next) {
        head.next = arena_calloc(TOKEN_ARENA, 1, sizeof(token_t));
        head.next->kind = T_eof;
        memcpy(&head.next->location, &loc, sizeof(source_location_t));
        cur = head.next;
    }

    if (cur->kind != T_eof)
        error_at("Internal error, expected eof at the end of file",
                 &cur->location);

    tks = malloc(sizeof(token_stream_t));
    tks->head = head.next;
    tks->tail = cur;
    hashmap_put(TOKEN_CACHE, filename, tks);
    return tks;
}

token_stream_t *gen_libc_token_stream()
{
    token_t head;
    token_t *cur = &head, *tk;
    token_stream_t *tks;
    char *filename = dynlink ? "lib/c.h" : "lib/c.c";
    strbuf_t *buf = LIBC_SRC;
    source_location_t loc = {0, 1, 1, 1, filename};

    tks = hashmap_get(TOKEN_CACHE, filename);

    if (tks)
        return tks;

    if (!hashmap_contains(SRC_FILE_MAP, filename))
        hashmap_put(SRC_FILE_MAP, filename, LIBC_SRC);

    /* This buffer was built by appending, so its capacity is whatever the
     * doubling left and runs past the text into memory that was never written
     * -- while the scan below, like the one over a file, stops at capacity.
     * Terminate it the way read_file() leaves a file: the text, a NUL, and
     * capacity naming one past the text. Without this the lexer reads
     * uninitialised bytes, and what it finds there depends on the allocator,
     * which is enough to make the compiler emit different code from one build
     * to the next.
     */
    if (!buf->size || buf->elements[buf->size - 1])
        strbuf_putc(buf, 0);
    buf->capacity = buf->size;

    /* Borrows strbuf_t#size to use as source index */
    buf->size = 0;

    while (buf->size < buf->capacity) {
        tk = lex_token(buf, &loc);

        /* Early break to discard eof token, so later we can concat libc token
         * stream with actual input file's token stream.
         */
        if (tk->kind == T_eof)
            break;

        cur->next = tk;
        cur = cur->next;
    }

    if (!head.next)
        fatal("Unable to include libc");

    if (tk->kind != T_eof)
        error_at("Internal error, expected eof at the end of file",
                 &cur->location);

    tks = malloc(sizeof(token_stream_t));
    tks->head = head.next;
    tks->tail = cur;
    hashmap_put(TOKEN_CACHE, filename, tks);
    return tks;
}

/* Fetches current token's location. */
source_location_t *cur_token_loc()
{
    return &cur_token->location;
}

/* Finds next token's location; if the current token is eof, returns the eof
 * token's location instead.
 */
source_location_t *next_token_loc()
{
    if (cur_token->kind == T_eof)
        return &cur_token->location;

    return &cur_token->next->location;
}

/* Lex next token with aliasing enabled */
token_kind_t lex_next(void)
{
    /* if reached eof, we always return eof token to avoid any advancement */
    if (cur_token->kind == T_eof)
        return T_eof;

    cur_token = cur_token->next;
    return cur_token->kind;
}

/* Accepts next token if token types are matched. */
bool lex_accept(token_kind_t kind)
{
    if (cur_token->next && cur_token->next->kind == kind) {
        lex_next();
        return true;
    }
    return false;
}

/* Peeks next token and copy token's literal to value if token types are
 * matched.
 */
bool lex_peek(token_kind_t kind, char *value)
{
    if (cur_token->next && cur_token->next->kind == kind) {
        if (!value)
            return true;
        strcpy(value, cur_token->next->literal);
        return true;
    }
    return false;
}

/* Copies a token literal into a caller buffer of n bytes. Identifiers are
 * bounded by MAX_ID_LEN when scanned, but some destinations are narrower --
 * 'type_name' is only MAX_TYPE_LEN -- so the bound has to travel with the
 * destination rather than be assumed from the source.
 */
void lex_copy_literal(token_t *tk, char *value, int n)
{
    int len = strlen(tk->literal);

    if (len >= n)
        error_at("Identifier too long", &tk->location);
    strcpy(value, tk->literal);
}

/* Peek the next token and, when it matches, copy its literal into a caller
 * buffer of n bytes. lex_peek() copies unbounded, which is safe only for
 * identifiers: numeric, char and string literals are scanned up to
 * MAX_TOKEN_LEN and overflow a MAX_ID_LEN destination.
 */
bool lex_peek_n(token_kind_t token, char *value, int n)
{
    if (!cur_token->next || cur_token->next->kind != token)
        return false;
    if (value)
        lex_copy_literal(cur_token->next, value, n);
    return true;
}

/* Strictly match next token with given token type and copy token's literal to
 * value, which is n bytes wide.
 */
void lex_ident_n(token_kind_t token, char *value, int n)
{
    if (cur_token->next && cur_token->next->kind == token) {
        lex_next();
        if (value)
            lex_copy_literal(cur_token, value, n);
        return;
    }
    token_t *tk = cur_token->next ? cur_token->next : cur_token;
    error_at("Unexpected token", &tk->location);
}

/* Strictly match next token with given token type and copy token's literal to
 * value.
 */
void lex_ident(token_kind_t token, char *value)
{
    if (cur_token->next && cur_token->next->kind == token) {
        lex_next();
        if (value)
            strcpy(value, cur_token->literal);
        return;
    }
    token_t *tk = cur_token->next ? cur_token->next : cur_token;
    error_at("Unexpected token", &tk->location);
}

/* Strictly match next token with given token type. */
void lex_expect(token_kind_t token)
{
    if (cur_token->next && cur_token->next->kind == token) {
        lex_next();
        return;
    }
    token_t *tk = cur_token->next ? cur_token->next : cur_token;
    error_at("Unexpected token", &tk->location);
}
