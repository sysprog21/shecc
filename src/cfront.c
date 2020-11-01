/* C language front-end */

int is_whitespace(char c)
{
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t')
        return 1;
    return 0;
}

/* is it alphabet, number or '_'? */
int is_alnum(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || (c == '_'))
        return 1;
    return 0;
}

int is_digit(char c)
{
    if (c >= '0' && c <= '9')
        return 1;
    return 0;
}

int is_hex(char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == 'x' ||
        (c >= 'A' && c <= 'F'))
        return 1;
    return 0;
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
    T_bit_or,        /* | */
    T_bit_xor,       /* ^ */
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

void skip_whitespace()
{
    while (is_whitespace(next_char))
        next_char = SOURCE[++source_idx];
}

char read_char(int is_skip_space)
{
    next_char = SOURCE[++source_idx];
    if (is_skip_space == 1)
        skip_whitespace();
    return next_char;
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

        if (strcmp(token_str, "#include") == 0) {
            i = 0;
            do {
                token_str[i++] = next_char;
            } while (read_char(0) != '\n');
            skip_whitespace();
            return T_include;
        }
        if (strcmp(token_str, "#define") == 0) {
            skip_whitespace();
            return T_define;
        }
        if (strcmp(token_str, "#ifdef") == 0) {
            i = 0;
            do {
                token_str[i++] = next_char;
            } while (read_char(0) != '\n');
            token_str[i] = 0;
            /* check if we have this alias/define */
            for (i = 0; i < aliases_idx; i++) {
                if (strcmp(token_str, ALIASES[i].alias) == 0) {
                    skip_whitespace();
                    return get_next_token();
                }
            }
            /* skip lines until #endif */
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
        if (strcmp(token_str, "#endif") == 0) {
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

        if (strcmp(token_str, "if") == 0)
            return T_if;
        if (strcmp(token_str, "while") == 0)
            return T_while;
        if (strcmp(token_str, "for") == 0)
            return T_for;
        if (strcmp(token_str, "do") == 0)
            return T_do;
        if (strcmp(token_str, "else") == 0)
            return T_else;
        if (strcmp(token_str, "return") == 0)
            return T_return;
        if (strcmp(token_str, "typedef") == 0)
            return T_typedef;
        if (strcmp(token_str, "enum") == 0)
            return T_enum;
        if (strcmp(token_str, "struct") == 0)
            return T_struct;
        if (strcmp(token_str, "sizeof") == 0)
            return T_sizeof;
        if (strcmp(token_str, "switch") == 0)
            return T_switch;
        if (strcmp(token_str, "case") == 0)
            return T_case;
        if (strcmp(token_str, "break") == 0)
            return T_break;
        if (strcmp(token_str, "default") == 0)
            return T_default;
        if (strcmp(token_str, "continue") == 0)
            return T_continue;

        alias = find_alias(token_str);
        if (alias) {
            strcpy(token_str, alias);
            return T_numeric;
        }

        return T_identifier;
    }
    error("Unrecognized input");
    return T_eof;
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
        if (value == NULL)
            return 1;
        strcpy(value, token_str);
        return 1;
    }
    return 0;
}

void lex_indent(token_t token, char *value)
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

void read_expr(int param_no, block_t *parent);

int write_symbol(char *data, int len)
{
    int startLen = elf_data_idx;
    elf_write_data_str(data, len);
    return startLen;
}

int get_size(var_t *var, type_t *type)
{
    if (var->is_ptr)
        return PTR_SIZE;
    return type->size;
}

int break_level;
int continue_level;

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

void read_inner_var_decl(var_t *vd)
{
    vd->init_val = 0;
    if (lex_accept(T_asterisk))
        vd->is_ptr = 1;
    else
        vd->is_ptr = 0;
    lex_indent(T_identifier, vd->var_name);
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
}

/* starting next_token, need to check the type */
void read_full_var_decl(var_t *vd)
{
    lex_accept(T_struct); /* ignore struct definition */
    lex_indent(T_identifier, vd->type_name);
    read_inner_var_decl(vd);
}

/* starting next_token, need to check the type */
void read_partial_var_decl(var_t *vd, var_t *template)
{
    strcpy(vd->type_name, template->type_name);
    read_inner_var_decl(vd);
}

int read_parameter_list_decl(var_t vds[])
{
    int vn = 0;
    lex_expect(T_open_bracket);
    while (lex_peek(T_identifier, NULL) == 1) {
        read_full_var_decl(&vds[vn++]);
        lex_accept(T_comma);
    }
    if (lex_accept(T_elipsis)) {
        /* variadic function. Max 8 parameters are passed.
         * create dummy parameters to put all on stack
         */
        for (; vn < MAX_PARAMS; vn++) {
            strcpy(vds[vn].type_name, "int");
            strcpy(vds[vn].var_name, "var_arg");
            vds[vn].is_ptr = 1;
        }
    }
    lex_expect(T_close_bracket);
    return vn;
}

void read_literal_param(int param_no)
{
    char literal[MAX_TOKEN_LEN];
    ir_instr_t *ii;
    int index;

    lex_indent(T_string, literal);

    index = write_symbol(literal, strlen(literal) + 1);
    ii = add_instr(OP_load_data_address);
    ii->param_no = param_no;
    ii->int_param1 = index;
}

void read_numeric_param(int param_no, int isneg)
{
    char token[MAX_ID_LEN];
    int value = 0;
    int i = 0;
    ir_instr_t *ii;
    char c;

    lex_indent(T_numeric, token);

    if (token[0] == '-') {
        isneg = 1 - isneg;
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

    ii = add_instr(OP_load_constant);
    ii->param_no = param_no;
    if (isneg)
        value = -value;
    ii->int_param1 = value;
}

void read_char_param(int param_no)
{
    ir_instr_t *ii;
    char token[5];

    lex_indent(T_char, token);

    ii = add_instr(OP_load_constant);
    ii->param_no = param_no;
    ii->int_param1 = token[0];
}

void read_func_parameters(block_t *parent)
{
    int param_num = 0;
    lex_expect(T_open_bracket);
    while (!lex_accept(T_close_bracket)) {
        read_expr(param_num++, parent);
        lex_accept(T_comma);
    }
}

void read_func_call(func_t *fn, int param_no, block_t *parent)
{
    ir_instr_t *ii;

    /* already have function name in fn */
    lex_expect(T_identifier);
    read_func_parameters(parent);
    ii = add_instr(OP_call);
    ii->str_param1 = fn->return_def.var_name;
    ii->param_no = param_no; /* return value here */
}

void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 int param_no,
                 int eval,
                 opcode_t op);

/* Maintain a stack of expression values and operators, depending on next
 * operators' priority. Either apply it or operator on stack first.
 */
void read_expr_operand(int param_no, block_t *parent)
{
    int isneg = 0;
    if (lex_accept(T_minus)) {
        isneg = 1;
        if (lex_peek(T_numeric, NULL) == 0 &&
            lex_peek(T_identifier, NULL) == 0 &&
            lex_peek(T_open_bracket, NULL) == 0) {
            error("Unexpected token after unary minus");
        }
    }

    if (lex_peek(T_string, NULL))
        read_literal_param(param_no);
    else if (lex_peek(T_char, NULL))
        read_char_param(param_no);
    else if (lex_peek(T_numeric, NULL))
        read_numeric_param(param_no, isneg);
    else if (lex_accept(T_log_not)) {
        ir_instr_t *ii;
        read_expr_operand(param_no, parent);
        ii = add_instr(OP_not);
        ii->param_no = param_no;
    } else if (lex_accept(T_ampersand)) {
        char token[MAX_VAR_LEN];
        var_t *var;
        lvalue_t lvalue;

        lex_peek(T_identifier, token);
        var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, param_no, 0, OP_generic);
    } else if (lex_peek(T_asterisk, NULL)) {
        /* dereference */
        char token[MAX_VAR_LEN];
        var_t *var;
        lvalue_t lvalue;
        ir_instr_t *ii;

        lex_accept(T_open_bracket);
        lex_peek(T_identifier, token);
        var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, param_no, 1, OP_generic);
        lex_accept(T_close_bracket);
        ii = add_instr(OP_read);
        ii->param_no = param_no;
        ii->int_param1 = param_no;
        ii->int_param2 = lvalue.size;
    } else if (lex_accept(T_open_bracket)) {
        read_expr(param_no, parent);
        lex_expect(T_close_bracket);

        if (isneg) {
            ir_instr_t *ii = add_instr(OP_negate);
            ii->param_no = param_no;
        }
    } else if (lex_accept(T_sizeof)) {
        char token[MAX_TYPE_LEN];
        type_t *type;
        ir_instr_t *ii = add_instr(OP_load_constant);

        lex_expect(T_open_bracket);
        lex_indent(T_identifier, token);
        type = find_type(token);
        if (type == NULL)
            error("Unable to find type");

        ii->param_no = param_no;
        ii->int_param1 = type->size;
        lex_expect(T_close_bracket);
    } else {
        /* function call, constant or variable - read token and determine */
        opcode_t prefix_op = OP_generic;
        char token[MAX_ID_LEN];
        func_t *fn;
        var_t *var;
        constant_t *con;

        if (lex_accept(T_increment))
            prefix_op = OP_add;
        else if (lex_accept(T_decrement))
            prefix_op = OP_sub;

        lex_peek(T_identifier, token);

        /* is a constant or variable? */
        con = find_constant(token);
        var = find_var(token, parent);
        fn = find_func(token);

        if (con) {
            int value = con->value;
            ir_instr_t *ii = add_instr(OP_load_constant);
            ii->param_no = param_no;
            ii->int_param1 = value;
            lex_expect(T_identifier);
        } else if (var) {
            /* evalue lvalue expression */
            lvalue_t lvalue;
            read_lvalue(&lvalue, var, parent, param_no, 1, prefix_op);
        } else if (fn) {
            ir_instr_t *ii;
            int pn;
            for (pn = 0; pn < param_no; pn++) {
                ii = add_instr(OP_push);
                ii->param_no = pn;
            }

            /* we should push existing parameters onto the stack since
             * function calls use the same.
             */
            read_func_call(fn, param_no, parent);

            for (pn = param_no - 1; pn >= 0; pn--) {
                ii = add_instr(OP_pop);
                ii->param_no = pn;
            }
        } else {
            printf("%s\n", token);
            /* unknown expression */
            error("Unrecognized expression token");
        }

        if (isneg) {
            ir_instr_t *ii = add_instr(OP_negate);
            ii->param_no = param_no;
        }
    }
}

int get_operator_prio(opcode_t op)
{
    /* https://www.cs.uic.edu/~i109/Notes/COperatorPrecedenceTable.pdf */
    switch (op) {
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
    return op;
}

void read_expr(int param_no, block_t *parent)
{
    opcode_t op_stack[10];
    int op_stack_index = 0;
    opcode_t op, next_op;
    ir_instr_t *il;

    /* read value into param_no */
    read_expr_operand(param_no, parent);

    /* check for any operator following */
    op = get_operator();
    if (op == OP_generic) /* no continuation */
        return;

    read_expr_operand(param_no + 1, parent);
    next_op = get_operator();

    if (next_op == OP_generic) {
        /* only two operands, apply and return */
        il = add_instr(op);
        il->param_no = param_no;
        il->int_param1 = param_no + 1;
        return;
    }

    /* if more than two operands, then use stack */
    il = add_instr(OP_push);
    il->param_no = param_no;
    il = add_instr(OP_push);
    il->param_no = param_no + 1;
    op_stack[0] = op;
    op_stack_index++;
    op = next_op;

    while (op != OP_generic) {
        /* if we have operand on stack, compare priorities */
        if (op_stack_index > 0) {
            /* we have a continuation, use stack */
            int same_op = 0;
            do {
                opcode_t stack_op = op_stack[op_stack_index - 1];
                if (get_operator_prio(stack_op) >= get_operator_prio(op)) {
                    /* stack has higher priority operator i.e. 5 * 6 + _
                     * pop stack and apply operators.
                     */
                    il = add_instr(OP_pop);
                    il->param_no = param_no + 1;

                    il = add_instr(OP_pop);
                    il->param_no = param_no;

                    /* apply stack operator  */
                    il = add_instr(stack_op);
                    il->param_no = param_no;
                    il->int_param1 = param_no + 1;

                    /* push value back on stack */
                    il = add_instr(OP_push);
                    il->param_no = param_no;

                    /* pop op stack */
                    op_stack_index--;
                } else {
                    same_op = 1;
                }
                /* continue util next operation is higher prio, i.e. 5 + 6 * _
                 */
            } while (op_stack_index > 0 && same_op == 0);
        }

        /* push operator on stack */
        op_stack[op_stack_index++] = op;

        /* push value on stack */
        read_expr_operand(param_no, parent);
        il = add_instr(OP_push);
        il->param_no = param_no;

        op = get_operator();
    }

    /* unwind stack and apply operations */
    while (op_stack_index > 0) {
        opcode_t stack_op = op_stack[op_stack_index - 1];

        /* pop stack and apply operators */
        il = add_instr(OP_pop);
        il->param_no = param_no + 1;

        il = add_instr(OP_pop);
        il->param_no = param_no;

        /* apply stack operator  */
        il = add_instr(stack_op);
        il->param_no = param_no;
        il->int_param1 = param_no + 1;

        if (op_stack_index == 1) /* done */
            return;

        /* push value back on stack */
        il = add_instr(OP_push);
        il->param_no = param_no;

        /* pop op stack */
        op_stack_index--;
    }

    error("Unexpected end of expression");
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
                 int param_no,
                 int eval,
                 opcode_t prefix_op)
{
    ir_instr_t *ii;
    int is_reference = 1;

    /* already peeked and have the variable */
    lex_expect(T_identifier);

    /* load memory location into param */
    ii = add_instr(OP_address_of);
    ii->param_no = param_no;
    ii->str_param1 = var->var_name;
    lvalue->type = find_type(var->type_name);
    lvalue->size = get_size(var, lvalue->type);
    lvalue->is_ptr = var->is_ptr;
    if (var->array_size > 0)
        is_reference = 0;

    while (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
           lex_peek(T_dot, NULL)) {
        if (lex_accept(T_open_square)) {
            is_reference = 1;
            if (var->is_ptr <= 1) /* if nested pointer, still pointer */
                lvalue->size = lvalue->type->size;

            /* offset, so var must be either a pointer or an array of some type
             */
            if (var->is_ptr == 0 && var->array_size == 0)
                error("Cannot apply square operator to non-pointer");

            /* if var is an array, the memory location points to its start, but
             * if var is a pointer, we need to dereference.
             */
            if (var->is_ptr) {
                ii = add_instr(OP_read);
                ii->param_no = param_no;
                ii->int_param1 = param_no;
                ii->int_param2 = PTR_SIZE; /* pointer */
            }

            /* param+1 has the offset in array terms */
            read_expr(param_no + 1, parent);

            /* multiply by element size */
            if (lvalue->size != 1) {
                ii = add_instr(OP_load_constant);
                ii->int_param1 = lvalue->size;
                ii->param_no = param_no + 2;

                ii = add_instr(OP_mul);
                ii->param_no = param_no + 1;
                ii->int_param1 = param_no + 2;
            }

            ii = add_instr(OP_add);
            ii->param_no = param_no;
            ii->int_param1 = param_no + 1;

            lex_expect(T_close_square);
        } else {
            char token[MAX_ID_LEN];

            if (lex_accept(T_arrow)) {
                /* dereference first */
                ii = add_instr(OP_read);
                ii->param_no = param_no;
                ii->int_param1 = param_no;
                ii->int_param2 = PTR_SIZE;
            } else
                lex_expect(T_dot);

            lex_indent(T_identifier, token);

            /* change type currently pointed to */
            var = find_member(token, lvalue->type);
            lvalue->type = find_type(var->type_name);
            lvalue->is_ptr = var->is_ptr;

            /* reset target */
            is_reference = 1;
            lvalue->size = get_size(var, lvalue->type);
            if (var->array_size > 0) {
                is_reference = 0;
            }

            /* move pointer to offset of structure */
            ii = add_instr(OP_load_constant);
            ii->int_param1 = var->offset;
            ii->param_no = param_no + 1;

            ii = add_instr(OP_add);
            ii->param_no = param_no;
            ii->int_param1 = param_no + 1;
        }
    }

    if (eval == 0)
        return;

    /* need to apply pointer arithmetic? */
    if (lex_peek(T_plus, NULL) &&
        ((var->is_ptr > 0) || (var->array_size > 0))) {
        lex_accept(T_plus);

        /* dereference if necessary */
        if (is_reference) {
            ii = add_instr(OP_read);
            ii->param_no = param_no;
            ii->int_param1 = param_no;
            ii->int_param2 = PTR_SIZE;
        }

        /* param+1 has the offset in array terms */
        read_expr_operand(param_no + 1, parent);

        /* shift by offset in type sizes */
        lvalue->size = lvalue->type->size;

        /* multiply by element size */
        if (lvalue->size != 1) {
            ii = add_instr(OP_load_constant);
            ii->int_param1 = lvalue->size;
            ii->param_no = param_no + 2;

            ii = add_instr(OP_mul);
            ii->param_no = param_no + 1;
            ii->int_param1 = param_no + 2;
        }

        ii = add_instr(OP_add);
        ii->param_no = param_no;
        ii->int_param1 = param_no + 1;
    } else {
        /* should NOT dereference if var is of type array and there was
         * no offset.
         */
        if (is_reference) {
            if (prefix_op != OP_generic) {
                /* read into (p + 1) */
                ii = add_instr(OP_read);
                ii->param_no = param_no + 1;
                ii->int_param1 = param_no;
                ii->int_param2 = lvalue->size;

                /* load 1 */
                ii = add_instr(OP_load_constant);
                ii->param_no = param_no + 2;
                ii->int_param1 = 1;

                /* add/sub */
                ii = add_instr(prefix_op);
                ii->param_no = param_no + 1;
                ii->int_param1 = param_no + 2;

                /* store */
                ii = add_instr(OP_write);
                ii->param_no = param_no + 1;
                ii->int_param1 = param_no;
                ii->int_param2 = lvalue->size;
            }
            if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
                /* load value into param_no + 1 */
                ii = add_instr(OP_read);
                ii->param_no = param_no + 1;
                ii->int_param1 = param_no;
                ii->int_param2 = lvalue->size;

                /* push the value */
                ii = add_instr(OP_push);
                ii->param_no = param_no + 1;

                /* load 1 */
                ii = add_instr(OP_load_constant);
                ii->param_no = param_no + 2;
                ii->int_param1 = 1;

                /* add 1 */
                if (lex_accept(T_increment))
                    ii = add_instr(OP_add);
                else
                    ii = add_instr(OP_sub);
                ii->param_no = param_no + 1;
                ii->int_param1 = param_no + 2;

                /* store */
                ii = add_instr(OP_write);
                ii->param_no = param_no + 1;
                ii->int_param1 = param_no;
                ii->int_param2 = lvalue->size;

                /* pop original  value */
                ii = add_instr(OP_pop);
                ii->param_no = param_no;
            } else {
                ii = add_instr(OP_read);
                ii->param_no = param_no;
                ii->int_param1 = param_no;
                ii->int_param2 = lvalue->size;
            }
        }
    }
}

int read_body_assignment(char *token, block_t *parent, opcode_t prefix_op)
{
    var_t *var = find_local_var(token, parent);
    if (var == NULL)
        var = find_global_var(token);
    if (var) {
        ir_instr_t *ii;
        int one = 0;
        opcode_t op = OP_generic;
        lvalue_t lvalue;
        int size = 0;

        /* has memory address that we want to set */
        read_lvalue(&lvalue, var, parent, 0, 0, OP_generic);
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
        } else if (prefix_op == OP_generic) {
            lex_expect(T_assign);
        } else {
            op = prefix_op;
            one = 1;
        }

        if (op != OP_generic) {
            int increment_size = 1;

            /* if we have a pointer, shift it by element size */
            if (lvalue.is_ptr)
                increment_size = lvalue.type->size;

            /* get current value into ?1 */
            ii = add_instr(OP_read);
            ii->param_no = 1;
            ii->int_param1 = 0;
            ii->int_param2 = size;

            /* set ?2 with either 1 or expression value */
            if (one == 1) {
                ii = add_instr(OP_load_constant);
                ii->param_no = 2;
                ii->int_param1 = increment_size;
            } else {
                read_expr(2, parent);

                /* multiply by element size if necessary */
                if (increment_size != 1) {
                    ii = add_instr(OP_load_constant);
                    ii->param_no = 3;
                    ii->int_param1 = increment_size;

                    ii = add_instr(OP_mul);
                    ii->param_no = 2;
                    ii->int_param1 = 3;
                }
            }

            /* apply operation to value in ?1 */
            ii = add_instr(op);
            ii->param_no = 1;
            ii->int_param1 = 2;
        } else {
            read_expr(1, parent); /* get expression value into ?1 */
        }

        /* store value at specific address, but need to know the type/size */
        ii = add_instr(OP_write);
        ii->param_no = 1;
        ii->int_param1 = 0;
        ii->int_param2 = size;

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
    case OP_lshift:
        res = op1 << op2;
        break;
    case OP_rshift:
        res = op1 >> op2;
        break;
    default:
        error("The requested operation is not supported.");
    }
    return res;
}

int read_global_assignment(char *token)
{
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
            var->init_val = operand1;
            return 1;
        }
        operand2 = read_numeric_sconstant();
        next_op = get_operator();
        if (next_op == OP_generic) {
            /* only two operands, apply and return */
            var->init_val = eval_expression_imm(op, operand1, operand2);
            return 1;
        }

        /* using stack if operands more than two */
        op_stack[op_stack_index++] = op;
        op = next_op;
        val_stack[val_stack_index++] = operand1;
        val_stack[val_stack_index++] = operand2;

        while (op != OP_generic) {
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
                var->init_val = val_stack[0];
                return 1;
            }

            /* pop op stack */
            op_stack_index--;
        }

        var->init_val = val_stack[0];
        return 1;
    }
    return 0;
}

int break_exit_ir_index[MAX_NESTING];
int conti_jump_ir_index[MAX_NESTING];

void read_code_block(func_t *func, block_t *parent);

void read_body_statement(block_t *parent)
{
    char token[MAX_ID_LEN];
    func_t *fn;
    type_t *type;
    var_t *var;
    ir_instr_t *ii;
    opcode_t prefix_op = OP_generic;

    /* statement can be:
     *   function call, variable declaration, assignment operation,
     *   keyword, block
     */

    if (lex_peek(T_open_curly, NULL)) {
        read_code_block(parent->func, parent);
        return;
    }

    if (lex_accept(T_return)) {
        if (!lex_accept(T_semicolon)) { /* can be "void" */
            /* get expression value into return value */
            read_expr(0, parent);
            lex_expect(T_semicolon);
        }
        fn = parent->func;
        ii = add_instr(OP_return);
        ii->str_param1 = fn->return_def.var_name;
        return;
    }

    if (lex_accept(T_if)) {
        ir_instr_t *false_jump;

        lex_expect(T_open_bracket);
        read_expr(0, parent); /* get expression value into return value */
        lex_expect(T_close_bracket);

        false_jump = add_instr(OP_jz);
        false_jump->param_no = 0;

        read_body_statement(parent);

        /* if we have an "else" block, jump to finish */
        if (lex_accept(T_else)) {
            /* jump true branch to finish */
            ir_instr_t *true_jump = add_instr(OP_jump);

            /* we will emit false branch, link false jump here */
            ii = add_instr(OP_label);
            false_jump->int_param1 = ii->ir_index;

            /* false branch */
            read_body_statement(parent);

            /* this is finish, link true jump */
            ii = add_instr(OP_label);
            true_jump->int_param1 = ii->ir_index;
        } else {
            /* this is done, and link false jump */
            ii = add_instr(OP_label);
            false_jump->int_param1 = ii->ir_index;
        }
        return;
    }

    if (lex_accept(T_while)) {
        ir_instr_t *false_jump;
        ir_instr_t *start_jump =
            add_instr(OP_jump); /* jump to while condition */
        ir_instr_t *exit_label = add_instr(OP_label);
        ir_instr_t *exit_jump = add_instr(OP_jump);
        ir_instr_t *start_label = add_instr(OP_label); /* start to return to */
        lex_expect(T_open_bracket);
        read_expr(0, parent); /* get expression value into return value */
        lex_expect(T_close_bracket);

        false_jump = add_instr(OP_jz);
        false_jump->param_no = 0;

        start_jump->int_param1 = start_label->ir_index;

        /* create exit jump for breaks */
        break_exit_ir_index[break_level++] = exit_label->ir_index;
        conti_jump_ir_index[continue_level++] = start_label->ir_index;
        read_body_statement(parent);
        break_level--;
        continue_level--;

        /* unconditional jump back to expression */
        ii = add_instr(OP_jump);
        ii->int_param1 = start_label->ir_index;

        /* exit label */
        ii = add_instr(OP_label);
        false_jump->int_param1 = ii->ir_index;
        exit_jump->int_param1 = ii->ir_index;
        return;
    }

    if (lex_accept(T_switch)) {
        int case_values[MAX_CASES];
        int case_ir_index[MAX_CASES];
        int case_index = 0;
        int default_ir_index = 0;
        int i;
        ir_instr_t *jump_to_check;
        ir_instr_t *switch_exit;

        lex_expect(T_open_bracket);
        read_expr(1, parent);
        lex_expect(T_close_bracket);

        jump_to_check = add_instr(OP_jump);

        /* create exit jump for breaks */
        switch_exit = add_instr(OP_jump);
        break_exit_ir_index[break_level++] = switch_exit->ir_index;

        lex_expect(T_open_curly);
        while (lex_peek(T_default, NULL) || lex_peek(T_case, NULL)) {
            if (lex_accept(T_default)) {
                ii = add_instr(OP_label);
                default_ir_index = ii->ir_index;
            } else {
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
                ii = add_instr(OP_label);
                case_values[case_index] = case_val;
                case_ir_index[case_index++] = ii->ir_index;
            }
            lex_expect(T_colon);

            /* body is optional, can be another case */
            while (!lex_peek(T_case, NULL) && !lex_peek(T_close_curly, NULL) &&
                   !lex_peek(T_default, NULL)) {
                read_body_statement(parent);
                /* should end with a break which will generate jump out */
            }
        }
        lex_expect(T_close_curly);

        ii = add_instr(OP_label);
        jump_to_check->int_param1 = ii->ir_index;

        /* perform checks against ?1 */
        for (i = 0; i < case_index; i++) {
            ii = add_instr(OP_load_constant);
            ii->param_no = 0;
            ii->int_param1 = case_values[i];
            ii = add_instr(OP_eq);
            ii->param_no = 0;
            ii->int_param1 = 1;
            ii = add_instr(OP_jnz);
            ii->param_no = 0;
            ii->int_param1 = case_ir_index[i];
        }
        /* jump to default */
        if (default_ir_index) {
            ii = add_instr(OP_jump);
            ii->int_param1 = default_ir_index;
        }

        break_level--;

        /* exit where breaks should exit to */
        ii = add_instr(OP_label);
        switch_exit->int_param1 = ii->ir_index;
        return;
    }

    if (lex_accept(T_break)) {
        ii = add_instr(OP_jump);
        ii->int_param1 = break_exit_ir_index[break_level - 1];
    }

    if (lex_accept(T_continue)) {
        ii = add_instr(OP_jump);
        ii->int_param1 = conti_jump_ir_index[continue_level - 1];
    }

    if (lex_accept(T_for)) {
        ir_instr_t *start_jump = add_instr(OP_jump);
        ir_instr_t *exit_label = add_instr(OP_label);
        ir_instr_t *exit_jump = add_instr(OP_jump);
        ir_instr_t *start_label = add_instr(OP_label);
        ir_instr_t *condition_start;
        ir_instr_t *condition_jump_out;
        ir_instr_t *condition_jump_in;
        ir_instr_t *increment;
        ir_instr_t *increment_jump;
        ir_instr_t *body_start;
        ir_instr_t *body_jump;
        ir_instr_t *end;

        start_jump->int_param1 = start_label->ir_index;
        lex_expect(T_open_bracket);

        /* setup - execute once */
        if (!lex_accept(T_semicolon)) {
            lex_peek(T_identifier, token);
            read_body_assignment(token, parent, OP_generic);
            lex_expect(T_semicolon);
        }

        /* condition - check before the loop */
        condition_start = add_instr(OP_label);
        if (!lex_accept(T_semicolon)) {
            read_expr(0, parent);
            lex_expect(T_semicolon);
        } else {
            /* always true */
            ir_instr_t *itrue = add_instr(OP_load_constant);
            itrue->param_no = 0;
            itrue->int_param1 = 1;
        }

        condition_jump_out = add_instr(OP_jz); /* jump out if zero */
        condition_jump_out->param_no = 0;
        condition_jump_in = add_instr(OP_jump); /* else jump to body */
        condition_jump_in->param_no = 0;

        /* increment after each loop */
        increment = add_instr(OP_label);
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
        increment_jump = add_instr(OP_jump);
        increment_jump->int_param1 = condition_start->ir_index;

        /* loop body */
        body_start = add_instr(OP_label);
        condition_jump_in->int_param1 = body_start->ir_index;
        break_exit_ir_index[break_level++] = exit_label->ir_index;
        conti_jump_ir_index[continue_level++] = increment->ir_index;
        read_body_statement(parent);
        break_level--;
        continue_level--;

        /* jump to increment */
        body_jump = add_instr(OP_jump);
        body_jump->int_param1 = increment->ir_index;

        end = add_instr(OP_label);
        condition_jump_out->int_param1 = end->ir_index;
        exit_jump->int_param1 = end->ir_index;
        return;
    }

    if (lex_accept(T_do)) {
        ir_instr_t *false_jump;
        ir_instr_t *start_jump = add_instr(OP_jump);
        ir_instr_t *cond_label;
        ir_instr_t *cond_jump = add_instr(OP_jump);
        ir_instr_t *exit_label;
        ir_instr_t *exit_jump = add_instr(OP_jump);
        ir_instr_t *start_label = add_instr(OP_label); /* start to return to */

        start_jump->int_param1 = start_label->ir_index;

        break_exit_ir_index[break_level++] = exit_jump->ir_index;
        conti_jump_ir_index[continue_level++] = cond_jump->ir_index;
        read_body_statement(parent);
        break_level--;
        continue_level--;

        cond_label = add_instr(OP_label);
        cond_jump->int_param1 = cond_label->ir_index;
        lex_expect(T_while);
        lex_expect(T_open_bracket);
        read_expr(0, parent); /* get expression value into return value */
        lex_expect(T_close_bracket);

        false_jump = add_instr(OP_jnz);
        false_jump->param_no = 0;
        false_jump->int_param1 = start_label->ir_index;
        exit_label = add_instr(OP_label);
        exit_jump->int_param1 = exit_label->ir_index;

        lex_expect(T_semicolon);
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
        var = &parent->locals[parent->next_local++];
        read_full_var_decl(var);
        if (lex_accept(T_assign)) {
            read_expr(1, parent); /* get expression value into ?1 */
            /* assign to our new variable */

            /* load variable location */
            ii = add_instr(OP_address_of);
            ii->param_no = 0;
            ii->str_param1 = var->var_name;

            /* store value at specifc address, but need to know the type/size */
            ii = add_instr(OP_write);
            ii->param_no = 1;
            ii->int_param1 = 0;
            ii->int_param2 = get_size(var, type);
        }
        while (lex_accept(T_comma)) {
            /* multiple (partial) declarations */
            var_t *nv = &parent->locals[parent->next_local++];
            read_partial_var_decl(nv, var); /* partial */
            if (lex_accept(T_assign)) {
                read_expr(1, parent); /* get expression value into ?1 */
                /* assign to our new variable */

                /* load variable location */
                ii = add_instr(OP_address_of);
                ii->param_no = 0;
                ii->str_param1 = nv->var_name;

                /* store value at specific address, but need to know the
                 * type/size */
                ii = add_instr(OP_write);
                ii->param_no = 1;
                ii->int_param1 = 0;
                ii->int_param2 = get_size(var, type);
            }
        }
        lex_expect(T_semicolon);
        return;
    }

    /* is a function call? */
    fn = find_func(token);
    if (fn) {
        read_func_call(fn, 0, parent);
        lex_expect(T_semicolon);
        return;
    }

    /* is an assignment? */
    if (read_body_assignment(token, parent, prefix_op)) {
        lex_expect(T_semicolon);
        return;
    }

    error("Unrecognized statement token");
}

void read_code_block(func_t *func, block_t *parent)
{
    block_t *blk = add_block(parent, func);
    ir_instr_t *ii = add_instr(OP_block_start);
    ii->int_param1 = blk->index;
    lex_expect(T_open_curly);

    while (!lex_accept(T_close_curly))
        read_body_statement(blk);

    ii = add_instr(OP_block_end);
    ii->int_param1 = blk->index;
}

void read_func_body(func_t *fdef)
{
    ir_instr_t *ii;

    read_code_block(fdef, NULL);

    /* only add return when we have no return type, as otherwise there should
     * have been a return statement.
     */
    ii = add_instr(OP_func_exit);
    ii->str_param1 = fdef->return_def.var_name;
    fdef->exit_point = ii->ir_index;
}

var_t _temp_var;

/* if first token is type */
void read_global_decl(block_t *block)
{
    /* new function, or variables under parent */
    read_full_var_decl(&_temp_var);

    if (lex_peek(T_open_bracket, NULL)) {
        ir_instr_t *ii;

        /* function */
        func_t *fd = add_func(_temp_var.var_name);
        memcpy(&fd->return_def, &_temp_var, sizeof(var_t));

        fd->num_params = read_parameter_list_decl(fd->param_defs);

        if (lex_peek(T_open_curly, NULL)) {
            ii = add_instr(OP_func_extry);
            ii->str_param1 = fd->return_def.var_name;
            fd->entry_point = ii->ir_index;

            read_func_body(fd);
            return;
        }
        if (lex_accept(T_semicolon)) /* forward definition */
            return;
        error("Syntax error in global declaration");
    }

    /* is a variable */
    memcpy(&block->locals[block->next_local++], &_temp_var, sizeof(var_t));

    if (lex_accept(T_assign)) {
        if (_temp_var.is_ptr == 0 && _temp_var.array_size == 0) {
            read_global_assignment(_temp_var.var_name);
            lex_expect(T_semicolon);
            return;
        }
        /* TODO: support global initialization for array and pointer */
        error("Global initialization for array and pointer not supported");
    } else if (lex_accept(T_comma))
        /* TODO: continuation */
        error("Global continuation not supported");
    else if (lex_accept(T_semicolon))
        return;
    error("Syntax error in global declaration");
}

void read_global_statement()
{
    char token[MAX_ID_LEN];
    block_t *block = &BLOCKS[0]; /* global block */

    if (lex_peek(T_include, token)) {
        if (strcmp(token_str, "<stdio.h>") == 0) {
            /* ignore, we inclue libc by default */
        }
        lex_expect(T_include);
    } else if (lex_accept(T_define)) {
        char alias[MAX_VAR_LEN];
        char value[MAX_VAR_LEN];

        lex_peek(T_identifier, alias);
        lex_expect(T_identifier);
        lex_peek(T_numeric, value);
        lex_expect(T_numeric);
        add_alias(alias, value);
    } else if (lex_accept(T_typedef)) {
        if (lex_accept(T_enum)) {
            int val = 0;
            type_t *type = add_type();

            type->base_type = TYPE_int;
            type->size = 4;
            lex_expect(T_open_curly);
            do {
                lex_indent(T_identifier, token);
                if (lex_accept(T_assign)) {
                    char value[MAX_ID_LEN];
                    lex_indent(T_numeric, value);
                    val = read_numeric_constant(value);
                }
                add_constant(token, val++);
            } while (lex_accept(T_comma));
            lex_expect(T_close_curly);
            lex_indent(T_identifier, token);
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
                read_full_var_decl(v);
                v->offset = size;
                size += size_var(v);
                lex_expect(T_semicolon);
            } while (!lex_accept(T_close_curly));

            lex_indent(T_identifier, token); /* type name */
            strcpy(type->type_name, token);
            type->size = size;
            type->num_fields = i;
            type->base_type = TYPE_struct; /* is used? */
            lex_expect(T_semicolon);
        } else {
            char base_type[MAX_TYPE_LEN];
            type_t *base;
            type_t *type = add_type();
            lex_indent(T_identifier, base_type);
            base = find_type(base_type);
            if (base == NULL)
                error("Unable to find base type");
            type->base_type = base->base_type;
            type->size = base->size;
            type->num_fields = 0;
            lex_indent(T_identifier, type->type_name);
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
    ir_instr_t *ii;
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

    add_block(NULL, NULL);    /* global block */
    elf_add_symbol("", 0, 0); /* undef symbol */

    /* architecture defines */
    add_alias("__arm__", "1"); /* defined by GNU C and RealView */

    /* binary entry point: read params, call main, exit */
    ii = add_instr(OP_label);
    ii->str_param1 = "__start";
    add_instr(OP_start);
    ii = add_instr(OP_call);
    ii->str_param1 = "main";
    ii = add_instr(OP_label);
    ii->str_param1 = "__exit";
    add_instr(OP_exit);

    /* Linux syscall */
    fn = add_func("__syscall");
    fn->num_params = 0;
    ii = add_instr(OP_func_extry);
    fn->entry_point = ii->ir_index;
    ii->str_param1 = fn->return_def.var_name;
    ii = add_instr(OP_syscall);
    ii->str_param1 = fn->return_def.var_name;
    ii = add_instr(OP_func_exit);
    ii->str_param1 = fn->return_def.var_name;
    fn->exit_point = ii->ir_index;

    /* internal */
    break_level = 0;
    continue_level = 0;

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
    for (;;) {
        if (fgets(buffer, MAX_LINE_LEN, f) == NULL) {
            fclose(f);
            return;
        }
        if ((strncmp(buffer, "#include ", 9) == 0) && (buffer[9] == '"')) {
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
