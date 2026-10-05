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

static void init_token_map(hashmap_t **map,
                           token_kind_t **storage,
                           const token_mapping_t *mappings,
                           int count,
                           int capacity)
{
    *map = hashmap_create(capacity);
    *storage = arena_alloc(TOKEN_ARENA, count * sizeof(**storage));
    for (int i = 0; i < count; i++) {
        (*storage)[i] = mappings[i].token;
        hashmap_put_borrowed(*map, mappings[i].name, &(*storage)[i]);
    }
}

void lex_init_directives(void)
{
    if (DIRECTIVE_MAP)
        return;

    token_mapping_t directives[] = {
        {"#define", T_cppd_define},   {"#elif", T_cppd_elif},
        {"#else", T_cppd_else},       {"#endif", T_cppd_endif},
        {"#error", T_cppd_error},     {"#if", T_cppd_if},
        {"#ifdef", T_cppd_ifdef},     {"#ifndef", T_cppd_ifndef},
        {"#include", T_cppd_include}, {"#pragma", T_cppd_pragma},
        {"#undef", T_cppd_undef},     {"#line", T_cppd_line},
    };
    init_token_map(&DIRECTIVE_MAP, &directive_tokens_storage, directives,
                   sizeof(directives) / sizeof(*directives), 16);
}

void lex_init_keywords(void)
{
    if (KEYWORD_MAP)
        return;

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
    init_token_map(&KEYWORD_MAP, &keyword_tokens_storage, keywords,
                   sizeof(keywords) / sizeof(*keywords), 32);
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
token_kind_t lookup_keyword(char *token, int length)
{
    if (length < 2 || length > 10)
        return T_identifier;

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

    /* Token storage arrays share TOKEN_ARENA's parse lifetime and are released
     * by release_token_arena().
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

/* @filename's contents, or NULL when it cannot be opened. */
strbuf_t *try_read_file(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    strbuf_t *src;

    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    int len = ftell(f);
    src = strbuf_create(len + 1);
    fseek(f, 0, SEEK_SET);

    src->size = file_read_all(f, src->elements, len);

    fclose(f);
    src->elements[len] = '\0';
    return src;
}

/* @filename's contents, read once and kept for every later request; NULL when
 * it cannot be opened, so that a caller with a better place to report the
 * mistake can.
 */
strbuf_t *try_get_file_buf(char *filename)
{
    strbuf_t *buf;

    if (hashmap_contains(SRC_FILE_MAP, filename))
        return hashmap_get(SRC_FILE_MAP, filename);
    buf = try_read_file(filename);
    if (buf)
        hashmap_put_borrowed(SRC_FILE_MAP, filename, buf);
    return buf;
}

strbuf_t *get_file_buf(char *filename)
{
    strbuf_t *buf = try_get_file_buf(filename);

    /* A file that cannot be opened is a mistake in how the compiler was
     * invoked, not a broken invariant, so it exits rather than abort()ing.
     */
    if (!buf) {
        printf("[Error]: cannot open source file '%s'\n", filename);
        fflush(NULL);
        exit(1);
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

static token_t *new_token_at_column(source_location_t *loc,
                                    token_kind_t kind,
                                    int len)
{
    token_t *token = new_token(kind, loc, len);

    loc->column += len;
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
            return new_token_at_column(loc, T_hashhash, paste_len);
        }

        if (!lex_at_line_start) {
            int hash_chars = is_digraph_hash ? 2 : 1;

            for (int i = 0; i < hash_chars; i++)
                read_char(buf);
            return new_token_at_column(loc, T_hash, hash_len);
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

        return new_token_at_column(loc, directive_kind, source_len);
    }

    /* Leave a UCN to lex_word(); an ordinary backslash remains available for
     * the preprocessor's line-splice handling.
     */
    if (ch == '\\' && peek_char(buf, 1) != 'u' && peek_char(buf, 1) != 'U') {
        read_char(buf);
        return new_token_at_column(loc, T_backslash, 1);
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
            return new_token_at_column(loc, T_divideeq, 2);
        }

        return new_token_at_column(loc, T_divide, 1);
    }

    if (ch == ' ') {
        /* Compacts sequence of whitespace together */
        int sz = 1;

        while (read_char(buf) == ' ')
            sz++;

        return new_token_at_column(loc, T_whitespace, sz);
    }

    if (ch == '\t') {
        read_char(buf);
        return new_token_at_column(loc, T_tab, 1);
    }

    if (ch == '\0') {
        read_char(buf);
        return new_token_at_column(loc, T_eof, 1);
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
    if (ch == '<' && peek_char(buf, 1) == '%') {
        read_char(buf);
        read_char(buf);
        return new_token_at_column(loc, T_open_curly, 2);
    }

    if (ch == '%' && peek_char(buf, 1) == '>') {
        read_char(buf);
        read_char(buf);
        return new_token_at_column(loc, T_close_curly, 2);
    }

    if (ch == '<' && peek_char(buf, 1) == ':') {
        read_char(buf);
        read_char(buf);
        return new_token_at_column(loc, T_open_square, 2);
    }

    if (ch == ':' && peek_char(buf, 1) == '>') {
        read_char(buf);
        read_char(buf);
        return new_token_at_column(loc, T_close_square, 2);
    }

    token_kind_t kind;
    switch (ch) {
    case '(':
        kind = T_open_bracket;
        break;
    case ')':
        kind = T_close_bracket;
        break;
    case '{':
        kind = T_open_curly;
        break;
    case '}':
        kind = T_close_curly;
        break;
    case '[':
        kind = T_open_square;
        break;
    case ']':
        kind = T_close_square;
        break;
    case ',':
        kind = T_comma;
        break;
    case '~':
        kind = T_bit_not;
        break;
    case ';':
        kind = T_semicolon;
        break;
    case '?':
        kind = T_question;
        break;
    case ':':
        kind = T_colon;
        break;
    default:
        return NULL;
    }
    read_char(buf);
    return new_token_at_column(loc, kind, 1);
}

/* Operators, each of which may or may not continue into a longer one.
 *
 * Returns NULL when 'ch' is none of its business, so that lex_token() can offer
 * the character to the next reader in line.
 */
static token_t *lex_short_operator(strbuf_t *buf,
                                   source_location_t *loc,
                                   char pair_char,
                                   token_kind_t pair_kind,
                                   char alternate_char,
                                   token_kind_t alternate_kind,
                                   token_kind_t assign_kind,
                                   token_kind_t single_kind)
{
    int length = 1;
    token_kind_t kind = single_kind;

    char ch = read_char(buf);
    if (pair_char && ch == pair_char) {
        kind = pair_kind;
        length++;
    } else if (alternate_char && ch == alternate_char) {
        kind = alternate_kind;
        length++;
    } else if (ch == '=' && assign_kind != T_start) {
        kind = assign_kind;
        length++;
    }
    if (length == 2)
        read_char(buf);
    return new_token_at_column(loc, kind, length);
}

static token_t *lex_angle_operator(strbuf_t *buf,
                                   source_location_t *loc,
                                   char first,
                                   token_kind_t single,
                                   token_kind_t equal,
                                   token_kind_t shift,
                                   token_kind_t shift_equal)
{
    char ch = read_char(buf);
    int length = 1;
    token_kind_t kind = single;

    if (ch == '=') {
        kind = equal;
        length = 2;
        read_char(buf);
    } else if (ch == first) {
        ch = read_char(buf);
        kind = shift;
        length = 2;
        if (ch == '=') {
            kind = shift_equal;
            length++;
            read_char(buf);
        }
    }
    return new_token_at_column(loc, kind, length);
}

token_t *lex_operator(strbuf_t *buf, source_location_t *loc, char ch)
{
    if (ch == '^')
        return lex_short_operator(buf, loc, 0, T_start, 0, T_start, T_xoreq,
                                  T_bit_xor);
    if (ch == '*')
        return lex_short_operator(buf, loc, 0, T_start, 0, T_start,
                                  T_asteriskeq, T_asterisk);
    if (ch == '&')
        return lex_short_operator(buf, loc, '&', T_log_and, 0, T_start, T_andeq,
                                  T_ampersand);
    if (ch == '|')
        return lex_short_operator(buf, loc, '|', T_log_or, 0, T_start, T_oreq,
                                  T_bit_or);

    if (ch == '<')
        return lex_angle_operator(buf, loc, '<', T_lt, T_le, T_lshift,
                                  T_lshifteq);

    if (ch == '%')
        return lex_short_operator(buf, loc, 0, T_start, 0, T_start, T_modeq,
                                  T_mod);

    if (ch == '>')
        return lex_angle_operator(buf, loc, '>', T_gt, T_ge, T_rshift,
                                  T_rshifteq);

    if (ch == '!')
        return lex_short_operator(buf, loc, 0, T_start, 0, T_start, T_noteq,
                                  T_log_not);

    if (ch == '.') {
        ch = read_char(buf);

        if (ch == '.' && peek_char(buf, 1) == '.') {
            buf->size += 2;
            return new_token_at_column(loc, T_elipsis, 3);
        }

        return new_token_at_column(loc, T_dot, 1);
    }

    if (ch == '-')
        return lex_short_operator(buf, loc, '-', T_decrement, '>', T_arrow,
                                  T_minuseq, T_minus);
    if (ch == '+')
        return lex_short_operator(buf, loc, '+', T_increment, 0, T_start,
                                  T_pluseq, T_plus);
    if (ch == '=')
        return lex_short_operator(buf, loc, '=', T_eq, 0, T_start, T_start,
                                  T_assign);

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

    return append_utf8(out, 0, out_size, value);
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

        token_kind_t kind = lookup_keyword(token_buffer, sz);

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

static token_stream_t *cache_token_stream(char *filename,
                                          token_t *head,
                                          token_t *tail)
{
    token_stream_t *stream = malloc(sizeof(*stream));

    stream->head = head;
    stream->tail = tail;
    hashmap_put_borrowed(TOKEN_CACHE, filename, stream);
    return stream;
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

    return cache_token_stream(filename, head.next, cur);
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
        hashmap_put_borrowed(SRC_FILE_MAP, filename, LIBC_SRC);

    /* This buffer was built by appending, so its capacity is whatever the
     * doubling left and runs past the text into memory that was never written
     * -- while the scan below, like the one over a file, stops at capacity.
     * Terminate it the way try_read_file() leaves a file: the text, a NUL, and
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

    return cache_token_stream(filename, head.next, cur);
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
