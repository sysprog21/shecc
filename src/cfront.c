/* C language front-end */

int is_whitespace(char c)
{
    return (c == ' ' || c == '\t');
}

char peek_char(int offset);

/* is it backslash-newline? */
int is_linebreak(char c)
{
    return c == '\\' && peek_char(1) == '\n';
}

int is_newline(char c)
{
    return (c == '\r' || c == '\n');
}

/* is it alphabet, number or '_'? */
int is_alnum(char c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || (c == '_'));
}

int is_digit(char c)
{
    return (c >= '0' && c <= '9') ? 1 : 0;
}

int is_hex(char c)
{
    return ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == 'x' ||
            (c >= 'A' && c <= 'F'));
}

int is_numeric(char buffer[])
{
    int i, hex, size = strlen(buffer);

    if (size > 2)
        hex = (buffer[0] == '0' && buffer[1] == 'x') ? 1 : 0;
    else
        hex = 0;

    for (i = 0; i < size; i++) {
        if (hex && (is_hex(buffer[i]) == 0))
            return 0;
        if (!hex && (is_digit(buffer[i]) == 0))
            return 0;
    }
    return 1;
}

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
    T_define,
    T_undef,
    T_error,
    T_include,
    T_typedef,
    T_enum,
    T_struct,
    T_sizeof,
    T_elipsis, /* ... */
    T_switch,
    T_case,
    T_break,
    T_default,
    T_continue
} token_t;

char token_str[MAX_TOKEN_LEN];
token_t next_token;
char next_char;
int skip_newline = 1;

int preproc_match;
/*
 * Allows replacing identifiers with alias value if alias exists. This is
 * disabled in certain cases, e.g. #undef.
 */
int preproc_aliasing = 1;
/*
 * Point to the first character after where the macro has been called. It is
 * needed when returning from the macro body.
 */
int macro_return_idx;

int global_var_idx = 0;
int global_label_idx = 0;
char global_str_buf[MAX_VAR_LEN];

char *gen_name()
{
    sprintf(global_str_buf, ".t%d", global_var_idx++);
    return global_str_buf;
}

char *gen_label()
{
    sprintf(global_str_buf, ".label.%d", global_label_idx++);
    return global_str_buf;
}

var_t *require_var(block_t *blk)
{
    return &blk->locals[blk->next_local++];
}

/* stack of the operands of 3AC */
var_t *operand_stack[MAX_OPERAND_STACK_SIZE];
int operand_stack_idx = 0;

void opstack_push(var_t *var)
{
    operand_stack[operand_stack_idx++] = var;
}

var_t *opstack_pop()
{
    return operand_stack[--operand_stack_idx];
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

/*
 * get alias name from defined() directive
 * i.e., get __arm__ from defined(__arm__)
 */
void read_alias_name_from_defined(char *alias_name, char *src)
{
    int i;

    src = src + 8; /* skip defined( */
    i = 0;
    while (src[i] != ')') {
        alias_name[i] = src[i];
        i++;
    }
    alias_name[i] = 0;
}

char peek_char(int offset)
{
    return SOURCE[source_idx + offset];
}

void if_elif_skip_lines()
{
    char peek_c;
    int i;

    do {
        skip_whitespace();
        i = 0;
        do {
            token_str[i++] = next_char;
        } while (read_char(0) != '\n');
        token_str[i] = 0;
        read_char(1);
        peek_c = peek_char(1);
    } while (next_char != '#' || (next_char == '#' && peek_c == 'd'));
    skip_whitespace();
}

void ifdef_else_skip_lines()
{
    int i;

    do {
        skip_whitespace();
        i = 0;
        do {
            token_str[i++] = next_char;
        } while (read_char(0) != '\n');
        token_str[i] = 0;
    } while (strcmp(token_str, "#else") && strcmp(token_str, "#endif"));
    skip_whitespace();
}

/*
 * check alias defined or not
 */
void chk_def(int defined)
{
    char *alias = NULL;
    char alias_name[MAX_TOKEN_LEN];

    if (defined) {
        read_alias_name_from_defined(alias_name, token_str);
        alias = find_alias(alias_name);
    } else
        alias = find_alias(token_str);

    if (alias)
        preproc_match = 1;
}

token_t get_next_token()
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

        if (!strcmp(token_str, "#include")) {
            do {
                token_str[i++] = next_char;
            } while (read_char(0) != '\n');
            skip_whitespace();
            return T_include;
        }
        if (!strcmp(token_str, "#define")) {
            skip_whitespace();
            return T_define;
        }
        if (!strcmp(token_str, "#undef")) {
            skip_whitespace();
            return T_undef;
        }
        if (!strcmp(token_str, "#error")) {
            skip_whitespace();
            return T_error;
        }
        if (!strcmp(token_str, "#if")) {
            preproc_match = 0;
            i = 0;
            do {
                token_str[i++] = next_char;
            } while (read_char(0) != '\n');
            token_str[i] = 0;

            if (!strncmp(token_str, "defined", 7)) {
                chk_def(1);
                if (preproc_match) {
                    skip_whitespace();
                    return get_next_token();
                }

                /* skip lines until #elif or #else or #endif */
                if_elif_skip_lines();
                return get_next_token();
            }
        }
        if (!strcmp(token_str, "#elif")) {
            if (preproc_match) {
                do {
                    skip_whitespace();
                    i = 0;
                    do {
                        token_str[i++] = next_char;
                    } while (read_char(0) != '\n');
                    token_str[i] = 0;
                } while (strcmp(token_str, "#endif"));
                skip_whitespace();
                return get_next_token();
            }

            i = 0;
            do {
                token_str[i++] = next_char;
            } while (read_char(0) != '\n');
            token_str[i] = 0;

            if (!strncmp(token_str, "defined", 7)) {
                chk_def(1);
                if (preproc_match) {
                    skip_whitespace();
                    return get_next_token();
                }
                /* skip lines until #elif or #else or #endif */
                if_elif_skip_lines();
                return get_next_token();
            }
        }
        if (!strcmp(token_str, "#ifdef")) {
            preproc_match = 0;
            i = 0;
            do {
                token_str[i++] = next_char;
            } while (read_char(0) != '\n');
            token_str[i] = 0;
            chk_def(0);
            if (preproc_match) {
                skip_whitespace();
                return get_next_token();
            }
            /* skip lines until #else or #endif */
            ifdef_else_skip_lines();
            return get_next_token();
        }
        if (!strcmp(token_str, "#else")) {
            /*
             * reach here has 2 possible cases:
             * 1. reach #ifdef preprocessor directive
             * 2. conditional expression in #elif is false
             */

            if (!preproc_match) {
                skip_whitespace();
                return get_next_token();
            }
            /* skip lines until #else or #endif */
            ifdef_else_skip_lines();
            return get_next_token();
        }
        if (!strcmp(token_str, "#endif")) {
            preproc_match = 0;
            skip_whitespace();
            return get_next_token();
        }
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
                        return get_next_token();
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

    /* end of file */
    /* "FIXME: The signedness of 'char' in the C programming language is indeed
     * implementation-specific. For example, gcc for Arm treats 'char' as
     * unsigned, while gcc for x86(-64) treats 'char' as signed. The warning
     * below is raised in gcc for Arm:
     *   warning: comparison is always false due to limited range of data type
     *   [-Wtype-limits]
     */
    if ((next_char == 0) || (next_char == -1))
        return T_eof;

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

        if (preproc_aliasing) {
            alias = find_alias(token_str);
            if (alias) {
                token_t t = is_numeric(alias) ? T_numeric : T_string;
                strcpy(token_str, alias);
                return t;
            }
        }

        return T_identifier;
    }

    /*
     * This only happens when parsing a macro. Move to the token after the
     * macro definition or return to where the macro has been called.
     */
    if (next_char == '\n') {
        if (macro_return_idx) {
            source_idx = macro_return_idx;
            next_char = SOURCE[source_idx];
        } else
            next_char = read_char(1);
        return get_next_token();
    }

    error("Unrecognized input");
    return T_eof;
}

/* Skip the content. We only need the index where the macro body begins. */
void skip_macro_body()
{
    while (!is_newline(next_char))
        next_token = get_next_token();

    skip_newline = 1;
    next_token = get_next_token();
}

int lex_accept(token_t token)
{
    if (next_token == token) {
        next_token = get_next_token();
        return 1;
    }
    return 0;
}

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

void lex_ident(token_t token, char *value)
{
    if (next_token != token)
        error("Unexpected token");
    strcpy(value, token_str);
    next_token = get_next_token();
}

void lex_expect(token_t token)
{
    if (next_token != token)
        error("Unexpected token");
    next_token = get_next_token();
}

void read_expr(block_t *parent);

int write_symbol(char *data, int len)
{
    int startLen = elf_data_idx;
    elf_write_data_str(data, len);
    return startLen;
}

int get_size(var_t *var, type_t *type)
{
    if (var->is_ptr || var->is_func)
        return PTR_SIZE;
    return type->size;
}

int read_numeric_constant(char buffer[])
{
    int i = 0;
    int value = 0;
    while (buffer[i]) {
        if (i == 1 && (buffer[i] == 'x')) { /* hexadecimal */
            value = 0;
            i = 2;
            while (buffer[i]) {
                char c = buffer[i++];
                value = value << 4;
                if (is_digit(c))
                    value += c - '0';
                c |= 32; /* convert to lower case */
                if (c >= 'a' && c <= 'f')
                    value += (c - 'a') + 10;
            }
            return value;
        }
        value = value * 10 + buffer[i++] - '0';
    }
    return value;
}

void read_parameter_list_decl(func_t *fd, int anon);

void read_inner_var_decl(var_t *vd, int anon, int is_param)
{
    vd->init_val = 0;
    if (lex_accept(T_asterisk))
        vd->is_ptr = 1;
    else
        vd->is_ptr = 0;

    /* is it function pointer declaration? */
    if (lex_accept(T_open_bracket)) {
        func_t func;
        lex_expect(T_asterisk);
        lex_ident(T_identifier, vd->var_name);
        lex_expect(T_close_bracket);
        read_parameter_list_decl(&func, 1);
        vd->is_func = 1;
    } else {
        if (anon == 0) {
            lex_ident(T_identifier, vd->var_name);
            if (!lex_peek(T_open_bracket, NULL) && !is_param) {
                if (vd->is_global) {
                    ph1_ir_t *ir = add_global_ir(OP_allocat);
                    ir->src0 = vd;
                    opstack_push(vd);
                } else {
                    ph1_ir_t *ph1_ir;
                    ph1_ir = add_ph1_ir(OP_allocat);
                    ph1_ir->src0 = vd;
                }
            }
        }
        if (lex_accept(T_open_square)) {
            char buffer[10];

            /* array with size */
            if (lex_peek(T_numeric, buffer)) {
                vd->array_size = read_numeric_constant(buffer);
                lex_expect(T_numeric);
            } else {
                /* array without size:
                 * regarded as a pointer although could be nested
                 */
                vd->is_ptr++;
            }
            lex_expect(T_close_square);
        } else {
            vd->array_size = 0;
        }
        vd->is_func = 0;
    }
}

/* starting next_token, need to check the type */
void read_full_var_decl(var_t *vd, int anon, int is_param)
{
    lex_accept(T_struct); /* ignore struct definition */
    lex_ident(T_identifier, vd->type_name);
    read_inner_var_decl(vd, anon, is_param);
}

/* starting next_token, need to check the type */
void read_partial_var_decl(var_t *vd, var_t *template)
{
    strcpy(vd->type_name, template->type_name);
    read_inner_var_decl(vd, 0, 0);
}

void read_parameter_list_decl(func_t *fd, int anon)
{
    int vn = 0;
    lex_expect(T_open_bracket);
    while (lex_peek(T_identifier, NULL) == 1) {
        read_full_var_decl(&fd->param_defs[vn++], anon, 1);
        lex_accept(T_comma);
    }
    if (lex_accept(T_elipsis)) {
        /* variadic function. Max 8 parameters are passed. */
        fd->va_args = 1;
        vn = MAX_PARAMS;
    }
    lex_expect(T_close_bracket);
    fd->num_params = vn;
}

void read_literal_param(block_t *parent)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    int index;
    char literal[MAX_TOKEN_LEN];

    lex_ident(T_string, literal);
    index = write_symbol(literal, strlen(literal) + 1);

    ph1_ir = add_ph1_ir(OP_load_data_address);
    vd = require_var(parent);
    strcpy(vd->var_name, gen_name());
    vd->init_val = index;
    ph1_ir->dest = vd;
    opstack_push(vd);
}

void read_numeric_param(block_t *parent, int is_neg)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    char token[MAX_ID_LEN];
    int value = 0;
    int i = 0;
    char c;

    lex_ident(T_numeric, token);

    if (token[0] == '-') {
        is_neg = 1 - is_neg;
        i++;
    }
    if ((token[0] == '0') && (token[1] == 'x')) { /* hexadecimal */
        i = 2;
        do {
            c = token[i++];
            if (is_digit(c))
                c -= '0';
            else {
                c |= 32; /* convert to lower case */
                if (c >= 'a' && c <= 'f')
                    c = (c - 'a') + 10;
                else
                    error("Invalid numeric constant");
            }

            value = (value * 16) + c;
        } while (is_hex(token[i]));
    } else {
        do {
            c = token[i++] - '0';
            value = (value * 10) + c;
        } while (is_digit(token[i]));
    }

    if (is_neg)
        value = -value;

    ph1_ir = add_ph1_ir(OP_load_constant);
    vd = require_var(parent);
    vd->init_val = value;
    strcpy(vd->var_name, gen_name());
    ph1_ir->dest = vd;
    opstack_push(vd);
}

void read_char_param(block_t *parent)
{
    char token[5];
    ph1_ir_t *ph1_ir;
    var_t *vd;

    lex_ident(T_char, token);

    ph1_ir = add_ph1_ir(OP_load_constant);
    vd = require_var(parent);
    vd->init_val = token[0];
    strcpy(vd->var_name, gen_name());
    ph1_ir->dest = vd;
    opstack_push(vd);
}

void read_ternary_operation(block_t *parent);
void read_func_parameters(block_t *parent)
{
    int i, param_num = 0;
    var_t *params[MAX_PARAMS];

    lex_expect(T_open_bracket);
    while (!lex_accept(T_close_bracket)) {
        read_expr(parent);
        read_ternary_operation(parent);

        params[param_num++] = opstack_pop();
        lex_accept(T_comma);
    }
    for (i = 0; i < param_num; i++) {
        ph1_ir_t *ph1_ir = add_ph1_ir(OP_push);
        ph1_ir->src0 = params[i];
    }
}

void read_func_call(func_t *fn, block_t *parent)
{
    ph1_ir_t *ph1_ir;

    /* direct function call */
    read_func_parameters(parent);

    ph1_ir = add_ph1_ir(OP_call);
    ph1_ir->param_num = fn->num_params;
    strcpy(ph1_ir->func_name, fn->return_def.var_name);
}

void read_indirect_call(block_t *parent)
{
    ph1_ir_t *ph1_ir;

    read_func_parameters(parent);

    ph1_ir = add_ph1_ir(OP_indirect);
    ph1_ir->src0 = opstack_pop();
}

ph1_ir_t side_effect[10];
int se_idx = 0;

void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 int eval,
                 opcode_t op);

/* Maintain a stack of expression values and operators, depending on next
 * operators' priority. Either apply it or operator on stack first.
 */
void read_expr_operand(block_t *parent)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    int is_neg = 0;

    if (lex_accept(T_minus)) {
        is_neg = 1;
        if (lex_peek(T_numeric, NULL) == 0 &&
            lex_peek(T_identifier, NULL) == 0 &&
            lex_peek(T_open_bracket, NULL) == 0) {
            error("Unexpected token after unary minus");
        }
    }

    if (lex_peek(T_string, NULL))
        read_literal_param(parent);
    else if (lex_peek(T_char, NULL))
        read_char_param(parent);
    else if (lex_peek(T_numeric, NULL))
        read_numeric_param(parent, is_neg);
    else if (lex_accept(T_log_not)) {
        read_expr_operand(parent);

        ph1_ir = add_ph1_ir(OP_log_not);
        ph1_ir->src0 = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
    } else if (lex_accept(T_bit_not)) {
        read_expr_operand(parent);

        ph1_ir = add_ph1_ir(OP_bit_not);
        ph1_ir->src0 = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
    } else if (lex_accept(T_ampersand)) {
        char token[MAX_VAR_LEN];
        var_t *var;
        lvalue_t lvalue;

        lex_peek(T_identifier, token);
        var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, 0, OP_generic);

        if (lvalue.is_reference == 0) {
            ph1_ir = add_ph1_ir(OP_address_of);
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
        }
    } else if (lex_accept(T_asterisk)) {
        /* dereference */
        char token[MAX_VAR_LEN];
        var_t *var;
        lvalue_t lvalue;

        lex_accept(T_open_bracket);
        lex_peek(T_identifier, token);
        var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, 1, OP_generic);
        lex_accept(T_close_bracket);

        ph1_ir = add_ph1_ir(OP_read);
        ph1_ir->src0 = opstack_pop();
        vd = require_var(parent);
        if (lvalue.is_ptr > 1)
            ph1_ir->size = PTR_SIZE;
        else
            ph1_ir->size = lvalue.type->size;
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
    } else if (lex_accept(T_open_bracket)) {
        read_expr(parent);
        read_ternary_operation(parent);
        lex_expect(T_close_bracket);
    } else if (lex_accept(T_sizeof)) {
        char token[MAX_TYPE_LEN];
        type_t *type;

        lex_expect(T_open_bracket);
        lex_ident(T_identifier, token);
        type = find_type(token);
        if (!type)
            error("Unable to find type");

        ph1_ir = add_ph1_ir(OP_load_constant);
        vd = require_var(parent);
        vd->init_val = type->size;
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
        lex_expect(T_close_bracket);
    } else {
        /* function call, constant or variable - read token and determine */
        opcode_t prefix_op = OP_generic;
        char token[MAX_ID_LEN];
        func_t *fn;
        var_t *var;
        constant_t *con;
        int macro_param_idx;
        macro_t *mac;

        if (lex_accept(T_increment))
            prefix_op = OP_add;
        else if (lex_accept(T_decrement))
            prefix_op = OP_sub;

        lex_peek(T_identifier, token);

        /* is a constant or variable? */
        con = find_constant(token);
        var = find_var(token, parent);
        fn = find_func(token);
        macro_param_idx = find_macro_param_src_idx(token, parent);
        mac = find_macro(token);

        if (!strcmp(token, "__VA_ARGS__")) {
            /* `source_idx` has pointed at the character after __VA_ARGS__ */
            int i, remainder, t = source_idx;
            macro_t *macro = parent->macro;

            if (!macro)
                error("The '__VA_ARGS__' identifier can only be used in macro");
            if (!macro->is_variadic)
                error("Unexpected identifier '__VA_ARGS__'");

            remainder = macro->num_params - macro->num_param_defs;
            for (i = 0; i < remainder; i++) {
                source_idx = macro->params[macro->num_params - remainder + i];
                next_char = SOURCE[source_idx];
                next_token = get_next_token();
                read_expr(parent);
            }
            source_idx = t;
            next_char = SOURCE[source_idx];
            next_token = get_next_token();
        } else if (mac) {
            if (parent->macro)
                error("Nested macro is not yet supported");

            parent->macro = mac;
            mac->num_params = 0;
            lex_expect(T_identifier);

            /* `source_idx` has pointed at the first parameter */
            while (!lex_peek(T_close_bracket, NULL)) {
                mac->params[mac->num_params++] = source_idx;
                do {
                    next_token = get_next_token();
                } while (next_token != T_comma &&
                         next_token != T_close_bracket);
            }
            /* move `source_idx` to the macro body */
            macro_return_idx = source_idx;
            source_idx = mac->start_source_idx;
            next_char = SOURCE[source_idx];
            lex_expect(T_close_bracket);

            skip_newline = 0;
            read_expr(parent);

            /* cleanup */
            skip_newline = 1;
            parent->macro = NULL;
            macro_return_idx = 0;
        } else if (macro_param_idx) {
            /* "expand" the argument from where it comes from */
            int t = source_idx;
            source_idx = macro_param_idx;
            next_char = SOURCE[source_idx];
            next_token = get_next_token();
            read_expr(parent);
            source_idx = t;
            next_char = SOURCE[source_idx];
            next_token = get_next_token();
        } else if (con) {
            ph1_ir = add_ph1_ir(OP_load_constant);
            vd = require_var(parent);
            vd->init_val = con->value;
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
            lex_expect(T_identifier);
        } else if (var) {
            /* evalue lvalue expression */
            lvalue_t lvalue;
            read_lvalue(&lvalue, var, parent, 1, prefix_op);

            /* is it an indirect call with function pointer? */
            if (lex_peek(T_open_bracket, NULL)) {
                read_indirect_call(parent);

                ph1_ir = add_ph1_ir(OP_func_ret);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
            }
        } else if (fn) {
            lex_expect(T_identifier);

            if (lex_peek(T_open_bracket, NULL)) {
                read_func_call(fn, parent);

                ph1_ir = add_ph1_ir(OP_func_ret);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
            } else {
                /* indirective function pointer assignment */
                vd = require_var(parent);
                vd->is_func = 1;
                strcpy(vd->var_name, token);
                opstack_push(vd);
            }
        } else {
            printf("%s\n", token);
            /* unknown expression */
            error("Unrecognized expression token");
        }

        if (is_neg) {
            ph1_ir = add_ph1_ir(OP_negate);
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
        }
    }
}

int get_operator_prio(opcode_t op)
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

opcode_t get_operator()
{
    opcode_t op = OP_generic;
    if (lex_accept(T_plus))
        op = OP_add;
    else if (lex_accept(T_minus))
        op = OP_sub;
    else if (lex_accept(T_asterisk))
        op = OP_mul;
    else if (lex_accept(T_divide))
        op = OP_div;
    else if (lex_accept(T_mod))
        op = OP_mod;
    else if (lex_accept(T_lshift))
        op = OP_lshift;
    else if (lex_accept(T_rshift))
        op = OP_rshift;
    else if (lex_accept(T_log_and))
        op = OP_log_and;
    else if (lex_accept(T_log_or))
        op = OP_log_or;
    else if (lex_accept(T_eq))
        op = OP_eq;
    else if (lex_accept(T_noteq))
        op = OP_neq;
    else if (lex_accept(T_lt))
        op = OP_lt;
    else if (lex_accept(T_le))
        op = OP_leq;
    else if (lex_accept(T_gt))
        op = OP_gt;
    else if (lex_accept(T_ge))
        op = OP_geq;
    else if (lex_accept(T_ampersand))
        op = OP_bit_and;
    else if (lex_accept(T_bit_or))
        op = OP_bit_or;
    else if (lex_accept(T_bit_xor))
        op = OP_bit_xor;
    else if (lex_peek(T_question, NULL))
        op = OP_ternary;
    return op;
}

void read_expr(block_t *parent)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    opcode_t op;
    opcode_t oper_stack[10];
    int oper_stack_idx = 0;

    read_expr_operand(parent);

    op = get_operator();
    if (op == OP_generic || op == OP_ternary)
        return;

    oper_stack[oper_stack_idx++] = op;
    read_expr_operand(parent);
    op = get_operator();

    while (op != OP_generic && op != OP_ternary) {
        if (oper_stack_idx > 0) {
            int same = 0;
            do {
                opcode_t top_op = oper_stack[oper_stack_idx - 1];
                if (get_operator_prio(top_op) >= get_operator_prio(op)) {
                    ph1_ir = add_ph1_ir(top_op);
                    ph1_ir->src1 = opstack_pop();
                    ph1_ir->src0 = opstack_pop();
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);

                    oper_stack_idx--;
                } else
                    same = 1;
            } while (oper_stack_idx > 0 && same == 0);
        }
        read_expr_operand(parent);
        oper_stack[oper_stack_idx++] = op;
        op = get_operator();
    }

    while (oper_stack_idx > 0) {
        ph1_ir = add_ph1_ir(oper_stack[--oper_stack_idx]);
        ph1_ir->src1 = opstack_pop();
        ph1_ir->src0 = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
    }
}

/* Return the address that an expression points to, or evaluate its value.
 *   x =;
 *   x[<expr>] =;
 *   x[expr].field =;
 *   x[expr]->field =;
 */
void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 int eval,
                 opcode_t prefix_op)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    int is_address_got = 0;
    int is_member = 0;

    /* already peeked and have the variable */
    lex_expect(T_identifier);

    lvalue->type = find_type(var->type_name);
    lvalue->size = get_size(var, lvalue->type);
    lvalue->is_ptr = var->is_ptr;
    lvalue->is_func = var->is_func;
    lvalue->is_reference = 0;

    opstack_push(var);

    if (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
        lex_peek(T_dot, NULL))
        lvalue->is_reference = 1;

    while (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
           lex_peek(T_dot, NULL)) {
        if (lex_accept(T_open_square)) {
            /* var must be either a pointer or an array of some type */
            if (var->is_ptr == 0 && var->array_size == 0)
                error("Cannot apply square operator to non-pointer");

            /* if nested pointer, still pointer */
            if (var->is_ptr <= 1 && var->array_size == 0)
                lvalue->size = lvalue->type->size;

            read_expr(parent);

            /* multiply by element size */
            if (lvalue->size != 1) {
                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                vd->init_val = lvalue->size;
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);

                ph1_ir = add_ph1_ir(OP_mul);
                ph1_ir->src1 = opstack_pop();
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
            }

            ph1_ir = add_ph1_ir(OP_add);
            ph1_ir->src1 = opstack_pop();
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);

            lex_expect(T_close_square);
            is_address_got = 1;
            is_member = 1;
            lvalue->is_reference = 1;
        } else {
            char token[MAX_ID_LEN];

            if (lex_accept(T_arrow)) {
                /* resolve where the pointer points at from the calculated
                 * address in a structure */
                if (is_member == 1) {
                    ph1_ir = add_ph1_ir(OP_read);
                    ph1_ir->src0 = opstack_pop();
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);
                    ph1_ir->size = 4;
                }
            } else {
                lex_expect(T_dot);

                if (is_address_got == 0) {
                    ph1_ir = add_ph1_ir(OP_address_of);
                    ph1_ir->src0 = opstack_pop();
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);

                    is_address_got = 1;
                }
            }

            lex_ident(T_identifier, token);

            /* change type currently pointed to */
            var = find_member(token, lvalue->type);
            lvalue->type = find_type(var->type_name);
            lvalue->is_ptr = var->is_ptr;
            lvalue->is_func = var->is_func;
            lvalue->size = get_size(var, lvalue->type);

            /* if it is an array, get the address of first element instead of
             * its value */
            if (var->array_size > 0)
                lvalue->is_reference = 0;

            /* move pointer to offset of structure */
            ph1_ir = add_ph1_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = var->offset;
            ph1_ir->dest = vd;
            opstack_push(vd);

            ph1_ir = add_ph1_ir(OP_add);
            ph1_ir->src1 = opstack_pop();
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);

            is_address_got = 1;
            is_member = 1;
        }
    }

    if (!eval)
        return;

    if (lex_peek(T_plus, NULL) && (var->is_ptr || var->array_size)) {
        while (lex_peek(T_plus, NULL) && (var->is_ptr || var->array_size)) {
            lex_expect(T_plus);
            if (lvalue->is_reference) {
                ph1_ir = add_ph1_ir(OP_read);
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                ph1_ir->size = lvalue->size;
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
            }

            read_expr_operand(parent);

            lvalue->size = lvalue->type->size;

            if (lvalue->size > 1) {
                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                vd->init_val = lvalue->size;
                ph1_ir->dest = vd;
                opstack_push(vd);

                ph1_ir = add_ph1_ir(OP_mul);
                ph1_ir->src1 = opstack_pop();
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
            }

            ph1_ir = add_ph1_ir(OP_add);
            ph1_ir->src1 = opstack_pop();
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
        }
    } else {
        var_t *t;

        /**
         * If operand is a reference, read the value and push to stack
         * for the incoming addition/subtraction. Otherwise, use the
         * top element of stack as the one of operands and the destination.
         */
        if (lvalue->is_reference) {
            ph1_ir = add_ph1_ir(OP_read);
            ph1_ir->src0 = operand_stack[operand_stack_idx - 1];
            t = require_var(parent);
            ph1_ir->size = lvalue->size;
            strcpy(t->var_name, gen_name());
            ph1_ir->dest = t;
            opstack_push(t);
        }
        if (prefix_op != OP_generic) {
            ph1_ir = add_ph1_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = 1;
            ph1_ir->dest = vd;
            opstack_push(vd);

            ph1_ir = add_ph1_ir(prefix_op);
            ph1_ir->src1 = opstack_pop();
            if (lvalue->is_reference)
                ph1_ir->src0 = opstack_pop();
            else
                ph1_ir->src0 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;

            if (lvalue->is_reference) {
                ph1_ir = add_ph1_ir(OP_write);
                ph1_ir->src0 = vd;
                ph1_ir->dest = opstack_pop();
                ph1_ir->size = lvalue->size;
            } else {
                ph1_ir = add_ph1_ir(OP_assign);
                ph1_ir->src0 = vd;
                ph1_ir->dest = operand_stack[operand_stack_idx - 1];
            }
        } else if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
            side_effect[se_idx].op = OP_load_constant;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = 1;
            side_effect[se_idx].dest = vd;
            se_idx++;

            side_effect[se_idx].op = lex_accept(T_increment) ? OP_add : OP_sub;
            side_effect[se_idx].src1 = vd;
            if (lvalue->is_reference)
                side_effect[se_idx].src0 = opstack_pop();
            else
                side_effect[se_idx].src0 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            side_effect[se_idx].dest = vd;
            se_idx++;

            if (lvalue->is_reference) {
                side_effect[se_idx].op = OP_write;
                side_effect[se_idx].src0 = vd;
                side_effect[se_idx].dest = opstack_pop();
                side_effect[se_idx].size = lvalue->size;
                opstack_push(t);
                se_idx++;
            } else {
                side_effect[se_idx].op = OP_assign;
                side_effect[se_idx].src0 = vd;
                side_effect[se_idx].dest = operand_stack[operand_stack_idx - 1];
                se_idx++;
            }
        } else {
            if (lvalue->is_reference) {
                /* pop the address and keep the read value */
                t = opstack_pop();
                opstack_pop();
                opstack_push(t);
            }
        }
    }
}

void read_ternary_operation(block_t *parent)
{
    ph1_ir_t *ph1_ir;
    var_t *vd, *var;
    char true_label[MAX_VAR_LEN], false_label[MAX_VAR_LEN],
        end_label[MAX_VAR_LEN];

    strcpy(true_label, gen_label());
    strcpy(false_label, gen_label());
    strcpy(end_label, gen_label());

    if (!lex_accept(T_question))
        return;

    /* ternary-operator */
    ph1_ir = add_ph1_ir(OP_branch);
    ph1_ir->dest = opstack_pop();
    vd = require_var(parent);
    strcpy(vd->var_name, true_label);
    ph1_ir->src0 = vd;
    vd = require_var(parent);
    strcpy(vd->var_name, false_label);
    ph1_ir->src1 = vd;

    /* true branch */
    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, true_label);
    ph1_ir->src0 = vd;

    read_expr(parent);
    if (!lex_accept(T_colon))
        return;

    ph1_ir = add_ph1_ir(OP_assign);
    ph1_ir->src0 = opstack_pop();
    var = require_var(parent);
    strcpy(var->var_name, gen_name());
    ph1_ir->dest = var;

    /* jump true branch to end of expression */
    ph1_ir = add_ph1_ir(OP_jump);
    vd = require_var(parent);
    strcpy(vd->var_name, end_label);
    ph1_ir->dest = vd;

    /* false branch */
    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, false_label);
    ph1_ir->src0 = vd;
    read_expr(parent);

    ph1_ir = add_ph1_ir(OP_assign);
    ph1_ir->src0 = opstack_pop();
    ph1_ir->dest = var;

    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, end_label);
    ph1_ir->src0 = vd;

    opstack_push(var);
}

int read_body_assignment(char *token, block_t *parent, opcode_t prefix_op)
{
    var_t *var = find_local_var(token, parent);
    if (!var)
        var = find_global_var(token);
    if (var) {
        ph1_ir_t *ph1_ir;
        int one = 0;
        opcode_t op = OP_generic;
        lvalue_t lvalue;
        int size = 0;

        /* has memory address that we want to set */
        read_lvalue(&lvalue, var, parent, 0, OP_generic);
        size = lvalue.size;

        if (lex_accept(T_increment)) {
            op = OP_add;
            one = 1;
        } else if (lex_accept(T_decrement)) {
            op = OP_sub;
            one = 1;
        } else if (lex_accept(T_pluseq)) {
            op = OP_add;
        } else if (lex_accept(T_minuseq)) {
            op = OP_sub;
        } else if (lex_accept(T_oreq)) {
            op = OP_bit_or;
        } else if (lex_accept(T_andeq)) {
            op = OP_bit_and;
        } else if (lex_peek(T_open_bracket, NULL)) {
            var_t *vd;
            /* dereference lvalue into function address */
            ph1_ir = add_ph1_ir(OP_read);
            ph1_ir->src0 = opstack_pop();
            ph1_ir->size = PTR_SIZE;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);

            read_indirect_call(parent);
            return 1;
        } else if (prefix_op == OP_generic) {
            lex_expect(T_assign);
        } else {
            op = prefix_op;
            one = 1;
        }

        if (op != OP_generic) {
            var_t *vd, *t;
            int increment_size = 1;

            /* if we have a pointer, shift it by element size */
            if (lvalue.is_ptr)
                increment_size = lvalue.type->size;

            /**
             * If operand is a reference, read the value and push to stack
             * for the incoming addition/subtraction. Otherwise, use the
             * top element of stack as the one of operands and the destination.
             */
            if (one == 1) {
                if (lvalue.is_reference) {
                    ph1_ir = add_ph1_ir(OP_read);
                    t = opstack_pop();
                    ph1_ir->src0 = t;
                    ph1_ir->size = lvalue.size;
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);
                } else
                    t = operand_stack[operand_stack_idx - 1];

                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                vd->init_val = increment_size;
                ph1_ir->dest = vd;

                ph1_ir = add_ph1_ir(op);
                ph1_ir->src1 = vd;
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;

                if (lvalue.is_reference) {
                    ph1_ir = add_ph1_ir(OP_write);
                    ph1_ir->src0 = vd;
                    ph1_ir->dest = t;
                    ph1_ir->size = size;
                } else {
                    ph1_ir = add_ph1_ir(OP_assign);
                    ph1_ir->src0 = vd;
                    ph1_ir->dest = t;
                }
            } else {
                if (lvalue.is_reference) {
                    ph1_ir = add_ph1_ir(OP_read);
                    t = opstack_pop();
                    ph1_ir->src0 = t;
                    vd = require_var(parent);
                    ph1_ir->size = lvalue.size;
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);
                } else
                    t = operand_stack[operand_stack_idx - 1];

                read_expr(parent);

                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                vd->init_val = increment_size;
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);

                ph1_ir = add_ph1_ir(OP_mul);
                ph1_ir->src1 = opstack_pop();
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);

                ph1_ir = add_ph1_ir(op);
                ph1_ir->src1 = opstack_pop();
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;

                if (lvalue.is_reference) {
                    ph1_ir = add_ph1_ir(OP_write);
                    ph1_ir->src0 = vd;
                    ph1_ir->dest = t;
                    ph1_ir->size = lvalue.size;
                } else {
                    ph1_ir = add_ph1_ir(OP_assign);
                    ph1_ir->src0 = vd;
                    ph1_ir->dest = t;
                }
            }
        } else {
            read_expr(parent);
            read_ternary_operation(parent);

            if (lvalue.is_func) {
                ph1_ir = add_ph1_ir(OP_write);
                ph1_ir->src0 = opstack_pop();
                ph1_ir->dest = opstack_pop();
            } else if (lvalue.is_reference) {
                ph1_ir = add_ph1_ir(OP_write);
                ph1_ir->src0 = opstack_pop();
                ph1_ir->dest = opstack_pop();
                ph1_ir->size = size;
            } else {
                ph1_ir = add_ph1_ir(OP_assign);
                ph1_ir->src0 = opstack_pop();
                ph1_ir->dest = opstack_pop();
            }
        }
        return 1;
    }
    return 0;
}

int read_numeric_sconstant()
{
    /* return signed constant */
    int isneg = 0, res;
    char buffer[10];
    if (lex_accept(T_minus))
        isneg = 1;
    if (lex_peek(T_numeric, buffer))
        res = read_numeric_constant(buffer);
    else
        error("Invalid value after assignment");
    lex_expect(T_numeric);
    if (isneg)
        return (-1) * res;
    return res;
}

int eval_expression_imm(opcode_t op, int op1, int op2)
{
    /* return immediate result */
    int tmp = op2;
    int res = 0;
    switch (op) {
    case OP_add:
        res = op1 + op2;
        break;
    case OP_sub:
        res = op1 - op2;
        break;
    case OP_mul:
        res = op1 * op2;
        break;
    case OP_div:
        res = op1 / op2;
        break;
    case OP_mod:
        /* TODO: provide arithmetic & operation instead of '&=' */
        /* TODO: do optimization for local expression */
        tmp &= (tmp - 1);
        if ((op2 != 0) && (tmp == 0)) {
            res = op1;
            res &= (op2 - 1);
        } else
            res = op1 % op2;
        break;
    case OP_lshift:
        res = op1 << op2;
        break;
    case OP_rshift:
        res = op1 >> op2;
        break;
    case OP_lt:
        res = op1 < op2 ? 1 : 0;
        break;
    case OP_gt:
        res = op1 > op2 ? 1 : 0;
        break;
    case OP_leq:
        res = op1 <= op2 ? 1 : 0;
        break;
    case OP_geq:
        res = op1 >= op2 ? 1 : 0;
        break;
    default:
        error("The requested operation is not supported.");
    }
    return res;
}

int read_global_assignment(char *token);
void eval_ternary_imm(int cond, char *token)
{
    if (cond == 0) {
        while (next_token != T_colon) {
            next_token = get_next_token();
        };
        lex_accept(T_colon);
        read_global_assignment(token);
    } else {
        read_global_assignment(token);
        lex_expect(T_colon);
        while (!lex_peek(T_semicolon, NULL)) {
            next_token = get_next_token();
        }
    }
}

int read_global_assignment(char *token)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    block_t *parent = &BLOCKS[0];

    /* global initialization must be constant */
    var_t *var = find_global_var(token);
    if (var) {
        opcode_t op_stack[10];
        opcode_t op, next_op;
        int val_stack[10];
        int op_stack_index = 0, val_stack_index = 0;
        int operand1, operand2;
        operand1 = read_numeric_sconstant();
        op = get_operator();
        /* only one value after assignment */
        if (op == OP_generic) {
            ph1_ir = add_global_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = operand1;
            ph1_ir->dest = vd;

            ph1_ir = add_global_ir(OP_assign);
            ph1_ir->src0 = vd;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = opstack_pop();
            return 1;
        } else if (op == OP_ternary) {
            lex_expect(T_question);
            eval_ternary_imm(operand1, token);
            return 1;
        }
        operand2 = read_numeric_sconstant();
        next_op = get_operator();
        if (next_op == OP_generic) {
            /* only two operands, apply and return */
            ph1_ir = add_global_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = eval_expression_imm(op, operand1, operand2);
            ph1_ir->dest = vd;

            ph1_ir = add_global_ir(OP_assign);
            ph1_ir->src0 = vd;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = opstack_pop();
            return 1;
        } else if (op == OP_ternary) {
            int cond;
            lex_expect(T_question);
            cond = eval_expression_imm(op, operand1, operand2);
            eval_ternary_imm(cond, token);
            return 1;
        }


        /* using stack if operands more than two */
        op_stack[op_stack_index++] = op;
        op = next_op;
        val_stack[val_stack_index++] = operand1;
        val_stack[val_stack_index++] = operand2;

        while (op != OP_generic && op != OP_ternary) {
            if (op_stack_index > 0) {
                /* we have a continuation, use stack */
                int same_op = 0;
                do {
                    opcode_t stack_op = op_stack[op_stack_index - 1];
                    if (get_operator_prio(stack_op) >= get_operator_prio(op)) {
                        operand1 = val_stack[val_stack_index - 2];
                        operand2 = val_stack[val_stack_index - 1];
                        val_stack_index -= 2;

                        /* apply stack operator and push result back */
                        val_stack[val_stack_index++] =
                            eval_expression_imm(stack_op, operand1, operand2);

                        /* pop op stack */
                        op_stack_index--;
                    } else {
                        same_op = 1;
                    }
                    /* continue util next operation is higher prio */
                } while (op_stack_index > 0 && same_op == 0);
            }
            /* push next operand on stack */
            val_stack[val_stack_index++] = read_numeric_sconstant();
            /* push operator on stack */
            op_stack[op_stack_index++] = op;
            op = get_operator();
        }
        /* unwind stack and apply operations */
        while (op_stack_index > 0) {
            opcode_t stack_op = op_stack[op_stack_index - 1];

            /* pop stack and apply operators */
            operand1 = val_stack[val_stack_index - 2];
            operand2 = val_stack[val_stack_index - 1];
            val_stack_index -= 2;

            /* apply stack operator and push value back on stack */
            val_stack[val_stack_index++] =
                eval_expression_imm(stack_op, operand1, operand2);

            if (op_stack_index == 1) {
                if (op == OP_ternary) {
                    lex_expect(T_question);
                    eval_ternary_imm(val_stack[0], token);
                } else {
                    ph1_ir = add_global_ir(OP_load_constant);
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    vd->init_val = val_stack[0];
                    ph1_ir->dest = vd;

                    ph1_ir = add_global_ir(OP_assign);
                    ph1_ir->src0 = vd;
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = opstack_pop();
                }
                return 1;
            }

            /* pop op stack */
            op_stack_index--;
        }
        if (op == OP_ternary) {
            lex_expect(T_question);
            eval_ternary_imm(val_stack[0], token);
        } else {
            ph1_ir = add_global_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = val_stack[0];
            ph1_ir->dest = vd;

            ph1_ir = add_global_ir(OP_assign);
            ph1_ir->src0 = vd;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = opstack_pop();
        }
        return 1;
    }
    return 0;
}

var_t *break_exit[MAX_NESTING];
int break_exit_idx = 0;
var_t *continue_pos[MAX_NESTING];
int continue_pos_idx = 0;

void perform_side_effect()
{
    int i;
    for (i = 0; i < se_idx; i++) {
        ph1_ir_t *ph1_ir = add_ph1_ir(side_effect[i].op);
        memcpy(ph1_ir, &side_effect[i], sizeof(ph1_ir_t));
    }
    se_idx = 0;
}

void read_code_block(func_t *func, macro_t *macro, block_t *parent);

void read_body_statement(block_t *parent)
{
    char token[MAX_ID_LEN];
    ph1_ir_t *ph1_ir;
    macro_t *mac;
    func_t *fn;
    type_t *type;
    var_t *vd, *var;
    opcode_t prefix_op = OP_generic;

    /* statement can be:
     *   function call, variable declaration, assignment operation,
     *   keyword, block
     */

    if (lex_peek(T_open_curly, NULL)) {
        read_code_block(parent->func, parent->macro, parent);
        return;
    }

    if (lex_accept(T_return)) {
        /* return void */
        if (lex_accept(T_semicolon)) {
            add_ph1_ir(OP_return);
            return;
        }

        /* get expression value into return value */
        read_expr(parent);
        read_ternary_operation(parent);

        /* apply side effect before function return */
        perform_side_effect();
        lex_expect(T_semicolon);

        ph1_ir = add_ph1_ir(OP_return);
        ph1_ir->src0 = opstack_pop();
        return;
    }

    if (lex_accept(T_if)) {
        char label_true[MAX_VAR_LEN], label_false[MAX_VAR_LEN],
            label_endif[MAX_VAR_LEN];
        strcpy(label_true, gen_label());
        strcpy(label_false, gen_label());
        strcpy(label_endif, gen_label());

        lex_expect(T_open_bracket);
        read_expr(parent);
        lex_expect(T_close_bracket);

        ph1_ir = add_ph1_ir(OP_branch);
        ph1_ir->dest = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, label_true);
        ph1_ir->src0 = vd;
        vd = require_var(parent);
        strcpy(vd->var_name, label_false);
        ph1_ir->src1 = vd;

        ph1_ir = add_ph1_ir(OP_label);
        vd = require_var(parent);
        strcpy(vd->var_name, label_true);
        ph1_ir->src0 = vd;

        read_body_statement(parent);

        /* if we have an "else" block, jump to finish */
        if (lex_accept(T_else)) {
            /* jump true branch to finish */
            ph1_ir = add_ph1_ir(OP_jump);
            vd = require_var(parent);
            strcpy(vd->var_name, label_endif);
            ph1_ir->dest = vd;

            /* false branch */
            ph1_ir = add_ph1_ir(OP_label);
            vd = require_var(parent);
            strcpy(vd->var_name, label_false);
            ph1_ir->src0 = vd;

            read_body_statement(parent);

            ph1_ir = add_ph1_ir(OP_label);
            vd = require_var(parent);
            strcpy(vd->var_name, label_endif);
            ph1_ir->src0 = vd;
        } else {
            /* this is done, and link false jump */
            ph1_ir = add_ph1_ir(OP_label);
            vd = require_var(parent);
            strcpy(vd->var_name, label_false);
            ph1_ir->src0 = vd;
        }
        return;
    }

    if (lex_accept(T_while)) {
        var_t *var_continue, *var_break;
        char label_start[MAX_VAR_LEN], label_body[MAX_VAR_LEN],
            label_end[MAX_VAR_LEN];
        strcpy(label_start, gen_label());
        strcpy(label_body, gen_label());
        strcpy(label_end, gen_label());

        ph1_ir = add_ph1_ir(OP_label);
        var_continue = require_var(parent);
        strcpy(var_continue->var_name, label_start);
        ph1_ir->src0 = var_continue;

        continue_pos[continue_pos_idx++] = var_continue;
        var_break = require_var(parent);
        strcpy(var_break->var_name, label_end);
        break_exit[break_exit_idx++] = var_break;

        lex_expect(T_open_bracket);
        read_expr(parent);
        lex_expect(T_close_bracket);

        ph1_ir = add_ph1_ir(OP_branch);
        ph1_ir->dest = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, label_body);
        ph1_ir->src0 = vd;
        vd = require_var(parent);
        strcpy(vd->var_name, label_end);
        ph1_ir->src1 = vd;

        ph1_ir = add_ph1_ir(OP_label);
        vd = require_var(parent);
        strcpy(vd->var_name, label_body);
        ph1_ir->src0 = vd;

        read_body_statement(parent);

        continue_pos_idx--;
        break_exit_idx--;

        /* create exit jump for breaks */
        ph1_ir = add_ph1_ir(OP_jump);
        vd = require_var(parent);
        strcpy(vd->var_name, label_start);
        ph1_ir->dest = vd;

        ph1_ir = add_ph1_ir(OP_label);
        ph1_ir->src0 = var_break;

        /* workaround to keep variables alive */
        var_continue->init_val = ph1_ir_idx - 1;
        return;
    }

    if (lex_accept(T_switch)) {
        var_t *var_break;
        int is_default = 0;
        char true_label[MAX_VAR_LEN], false_label[MAX_VAR_LEN];
        strcpy(true_label, gen_label());
        strcpy(false_label, gen_label());

        lex_expect(T_open_bracket);
        read_expr(parent);
        lex_expect(T_close_bracket);

        /* create exit jump for breaks */
        var_break = require_var(parent);
        break_exit[break_exit_idx++] = var_break;

        lex_expect(T_open_curly);
        while (lex_peek(T_default, NULL) || lex_peek(T_case, NULL)) {
            if (lex_accept(T_default))
                is_default = 1;
            else {
                int case_val;

                lex_accept(T_case);
                if (lex_peek(T_numeric, NULL)) {
                    case_val = read_numeric_constant(token_str);
                    lex_expect(T_numeric); /* already read it */
                } else {
                    constant_t *cd = find_constant(token_str);
                    case_val = cd->value;
                    lex_expect(T_identifier); /* already read it */
                }

                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                vd->init_val = case_val;
                ph1_ir->dest = vd;
                opstack_push(vd);

                ph1_ir = add_ph1_ir(OP_eq);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->src0 = opstack_pop();
                ph1_ir->src1 = operand_stack[operand_stack_idx - 1];
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;

                ph1_ir = add_ph1_ir(OP_branch);
                ph1_ir->dest = vd;
                vd = require_var(parent);
                strcpy(vd->var_name, true_label);
                ph1_ir->src0 = vd;
                vd = require_var(parent);
                strcpy(vd->var_name, false_label);
                ph1_ir->src1 = vd;
            }
            lex_expect(T_colon);

            /* body is optional, can be another case */
            if (!is_default && !lex_peek(T_case, NULL) &&
                !lex_peek(T_close_curly, NULL) && !lex_peek(T_default, NULL)) {
                ph1_ir = add_ph1_ir(OP_label);
                vd = require_var(parent);
                strcpy(vd->var_name, true_label);
                ph1_ir->src0 = vd;

                /* only create new true label at the first line of case body */
                strcpy(true_label, gen_label());
            }

            while (!lex_peek(T_case, NULL) && !lex_peek(T_close_curly, NULL) &&
                   !lex_peek(T_default, NULL))
                read_body_statement(parent);

            ph1_ir = add_ph1_ir(OP_label);
            vd = require_var(parent);
            strcpy(vd->var_name, false_label);
            ph1_ir->src0 = vd;

            /* only create new false label at the last line of case body */
            strcpy(false_label, gen_label());
        }

        /* remove the expression in switch() */
        opstack_pop();
        lex_expect(T_close_curly);

        strcpy(var_break->var_name, vd->var_name);
        break_exit_idx--;
        return;
    }

    if (lex_accept(T_break)) {
        ph1_ir = add_ph1_ir(OP_jump);
        ph1_ir->dest = break_exit[break_exit_idx - 1];
    }

    if (lex_accept(T_continue)) {
        ph1_ir = add_ph1_ir(OP_jump);
        ph1_ir->dest = continue_pos[continue_pos_idx - 1];
    }

    if (lex_accept(T_for)) {
        var_t *var_condition, *var_break, *var_inc;
        char cond[MAX_VAR_LEN], body[MAX_VAR_LEN], inc[MAX_VAR_LEN],
            end[MAX_VAR_LEN];
        strcpy(cond, gen_label());
        strcpy(body, gen_label());
        strcpy(inc, gen_label());
        strcpy(end, gen_label());

        lex_expect(T_open_bracket);

        /* setup - execute once */
        if (!lex_accept(T_semicolon)) {
            lex_peek(T_identifier, token);
            read_body_assignment(token, parent, OP_generic);
            lex_expect(T_semicolon);
        }

        /* condition - check before the loop */
        ph1_ir = add_ph1_ir(OP_label);
        var_condition = require_var(parent);
        strcpy(var_condition->var_name, cond);
        ph1_ir->src0 = var_condition;
        if (!lex_accept(T_semicolon)) {
            read_expr(parent);
            lex_expect(T_semicolon);
        } else {
            /* always true */
            ph1_ir = add_ph1_ir(OP_load_constant);
            vd = require_var(parent);
            vd->init_val = 1;
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
        }

        ph1_ir = add_ph1_ir(OP_branch);
        ph1_ir->dest = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, body);
        ph1_ir->src0 = vd;
        vd = require_var(parent);
        strcpy(vd->var_name, end);
        ph1_ir->src1 = vd;

        var_break = require_var(parent);
        strcpy(var_break->var_name, end);

        break_exit[break_exit_idx++] = var_break;

        /* increment after each loop */
        ph1_ir = add_ph1_ir(OP_label);
        var_inc = require_var(parent);
        strcpy(var_inc->var_name, inc);
        ph1_ir->src0 = var_inc;

        continue_pos[continue_pos_idx++] = var_inc;

        if (!lex_accept(T_close_bracket)) {
            if (lex_accept(T_increment))
                prefix_op = OP_add;
            else if (lex_accept(T_decrement))
                prefix_op = OP_sub;
            lex_peek(T_identifier, token);
            read_body_assignment(token, parent, prefix_op);
            lex_expect(T_close_bracket);
        }

        /* jump back to condition */
        ph1_ir = add_ph1_ir(OP_jump);
        vd = require_var(parent);
        strcpy(vd->var_name, cond);
        ph1_ir->dest = vd;

        /* loop body */
        ph1_ir = add_ph1_ir(OP_label);
        vd = require_var(parent);
        strcpy(vd->var_name, body);
        ph1_ir->src0 = vd;
        read_body_statement(parent);

        /* jump to increment */
        ph1_ir = add_ph1_ir(OP_jump);
        vd = require_var(parent);
        strcpy(vd->var_name, inc);
        ph1_ir->dest = vd;

        ph1_ir = add_ph1_ir(OP_label);
        ph1_ir->src0 = var_break;

        var_condition->init_val = ph1_ir_idx - 1;

        continue_pos_idx--;
        break_exit_idx--;
        return;
    }

    if (lex_accept(T_do)) {
        var_t *var_start, *var_condition, *var_break;

        ph1_ir = add_ph1_ir(OP_label);
        var_start = require_var(parent);
        strcpy(var_start->var_name, gen_label());
        ph1_ir->src0 = var_start;

        var_condition = require_var(parent);
        strcpy(var_condition->var_name, gen_label());

        continue_pos[continue_pos_idx++] = var_condition;

        var_break = require_var(parent);
        strcpy(var_break->var_name, gen_label());

        break_exit[break_exit_idx++] = var_break;

        read_body_statement(parent);

        lex_expect(T_while);
        lex_expect(T_open_bracket);

        ph1_ir = add_ph1_ir(OP_label);
        vd = require_var(parent);
        strcpy(vd->var_name, var_condition->var_name);
        ph1_ir->src0 = vd;

        read_expr(parent);
        lex_expect(T_close_bracket);

        ph1_ir = add_ph1_ir(OP_branch);
        ph1_ir->dest = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, var_start->var_name);
        ph1_ir->src0 = vd;
        vd = require_var(parent);
        strcpy(vd->var_name, var_break->var_name);
        ph1_ir->src1 = vd;

        ph1_ir = add_ph1_ir(OP_label);
        ph1_ir->src0 = var_break;

        var_start->init_val = ph1_ir_idx - 1;
        lex_expect(T_semicolon);

        continue_pos_idx--;
        break_exit_idx--;
        return;
    }

    /* empty statement */
    if (lex_accept(T_semicolon))
        return;

    /* statement with prefix */
    if (lex_accept(T_increment))
        prefix_op = OP_add;
    else if (lex_accept(T_decrement))
        prefix_op = OP_sub;
    /* must be an identifier */
    if (!lex_peek(T_identifier, token))
        error("Unexpected token");

    /* is it a variable declaration? */
    type = find_type(token);
    if (type) {
        var = require_var(parent);
        read_full_var_decl(var, 0, 0);
        if (lex_accept(T_assign)) {
            read_expr(parent);
            read_ternary_operation(parent);

            ph1_ir = add_ph1_ir(OP_assign);
            ph1_ir->src0 = opstack_pop();
            ph1_ir->dest = var;
        }
        while (lex_accept(T_comma)) {
            var_t *nv;

            /* add sequence point at T_comma */
            perform_side_effect();

            /* multiple (partial) declarations */
            nv = require_var(parent);
            read_partial_var_decl(nv, var); /* partial */
            if (lex_accept(T_assign)) {
                read_expr(parent);

                ph1_ir = add_ph1_ir(OP_assign);
                ph1_ir->src0 = opstack_pop();
                ph1_ir->dest = nv;
            }
        }
        lex_expect(T_semicolon);
        return;
    }

    mac = find_macro(token);
    if (mac) {
        if (parent->macro)
            error("Nested macro is not yet supported");

        parent->macro = mac;
        mac->num_params = 0;
        lex_expect(T_identifier);

        /* `source_idx` has pointed at the first parameter */
        while (!lex_peek(T_close_bracket, NULL)) {
            mac->params[mac->num_params++] = source_idx;
            do {
                next_token = get_next_token();
            } while (next_token != T_comma && next_token != T_close_bracket);
        }
        /* move `source_idx` to the macro body */
        macro_return_idx = source_idx;
        source_idx = mac->start_source_idx;
        next_char = SOURCE[source_idx];
        lex_expect(T_close_bracket);

        skip_newline = 0;
        read_body_statement(parent);

        /* cleanup */
        skip_newline = 1;
        parent->macro = NULL;
        macro_return_idx = 0;
        return;
    }

    /* is a function call? */
    fn = find_func(token);
    if (fn) {
        lex_expect(T_identifier);
        read_func_call(fn, parent);
        perform_side_effect();
        lex_expect(T_semicolon);
        return;
    }

    /* is an assignment? */
    if (read_body_assignment(token, parent, prefix_op)) {
        perform_side_effect();
        lex_expect(T_semicolon);
        return;
    }

    error("Unrecognized statement token");
}

void read_code_block(func_t *func, macro_t *macro, block_t *parent)
{
    block_t *blk = add_block(parent, func, macro);

    add_ph1_ir(OP_block_start);
    lex_expect(T_open_curly);

    while (!lex_accept(T_close_curly)) {
        read_body_statement(blk);
        perform_side_effect();
    }

    add_ph1_ir(OP_block_end);
}

void read_func_body(func_t *fdef)
{
    read_code_block(fdef, NULL, NULL);
}

/* if first token is type */
void read_global_decl(block_t *block)
{
    var_t *var = require_var(block);
    var->is_global = 1;

    /* new function, or variables under parent */
    read_full_var_decl(var, 0, 0);

    if (lex_peek(T_open_bracket, NULL)) {
        /* function */
        func_t *fd = add_func(var->var_name);
        memcpy(&fd->return_def, var, sizeof(var_t));
        block->next_local--;

        read_parameter_list_decl(fd, 0);

        if (lex_peek(T_open_curly, NULL)) {
            ph1_ir_t *ph1_ir = add_ph1_ir(OP_define);
            strcpy(ph1_ir->func_name, var->var_name);

            read_func_body(fd);
            return;
        }
        if (lex_accept(T_semicolon)) /* forward definition */
            return;
        error("Syntax error in global declaration");
    }

    /* NO support for global initialization */
    /* is a variable */
    if (lex_accept(T_assign)) {
        if (var->is_ptr == 0 && var->array_size == 0) {
            read_global_assignment(var->var_name);
            lex_expect(T_semicolon);
            return;
        }
        /* TODO: support global initialization for array and pointer */
        error("Global initialization for array and pointer not supported");
    } else if (lex_accept(T_comma))
        /* TODO: continuation */
        error("Global continuation not supported");
    else if (lex_accept(T_semicolon)) {
        opstack_pop();
        return;
    }
    error("Syntax error in global declaration");
}

void read_global_statement()
{
    char token[MAX_ID_LEN];
    block_t *block = &BLOCKS[0]; /* global block */

    if (lex_peek(T_include, token)) {
        if (!strcmp(token_str, "<stdio.h>")) {
            /* ignore, we include libc by default */
        }
        lex_expect(T_include);
    } else if (lex_accept(T_define)) {
        char alias[MAX_VAR_LEN];
        char value[MAX_VAR_LEN];

        lex_peek(T_identifier, alias);
        lex_expect(T_identifier);
        if (lex_peek(T_numeric, value)) {
            lex_expect(T_numeric);
            add_alias(alias, value);
        } else if (lex_peek(T_string, value)) {
            lex_expect(T_string);
            add_alias(alias, value);
        } else if (lex_accept(T_open_bracket)) { /* function-like macro */
            macro_t *macro = add_macro(alias);

            skip_newline = 0;
            while (lex_peek(T_identifier, alias)) {
                lex_expect(T_identifier);
                strcpy(macro->param_defs[macro->num_param_defs++].var_name,
                       alias);
                lex_accept(T_comma);
            }
            if (lex_accept(T_elipsis))
                macro->is_variadic = 1;

            macro->start_source_idx = source_idx;
            skip_macro_body();
        }
    } else if (lex_peek(T_undef, token)) {
        char alias[MAX_VAR_LEN];

        preproc_aliasing = 0;
        lex_expect(T_undef);
        lex_peek(T_identifier, alias);
        preproc_aliasing = 1;
        lex_expect(T_identifier);

        remove_alias(alias);
        remove_macro(alias);
    } else if (lex_peek(T_error, NULL)) {
        int i = 0;
        char error_diagnostic[MAX_LINE_LEN];

        do {
            error_diagnostic[i++] = next_char;
        } while (read_char(0) != '\n');
        error_diagnostic[i] = 0;

        error(error_diagnostic);
    } else if (lex_accept(T_typedef)) {
        if (lex_accept(T_enum)) {
            int val = 0;
            type_t *type = add_type();

            type->base_type = TYPE_int;
            type->size = 4;
            lex_expect(T_open_curly);
            do {
                lex_ident(T_identifier, token);
                if (lex_accept(T_assign)) {
                    char value[MAX_ID_LEN];
                    lex_ident(T_numeric, value);
                    val = read_numeric_constant(value);
                }
                add_constant(token, val++);
            } while (lex_accept(T_comma));
            lex_expect(T_close_curly);
            lex_ident(T_identifier, token);
            strcpy(type->type_name, token);
            lex_expect(T_semicolon);
        } else if (lex_accept(T_struct)) {
            int i = 0, size = 0;
            type_t *type = add_type();

            if (lex_peek(T_identifier, token)) /* recursive declaration */
                lex_accept(T_identifier);
            lex_expect(T_open_curly);
            do {
                var_t *v = &type->fields[i++];
                read_full_var_decl(v, 0, 1);
                v->offset = size;
                size += size_var(v);
                lex_expect(T_semicolon);
            } while (!lex_accept(T_close_curly));

            lex_ident(T_identifier, token); /* type name */
            strcpy(type->type_name, token);
            type->size = size;
            type->num_fields = i;
            type->base_type = TYPE_struct; /* is used? */
            lex_expect(T_semicolon);
        } else {
            char base_type[MAX_TYPE_LEN];
            type_t *base;
            type_t *type = add_type();
            lex_ident(T_identifier, base_type);
            base = find_type(base_type);
            if (!base)
                error("Unable to find base type");
            type->base_type = base->base_type;
            type->size = base->size;
            type->num_fields = 0;
            lex_ident(T_identifier, type->type_name);
            lex_expect(T_semicolon);
        }
    } else if (lex_peek(T_identifier, NULL)) {
        read_global_decl(block);
    } else
        error("Syntax error in global statement");
}

void parse_internal()
{
    /* parser initialization */
    type_t *type;
    func_t *fn;

    /* built-in types */
    type = add_named_type("void");
    type->base_type = TYPE_void;
    type->size = 0;

    type = add_named_type("char");
    type->base_type = TYPE_char;
    type->size = 1;

    type = add_named_type("int");
    type->base_type = TYPE_int;
    type->size = 4;

    add_block(NULL, NULL, NULL); /* global block */
    elf_add_symbol("", 0, 0);    /* undef symbol */

    /* architecture defines */
    add_alias(ARCH_PREDEFINED, "1");

    /* Linux syscall */
    fn = add_func("__syscall");
    fn->va_args = 1;

    /* lexer initialization */
    source_idx = 0;
    next_char = SOURCE[0];
    lex_expect(T_start);

    do {
        read_global_statement();
    } while (!lex_accept(T_eof));
}

/* Load specified source file and referred inclusion recursively */
void load_source_file(char *file)
{
    char buffer[MAX_LINE_LEN];

    FILE *f = fopen(file, "rb");
    if (!f)
        abort();

    for (;;) {
        if (!fgets(buffer, MAX_LINE_LEN, f)) {
            fclose(f);
            return;
        }
        if (!strncmp(buffer, "#include ", 9) && (buffer[9] == '"')) {
            char path[MAX_LINE_LEN];
            int c = strlen(file) - 1;
            while (c > 0 && file[c] != '/')
                c--;
            if (c) {
                /* prepend directory name */
                strncpy(path, file, c + 1);
                c++;
            }
            path[c] = 0;
            buffer[strlen(buffer) - 2] = 0;
            strcpy(path + c, buffer + 10);
            load_source_file(path);
        } else {
            strcpy(SOURCE + source_idx, buffer);
            source_idx += strlen(buffer);
        }
    }
    fclose(f);
}

void parse(char *file)
{
    load_source_file(file);
    parse_internal();
}
