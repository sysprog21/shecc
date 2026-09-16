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
#define NUM_DIRECTIVES 12
#define NUM_KEYWORDS 32

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

void lex_init_directives(void)
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
        {"#undef", T_cppd_undef},     {"#line", T_cppd_line},
    };

    /* hashmap insertion */
    for (int i = 0; i < NUM_DIRECTIVES; i++) {
        directive_tokens_storage[i] = directives[i].token;
        hashmap_put(DIRECTIVE_MAP, directives[i].name,
                    &directive_tokens_storage[i]);
    }
}

void lex_init_keywords(void)
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
        {"volatile", T_volatile},
        {"static", T_static},
        {"extern", T_extern},
        {"register", T_register},
        {"auto", T_auto},
        {"restrict", T_restrict},
        {"inline", T_inline},
        {"signed", T_signed},
        {"unsigned", T_unsigned},
        {"long", T_long},
        {"float", T_float},
        {"double", T_double},
        {"_Complex", T_complex},
        {"_Imaginary", T_imaginary},
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
void lexer_cleanup(void)
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

/* C99 translation phase 1 replaces trigraphs before every later lexical
 * decision. Keep the source buffer immutable so locations and quoted-include
 * recovery retain physical offsets; this view maps one logical character to
 * either one physical byte or a three-byte trigraph spelling.
 */
char trigraph_char_at(strbuf_t *buf, int pos)
{
    char third;

    if (buf->plain_source || pos + 2 >= buf->capacity ||
        buf->elements[pos] != '?' || buf->elements[pos + 1] != '?')
        return '\0';
    third = buf->elements[pos + 2];
    switch (third) {
    case '=':
        return '#';
    case '/':
        return '\\';
    case '\'':
        return '^';
    case '(':
        return '[';
    case ')':
        return ']';
    case '!':
        return '|';
    case '<':
        return '{';
    case '>':
        return '}';
    case '-':
        return '~';
    default:
        return '\0';
    }
}

int source_char_width(strbuf_t *buf, int pos)
{
    return trigraph_char_at(buf, pos) ? 3 : 1;
}

char source_char_at(strbuf_t *buf, int pos)
{
    char replacement;

    if (pos >= buf->capacity)
        return '\0';
    replacement = trigraph_char_at(buf, pos);
    return replacement ? replacement : buf->elements[pos];
}

/* Phase 2 operates on the phase-1 trigraph view. Preserve physical source
 * bytes, but skip every logical backslash/newline pair before token formation.
 */
int skip_splices(strbuf_t *buf, int pos)
{
    int width;

    if (buf->plain_source)
        return pos;
    while (pos < buf->capacity) {
        width = source_char_width(buf, pos);
        if (source_char_at(buf, pos) != '\\' ||
            source_char_at(buf, pos + width) != '\n')
            break;
        pos += width + 1;
    }
    return pos;
}

char peek_char(strbuf_t *buf, int offset)
{
    int pos = buf->size;

    if (buf->plain_source) {
        if (pos < buf->capacity) {
            pos += offset;
            if (pos > buf->capacity)
                pos = buf->capacity;
        }
        return pos >= buf->capacity ? '\0' : buf->elements[pos];
    }

    pos = skip_splices(buf, pos);

    while (offset-- > 0 && pos < buf->capacity) {
        pos += source_char_width(buf, pos);
        pos = skip_splices(buf, pos);
    }
    return source_char_at(buf, pos);
}

char read_char(strbuf_t *buf)
{
    int pos = buf->size;

    if (buf->plain_source) {
        if (pos + 1 >= buf->capacity)
            return buf->elements[buf->capacity - 1];
        buf->size = pos + 1;
        return buf->elements[buf->size];
    }

    pos = skip_splices(buf, pos);

    if (pos + source_char_width(buf, pos) >= buf->capacity)
        return source_char_at(buf, buf->capacity - 1);
    pos += source_char_width(buf, pos);
    buf->size = skip_splices(buf, pos);
    return source_char_at(buf, buf->size);
}

/* Translation phases 1 and 2 are the identity on a source with no "??" and no
 * backslash-newline, which is nearly every file. Check that once per buffer so
 * the per-character readers above can index its bytes directly instead of
 * testing each position for a trigraph and a splice.
 */
void classify_plain_source(strbuf_t *buf)
{
    buf->plain_source = true;
    for (int i = 0; i + 1 < buf->capacity; i++) {
        char c = buf->elements[i];

        if ((c == '?' && buf->elements[i + 1] == '?') ||
            (c == '\\' && buf->elements[i + 1] == '\n')) {
            buf->plain_source = false;
            return;
        }
    }
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

strbuf_t *read_file(const char *filename)
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

token_t *new_token(token_kind_t kind, const source_location_t *loc, int len)
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

/* A '#' opens a directive only when nothing but white space precedes it on its
 * logical line. The physical column cannot tell: a backslash-newline puts a '#'
 * at column 1 of the next physical line while phase 2 has already joined it to
 * the text before. So every token lex_token() returns updates this instead.
 */
bool lex_at_line_start = true;

/* Skipping a comment or a run of whitespace resumes the scan, and lex_layout()
 * below does that by starting a fresh token here.
 */
token_t *lex_token(strbuf_t *buf, source_location_t *loc);
char read_layout_char(strbuf_t *buf, source_location_t *loc);

/* Readers consume logical characters, but diagnostics index immutable physical
 * source bytes. The source span is finalized at each lex_token() exit; readers
 * which cross a newline update the logical cursor themselves.
 */
#define RETURN_LEX_TOKEN(tk)                                    \
    do {                                                        \
        int pos, line, column;                                  \
        tk->location.len = buf->size - tk->location.pos;        \
        pos = tk->location.pos;                                 \
        line = tk->location.line;                               \
        column = tk->location.column;                           \
        while (pos < buf->size) {                               \
            if (buf->elements[pos] == '\n') {                   \
                line++;                                         \
                column = 1;                                     \
            } else                                              \
                column++;                                       \
            pos++;                                              \
        }                                                       \
        loc->line = line;                                       \
        loc->column = column;                                   \
        if (tk->kind == T_newline)                              \
            lex_at_line_start = true;                           \
        else if (tk->kind != T_whitespace && tk->kind != T_tab) \
            lex_at_line_start = false;                          \
        return tk;                                              \
    } while (0)

/* Preprocessor directives, comments, and the whitespace between tokens.
 *
 * Returns NULL when 'ch' is none of its business, so that lex_token() can offer
 * the character to the next reader in line.
 */
token_t *lex_layout(strbuf_t *buf, source_location_t *loc, char ch)
{
    token_t *token;
    char token_buffer[MAX_TOKEN_LEN];

    if (ch == '#' || (ch == '%' && peek_char(buf, 1) == ':')) {
        bool is_digraph_hash = ch == '%';
        int hash_len = is_digraph_hash ? 2 : source_char_width(buf, buf->size);

        /* Inside a macro replacement list '#' stringifies the parameter that
         * follows and '##' pastes its neighbours. Neither can be a directive,
         * which only exists at the start of a line.
         */
        if ((ch == '#' && peek_char(buf, 1) == '#') ||
            (ch == '%' && peek_char(buf, 1) == ':' &&
             peek_char(buf, 2) == '%' && peek_char(buf, 3) == ':')) {
            int paste_chars = ch == '#' ? 2 : 4;
            int paste_len = 0;

            for (int i = 0; i < paste_chars; i++) {
                paste_len += source_char_width(buf, buf->size);
                read_char(buf);
            }
            token = new_token(T_hashhash, loc, paste_len);
            loc->column += paste_len;
            return token;
        }

        if (!lex_at_line_start) {
            int hash_chars = is_digraph_hash ? 2 : 1;

            for (int i = 0; i < hash_chars; i++)
                read_char(buf);
            token = new_token(T_hash, loc, hash_len);
            loc->column += hash_len;
            return token;
        }

        int sz = 0, source_len = hash_len;

        token_buffer[sz++] = '#';
        int hash_chars = is_digraph_hash ? 2 : 1;
        for (int i = 0; i < hash_chars; i++)
            ch = read_char(buf);

        /* White space may separate '#' from the directive name (C99 6.10p2),
         * and a comment is white space by then. RETURN_LEX_TOKEN recounts the
         * span from the source, so a comment across lines needs no care here.
         */
        for (;;) {
            if (ch == ' ' || ch == '\t') {
                ch = read_char(buf);
                continue;
            }
            if (ch == '/' && peek_char(buf, 1) == '*') {
                read_char(buf);
                ch = read_char(buf);
                while (ch && !(ch == '*' && peek_char(buf, 1) == '/'))
                    ch = read_char(buf);
                if (!ch)
                    error_at("Unenclosed C-style comment", loc);
                read_char(buf);
                ch = read_char(buf);
                continue;
            }
            break;
        }

        /* The null directive, a '#' alone on its line (C99 6.10.7), does
         * nothing. What remains of the line is layout, and a line comment is
         * left to be lexed as one.
         */
        if (ch == '\n' || ch == '\0' || (ch == '/' && peek_char(buf, 1) == '/'))
            return new_token(T_whitespace, loc, 1);

        while (isalnum(ch) || ch == '_') {
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz;
                error_at("Token too long", loc);
            }
            token_buffer[sz++] = ch;
            ch = read_char(buf);
            source_len++;
        }
        token_buffer[sz] = '\0';

        /* Whether a name shecc does not know is an error depends on the group
         * it sits in: a skipped group may hold any line that starts with '#'.
         * The preprocessor decides once it knows which group that is.
         */
        token_kind_t directive_kind = lookup_directive(token_buffer);
        if (directive_kind == T_identifier)
            directive_kind = T_cppd_unknown;

        token = new_token(directive_kind, loc, source_len);
        loc->column += source_len;
        return token;
    }

    /* Leave a UCN to lex_word(); an ordinary backslash remains available for
     * the preprocessor's line-splice handling.
     */
    if (ch == '\\' && peek_char(buf, 1) != 'u' && peek_char(buf, 1) != 'U') {
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
            loc->column += source_char_width(buf, loc->pos);
            loc->column += source_char_width(buf, buf->size);
            read_layout_char(buf, loc);
            while (peek_char(buf, 0)) {
                ch = peek_char(buf, 0);
                if (ch == '*' && peek_char(buf, 1) == '/') {
                    loc->column += source_char_width(buf, buf->size);
                    read_layout_char(buf, loc);
                    loc->column += source_char_width(buf, buf->size);
                    read_layout_char(buf, loc);
                    return lex_token(buf, loc);
                }
                if (ch == '\n') {
                    read_layout_char(buf, loc);
                    continue;
                }
                loc->column += source_char_width(buf, buf->size);
                read_layout_char(buf, loc);
            }

            error_at("Unenclosed C-style comment", loc);
            return NULL;
        }

        if (ch == '/') {
            /* C++-style comment */
            loc->column += source_char_width(buf, loc->pos);
            loc->column += source_char_width(buf, buf->size);
            read_layout_char(buf, loc);
            while (peek_char(buf, 0) && peek_char(buf, 0) != '\n') {
                loc->column += source_char_width(buf, buf->size);
                read_layout_char(buf, loc);
            }
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

    return NULL;
}

/* Append @ch to the numeric literal spelled so far in @token_buffer, reporting
 * a literal too long for the buffer.
 */
static int number_append(char token_buffer[],
                         int sz,
                         char ch,
                         source_location_t *loc)
{
    if (sz >= MAX_TOKEN_LEN - 1) {
        loc->len = sz;
        error_at("Token too long", loc);
    }
    token_buffer[sz] = ch;
    return sz + 1;
}

/* Integer literals, in every base the language accepts.
 *
 * Returns NULL when 'ch' is none of its business, so that lex_token() can offer
 * the character to the next reader in line.
 */
token_t *lex_number(strbuf_t *buf, source_location_t *loc, char ch)
{
    token_t *token;
    char token_buffer[MAX_TOKEN_LEN];

    if (isdigit(ch) || (ch == '.' && isdigit(peek_char(buf, 1)))) {
        int sz = 0;
        bool is_floating = ch == '.';
        bool is_hex = false;
        bool has_hex_exponent = false;
        bool has_hex_significand = false;
        if (is_floating) {
            token_buffer[sz++] = ch;
            ch = read_char(buf);
            while (isdigit(ch)) {
                sz = number_append(token_buffer, sz, ch, loc);
                ch = read_char(buf);
            }
        } else {
            token_buffer[sz++] = ch;
            ch = read_char(buf);
        }

        if (!is_floating && token_buffer[0] == '0' && ((ch | 32) == 'x')) {
            /* Hexadecimal: starts with 0x or 0X */
            is_hex = true;
            sz = number_append(token_buffer, sz, ch, loc);

            ch = read_char(buf);

            /* C99 also permits the first hexadecimal significand digit after
             * the point (`0x.8p2`), so defer the nonempty-significand check
             * until the floating spelling has been recognized.
             */
            while (isxdigit(ch)) {
                sz = number_append(token_buffer, sz, ch, loc);
                has_hex_significand = true;
                ch = read_char(buf);
            }

        } else if (!is_floating && token_buffer[0] == '0' &&
                   ((ch | 32) == 'b')) {
            /* Binary literal: 0b or 0B */
            if (strict_c99)
                error_at("binary literals are a GNU extension in C99", loc);
            sz = number_append(token_buffer, sz, ch, loc);

            ch = read_char(buf);
            if (ch != '0' && ch != '1') {
                loc->len = 3;
                error_at("Binary literal expects 0 or 1 after 0b", loc);
            }

            do {
                sz = number_append(token_buffer, sz, ch, loc);
                ch = read_char(buf);
            } while (ch == '0' || ch == '1');

        } else if (!is_floating && token_buffer[0] == '0') {
            /* Octal: starts with 0 but not followed by 'x' or 'b' */
            while (isdigit(ch)) {
                sz = number_append(token_buffer, sz, ch, loc);
                ch = read_char(buf);
            }

        } else if (!is_floating) {
            /* Decimal */
            while (isdigit(ch)) {
                sz = number_append(token_buffer, sz, ch, loc);
                ch = read_char(buf);
            }
        }

        /* Decimal floating forms are admitted lexically now. They retain the
         * original spelling as payload; semantic float types/lowering remain
         * deliberately outside this lexer stage.
         */
        if (ch == '.') {
            is_floating = true;
            sz = number_append(token_buffer, sz, ch, loc);
            ch = read_char(buf);
            if (is_hex) {
                while (isxdigit(ch)) {
                    sz = number_append(token_buffer, sz, ch, loc);
                    has_hex_significand = true;
                    ch = read_char(buf);
                }
            } else {
                while (isdigit(ch)) {
                    sz = number_append(token_buffer, sz, ch, loc);
                    ch = read_char(buf);
                }
            }
        }
        if ((!is_hex && (ch | 32) == 'e') || (is_hex && (ch | 32) == 'p')) {
            is_floating = true;
            if (is_hex)
                has_hex_exponent = true;
            sz = number_append(token_buffer, sz, ch, loc);
            ch = read_char(buf);
            if (ch == '+' || ch == '-') {
                sz = number_append(token_buffer, sz, ch, loc);
                ch = read_char(buf);
            }
            if (!isdigit(ch))
                error_at("Floating literal needs an exponent", loc);
            do {
                sz = number_append(token_buffer, sz, ch, loc);
                ch = read_char(buf);
            } while (isdigit(ch));
        }
        if (is_floating) {
            if (is_hex && !has_hex_exponent)
                error_at("Hexadecimal floating literal needs a p exponent",
                         loc);
            if (is_hex && !has_hex_significand)
                error_at("Hexadecimal floating literal needs a significand",
                         loc);
            if ((ch | 32) == 'f' || (ch | 32) == 'l') {
                sz = number_append(token_buffer, sz, ch, loc);
                ch = read_char(buf);
            }
            token_buffer[sz] = '\0';
            token = new_token(T_floating, loc, sz);
            token->literal = intern_string(token_buffer);
            loc->column += sz;
            return token;
        }

        /* The floating path above admits a significand digit after the point,
         * so only an integer spelling can still lack one here.
         */
        if (is_hex && !has_hex_significand) {
            loc->len = sz;
            error_at("Invalid hex literal: expected hex digit after 0x", loc);
        }
        if (!is_hex && token_buffer[0] == '0') {
            for (int i = 1; i < sz; i++) {
                if (token_buffer[i] >= '8' && token_buffer[i] <= '9') {
                    loc->pos += i;
                    loc->column += i;
                    error_at("Invalid octal digit, must be in range 0-7", loc);
                }
            }
        }
        if ((ch | 32) == 'p')
            error_at("Hexadecimal floating literal needs a significand", loc);

        /* C99 integer suffixes belong to the numeric token rather than starting
         * an adjacent identifier. The existing `long` spelling has the same
         * 32-bit representation as int. Keep a double-long suffix for the
         * parser, which selects the distinct 64-bit type on targets that can
         * lower it.
         */
        bool has_unsigned_suffix = false;
        int long_suffix_count = 0;
        while ((ch | 32) == 'u' || (ch | 32) == 'l') {
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz;
                error_at("Token too long", loc);
            }
            if ((ch | 32) == 'u') {
                if (has_unsigned_suffix)
                    error_at("Invalid integer literal suffix", loc);
                has_unsigned_suffix = true;
            } else {
                long_suffix_count++;
                if (long_suffix_count > 2)
                    error_at("Invalid integer literal suffix", loc);
            }
            token_buffer[sz++] = ch;
            ch = read_char(buf);
        }
        token_buffer[sz] = '\0';
        token = new_token(T_numeric, loc, sz);
        token->literal = intern_string(token_buffer);
        loc->column += sz;
        return token;
    }

    return NULL;
}

/* String and character literals.
 *
 * Returns NULL when 'ch' is none of its business, so that lex_token() can offer
 * the character to the next reader in line.
 */
token_t *lex_literal(strbuf_t *buf, source_location_t *loc, char ch)
{
    token_t *token;
    char token_buffer[MAX_TOKEN_LEN];

    if (ch == '"') {
        int sz = 0;
        bool special = false;

        /* read_char() has already removed every backslash-newline, so a newline
         * still here ends the line inside the literal, which neither an s-char
         * nor an escape may do (C99 6.4.5).
         */
        ch = read_char(buf);
        while (ch != '"' || special) {
            if (ch == '\n' || !ch) {
                loc->len = 1;
                error_at("Unenclosed string literal", loc);
            }
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz + 1;
                error_at("String literal too long", loc);
            }
            token_buffer[sz++] = ch;

            /* A backslash escapes the character after it, and an escaped
             * backslash escapes nothing further, so "\\" still ends at its
             * closing quote.
             */
            special = ch == '\\' && !special;

            ch = read_char(buf);
        }
        token_buffer[sz] = '\0';

        /* Validate every escape while the literal is still represented by one
         * token. Some later string-initializer paths only need its decoded
         * bytes and historically did not inspect unescape_string()'s status.
         */
        char unescaped[MAX_TOKEN_LEN];
        if (unescape_string(token_buffer, unescaped, sizeof(unescaped)) < 0)
            error_at("Invalid escape sequence", loc);

        read_char(buf);
        token = new_token(T_string, loc, sz + 2);
        token->literal = intern_string(token_buffer);
        loc->column += sz + 2;
        return token;
    }

    if (ch == '\'') {
        int sz = 0;

        /* As in a string literal, a newline is never part of the constant. */
        ch = read_char(buf);
        while (ch && ch != '\'' && ch != '\n') {
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz + 1;
                error_at("Character literal too long", loc);
            }
            token_buffer[sz++] = ch;
            if (ch == '\\') {
                ch = read_char(buf);
                if (!ch || ch == '\n')
                    break;
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz + 1;
                    error_at("Character literal too long", loc);
                }
                token_buffer[sz++] = ch;
            }
            ch = read_char(buf);
        }
        token_buffer[sz] = '\0';

        if (ch != '\'') {
            loc->len = 2;
            error_at("Unenclosed character literal", loc);
        }
        if (!sz) {
            loc->len = 2;
            error_at("Empty character constant", loc);
        }

        char unescaped[MAX_TOKEN_LEN];
        if (unescape_string(token_buffer, unescaped, sizeof(unescaped)) < 0)
            error_at("Invalid escape sequence", loc);

        read_char(buf);
        token = new_token(T_char, loc, sz + 2);
        token->literal = intern_string(token_buffer);
        loc->column += sz + 2;
        return token;
    }

    return NULL;
}

/* Punctuation that is never the start of a longer token.
 *
 * Returns NULL when 'ch' is none of its business, so that lex_token() can offer
 * the character to the next reader in line.
 */
token_t *lex_punct(strbuf_t *buf, source_location_t *loc, char ch)
{
    token_t *token;

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

    if (ch == '<' && peek_char(buf, 1) == '%') {
        read_char(buf);
        read_char(buf);
        token = new_token(T_open_curly, loc, 2);
        loc->column += 2;
        return token;
    }

    if (ch == '}') {
        ch = read_char(buf);
        token = new_token(T_close_curly, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '%' && peek_char(buf, 1) == '>') {
        read_char(buf);
        read_char(buf);
        token = new_token(T_close_curly, loc, 2);
        loc->column += 2;
        return token;
    }

    if (ch == '[') {
        ch = read_char(buf);
        token = new_token(T_open_square, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '<' && peek_char(buf, 1) == ':') {
        read_char(buf);
        read_char(buf);
        token = new_token(T_open_square, loc, 2);
        loc->column += 2;
        return token;
    }

    if (ch == ']') {
        ch = read_char(buf);
        token = new_token(T_close_square, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ':' && peek_char(buf, 1) == '>') {
        read_char(buf);
        read_char(buf);
        token = new_token(T_close_square, loc, 2);
        loc->column += 2;
        return token;
    }

    if (ch == ',') {
        ch = read_char(buf);
        token = new_token(T_comma, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '~') {
        ch = read_char(buf);
        token = new_token(T_bit_not, loc, 1);
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

    return NULL;
}

/* Operators, each of which may or may not continue into a longer one.
 *
 * Returns NULL when 'ch' is none of its business, so that lex_token() can offer
 * the character to the next reader in line.
 */
token_t *lex_operator(strbuf_t *buf, source_location_t *loc, char ch)
{
    token_t *token;

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

    return NULL;
}

/* UCNs in identifiers are lexical spellings, rather than string escapes: the
 * token keeps their UTF-8 spelling while its source location counts the raw
 * six- or ten-byte `\\u`/`\\U` sequence. Keep this decoder here and
 * deliberately leaf-sized. Calling the general literal unescaper from
 * lex_word() made the self-hosted compiler's stage-1 build fail before it
 * reached user input.
 */
static bool lex_ucn_starts(strbuf_t *buf)
{
    return peek_char(buf, 0) == '\\' &&
           (peek_char(buf, 1) == 'u' || peek_char((strbuf_t *) buf, 1) == 'U');
}

static int lex_ucn_identifier(char *out, int out_size, strbuf_t *buf)
{
    int digits = peek_char(buf, 1) == 'u' ? 4 : 8;
    unsigned int value = 0;
    int out_len;

    for (int i = 0; i < digits; i++) {
        int digit = hex_digit_value(peek_char(buf, i + 2));

        if (digit < 0)
            return -1;
        value = (value << 4) | digit;
    }

    /* C99 6.4.3 forbids surrogates, out-of-range scalars, and a UCN spelling of
     * a basic-source character except $, @, and `. Those three are still
     * grammar-level UCN nondigits, even though their direct spellings are not
     * ordinary identifier characters.
     */
    if ((value < 0xa0 && value != '$' && value != '@' && value != '`') ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return -1;

    if (value <= 0x7f)
        out_len = 1;
    else if (value <= 0x7ff)
        out_len = 2;
    else if (value <= 0xffff)
        out_len = 3;
    else
        out_len = 4;
    if (out_len >= out_size)
        return -1;

    if (out_len == 1) {
        out[0] = value;
    } else if (out_len == 2) {
        out[0] = 0xc0 | (value >> 6);
        out[1] = 0x80 | (value & 0x3f);
    } else if (out_len == 3) {
        out[0] = 0xe0 | (value >> 12);
        out[1] = 0x80 | ((value >> 6) & 0x3f);
        out[2] = 0x80 | (value & 0x3f);
    } else {
        out[0] = 0xf0 | (value >> 18);
        out[1] = 0x80 | ((value >> 12) & 0x3f);
        out[2] = 0x80 | ((value >> 6) & 0x3f);
        out[3] = 0x80 | (value & 0x3f);
    }
    return out_len;
}

/* Identifiers, and the keywords spelled like them.
 *
 * Returns NULL when 'ch' is none of its business, so that lex_token() can offer
 * the character to the next reader in line.
 */
token_t *lex_word(strbuf_t *buf, source_location_t *loc, char ch)
{
    token_t *token;
    char token_buffer[MAX_TOKEN_LEN];

    if (isalnum(ch) || ch == '_' || lex_ucn_starts(buf)) {
        int sz = 0;
        int source_len = 0;
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
            if (lex_ucn_starts(buf)) {
                int ucn_len =
                    lex_ucn_identifier(token_buffer + sz, MAX_ID_LEN - sz, buf);
                int raw_len = peek_char(buf, 1) == 'u' ? 6 : 10;

                if (ucn_len < 0) {
                    loc->len = raw_len;
                    error_at("Invalid universal character name in identifier",
                             loc);
                }
                sz += ucn_len;
                source_len += raw_len;
                for (int i = 0; i < raw_len; i++)
                    read_char(buf);
                ch = peek_char(buf, 0);
            } else {
                token_buffer[sz++] = ch;
                source_len++;
                ch = read_char(buf);
            }
        } while (isalnum(ch) || ch == '_' || lex_ucn_starts(buf));
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

        case 5: /* 5-letter keywords: while, break, union, const, float */
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
            else if (token_buffer[0] == 'f' &&
                     !memcmp(token_buffer, "float", 5))
                kind = T_float;
            break;

        case 6: /* 6-letter keywords: return, struct, switch, sizeof, static,
                   extern, inline
                   */
            if (token_buffer[0] == 'r' && !memcmp(token_buffer, "return", 6))
                kind = T_return;
            else if (token_buffer[0] == 'e' &&
                     !memcmp(token_buffer, "extern", 6))
                kind = T_extern;
            else if (token_buffer[0] == 'i' &&
                     !memcmp(token_buffer, "inline", 6))
                kind = T_inline;
            else if (token_buffer[0] == 'd' &&
                     !memcmp(token_buffer, "double", 6))
                kind = T_double;
            else if (token_buffer[0] == 's') {
                if (!memcmp(token_buffer, "struct", 6))
                    kind = T_struct;
                else if (!memcmp(token_buffer, "switch", 6))
                    kind = T_switch;
                else if (!memcmp(token_buffer, "sizeof", 6))
                    kind = T_sizeof;
                else if (!memcmp(token_buffer, "static", 6))
                    kind = T_static;
            }
            break;

        case 7: /* 7-letter keywords: typedef, default */
            if (!memcmp(token_buffer, "typedef", 7))
                kind = T_typedef;
            else if (!memcmp(token_buffer, "default", 7))
                kind = T_default;
            break;

        case 8: /* 8-letter keywords: continue, register, restrict, volatile */
            if (!memcmp(token_buffer, "continue", 8))
                kind = T_continue;
            else if (!memcmp(token_buffer, "register", 8))
                kind = T_register;
            else if (!memcmp(token_buffer, "restrict", 8))
                kind = T_restrict;
            else if (!memcmp(token_buffer, "volatile", 8))
                kind = T_volatile;
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
        if (kind == T_identifier && !strcmp(token_buffer, "_Imaginary"))
            kind = T_imaginary;
        else if (kind == T_identifier && sz >= 2 && sz <= 8)
            kind = lookup_keyword(token_buffer);

        token = new_token(kind, loc, source_len);
        token->literal = intern_string(token_buffer);
        loc->column += source_len;
        return token;
    }

    return NULL;
}

/* Reads one token, dispatching on the character it starts with. Each reader
 * above claims the characters it knows and returns NULL for the rest; the order
 * of the calls matters only between lex_number() and lex_word(), which would
 * otherwise both claim a leading digit.
 */
token_t *lex_token(strbuf_t *buf, source_location_t *loc)
{
    token_t *token;
    char ch;

    while (source_char_at(buf, buf->size) == '\\' &&
           source_char_at(buf, buf->size + source_char_width(buf, buf->size)) ==
               '\n') {
        buf->size += source_char_width(buf, buf->size) + 1;
        loc->line++;
        loc->column = 1;
    }
    ch = peek_char(buf, 0);
    loc->pos = buf->size;

    token = lex_layout(buf, loc, ch);
    if (token)
        RETURN_LEX_TOKEN(token);
    token = lex_number(buf, loc, ch);
    if (token)
        RETURN_LEX_TOKEN(token);
    if (ch == 'L' && peek_char(buf, 1) == '\'') {
        /* Keep the prefix in the source span, while the literal reader owns
         * escape validation and the quoted payload. Wide strings remain a
         * separate object-layout task.
         */
        read_char(buf);
        token = lex_literal(buf, loc, '\'');
        token->kind = T_wchar;
        token->location.len++;
        loc->column++;
        RETURN_LEX_TOKEN(token);
    }
    if (ch == 'L' && peek_char(buf, 1) == '"') {
        /* Keep the prefix in the source span. The parser owns the execution
         * wide-character representation, but the lexer must preserve this as a
         * distinct literal so phase 6 can make a join with it wide.
         */
        read_char(buf);
        token = lex_literal(buf, loc, '"');
        token->kind = T_wstring;
        token->location.len++;
        loc->column++;
        RETURN_LEX_TOKEN(token);
    }
    token = lex_literal(buf, loc, ch);
    if (token) {
        /* A narrow string literal is checked once phase 6 has joined it, since
         * a wide neighbour makes the joined literal wide.
         */
        if (token->kind == T_char && hex_escape_exceeds_byte(token->literal))
            error_at("Hexadecimal escape sequence out of range",
                     &token->location);
        RETURN_LEX_TOKEN(token);
    }
    token = lex_punct(buf, loc, ch);
    if (token)
        RETURN_LEX_TOKEN(token);
    token = lex_operator(buf, loc, ch);
    if (token)
        RETURN_LEX_TOKEN(token);
    token = lex_word(buf, loc, ch);
    if (token)
        RETURN_LEX_TOKEN(token);

    error_at("Unexpected token", loc);
    return NULL;
}

#undef RETURN_LEX_TOKEN

/* read_char() hides phase-2 pairs. Comments do not produce a token for the
 * finalizer above, so account for any physical newlines it crosses here.
 */
char read_layout_char(strbuf_t *buf, source_location_t *loc)
{
    int pos = buf->size;
    char ch = read_char(buf);

    while (pos < buf->size) {
        if (buf->elements[pos] == '\n') {
            loc->line++;
            loc->column = 1;
        }
        pos++;
    }
    return ch;
}

/* Return one lexical spelling for a source path. This deliberately does not
 * call realpath(3): headers need not exist until after the preprocessor has
 * formed their name, and preserving a lexical path keeps diagnostics useful. It
 * is nevertheless important that the token cache and #pragma once see "a/./b.h"
 * and "a/x/../b.h" as the same header.
 */
static char *normalize_filename(const char *filename)
{
    char path[MAX_LINE_LEN];
    int component_start[MAX_LINE_LEN];
    bool component_is_normal[MAX_LINE_LEN];
    int component_count = 0;
    int in = 0;
    int out = 0;
    bool absolute = filename[0] == '/';

    if (strlen(filename) >= MAX_LINE_LEN)
        fatal("Source filename is too long");

    if (absolute)
        path[out++] = '/';

    while (filename[in]) {
        int start;
        int len;

        while (filename[in] == '/')
            in++;
        start = in;
        while (filename[in] && filename[in] != '/')
            in++;
        len = in - start;

        if (!len || (len == 1 && filename[start] == '.'))
            continue;

        if (len == 2 && filename[start] == '.' && filename[start + 1] == '.') {
            if (component_count && component_is_normal[component_count - 1])
                out = component_start[--component_count];
            else if (!absolute) {
                if (out)
                    path[out++] = '/';
                component_start[component_count++] = out;
                component_is_normal[component_count - 1] = false;
                path[out++] = '.';
                path[out++] = '.';
            }
            continue;
        }

        if (out && path[out - 1] != '/')
            path[out++] = '/';
        component_start[component_count++] = out;
        component_is_normal[component_count - 1] = true;
        memcpy(path + out, filename + start, len);
        out += len;
    }

    if (!out)
        path[out++] = '.';
    path[out] = '\0';
    return intern_string(path);
}

token_stream_t *gen_file_token_stream(char *filename)
{
    token_t head;
    token_t *cur = &head;
    token_stream_t *tks;

    /* initialie source location with the following configuration: pos is at 0,
     * len is 1 for reporting convenience, and the column and line number are
     * set to 1.
     */
    filename = normalize_filename(filename);
    source_location_t loc = {0, 1, 1, 1, filename, filename};
    strbuf_t *buf;

    tks = hashmap_get(TOKEN_CACHE, filename);

    /* Already cached, just return the computed token stream */
    if (tks)
        return tks;

    buf = get_file_buf(filename);
    classify_plain_source(buf);

    /* Borrows strbuf_t#size to use as source index */
    buf->size = 0;
    lex_at_line_start = true;

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

token_stream_t *gen_libc_token_stream(void)
{
    token_t head;
    token_t *cur = &head, *tk = NULL;
    token_stream_t *tks;
    char *filename = dynlink ? "lib/c.h" : "lib/c.c";
    strbuf_t *buf = LIBC_SRC;
    source_location_t loc = {0, 1, 1, 1, filename, filename};

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
    classify_plain_source(buf);

    /* Borrows strbuf_t#size to use as source index */
    buf->size = 0;
    lex_at_line_start = true;

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

    if (!tk || !head.next)
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
source_location_t *cur_token_loc(void)
{
    return &cur_token->location;
}

/* Finds next token's location; if the current token is eof, returns the eof
 * token's location instead.
 */
source_location_t *next_token_loc(void)
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
 * bounded by MAX_ID_LEN when scanned, but numeric literals run to
 * MAX_TOKEN_LEN, so the bound has to travel with the destination rather than be
 * assumed from the source.
 */
void lex_copy_literal(token_t *tk, char *value, int n)
{
    int len = strlen(tk->literal);

    if (len >= n)
        error_at("Identifier too long", &tk->location);
    strcpy(value, tk->literal);
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
