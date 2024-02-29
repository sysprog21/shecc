/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* lexer tokens */
typedef enum {
    T_start, /* FIXME: it was intended to start the state machine. */
    T_numeric,
    T_identifier,
    T_comma,  /* , */
    T_string, /* null-terminated string */
    T_char,
    T_open_bracket,  /* ( */
    T_close_bracket, /* ) */
    T_open_curly,    /* { */
    T_close_curly,   /* } */
    T_open_square,   /* [ */
    T_close_square,  /* ] */
    T_asterisk,      /* '*' */
    T_divide,        /* / */
    T_mod,           /* % */
    T_bit_or,        /* | */
    T_bit_xor,       /* ^ */
    T_bit_not,       /* ~ */
    T_log_and,       /* && */
    T_log_or,        /* || */
    T_log_not,       /* ! */
    T_lt,            /* < */
    T_gt,            /* > */
    T_le,            /* <= */
    T_ge,            /* >= */
    T_lshift,        /* << */
    T_rshift,        /* >> */
    T_dot,           /* . */
    T_arrow,         /* -> */
    T_plus,          /* + */
    T_minus,         /* - */
    T_minuseq,       /* -= */
    T_pluseq,        /* += */
    T_oreq,          /* |= */
    T_andeq,         /* &= */
    T_eq,            /* == */
    T_noteq,         /* != */
    T_assign,        /* = */
    T_increment,     /* ++ */
    T_decrement,     /* -- */
    T_question,      /* ? */
    T_colon,         /* : */
    T_semicolon,     /* ; */
    T_eof,           /* end-of-file (EOF) */
    T_ampersand,     /* & */
    T_return,
    T_if,
    T_else,
    T_while,
    T_for,
    T_do,
    T_typedef,
    T_enum,
    T_struct,
    T_sizeof,
    T_elipsis, /* ... */
    T_switch,
    T_case,
    T_break,
    T_default,
    T_continue,
    /* C pre-processor directives */
    T_cppd_include,
    T_cppd_define,
    T_cppd_undef,
    T_cppd_error,
    T_cppd_if,
    T_cppd_elif,
    T_cppd_else,
    T_cppd_endif,
    T_cppd_ifdef
} token_t;

char token_str[MAX_TOKEN_LEN];
token_t next_token;
char next_char;
int skip_newline = 1;

int preproc_match;

/* Point to the first character after where the macro has been called. It is
 * needed when returning from the macro body.
 */
int macro_return_idx;

int is_whitespace(char c)
{
    return c == ' ' || c == '\t';
}

char peek_char(int offset);

/* is it backslash-newline? */
int is_linebreak(char c)
{
    return c == '\\' && peek_char(1) == '\n';
}

int is_newline(char c)
{
    return c == '\r' || c == '\n';
}

/* is it alphabet, number or '_'? */
int is_alnum(char c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || (c == '_'));
}

int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

int is_hex(char c)
{
    return ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == 'x' ||
            (c >= 'A' && c <= 'F'));
}

int is_numeric(char buffer[])
{
    int i, hex = 0, size = strlen(buffer);

    if (size > 2)
        hex = (buffer[0] == '0' && buffer[1] == 'x') ? 1 : 0;

    for (i = 0; i < size; i++) {
        if (hex && (is_hex(buffer[i]) == 0))
            return 0;
        if (!hex && (is_digit(buffer[i]) == 0))
            return 0;
    }
    return 1;
}

void skip_whitespace()
{
    while (1) {
        if (is_linebreak(next_char)) {
            source_idx += 2;
            next_char = SOURCE[source_idx];
            continue;
        }
        if (is_whitespace(next_char) ||
            (skip_newline && is_newline(next_char))) {
            next_char = SOURCE[++source_idx];
            continue;
        }
        break;
    }
}

char read_char(int is_skip_space)
{
    next_char = SOURCE[++source_idx];
    if (is_skip_space == 1)
        skip_whitespace();
    return next_char;
}

char peek_char(int offset)
{
    return SOURCE[source_idx + offset];
}

/* Lex next token and returns its token type. Parameter `aliasing` is used for
 * disable preprocessor aliasing on identifier tokens.
 */
token_t lex_token_internal(int aliasing)
{
    token_str[0] = 0;

    /* partial preprocessor */
    if (next_char == '#') {
        int i = 0;

        do {
            token_str[i++] = next_char;
        } while (is_alnum(read_char(0)));
        token_str[i] = 0;
        skip_whitespace();

        if (!strcmp(token_str, "#include"))
            return T_cppd_include;
        if (!strcmp(token_str, "#define"))
            return T_cppd_define;
        if (!strcmp(token_str, "#undef"))
            return T_cppd_undef;
        if (!strcmp(token_str, "#error"))
            return T_cppd_error;
        if (!strcmp(token_str, "#if"))
            return T_cppd_if;
        if (!strcmp(token_str, "#elif"))
            return T_cppd_elif;
        if (!strcmp(token_str, "#ifdef"))
            return T_cppd_ifdef;
        if (!strcmp(token_str, "#else"))
            return T_cppd_else;
        if (!strcmp(token_str, "#endif"))
            return T_cppd_endif;
        error("Unknown directive");
    }

    /* C-style comments */
    if (next_char == '/') {
        read_char(0);
        if (next_char == '*') {
            /* in a comment, skip until end */
            do {
                read_char(0);
                if (next_char == '*') {
                    read_char(0);
                    if (next_char == '/') {
                        read_char(1);
                        return lex_token_internal(aliasing);
                    }
                }
            } while (next_char);
        } else {
            /* single '/', predict divide */
            if (next_char == ' ')
                read_char(1);
            return T_divide;
        }
        /* TODO: check invalid cases */
        error("Unexpected '/'");
    }

    if (is_digit(next_char)) {
        int i = 0;
        do {
            token_str[i++] = next_char;
        } while (is_hex(read_char(0)));
        token_str[i] = 0;
        skip_whitespace();
        return T_numeric;
    }
    if (next_char == '(') {
        read_char(1);
        return T_open_bracket;
    }
    if (next_char == ')') {
        read_char(1);
        return T_close_bracket;
    }
    if (next_char == '{') {
        read_char(1);
        return T_open_curly;
    }
    if (next_char == '}') {
        read_char(1);
        return T_close_curly;
    }
    if (next_char == '[') {
        read_char(1);
        return T_open_square;
    }
    if (next_char == ']') {
        read_char(1);
        return T_close_square;
    }
    if (next_char == ',') {
        read_char(1);
        return T_comma;
    }
    if (next_char == '^') {
        read_char(1);
        return T_bit_xor;
    }
    if (next_char == '~') {
        read_char(1);
        return T_bit_not;
    }
    if (next_char == '"') {
        int i = 0;
        int special = 0;

        while ((read_char(0) != '"') || special) {
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
        read_char(1);
        return T_string;
    }
    if (next_char == '\'') {
        read_char(0);
        if (next_char == '\\') {
            read_char(0);
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
            else
                abort();
        } else {
            token_str[0] = next_char;
        }
        token_str[1] = 0;
        if (read_char(0) != '\'')
            abort();
        read_char(1);
        return T_char;
    }
    if (next_char == '*') {
        read_char(1);
        return T_asterisk;
    }
    if (next_char == '&') {
        read_char(0);
        if (next_char == '&') {
            read_char(1);
            return T_log_and;
        };
        if (next_char == '=') {
            read_char(1);
            return T_andeq;
        }
        skip_whitespace();
        return T_ampersand;
    }
    if (next_char == '|') {
        read_char(0);
        if (next_char == '|') {
            read_char(1);
            return T_log_or;
        };
        if (next_char == '=') {
            read_char(1);
            return T_oreq;
        }
        skip_whitespace();
        return T_bit_or;
    }
    if (next_char == '<') {
        read_char(0);
        if (next_char == '=') {
            read_char(1);
            return T_le;
        };
        if (next_char == '<') {
            read_char(1);
            return T_lshift;
        };
        skip_whitespace();
        return T_lt;
    }
    if (next_char == '%') {
        read_char(1);
        return T_mod;
    }
    if (next_char == '>') {
        read_char(0);
        if (next_char == '=') {
            read_char(1);
            return T_ge;
        };
        if (next_char == '>') {
            read_char(1);
            return T_rshift;
        };
        skip_whitespace();
        return T_gt;
    }
    if (next_char == '!') {
        read_char(0);
        if (next_char == '=') {
            read_char(1);
            return T_noteq;
        }
        skip_whitespace();
        return T_log_not;
    }
    if (next_char == '.') {
        read_char(0);
        if (next_char == '.') {
            read_char(0);
            if (next_char == '.') {
                read_char(1);
                return T_elipsis;
            }
            abort();
        }
        skip_whitespace();
        return T_dot;
    }
    if (next_char == '-') {
        read_char(0);
        if (next_char == '>') {
            read_char(1);
            return T_arrow;
        }
        if (next_char == '-') {
            read_char(1);
            return T_decrement;
        }
        if (next_char == '=') {
            read_char(1);
            return T_minuseq;
        }
        skip_whitespace();
        return T_minus;
    }
    if (next_char == '+') {
        read_char(0);
        if (next_char == '+') {
            read_char(1);
            return T_increment;
        }
        if (next_char == '=') {
            read_char(1);
            return T_pluseq;
        }
        skip_whitespace();
        return T_plus;
    }
    if (next_char == ';') {
        read_char(1);
        return T_semicolon;
    }
    if (next_char == '?') {
        read_char(1);
        return T_question;
    }
    if (next_char == ':') {
        read_char(1);
        return T_colon;
    }
    if (next_char == '=') {
        read_char(0);
        if (next_char == '=') {
            read_char(1);
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
        } while (is_alnum(read_char(0)));
        token_str[i] = 0;
        skip_whitespace();

        if (!strcmp(token_str, "if"))
            return T_if;
        if (!strcmp(token_str, "while"))
            return T_while;
        if (!strcmp(token_str, "for"))
            return T_for;
        if (!strcmp(token_str, "do"))
            return T_do;
        if (!strcmp(token_str, "else"))
            return T_else;
        if (!strcmp(token_str, "return"))
            return T_return;
        if (!strcmp(token_str, "typedef"))
            return T_typedef;
        if (!strcmp(token_str, "enum"))
            return T_enum;
        if (!strcmp(token_str, "struct"))
            return T_struct;
        if (!strcmp(token_str, "sizeof"))
            return T_sizeof;
        if (!strcmp(token_str, "switch"))
            return T_switch;
        if (!strcmp(token_str, "case"))
            return T_case;
        if (!strcmp(token_str, "break"))
            return T_break;
        if (!strcmp(token_str, "default"))
            return T_default;
        if (!strcmp(token_str, "continue"))
            return T_continue;

        if (aliasing) {
            alias = find_alias(token_str);
            if (alias) {
                token_t t = is_numeric(alias) ? T_numeric : T_string;
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
            source_idx = macro_return_idx;
            next_char = SOURCE[source_idx];
        } else
            next_char = read_char(1);
        return lex_token_internal(aliasing);
    }

    if (next_char == 0)
        return T_eof;

    error("Unrecognized input");

    /* Unreachable, but we need an explicit return for non-void method. */
    return T_eof;
}

/* Lex next token and returns its token type. To disable aliasing on next
 * token, use `lex_token_internal`. */
token_t lex_token()
{
    return lex_token_internal(1);
}

/* Skip the content. We only need the index where the macro body begins. */
void skip_macro_body()
{
    while (!is_newline(next_char))
        next_token = lex_token();

    skip_newline = 1;
    next_token = lex_token();
}

/* Accepts next token if token types are matched. */
int lex_accept_internal(token_t token, int aliasing)
{
    if (next_token == token) {
        next_token = lex_token_internal(aliasing);
        return 1;
    }

    return 0;
}

/* Accepts next token if token types are matched. To disable aliasing
 * on next token, use `lex_accept_internal`.
 */
int lex_accept(token_t token)
{
    return lex_accept_internal(token, 1);
}

/* Peeks next token and copy token's literal to value if token types
 * are matched.
 */
int lex_peek(token_t token, char *value)
{
    if (next_token == token) {
        if (!value)
            return 1;
        strcpy(value, token_str);
        return 1;
    }
    return 0;
}

/* Strictly match next token with given token type and copy token's
 * literal to value.
 */
void lex_ident_internal(token_t token, char *value, int aliasing)
{
    if (next_token != token)
        error("Unexpected token");
    strcpy(value, token_str);
    next_token = lex_token_internal(aliasing);
}

/* Strictly match next token with given token type and copy token's
 * literal to value. To disable aliasing on next token, use
 * `lex_ident_internal`.
 */
void lex_ident(token_t token, char *value)
{
    lex_ident_internal(token, value, 1);
}

/* Strictly match next token with given token type. */
void lex_expect_internal(token_t token, int aliasing)
{
    if (next_token != token)
        error("Unexpected token");
    next_token = lex_token_internal(aliasing);
}

/* Strictly match next token with given token type. To disable aliasing
 * on next token, use `lex_expect_internal`.
 */
void lex_expect(token_t token)
{
    lex_expect_internal(token, 1);
}
