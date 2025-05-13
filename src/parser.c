/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../config"
#include "defs.h"
#include "globals.c"

/* C language syntactic analyzer */
int global_var_idx = 0;

/* Side effect instructions cache */
insn_t side_effect[10];
int se_idx = 0;

/* Control flow utilities */
basic_block_t *break_bb[MAX_NESTING];
int break_exit_idx = 0;
basic_block_t *continue_bb[MAX_NESTING];
int continue_pos_idx = 0;

/* Label utilities */
label_t labels[MAX_LABELS];
int label_idx = 0;
basic_block_t *backpatch_bb[MAX_LABELS];
int backpatch_bb_idx = 0;

/* stack of the operands of 3AC */
var_t *operand_stack[MAX_OPERAND_STACK_SIZE];
int operand_stack_idx = 0;

/* Forward declarations */
basic_block_t *read_body_statement(block_t *parent, basic_block_t *bb);
void perform_side_effect(block_t *parent, basic_block_t *bb);
void read_inner_var_decl(var_t *vd, bool anon, bool is_param);
void read_partial_var_decl(var_t *vd, var_t *template);
void parse_array_init(var_t *var,
                      block_t *parent,
                      basic_block_t **bb,
                      bool emit_code);


label_t *find_label(char *name)
{
    for (int i = 0; i < label_idx; i++) {
        if (!strcmp(name, labels[i].label_name))
            return &labels[i];
    }
    return NULL;
}

void add_label(char *name, basic_block_t *bb)
{
    if (label_idx > MAX_LABELS - 1)
        error("Too many labels in function");

    label_t *l = &labels[label_idx++];
    strncpy(l->label_name, name, MAX_ID_LEN);
    l->bb = bb;
}

char *gen_name_to(char *buf)
{
    sprintf(buf, ".t%d", global_var_idx++);
    return buf;
}

var_t *require_var(block_t *blk)
{
    var_list_t *var_list = &blk->locals;

    if (var_list->size >= var_list->capacity) {
        var_list->capacity <<= 1;

        var_t **new_locals =
            arena_alloc(BLOCK_ARENA, var_list->capacity * sizeof(var_t *));
        memcpy(new_locals, var_list->elements,
               var_list->size * sizeof(var_t *));
        var_list->elements = new_locals;
    }

    var_t *var = arena_calloc(BLOCK_ARENA, 1, sizeof(var_t));
    var_list->elements[var_list->size++] = var;
    var->consumed = -1;
    var->phys_reg = -1;
    var->first_use = -1;
    var->last_use = -1;
    var->loop_depth = 0;
    var->use_count = 0;
    var->base = var;
    var->type = TY_int;
    return var;
}

var_t *require_typed_var(block_t *blk, type_t *type)
{
    if (!type)
        error("Type must not be NULL");

    var_t *var = require_var(blk);
    var->type = type;
    return var;
}

var_t *require_typed_ptr_var(block_t *blk, type_t *type, int ptr)
{
    var_t *var = require_typed_var(blk, type);
    var->ptr_level = ptr;
    return var;
}

var_t *require_ref_var(block_t *blk, type_t *type, int ptr)
{
    if (!type)
        error("Cannot reference variable from NULL type");

    var_t *var = require_typed_var(blk, type);
    var->ptr_level = ptr + 1;
    return var;
}

var_t *require_deref_var(block_t *blk, type_t *type, int ptr)
{
    if (!type)
        error("Cannot dereference variable from NULL type");

    /* Allowing integer dereferencing */
    if (!ptr && type->base_type != TYPE_struct &&
        type->base_type != TYPE_typedef)
        return require_var(blk);

    if (!ptr)
        error("Cannot dereference from non-pointer typed variable");

    var_t *var = require_typed_var(blk, type);
    var->ptr_level = ptr - 1;
    return var;
}

void opstack_push(var_t *var)
{
    operand_stack[operand_stack_idx++] = var;
}

var_t *opstack_pop(void)
{
    return operand_stack[--operand_stack_idx];
}

void read_expr(block_t *parent, basic_block_t **bb);

int write_symbol(const char *data)
{
    /* Write string literals to .rodata section */
    const int start_len = elf_rodata->size;
    elf_write_str(elf_rodata, data);
    elf_write_byte(elf_rodata, 0);
    return start_len;
}

int get_size(var_t *var)
{
    if (var->ptr_level || var->is_func)
        return PTR_SIZE;
    return var->type->size;
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

int get_unary_operator_prio(opcode_t op)
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

opcode_t get_operator(void)
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

var_t *promote_unchecked(block_t *block,
                         basic_block_t **bb,
                         var_t *var,
                         type_t *target_type,
                         int target_ptr)
{
    var_t *rd = require_typed_ptr_var(block, target_type, target_ptr);
    gen_name_to(rd->var_name);
    /* Encode both source and target sizes in src1:
     * Lower 16 bits: target size
     * Upper 16 bits: source size
     * This allows codegen to distinguish between different promotion types
     * without changing IR semantics.
     */
    int encoded_size = ((var->type->size) << 16);
    if (target_ptr)
        encoded_size |= PTR_SIZE;
    else
        encoded_size |= target_type->size;
    add_insn(block, *bb, OP_sign_ext, rd, var, NULL, encoded_size, NULL);
    return rd;
}

var_t *promote(block_t *block,
               basic_block_t **bb,
               var_t *var,
               type_t *target_type,
               int target_ptr)
{
    /* Effectively checking whether var has size of int */
    if (var->type->size == target_type->size || var->ptr_level ||
        var->array_size)
        return var;

    if (var->type->size > TY_int->size && !var->ptr_level) {
        printf("Warning: Suspicious type promotion %s\n", var->type->type_name);
        return var;
    }

    return promote_unchecked(block, bb, var, target_type, target_ptr);
}

var_t *truncate_unchecked(block_t *block,
                          basic_block_t **bb,
                          var_t *var,
                          type_t *target_type,
                          int target_ptr)
{
    var_t *rd = require_typed_ptr_var(block, target_type, target_ptr);
    gen_name_to(rd->var_name);
    add_insn(block, *bb, OP_trunc, rd, var, NULL,
             target_ptr ? PTR_SIZE : target_type->size, NULL);
    return rd;
}

var_t *resize_var(block_t *block, basic_block_t **bb, var_t *from, var_t *to)
{
    bool is_from_ptr = from->ptr_level || from->array_size,
         is_to_ptr = to->ptr_level || to->array_size ||
                     (to->type && to->type->ptr_level > 0);

    if (is_from_ptr && is_to_ptr)
        return from;

    int from_size = get_size(from), to_size = get_size(to);

    if (from_size > to_size) {
        /* Truncation */
        return truncate_unchecked(block, bb, from, to->type, to->ptr_level);
    }

    if (from_size < to_size) {
        /* Sign extend */
        return promote_unchecked(block, bb, from, to->type, to->ptr_level);
    }

    return from;
}

int read_numeric_constant(char buffer[])
{
    int i = 0;
    int value = 0;
    while (buffer[i]) {
        if (i == 1 && (buffer[i] | 32) == 'x') { /* hexadecimal */
            value = 0;
            i = 2;
            while (buffer[i]) {
                char c = buffer[i++];
                value <<= 4;
                if (is_digit(c))
                    value += c - '0';
                c |= 32; /* convert to lower case */
                if (c >= 'a' && c <= 'f')
                    value += (c - 'a') + 10;
            }
            return value;
        }
        if (i == 1 && (buffer[i] | 32) == 'b') { /* binary */
            value = 0;
            i = 2;
            while (buffer[i]) {
                char c = buffer[i++];
                value <<= 1;
                if (c == '1')
                    value += 1;
            }
            return value;
        }
        if (buffer[0] == '0') /* octal */
            value = value * 8 + buffer[i++] - '0';
        else
            value = value * 10 + buffer[i++] - '0';
    }
    return value;
}

int read_constant_expr_operand(void)
{
    char buffer[MAX_ID_LEN];
    int value;

    if (lex_peek(T_numeric, buffer)) {
        lex_expect(T_numeric);
        return read_numeric_constant(buffer);
    }

    if (lex_accept(T_open_bracket)) {
        value = read_constant_expr_operand();
        lex_expect(T_close_bracket);
        return value;
    }

    if (lex_peek(T_identifier, buffer) && !strcmp(buffer, "defined")) {
        char lookup_alias[MAX_TOKEN_LEN];

        lex_expect(T_identifier); /* defined */
        lex_expect_internal(T_open_bracket, 0);
        lex_ident(T_identifier, lookup_alias);
        lex_expect(T_close_bracket);

        return find_alias(lookup_alias) ? 1 : 0;
    }

    error("Unexpected token while evaluating constant");
    return -1;
}

int read_constant_infix_expr(int precedence)
{
    int lhs, rhs;

    /* Evaluate unary expression first */
    opcode_t op = get_operator();
    int current_precedence = get_unary_operator_prio(op);
    if (current_precedence != 0 && current_precedence >= precedence) {
        lhs = read_constant_infix_expr(current_precedence);

        switch (op) {
        case OP_add:
            break;
        case OP_sub:
            lhs = -lhs;
            break;
        case OP_bit_not:
            lhs = ~lhs;
            break;
        case OP_log_not:
            lhs = !lhs;
            break;
        default:
            error("Unexpected unary token while evaluating constant");
        }
    } else {
        lhs = read_constant_expr_operand();
    }

    while (true) {
        op = get_operator();
        current_precedence = get_operator_prio(op);

        if (current_precedence == 0 || current_precedence <= precedence) {
            break;
        }

        rhs = read_constant_infix_expr(current_precedence);

        switch (op) {
        case OP_add:
            lhs += rhs;
            break;
        case OP_sub:
            lhs -= rhs;
            break;
        case OP_mul:
            lhs *= rhs;
            break;
        case OP_div:
            lhs /= rhs;
            break;
        case OP_bit_and:
            lhs &= rhs;
            break;
        case OP_bit_or:
            lhs |= rhs;
            break;
        case OP_bit_xor:
            lhs ^= rhs;
            break;
        case OP_lshift:
            lhs <<= rhs;
            break;
        case OP_rshift:
            lhs >>= rhs;
            break;
        case OP_gt:
            lhs = lhs > rhs;
            break;
        case OP_geq:
            lhs = lhs >= rhs;
            break;
        case OP_lt:
            lhs = lhs < rhs;
            break;
        case OP_leq:
            lhs = lhs <= rhs;
            break;
        case OP_eq:
            lhs = lhs == rhs;
            break;
        case OP_neq:
            lhs = lhs != rhs;
            break;
        case OP_log_and:
            lhs = lhs && rhs;
            break;
        case OP_log_or:
            lhs = lhs || rhs;
            break;
        default:
            error("Unexpected infix token while evaluating constant");
        }

        op = get_operator();
    }

    return lhs;
}

int read_constant_expr(void)
{
    return read_constant_infix_expr(0);
}

/* Skips lines where preprocessor match is false, this will stop once next
 * token is either 'T_cppd_elif', 'T_cppd_else' or 'cppd_endif'.
 */
void cppd_control_flow_skip_lines(void)
{
    while (!lex_peek(T_cppd_elif, NULL) && !lex_peek(T_cppd_else, NULL) &&
           !lex_peek(T_cppd_endif, NULL)) {
        next_token = lex_token();
    }
    skip_whitespace();
}

void check_def(char *alias, bool expected)
{
    if ((find_alias(alias) != NULL) == expected)
        preproc_match = true;
}

void read_defined_macro(void)
{
    char lookup_alias[MAX_TOKEN_LEN];

    lex_expect(T_identifier); /* defined */
    lex_expect_internal(T_open_bracket, 0);
    lex_ident(T_identifier, lookup_alias);
    lex_expect(T_close_bracket);

    check_def(lookup_alias, true);
}

/* read preprocessor directive at each potential positions: e.g., global
 * statement / body statement
 */
bool read_preproc_directive(void)
{
    char token[MAX_ID_LEN];

    if (lex_peek(T_cppd_include, token)) {
        lex_expect(T_cppd_include);

        /* Basic #define syntax validation */
        if (lex_peek(T_string, NULL)) {
            /* #define "header.h" */
            lex_expect(T_string);
        } else {
            /* #define <stdlib.h> */
            lex_expect(T_lt);

            while (!lex_peek(T_gt, NULL)) {
                next_token = lex_token();
            }

            lex_expect(T_gt);
        }

        return true;
    }
    if (lex_accept(T_cppd_define)) {
        char alias[MAX_VAR_LEN];
        char value[MAX_VAR_LEN];

        lex_ident_internal(T_identifier, alias, false);

        if (lex_peek(T_numeric, value)) {
            lex_expect(T_numeric);
            add_alias(alias, value);
        } else if (lex_peek(T_string, value)) {
            lex_expect(T_string);
            add_alias(alias, value);
        } else if (lex_peek(T_identifier, value)) {
            lex_expect(T_identifier);
            add_alias(alias, value);
        } else if (lex_accept(T_open_bracket)) { /* function-like macro */
            macro_t *macro = add_macro(alias);

            skip_newline = false;
            while (lex_peek(T_identifier, alias)) {
                lex_expect(T_identifier);
                strcpy(macro->param_defs[macro->num_param_defs++].var_name,
                       intern_string(alias));
                lex_accept(T_comma);
            }
            if (lex_accept(T_elipsis))
                macro->is_variadic = true;

            macro->start_source_idx = SOURCE->size;
            skip_macro_body();
        } else {
            /* Empty alias, may be dummy alias serves as include guard */
            value[0] = 0;
            add_alias(alias, value);
        }

        return true;
    }
    if (lex_peek(T_cppd_undef, token)) {
        char alias[MAX_VAR_LEN];

        lex_expect_internal(T_cppd_undef, false);
        lex_peek(T_identifier, alias);
        lex_expect(T_identifier);

        remove_alias(alias);
        remove_macro(alias);
        return true;
    }
    if (lex_peek(T_cppd_error, NULL)) {
        int i = 0;
        char error_diagnostic[MAX_LINE_LEN];

        do {
            error_diagnostic[i++] = next_char;
        } while (read_char(false) != '\n');
        error_diagnostic[i] = 0;

        error(error_diagnostic);
    }
    if (lex_accept(T_cppd_if)) {
        preproc_match = read_constant_expr() != 0;

        if (preproc_match) {
            skip_whitespace();
        } else {
            cppd_control_flow_skip_lines();
        }

        return true;
    }
    if (lex_accept(T_cppd_elif)) {
        if (preproc_match) {
            while (!lex_peek(T_cppd_endif, NULL)) {
                next_token = lex_token();
            }
            return true;
        }

        preproc_match = read_constant_expr() != 0;

        if (preproc_match) {
            skip_whitespace();
        } else {
            cppd_control_flow_skip_lines();
        }

        return true;
    }
    if (lex_accept(T_cppd_else)) {
        /* reach here has 2 possible cases:
         * 1. reach #ifdef preprocessor directive
         * 2. conditional expression in #elif is false
         */
        if (!preproc_match) {
            skip_whitespace();
            return true;
        }

        cppd_control_flow_skip_lines();
        return true;
    }
    if (lex_accept(T_cppd_endif)) {
        preproc_match = false;
        skip_whitespace();
        return true;
    }
    if (lex_accept_internal(T_cppd_ifdef, false)) {
        preproc_match = false;
        lex_ident(T_identifier, token);
        check_def(token, true);

        if (preproc_match) {
            skip_whitespace();
            return true;
        }

        cppd_control_flow_skip_lines();
        return true;
    }
    if (lex_accept_internal(T_cppd_ifndef, false)) {
        preproc_match = false;
        lex_ident(T_identifier, token);
        check_def(token, false);

        if (preproc_match) {
            skip_whitespace();
            return true;
        }

        cppd_control_flow_skip_lines();
        return true;
    }
    if (lex_accept_internal(T_cppd_pragma, false)) {
        lex_expect(T_identifier);
        return true;
    }

    return false;
}

void read_parameter_list_decl(func_t *func, bool anon);

/* Forward declaration for ternary handling used by initializers */
void read_ternary_operation(block_t *parent, basic_block_t **bb);

/* Parse array initializer to determine size for implicit arrays and
 * optionally emit initialization code.
 */
var_t *compute_element_address(block_t *parent,
                               basic_block_t **bb,
                               var_t *base_addr,
                               int index,
                               int elem_size)
{
    if (index == 0)
        return base_addr;

    var_t *offset = require_var(parent);
    gen_name_to(offset->var_name);
    offset->init_val = index * elem_size;
    add_insn(parent, *bb, OP_load_constant, offset, NULL, NULL, 0, NULL);

    var_t *addr = require_var(parent);
    gen_name_to(addr->var_name);
    add_insn(parent, *bb, OP_add, addr, base_addr, offset, 0, NULL);
    return addr;
}

var_t *compute_field_address(block_t *parent,
                             basic_block_t **bb,
                             var_t *struct_addr,
                             var_t *field)
{
    if (field->offset == 0)
        return struct_addr;

    var_t *offset = require_var(parent);
    gen_name_to(offset->var_name);
    offset->init_val = field->offset;
    add_insn(parent, *bb, OP_load_constant, offset, NULL, NULL, 0, NULL);

    var_t *addr = require_var(parent);
    gen_name_to(addr->var_name);
    add_insn(parent, *bb, OP_add, addr, struct_addr, offset, 0, NULL);
    return addr;
}

var_t *parse_global_constant_value(block_t *parent, basic_block_t **bb)
{
    var_t *val = NULL;

    if (lex_peek(T_numeric, NULL) || lex_peek(T_minus, NULL)) {
        bool is_neg = false;
        if (lex_accept(T_minus))
            is_neg = true;
        char numtok[MAX_ID_LEN];
        lex_ident(T_numeric, numtok);
        int num_val = read_numeric_constant(numtok);
        if (is_neg)
            num_val = -num_val;

        val = require_var(parent);
        gen_name_to(val->var_name);
        val->init_val = num_val;
        add_insn(parent, *bb, OP_load_constant, val, NULL, NULL, 0, NULL);
    } else if (lex_peek(T_char, NULL)) {
        char chtok[5];
        lex_ident(T_char, chtok);

        val = require_typed_var(parent, TY_char);
        gen_name_to(val->var_name);
        val->init_val = chtok[0];
        add_insn(parent, *bb, OP_load_constant, val, NULL, NULL, 0, NULL);
    } else if (lex_peek(T_string, NULL)) {
        lex_accept(T_string);
        /* TODO: String fields in structs not yet supported - requires proper
         * handling of string literals as initializers
         */
    } else {
        error("Global array initialization requires constant values");
    }

    return val;
}

void consume_global_constant_syntax(void)
{
    if (lex_peek(T_numeric, NULL)) {
        lex_accept(T_numeric);
    } else if (lex_peek(T_minus, NULL)) {
        lex_accept(T_minus);
        lex_accept(T_numeric);
    } else if (lex_peek(T_string, NULL)) {
        lex_accept(T_string);
    } else if (lex_peek(T_char, NULL)) {
        lex_accept(T_char);
    } else {
        error("Global array initialization requires constant values");
    }
}

void parse_struct_field_init(block_t *parent,
                             basic_block_t **bb,
                             type_t *struct_type,
                             var_t *target_addr,
                             bool emit_code)
{
    int field_idx = 0;

    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            var_t *field_val_raw = NULL;

            if (parent == GLOBAL_BLOCK) {
                if (emit_code) {
                    field_val_raw = parse_global_constant_value(parent, bb);
                } else {
                    consume_global_constant_syntax();
                }
            } else {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
                field_val_raw = opstack_pop();
            }

            if (field_val_raw && field_idx < struct_type->num_fields) {
                var_t *field = &struct_type->fields[field_idx];

                var_t target = {0};
                target.type = field->type;
                target.ptr_level = field->ptr_level;
                var_t *field_val =
                    resize_var(parent, bb, field_val_raw, &target);

                var_t *field_addr =
                    compute_field_address(parent, bb, target_addr, field);

                int field_size = size_var(field);
                add_insn(parent, *bb, OP_write, NULL, field_addr, field_val,
                         field_size, NULL);
            }

            field_idx++;
            if (!lex_accept(T_comma))
                break;
            if (lex_peek(T_close_curly, NULL))
                break;
        }
    }
}

void parse_array_literal_expr(block_t *parent, basic_block_t **bb)
{
    var_t *array_var = require_var(parent);
    gen_name_to(array_var->var_name);

    int element_count = 0;
    var_t *first_element = NULL;

    if (!lex_peek(T_close_curly, NULL)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
        first_element = opstack_pop();
        element_count = 1;

        while (lex_accept(T_comma)) {
            if (lex_peek(T_close_curly, NULL))
                break;

            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
            opstack_pop();
            element_count++;
        }
    }

    lex_expect(T_close_curly);

    array_var->array_size = element_count;
    if (first_element) {
        array_var->type = first_element->type;
        array_var->init_val = first_element->init_val;
    } else {
        array_var->type = TY_int;
        array_var->init_val = 0;
    }

    opstack_push(array_var);
    add_insn(parent, *bb, OP_load_constant, array_var, NULL, NULL, 0, NULL);
}

basic_block_t *handle_return_statement(block_t *parent, basic_block_t *bb)
{
    if (lex_accept(T_semicolon)) {
        add_insn(parent, bb, OP_return, NULL, NULL, NULL, 0, NULL);
        bb_connect(bb, parent->func->exit, NEXT);
        return NULL;
    }

    read_expr(parent, &bb);
    read_ternary_operation(parent, &bb);
    perform_side_effect(parent, bb);
    lex_expect(T_semicolon);

    var_t *rs1 = opstack_pop();

    /* Handle array compound literals in return context.
     * Convert array compound literals to their first element value.
     */
    if (rs1 && rs1->array_size > 0 && rs1->var_name[0] == '.') {
        var_t *val = require_var(parent);
        val->type = rs1->type;
        val->init_val = rs1->init_val;
        gen_name_to(val->var_name);
        add_insn(parent, bb, OP_load_constant, val, NULL, NULL, 0, NULL);
        rs1 = val;
    }

    add_insn(parent, bb, OP_return, NULL, rs1, NULL, 0, NULL);
    bb_connect(bb, parent->func->exit, NEXT);
    return NULL;
}

basic_block_t *handle_if_statement(block_t *parent, basic_block_t *bb)
{
    basic_block_t *n = bb_create(parent);
    bb_connect(bb, n, NEXT);
    bb = n;

    lex_expect(T_open_bracket);
    read_expr(parent, &bb);
    lex_expect(T_close_bracket);

    var_t *vd = opstack_pop();
    add_insn(parent, bb, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    bb_connect(bb, then_, THEN);
    bb_connect(bb, else_, ELSE);

    basic_block_t *then_body = read_body_statement(parent, then_);
    basic_block_t *then_next_ = NULL;
    if (then_body) {
        then_next_ = bb_create(parent);
        bb_connect(then_body, then_next_, NEXT);
    }

    if (lex_accept(T_else)) {
        basic_block_t *else_body = read_body_statement(parent, else_);
        basic_block_t *else_next_ = NULL;
        if (else_body) {
            else_next_ = bb_create(parent);
            bb_connect(else_body, else_next_, NEXT);
        }

        if (then_next_ && else_next_) {
            basic_block_t *next_ = bb_create(parent);
            bb_connect(then_next_, next_, NEXT);
            bb_connect(else_next_, next_, NEXT);
            return next_;
        }

        return then_next_ ? then_next_ : else_next_;
    } else {
        if (then_next_) {
            bb_connect(else_, then_next_, NEXT);
            return then_next_;
        }
        return else_;
    }
}

basic_block_t *handle_while_statement(block_t *parent, basic_block_t *bb)
{
    basic_block_t *n = bb_create(parent);
    bb_connect(bb, n, NEXT);
    bb = n;

    continue_bb[continue_pos_idx++] = bb;

    basic_block_t *cond = bb;
    lex_expect(T_open_bracket);
    read_expr(parent, &bb);
    lex_expect(T_close_bracket);

    var_t *vd = opstack_pop();
    add_insn(parent, bb, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    bb_connect(bb, then_, THEN);
    bb_connect(bb, else_, ELSE);
    break_bb[break_exit_idx++] = else_;

    basic_block_t *body_ = read_body_statement(parent, then_);

    continue_pos_idx--;
    break_exit_idx--;

    if (body_)
        bb_connect(body_, cond, NEXT);

    return else_;
}

basic_block_t *handle_goto_statement(block_t *parent, basic_block_t *bb)
{
    /* Since a goto splits the current program into two basic blocks and makes
     * the subsequent basic block unreachable, this causes problems for later
     * CFG operations. Therefore, we create a fake if that always executes to
     * wrap the goto, and connect the unreachable basic block to the else
     * branch. Finally, return this else block.
     *
     * after:
     * a = b + c;
     * goto label;
     * c *= d;
     *
     * before:
     * a = b + c;
     * if (1)
     *     goto label;
     * c *= d;
     */

    char token[MAX_ID_LEN];
    if (!lex_peek(T_identifier, token))
        error("Expected identifier after 'goto'");

    lex_expect(T_identifier);
    lex_expect(T_semicolon);

    basic_block_t *fake_if = bb_create(parent);
    bb_connect(bb, fake_if, NEXT);
    var_t *val = require_var(parent);
    gen_name_to(val->var_name);
    val->init_val = 1;
    add_insn(parent, fake_if, OP_load_constant, val, NULL, NULL, 0, NULL);
    add_insn(parent, fake_if, OP_branch, NULL, val, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    bb_connect(fake_if, then_, THEN);
    bb_connect(fake_if, else_, ELSE);

    add_insn(parent, then_, OP_jump, NULL, NULL, NULL, 0, token);
    label_t *label = find_label(token);
    if (label) {
        label->used = true;
        bb_connect(then_, label->bb, NEXT);
        return else_;
    }

    if (backpatch_bb_idx > MAX_LABELS - 1)
        error("Too many forward-referenced labels");

    backpatch_bb[backpatch_bb_idx++] = then_;
    return else_;
}

basic_block_t *handle_struct_variable_decl(block_t *parent,
                                           basic_block_t *bb,
                                           char *token)
{
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (find_type_flag == 1 && lex_accept(T_union)) {
        find_type_flag = 2;
    }

    type_t *type = find_type(token, find_type_flag);
    if (!type)
        return bb;

    var_t *var = require_typed_var(parent, type);
    read_partial_var_decl(var, NULL);
    add_insn(parent, bb, OP_allocat, var, NULL, NULL, 0, NULL);
    add_symbol(bb, var);

    if (lex_accept(T_assign)) {
        if (lex_peek(T_open_curly, NULL) &&
            (var->array_size > 0 || var->ptr_level > 0)) {
            parse_array_init(var, parent, &bb, true);
        } else if (lex_peek(T_open_curly, NULL) &&
                   (var->type->base_type == TYPE_struct ||
                    var->type->base_type == TYPE_typedef)) {
            type_t *struct_type = var->type;
            if (struct_type->base_type == TYPE_typedef &&
                struct_type->base_struct)
                struct_type = struct_type->base_struct;

            var_t *struct_addr = require_var(parent);
            gen_name_to(struct_addr->var_name);
            add_insn(parent, bb, OP_address_of, struct_addr, var, NULL, 0,
                     NULL);

            lex_expect(T_open_curly);
            parse_struct_field_init(parent, &bb, struct_type, struct_addr,
                                    true);
            lex_expect(T_close_curly);
        } else {
            read_expr(parent, &bb);
            read_ternary_operation(parent, &bb);
            var_t *rs1 = resize_var(parent, &bb, opstack_pop(), var);
            add_insn(parent, bb, OP_assign, var, rs1, NULL, 0, NULL);
        }
    }

    while (lex_accept(T_comma)) {
        var_t *nv = require_typed_var(parent, type);
        read_inner_var_decl(nv, false, false);
        add_insn(parent, bb, OP_allocat, nv, NULL, NULL, 0, NULL);
        add_symbol(bb, nv);
        if (lex_accept(T_assign)) {
            if (lex_peek(T_open_curly, NULL) &&
                (nv->array_size > 0 || nv->ptr_level > 0)) {
                parse_array_init(nv, parent, &bb, true);
            } else if (lex_peek(T_open_curly, NULL) &&
                       (nv->type->base_type == TYPE_struct ||
                        nv->type->base_type == TYPE_typedef)) {
                type_t *struct_type = nv->type;
                if (struct_type->base_type == TYPE_typedef &&
                    struct_type->base_struct)
                    struct_type = struct_type->base_struct;

                var_t *struct_addr = require_var(parent);
                gen_name_to(struct_addr->var_name);
                add_insn(parent, bb, OP_address_of, struct_addr, nv, NULL, 0,
                         NULL);

                lex_expect(T_open_curly);
                parse_struct_field_init(parent, &bb, struct_type, struct_addr,
                                        1);
                lex_expect(T_close_curly);
            } else {
                read_expr(parent, &bb);
                read_ternary_operation(parent, &bb);
                var_t *rs1 = resize_var(parent, &bb, opstack_pop(), nv);
                add_insn(parent, bb, OP_assign, nv, rs1, NULL, 0, NULL);
            }
        }
    }

    lex_expect(T_semicolon);
    return bb;
}

void parse_array_init(var_t *var,
                      block_t *parent,
                      basic_block_t **bb,
                      bool emit_code)
{
    int elem_size = var->type->size;
    int count = 0;
    var_t *base_addr = NULL;
    var_t *stored_vals[256];
    bool is_implicit = (var->array_size == 0);

    if (emit_code)
        base_addr = var;

    lex_expect(T_open_curly);
    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            var_t *val = NULL;

            if (lex_peek(T_open_curly, NULL) &&
                (var->type->base_type == TYPE_struct ||
                 var->type->base_type == TYPE_typedef)) {
                type_t *struct_type = var->type;
                if (struct_type->base_type == TYPE_typedef &&
                    struct_type->base_struct)
                    struct_type = struct_type->base_struct;

                if (emit_code) {
                    var_t *elem_addr = compute_element_address(
                        parent, bb, base_addr, count, elem_size);
                    lex_expect(T_open_curly);
                    parse_struct_field_init(parent, bb, struct_type, elem_addr,
                                            emit_code);
                    lex_expect(T_close_curly);
                    val = NULL;
                } else {
                    lex_expect(T_open_curly);
                    while (!lex_peek(T_close_curly, NULL)) {
                        if (parent == GLOBAL_BLOCK) {
                            consume_global_constant_syntax();
                        } else {
                            read_expr(parent, bb);
                            read_ternary_operation(parent, bb);
                            opstack_pop();
                        }
                        if (!lex_accept(T_comma))
                            break;
                        if (lex_peek(T_close_curly, NULL))
                            break;
                    }
                    lex_expect(T_close_curly);
                    val = NULL;
                }
            } else {
                if (parent == GLOBAL_BLOCK) {
                    consume_global_constant_syntax();
                    val = NULL;
                } else {
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                    val = opstack_pop();
                }
            }

            if (is_implicit && emit_code && count < 256)
                stored_vals[count] = val;

            if (val && emit_code && !is_implicit && count < var->array_size) {
                var_t target = {0};
                target.type = var->type;
                target.ptr_level = 0;
                var_t *v = resize_var(parent, bb, val, &target);

                var_t *elem_addr = compute_element_address(
                    parent, bb, base_addr, count, elem_size);

                if (elem_size <= 4) {
                    add_insn(parent, *bb, OP_write, NULL, elem_addr, v,
                             elem_size, NULL);
                } else {
                    fatal("Unsupported: struct assignment > 4 bytes in array");
                }
            }

            count++;
            if (!lex_accept(T_comma))
                break;
            if (lex_peek(T_close_curly, NULL))
                break;
        }
    }
    lex_expect(T_close_curly);

    if (is_implicit) {
        if (var->ptr_level > 0)
            var->ptr_level = 0;
        var->array_size = count;

        if (emit_code && count > 0) {
            base_addr = var;

            for (int i = 0; i < count && i < 256; i++) {
                if (!stored_vals[i])
                    continue;
                var_t target = {0};
                target.type = var->type;
                target.ptr_level = 0;
                var_t *v = resize_var(parent, bb, stored_vals[i], &target);

                var_t *elem_addr = compute_element_address(
                    parent, bb, base_addr, i, elem_size);

                add_insn(parent, *bb, OP_write, NULL, elem_addr, v, elem_size,
                         NULL);
            }
        }
    }
}

void read_inner_var_decl(var_t *vd, bool anon, bool is_param)
{
    /* Preserve typedef pointer level - don't reset if already inherited */
    vd->init_val = 0;
    if (is_param) {
        /* However, if the parsed variable is a function parameter,
         * reset its pointer level to zero.
         */
        vd->ptr_level = 0;
    }

    while (lex_accept(T_asterisk)) {
        vd->ptr_level++;
        /* Check for const after asterisk (e.g., int * const ptr).
         * For now, we just consume const qualifiers after pointer.
         * Full support would require tracking const-ness of the pointer
         * itself vs the pointed-to data separately.
         */
        while (lex_peek(T_const, NULL))
            lex_accept(T_const);
    }

    /* is it function pointer declaration? */
    if (lex_accept(T_open_bracket)) {
        func_t func;
        char temp_name[MAX_VAR_LEN];
        lex_expect(T_asterisk);
        lex_ident(T_identifier, temp_name);
        strcpy(vd->var_name, intern_string(temp_name));
        lex_expect(T_close_bracket);
        read_parameter_list_decl(&func, true);
        vd->is_func = true;
    } else {
        if (!anon) {
            char temp_name[MAX_VAR_LEN];
            lex_ident(T_identifier, temp_name);
            strcpy(vd->var_name, intern_string(temp_name));
            if (!lex_peek(T_open_bracket, NULL) && !is_param) {
                if (vd->is_global) {
                    opstack_push(vd);
                }
            }
        }
        if (lex_accept(T_open_square)) {
            char buffer[10];

            /* array with size */
            if (lex_peek(T_numeric, buffer)) {
                vd->array_size = read_numeric_constant(buffer);
                vd->array_dim1 = vd->array_size; /* Store first dimension */
                lex_expect(T_numeric);
            } else {
                /* array without size:
                 * regarded as a pointer although could be nested
                 */
                vd->ptr_level++;
            }
            lex_expect(T_close_square);

            /* Handle multi-dimensional arrays: int matrix[3][4] becomes array
             * of 3*4=12 elements
             */
            if (lex_accept(T_open_square)) {
                if (lex_peek(T_numeric, buffer)) {
                    int next_dim = read_numeric_constant(buffer);
                    lex_expect(T_numeric);
                    vd->array_dim2 = next_dim; /* Store second dimension */
                    if (vd->array_size > 0) {
                        vd->array_size *=
                            next_dim; /* multiply dimensions together */
                    } else {
                        vd->array_size = next_dim;
                    }
                } else {
                    vd->ptr_level++;
                }
                lex_expect(T_close_square);

                /* For now, only support 2D arrays */
                while (lex_accept(T_open_square)) {
                    if (lex_peek(T_numeric, buffer)) {
                        int next_dim = read_numeric_constant(buffer);
                        lex_expect(T_numeric);
                        if (vd->array_size > 0) {
                            vd->array_size *= next_dim;
                        } else {
                            vd->array_size = next_dim;
                        }
                    } else {
                        vd->ptr_level++;
                    }
                    lex_expect(T_close_square);
                }
            }
        } else {
            vd->array_size = 0;
            vd->array_dim1 = 0;
            vd->array_dim2 = 0;
        }
        vd->is_func = false;
    }
}

/* starting next_token, need to check the type */
void read_full_var_decl(var_t *vd, bool anon, bool is_param)
{
    char type_name[MAX_TYPE_LEN];
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (find_type_flag == 1 && lex_accept(T_union)) {
        find_type_flag = 2;
    }
    lex_ident(T_identifier, type_name);
    type_t *type = find_type(type_name, find_type_flag);

    if (!type) {
        printf("Could not find type %s%s\n",
               find_type_flag == 2 ? "struct/union " : "", type_name);
        abort();
    }

    vd->type = type;

    read_inner_var_decl(vd, anon, is_param);
}

/* starting next_token, need to check the type */
void read_partial_var_decl(var_t *vd, var_t *template)
{
    read_inner_var_decl(vd, false, false);
}

void read_parameter_list_decl(func_t *func, bool anon)
{
    int vn = 0;
    lex_expect(T_open_bracket);

    char token[MAX_TYPE_LEN];
    if (lex_peek(T_identifier, token) && !strncmp(token, "void", 4)) {
        next_token = lex_token();
        if (lex_accept(T_close_bracket))
            return;
        func->param_defs[vn].type = TY_void;
        read_inner_var_decl(&func->param_defs[vn], anon, true);
        if (!func->param_defs[vn].ptr_level && !func->param_defs[vn].is_func &&
            !func->param_defs[vn].array_size)
            error("'void' must be the only parameter and unnamed");
        vn++;
        lex_accept(T_comma);
    }

    while (lex_peek(T_identifier, NULL) || lex_peek(T_const, NULL)) {
        /* Check for const qualifier */
        bool is_const = false;
        if (lex_accept(T_const))
            is_const = true;

        read_full_var_decl(&func->param_defs[vn], anon, true);
        func->param_defs[vn].is_const_qualified = is_const;
        vn++;
        lex_accept(T_comma);
    }
    func->num_params = vn;

    /* Up to 'MAX_PARAMS' parameters are accepted for the variadic function. */
    if (lex_accept(T_elipsis))
        func->va_args = 1;

    lex_expect(T_close_bracket);
}

void read_literal_param(block_t *parent, basic_block_t *bb)
{
    char literal[MAX_TOKEN_LEN];
    char combined[MAX_TOKEN_LEN];
    int combined_len = 0;

    /* Read first string literal */
    lex_ident(T_string, literal);
    strcpy(combined, literal);
    combined_len = strlen(literal);

    /* Check for adjacent string literals and concatenate them */
    while (lex_peek(T_string, NULL)) {
        lex_ident(T_string, literal);
        int literal_len = strlen(literal);
        if (combined_len + literal_len >= MAX_TOKEN_LEN - 1)
            error("Concatenated string literal too long");

        strcpy(combined + combined_len, literal);
        combined_len += literal_len;
    }

    const int index = write_symbol(combined);

    var_t *vd = require_typed_ptr_var(parent, TY_char, true);
    gen_name_to(vd->var_name);
    vd->init_val = index;
    opstack_push(vd);
    /* String literals are now in .rodata section */
    add_insn(parent, bb, OP_load_rodata_address, vd, NULL, NULL, 0, NULL);
}

void read_numeric_param(block_t *parent, basic_block_t *bb, bool is_neg)
{
    char token[MAX_ID_LEN];
    int value = 0;
    int i = 0;
    char c;

    lex_ident(T_numeric, token);

    if (token[0] == '-') {
        is_neg = !is_neg;
        i++;
    }
    if (token[0] == '0') {
        if ((token[1] | 32) == 'x') { /* hexdecimal */
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
        } else if ((token[1] | 32) == 'b') { /* binary */
            i = 2;
            do {
                c = token[i++];
                if (c != '0' && c != '1')
                    error("Invalid binary constant");
                c -= '0';
                value = (value * 2) + c;
            } while (token[i] == '0' || token[i] == '1');
        } else { /* octal */
            do {
                c = token[i++];
                if (c > '7')
                    error("Invalid numeric constant");
                c -= '0';
                value = (value * 8) + c;
            } while (is_digit(token[i]));
        }
    } else {
        do {
            c = token[i++] - '0';
            value = (value * 10) + c;
        } while (is_digit(token[i]));
    }

    if (is_neg)
        value = -value;

    var_t *vd = require_var(parent);
    gen_name_to(vd->var_name);
    vd->init_val = value;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

void read_char_param(block_t *parent, basic_block_t *bb)
{
    char token[5];

    lex_ident(T_char, token);

    var_t *vd = require_typed_var(parent, TY_char);
    gen_name_to(vd->var_name);
    vd->init_val = token[0];
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

void read_logical(opcode_t op, block_t *parent, basic_block_t **bb);
void read_func_parameters(func_t *func, block_t *parent, basic_block_t **bb)
{
    int param_num = 0;
    var_t *params[MAX_PARAMS], *param;

    lex_expect(T_open_bracket);
    while (!lex_accept(T_close_bracket)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);

        param = opstack_pop();

        /* Handle parameter type conversion for direct calls.
         * Indirect calls currently don't provide function instance.
         */
        if (func) {
            if (param_num >= func->num_params && func->va_args) {
                param = promote(parent, bb, param, TY_int, 0);
            } else {
                param =
                    resize_var(parent, bb, param, &func->param_defs[param_num]);
            }
        }

        params[param_num++] = param;
        lex_accept(T_comma);
    }

    for (int i = 0; i < param_num; i++) {
        /* The operand should keep alive before calling function. Pass the
         * number of remained parameters to allocator to extend their liveness.
         */
        add_insn(parent, *bb, OP_push, NULL, params[i], NULL, param_num - i,
                 NULL);
    }
}

void read_func_call(func_t *func, block_t *parent, basic_block_t **bb)
{
    /* direct function call */
    read_func_parameters(func, parent, bb);

    add_insn(parent, *bb, OP_call, NULL, NULL, NULL, 0,
             func->return_def.var_name);
}

void read_indirect_call(block_t *parent, basic_block_t **bb)
{
    /* Note: Indirect calls use generic parameter handling */
    read_func_parameters(NULL, parent, bb);

    add_insn(parent, *bb, OP_indirect, NULL, opstack_pop(), NULL, 0, NULL);
}

void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 basic_block_t **bb,
                 bool eval,
                 opcode_t op);

/* Maintain a stack of expression values and operators, depending on next
 * operators' priority. Either apply it or operator on stack first.
 */
void handle_address_of_operator(block_t *parent, basic_block_t **bb)
{
    char token[MAX_VAR_LEN];
    lvalue_t lvalue;
    var_t *vd, *rs1;

    lex_peek(T_identifier, token);
    var_t *var = find_var(token, parent);
    read_lvalue(&lvalue, var, parent, bb, false, OP_generic);

    if (!lvalue.is_reference) {
        rs1 = opstack_pop();
        vd = require_ref_var(parent, lvalue.type, lvalue.ptr_level);
        gen_name_to(vd->var_name);
        opstack_push(vd);
        add_insn(parent, *bb, OP_address_of, vd, rs1, NULL, 0, NULL);
    }
}

void handle_single_dereference(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    int sz;

    if (lex_peek(T_open_bracket, NULL)) {
        /* Handle general expression dereference: *(expr) */
        lex_expect(T_open_bracket);
        read_expr(parent, bb);
        lex_expect(T_close_bracket);

        rs1 = opstack_pop();
        /* For pointer dereference, we need to determine the target type and
         * size. Since we do not have full type tracking in expressions, use
         * defaults
         */
        type_t *deref_type = rs1->type ? rs1->type : TY_int;
        int deref_ptr = rs1->ptr_level > 0 ? rs1->ptr_level - 1 : 0;

        vd = require_deref_var(parent, deref_type, deref_ptr);
        if (deref_ptr > 0)
            sz = PTR_SIZE;
        else
            sz = deref_type->size;
        gen_name_to(vd->var_name);
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
    } else {
        /* Handle simple identifier dereference: *var */
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        lex_peek(T_identifier, token);
        var_t *var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic);

        rs1 = opstack_pop();
        vd = require_deref_var(parent, var->type, var->ptr_level);
        if (lvalue.ptr_level > 1)
            sz = PTR_SIZE;
        else {
            /* For typedef pointers, get the size of the pointed-to type */
            if (lvalue.type && lvalue.type->ptr_level > 0) {
                /* This is a typedef pointer */
                switch (lvalue.type->base_type) {
                case TYPE_char:
                    sz = TY_char->size;
                    break;
                case TYPE_short:
                    sz = TY_short->size;
                    break;
                case TYPE_int:
                    sz = TY_int->size;
                    break;
                case TYPE_void:
                    sz = 1;
                    break;
                default:
                    sz = lvalue.type->size;
                    break;
                }
            } else {
                sz = lvalue.type->size;
            }
        }
        gen_name_to(vd->var_name);
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
    }
}

void handle_multiple_dereference(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    int sz;

    /* Handle consecutive asterisks for multiple dereference: **pp, ***ppp,
     * ***(expr) */
    int deref_count = 1; /* We already consumed one asterisk */
    while (lex_accept(T_asterisk))
        deref_count++;

    /* Check if we have a parenthesized expression or simple identifier */
    if (lex_peek(T_open_bracket, NULL)) {
        /* Handle ***(expr) case */
        lex_expect(T_open_bracket);
        read_expr(parent, bb);
        lex_expect(T_close_bracket);

        /* Apply dereferences one by one */
        for (int i = 0; i < deref_count; i++) {
            rs1 = opstack_pop();
            /* For expression dereference, use default type info */
            type_t *deref_type = rs1->type ? rs1->type : TY_int;
            int deref_ptr = rs1->ptr_level > 0 ? rs1->ptr_level - 1 : 0;

            vd = require_deref_var(parent, deref_type, deref_ptr);
            if (deref_ptr > 0)
                sz = PTR_SIZE;
            else
                sz = deref_type->size;
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
        }
    } else {
        /* Handle **pp, ***ppp case with simple identifier */
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        lex_peek(T_identifier, token);
        var_t *var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic);

        /* Apply dereferences one by one */
        for (int i = 0; i < deref_count; i++) {
            rs1 = opstack_pop();
            vd = require_deref_var(
                parent, var->type,
                lvalue.ptr_level > i ? lvalue.ptr_level - i - 1 : 0);
            if (lvalue.ptr_level > i + 1)
                sz = PTR_SIZE;
            else {
                /* For typedef pointers, get the size of the pointed-to type */
                if (lvalue.type && lvalue.type->ptr_level > 0 &&
                    i == deref_count - 1) {
                    /* This is a typedef pointer on the final dereference */
                    switch (lvalue.type->base_type) {
                    case TYPE_char:
                        sz = TY_char->size;
                        break;
                    case TYPE_short:
                        sz = TY_short->size;
                        break;
                    case TYPE_int:
                        sz = TY_int->size;
                        break;
                    case TYPE_void:
                        sz = 1;
                        break;
                    default:
                        sz = lvalue.type->size;
                        break;
                    }
                } else {
                    sz = lvalue.type->size;
                }
            }
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
        }
    }
}

void handle_sizeof_operator(block_t *parent, basic_block_t **bb)
{
    char token[MAX_TYPE_LEN];
    int ptr_cnt = 0;
    type_t *type = NULL;
    var_t *vd;

    lex_expect(T_open_bracket);

    /* Check if this is sizeof(type) or sizeof(expression) */
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (find_type_flag == 1 && lex_accept(T_union))
        find_type_flag = 2;

    if (lex_peek(T_identifier, token)) {
        /* Try to parse as a type first */
        type = find_type(token, find_type_flag);
        if (type) {
            /* sizeof(type) */
            lex_expect(T_identifier);
            while (lex_accept(T_asterisk))
                ptr_cnt++;
        }
    }

    if (!type) {
        /* sizeof(expression) - parse the expression and get its type */
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
        var_t *expr_var = opstack_pop();
        type = expr_var->type;
        ptr_cnt = expr_var->ptr_level;
    }

    if (!type)
        error("Unable to determine type in sizeof");

    vd = require_var(parent);
    vd->init_val = ptr_cnt ? PTR_SIZE : type->size;
    gen_name_to(vd->var_name);
    opstack_push(vd);
    lex_expect(T_close_bracket);
    add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

void read_expr_operand(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    bool is_neg = false;

    if (lex_accept(T_minus)) {
        is_neg = true;
        if (!lex_peek(T_numeric, NULL) && !lex_peek(T_identifier, NULL) &&
            !lex_peek(T_open_bracket, NULL)) {
            error("Unexpected token after unary minus");
        }
    }

    if (lex_peek(T_string, NULL))
        read_literal_param(parent, *bb);
    else if (lex_peek(T_char, NULL))
        read_char_param(parent, *bb);

    else if (lex_peek(T_numeric, NULL))
        read_numeric_param(parent, *bb, is_neg);
    else if (lex_accept(T_log_not)) {
        read_expr_operand(parent, bb);

        rs1 = opstack_pop();

        /* Constant folding for logical NOT */
        if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            vd->is_const = true;
            vd->init_val = !rs1->init_val;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else {
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(parent, *bb, OP_log_not, vd, rs1, NULL, 0, NULL);
        }
    } else if (lex_accept(T_bit_not)) {
        read_expr_operand(parent, bb);

        rs1 = opstack_pop();

        /* Constant folding for bitwise NOT */
        if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            vd->is_const = true;
            vd->init_val = ~rs1->init_val;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else {
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(parent, *bb, OP_bit_not, vd, rs1, NULL, 0, NULL);
        }
    } else if (lex_accept(T_ampersand)) {
        handle_address_of_operator(parent, bb);
    } else if (lex_accept(T_asterisk)) {
        /* dereference */
        if (lex_peek(T_asterisk, NULL)) {
            handle_multiple_dereference(parent, bb);
        } else {
            handle_single_dereference(parent, bb);
        }
    } else if (lex_accept(T_open_bracket)) {
        /* Check if this is a cast, compound literal, or parenthesized
         * expression */
        char lookahead_token[MAX_TYPE_LEN];
        bool is_compound_literal = false;
        bool is_cast = false;
        type_t *cast_or_literal_type = NULL;
        int cast_ptr_level = 0;

        /* Look ahead to see if we have a typename followed by ) */
        if (lex_peek(T_identifier, lookahead_token)) {
            /* Check if it's a basic type or typedef */
            type_t *type = find_type(lookahead_token, true);

            if (type) {
                /* Save current position to backtrack if needed */
                int saved_pos = SOURCE->size;
                char saved_char = next_char;
                token_t saved_token = next_token;

                /* Try to parse as typename */
                lex_expect(T_identifier);

                /* Check for pointer types: int*, char*, etc. */
                int ptr_level = 0;
                while (lex_accept(T_asterisk)) {
                    ptr_level++;
                }

                /* Check for array brackets: [size] or [] */
                bool is_array = false;
                if (lex_accept(T_open_square)) {
                    is_array = true;
                    /* Skip array size if present */
                    if (lex_peek(T_numeric, NULL)) {
                        char size_buffer[10];
                        lex_ident(T_numeric, size_buffer);
                    }
                    lex_expect(T_close_square);
                }

                /* Check what follows the closing ) */
                if (lex_accept(T_close_bracket)) {
                    if (lex_peek(T_open_curly, NULL)) {
                        /* (type){...} - compound literal */
                        is_compound_literal = true;
                        cast_or_literal_type = type;
                        cast_ptr_level = ptr_level;
                        /* Store is_array flag in cast_ptr_level if it's an
                         * array
                         */
                        if (is_array) {
                            /* Special marker for array compound literal */
                            cast_ptr_level = -1;
                        }
                    } else {
                        /* (type)expr - cast expression */
                        is_cast = true;
                        cast_or_literal_type = type;
                        cast_ptr_level = ptr_level;
                    }
                } else {
                    /* Not a cast or compound literal - backtrack */
                    SOURCE->size = saved_pos;
                    next_char = saved_char;
                    next_token = saved_token;
                }
            }
        }

        if (is_cast) {
            /* Process cast: (type)expr */
            /* Parse the expression to be cast */
            read_expr_operand(parent, bb);

            /* Get the expression result */
            var_t *expr_var = opstack_pop();

            /* Create variable for cast result */
            var_t *cast_var = require_typed_ptr_var(
                parent, cast_or_literal_type, cast_ptr_level);
            gen_name_to(cast_var->var_name);

            /* Generate cast IR */
            add_insn(parent, *bb, OP_cast, cast_var, expr_var, NULL,
                     cast_or_literal_type->size, NULL);

            /* Push the cast result */
            opstack_push(cast_var);

        } else if (is_compound_literal) {
            /* Process compound literal */
            lex_expect(T_open_curly);

            /* Create variable for compound literal result */
            var_t *compound_var =
                require_typed_var(parent, cast_or_literal_type);
            gen_name_to(compound_var->var_name);

            /* Check if this is an array compound literal (int[]){...} */
            bool is_array_literal = (cast_ptr_level == -1);
            if (is_array_literal)
                cast_ptr_level = 0; /* Reset for normal processing */

            /* Check if this is a pointer compound literal */
            if (cast_ptr_level > 0) {
                /* Pointer compound literal: (int*){&x} */
                compound_var->ptr_level = cast_ptr_level;

                /* Parse the pointer value (should be an address) */
                if (!lex_peek(T_close_curly, NULL)) {
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                    var_t *ptr_val = opstack_pop();

                    /* For pointer compound literals, store the address */
                    compound_var->init_val = ptr_val->init_val;

                    /* Consume additional values if present (for pointer arrays)
                     */
                    while (lex_accept(T_comma)) {
                        if (lex_peek(T_close_curly, NULL))
                            break;
                        read_expr(parent, bb);
                        read_ternary_operation(parent, bb);
                        opstack_pop();
                    }
                } else {
                    /* Empty pointer compound literal: (int*){} */
                    compound_var->init_val = 0; /* NULL pointer */
                }

                /* Generate code for pointer compound literal */
                opstack_push(compound_var);
                add_insn(parent, *bb, OP_load_constant, compound_var, NULL,
                         NULL, 0, NULL);
            } else if (cast_or_literal_type->base_type == TYPE_struct ||
                       cast_or_literal_type->base_type == TYPE_typedef) {
                /* Struct compound literal support (including typedef structs)
                 */
                /* For typedef structs, the actual struct info is in the type */

                /* Initialize struct compound literal */
                compound_var->init_val = 0;
                compound_var->ptr_level = 0;

                /* Parse first field value */
                if (!lex_peek(T_close_curly, NULL)) {
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                    var_t *first_field = opstack_pop();
                    compound_var->init_val = first_field->init_val;

                    /* Consume additional fields if present */
                    while (lex_accept(T_comma)) {
                        if (lex_peek(T_close_curly, NULL)) {
                            break;
                        }
                        read_expr(parent, bb);
                        read_ternary_operation(parent, bb);
                        opstack_pop(); /* Consume additional field values */
                    }
                }

                /* Generate code for struct compound literal */
                opstack_push(compound_var);
                add_insn(parent, *bb, OP_load_constant, compound_var, NULL,
                         NULL, 0, NULL);
            } else if (cast_or_literal_type->base_type == TYPE_int ||
                       cast_or_literal_type->base_type == TYPE_short ||
                       cast_or_literal_type->base_type == TYPE_char) {
                /* Handle empty compound literals */
                if (lex_peek(T_close_curly, NULL)) {
                    /* Empty compound literal: (int){} */
                    compound_var->init_val = 0;
                    compound_var->array_size = 0;
                    opstack_push(compound_var);
                    add_insn(parent, *bb, OP_load_constant, compound_var, NULL,
                             NULL, 0, NULL);
                } else if (lex_peek(T_numeric, NULL) ||
                           lex_peek(T_identifier, NULL) ||
                           lex_peek(T_char, NULL)) {
                    /* Parse first element */
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);

                    /* Check if there are more elements (comma-separated) or if
                     * it's an explicit array
                     */
                    if (lex_peek(T_comma, NULL) || is_array_literal) {
                        /* Array compound literal: (int[]){1, 2, 3} */
                        var_t *first_element = opstack_pop();

                        /* Store elements temporarily */
                        var_t *elements[256];
                        elements[0] = first_element;
                        int element_count = 1;

                        /* Parse remaining elements */
                        while (lex_accept(T_comma)) {
                            if (lex_peek(T_close_curly, NULL))
                                break; /* Trailing comma */

                            read_expr(parent, bb);
                            read_ternary_operation(parent, bb);
                            if (element_count < 256) {
                                elements[element_count] = opstack_pop();
                            } else {
                                opstack_pop(); /* Discard if too many */
                            }
                            element_count++;
                        }

                        /* Set array metadata */
                        compound_var->array_size = element_count;
                        compound_var->init_val = first_element->init_val;

                        /* Allocate space for the array on stack */
                        add_insn(parent, *bb, OP_allocat, compound_var, NULL,
                                 NULL, 0, NULL);

                        /* Initialize each element */
                        for (int i = 0; i < element_count && i < 256; i++) {
                            if (!elements[i])
                                continue;

                            /* Store element at offset i * sizeof(element) */
                            var_t *elem_offset = require_var(parent);
                            elem_offset->init_val =
                                i * cast_or_literal_type->size;
                            gen_name_to(elem_offset->var_name);
                            add_insn(parent, *bb, OP_load_constant, elem_offset,
                                     NULL, NULL, 0, NULL);

                            /* Calculate address of element */
                            var_t *elem_addr = require_var(parent);
                            elem_addr->ptr_level = 1;
                            gen_name_to(elem_addr->var_name);
                            add_insn(parent, *bb, OP_add, elem_addr,
                                     compound_var, elem_offset, 0, NULL);

                            /* Store the element value */
                            add_insn(parent, *bb, OP_write, NULL, elem_addr,
                                     elements[i], cast_or_literal_type->size,
                                     NULL);
                        }

                        /* Store first element value for array-to-scalar */
                        compound_var->init_val = first_element->init_val;

                        /* Create result that provides first element access.
                         * This enables array compound literals in scalar
                         * contexts: int x = (int[]){1,2,3};  // x gets 1 int y
                         * = 5 + (int[]){10}; // adds 5 + 10
                         */
                        var_t *result_var = require_var(parent);
                        gen_name_to(result_var->var_name);
                        result_var->type = compound_var->type;
                        result_var->ptr_level = 0;
                        result_var->array_size = 0;

                        /* Read first element from the array */
                        add_insn(parent, *bb, OP_read, result_var, compound_var,
                                 NULL, compound_var->type->size, NULL);
                        opstack_push(result_var);
                    } else {
                        /* Single value: (int){42} - scalar compound literal */
                        compound_var = opstack_pop();
                        opstack_push(compound_var);
                    }
                }
            }

            lex_expect(T_close_curly);
        } else {
            /* Regular parenthesized expression */
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
            lex_expect(T_close_bracket);
        }
    } else if (lex_accept(T_sizeof)) {
        handle_sizeof_operator(parent, bb);
    } else {
        /* function call, constant or variable - read token and determine */
        opcode_t prefix_op = OP_generic;
        char token[MAX_ID_LEN];

        if (lex_accept(T_increment))
            prefix_op = OP_add;
        else if (lex_accept(T_decrement))
            prefix_op = OP_sub;

        lex_peek(T_identifier, token);

        /* is a constant or variable? */
        constant_t *con = find_constant(token);
        var_t *var = find_var(token, parent);
        func_t *func = find_func(token);
        int macro_param_idx = find_macro_param_src_idx(token, parent);
        macro_t *mac = find_macro(token);

        if (!strcmp(token, "__VA_ARGS__")) {
            /* 'size' has pointed at the character after __VA_ARGS__ */
            int remainder, t = SOURCE->size;
            macro_t *macro = parent->macro;

            if (!macro)
                error("The '__VA_ARGS__' identifier can only be used in macro");
            if (!macro->is_variadic)
                error("Unexpected identifier '__VA_ARGS__'");

            remainder = macro->num_params - macro->num_param_defs;
            for (int i = 0; i < remainder; i++) {
                SOURCE->size = macro->params[macro->num_params - remainder + i];
                next_char = SOURCE->elements[SOURCE->size];
                next_token = lex_token();
                read_expr(parent, bb);
            }
            SOURCE->size = t;
            next_char = SOURCE->elements[SOURCE->size];
            next_token = lex_token();
        } else if (mac) {
            if (parent->macro)
                error("Nested macro is not yet supported");

            parent->macro = mac;
            mac->num_params = 0;
            lex_expect(T_identifier);

            /* 'size' has pointed at the first parameter */
            while (!lex_peek(T_close_bracket, NULL)) {
                mac->params[mac->num_params++] = SOURCE->size;
                do {
                    next_token = lex_token();
                } while (next_token != T_comma &&
                         next_token != T_close_bracket);
            }
            /* move 'size' to the macro body */
            macro_return_idx = SOURCE->size;
            SOURCE->size = mac->start_source_idx;
            next_char = SOURCE->elements[SOURCE->size];
            lex_expect(T_close_bracket);

            skip_newline = 0;
            read_expr(parent, bb);

            /* cleanup */
            skip_newline = 1;
            parent->macro = NULL;
            macro_return_idx = 0;
        } else if (macro_param_idx) {
            /* "expand" the argument from where it comes from */
            int t = SOURCE->size;
            SOURCE->size = macro_param_idx;
            next_char = SOURCE->elements[SOURCE->size];
            next_token = lex_token();
            read_expr(parent, bb);
            SOURCE->size = t;
            next_char = SOURCE->elements[SOURCE->size];
            next_token = lex_token();
        } else if (con) {
            vd = require_var(parent);
            vd->init_val = con->value;
            gen_name_to(vd->var_name);
            opstack_push(vd);
            lex_expect(T_identifier);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else if (var) {
            /* evalue lvalue expression */
            lvalue_t lvalue;
            read_lvalue(&lvalue, var, parent, bb, true, prefix_op);

            /* is it an indirect call with function pointer? */
            if (lex_peek(T_open_bracket, NULL)) {
                read_indirect_call(parent, bb);

                vd = require_var(parent);
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_func_ret, vd, NULL, NULL, 0, NULL);
            }
        } else if (func) {
            lex_expect(T_identifier);

            if (lex_peek(T_open_bracket, NULL)) {
                read_func_call(func, parent, bb);

                vd = require_typed_ptr_var(parent, func->return_def.type,
                                           func->return_def.ptr_level);
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_func_ret, vd, NULL, NULL, 0, NULL);
            } else {
                /* indirective function pointer assignment */
                vd = require_var(parent);
                vd->is_func = true;
                strcpy(vd->var_name, intern_string(token));
                opstack_push(vd);
            }
        } else if (lex_accept(T_open_curly)) {
            parse_array_literal_expr(parent, bb);
        } else {
            printf("%s\n", token);
            /* unknown expression */
            error("Unrecognized expression token");
        }

        if (is_neg) {
            rs1 = opstack_pop();

            /* Constant folding for negation */
            if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                vd->is_const = true;
                vd->init_val = -rs1->init_val;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);
            } else {
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_negate, vd, rs1, NULL, 0, NULL);
            }
        }
    }
}

void finalize_logical(opcode_t op,
                      block_t *parent,
                      basic_block_t **bb,
                      basic_block_t *shared_bb);

bool is_logical(opcode_t op)
{
    return op == OP_log_and || op == OP_log_or;
}

/* Helper function to calculate element size for pointer operations */
int get_pointer_element_size(var_t *ptr_var)
{
    int element_size = PTR_SIZE; /* Default to pointer size */

    if (!ptr_var || !ptr_var->type)
        return element_size;

    /* Direct pointer with type info */
    if (ptr_var->ptr_level && ptr_var->type)
        return ptr_var->type->size;

    /* Typedef pointer or array-derived pointer */
    if (ptr_var->type && ptr_var->type->ptr_level > 0) {
        switch (ptr_var->type->base_type) {
        case TYPE_char:
            return TY_char->size;
        case TYPE_short:
            return TY_short->size;
        case TYPE_int:
            return TY_int->size;
        case TYPE_void:
            return 1;
        default:
            return ptr_var->type->size ? ptr_var->type->size : PTR_SIZE;
        }
    }

    /* Array-derived pointer without ptr_level set */
    if (ptr_var->type) {
        switch (ptr_var->type->base_type) {
        case TYPE_char:
            return TY_char->size;
        case TYPE_short:
            return TY_short->size;
        case TYPE_int:
            return TY_int->size;
        case TYPE_void:
            return 1;
        default:
            return ptr_var->type->size ? ptr_var->type->size : PTR_SIZE;
        }
    }

    return element_size;
}

/* Helper function to handle pointer difference calculation */
void handle_pointer_difference(block_t *parent,
                               basic_block_t **bb,
                               var_t *rs1,
                               var_t *rs2)
{
    /* First perform the subtraction to get byte difference */
    var_t *vd = require_var(parent);
    gen_name_to(vd->var_name);
    add_insn(parent, *bb, OP_sub, vd, rs1, rs2, 0, NULL);

    /* Determine element size for division */
    int element_size = get_pointer_element_size(rs1);

    /* Divide by element size to get element count */
    if (element_size > 1) {
        var_t *size_const = require_var(parent);
        gen_name_to(size_const->var_name);
        size_const->init_val = element_size;
        add_insn(parent, *bb, OP_load_constant, size_const, NULL, NULL, 0,
                 NULL);

        var_t *result = require_var(parent);
        gen_name_to(result->var_name);
        add_insn(parent, *bb, OP_div, result, vd, size_const, 0, NULL);
        /* Push the result */
        opstack_push(result);
    } else {
        /* Element size is 1 (e.g., char), no division needed */
        opstack_push(vd);
    }
}

/* Helper function to handle pointer arithmetic (add/sub with scaling) */
void handle_pointer_arithmetic(block_t *parent,
                               basic_block_t **bb,
                               opcode_t op,
                               var_t *rs1,
                               var_t *rs2)
{
    var_t *ptr_var = NULL;
    var_t *int_var = NULL;
    int element_size = 0;

    /* Pointer arithmetic: differences (char*, int*, struct*, etc.),
     * addition/increment with scaling, and array indexing.
     */

    /* Check if both operands are pointers (pointer difference) */
    if (op == OP_sub) {
        /* If both are variables (not temporaries), look them up */
        var_t *orig_rs1 = rs1, *orig_rs2 = rs2;

        /* If they have names, they might be variable references - look them up
         */
        if (rs1->var_name[0] && !rs1->init_val) {
            var_t *found = find_var(rs1->var_name, parent);
            if (found)
                orig_rs1 = found;
        }
        if (rs2->var_name[0] && !rs2->init_val) {
            var_t *found = find_var(rs2->var_name, parent);
            if (found)
                orig_rs2 = found;
        }

        /* Check if both have ptr_level or typedef pointer type */
        bool rs1_is_ptr = (orig_rs1->ptr_level > 0) ||
                          (orig_rs1->type && orig_rs1->type->ptr_level > 0);
        bool rs2_is_ptr = (orig_rs2->ptr_level > 0) ||
                          (orig_rs2->type && orig_rs2->type->ptr_level > 0);

        /* If variable lookup failed, check the passed variables directly */
        if (!rs1_is_ptr) {
            rs1_is_ptr =
                (rs1->ptr_level > 0) || (rs1->type && rs1->type->ptr_level > 0);
        }
        if (!rs2_is_ptr) {
            rs2_is_ptr =
                (rs2->ptr_level > 0) || (rs2->type && rs2->type->ptr_level > 0);
        }

        if (rs1_is_ptr && rs2_is_ptr) {
            /* Both are pointers - this is pointer difference */
            /* Determine element size */
            element_size = PTR_SIZE; /* Default */

            /* Get element size from the first pointer */
            if (orig_rs1->type) {
                /* Check if this is a typedef pointer or regular pointer */
                if (orig_rs1->type->ptr_level > 0) {
                    /* Typedef pointer - element size from base type */
                    switch (orig_rs1->type->base_type) {
                    case TYPE_char:
                        element_size = 1;
                        break;
                    case TYPE_short:
                        element_size = 2;
                        break;
                    case TYPE_int:
                        element_size = 4;
                        break;
                    default:
                        /* For struct/union typedef pointers, use the actual
                         * type size
                         */
                        if (orig_rs1->type->size > 0)
                            element_size = orig_rs1->type->size;
                        break;
                    }
                } else if (orig_rs1->ptr_level > 0) {
                    /* Regular pointer (e.g., int *p) - type gives the base type
                     */
                    switch (orig_rs1->type->base_type) {
                    case TYPE_char:
                        element_size = 1;
                        break;
                    case TYPE_short:
                        element_size = 2;
                        break;
                    case TYPE_int:
                        element_size = 4;
                        break;
                    case TYPE_void:
                        element_size = 1; /* void* arithmetic uses byte size */
                        break;
                    default:
                        /* For struct pointers, use the struct size */
                        element_size = orig_rs1->type->size;
                        break;
                    }
                }
            }

            /* Perform subtraction first */
            var_t *diff = require_var(parent);
            gen_name_to(diff->var_name);
            add_insn(parent, *bb, OP_sub, diff, rs1, rs2, 0, NULL);

            /* Then divide by element size if needed */
            if (element_size > 1) {
                var_t *size_const = require_var(parent);
                gen_name_to(size_const->var_name);
                size_const->init_val = element_size;
                add_insn(parent, *bb, OP_load_constant, size_const, NULL, NULL,
                         0, NULL);

                var_t *result = require_var(parent);
                gen_name_to(result->var_name);
                add_insn(parent, *bb, OP_div, result, diff, size_const, 0,
                         NULL);
                opstack_push(result);
            } else {
                opstack_push(diff);
            }
            return;
        }
    }
    /* Determine which operand is the pointer for regular pointer arithmetic */
    if (rs1->ptr_level || (rs1->type && rs1->type->ptr_level > 0)) {
        ptr_var = rs1;
        int_var = rs2;
        element_size = get_pointer_element_size(rs1);
    } else if (rs2->ptr_level || (rs2->type && rs2->type->ptr_level > 0)) {
        /* Only for addition (p + n == n + p) */
        if (op == OP_add) {
            ptr_var = rs2;
            int_var = rs1;
            element_size = get_pointer_element_size(rs2);
            /* Swap operands so pointer is rs1 */
            rs1 = ptr_var;
            rs2 = int_var;
        }
    }

    /* If we need to scale the integer operand */
    if (ptr_var && element_size > 1) {
        /* Create multiplication by element size */
        var_t *size_const = require_var(parent);
        gen_name_to(size_const->var_name);
        size_const->init_val = element_size;
        add_insn(parent, *bb, OP_load_constant, size_const, NULL, NULL, 0,
                 NULL);

        var_t *scaled = require_var(parent);
        gen_name_to(scaled->var_name);
        add_insn(parent, *bb, OP_mul, scaled, int_var, size_const, 0, NULL);

        /* Use scaled value as rs2 */
        rs2 = scaled;
    }

    /* Perform the operation */
    var_t *vd = require_var(parent);
    /* Preserve pointer type metadata on results of pointer arithmetic */
    if (ptr_var) {
        vd->type = ptr_var->type;
        vd->ptr_level = ptr_var->ptr_level;
    }
    gen_name_to(vd->var_name);
    opstack_push(vd);
    add_insn(parent, *bb, op, vd, rs1, rs2, 0, NULL);
}

/* Helper function to check if pointer arithmetic is needed */
bool is_pointer_operation(opcode_t op, var_t *rs1, var_t *rs2)
{
    if (op != OP_add && op != OP_sub)
        return false;

    return (rs1->ptr_level || (rs1->type && rs1->type->ptr_level > 0) ||
            rs2->ptr_level || (rs2->type && rs2->type->ptr_level > 0));
}

/* Helper function to check if a variable is a pointer based on its declaration
 */
bool is_pointer_var(var_t *v, block_t *parent)
{
    if (!v || !v->var_name[0])
        return false;

    /* Check if it has explicit ptr_level or type with ptr_level */
    if (v->ptr_level > 0 || (v->type && v->type->ptr_level > 0))
        return true;

    /* For variables that lost their type info during loading,
     * try to find the original declaration */
    var_t *orig = find_var(v->var_name, parent);
    if (orig &&
        (orig->ptr_level > 0 || (orig->type && orig->type->ptr_level > 0)))
        return true;

    return false;
}

void read_expr(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1, *rs2;
    opcode_t oper_stack[10];
    int oper_stack_idx = 0;

    /* These variables used for parsing logical-and/or operation.
     *
     * For the logical-and operation, the false condition code path for testing
     * each operand uses the same code snippet (basic block).
     *
     * Likewise, when testing each operand for the logical-or operation, all of
     * them share a unified code path for the true condition.
     */
    bool has_prev_log_op = false;
    opcode_t prev_log_op = 0, pprev_log_op = 0;
    basic_block_t *log_and_shared_bb = bb_create(parent),
                  *log_or_shared_bb = bb_create(parent);

    read_expr_operand(parent, bb);

    opcode_t op = get_operator();
    if (op == OP_generic || op == OP_ternary)
        return;
    if (is_logical(op)) {
        bb_connect(*bb, op == OP_log_and ? log_and_shared_bb : log_or_shared_bb,
                   op == OP_log_and ? ELSE : THEN);
        read_logical(op, parent, bb);
        has_prev_log_op = true;
        prev_log_op = op;
    } else
        oper_stack[oper_stack_idx++] = op;
    read_expr_operand(parent, bb);
    op = get_operator();

    while (op != OP_generic && op != OP_ternary) {
        if (oper_stack_idx > 0) {
            int same = 0;
            do {
                opcode_t top_op = oper_stack[oper_stack_idx - 1];
                if (get_operator_prio(top_op) >= get_operator_prio(op)) {
                    rs2 = opstack_pop();
                    rs1 = opstack_pop();

                    /* Handle pointer arithmetic for addition and subtraction */
                    if (is_pointer_operation(top_op, rs1, rs2)) {
                        /* handle_pointer_arithmetic handles both pointer
                         * differences and regular pointer arithmetic internally
                         */
                        handle_pointer_arithmetic(parent, bb, top_op, rs1, rs2);
                        oper_stack_idx--;
                        continue;
                    }

                    vd = require_var(parent);
                    gen_name_to(vd->var_name);
                    opstack_push(vd);
                    add_insn(parent, *bb, top_op, vd, rs1, rs2, 0, NULL);

                    oper_stack_idx--;
                } else
                    same = 1;
            } while (oper_stack_idx > 0 && same == 0);
        }
        if (is_logical(op)) {
            if (prev_log_op == 0 || prev_log_op == op) {
                bb_connect(
                    *bb,
                    op == OP_log_and ? log_and_shared_bb : log_or_shared_bb,
                    op == OP_log_and ? ELSE : THEN);
                read_logical(op, parent, bb);
                prev_log_op = op;
                has_prev_log_op = true;
            } else if (prev_log_op == OP_log_and) {
                /* For example: a && b || c
                 * previous opcode: prev_log_op == OP_log_and
                 * current opcode:  op == OP_log_or
                 * current operand: b
                 *
                 * Finalize the logical-and operation and test the operand for
                 * the following logical-or operation.
                 */
                finalize_logical(prev_log_op, parent, bb, log_and_shared_bb);
                log_and_shared_bb = bb_create(parent);
                bb_connect(*bb, log_or_shared_bb, THEN);
                read_logical(op, parent, bb);

                /* Here are two cases to illustrate the following assignments
                 * after finalizing the logical-and operation and testing the
                 * operand for the following logical-or operation.
                 *
                 * 1. a && b || c
                 *    pprev opcode:    pprev_log_op == 0 (no opcode)
                 *    previous opcode: prev_log_op == OP_log_and
                 *    current opcode:  op == OP_log_or
                 *    current operand: b
                 *
                 *    The current opcode should become the previous opcode,
                 * and the pprev opcode remains 0.
                 *
                 * 2. a || b && c || d
                 *    pprev opcode:    pprev_log_op == OP_log_or
                 *    previous opcode: prev_log_op == OP_log_and
                 *    current opcode:  op == OP_log_or
                 *    current operand: b
                 *
                 *    The previous opcode should inherit the pprev opcode, which
                 * is equivalent to inheriting the current opcode because both
                 * of pprev opcode and current opcode are logical-or operator.
                 *
                 *    Thus, pprev opcode is considered used and is cleared to 0.
                 *
                 * Eventually, the current opcode becomes the previous opcode
                 * and pprev opcode is set to 0.
                 * */
                prev_log_op = op;
                pprev_log_op = 0;
            } else {
                /* For example: a || b && c
                 * previous opcode: prev_log_op == OP_log_or
                 * current opcode:  op == OP_log_and
                 * current operand: b
                 *
                 * Using the logical-and operation to test the current operand
                 * instead of using the logical-or operation.
                 *
                 * Then, the previous opcode becomes pprev opcode and the
                 * current opcode becomes the previous opcode.
                 */
                bb_connect(*bb, log_and_shared_bb, ELSE);
                read_logical(op, parent, bb);
                pprev_log_op = prev_log_op;
                prev_log_op = op;
            }
        } else {
            while (has_prev_log_op &&
                   (get_operator_prio(op) < get_operator_prio(prev_log_op))) {
                /* When encountering an operator with lower priority, conclude
                 * the current logical-and/or and create a new basic block for
                 * next logical-and/or operator.
                 */
                finalize_logical(prev_log_op, parent, bb,
                                 prev_log_op == OP_log_and ? log_and_shared_bb
                                                           : log_or_shared_bb);
                if (prev_log_op == OP_log_and)
                    log_and_shared_bb = bb_create(parent);
                else
                    log_or_shared_bb = bb_create(parent);

                /* After finalizing the previous logical-and/or operation, the
                 * prev_log_op should inherit pprev_log_op and continue to check
                 * whether to finalize a logical-and/or operation.
                 */
                prev_log_op = pprev_log_op;
                has_prev_log_op = prev_log_op != 0;
                pprev_log_op = 0;
            }
        }
        read_expr_operand(parent, bb);
        if (!is_logical(op))
            oper_stack[oper_stack_idx++] = op;
        op = get_operator();
    }

    while (oper_stack_idx > 0) {
        opcode_t top_op = oper_stack[--oper_stack_idx];
        rs2 = opstack_pop();
        rs1 = opstack_pop();

        /* Pointer arithmetic handling */
        if (is_pointer_operation(top_op, rs1, rs2)) {
            handle_pointer_arithmetic(parent, bb, top_op, rs1, rs2);
            continue; /* skip normal processing */
        }

        /* Constant folding for binary operations */
        if (rs1 && rs2 && rs1->init_val && !rs1->ptr_level && !rs1->is_global &&
            rs2->init_val && !rs2->ptr_level && !rs2->is_global) {
            /* Both operands are compile-time constants */
            int result = 0;
            bool folded = true;

            switch (top_op) {
            case OP_add:
                result = rs1->init_val + rs2->init_val;
                break;
            case OP_sub:
                result = rs1->init_val - rs2->init_val;
                break;
            case OP_mul:
                result = rs1->init_val * rs2->init_val;
                break;
            case OP_div:
                if (rs2->init_val != 0)
                    result = rs1->init_val / rs2->init_val;
                else
                    folded = false; /* Division by zero */
                break;
            case OP_mod:
                if (rs2->init_val != 0)
                    result = rs1->init_val % rs2->init_val;
                else
                    folded = false; /* Modulo by zero */
                break;
            case OP_bit_and:
                result = rs1->init_val & rs2->init_val;
                break;
            case OP_bit_or:
                result = rs1->init_val | rs2->init_val;
                break;
            case OP_bit_xor:
                result = rs1->init_val ^ rs2->init_val;
                break;
            case OP_lshift:
                result = rs1->init_val << rs2->init_val;
                break;
            case OP_rshift:
                result = rs1->init_val >> rs2->init_val;
                break;
            case OP_eq:
                result = rs1->init_val == rs2->init_val;
                break;
            case OP_neq:
                result = rs1->init_val != rs2->init_val;
                break;
            case OP_lt:
                result = rs1->init_val < rs2->init_val;
                break;
            case OP_leq:
                result = rs1->init_val <= rs2->init_val;
                break;
            case OP_gt:
                result = rs1->init_val > rs2->init_val;
                break;
            case OP_geq:
                result = rs1->init_val >= rs2->init_val;
                break;
            default:
                folded = false;
                break;
            }

            if (folded) {
                /* Create constant result */
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                vd->init_val = result;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);
            } else {
                /* Normal operation - folding failed or not supported */
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, top_op, vd, rs1, rs2, 0, NULL);
            }
        } else {
            /* Normal operation */
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(parent, *bb, top_op, vd, rs1, rs2, 0, NULL);
        }
    }
    while (has_prev_log_op) {
        finalize_logical(
            prev_log_op, parent, bb,
            prev_log_op == OP_log_and ? log_and_shared_bb : log_or_shared_bb);

        prev_log_op = pprev_log_op;
        has_prev_log_op = prev_log_op != 0;
        pprev_log_op = 0;
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
                 basic_block_t **bb,
                 bool eval,
                 opcode_t prefix_op)
{
    var_t *vd, *rs1, *rs2;
    bool is_address_got = false;
    bool is_member = false;

    /* already peeked and have the variable */
    lex_expect(T_identifier);

    lvalue->type = var->type;
    lvalue->size = get_size(var);
    lvalue->ptr_level = var->ptr_level;
    lvalue->is_func = var->is_func;
    lvalue->is_reference = false;

    opstack_push(var);

    if (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
        lex_peek(T_dot, NULL))
        lvalue->is_reference = true;

    while (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
           lex_peek(T_dot, NULL)) {
        if (lex_accept(T_open_square)) {
            /* if subscripted member's is not yet resolved, dereference to
             * resolve base address.
             * e.g., dereference of "->" in "data->raw[0]" would be performed
             * here.
             */
            if (lvalue->is_reference && lvalue->ptr_level && is_member) {
                rs1 = opstack_pop();
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, rs1, NULL, 4, NULL);
            }

            /* var must be either a pointer or an array of some type */
            /* For typedef pointers, check the type's ptr_level */
            bool is_typedef_pointer = (var->type && var->type->ptr_level > 0);
            if (var->ptr_level == 0 && var->array_size == 0 &&
                !is_typedef_pointer)
                error("Cannot apply square operator to non-pointer");

            /* if nested pointer, still pointer */
            /* Also handle typedef pointers which have ptr_level == 0 */
            if ((var->ptr_level <= 1 || is_typedef_pointer) &&
                var->array_size == 0) {
                /* For typedef pointers, get the size of the base type that the
                 * pointer points to
                 */
                if (lvalue->type->ptr_level > 0) {
                    /* This is a typedef pointer, get base type size */
                    switch (lvalue->type->base_type) {
                    case TYPE_char:
                        lvalue->size = TY_char->size;
                        break;
                    case TYPE_short:
                        lvalue->size = TY_short->size;
                        break;
                    case TYPE_int:
                        lvalue->size = TY_int->size;
                        break;
                    case TYPE_void:
                        /* void pointers treated as byte pointers */
                        lvalue->size = 1;
                        break;
                    default:
                        lvalue->size = lvalue->type->size;
                        break;
                    }
                } else {
                    lvalue->size = lvalue->type->size;
                }
            }

            read_expr(parent, bb);

            /* multiply by element size */
            /* For 2D arrays, check if this is the first or second dimension */
            int multiplier = lvalue->size;

            /* If this is the first index of a 2D array, multiply by dim2 *
             * element_size
             */
            if (!is_address_got && var->array_dim2 > 0)
                multiplier = var->array_dim2 * lvalue->size;

            if (multiplier != 1) {
                vd = require_var(parent);
                vd->init_val = multiplier;
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, vd, rs1, rs2, 0, NULL);
            }

            rs2 = opstack_pop();
            rs1 = opstack_pop();
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);

            lex_expect(T_close_square);
            is_address_got = true;
            is_member = true;
            lvalue->is_reference = true;
        } else {
            char token[MAX_ID_LEN];

            if (lex_accept(T_arrow)) {
                /* resolve where the pointer points at from the calculated
                 * address in a structure.
                 */
                if (is_member) {
                    rs1 = opstack_pop();
                    vd = require_var(parent);
                    gen_name_to(vd->var_name);
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_read, vd, rs1, NULL, 4, NULL);
                }
            } else {
                lex_expect(T_dot);

                if (!is_address_got) {
                    rs1 = opstack_pop();
                    vd = require_var(parent);
                    gen_name_to(vd->var_name);
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_address_of, vd, rs1, NULL, 0,
                             NULL);

                    is_address_got = true;
                }
            }

            lex_ident(T_identifier, token);

            /* change type currently pointed to */
            var = find_member(token, lvalue->type);
            lvalue->type = var->type;
            lvalue->ptr_level = var->ptr_level;
            lvalue->is_func = var->is_func;
            lvalue->size = get_size(var);

            /* if it is an array, get the address of first element instead of
             * its value.
             */
            if (var->array_size > 0)
                lvalue->is_reference = false;

            /* move pointer to offset of structure */
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            vd->init_val = var->offset;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs2 = opstack_pop();
            rs1 = opstack_pop();
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);

            is_address_got = true;
            is_member = true;
        }
    }

    if (!eval)
        return;

    /* Only handle pointer arithmetic if we have a pointer/array that hasn't
     * been dereferenced. After array indexing like arr[0], we have a value, not
     * a pointer.
     */
    if (lex_peek(T_plus, NULL) && (var->ptr_level || var->array_size) &&
        !lvalue->is_reference) {
        while (lex_peek(T_plus, NULL) && (var->ptr_level || var->array_size)) {
            lex_expect(T_plus);
            if (lvalue->is_reference) {
                rs1 = opstack_pop();
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, rs1, NULL, lvalue->size,
                         NULL);
            }

            read_expr_operand(parent, bb);

            lvalue->size = lvalue->type->size;

            if (lvalue->size > 1) {
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                vd->init_val = lvalue->size;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, vd, rs1, rs2, 0, NULL);
            }

            rs2 = opstack_pop();
            rs1 = opstack_pop();
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);
        }
    } else {
        var_t *t;

        /* If operand is a reference, read the value and push to stack for the
         * incoming addition/subtraction. Otherwise, use the top element of
         * stack as the one of operands and the destination.
         */
        if (lvalue->is_reference) {
            rs1 = operand_stack[operand_stack_idx - 1];
            t = require_var(parent);
            gen_name_to(t->var_name);
            opstack_push(t);
            add_insn(parent, *bb, OP_read, t, rs1, NULL, lvalue->size, NULL);
        }
        if (prefix_op != OP_generic) {
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            /* For pointer arithmetic, increment by the size of pointed-to type
             */
            if (lvalue->ptr_level)
                vd->init_val = lvalue->type->size;
            else
                vd->init_val = 1;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs2 = opstack_pop();
            if (lvalue->is_reference)
                rs1 = opstack_pop();
            else
                rs1 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            add_insn(parent, *bb, prefix_op, vd, rs1, rs2, 0, NULL);

            if (lvalue->is_reference) {
                rs1 = vd;
                vd = opstack_pop();
                /* The column of arguments of the new insn of 'OP_write' is
                 * different from 'ph1_ir'
                 */
                add_insn(parent, *bb, OP_write, NULL, vd, rs1, lvalue->size,
                         NULL);
                /* Push the new value onto the operand stack */
                opstack_push(rs1);
            } else {
                rs1 = vd;
                vd = operand_stack[operand_stack_idx - 1];
                add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
            }
        } else if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
            side_effect[se_idx].opcode = OP_load_constant;
            vd = require_var(parent);
            gen_name_to(vd->var_name);

            /* Calculate increment size based on pointer type */
            int increment_size = 1;
            if (lvalue->ptr_level && !lvalue->is_reference) {
                increment_size = lvalue->type->size;
            } else if (!lvalue->is_reference && lvalue->type &&
                       lvalue->type->ptr_level > 0) {
                /* This is a typedef pointer */
                switch (lvalue->type->base_type) {
                case TYPE_char:
                    increment_size = TY_char->size;
                    break;
                case TYPE_short:
                    increment_size = TY_short->size;
                    break;
                case TYPE_int:
                    increment_size = TY_int->size;
                    break;
                case TYPE_void:
                    increment_size = 1;
                    break;
                default:
                    increment_size = lvalue->type->size;
                    break;
                }
            }
            vd->init_val = increment_size;

            side_effect[se_idx].rd = vd;
            side_effect[se_idx].rs1 = NULL;
            side_effect[se_idx].rs2 = NULL;
            se_idx++;

            side_effect[se_idx].opcode =
                lex_accept(T_increment) ? OP_add : OP_sub;
            side_effect[se_idx].rs2 = vd;
            if (lvalue->is_reference)
                side_effect[se_idx].rs1 = opstack_pop();
            else
                side_effect[se_idx].rs1 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            side_effect[se_idx].rd = vd;
            se_idx++;

            if (lvalue->is_reference) {
                side_effect[se_idx].opcode = OP_write;
                side_effect[se_idx].rs2 = vd;
                side_effect[se_idx].rs1 = opstack_pop();
                side_effect[se_idx].sz = lvalue->size;
                side_effect[se_idx].rd = NULL;
                opstack_push(t);
                se_idx++;
            } else {
                side_effect[se_idx].opcode = OP_assign;
                side_effect[se_idx].rs1 = vd;
                side_effect[se_idx].rd = operand_stack[operand_stack_idx - 1];
                side_effect[se_idx].rs2 = NULL;
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

void read_logical(opcode_t op, block_t *parent, basic_block_t **bb)
{
    var_t *vd;

    if (op != OP_log_and && op != OP_log_or)
        error("encounter an invalid logical opcode in read_logical()");

    /* Test the operand before the logical-and/or operator */
    vd = opstack_pop();
    add_insn(parent, *bb, OP_branch, NULL, vd, NULL, 0, NULL);

    /* Create a proper branch label for the operand of the logical-and/or
     * operation.
     */
    basic_block_t *new_bb = bb_create(parent);
    bb_connect(*bb, new_bb, op == OP_log_and ? THEN : ELSE);

    bb[0] = new_bb;
}

void finalize_logical(opcode_t op,
                      block_t *parent,
                      basic_block_t **bb,
                      basic_block_t *shared_bb)
{
    basic_block_t *then, *then_next, *else_if, *else_bb;
    basic_block_t *end = bb_create(parent);
    var_t *vd, *log_op_res;

    if (op == OP_log_and) {
        /* For example: a && b
         *
         * If handling the expression, the basic blocks will
         * connect to each other as the following illustration:
         *
         *  bb1                 bb2                bb3
         * +-----------+       +-----------+       +---------+
         * | teq a, #0 | True  | teq b, #0 | True  | ldr 1   |
         * | bne bb2   | ----> | bne bb3   | ----> | b   bb5 |
         * | b   bb4   |       | b   bb4   |       +---------+
         * +-----------+       +-----------+           |
         *      |                   |                  |
         *      | False             | False            |
         *      |                   |                  |
         *      |              +---------+         +--------+
         *      -------------> | ldr 0   | ------> |        |
         *                     | b   bb5 |         |        |
         *                     +---------+         +--------+
         *                      bb4                 bb5
         *
         * In this case, finalize_logical() should add some
         * instructions to bb2 ~ bb5 and properly connect them
         * to each other.
         *
         * Notice that
         * - bb1 has been handled by read_logical().
         * - bb2 is equivalent to '*bb'.
         * - bb3 needs to be created.
         * - bb4 is 'shared_bb'.
         * - bb5 needs to be created.
         *
         * Thus, here uses 'then', 'then_next', 'else_bb' and
         * 'end' to respectively point to bb2 ~ bb5. Subsequently,
         * perform the mentioned operations for finalizing.
         * */
        then = *bb;
        then_next = bb_create(parent);
        else_bb = shared_bb;
        bb_connect(then, then_next, THEN);
        bb_connect(then, else_bb, ELSE);
        bb_connect(then_next, end, NEXT);
    } else if (op == OP_log_or) {
        /* For example: a || b
         *
         * Similar to handling logical-and operations, it should
         * add some instructions to the basic blocks and connect
         * them to each other for logical-or operations as in
         * the figure:
         *
         *  bb1                 bb2                bb3
         * +-----------+       +-----------+       +---------+
         * | teq a, #0 | False | teq b, #0 | False | ldr 0   |
         * | bne bb4   | ----> | bne bb4   | ----> | b   bb5 |
         * | b   bb2   |       | b   bb3   |       +---------+
         * +-----------+       +-----------+           |
         *      |                   |                  |
         *      | True              | True             |
         *      |                   |                  |
         *      |              +---------+         +--------+
         *      -------------> | ldr 1   | ------> |        |
         *                     | b   bb5 |         |        |
         *                     +---------+         +--------+
         *                      bb4                 bb5
         *
         * Similarly, here uses 'else_if', 'else_bb', 'then' and
         * 'end' to respectively point to bb2 ~ bb5, and then
         * finishes the finalization.
         * */
        then = shared_bb;
        else_if = *bb;
        else_bb = bb_create(parent);
        bb_connect(else_if, then, THEN);
        bb_connect(else_if, else_bb, ELSE);
        bb_connect(then, end, NEXT);
    } else
        error("encounter an invalid logical opcode in finalize_logical()");
    bb_connect(else_bb, end, NEXT);

    /* Create the branch instruction for final logical-and/or operand */
    vd = opstack_pop();
    add_insn(parent, op == OP_log_and ? then : else_if, OP_branch, NULL, vd,
             NULL, 0, NULL);

    /*
     * If handling logical-and operation, here creates a true branch for the
     * logical-and operation and assigns a true value.
     *
     * Otherwise, create a false branch and assign a false value for logical-or
     * operation.
     * */
    vd = require_var(parent);
    gen_name_to(vd->var_name);
    vd->init_val = op == OP_log_and;
    add_insn(parent, op == OP_log_and ? then_next : else_bb, OP_load_constant,
             vd, NULL, NULL, 0, NULL);

    log_op_res = require_var(parent);
    gen_name_to(log_op_res->var_name);
    add_insn(parent, op == OP_log_and ? then_next : else_bb, OP_assign,
             log_op_res, vd, NULL, 0, NULL);

    /* After assigning a value, go to the final basic block, this is done by BB
     * fallthrough.
     */

    /* Create the shared branch and assign the other value for the other
     * condition of a logical-and/or operation.
     *
     * If handing a logical-and operation, assign a false value. else, assign
     * a true value for a logical-or operation.
     */
    vd = require_var(parent);
    gen_name_to(vd->var_name);
    vd->init_val = op != OP_log_and;
    add_insn(parent, op == OP_log_and ? else_bb : then, OP_load_constant, vd,
             NULL, NULL, 0, NULL);

    add_insn(parent, op == OP_log_and ? else_bb : then, OP_assign, log_op_res,
             vd, NULL, 0, NULL);

    log_op_res->is_logical_ret = true;
    opstack_push(log_op_res);

    bb[0] = end;
}

void read_ternary_operation(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;

    if (!lex_accept(T_question))
        return;

    /* ternary-operator */
    vd = opstack_pop();
    add_insn(parent, *bb, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    basic_block_t *end_ternary = bb_create(parent);
    bb_connect(then_, end_ternary, NEXT);
    bb_connect(else_, end_ternary, NEXT);

    /* true branch */
    read_expr(parent, &then_);
    bb_connect(*bb, then_, THEN);

    if (!lex_accept(T_colon)) {
        /* ternary operator in standard C needs three operands */
        /* Note: Dangling basic block cleanup handled by arena allocator */
        abort();
    }

    rs1 = opstack_pop();
    vd = require_var(parent);
    gen_name_to(vd->var_name);
    add_insn(parent, then_, OP_assign, vd, rs1, NULL, 0, NULL);

    /* false branch */
    read_expr(parent, &else_);
    bb_connect(*bb, else_, ELSE);

    rs1 = opstack_pop();
    add_insn(parent, else_, OP_assign, vd, rs1, NULL, 0, NULL);

    vd->is_ternary_ret = true;
    opstack_push(vd);
    bb[0] = end_ternary;
}

bool read_body_assignment(char *token,
                          block_t *parent,
                          opcode_t prefix_op,
                          basic_block_t **bb)
{
    var_t *var = find_local_var(token, parent), *vd, *rs1, *rs2, *t;
    if (!var)
        var = find_global_var(token);

    if (var) {
        int one = 0;
        opcode_t op = OP_generic;
        lvalue_t lvalue;
        int size = 0;

        /* has memory address that we want to set */
        read_lvalue(&lvalue, var, parent, bb, false, OP_generic);
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
        } else if (lex_accept(T_asteriskeq)) {
            op = OP_mul;
        } else if (lex_accept(T_divideeq)) {
            op = OP_div;
        } else if (lex_accept(T_modeq)) {
            op = OP_mod;
        } else if (lex_accept(T_lshifteq)) {
            op = OP_lshift;
        } else if (lex_accept(T_rshifteq)) {
            op = OP_rshift;
        } else if (lex_accept(T_xoreq)) {
            op = OP_bit_xor;
        } else if (lex_accept(T_oreq)) {
            op = OP_bit_or;
        } else if (lex_accept(T_andeq)) {
            op = OP_bit_and;
        } else if (lex_peek(T_open_bracket, NULL)) {
            /* dereference lvalue into function address */
            rs1 = opstack_pop();
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, vd, rs1, NULL, PTR_SIZE, NULL);

            read_indirect_call(parent, bb);
            return true;
        } else if (prefix_op == OP_generic) {
            lex_expect(T_assign);
        } else {
            op = prefix_op;
            one = 1;
        }

        if (op != OP_generic) {
            int increment_size = 1;

            /* if we have a pointer, shift it by element size */
            /* But not if we are operating on a dereferenced value (array
             * indexing)
             */
            if (lvalue.ptr_level && !lvalue.is_reference)
                increment_size = lvalue.type->size;
            /* Also check for typedef pointers which have is_ptr == 0 */
            else if (!lvalue.is_reference && lvalue.type &&
                     lvalue.type->ptr_level > 0) {
                /* This is a typedef pointer, get the base type size */
                switch (lvalue.type->base_type) {
                case TYPE_char:
                    increment_size = TY_char->size;
                    break;
                case TYPE_short:
                    increment_size = TY_short->size;
                    break;
                case TYPE_int:
                    increment_size = TY_int->size;
                    break;
                case TYPE_void:
                    /* void pointers treated as byte pointers */
                    increment_size = 1;
                    break;
                default:
                    /* For struct pointers and other types */
                    increment_size = lvalue.type->size;
                    break;
                }
            }

            /* If operand is a reference, read the value and push to stack for
             * the incoming addition/subtraction. Otherwise, use the top element
             * of stack as the one of operands and the destination.
             */
            if (one == 1) {
                if (lvalue.is_reference) {
                    t = opstack_pop();
                    vd = require_var(parent);
                    gen_name_to(vd->var_name);
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_read, vd, t, NULL, lvalue.size,
                             NULL);
                } else
                    t = operand_stack[operand_stack_idx - 1];

                vd = require_var(parent);
                gen_name_to(vd->var_name);
                vd->init_val = increment_size;
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = vd;
                rs1 = opstack_pop();
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                add_insn(parent, *bb, op, vd, rs1, rs2, 0, NULL);

                if (lvalue.is_reference) {
                    add_insn(parent, *bb, OP_write, NULL, t, vd, size, NULL);
                } else {
                    vd = resize_var(parent, bb, vd, t);
                    add_insn(parent, *bb, OP_assign, t, vd, NULL, 0, NULL);
                }
            } else {
                if (lvalue.is_reference) {
                    t = opstack_pop();
                    vd = require_var(parent);
                    gen_name_to(vd->var_name);
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_read, vd, t, NULL, lvalue.size,
                             NULL);
                } else
                    t = operand_stack[operand_stack_idx - 1];

                read_expr(parent, bb);

                vd = require_var(parent);
                vd->init_val = increment_size;
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, vd, rs1, rs2, 0, NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                gen_name_to(vd->var_name);
                add_insn(parent, *bb, op, vd, rs1, rs2, 0, NULL);

                if (lvalue.is_reference) {
                    add_insn(parent, *bb, OP_write, NULL, t, vd, lvalue.size,
                             NULL);
                } else {
                    vd = resize_var(parent, bb, vd, t);
                    add_insn(parent, *bb, OP_assign, t, vd, NULL, 0, NULL);
                }
            }
        } else {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);

            if (lvalue.is_func) {
                rs2 = opstack_pop();
                rs1 = opstack_pop();
                add_insn(parent, *bb, OP_write, NULL, rs1, rs2, PTR_SIZE, NULL);
            } else if (lvalue.is_reference) {
                rs2 = opstack_pop();
                rs1 = opstack_pop();
                add_insn(parent, *bb, OP_write, NULL, rs1, rs2, size, NULL);
            } else {
                rs1 = opstack_pop();
                vd = opstack_pop();
                rs1 = resize_var(parent, bb, rs1, vd);
                add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
            }
        }
        return true;
    }
    return false;
}

int read_primary_constant(void)
{
    /* return signed constant */
    int isneg = 0, res;
    char buffer[10];
    if (lex_accept(T_minus))
        isneg = 1;
    if (lex_accept(T_open_bracket)) {
        res = read_primary_constant();
        lex_expect(T_close_bracket);
    } else if (lex_peek(T_numeric, buffer)) {
        res = read_numeric_constant(buffer);
        lex_expect(T_numeric);
    } else if (lex_peek(T_char, buffer)) {
        res = buffer[0];
        lex_expect(T_char);
    } else
        error("Invalid value after assignment");
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
        /* Use bitwise AND for modulo optimization when divisor is power of 2 */
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
    case OP_log_and:
        res = op1 && op2;
        break;
    case OP_log_or:
        res = op1 || op2;
        break;
    case OP_eq:
        res = op1 == op2;
        break;
    case OP_neq:
        res = op1 != op2;
        break;
    case OP_lt:
        res = op1 < op2;
        break;
    case OP_gt:
        res = op1 > op2;
        break;
    case OP_leq:
        res = op1 <= op2;
        break;
    case OP_geq:
        res = op1 >= op2;
        break;
    default:
        error("The requested operation is not supported.");
    }
    return res;
}

bool read_global_assignment(char *token);
void eval_ternary_imm(int cond, char *token)
{
    if (cond == 0) {
        while (next_token != T_colon) {
            next_token = lex_token();
        }
        lex_accept(T_colon);
        read_global_assignment(token);
    } else {
        read_global_assignment(token);
        lex_expect(T_colon);
        while (!lex_peek(T_semicolon, NULL)) {
            next_token = lex_token();
        }
    }
}

bool read_global_assignment(char *token)
{
    var_t *vd, *rs1, *var;
    block_t *parent = GLOBAL_BLOCK;
    basic_block_t *bb = GLOBAL_FUNC->bbs;

    /* global initialization must be constant */
    var = find_global_var(token);
    if (var) {
        if (lex_peek(T_string, NULL)) {
            /* String literal global initialization:
             * String literals are now stored in .rodata section.
             * TODO: Implement compile-time address resolution for global
             * pointer initialization with rodata addresses
             * (e.g., char *p = "str";)
             */
            read_literal_param(parent, bb);
            rs1 = opstack_pop();
            vd = var;
            add_insn(parent, bb, OP_assign, vd, rs1, NULL, 0, NULL);
            return true;
        }

        opcode_t op_stack[10];
        opcode_t op, next_op;
        int val_stack[10];
        int op_stack_index = 0, val_stack_index = 0;
        int operand1, operand2;
        operand1 = read_primary_constant();
        op = get_operator();
        /* only one value after assignment */
        if (op == OP_generic) {
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            vd->init_val = operand1;
            add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs1 = vd;
            vd = opstack_pop();
            add_insn(parent, bb, OP_assign, vd, rs1, NULL, 0, NULL);
            return true;
        }
        if (op == OP_ternary) {
            lex_expect(T_question);
            eval_ternary_imm(operand1, token);
            return true;
        }
        operand2 = read_primary_constant();
        next_op = get_operator();
        if (next_op == OP_generic) {
            /* only two operands, apply and return */
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            vd->init_val = eval_expression_imm(op, operand1, operand2);
            add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs1 = vd;
            vd = opstack_pop();
            add_insn(parent, bb, OP_assign, vd, rs1, NULL, 0, NULL);
            return true;
        }
        if (op == OP_ternary) {
            lex_expect(T_question);
            int cond = eval_expression_imm(op, operand1, operand2);
            eval_ternary_imm(cond, token);
            return true;
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
            val_stack[val_stack_index++] = read_primary_constant();
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
                    vd = require_var(parent);
                    gen_name_to(vd->var_name);
                    vd->init_val = val_stack[0];
                    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0,
                             NULL);

                    rs1 = vd;
                    vd = opstack_pop();
                    add_insn(parent, bb, OP_assign, vd, rs1, NULL, 0, NULL);
                }
                return true;
            }

            /* pop op stack */
            op_stack_index--;
        }
        if (op == OP_ternary) {
            lex_expect(T_question);
            eval_ternary_imm(val_stack[0], token);
        } else {
            vd = require_var(parent);
            gen_name_to(vd->var_name);
            vd->init_val = val_stack[0];
            add_insn(parent, GLOBAL_FUNC->bbs, OP_load_constant, vd, NULL, NULL,
                     0, NULL);

            rs1 = vd;
            vd = opstack_pop();
            add_insn(parent, GLOBAL_FUNC->bbs, OP_assign, vd, rs1, NULL, 0,
                     NULL);
        }
        return true;
    }
    return false;
}

void perform_side_effect(block_t *parent, basic_block_t *bb)
{
    for (int i = 0; i < se_idx; i++) {
        insn_t *insn = &side_effect[i];
        add_insn(parent, bb, insn->opcode, insn->rd, insn->rs1, insn->rs2,
                 insn->sz, insn->str);
    }
    se_idx = 0;
}

basic_block_t *read_code_block(func_t *func,
                               macro_t *macro,
                               block_t *parent,
                               basic_block_t *bb);

basic_block_t *read_body_statement(block_t *parent, basic_block_t *bb)
{
    char token[MAX_ID_LEN];
    macro_t *mac;
    func_t *func;
    type_t *type;
    var_t *vd, *rs1, *rs2, *var;
    opcode_t prefix_op = OP_generic;
    bool is_const = false;

    if (!bb)
        printf("Warning: unreachable code detected\n");

    /* statement can be:
     *   function call, variable declaration, assignment operation,
     *   keyword, block
     */

    if (lex_peek(T_open_curly, NULL))
        return read_code_block(parent->func, parent->macro, parent, bb);

    if (lex_accept(T_return)) {
        return handle_return_statement(parent, bb);
    }

    if (lex_accept(T_if)) {
        return handle_if_statement(parent, bb);
    }

    if (lex_accept(T_while)) {
        return handle_while_statement(parent, bb);
    }

    if (lex_accept(T_switch)) {
        bool is_default = false;

        basic_block_t *n = bb_create(parent);
        bb_connect(bb, n, NEXT);
        bb = n;

        lex_expect(T_open_bracket);
        read_expr(parent, &bb);
        lex_expect(T_close_bracket);

        /* create exit jump for breaks */
        basic_block_t *switch_end = bb_create(parent);
        break_bb[break_exit_idx++] = switch_end;
        basic_block_t *true_body_ = bb_create(parent);

        lex_expect(T_open_curly);
        while (lex_peek(T_default, NULL) || lex_peek(T_case, NULL)) {
            if (lex_accept(T_default))
                is_default = true;
            else {
                int case_val;

                lex_accept(T_case);
                if (lex_peek(T_numeric, NULL)) {
                    case_val = read_numeric_constant(token_str);
                    lex_expect(T_numeric); /* already read it */
                } else if (lex_peek(T_char, token)) {
                    case_val = token[0];
                    lex_expect(T_char);
                } else {
                    constant_t *cd = find_constant(token_str);
                    case_val = cd->value;
                    lex_expect(T_identifier); /* already read it */
                }

                vd = require_var(parent);
                gen_name_to(vd->var_name);
                vd->init_val = case_val;
                opstack_push(vd);
                add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

                vd = require_var(parent);
                gen_name_to(vd->var_name);
                rs1 = opstack_pop();
                rs2 = operand_stack[operand_stack_idx - 1];
                add_insn(parent, bb, OP_eq, vd, rs1, rs2, 0, NULL);

                add_insn(parent, bb, OP_branch, NULL, vd, NULL, 0, NULL);
            }
            lex_expect(T_colon);

            if (is_default)
                /* there's no condition if it is a default label */
                bb_connect(bb, true_body_, NEXT);
            else
                bb_connect(bb, true_body_, THEN);

            int control = 0;

            while (!lex_peek(T_case, NULL) && !lex_peek(T_close_curly, NULL) &&
                   !lex_peek(T_default, NULL)) {
                true_body_ = read_body_statement(parent, true_body_);
                control = 1;
            }

            if (control && true_body_) {
                /* Create a new body block for next case, and connect the last
                 * body block which lacks 'break' to it to make that one ignore
                 * the upcoming cases.
                 */
                n = bb_create(parent);
                bb_connect(true_body_, n, NEXT);
                true_body_ = n;
            }

            if (!lex_peek(T_close_curly, NULL)) {
                if (is_default)
                    error("Label default should be the last one");

                /* create a new conditional block for next case */
                n = bb_create(parent);
                bb_connect(bb, n, ELSE);
                bb = n;

                /* create a new body block for next case if the last body block
                 * exits 'switch'.
                 */
                if (!true_body_)
                    true_body_ = bb_create(parent);
            } else if (!is_default) {
                /* handle missing default label */
                bb_connect(bb, switch_end, ELSE);
            }
        }

        /* remove the expression in switch() */
        opstack_pop();
        lex_expect(T_close_curly);

        if (true_body_)
            /* if the last label has no explicit break, connect it to the end */
            bb_connect(true_body_, switch_end, NEXT);

        break_exit_idx--;

        int dangling = 1;
        for (int i = 0; i < MAX_BB_PRED; i++)
            if (switch_end->prev[i].bb)
                dangling = 0;

        if (dangling)
            return NULL;

        return switch_end;
    }

    if (lex_accept(T_break)) {
        bb_connect(bb, break_bb[break_exit_idx - 1], NEXT);
        lex_expect(T_semicolon);
        return NULL;
    }

    if (lex_accept(T_continue)) {
        bb_connect(bb, continue_bb[continue_pos_idx - 1], NEXT);
        lex_expect(T_semicolon);
        return NULL;
    }

    if (lex_accept(T_for)) {
        lex_expect(T_open_bracket);

        /* synthesize for loop block */
        block_t *blk = add_block(parent, parent->func, parent->macro);

        /* setup - execute once */
        basic_block_t *setup = bb_create(blk);
        bb_connect(bb, setup, NEXT);

        if (!lex_accept(T_semicolon)) {
            if (!lex_peek(T_identifier, token))
                error("Unexpected token");

            int find_type_flag = lex_accept(T_struct) ? 2 : 1;
            if (find_type_flag == 1 && lex_accept(T_union)) {
                find_type_flag = 2;
            }
            type = find_type(token, find_type_flag);
            if (type) {
                var = require_typed_var(blk, type);
                read_full_var_decl(var, false, false);
                add_insn(blk, setup, OP_allocat, var, NULL, NULL, 0, NULL);
                add_symbol(setup, var);
                if (lex_accept(T_assign)) {
                    read_expr(blk, &setup);
                    read_ternary_operation(blk, &setup);

                    rs1 = resize_var(parent, &bb, opstack_pop(), var);
                    add_insn(blk, setup, OP_assign, var, rs1, NULL, 0, NULL);
                }
                while (lex_accept(T_comma)) {
                    var_t *nv;

                    /* add sequence point at T_comma */
                    perform_side_effect(blk, setup);

                    /* multiple (partial) declarations */
                    nv = require_typed_var(blk, type);
                    read_partial_var_decl(nv, var); /* partial */
                    add_insn(blk, setup, OP_allocat, nv, NULL, NULL, 0, NULL);
                    add_symbol(setup, nv);
                    if (lex_accept(T_assign)) {
                        read_expr(blk, &setup);

                        rs1 = resize_var(parent, &bb, opstack_pop(), nv);
                        add_insn(blk, setup, OP_assign, nv, rs1, NULL, 0, NULL);
                    }
                }
            } else {
                read_body_assignment(token, blk, OP_generic, &setup);
            }

            lex_expect(T_semicolon);
        }

        basic_block_t *cond_ = bb_create(blk);
        basic_block_t *for_end = bb_create(parent);
        basic_block_t *cond_start = cond_;
        break_bb[break_exit_idx++] = for_end;
        bb_connect(setup, cond_, NEXT);

        /* condition - check before the loop */
        if (!lex_accept(T_semicolon)) {
            read_expr(blk, &cond_);
            lex_expect(T_semicolon);
        } else {
            /* always true */
            vd = require_var(blk);
            vd->init_val = 1;
            gen_name_to(vd->var_name);
            opstack_push(vd);
            add_insn(blk, cond_, OP_load_constant, vd, NULL, NULL, 0, NULL);
        }
        bb_connect(cond_, for_end, ELSE);

        vd = opstack_pop();
        add_insn(blk, cond_, OP_branch, NULL, vd, NULL, 0, NULL);

        basic_block_t *inc_ = bb_create(blk);
        continue_bb[continue_pos_idx++] = inc_;

        /* increment after each loop */
        if (!lex_accept(T_close_bracket)) {
            if (lex_accept(T_increment))
                prefix_op = OP_add;
            else if (lex_accept(T_decrement))
                prefix_op = OP_sub;
            lex_peek(T_identifier, token);
            read_body_assignment(token, blk, prefix_op, &inc_);
            lex_expect(T_close_bracket);
        }

        /* loop body */
        basic_block_t *body_ = bb_create(blk);
        bb_connect(cond_, body_, THEN);
        body_ = read_body_statement(blk, body_);

        if (body_) {
            bb_connect(body_, inc_, NEXT);
            bb_connect(inc_, cond_start, NEXT);
        } else if (inc_->insn_list.head) {
            bb_connect(inc_, cond_start, NEXT);
        } else {
            /* Empty increment block - cleanup handled by arena allocator */
        }

        /* jump to increment */
        continue_pos_idx--;
        break_exit_idx--;
        return for_end;
    }

    if (lex_accept(T_do)) {
        basic_block_t *n = bb_create(parent);
        bb_connect(bb, n, NEXT);
        bb = n;

        basic_block_t *cond_ = bb_create(parent);
        basic_block_t *do_while_end = bb_create(parent);

        continue_bb[continue_pos_idx++] = cond_;
        break_bb[break_exit_idx++] = do_while_end;

        basic_block_t *do_body = read_body_statement(parent, bb);
        if (do_body)
            bb_connect(do_body, cond_, NEXT);

        lex_expect(T_while);
        lex_expect(T_open_bracket);
        read_expr(parent, &cond_);
        lex_expect(T_close_bracket);

        vd = opstack_pop();
        add_insn(parent, cond_, OP_branch, NULL, vd, NULL, 0, NULL);

        lex_expect(T_semicolon);

        for (int i = 0; i < MAX_BB_PRED; i++) {
            if (cond_->prev[i].bb) {
                bb_connect(cond_, bb, THEN);
                bb_connect(cond_, do_while_end, ELSE);
                break;
            }
            /* if breaking out of loop, skip condition block */
        }

        continue_pos_idx--;
        break_exit_idx--;
        return do_while_end;
    }

    if (lex_accept(T_goto))
        return handle_goto_statement(parent, bb);

    /* empty statement */
    if (lex_accept(T_semicolon))
        return bb;

    /* struct/union variable declaration */
    if (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL)) {
        int find_type_flag = lex_accept(T_struct) ? 2 : 1;
        if (find_type_flag == 1 && lex_accept(T_union)) {
            find_type_flag = 2;
        }
        lex_ident(T_identifier, token);
        type = find_type(token, find_type_flag);
        if (type) {
            var = require_typed_var(parent, type);
            var->is_const_qualified = is_const;
            read_partial_var_decl(var, NULL);
            add_insn(parent, bb, OP_allocat, var, NULL, NULL, 0, NULL);
            add_symbol(bb, var);
            if (lex_accept(T_assign)) {
                if (lex_peek(T_open_curly, NULL) &&
                    (var->array_size > 0 || var->ptr_level > 0)) {
                    parse_array_init(var, parent, &bb,
                                     1); /* Always emit code */
                } else if (lex_peek(T_open_curly, NULL) &&
                           (var->type->base_type == TYPE_struct ||
                            var->type->base_type == TYPE_typedef)) {
                    /* C90-compliant struct compound literal support */
                    type_t *struct_type = var->type;

                    /* Handle typedef by getting actual struct type */
                    if (struct_type->base_type == TYPE_typedef &&
                        struct_type->base_struct)
                        struct_type = struct_type->base_struct;

                    lex_expect(T_open_curly);
                    int field_idx = 0;

                    if (!lex_peek(T_close_curly, NULL)) {
                        for (;;) {
                            /* Parse field value expression */
                            read_expr(parent, &bb);
                            read_ternary_operation(parent, &bb);
                            var_t *val = opstack_pop();

                            /* Initialize field if within bounds */
                            if (field_idx < struct_type->num_fields) {
                                var_t *field = &struct_type->fields[field_idx];

                                /* Create target variable for field */
                                var_t target = {0};
                                target.type = field->type;
                                target.ptr_level = field->ptr_level;
                                var_t *field_val =
                                    resize_var(parent, &bb, val, &target);

                                /* Compute field address: &struct + field_offset
                                 */
                                var_t *struct_addr = require_var(parent);
                                gen_name_to(struct_addr->var_name);
                                add_insn(parent, bb, OP_address_of, struct_addr,
                                         var, NULL, 0, NULL);

                                var_t *field_addr = struct_addr;
                                if (field->offset > 0) {
                                    var_t *offset = require_var(parent);
                                    gen_name_to(offset->var_name);
                                    offset->init_val = field->offset;
                                    add_insn(parent, bb, OP_load_constant,
                                             offset, NULL, NULL, 0, NULL);

                                    var_t *addr = require_var(parent);
                                    gen_name_to(addr->var_name);
                                    add_insn(parent, bb, OP_add, addr,
                                             struct_addr, offset, 0, NULL);
                                    field_addr = addr;
                                }

                                /* Write field value */
                                int field_size = size_var(field);
                                add_insn(parent, bb, OP_write, NULL, field_addr,
                                         field_val, field_size, NULL);
                            }

                            field_idx++;
                            if (!lex_accept(T_comma))
                                break;
                            if (lex_peek(T_close_curly, NULL))
                                break;
                        }
                    }
                    lex_expect(T_close_curly);
                } else {
                    read_expr(parent, &bb);
                    read_ternary_operation(parent, &bb);

                    var_t *expr_result = opstack_pop();

                    /* Handle array compound literal to scalar assignment.
                     * When assigning array compound literals to scalar
                     * variables, use the first element value rather than array
                     * address.
                     */
                    if (expr_result && expr_result->array_size > 0 &&
                        !var->ptr_level && var->array_size == 0 && var->type &&
                        (var->type->base_type == TYPE_int ||
                         var->type->base_type == TYPE_short) &&
                        expr_result->var_name[0] == '.') {
                        var_t *first_elem = require_var(parent);
                        first_elem->type = var->type;
                        gen_name_to(first_elem->var_name);

                        /* Extract first element from compound literal array */
                        add_insn(parent, bb, OP_read, first_elem, expr_result,
                                 NULL, var->type->size, NULL);
                        expr_result = first_elem;
                    }

                    rs1 = resize_var(parent, &bb, expr_result, var);
                    add_insn(parent, bb, OP_assign, var, rs1, NULL, 0, NULL);
                }
            }
            while (lex_accept(T_comma)) {
                var_t *nv;

                /* add sequence point at T_comma */
                perform_side_effect(parent, bb);

                /* multiple (partial) declarations */
                nv = require_typed_var(parent, type);
                read_inner_var_decl(nv, false, false);
                add_insn(parent, bb, OP_allocat, nv, NULL, NULL, 0, NULL);
                add_symbol(bb, nv);
                if (lex_accept(T_assign)) {
                    if (lex_peek(T_open_curly, NULL) &&
                        (nv->array_size > 0 || nv->ptr_level > 0)) {
                        parse_array_init(nv, parent, &bb, true);
                    } else if (lex_peek(T_open_curly, NULL) &&
                               (nv->type->base_type == TYPE_struct ||
                                nv->type->base_type == TYPE_typedef)) {
                        /* C90-compliant struct compound literal support */
                        type_t *struct_type = nv->type;

                        /* Handle typedef by getting actual struct type */
                        if (struct_type->base_type == TYPE_typedef &&
                            struct_type->base_struct)
                            struct_type = struct_type->base_struct;

                        lex_expect(T_open_curly);
                        int field_idx = 0;

                        if (!lex_peek(T_close_curly, NULL)) {
                            for (;;) {
                                /* Parse field value expression */
                                read_expr(parent, &bb);
                                read_ternary_operation(parent, &bb);
                                var_t *val = opstack_pop();

                                /* Initialize field if within bounds */
                                if (field_idx < struct_type->num_fields) {
                                    var_t *field =
                                        &struct_type->fields[field_idx];

                                    /* Create target variable for field */
                                    var_t target = {0};
                                    target.type = field->type;
                                    target.ptr_level = field->ptr_level;
                                    var_t *field_val =
                                        resize_var(parent, &bb, val, &target);

                                    /* Compute field address: &struct +
                                     * field_offset */
                                    var_t *struct_addr = require_var(parent);
                                    gen_name_to(struct_addr->var_name);
                                    add_insn(parent, bb, OP_address_of,
                                             struct_addr, nv, NULL, 0, NULL);

                                    var_t *field_addr = struct_addr;
                                    if (field->offset > 0) {
                                        var_t *offset = require_var(parent);
                                        gen_name_to(offset->var_name);
                                        offset->init_val = field->offset;
                                        add_insn(parent, bb, OP_load_constant,
                                                 offset, NULL, NULL, 0, NULL);

                                        var_t *addr = require_var(parent);
                                        gen_name_to(addr->var_name);
                                        add_insn(parent, bb, OP_add, addr,
                                                 struct_addr, offset, 0, NULL);
                                        field_addr = addr;
                                    }

                                    /* Write field value */
                                    int field_size = size_var(field);
                                    add_insn(parent, bb, OP_write, NULL,
                                             field_addr, field_val, field_size,
                                             NULL);
                                }

                                field_idx++;
                                if (!lex_accept(T_comma))
                                    break;
                                if (lex_peek(T_close_curly, NULL))
                                    break;
                            }
                        }
                        lex_expect(T_close_curly);
                    } else {
                        read_expr(parent, &bb);
                        read_ternary_operation(parent, &bb);

                        rs1 = resize_var(parent, &bb, opstack_pop(), nv);
                        add_insn(parent, bb, OP_assign, nv, rs1, NULL, 0, NULL);
                    }
                }
            }
            lex_expect(T_semicolon);
            return bb;
        }
        error("Unknown struct/union type");
    }

    /* Handle const qualifier for local variable declarations */
    if (lex_accept(T_const)) {
        is_const = true;
        /* After const, we expect a type */
        if (!lex_peek(T_identifier, token))
            error("Expected type after const");
    }

    /* statement with prefix */
    if (!is_const && lex_accept(T_increment))
        prefix_op = OP_add;
    else if (!is_const && lex_accept(T_decrement))
        prefix_op = OP_sub;
    /* must be an identifier or asterisk (for pointer dereference) */
    bool has_asterisk = lex_peek(T_asterisk, NULL);
    if (!is_const && !lex_peek(T_identifier, token) && !has_asterisk)
        error("Unexpected token");

    /* handle macro parameter substitution for statements */
    int macro_param_idx = find_macro_param_src_idx(token, parent);
    if (macro_param_idx && parent->macro) {
        /* save current state */
        int saved_size = SOURCE->size;
        char saved_char = next_char;
        int saved_token = next_token;

        /* jump to parameter value */
        SOURCE->size = macro_param_idx;
        next_char = SOURCE->elements[SOURCE->size];
        next_token = lex_token();

        /* extract the parameter value as identifier token */
        if (lex_peek(T_identifier, token)) {
            lex_expect(T_identifier);
        } else {
            /* parameter is not a simple identifier, restore state and continue
             */
            SOURCE->size = saved_size;
            next_char = saved_char;
            next_token = saved_token;
        }

        /* restore source position */
        SOURCE->size = saved_size;
        next_char = saved_char;
        next_token = saved_token;
    }

    /* is it a variable declaration? */
    /* Special handling when statement starts with asterisk */
    if (has_asterisk) {
        /* For "*identifier", check if identifier is a type.
         * If not, it's a dereference, not a declaration. */
        int saved_size = SOURCE->size;
        char saved_char = next_char;
        int saved_token = next_token;

        /* Skip the asterisk to peek at the identifier */
        lex_accept(T_asterisk);
        char next_ident[MAX_TOKEN_LEN];
        bool could_be_type = false;

        if (lex_peek(T_identifier, next_ident)) {
            /* Check if it's a type name */
            type = find_type(next_ident, 0);
            if (type)
                could_be_type = true;
        }

        /* Restore position */
        SOURCE->size = saved_size;
        next_char = saved_char;
        next_token = saved_token;

        /* If it's not a type, skip the declaration block */
        if (!could_be_type)
            type = NULL;
    } else {
        /* Normal type checking without asterisk */
        int find_type_flag = lex_accept(T_struct) ? 2 : 1;
        if (find_type_flag == 1 && lex_accept(T_union))
            find_type_flag = 2;
        type = find_type(token, find_type_flag);
    }

    if (type) {
        var = require_typed_var(parent, type);
        var->is_const_qualified = is_const;
        read_full_var_decl(var, false, false);
        add_insn(parent, bb, OP_allocat, var, NULL, NULL, 0, NULL);
        add_symbol(bb, var);
        if (lex_accept(T_assign)) {
            if (lex_peek(T_open_curly, NULL) &&
                (var->array_size > 0 || var->ptr_level > 0)) {
                /* Emit code for locals in functions */
                parse_array_init(var, parent, &bb, 1);
            } else if (lex_peek(T_open_curly, NULL) &&
                       (var->type->base_type == TYPE_struct ||
                        var->type->base_type == TYPE_typedef)) {
                /* C90-compliant struct compound literal support */
                type_t *struct_type = var->type;

                /* Handle typedef by getting actual struct type */
                if (struct_type->base_type == TYPE_typedef &&
                    struct_type->base_struct)
                    struct_type = struct_type->base_struct;

                lex_expect(T_open_curly);
                int field_idx = 0;

                if (!lex_peek(T_close_curly, NULL)) {
                    for (;;) {
                        /* Parse field value expression */
                        read_expr(parent, &bb);
                        read_ternary_operation(parent, &bb);
                        var_t *val = opstack_pop();

                        /* Initialize field if within bounds */
                        if (field_idx < struct_type->num_fields) {
                            var_t *field = &struct_type->fields[field_idx];

                            /* Create target variable for field */
                            var_t target = {0};
                            target.type = field->type;
                            target.ptr_level = field->ptr_level;
                            var_t *field_val =
                                resize_var(parent, &bb, val, &target);

                            /* Compute field address: &struct + field_offset */
                            var_t *struct_addr = require_var(parent);
                            gen_name_to(struct_addr->var_name);
                            add_insn(parent, bb, OP_address_of, struct_addr,
                                     var, NULL, 0, NULL);

                            var_t *field_addr = struct_addr;
                            if (field->offset > 0) {
                                var_t *offset = require_var(parent);
                                gen_name_to(offset->var_name);
                                offset->init_val = field->offset;
                                add_insn(parent, bb, OP_load_constant, offset,
                                         NULL, NULL, 0, NULL);

                                var_t *addr = require_var(parent);
                                gen_name_to(addr->var_name);
                                add_insn(parent, bb, OP_add, addr, struct_addr,
                                         offset, 0, NULL);
                                field_addr = addr;
                            }

                            /* Write field value */
                            int field_size = size_var(field);
                            add_insn(parent, bb, OP_write, NULL, field_addr,
                                     field_val, field_size, NULL);
                        }

                        field_idx++;
                        if (!lex_accept(T_comma))
                            break;
                        if (lex_peek(T_close_curly, NULL))
                            break;
                    }
                }
                lex_expect(T_close_curly);
            } else {
                read_expr(parent, &bb);
                read_ternary_operation(parent, &bb);

                var_t *expr_result = opstack_pop();

                /* Handle array compound literal to scalar assignment */
                if (expr_result && expr_result->array_size > 0 &&
                    !var->ptr_level && var->array_size == 0 && var->type &&
                    (var->type->base_type == TYPE_int ||
                     var->type->base_type == TYPE_short) &&
                    expr_result->var_name[0] == '.') {
                    /* Extract first element from compound literal array */
                    var_t *first_elem = require_var(parent);
                    first_elem->type = var->type;
                    gen_name_to(first_elem->var_name);

                    /* Read first element from array at offset 0
                     * expr_result is the array itself, so we can read
                     * directly from it
                     */
                    add_insn(parent, bb, OP_read, first_elem, expr_result, NULL,
                             var->type->size, NULL);
                    expr_result = first_elem;
                }

                rs1 = resize_var(parent, &bb, expr_result, var);
                add_insn(parent, bb, OP_assign, var, rs1, NULL, 0, NULL);
            }
        }
        while (lex_accept(T_comma)) {
            var_t *nv;

            /* add sequence point at T_comma */
            perform_side_effect(parent, bb);

            /* multiple (partial) declarations */
            nv = require_typed_var(parent, type);
            read_partial_var_decl(nv, var); /* partial */
            add_insn(parent, bb, OP_allocat, nv, NULL, NULL, 0, NULL);
            add_symbol(bb, nv);
            if (lex_accept(T_assign)) {
                if (lex_peek(T_open_curly, NULL) &&
                    (nv->array_size > 0 || nv->ptr_level > 0)) {
                    /* Emit code for locals */
                    parse_array_init(nv, parent, &bb, 1);
                } else if (lex_peek(T_open_curly, NULL) &&
                           (nv->type->base_type == TYPE_struct ||
                            nv->type->base_type == TYPE_typedef)) {
                    /* C90-compliant struct compound literal support */
                    type_t *struct_type = nv->type;

                    /* Handle typedef by getting actual struct type */
                    if (struct_type->base_type == TYPE_typedef &&
                        struct_type->base_struct)
                        struct_type = struct_type->base_struct;

                    lex_expect(T_open_curly);
                    int field_idx = 0;

                    if (!lex_peek(T_close_curly, NULL)) {
                        for (;;) {
                            /* Parse field value expression */
                            read_expr(parent, &bb);
                            read_ternary_operation(parent, &bb);
                            var_t *val = opstack_pop();

                            /* Initialize field if within bounds */
                            if (field_idx < struct_type->num_fields) {
                                var_t *field = &struct_type->fields[field_idx];

                                /* Create target variable for field */
                                var_t target = {0};
                                target.type = field->type;
                                target.ptr_level = field->ptr_level;
                                var_t *field_val =
                                    resize_var(parent, &bb, val, &target);

                                /* Compute field address: &struct + field_offset
                                 */
                                var_t *struct_addr = require_var(parent);
                                gen_name_to(struct_addr->var_name);
                                add_insn(parent, bb, OP_address_of, struct_addr,
                                         nv, NULL, 0, NULL);

                                var_t *field_addr = struct_addr;
                                if (field->offset > 0) {
                                    var_t *offset = require_var(parent);
                                    gen_name_to(offset->var_name);
                                    offset->init_val = field->offset;
                                    add_insn(parent, bb, OP_load_constant,
                                             offset, NULL, NULL, 0, NULL);

                                    var_t *addr = require_var(parent);
                                    gen_name_to(addr->var_name);
                                    add_insn(parent, bb, OP_add, addr,
                                             struct_addr, offset, 0, NULL);
                                    field_addr = addr;
                                }

                                /* Write field value */
                                int field_size = size_var(field);
                                add_insn(parent, bb, OP_write, NULL, field_addr,
                                         field_val, field_size, NULL);
                            }

                            field_idx++;
                            if (!lex_accept(T_comma))
                                break;
                            if (lex_peek(T_close_curly, NULL))
                                break;
                        }
                    }
                    lex_expect(T_close_curly);
                } else {
                    read_expr(parent, &bb);

                    rs1 = resize_var(parent, &bb, opstack_pop(), nv);
                    add_insn(parent, bb, OP_assign, nv, rs1, NULL, 0, NULL);
                }
            }
        }
        lex_expect(T_semicolon);
        return bb;
    }

    mac = find_macro(token);
    if (mac) {
        if (parent->macro)
            error("Nested macro is not yet supported");

        parent->macro = mac;
        mac->num_params = 0;
        lex_expect(T_identifier);

        /* 'size' has pointed at the first parameter */
        while (!lex_peek(T_close_bracket, NULL)) {
            mac->params[mac->num_params++] = SOURCE->size;
            do {
                next_token = lex_token();
            } while (next_token != T_comma && next_token != T_close_bracket);
        }
        /* move 'size' to the macro body */
        macro_return_idx = SOURCE->size;
        SOURCE->size = mac->start_source_idx;
        next_char = SOURCE->elements[SOURCE->size];
        lex_expect(T_close_bracket);

        skip_newline = 0;
        bb = read_body_statement(parent, bb);

        /* cleanup */
        skip_newline = 1;
        parent->macro = NULL;
        macro_return_idx = 0;
        return bb;
    }

    /* is a function call? Skip function call check when has_asterisk is true */
    if (!has_asterisk) {
        func = find_func(token);
        if (func) {
            lex_expect(T_identifier);
            read_func_call(func, parent, &bb);
            perform_side_effect(parent, bb);
            lex_expect(T_semicolon);
            return bb;
        }
    }

    /* handle pointer dereference expressions like *ptr = value */
    if (lex_peek(T_asterisk, NULL)) {
        read_expr(parent, &bb);
        read_ternary_operation(parent, &bb);

        /* Check if it's an assignment */
        if (lex_accept(T_assign)) {
            var_t *lvalue = opstack_pop();
            read_expr(parent, &bb);
            read_ternary_operation(parent, &bb);
            var_t *rvalue = opstack_pop();

            /* Generate OP_write for pointer dereference assignment */
            add_insn(parent, bb, OP_write, NULL, lvalue, rvalue,
                     rvalue->type ? rvalue->type->size : PTR_SIZE, NULL);
        } else {
            /* Expression statement without assignment */
            perform_side_effect(parent, bb);
        }
        lex_expect(T_semicolon);
        return bb;
    }

    /* is an assignment? */
    if (read_body_assignment(token, parent, prefix_op, &bb)) {
        perform_side_effect(parent, bb);
        lex_expect(T_semicolon);
        return bb;
    }

    if (lex_peek(T_identifier, token)) {
        lex_accept(T_identifier);
        if (lex_accept(T_colon)) {
            label_t *l = find_label(token);
            if (l)
                error("label redefinition");

            basic_block_t *n = bb_create(parent);
            bb_connect(bb, n, NEXT);
            add_label(token, n);
            add_insn(parent, n, OP_label, NULL, NULL, NULL, 0, token);
            return n;
        }
    }

    error("Unrecognized statement token");
    return NULL;
}

basic_block_t *read_code_block(func_t *func,
                               macro_t *macro,
                               block_t *parent,
                               basic_block_t *bb)
{
    block_t *blk = add_block(parent, func, macro);
    bb->scope = blk;

    lex_expect(T_open_curly);

    while (!lex_accept(T_close_curly)) {
        if (read_preproc_directive())
            continue;
        bb = read_body_statement(blk, bb);
        perform_side_effect(blk, bb);
    }

    return bb;
}

void var_add_killed_bb(var_t *var, basic_block_t *bb);

void read_func_body(func_t *func)
{
    block_t *blk = add_block(NULL, func, NULL);
    func->bbs = bb_create(blk);
    func->exit = bb_create(blk);

    for (int i = 0; i < func->num_params; i++) {
        /* arguments */
        add_symbol(func->bbs, &func->param_defs[i]);
        func->param_defs[i].base = &func->param_defs[i];
        var_add_killed_bb(&func->param_defs[i], func->bbs);
    }
    basic_block_t *body = read_code_block(func, NULL, NULL, func->bbs);
    if (body)
        bb_connect(body, func->exit, NEXT);

    for (int i = 0; i < backpatch_bb_idx; i++) {
        basic_block_t *bb = backpatch_bb[i];
        insn_t *g = bb->insn_list.tail;
        label_t *label = find_label(g->str);
        if (!label)
            error("goto label undefined");

        label->used = true;
        bb_connect(bb, label->bb, NEXT);
    }

    for (int i = 0; i < label_idx; i++) {
        label_t *label = &labels[i];
        if (label->used)
            continue;

        printf("Warning: unused label %s\n", label->label_name);
    }

    backpatch_bb_idx = 0;
    label_idx = 0;
}

void print_ptr_level(int level)
{
    while (level > 0) {
        printf("*");
        level--;
    }
}

void print_func_decl(func_t *func, const char *prefix, bool newline)
{
    if (prefix)
        printf("%s", prefix);

    if (func->return_def.is_const_qualified)
        printf("const ");
    printf("%s ", func->return_def.type->type_name);
    print_ptr_level(func->return_def.ptr_level -
                    func->return_def.type->ptr_level);
    printf("%s(", func->return_def.var_name);

    for (int i = 0; i < func->num_params; i++) {
        var_t *var = &func->param_defs[i];

        if (var->is_const_qualified)
            printf("const ");
        printf("%s ", var->type->type_name);

        print_ptr_level(var->ptr_level - var->type->ptr_level);

        printf("%s", var->var_name);

        if (i != func->num_params - 1)
            printf(", ");
    }

    if (func->va_args)
        printf(", ...");
    printf(")");

    if (newline)
        printf("\n");
}

/* if first token is type */
void read_global_decl(block_t *block, bool is_const)
{
    var_t *var = require_var(block);
    var->is_global = true;
    var->is_const_qualified = is_const;

    /* new function, or variables under parent */
    read_full_var_decl(var, false, false);

    if (lex_peek(T_open_bracket, NULL)) {
        /* function */
        func_t *func = find_func(var->var_name);
        func_t func_tmp;
        bool check_decl = false;

        if (func) {
            memcpy(&func_tmp, func, sizeof(func_t));
            check_decl = true;
        } else
            func = add_func(var->var_name, false);

        memcpy(&func->return_def, var, sizeof(var_t));
        block->locals.size--;
        read_parameter_list_decl(func, 0);

        if (check_decl) {
            /* Validate whether the previous declaration and the current
             * one differ.
             */
            if ((func->return_def.type != func_tmp.return_def.type) ||
                (func->return_def.ptr_level != func_tmp.return_def.ptr_level) ||
                (func->return_def.is_const_qualified !=
                 func_tmp.return_def.is_const_qualified)) {
                printf("Error: conflicting types for the function %s.\n",
                       func->return_def.var_name);
                print_func_decl(&func_tmp, "before: ", true);
                print_func_decl(func, "after: ", true);
                abort();
            }

            if (func->num_params != func_tmp.num_params) {
                printf(
                    "Error: confilcting number of arguments for the function "
                    "%s.\n",
                    func->return_def.var_name);
                print_func_decl(&func_tmp, "before: ", true);
                print_func_decl(func, "after: ", true);
                abort();
            }

            for (int i = 0; i < func->num_params; i++) {
                var_t *func_var = &func->param_defs[i];
                var_t *func_tmp_var = &func_tmp.param_defs[i];
                if ((func_var->type != func_tmp_var->type) ||
                    (func_var->ptr_level != func_tmp_var->ptr_level) ||
                    (func_var->is_const_qualified !=
                     func_tmp_var->is_const_qualified)) {
                    printf("Error: confilcting types for the function %s.\n",
                           func->return_def.var_name);
                    print_func_decl(&func_tmp, "before: ", true);
                    print_func_decl(func, "after: ", true);
                    abort();
                }
            }

            if (func->va_args != func_tmp.va_args) {
                printf("Error: conflicting types for the function %s.\n",
                       func->return_def.var_name);
                print_func_decl(&func_tmp, "before: ", true);
                print_func_decl(func, "after: ", true);
                abort();
            }
        }

        if (lex_peek(T_open_curly, NULL)) {
            read_func_body(func);
            return;
        }
        if (lex_accept(T_semicolon)) /* forward definition */
            return;
        error("Syntax error in global declaration");
    } else
        add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0, NULL);

    /* is a variable */
    if (lex_accept(T_assign)) {
        /* If '{' follows and this is an array (explicit or implicit-size via
         * pointer syntax), reuse the array initializer to emit per-element
         * stores for globals as well.
         */
        if (lex_peek(T_open_curly, NULL) &&
            (var->array_size > 0 || var->ptr_level > 0)) {
            parse_array_init(var, block, &GLOBAL_FUNC->bbs, true);
            lex_expect(T_semicolon);
            return;
        }

        /* Otherwise fall back to scalar/constant global assignment */
        read_global_assignment(var->var_name);
        lex_expect(T_semicolon);
        return;
    } else if (lex_accept(T_comma)) {
        /* TODO: Implement global variable continuation syntax for multiple
         * declarations in single statement (e.g., int a = 1, b = 2;)
         */
        error("Global continuation not supported");
    } else if (lex_accept(T_semicolon)) {
        opstack_pop();
        return;
    }
    error("Syntax error in global declaration");
}

void consume_global_compound_literal(void)
{
    lex_expect(T_open_curly);

    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            /* Just consume constant values for now */
            if (lex_peek(T_numeric, NULL)) {
                lex_accept(T_numeric);
            } else if (lex_peek(T_minus, NULL)) {
                lex_accept(T_minus);
                lex_accept(T_numeric);
            } else if (lex_peek(T_string, NULL)) {
                lex_accept(T_string);
            } else if (lex_peek(T_char, NULL)) {
                lex_accept(T_char);
            } else {
                error("Global struct initialization requires constant values");
            }

            if (!lex_accept(T_comma))
                break;
            if (lex_peek(T_close_curly, NULL))
                break;
        }
    }
    lex_expect(T_close_curly);
}

void initialize_struct_field(var_t *nv, var_t *v, int offset)
{
    nv->type = v->type;
    nv->var_name[0] = '\0';
    nv->ptr_level = 0;
    nv->is_func = false;
    nv->is_global = false;
    nv->is_const_qualified = false;
    nv->array_size = 0;
    nv->offset = offset;
    nv->init_val = 0;
    nv->liveness = 0;
    nv->in_loop = 0;
    nv->base = NULL;
    nv->subscript = 0;
    nv->subscripts_idx = 0;
}

void read_global_statement(void)
{
    char token[MAX_ID_LEN];
    block_t *block = GLOBAL_BLOCK; /* global block */
    bool is_const = false;

    /* Handle const qualifier */
    if (lex_accept(T_const))
        is_const = true;

    if (lex_accept(T_struct)) {
        int i = 0, size = 0;

        lex_ident(T_identifier, token);

        /* variable declaration using existing struct tag? */
        if (!lex_peek(T_open_curly, NULL)) {
            type_t *decl_type = find_type(token, 2);
            if (!decl_type)
                error("Unknown struct type");

            /* one or more declarators */
            var_t *var = require_typed_var(block, decl_type);
            var->is_global = true; /* Global struct variable */
            var->is_const_qualified = is_const;
            read_partial_var_decl(var, NULL);
            add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0,
                     NULL);
            if (lex_accept(T_assign)) {
                if (lex_peek(T_open_curly, NULL) &&
                    (var->array_size > 0 || var->ptr_level > 0)) {
                    parse_array_init(var, block, &GLOBAL_FUNC->bbs, true);
                } else if (lex_peek(T_open_curly, NULL) &&
                           var->array_size == 0 && var->ptr_level == 0 &&
                           (decl_type->base_type == TYPE_struct ||
                            decl_type->base_type == TYPE_typedef)) {
                    /* Global struct compound literal support
                     * Currently we just consume the syntax - actual
                     * initialization would require runtime code which globals
                     * don't support
                     */
                    consume_global_compound_literal();
                } else {
                    read_global_assignment(var->var_name);
                }
            }
            while (lex_accept(T_comma)) {
                var_t *nv = require_typed_var(block, decl_type);
                read_inner_var_decl(nv, false, false);
                add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, nv, NULL, NULL, 0,
                         NULL);
                if (lex_accept(T_assign)) {
                    if (lex_peek(T_open_curly, NULL) &&
                        (nv->array_size > 0 || nv->ptr_level > 0)) {
                        parse_array_init(nv, block, &GLOBAL_FUNC->bbs, true);
                    } else if (lex_peek(T_open_curly, NULL) &&
                               nv->array_size == 0 && nv->ptr_level == 0 &&
                               (decl_type->base_type == TYPE_struct ||
                                decl_type->base_type == TYPE_typedef)) {
                        /* Global struct compound literal support for
                         * continuation Currently we just consume the syntax
                         */
                        consume_global_compound_literal();
                    } else {
                        read_global_assignment(nv->var_name);
                    }
                }
            }
            lex_expect(T_semicolon);
            return;
        }

        /* struct definition */
        /* has forward declaration? */
        type_t *type = find_type(token, 2);
        if (!type)
            type = add_type();

        strcpy(type->type_name, intern_string(token));
        type->base_type = TYPE_struct;

        lex_expect(T_open_curly);
        do {
            var_t *v = &type->fields[i++];
            read_full_var_decl(v, false, true);
            v->offset = size;
            size += size_var(v);

            /* Handle multiple variable declarations with same base type */
            while (lex_accept(T_comma)) {
                if (i >= MAX_FIELDS)
                    error("Too many struct fields");

                var_t *nv = &type->fields[i++];
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, true);
                nv->offset = size;
                size += size_var(nv);
            }

            lex_expect(T_semicolon);
        } while (!lex_accept(T_close_curly));

        type->size = size;
        type->num_fields = i;
        lex_expect(T_semicolon);
    } else if (lex_accept(T_union)) {
        int i = 0, max_size = 0;

        lex_ident(T_identifier, token);

        /* has forward declaration? */
        type_t *type = find_type(token, 2);
        if (!type)
            type = add_type();

        strcpy(type->type_name, intern_string(token));
        type->base_type = TYPE_union;

        lex_expect(T_open_curly);
        do {
            var_t *v = &type->fields[i++];
            read_full_var_decl(v, false, true);
            v->offset = 0; /* All union fields start at offset 0 */
            int field_size = size_var(v);
            if (field_size > max_size)
                max_size = field_size;

            /* Handle multiple variable declarations with same base type */
            while (lex_accept(T_comma)) {
                if (i >= MAX_FIELDS)
                    error("Too many union fields");

                var_t *nv = &type->fields[i++];
                /* All union fields start at offset 0 */
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, true);
                field_size = size_var(nv);
                if (field_size > max_size)
                    max_size = field_size;
            }

            lex_expect(T_semicolon);
        } while (!lex_accept(T_close_curly));

        type->size = max_size;
        type->num_fields = i;
        lex_expect(T_semicolon);
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
            strcpy(type->type_name, intern_string(token));
            lex_expect(T_semicolon);
        } else if (lex_accept(T_struct)) {
            int i = 0, size = 0;
            bool has_struct_def = false;
            type_t *tag = NULL, *type = add_type();

            /* is struct definition? */
            if (lex_peek(T_identifier, token)) {
                lex_expect(T_identifier);

                /* is existent? */
                tag = find_type(token, 2);
                if (!tag) {
                    tag = add_type();
                    tag->base_type = TYPE_struct;
                    strcpy(tag->type_name, intern_string(token));
                }
            }

            /* typedef with struct definition */
            if (lex_accept(T_open_curly)) {
                has_struct_def = true;
                do {
                    var_t *v = &type->fields[i++];
                    read_full_var_decl(v, false, true);
                    v->offset = size;
                    size += size_var(v);

                    /* Handle multiple variable declarations with same base type
                     */
                    while (lex_accept(T_comma)) {
                        if (i >= MAX_FIELDS)
                            error("Too many struct fields");

                        var_t *nv = &type->fields[i++];
                        initialize_struct_field(nv, v, 0);
                        read_inner_var_decl(nv, false, true);
                        nv->offset = size;
                        size += size_var(nv);
                    }

                    lex_expect(T_semicolon);
                } while (!lex_accept(T_close_curly));
            }

            lex_ident(T_identifier, type->type_name);
            type->size = size;
            type->num_fields = i;
            type->base_type = TYPE_typedef;

            if (tag && has_struct_def == 1) {
                strcpy(token, tag->type_name);
                memcpy(tag, type, sizeof(type_t));
                tag->base_type = TYPE_struct;
                strcpy(tag->type_name, intern_string(token));
            } else {
                /* If it is a forward declaration, build a connection between
                 * structure tag and alias. In 'find_type', it will retrieve
                 * infomation from base structure for alias.
                 */
                type->base_struct = tag;
            }

            lex_expect(T_semicolon);
        } else if (lex_accept(T_union)) {
            int i = 0, max_size = 0;
            bool has_union_def = false;
            type_t *tag = NULL, *type = add_type();

            /* is union definition? */
            if (lex_peek(T_identifier, token)) {
                lex_expect(T_identifier);

                /* is existent? */
                tag = find_type(token, 2);
                if (!tag) {
                    tag = add_type();
                    tag->base_type = TYPE_union;
                    strcpy(tag->type_name, intern_string(token));
                }
            }

            /* typedef with union definition */
            if (lex_accept(T_open_curly)) {
                has_union_def = true;
                do {
                    var_t *v = &type->fields[i++];
                    read_full_var_decl(v, false, true);
                    v->offset = 0; /* All union fields start at offset 0 */
                    int field_size = size_var(v);
                    if (field_size > max_size)
                        max_size = field_size;

                    /* Handle multiple variable declarations with same base type
                     */
                    while (lex_accept(T_comma)) {
                        if (i >= MAX_FIELDS)
                            error("Too many union fields");

                        var_t *nv = &type->fields[i++];
                        /* All union fields start at offset 0 */
                        initialize_struct_field(nv, v, 0);
                        read_inner_var_decl(nv, false, true);
                        field_size = size_var(nv);
                        if (field_size > max_size)
                            max_size = field_size;
                    }

                    lex_expect(T_semicolon);
                } while (!lex_accept(T_close_curly));
            }

            lex_ident(T_identifier, type->type_name);
            type->size = max_size;
            type->num_fields = i;
            type->base_type = TYPE_typedef;

            if (tag && has_union_def == 1) {
                strcpy(token, tag->type_name);
                memcpy(tag, type, sizeof(type_t));
                tag->base_type = TYPE_union;
                strcpy(tag->type_name, intern_string(token));
            } else {
                /* If it is a forward declaration, build a connection between
                 * union tag and alias. In 'find_type', it will retrieve
                 * information from base union for alias.
                 */
                type->base_struct = tag;
            }

            lex_expect(T_semicolon);
        } else {
            char base_type[MAX_TYPE_LEN];
            type_t *base;
            type_t *type = add_type();
            lex_ident(T_identifier, base_type);
            base = find_type(base_type, true);
            if (!base)
                error("Unable to find base type");
            type->base_type = base->base_type;
            type->size = base->size;
            type->num_fields = 0;
            type->ptr_level = 0;

            /* Handle pointer types in typedef: typedef char *string; */
            while (lex_accept(T_asterisk)) {
                type->ptr_level++;
                type->size = PTR_SIZE;
            }

            lex_ident(T_identifier, type->type_name);
            lex_expect(T_semicolon);
        }
    } else if (lex_peek(T_identifier, NULL)) {
        read_global_decl(block, is_const);
    } else
        error("Syntax error in global statement");
}

void parse_internal(void)
{
    /* set starting point of global stack manually */
    GLOBAL_FUNC = add_func("", true);
    GLOBAL_FUNC->stack_size = 4;
    GLOBAL_FUNC->bbs = arena_calloc(BB_ARENA, 1, sizeof(basic_block_t));
    GLOBAL_FUNC->bbs->belong_to = GLOBAL_FUNC; /* Prevent nullptr deref in RA */

    /* built-in types */
    TY_void = add_named_type("void");
    TY_void->base_type = TYPE_void;
    TY_void->size = 0;

    TY_char = add_named_type("char");
    TY_char->base_type = TYPE_char;
    TY_char->size = 1;

    TY_int = add_named_type("int");
    TY_int->base_type = TYPE_int;
    TY_int->size = 4;

    TY_short = add_named_type("short");
    TY_short->base_type = TYPE_short;
    TY_short->size = 2;

    /* builtin type _Bool was introduced in C99 specification, it is more
     * well-known as macro type bool, which is defined in <std_bool.h> (in
     * shecc, it is defined in 'lib/c.c').
     */
    TY_bool = add_named_type("_Bool");
    TY_bool->base_type = TYPE_char;
    TY_bool->size = 1;

    GLOBAL_BLOCK = add_block(NULL, NULL, NULL); /* global block */
    elf_add_symbol("", 0);                      /* undef symbol */

    /* architecture defines */
    add_alias(ARCH_PREDEFINED, "1");

    /* shecc run-time defines */
    add_alias("__SHECC__", "1");

    if (dynlink) {
        /* In dynamic mode, __syscall won't be implemented.
         *
         * Simply declare a 'syscall' function as follows if the program
         * needs to use 'syscall':
         *
         * int syscall(int number, ...);
         *
         * shecc will treat it as an external function, and the compiled
         * program will eventually use the implementation provided by
         * the external C library.
         *
         * If shecc supports the 'long' data type in the future, it would be
         * better to declare syscall using its original prototype:
         *
         * long syscall(long number, ...);
         * */
    } else {
        /* Linux syscall */
        func_t *func = add_func("__syscall", true);
        func->return_def.type = TY_int;
        func->num_params = 0;
        func->va_args = 1;
        func->bbs = NULL;
        /* Otherwise, allocate a basic block to implement in static mode. */
        func->bbs = arena_calloc(BB_ARENA, 1, sizeof(basic_block_t));
    }

    /* lexer initialization */
    SOURCE->size = 0;
    next_char = SOURCE->elements[0];
    lex_expect(T_start);

    do {
        if (read_preproc_directive())
            continue;
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
            break;
        }
        if (!strncmp(buffer, "#pragma once", 12) &&
            hashmap_contains(INCLUSION_MAP, file)) {
            fclose(f);
            return;
        }
        if (!strncmp(buffer, "#include ", 9) && (buffer[9] == '"')) {
            char path[MAX_LINE_LEN];
            int c = strlen(file) - 1, inclusion_path_len = strlen(buffer) - 11;
            while (c > 0 && file[c] != '/')
                c--;
            if (c) {
                /* prepend directory name */
                snprintf(path, c + 2, "%s", file);
            }

            snprintf(path + c + 1, inclusion_path_len, "%s", buffer + 10);
            load_source_file(path);
        } else {
            strbuf_puts(SOURCE, buffer);
        }
    }

    hashmap_put(INCLUSION_MAP, file, NULL);
    fclose(f);
}

void parse(char *file)
{
    load_source_file(file);
    parse_internal();
}
