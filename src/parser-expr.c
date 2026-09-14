/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Expressions: operands, pointer arithmetic, lvalues, logical and conditional
 * operators, and assignment.
 *
 * A fragment of the parser: parser.c includes it in order, so it sees every
 * definition that precedes it there and cannot be compiled on its own.
 */

/* The grouped row forms below accept only `(*name)` where name points to a
 * scalar fixed array; any other operand, such as `(*pp)[1]` on an int **,
 * belongs to ordinary expression parsing.
 */
static bool names_scalar_row_pointer(token_t *token, block_t *parent)
{
    var_t *var = find_var(token->literal, parent);

    return var && var->pointee_array_size &&
           !var->pointee_array_element_ptr_level &&
           effective_pointer_depth(var) == 1;
}

/* Keep grouped row-element postfix support deliberately exact until general
 * grouped lvalues have an address representation. This scanner never consumes
 * tokens, so every non-match remains owned by ordinary expression parsing.
 */
static bool grouped_scalar_pointee_row_postfix_starts(block_t *parent)
{
    token_t *token = cur_token ? cur_token->next : NULL;
    int suffixes = 0;

    if (!token || token->kind != T_open_bracket || !(token = token->next) ||
        token->kind != T_asterisk || !(token = token->next) ||
        token->kind != T_identifier ||
        !names_scalar_row_pointer(token, parent) || !(token = token->next) ||
        token->kind != T_close_bracket || !(token = token->next))
        return false;

    while (suffixes < 4 && token && token->kind == T_open_square) {
        int depth = 0;

        for (; token; token = token->next) {
            if (token->kind == T_open_square || token->kind == T_open_bracket ||
                token->kind == T_open_curly)
                depth++;
            else if (token->kind == T_close_square ||
                     token->kind == T_close_bracket ||
                     token->kind == T_close_curly) {
                if (!--depth) {
                    token = token->next;
                    break;
                }
            }
        }
        if (!token)
            return false;
        suffixes++;
    }

    return suffixes &&
           (token->kind == T_increment || token->kind == T_decrement);
}

static void handle_grouped_scalar_pointee_row_postfix(block_t *parent,
                                                      basic_block_t **bb)
{
    char name[MAX_VAR_LEN];
    var_t *source, *row, *index, *address, *old, *one, *updated;
    fixed_array_shape_t shape;
    int dimensions;
    int depth = 0;
    opcode_t op;

    lex_expect(T_open_bracket);
    lex_expect(T_asterisk);
    lex_ident(T_identifier, name);
    source = find_var(name, parent);
    if (!source)
        error_at("Undeclared identifier", cur_token_loc());
    if (!lower_pointee_array_dereference(source, parent, bb))
        error_at("Grouped pointer-to-array update requires a scalar fixed row",
                 cur_token_loc());
    row = opstack_pop();
    if (row->is_const_qualified)
        error_at("assignment of read-only location", cur_token_loc());
    shape = fixed_array_shape_from_var(row);
    dimensions = shape.rank;
    if (dimensions >= 3 &&
        (is_record_type(row->type) || row->type->is_floating))
        error_at(
            "Grouped rank-three/four postfix update requires an integer scalar",
            cur_token_loc());
    if (dimensions > 4)
        error_at(
            "Grouped pointer-to-array postfix update supports at most four "
            "dimensions",
            cur_token_loc());
    lex_expect(T_close_bracket);
    address = row;
    while (lex_accept(T_open_square)) {
        int stride;

        if (depth >= dimensions)
            error_at("Too many grouped array subscripts", cur_token_loc());
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        index = opstack_pop();
        lex_expect(T_close_square);
        stride = fixed_array_shape_stride(&shape, depth, row->type->size);
        if (stride != 1) {
            one = require_var(parent);
            one->var_name = gen_name();
            one->init_val = stride;
            add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
            updated = require_var(parent);
            updated->var_name = gen_name();
            add_insn(parent, *bb, OP_mul, updated, index, one, 0, NULL);
            index = updated;
        }
        updated = require_typed_ptr_var(parent, row->type, 1);
        updated->var_name = gen_name();
        add_insn(parent, *bb, OP_add, updated, address, index, 0, NULL);
        address = updated;
        depth++;
    }
    if (depth != dimensions)
        error_at("Grouped pointer-to-array postfix update needs all subscripts",
                 cur_token_loc());
    old = require_typed_var(parent, row->type);
    old->var_name = gen_name();
    add_insn(parent, *bb, OP_read, old, address, NULL, row->type->size, NULL);
    if (lex_accept(T_increment))
        op = OP_add;
    else {
        lex_expect(T_decrement);
        op = OP_sub;
    }
    one = require_typed_var(parent, TY_int);
    one->var_name = gen_name();
    one->init_val = 1;
    add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
    updated = require_var(parent);
    updated->var_name = gen_name();
    updated->type = integer_binary_result_type(op, old, one);
    add_insn(parent, *bb, op, updated, old, one, 0, NULL);
    updated = resize_to(parent, bb, updated, row->type, 0);
    add_insn(parent, *bb, OP_write, NULL, address, updated, row->type->size,
             NULL);
    opstack_push(old);
}

/* Keep prefix support equally narrow: the generic prefix path has no general
 * grouped-lvalue address representation. This scanner is non-mutating so a
 * non-match remains entirely owned by that generic path.
 */
static bool grouped_scalar_pointee_row_prefix_starts(block_t *parent)
{
    token_t *token = cur_token ? cur_token->next : NULL;
    int suffixes = 0;

    if (!token || (token->kind != T_increment && token->kind != T_decrement) ||
        !(token = token->next) || token->kind != T_open_bracket ||
        !(token = token->next) || token->kind != T_asterisk ||
        !(token = token->next) || token->kind != T_identifier ||
        !names_scalar_row_pointer(token, parent) || !(token = token->next) ||
        token->kind != T_close_bracket || !(token = token->next))
        return false;

    while (suffixes < 4 && token && token->kind == T_open_square) {
        int depth = 0;

        for (; token; token = token->next) {
            if (token->kind == T_open_square || token->kind == T_open_bracket ||
                token->kind == T_open_curly)
                depth++;
            else if (token->kind == T_close_square ||
                     token->kind == T_close_bracket ||
                     token->kind == T_close_curly) {
                if (!--depth) {
                    token = token->next;
                    break;
                }
            }
        }
        if (!token)
            return false;
        suffixes++;
    }
    return suffixes;
}

static void handle_grouped_scalar_pointee_row_prefix(block_t *parent,
                                                     basic_block_t **bb)
{
    char name[MAX_VAR_LEN];
    var_t *source, *row, *index, *address, *current, *one, *updated;
    fixed_array_shape_t shape;
    int dimensions;
    int depth = 0;
    opcode_t op;

    if (lex_accept(T_increment))
        op = OP_add;
    else {
        lex_expect(T_decrement);
        op = OP_sub;
    }
    lex_expect(T_open_bracket);
    lex_expect(T_asterisk);
    lex_ident(T_identifier, name);
    source = find_var(name, parent);
    if (!source)
        error_at("Undeclared identifier", cur_token_loc());
    if (!lower_pointee_array_dereference(source, parent, bb))
        error_at("Grouped pointer-to-array update requires a scalar fixed row",
                 cur_token_loc());
    row = opstack_pop();
    if (row->is_const_qualified)
        error_at("assignment of read-only location", cur_token_loc());
    shape = fixed_array_shape_from_var(row);
    dimensions = shape.rank;
    if (dimensions >= 3 &&
        (is_record_type(row->type) || row->type->is_floating))
        error_at(
            "Grouped rank-three/four prefix update requires an integer scalar",
            cur_token_loc());
    if (dimensions > 4)
        error_at(
            "Grouped pointer-to-array prefix update supports at most four "
            "dimensions",
            cur_token_loc());
    lex_expect(T_close_bracket);
    address = row;
    while (lex_accept(T_open_square)) {
        int stride;

        if (depth >= dimensions)
            error_at("Too many grouped array subscripts", cur_token_loc());
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        index = opstack_pop();
        lex_expect(T_close_square);
        stride = fixed_array_shape_stride(&shape, depth, row->type->size);
        if (stride != 1) {
            one = require_var(parent);
            one->var_name = gen_name();
            one->init_val = stride;
            add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
            updated = require_var(parent);
            updated->var_name = gen_name();
            add_insn(parent, *bb, OP_mul, updated, index, one, 0, NULL);
            index = updated;
        }
        updated = require_typed_ptr_var(parent, row->type, 1);
        updated->var_name = gen_name();
        add_insn(parent, *bb, OP_add, updated, address, index, 0, NULL);
        address = updated;
        depth++;
    }
    if (depth != dimensions)
        error_at("Grouped pointer-to-array prefix update needs all subscripts",
                 cur_token_loc());
    current = require_typed_var(parent, row->type);
    current->var_name = gen_name();
    add_insn(parent, *bb, OP_read, current, address, NULL, row->type->size,
             NULL);
    one = require_typed_var(parent, TY_int);
    one->var_name = gen_name();
    one->init_val = 1;
    add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
    updated = require_var(parent);
    updated->var_name = gen_name();
    updated->type = integer_binary_result_type(op, current, one);
    add_insn(parent, *bb, op, updated, current, one, 0, NULL);
    updated = resize_to(parent, bb, updated, row->type, 0);
    add_insn(parent, *bb, OP_write, NULL, address, updated, row->type->size,
             NULL);
    opstack_push(updated);
}

/* The type name read after "(" by read_parenthesized_operand(), for the cast or
 * compound literal it begins.
 */
typedef struct {
    type_t *type;
    int ptr_level;
    int array_size;
    int array_dim2;
    int array_dim3;
    int array_dim4;
    int array_dims;
    int array_element_ptr_level;
    int pointee_array_size;
    int pointee_array_dim2;
    int pointee_array_dim3;
    int pointee_array_dim4;
    bool array_outer_unsized;
    bool parenthesized_array;
    bool const_qualified;
    bool const_pointer;
    unsigned int pointer_const_mask;
    bool volatile_qualified;
} paren_type_name_t;

/* A subscript applied to the value on top of the operand stack, which has no
 * declaration for read_lvalue() to follow.
 */
static void lower_value_subscript(block_t *parent, basic_block_t **bb)
{
    var_t *base = opstack_pop();
    var_t *index;
    var_t *address;
    var_t *vd;

    lex_expect(T_open_square);
    if (base->array_size || base->has_unsized_array) {
        int subscript_depth = 0;
        int array_dims =
            1 + !!base->array_dim2 + !!base->array_dim3 + !!base->array_dim4;
        int element_size = base->ptr_level ? PTR_SIZE : base->type->size;

        /* A grouping around an array, such as an array compound literal,
         * preserves its array type. Lower each following index against the
         * original shape, just as the direct compound-literal postfix path
         * does. An array of pointers, as `*p` is for a pointer p to one, has
         * pointer-sized elements whatever their base type.
         */
        address = base;
        do {
            int stride = element_size;

            if (!read_assignment_expression(parent, bb)) {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
            }
            index = opstack_pop();
            lex_expect(T_close_square);
            if (subscript_depth == 0 && base->array_dim2) {
                stride *= base->array_dim2;
                if (base->array_dim3)
                    stride *= base->array_dim3;
                if (base->array_dim4)
                    stride *= base->array_dim4;
            } else if (subscript_depth == 1 && base->array_dim3) {
                stride *= base->array_dim3;
                if (base->array_dim4)
                    stride *= base->array_dim4;
            } else if (subscript_depth == 2 && base->array_dim4) {
                stride *= base->array_dim4;
            }
            if (stride != 1) {
                var_t *scale = require_var(parent);
                var_t *scaled = require_var(parent);

                scale->var_name = gen_name();
                scale->init_val = stride;
                add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL, 0,
                         NULL);
                scaled->var_name = gen_name();
                add_insn(parent, *bb, OP_mul, scaled, index, scale, 0, NULL);
                index = scaled;
            }
            var_t *indexed =
                require_typed_ptr_var(parent, base->type, base->ptr_level + 1);
            indexed->var_name = gen_name();
            add_insn(parent, *bb, OP_add, indexed, address, index, 0, NULL);
            address = indexed;
            subscript_depth++;
        } while (subscript_depth < array_dims && lex_accept(T_open_square));

        if (subscript_depth < array_dims) {
            opstack_push(address);
        } else {
            vd = require_typed_ptr_var(parent, base->type, base->ptr_level);
            vd->var_name = gen_name();
            vd->is_const_qualified = base->is_const_qualified;
            push_object_at(parent, bb, vd, address, element_size);
        }
        return;
    }

    /* E1[E2] is (*((E1)+(E2))): pointer arithmetic scales the index by the
     * element size, and the dereference yields the element object.
     */
    if (!read_assignment_expression(parent, bb)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
    }
    index = opstack_pop();
    lex_expect(T_close_square);
    if (is_pointer_like_value(base) == is_pointer_like_value(index) ||
        is_record_object(base) || is_record_object(index))
        error_at("Cannot apply square operator to non-pointer",
                 cur_token_loc());
    handle_pointer_arithmetic(parent, bb, OP_add, base, index);
    push_dereference(parent, bb, opstack_pop());
}

/* Whether the next tokens are an integer object @var followed by a subscript,
 * the E1[E2] spelling whose array is the second operand: `i[arr]`.
 */
bool is_swapped_subscript_base(const var_t *var)
{
    return cur_token->next && cur_token->next->next &&
           cur_token->next->next->kind == T_open_square &&
           !has_effective_pointer(var) && !is_array_declarator(var) &&
           !var->has_unsized_array && !var->is_func && !var->func_signature &&
           !is_record_type(var->type);
}

/* A member selection with `.` or `->` applied to the value on top of the
 * operand stack.
 */
static void lower_value_member(block_t *parent, basic_block_t **bb)
{
    var_t *base = opstack_pop();
    var_t *address;
    type_t *record_type;
    bool is_lvalue = true;

    if (lex_peek(T_arrow, NULL)) {
        record_type = pointee_type_from_pointer_typedef(base->type);
        if (base->func_signature || base->is_func ||
            (effective_pointer_depth(base) != 1 &&
             !(base->array_size && !effective_pointer_depth(base))) ||
            !is_record_type(record_type))
            error_at("Cannot apply arrow operator to non-record pointer",
                     cur_token_loc());
        if (effective_pointer_depth(base) != 1)
            record_type = base->type;
        address = base;
        lower_member_postfix(address, record_type, true, true, parent, bb);
        return;
    }

    if (!base->type || !is_record_type(base->type) ||
        effective_pointer_depth(base) || base->array_size)
        error_at("Cannot apply dot operator to non-record", cur_token_loc());
    record_type = base->type;
    if (base->is_compound_literal_reference) {
        /* A record loaded from an object: select from the object itself. */
        address = base->compound_literal_address;
    } else {
        /* A named record or a compound literal is an lvalue; a temporary, such
         * as a call or assignment result, is not.
         */
        is_lvalue = base->is_compound_literal || is_named_object(base, parent);
        address = require_ref_var(parent, base->type, 0);
        address->var_name = gen_name();
        add_insn(parent, *bb, OP_address_of, address, base, NULL, 0, NULL);
    }
    lower_member_postfix(address, record_type, false, is_lvalue, parent, bb);
}

/* The postfix operators C99 6.5.2 lets follow any postfix expression, applied
 * to the value on top of the operand stack: a parenthesized expression or a
 * call result, neither of which has a declaration for read_lvalue() to follow.
 * A value loaded from an object records its address, so subscripts, member
 * selections and updates reach that object.
 */
void lower_postfix_operators(block_t *parent, basic_block_t **bb)
{
    for (;;) {
        var_t *top = operand_stack[operand_stack_idx - 1];

        if (lex_peek(T_open_square, NULL)) {
            if (top->pointee_array_size)
                lower_call_result_array_postfix(&top, parent, bb);
            else
                lower_value_subscript(parent, bb);
        } else if (lex_peek(T_dot, NULL) || lex_peek(T_arrow, NULL)) {
            lower_value_member(parent, bb);
        } else if (lex_peek(T_open_bracket, NULL)) {
            /* Function calls are postfix expressions, so a parenthesized
             * function designator remains callable: `(fn)(...)` and
             * `(*fp)(...)` have the same meaning as `fn(...)` and `fp(...)`.
             */
            func_t *signature = get_func_signature(top);

            if (signature) {
                if (!top->ptr_level) {
                    opstack_pop();
                    opstack_push(load_function_pointer_object(parent, bb, top));
                }
                emit_indirect_call_result(top, signature, true, parent, bb);
            } else if (top->is_func) {
                signature = find_func(top->var_name);
                if (!signature)
                    error_at("Called object is not a function",
                             cur_token_loc());
                opstack_pop();
                emit_direct_call_result(signature, true, parent, bb);
            } else {
                error_at("Called object is not a function pointer",
                         cur_token_loc());
            }
        } else
            break;
    }

    if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
        var_t *object = opstack_pop();
        opcode_t op = OP_sub;

        if (lex_accept(T_increment))
            op = OP_add;
        else
            lex_expect(T_decrement);
        if (!object->is_compound_literal_reference &&
            is_named_object(object, parent)) {
            /* A grouped named object, `(count)++`, updates the variable. */
            var_t *old =
                require_typed_ptr_var(parent, object->type, object->ptr_level);
            var_t *one = require_typed_var(parent, TY_int);
            var_t *updated;

            if (object->array_size)
                error_at("assignment to expression with array type",
                         cur_token_loc());
            if (object->is_const_qualified || is_record_object(object) ||
                object->is_func)
                error_at(
                    "Increment or decrement requires a scalar modifiable "
                    "lvalue",
                    cur_token_loc());
            old->var_name = gen_name();
            add_insn(parent, *bb, OP_assign, old, object, NULL, 0, NULL);
            one->var_name = gen_name();
            one->init_val = 1;
            add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
            if (is_pointer_operation(op, object, one)) {
                handle_pointer_arithmetic(parent, bb, op, object, one);
                updated = opstack_pop();
            } else {
                updated = require_var(parent);
                updated->var_name = gen_name();
                updated->type = integer_binary_result_type(op, object, one);
                add_insn(parent, *bb, op, updated, object, one, 0, NULL);
            }
            updated = resize_var(parent, bb, updated, object);
            mark_var_mutated(object);
            add_insn(parent, *bb, OP_assign, object, updated, NULL, 0, NULL);
            opstack_push(old);
            return;
        }
        lower_reference_update(object, op, parent, bb);

        /* The expression has the old value and is not itself an lvalue. */
        object->is_compound_literal_reference = false;
        opstack_push(object);
    }
}

/* A parenthesized expression, possibly a comma expression, and the postfix
 * operators applied to its value.
 */
static void read_grouped_operand(block_t *parent, basic_block_t **bb)
{
    /* Regular parenthesized expression */
    if (!read_assignment_expression(parent, bb)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
    }
    while (lex_accept(T_comma)) {
        /* A comma inside an explicit parenthesized expression is the C99
         * sequencing operator, not an argument or initializer delimiter.
         * Discard the completed left value after its IR is emitted; the final
         * expression supplies the result, which is not an lvalue.
         */
        opstack_pop();
        perform_side_effect(parent, *bb);
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        operand_stack[operand_stack_idx - 1]->is_compound_literal_reference =
            false;
    }
    lex_expect(T_close_bracket);
    lower_postfix_operators(parent, bb);
}

/* Subscripts, and the updates or stores that follow them, applied directly to
 * an array compound literal.
 */
static void lower_array_literal_subscripts(block_t *parent,
                                           basic_block_t **bb,
                                           paren_type_name_t *tn,
                                           var_t *compound_var)
{
    var_t *base = opstack_pop();
    var_t *address = base;
    var_t *element;
    int element_size = compound_var->type->size;
    int subscript_depth = 0;

    while (lex_accept(T_open_square)) {
        var_t *index;
        int stride = element_size;

        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        index = opstack_pop();
        lex_expect(T_close_square);
        if (subscript_depth == 0 && compound_var->array_dim2) {
            stride *= compound_var->array_dim2;
            if (compound_var->array_dim3)
                stride *= compound_var->array_dim3;
            if (compound_var->array_dim4)
                stride *= compound_var->array_dim4;
        } else if (subscript_depth == 1 && compound_var->array_dim3) {
            stride *= compound_var->array_dim3;
            if (compound_var->array_dim4)
                stride *= compound_var->array_dim4;
        } else if (subscript_depth == 2 && compound_var->array_dim4) {
            stride *= compound_var->array_dim4;
        }
        if (stride != 1) {
            var_t *scale = require_var(parent);
            scale->var_name = gen_name();
            scale->init_val = stride;
            add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL, 0, NULL);
            var_t *scaled = require_var(parent);
            scaled->var_name = gen_name();
            add_insn(parent, *bb, OP_mul, scaled, index, scale, 0, NULL);
            index = scaled;
        }
        var_t *indexed = require_typed_ptr_var(parent, compound_var->type, 1);
        indexed->var_name = gen_name();
        add_insn(parent, *bb, OP_add, indexed, address, index, 0, NULL);
        address = indexed;
        subscript_depth++;
    }

    if (subscript_depth < tn->array_dims) {
        /* A partially selected row decays to its first element. It remains a
         * pointer value, not a scalar read.
         */
        opstack_push(address);
    } else {
        opcode_t compound_op = OP_generic;
        if (lex_accept(T_assign) || accept_compound_assign_op(&compound_op)) {
            if (compound_var->is_const_qualified)
                error_at("assignment of read-only location", cur_token_loc());
            var_t *value;
            if (!read_assignment_expression(parent, bb)) {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
            }
            value = opstack_pop();
            if (compound_op != OP_generic) {
                var_t *current = require_typed_var(parent, compound_var->type);
                current->var_name = gen_name();
                add_insn(parent, *bb, OP_read, current, address, NULL,
                         element_size, NULL);
                /* A pointer object steps by its element size. */
                if (is_pointer_operation(compound_op, current, value)) {
                    handle_pointer_arithmetic(parent, bb, compound_op, current,
                                              value);
                    value = opstack_pop();
                } else {
                    current = integer_promote_operand(parent, bb, current);
                    value = integer_promote_operand(parent, bb, value);
                    normalize_integer_binary_operands(parent, bb, compound_op,
                                                      &current, &value);
                    var_t *combined = require_var(parent);
                    combined->var_name = gen_name();
                    combined->type =
                        integer_binary_result_type(compound_op, current, value);
                    add_insn(parent, *bb, compound_op, combined, current, value,
                             0, NULL);
                    value = combined;
                }
            }
            value = convert_stored_value(parent, bb, value, compound_var->type,
                                         compound_var->ptr_level);
            add_insn(parent, *bb, OP_write, NULL, address, value, element_size,
                     NULL);
        }
        if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
            opcode_t op = OP_sub;
            var_t *one;
            var_t *updated;

            if (lex_accept(T_increment))
                op = OP_add;
            else
                lex_expect(T_decrement);

            if (compound_var->is_const_qualified)
                error_at("assignment of read-only location", cur_token_loc());
            element = require_typed_var(parent, compound_var->type);
            element->var_name = gen_name();
            add_insn(parent, *bb, OP_read, element, address, NULL, element_size,
                     NULL);
            one = require_typed_var(parent, TY_int);
            one->var_name = gen_name();
            one->init_val = 1;
            add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
            updated = require_var(parent);
            updated->var_name = gen_name();
            updated->type = integer_binary_result_type(op, element, one);
            add_insn(parent, *bb, op, updated, element, one, 0, NULL);
            updated = resize_var(parent, bb, updated, element);
            add_insn(parent, *bb, OP_write, NULL, address, updated,
                     element_size, NULL);
            opstack_push(element);
        } else {
            element = require_typed_var(parent, compound_var->type);
            element->var_name = gen_name();
            add_insn(parent, *bb, OP_read, element, address, NULL, element_size,
                     NULL);
            element->is_compound_literal_reference = true;
            element->is_const_qualified = compound_var->is_const_qualified;
            element->compound_literal_address = address;
            opstack_push(element);
        }
    }
}

/* Member selections on a record compound literal, with the subscripts, updates
 * and stores that may follow the selected member.
 */
static void lower_record_literal_members(block_t *parent,
                                         basic_block_t **bb,
                                         paren_type_name_t *tn,
                                         var_t *compound_object)
{
    char field_name[MAX_ID_LEN];
    var_t *base = opstack_pop();
    var_t *base_addr = require_ref_var(parent, base->type, 0);
    var_t *field;
    var_t *address;
    var_t *element;
    opcode_t compound_op = OP_generic;
    type_t *value_type;
    int value_ptr_level;
    int value_size;
    type_t *record_type = tn->type;

    base_addr->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, base_addr, base, NULL, 0, NULL);
    address = base_addr;
    while (1) {
        lex_ident(T_identifier, field_name);
        field = find_member(field_name, record_type);
        if (!field)
            error_at("Unknown record member", cur_token_loc());
        address = compute_field_address(parent, bb, address, field);

        /* The member-chain delimiter is already a token stream property.
         * Advance it directly rather than depending on a boolean helper result
         * after the current field's IR lowering; this keeps the syntactic chain
         * intact through self-hosted target builds as well.
         */
        if (!cur_token->next || cur_token->next->kind != T_dot)
            break;
        lex_next();
        if (!is_record_type(field->type) || field->ptr_level ||
            field->array_size)
            error_at("Member access requires a record", cur_token_loc());
        record_type = field->type;
    }
    value_type = field->type;
    value_ptr_level = field->ptr_level;
    value_size = field->type->size;

    /* Keep array-member subscripts as lvalues. The field address starts at
     * element zero; each index is scaled by the complete extent of the
     * remaining dimensions, just as ordinary array postfix parsing does.
     */
    int subscript_depth = 0;
    while (field->array_size && lex_accept(T_open_square)) {
        var_t *index;
        int stride = value_size;

        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        index = opstack_pop();
        lex_expect(T_close_square);
        if (subscript_depth == 0 && field->array_dim2 > 0) {
            stride *= field->array_dim2;
            if (field->array_dim3 > 0)
                stride *= field->array_dim3;
            if (field->array_dim4 > 0)
                stride *= field->array_dim4;
        } else if (subscript_depth == 1 && field->array_dim3 > 0) {
            stride *= field->array_dim3;
            if (field->array_dim4 > 0)
                stride *= field->array_dim4;
        } else if (subscript_depth == 2 && field->array_dim4 > 0) {
            stride *= field->array_dim4;
        }
        if (stride != 1) {
            var_t *scale = require_var(parent);
            scale->var_name = gen_name();
            scale->init_val = stride;
            add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL, 0, NULL);
            var_t *scaled = require_var(parent);
            scaled->var_name = gen_name();
            add_insn(parent, *bb, OP_mul, scaled, index, scale, 0, NULL);
            index = scaled;
        }
        var_t *indexed = require_typed_ptr_var(parent, value_type, 1);
        indexed->var_name = gen_name();
        add_insn(parent, *bb, OP_add, indexed, address, index, 0, NULL);
        address = indexed;
        value_ptr_level = 0;
        subscript_depth++;
    }

    if (lex_accept(T_assign) || accept_compound_assign_op(&compound_op)) {
        if (compound_object->is_const_qualified)
            error_at("assignment of read-only location", cur_token_loc());
        var_t *value;
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        value = opstack_pop();
        if (compound_op != OP_generic) {
            var_t *current;
            if (is_bitfield(field))
                current = read_bitfield_value(parent, bb, address, field);
            else {
                current = require_typed_var(parent, value_type);
                current->ptr_level = value_ptr_level;
                current->var_name = gen_name();
                add_insn(parent, *bb, OP_read, current, address, NULL,
                         value_size, NULL);
            }
            /* A pointer object steps by its element size. */
            if (is_pointer_operation(compound_op, current, value)) {
                handle_pointer_arithmetic(parent, bb, compound_op, current,
                                          value);
                value = opstack_pop();
            } else {
                current = integer_promote_operand(parent, bb, current);
                value = integer_promote_operand(parent, bb, value);
                normalize_integer_binary_operands(parent, bb, compound_op,
                                                  &current, &value);
                var_t *combined = require_var(parent);
                combined->var_name = gen_name();
                combined->type =
                    integer_binary_result_type(compound_op, current, value);
                add_insn(parent, *bb, compound_op, combined, current, value, 0,
                         NULL);
                value = combined;
            }
        }
        if (is_bitfield(field))
            write_bitfield_value(parent, bb, address, value, field);
        else {
            value = convert_stored_value(parent, bb, value, value_type,
                                         value_ptr_level);
            add_insn(parent, *bb, OP_write, NULL, address, value, value_size,
                     NULL);
        }
    }

    if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
        opcode_t op = lex_accept(T_increment) ? OP_add : OP_sub;
        var_t *old;
        var_t *one;
        var_t *updated;

        if (compound_object->is_const_qualified)
            error_at("assignment of read-only location", cur_token_loc());
        if (is_bitfield(field))
            old = read_bitfield_value(parent, bb, address, field);
        else {
            old = require_typed_var(parent, value_type);
            old->ptr_level = value_ptr_level;
            old->var_name = gen_name();
            add_insn(parent, *bb, OP_read, old, address, NULL, value_size,
                     NULL);
        }
        one = require_typed_var(parent, TY_int);
        one->var_name = gen_name();
        one->init_val = 1;
        add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
        updated = require_var(parent);
        updated->var_name = gen_name();
        updated->type = integer_binary_result_type(op, old, one);
        add_insn(parent, *bb, op, updated, old, one, 0, NULL);
        updated = resize_var(parent, bb, updated, old);
        if (is_bitfield(field))
            write_bitfield_value(parent, bb, address, updated, field);
        else
            add_insn(parent, *bb, OP_write, NULL, address, updated, value_size,
                     NULL);
        element = old;
    } else if (is_bitfield(field)) {
        element = read_bitfield_value(parent, bb, address, field);
        element->is_bitfield = false;
    } else {
        element = require_typed_var(parent, value_type);
        element->ptr_level = value_ptr_level;
        element->var_name = gen_name();
        add_insn(parent, *bb, OP_read, element, address, NULL, value_size,
                 NULL);
    }

    /* A scalar member of a compound literal remains a modifiable lvalue. Retain
     * its storage location for a surrounding prefix ++/--, including bit-fields
     * which need the masked write path. Partially selected array rows decay
     * instead.
     */
    int field_dims = field->array_size ? 1 : 0;
    if (field->array_dim2)
        field_dims++;
    if (field->array_dim3)
        field_dims++;
    if (field->array_dim4)
        field_dims++;
    if (!field->array_size || subscript_depth >= field_dims) {
        element->is_compound_literal_reference = true;
        element->is_const_qualified = compound_object->is_const_qualified;
        element->compound_literal_address = address;
        if (is_bitfield(field))
            element->compound_literal_bitfield = field;
    }
    opstack_push(element);
}

/* A compound literal whose type name read_parenthesized_operand() has just
 * parsed: materialize the object, lower its initializer, and apply postfix
 * operators to it.
 */
static void read_compound_literal_operand(block_t *parent,
                                          basic_block_t **bb,
                                          paren_type_name_t *tn)
{
    var_t *compound_object = NULL;

    /* Create variable for compound literal result */
    var_t *compound_var = require_typed_var(parent, tn->type);
    compound_var->var_name = gen_name();
    compound_var->is_compound_literal = true;
    compound_var->is_const_qualified = tn->const_qualified;
    compound_var->is_const_pointer = tn->const_pointer;
    compound_var->pointer_const_mask = tn->pointer_const_mask;

    /* Check if this is an array compound literal (int[]){...} */
    bool is_array_literal = (tn->ptr_level == -1);
    if (is_array_literal)
        tn->ptr_level = 0; /* Reset for normal processing */
    bool consumed_close_brace = false;

    /* parse_array_init() consumes its own opening brace. The older
     * one-dimensional literal helper expects it already consumed.
     */
    if (!is_array_literal ||
        (tn->array_dims <= 1 && !tn->array_element_ptr_level))
        lex_expect(T_open_curly);
    if (!is_array_literal ||
        (tn->array_dims <= 1 && !tn->array_element_ptr_level))
        reject_empty_initializer_in_strict_c99();
    /* Check if this is a pointer compound literal */
    if (is_array_literal) {
        compound_var->array_size = tn->array_size;
        compound_var->array_dim2 = tn->array_dim2;
        compound_var->array_dim3 = tn->array_dim3;
        compound_var->array_dim4 = tn->array_dim4;
        compound_var->has_unsized_array = tn->array_outer_unsized;
        compound_var->ptr_level = tn->array_element_ptr_level;
        compound_var->pointee_array_size = tn->pointee_array_size;
        compound_var->pointee_array_dim2 = tn->pointee_array_dim2;
        compound_var->pointee_array_dim3 = tn->pointee_array_dim3;
        compound_var->pointee_array_dim4 = tn->pointee_array_dim4;
        add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0, NULL);
        if (tn->array_dims > 1 || tn->array_element_ptr_level)
            parse_array_init(compound_var, parent, bb, true);
        else
            parse_array_compound_literal(compound_var, parent, bb);

        if (compound_var->array_size == 0) {
            compound_var->init_val = 0;
            add_insn(parent, *bb, OP_load_constant, compound_var, NULL, NULL, 0,
                     NULL);
        }
        opstack_push(compound_var);
        consumed_close_brace = true;
    } else if (tn->ptr_level > 0) {
        /* Pointer compound literal: (int*){&x} */
        var_t *initializer;

        compound_var->ptr_level = tn->ptr_level;

        /* Materialize the pointer object from the expression value, rather than
         * treating a local address as an init_val constant. Compound literals
         * have automatic storage and must retain a usable pointer value for
         * later updates.
         */
        if (!lex_peek(T_close_curly, NULL)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
            initializer = opstack_pop();

            if (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL))
                error_at("Too many elements in scalar compound literal",
                         cur_token_loc());
        } else {
            /* Empty pointer compound literal: (int*){} */
            initializer = require_typed_var(parent, tn->type);
            initializer->ptr_level = tn->ptr_level;
            initializer->var_name = gen_name();
            initializer->init_val = 0;
            add_insn(parent, *bb, OP_load_constant, initializer, NULL, NULL, 0,
                     NULL);
        }

        add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0, NULL);
        emit_object_assignment(parent, bb, compound_var, initializer);
        opstack_push(compound_var);

        /* Like scalar integer compound literals, a pointer compound literal is
         * a block-lived modifiable object. Keep it as the assignment target for
         * an immediately following `=`.
         */
        compound_object = compound_var;
    } else if (is_record_type(tn->type)) {
        /* Record compound literals use the same aggregate initializer path for
         * structs, unions, and their typedef aliases.
         */
        type_t *struct_type = tn->type;
        if (struct_type->base_type == TYPE_typedef && struct_type->base_struct)
            struct_type = struct_type->base_struct;

        add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0, NULL);
        var_t *compound_addr = require_ref_var(parent, compound_var->type, 0);
        compound_addr->var_name = gen_name();
        add_insn(parent, *bb, OP_address_of, compound_addr, compound_var, NULL,
                 0, NULL);
        parse_struct_field_init(parent, bb, struct_type, compound_addr, true);
        compound_object = compound_var;
        opstack_push(compound_var);
    } else if (tn->type->base_type == TYPE_int ||
               tn->type->base_type == TYPE_short ||
               tn->type->base_type == TYPE_char) {
        /* Handle empty compound literals */
        if (lex_peek(T_close_curly, NULL)) {
            /* Empty compound literal: (int){} */
            compound_var->init_val = 0;
            compound_var->array_size = 0;
            add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0,
                     NULL);
            var_t *zero = require_typed_var(parent, tn->type);
            zero->var_name = gen_name();
            zero->init_val = 0;
            add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
            emit_object_assignment(parent, bb, compound_var, zero);
            opstack_push(compound_var);
            compound_object = compound_var;
        } else if (lex_peek(T_numeric, NULL) || lex_peek(T_identifier, NULL) ||
                   lex_peek(T_char, NULL)) {
            /* Parse first element */
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);

            /* A scalar compound literal has one initializer; a trailing comma
             * is permitted, but a second value is a C99 constraint violation.
             */
            if (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL))
                error_at("Too many elements in scalar compound literal",
                         cur_token_loc());

            /* Retained for the parser's array-literal extension; scalar
             * spellings above have already rejected a second initializer.
             */
            if (lex_peek(T_comma, NULL)) {
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
                add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0,
                         NULL);

                /* Initialize each element */
                for (int i = 0; i < element_count && i < 256; i++) {
                    if (!elements[i])
                        continue;

                    /* Store element at offset i * sizeof(element) */
                    var_t *elem_offset = require_var(parent);
                    elem_offset->init_val = i * tn->type->size;
                    elem_offset->var_name = gen_name();
                    add_insn(parent, *bb, OP_load_constant, elem_offset, NULL,
                             NULL, 0, NULL);

                    /* Calculate address of element */
                    var_t *elem_addr = require_var(parent);
                    elem_addr->ptr_level = 1;
                    elem_addr->var_name = gen_name();
                    add_insn(parent, *bb, OP_add, elem_addr, compound_var,
                             elem_offset, 0, NULL);

                    /* Store the element value */
                    add_insn(parent, *bb, OP_write, NULL, elem_addr,
                             elements[i], tn->type->size, NULL);
                }

                /* Store first element value for array-to-scalar */
                compound_var->init_val = first_element->init_val;

                /* Create result that provides first element access. This
                 * enables array compound literals in scalar contexts: int x =
                 * (int[]){1,2,3}; // x gets 1 int y = 5 + (int[]){10}; // adds
                 * 5 + 10
                 */
                var_t *result_var = require_var(parent);
                result_var->var_name = gen_name();
                result_var->type = compound_var->type;
                result_var->ptr_level = 0;
                result_var->array_size = 0;

                /* Read first element from the array */
                add_insn(parent, *bb, OP_read, result_var, compound_var, NULL,
                         compound_var->type->size, NULL);
                opstack_push(result_var);
            } else {
                /* Single value: (int){42} - scalar compound literal */
                var_t *initializer = opstack_pop();
                add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0,
                         NULL);
                emit_object_assignment(parent, bb, compound_var, initializer);
                compound_object = compound_var;
                opstack_push(compound_var);
            }
        }
    }

    if (!consumed_close_brace)
        lex_expect(T_close_curly);

    /* Array compound literals are addressable automatic arrays. Carry every
     * direct postfix subscript through the flattened backing object, scaling
     * each one by the extent of the dimensions still to its right. This keeps
     * `(int[2][3]){...}[1][2]` an lvalue just like an ordinary multidimensional
     * array access.
     */
    if (is_array_literal && lex_peek(T_open_square, NULL)) {
        lower_array_literal_subscripts(parent, bb, tn, compound_var);
    }

    /* A record compound literal is likewise an automatic object, so preserve a
     * direct member postfix as an addressable lvalue.
     */
    if (compound_object && is_record_type(tn->type) && lex_accept(T_dot)) {
        lower_record_literal_members(parent, bb, tn, compound_object);
    }

    /* A non-const scalar or record compound literal is a block-lived object in
     * C99, hence a modifiable lvalue. The ordinary parser previously collapsed
     * scalar literals to their initializer value, losing that property before
     * an immediately following assignment could be recognized.
     */
    if (compound_object && !is_record_type(tn->type) &&
        (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL))) {
        opcode_t op = lex_accept(T_increment) ? OP_add : OP_sub;
        var_t *old;
        var_t *one;
        var_t *updated;

        if (compound_object->is_const_qualified ||
            compound_object->is_const_pointer)
            error_at("assignment of read-only location", cur_token_loc());
        opstack_pop();
        old = require_typed_var(parent, compound_object->type);
        old->ptr_level = compound_object->ptr_level;
        old->var_name = gen_name();
        add_insn(parent, *bb, OP_assign, old, compound_object, NULL, 0, NULL);
        one = require_typed_var(parent, TY_int);
        one->var_name = gen_name();
        one->init_val = 1;
        add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
        if (is_pointer_operation(op, compound_object, one)) {
            handle_pointer_arithmetic(parent, bb, op, compound_object, one);
            updated = opstack_pop();
        } else {
            updated = require_var(parent);
            updated->var_name = gen_name();
            updated->type =
                integer_binary_result_type(op, compound_object, one);
            add_insn(parent, *bb, op, updated, compound_object, one, 0, NULL);
        }
        updated = resize_var(parent, bb, updated, compound_object);
        add_insn(parent, *bb, OP_assign, compound_object, updated, NULL, 0,
                 NULL);
        opstack_push(old);
    }
    opcode_t scalar_compound_op = OP_generic;
    if (compound_object && (lex_accept(T_assign) ||
                            accept_compound_assign_op(&scalar_compound_op))) {
        if (compound_object->is_const_qualified ||
            compound_object->is_const_pointer)
            error_at("assignment of read-only location", cur_token_loc());
        opstack_pop();
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        var_t *value = opstack_pop();
        if (scalar_compound_op != OP_generic) {
            var_t *current = compound_object;

            if (is_pointer_operation(scalar_compound_op, current, value)) {
                handle_pointer_arithmetic(parent, bb, scalar_compound_op,
                                          current, value);
                value = opstack_pop();
            } else {
                current = integer_promote_operand(parent, bb, current);
                value = integer_promote_operand(parent, bb, value);
                normalize_integer_binary_operands(
                    parent, bb, scalar_compound_op, &current, &value);
                var_t *combined = require_var(parent);
                combined->var_name = gen_name();
                combined->type = integer_binary_result_type(scalar_compound_op,
                                                            current, value);
                add_insn(parent, *bb, scalar_compound_op, combined, current,
                         value, 0, NULL);
                value = combined;
            }
        }
        emit_object_assignment(parent, bb, compound_object, value);
        opstack_push(compound_object);
    }
}

/* Store in result, whose integer type is the target of a cast, the payload of
 * the integer constant lo:hi converted to that type.
 *
 * Mirror the scalar cast's stored representation instead of merely copying its
 * source payload: `(unsigned char)256`, `(short)65536`, and a 64-to-32 cast all
 * become the integer constant zero and may therefore serve as null pointers.
 */
void fold_integer_constant_cast(var_t *result, unsigned int lo, unsigned int hi)
{
    if (result->type->is_bool) {
        /* `_Bool` conversion is boolean, not truncation: any nonzero source
         * becomes one, including a high 64-bit word that a 32-bit truncation
         * would otherwise lose.
         */
        result->init_val = lo || hi;
        result->init_val_hi = 0;
    } else if (result->type->size < TY_int->size) {
        unsigned int bits = result->type->size * 8;
        unsigned int mask = (1U << bits) - 1;

        lo &= mask;
        if (!result->type->is_unsigned && (lo & (1U << (bits - 1))))
            lo |= ~mask;
        result->init_val = (int) lo;
        result->init_val_hi = result->init_val < 0 ? -1 : 0;
    } else if (result->type->size == TY_int->size) {
        result->init_val = (int) lo;
        result->init_val_hi =
            result->type->is_unsigned ? 0 : (result->init_val < 0 ? -1 : 0);
    } else {
        result->init_val = (int) lo;
        result->init_val_hi = (int) hi;
    }
}

/* A cast whose type name read_parenthesized_operand() has just parsed: read the
 * operand and convert it.
 */
static void read_cast_operand(block_t *parent,
                              basic_block_t **bb,
                              paren_type_name_t *tn)
{
    /* Process cast: (type)expr Parse the expression to be cast */
    reading_unary_operand = true;
    read_expr_operand(parent, bb);

    /* Get the expression result */
    var_t *expr_var = opstack_pop();

    /* Apart from a cast to void, a cast converts a scalar to a scalar (C99
     * 6.5.4p2).
     */
    if (tn->type->base_type != TYPE_void || tn->ptr_level) {
        reject_record_operand(expr_var);
        if (!tn->ptr_level && !tn->type->ptr_level && is_record_type(tn->type))
            error_at("Cast requires a scalar type", cur_token_loc());
    }

    /* A cast of a function designator is still a pointer value. Raw symbols
     * have no local defining IR, so materialize the code address before OP_cast
     * treats it as an ordinary operand.
     */
    if (tn->type->func_signature)
        expr_var = materialize_function_designator(parent, bb, expr_var);

    /* Create variable for cast result */
    var_t *cast_var = require_typed_ptr_var(parent, tn->type, tn->ptr_level);
    cast_var->var_name = gen_name();
    cast_var->is_const_qualified = tn->const_qualified;
    cast_var->is_const_pointer = tn->const_pointer;
    cast_var->pointer_const_mask = tn->pointer_const_mask;
    cast_var->is_volatile = tn->volatile_qualified;
    if (tn->type->func_signature) {
        cast_var->ptr_level = 1;
        cast_var->func_signature = tn->type->func_signature;
    }

    /* A cast of an integer constant expression remains an integer constant
     * expression. Preserve that payload for consumers such as the
     * null-pointer-constant constraint of `?:`; pointer casts deliberately do
     * not receive this integer classification.
     */
    if (!tn->ptr_level && expr_var->is_const &&
        !is_pointer_like_value(expr_var) && !expr_var->is_func &&
        cast_var->type->base_type != TYPE_void) {
        cast_var->is_const = true;
        fold_integer_constant_cast(cast_var, (unsigned int) expr_var->init_val,
                                   (unsigned int) expr_var->init_val_hi);
    }

    /* An explicit C cast is permitted to remove qualifiers, but it is almost
     * always a bug. Keep compiling it while making that loss visible, unlike
     * implicit pointer assignments which are rejected.
     */
    if (incompatible_const_pointer_conversion(expr_var, cast_var))
        printf("Warning: discarding const qualifier in cast\n");

    /* Conversion to _Bool compares against zero (C99 6.3.1.2) rather than
     * truncating: (_Bool) 0x100 is 1, as is a nonzero high word or pointer.
     */
    if (is_bool_scalar(cast_var->type, cast_var->ptr_level)) {
        var_t *converted = normalize_bool(parent, bb, expr_var);

        if (converted != expr_var)
            converted->type = cast_var->type;
        opstack_push(converted);
        return;
    }

    /* Generate cast IR. A cast down to a narrower type has to discard the high
     * bits: OP_cast is only a move, so "(char) 300" kept the whole 300 and
     * compared unequal to 44, even though assigning the same value to a char
     * produced 44. get_size() decides that the same way the rest of the parser
     * does, including for pointers, arrays and typedefs.
     *
     * Widening an integer to long long is, for the same reason, the extension
     * an assignment performs. A move kept whatever a 32-bit instruction left in
     * the upper half, so a negative int product cast to long long came out
     * positive on AArch64, and an unsigned char came out sign-extended.
     */
    opcode_t cast_op = OP_cast;
    int cast_size = get_size(cast_var);
    if (cast_size < get_size(expr_var))
        cast_op = OP_trunc;
    else if (cast_size > TY_int->size && get_size(expr_var) <= TY_int->size &&
             !cast_var->ptr_level && !is_pointer_like_value(expr_var) &&
             !expr_var->is_func && !is_record_type(expr_var->type) &&
             !is_record_type(cast_var->type)) {
        cast_op = OP_sign_ext;
        cast_size |= get_size(expr_var) << 16;
    }
    add_insn(parent, *bb, cast_op, cast_var, expr_var, NULL, cast_size, NULL);

    /* Push the cast result */
    opstack_push(cast_var);
}

/* The operand after an opening parenthesis: a cast, a compound literal, or a
 * parenthesized expression, together with any postfix operators applied to the
 * result.
 */
static void read_parenthesized_operand(block_t *parent, basic_block_t **bb)
{
    /* Check if this is a cast, compound literal, or parenthesized expression */
    char lookahead_token[MAX_ID_LEN];
    bool is_compound_literal = false;
    bool is_cast = false;
    paren_type_name_t tn = {0};

    if (floating_type_starts_here())
        error_at("Floating point types are not yet supported", cur_token_loc());

    /* Look ahead to see if we have a typename followed by ) */
    token_t *type_start = cur_token;
    bool has_const_type = false;
    bool has_signed_type = false;
    bool has_unsigned_type = false;
    int long_type_count = 0;
    type_t *leading_scalar_type = NULL;

    if (lex_peek(T_identifier, lookahead_token) &&
        (!strcmp(lookahead_token, "char") ||
         !strcmp(lookahead_token, "short") ||
         !strcmp(lookahead_token, "int"))) {
        token_t *after_base = cur_token->next->next;

        while (after_base && after_base->kind == T_const)
            after_base = after_base->next;
        if (after_base &&
            (after_base->kind == T_signed || after_base->kind == T_unsigned)) {
            lex_expect(T_identifier);
            leading_scalar_type = find_type(lookahead_token, true);
        }
    }
    while (lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
           lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
           lex_peek(T_long, NULL)) {
        if (lex_accept(T_const))
            has_const_type = true;
        else if (lex_accept(T_volatile))
            tn.volatile_qualified = true;
        else if (lex_accept(T_signed)) {
            if (has_signed_type)
                error_at("duplicate signed type specifier", cur_token_loc());
            has_signed_type = true;
        } else if (lex_accept(T_unsigned)) {
            if (has_unsigned_type)
                error_at("duplicate unsigned type specifier", cur_token_loc());
            has_unsigned_type = true;
        } else {
            lex_expect(T_long);
            long_type_count++;
        }
    }
    if (long_type_count > 2)
        error_at("too many long type specifiers", cur_token_loc());
    if (has_signed_type && has_unsigned_type)
        error_at("both signed and unsigned specified", cur_token_loc());
    if (leading_scalar_type == TY_int &&
        lex_peek(T_identifier, lookahead_token) &&
        !strcmp(lookahead_token, "char"))
        error_at("int cannot be combined with char", cur_token_loc());
    bool has_long_type = long_type_count > 0;
    bool has_enum_type = lex_accept(T_enum);
    if (has_enum_type &&
        (has_signed_type || has_unsigned_type || has_long_type))
        error_at("enum type cannot be combined with integer specifiers",
                 cur_token_loc());
    bool has_type_identifier = lex_peek(T_identifier, lookahead_token);
    if (has_const_type || has_signed_type || has_unsigned_type ||
        has_long_type || has_enum_type || has_type_identifier ||
        lex_peek(T_struct, NULL) || lex_peek(T_union, NULL)) {
        /* Check if it's a basic type or typedef */
        token_t *saved_token = type_start;
        base_type_t record_kind = accept_record_keyword();
        bool is_record = record_kind != TYPE_void;
        if (is_record)
            lex_ident(T_identifier, lookahead_token);

        type_t *type;
        if (has_enum_type) {
            lex_ident(T_identifier, lookahead_token);
            type = reference_enum_tag(lookahead_token, parent);
        } else if (has_unsigned_type && !is_record) {
            if (has_long_type) {
                type = TY_ulong;
                if (long_type_count > 1) {
                    type = TY_ulong_long;
                }
                if (lex_peek(T_identifier, lookahead_token) &&
                    !strcmp(lookahead_token, "int"))
                    lex_expect(T_identifier);
            } else if (leading_scalar_type == TY_char) {
                type = TY_uchar;
            } else if (leading_scalar_type == TY_short) {
                if (lex_peek(T_identifier, lookahead_token) &&
                    !strcmp(lookahead_token, "int"))
                    lex_expect(T_identifier);
                type = TY_ushort;
            } else if (lex_peek(T_identifier, lookahead_token) &&
                       (!strcmp(lookahead_token, "int") ||
                        !strcmp(lookahead_token, "char") ||
                        !strcmp(lookahead_token, "short"))) {
                lex_expect(T_identifier);
                if (!strcmp(lookahead_token, "char"))
                    type = TY_uchar;
                else if (!strcmp(lookahead_token, "short"))
                    type = TY_ushort;
                else
                    type = TY_uint;
            } else
                type = TY_uint;
        } else if (has_long_type && !is_record) {
            type = TY_long;
            if (long_type_count > 1) {
                type = TY_long_long;
            }
            lex_accept(T_signed);
            lex_accept(T_const);
            if (lex_peek(T_identifier, lookahead_token) &&
                !strcmp(lookahead_token, "int"))
                lex_expect(T_identifier);
        } else if (has_signed_type && !is_record) {
            if (leading_scalar_type == TY_char ||
                leading_scalar_type == TY_short) {
                if (leading_scalar_type == TY_short &&
                    lex_peek(T_identifier, lookahead_token) &&
                    !strcmp(lookahead_token, "int"))
                    lex_expect(T_identifier);
                type = leading_scalar_type == TY_char ? TY_schar
                                                      : leading_scalar_type;
            } else if (lex_peek(T_identifier, lookahead_token) &&
                       (!strcmp(lookahead_token, "int") ||
                        !strcmp(lookahead_token, "char") ||
                        !strcmp(lookahead_token, "short"))) {
                lex_expect(T_identifier);
                type = !strcmp(lookahead_token, "char")
                           ? TY_schar
                           : find_type(lookahead_token, true);
            } else {
                type = TY_int;
            }
        } else if (leading_scalar_type) {
            type = leading_scalar_type;
        } else {
            type = is_record
                       ? find_record_tag(lookahead_token, parent, record_kind)
                       : find_type(lookahead_token, true);
        }

        if (type) {
            if (type->is_floating)
                error_at("Floating point types are not yet supported",
                         cur_token_loc());

            /* Save current position to backtrack if needed Try to parse as
             * typename
             */
            if (!is_record && !has_enum_type && !has_signed_type &&
                !has_unsigned_type && !has_long_type)
                lex_expect(T_identifier);

            /* A qualifier may appear before or after the base type. */
            while (lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL)) {
                if (lex_accept(T_const))
                    has_const_type = true;
                else
                    tn.volatile_qualified = true;
            }

            /* Check for pointer types: int*, char*, etc. */
            int ptr_level = 0;
            while (lex_accept(T_asterisk)) {
                ptr_level++;
                while (lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
                       lex_peek(T_restrict, NULL)) {
                    if (lex_accept(T_const)) {
                        tn.const_pointer = true;
                        if (ptr_level <= 32)
                            tn.pointer_const_mask |= 1U << (ptr_level - 1);
                    } else if (lex_accept(T_volatile))
                        tn.volatile_qualified = true;
                    else
                        lex_expect(T_restrict);
                }
            }

            bool is_array = false;

            /* A parenthesized abstract declarator, such as `(int
             * (*[])[2]){...}`, is an array whose elements are pointers to rows.
             * Keep the outer array and pointee bounds separate, matching named
             * pointer-to-array declarations.
             */
            if (lex_accept(T_open_bracket)) {
                int pointee_dims = 0;

                if (ptr_level || !lex_peek(T_asterisk, NULL))
                    error_at(
                        "Array compound literal needs a pointer declarator",
                        cur_token_loc());
                do {
                    lex_expect(T_asterisk);
                    tn.array_element_ptr_level++;
                    while (lex_accept(T_const) || lex_accept(T_volatile) ||
                           lex_accept(T_restrict))
                        ;
                } while (lex_peek(T_asterisk, NULL));
                lex_expect(T_open_square);
                if (!lex_peek(T_close_square, NULL)) {
                    tn.array_size = read_const_expr(parent);
                    if (tn.array_size <= 0)
                        error_at(
                            "Array compound literal needs a positive bound",
                            cur_token_loc());
                } else {
                    tn.array_outer_unsized = true;
                }
                lex_expect(T_close_square);
                lex_expect(T_close_bracket);
                while (lex_accept(T_open_square)) {
                    int bound = read_const_expr(parent);

                    if (pointee_dims >= 4)
                        error_at(
                            "Array declarators support at most four "
                            "dimensions",
                            cur_token_loc());
                    if (bound <= 0)
                        error_at("Array size must be positive",
                                 cur_token_loc());
                    if (pointee_dims == 0)
                        tn.pointee_array_size = bound;
                    else {
                        if (pointee_dims == 1)
                            tn.pointee_array_dim2 = bound;
                        else if (pointee_dims == 2)
                            tn.pointee_array_dim3 = bound;
                        else
                            tn.pointee_array_dim4 = bound;
                        tn.pointee_array_size *= bound;
                    }
                    lex_expect(T_close_square);
                    pointee_dims++;
                }
                if (!pointee_dims)
                    error_at("Array compound literal needs a row bound",
                             cur_token_loc());
                is_array = true;
                tn.array_dims = 1;
                tn.parenthesized_array = true;
            }

            /* Parse every array bound in a compound-literal type name. `var_t`
             * records the complete flattened element count plus up to three
             * trailing dimensions, the same representation ordinary declarators
             * use for `int a[2][3]`.
             */
            while (!tn.parenthesized_array && lex_accept(T_open_square)) {
                int bound = 0;

                is_array = true;
                if (tn.array_dims >= 4)
                    error_at(
                        "Array declarators support at most four dimensions",
                        cur_token_loc());
                if (lex_peek(T_numeric, NULL)) {
                    char bound_token[MAX_TOKEN_LEN];
                    lex_ident_n(T_numeric, bound_token, MAX_TOKEN_LEN);
                    bound = parse_numeric_constant(bound_token);
                    if (bound <= 0)
                        error_at(
                            "Array compound literal needs a positive bound",
                            next_token_loc());
                }
                if (!bound && tn.array_dims)
                    error_at("Only the outer array bound may be omitted",
                             cur_token_loc());
                if (!tn.array_dims)
                    tn.array_size = bound;
                else {
                    if (tn.array_size)
                        tn.array_size *= bound;
                    else
                        tn.array_size = bound;
                    if (tn.array_dims == 1)
                        tn.array_dim2 = bound;
                    else if (tn.array_dims == 2)
                        tn.array_dim3 = bound;
                    else
                        tn.array_dim4 = bound;
                }
                if (!tn.array_dims && !bound)
                    tn.array_outer_unsized = true;
                tn.array_dims++;
                lex_expect(T_close_square);
            }
            if (!is_array && type->array_size) {
                is_array = true;
                tn.array_size = type->array_size;
                tn.array_dim2 = type->array_dim2;
                tn.array_dim3 = type->array_dim3;
                tn.array_dim4 = type->array_dim4;
                tn.array_dims =
                    1 + !!tn.array_dim2 + !!tn.array_dim3 + !!tn.array_dim4;
            }

            /* Check what follows the closing ) */
            if (lex_accept(T_close_bracket)) {
                if (lex_peek(T_open_curly, NULL)) {
                    /* (type){...} - compound literal */
                    is_compound_literal = true;
                    tn.type = type;
                    tn.ptr_level = ptr_level;
                    tn.const_qualified =
                        has_const_type || type->is_const_qualified;

                    /* Store is_array flag in tn.ptr_level if it's an array */
                    if (is_array) {
                        /* Special marker for array compound literal */
                        tn.ptr_level = -1;
                    }
                } else {
                    if (is_array)
                        error_at("Cast cannot specify an array type",
                                 cur_token_loc());
                    /* (type)expr - cast expression */
                    is_cast = true;
                    tn.type = type;
                    tn.ptr_level = ptr_level;
                    tn.const_qualified =
                        has_const_type || type->is_const_qualified;
                }
            } else {
                /* Not a cast or compound literal - backtrack */
                cur_token = saved_token;
            }
        }
    }

    if (is_cast) {
        read_cast_operand(parent, bb, &tn);
    } else if (is_compound_literal) {
        read_compound_literal_operand(parent, bb, &tn);
    } else {
        read_grouped_operand(parent, bb);
    }
}

void read_expr_operand(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    bool is_neg = false;
    bool allow_ptr_arith = !reading_unary_operand;

    reading_unary_operand = false;

    if (grouped_scalar_pointee_row_postfix_starts(parent)) {
        handle_grouped_scalar_pointee_row_postfix(parent, bb);
        return;
    }

    if ((lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) &&
        grouped_scalar_pointee_row_prefix_starts(parent)) {
        handle_grouped_scalar_pointee_row_prefix(parent, bb);
        return;
    }

    bool prefix_increment = lex_peek(T_increment, NULL);
    bool prefix_decrement = lex_peek(T_decrement, NULL);
    if ((prefix_increment || prefix_decrement) && cur_token->next->next &&
        (cur_token->next->next->kind == T_open_bracket ||
         cur_token->next->next->kind == T_asterisk ||
         cur_token->next->next->kind == T_numeric)) {
        /* The operand is a unary expression rather than an identifier: `++*p`,
         * `--*p++` or `++(*q).m`. It has recorded the address of the object it
         * was loaded from, or is itself a named or compound-literal object.
         */
        opcode_t op = prefix_increment ? OP_add : OP_sub;
        var_t *object;
        var_t *one;

        if (prefix_increment)
            lex_expect(T_increment);
        else
            lex_expect(T_decrement);
        read_expr_operand(parent, bb);
        object = opstack_pop();
        if (object->is_compound_literal_reference) {
            opstack_push(lower_reference_update(object, op, parent, bb));
            return;
        }
        if (object->array_size && is_named_object(object, parent))
            error_at("assignment to expression with array type",
                     cur_token_loc());
        if ((!object->is_compound_literal &&
             !is_named_object(object, parent)) ||
            object->array_size || is_record_type(object->type) ||
            object->is_func)
            error_at("Prefix update requires a scalar modifiable lvalue",
                     cur_token_loc());
        if (object->is_const_qualified)
            error_at("assignment of read-only location", cur_token_loc());
        one = require_typed_var(parent, TY_int);
        one->var_name = gen_name();
        one->init_val = 1;
        add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
        if (is_pointer_operation(op, object, one)) {
            handle_pointer_arithmetic(parent, bb, op, object, one);
            vd = opstack_pop();
        } else {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = integer_binary_result_type(op, object, one);
            add_insn(parent, *bb, op, vd, object, one, 0, NULL);
        }
        vd = resize_var(parent, bb, vd, object);
        mark_var_mutated(object);
        add_insn(parent, *bb, OP_assign, object, vd, NULL, 0, NULL);
        opstack_push(vd);
        return;
    }

    if (lex_accept(T_plus)) {
        reading_unary_operand = true;
        read_expr_operand(parent, bb);
        rs1 = opstack_pop();
        reject_record_operand(rs1);
        if (is_pointer_like_value(rs1) || rs1->is_func)
            error_at("unary plus requires an arithmetic operand",
                     cur_token_loc());
        rs1 = integer_promote_operand(parent, bb, rs1);

        /* Unary plus performs the integer promotions, producing an ordinary
         * arithmetic expression rather than a bit-field designator.
         */
        rs1->is_bitfield = false;
        opstack_push(rs1);
        return;
    }

    if (lex_accept(T_minus)) {
        is_neg = true;
        if (!lex_peek(T_numeric, NULL) && !lex_peek(T_identifier, NULL) &&
            !lex_peek(T_open_bracket, NULL)) {
            error_at("Unexpected token after unary minus", next_token_loc());
        }
    }

    if (lex_peek(T_string, NULL))
        read_literal_param(parent, *bb);
    else if (lex_peek(T_wstring, NULL))
        read_wstring_param(parent, *bb);
    else if (lex_peek(T_char, NULL))
        read_char_param(parent, *bb);
    else if (lex_peek(T_wchar, NULL))
        read_wchar_param(parent, *bb);

    else if (lex_peek(T_floating, NULL))
        error_at("Floating point literals are not yet supported",
                 cur_token_loc());

    else if (lex_peek(T_numeric, NULL) && cur_token->next->next &&
             cur_token->next->next->kind == T_open_square) {
        /* E1[E2] is (*((E1)+(E2))), so the integer may come first: `2[arr]`.
         * The subscript binds tighter than a unary minus, which is applied to
         * the element below.
         */
        read_numeric_param(parent, *bb, false);
        lower_postfix_operators(parent, bb);
    } else if (lex_peek(T_numeric, NULL)) {
        read_numeric_param(parent, *bb, is_neg);
        is_neg = false;
    } else if (lex_accept(T_log_not)) {
        reading_unary_operand = true;
        read_expr_operand(parent, bb);

        rs1 = opstack_pop();
        reject_record_operand(rs1);
        rs1 = materialize_function_designator(parent, bb, rs1);

        /* Constant folding for logical NOT */
        if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = TY_int;
            vd->is_const = true;
            vd->init_val = !(rs1->init_val || rs1->init_val_hi);
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else {
            vd = require_var(parent);
            vd->var_name = gen_name();

            /* C99 6.5.3.3: logical negation always yields int. Preserving the
             * operand's type made !pointer inherit the pointed-to record size,
             * so a later comparison attempted an invalid truncation on 32-bit
             * targets.
             */
            vd->type = TY_int;
            vd->ptr_level = 0;
            opstack_push(vd);
            add_insn(parent, *bb, OP_log_not, vd, rs1, NULL, 0, NULL);
        }
    } else if (lex_accept(T_bit_not)) {
        reading_unary_operand = true;
        read_expr_operand(parent, bb);

        rs1 = opstack_pop();
        reject_record_operand(rs1);
        if (is_pointer_like_value(rs1) || rs1->is_func)
            error_at("bitwise complement requires an integer operand",
                     cur_token_loc());
        rs1 = integer_promote_operand(parent, bb, rs1);

        /* Constant folding for bitwise NOT */
        if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = rs1->type;
            vd->is_const = true;
            vd->init_val = ~rs1->init_val;
            vd->init_val_hi = ~rs1->init_val_hi;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = rs1->type;
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
        read_parenthesized_operand(parent, bb);
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
        const constant_t *con = find_scoped_constant(token, parent);
        var_t *var = find_var(token, parent);
        func_t *func = find_visible_func(token, parent);

        /* A block-scope function prototype shadows an automatic object, but its
         * designator is the file-scope function rather than an indirect call
         * through that object.
         */
        if (var && var->is_extern_function_alias)
            var = NULL;

        if (!strcmp(token, "__builtin_offsetof")) {
            read_builtin_offsetof(parent, bb);
        } else if (!strcmp(token, "__builtin_va_arg")) {
            read_builtin_va_arg(parent, bb);
        } else if (!strcmp(token, "__func__")) {
            if (!parent->func || !parent->func->return_def.var_name[0])
                error_at("__func__ is only defined inside a function",
                         next_token_loc());
            lex_expect(T_identifier);
            vd = require_typed_ptr_var(parent, TY_char, true);
            vd->var_name = gen_name();
            vd->init_val = write_symbol(parent->func->return_def.var_name);
            vd->is_string_literal = true;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_rodata_address, vd, NULL, NULL, 0,
                     NULL);
            if (lex_accept(T_open_square)) {
                var_t *base = opstack_pop();
                var_t *index;
                var_t *address;

                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
                index = opstack_pop();
                lex_expect(T_close_square);
                address = require_typed_ptr_var(parent, TY_char, true);
                address->var_name = gen_name();
                add_insn(parent, *bb, OP_add, address, base, index, 0, NULL);
                vd = require_typed_var(parent, TY_char);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, address, NULL, 1, NULL);
            }
        } else if (con) {
            vd = require_var(parent);
            vd->init_val = con->value;
            vd->var_name = gen_name();
            opstack_push(vd);
            lex_expect(T_identifier);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else if (var && is_swapped_subscript_base(var)) {
            /* An integer object subscripting an array: `i[arr]`. */
            lex_expect(T_identifier);
            opstack_push(var);
            lower_postfix_operators(parent, bb);
            lower_call_result_prefix_update(&vd, prefix_op, parent, bb);
        } else if (var) {
            /* evalue lvalue expression */
            lvalue_t lvalue;
            bool deferred_call_prefix =
                prefix_op != OP_generic && cur_token->next->next &&
                cur_token->next->next->kind == T_open_bracket;

            read_lvalue(&lvalue, var, parent, bb, true,
                        deferred_call_prefix ? OP_generic : prefix_op,
                        allow_ptr_arith && !is_neg);

            /* is it an indirect call with function pointer? */
            if (lex_peek(T_open_bracket, NULL)) {
                var_t *callee = operand_stack[operand_stack_idx - 1];
                func_t *signature = get_func_signature(lvalue.decl);

                /* An indexed callback typedef has a reference to the selected
                 * element on the operand stack. Its declaration is the array
                 * (whose direct-call signature is intentionally absent), but
                 * the selected element's type retains the callback prototype.
                 */
                if (!signature && lvalue.is_reference && lvalue.type &&
                    !lvalue.value_ptr_level)
                    signature = lvalue.type->func_signature;

                if (!signature)
                    error_at("Called object is not a function pointer",
                             cur_token_loc());

                /* A standalone function-pointer object is an lvalue, so the
                 * operand stack still holds its storage location here. Calls
                 * need the stored code address instead. Member pointers have
                 * already been read by read_lvalue() because they are
                 * references; only materialize the load for an object itself.
                 */
                if (!lvalue.is_reference && lvalue.decl->func_signature) {
                    var_t *object = opstack_pop();
                    opstack_push(
                        load_function_pointer_object(parent, bb, object));
                    callee = operand_stack[operand_stack_idx - 1];
                } else if (lvalue.is_reference) {
                    /* The element/member load already produced the pointer
                     * value. Carry its prototype and pointer-valued IR shape
                     * into indirect-call lowering.
                     */
                    callee->func_signature = signature;
                    callee->ptr_level = 1;
                }
                vd = emit_indirect_call_result(callee, signature, true, parent,
                                               bb);
                lower_postfix_operators(parent, bb);
                lower_call_result_prefix_update(
                    &vd, deferred_call_prefix ? prefix_op : OP_generic, parent,
                    bb);
            }
        } else if (func) {
            if (parent->func && parent->func->is_inline &&
                !parent->func->is_static && func->is_static)
                error_at(
                    "external inline definition references internal-linkage "
                    "function",
                    next_token_loc());
            lex_expect(T_identifier);

            if (lex_peek(T_open_bracket, NULL)) {
                vd = emit_direct_call_result(func, true, parent, bb);
                lower_postfix_operators(parent, bb);
                lower_call_result_prefix_update(&vd, prefix_op, parent, bb);
            } else {
                /* indirective function pointer assignment */
                vd = require_func_symbol_var(parent);
                vd->is_func = true;
                vd->var_name = intern_string(token);
                opstack_push(vd);
            }
        } else if (lex_accept(T_open_curly)) {
            parse_array_literal_expr(parent, bb);
        } else {
            /* unknown expression */
            error_at("Unrecognized expression token", next_token_loc());
        }
    }

    /* A numeric literal takes its minus sign as part of its value; any other
     * operand, grouped ones included, is negated here. Only the identifier path
     * used to apply it, so "-(x)" evaluated to x.
     */
    if (is_neg) {
        rs1 = opstack_pop();
        reject_record_operand(rs1);
        if (is_pointer_like_value(rs1) || rs1->is_func)
            error_at("unary minus requires an arithmetic operand",
                     cur_token_loc());
        rs1 = integer_promote_operand(parent, bb, rs1);

        /* Constant folding for negation */
        if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = rs1->type;
            vd->is_const = true;
            vd->init_val = -rs1->init_val;
            vd->init_val_hi = ~rs1->init_val_hi + (vd->init_val == 0);
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = rs1->type;
            opstack_push(vd);
            add_insn(parent, *bb, OP_negate, vd, rs1, NULL, 0, NULL);
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

/* Consume a compound-assignment operator ("+=", "-=", ...) and report the
 * arithmetic it applies.
 *
 * Returns false and consumes nothing when the next token is not one, so it can
 * sit in an else-if chain beside the other statement forms.
 */
bool accept_compound_assign_op(opcode_t *op)
{
    if (lex_accept(T_pluseq))
        op[0] = OP_add;
    else if (lex_accept(T_minuseq))
        op[0] = OP_sub;
    else if (lex_accept(T_asteriskeq))
        op[0] = OP_mul;
    else if (lex_accept(T_divideeq))
        op[0] = OP_div;
    else if (lex_accept(T_modeq))
        op[0] = OP_mod;
    else if (lex_accept(T_lshifteq))
        op[0] = OP_lshift;
    else if (lex_accept(T_rshifteq))
        op[0] = OP_rshift;
    else if (lex_accept(T_xoreq))
        op[0] = OP_bit_xor;
    else if (lex_accept(T_oreq))
        op[0] = OP_bit_or;
    else if (lex_accept(T_andeq))
        op[0] = OP_bit_and;
    else
        return false;
    return true;
}

bool lvalue_write_follows(opcode_t prefix_op)
{
    return prefix_op != OP_generic || lex_peek(T_assign, NULL) ||
           lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL) ||
           lex_peek(T_pluseq, NULL) || lex_peek(T_minuseq, NULL) ||
           lex_peek(T_asteriskeq, NULL) || lex_peek(T_divideeq, NULL) ||
           lex_peek(T_modeq, NULL) || lex_peek(T_lshifteq, NULL) ||
           lex_peek(T_rshifteq, NULL) || lex_peek(T_xoreq, NULL) ||
           lex_peek(T_oreq, NULL) || lex_peek(T_andeq, NULL);
}

int get_pointer_element_size(var_t *ptr_var)
{
    int pointer_depth;

    if (!ptr_var || !ptr_var->type)
        return PTR_SIZE; /* Default to pointer size */

    pointer_depth = effective_pointer_depth(ptr_var);

    /* An array of pointers decays to a pointer-to-pointer. The declaration
     * records its element's indirection level, which may be held in a typedef.
     * Account for the decay before deriving the pointed-to object size.
     */
    if ((ptr_var->array_size || ptr_var->has_unsized_array) && pointer_depth)
        return PTR_SIZE;

    /* Direct pointer with type info.
     *
     * Only a single level of indirection points at the base type. For deeper
     * pointers (int **, char ***, ...) the element is itself a pointer, so the
     * step is PTR_SIZE. Returning the base type size there makes "q + 1"
     * advance by 4 instead of 8 on LP64 and drops a level of type information
     * from the result.
     */
    if (pointer_depth) {
        if (pointer_depth > 1)
            return PTR_SIZE;

        /* A single typedef-hidden pointer still advances by its underlying
         * pointee, not by the alias object's pointer-sized representation.
         */
        return pointer_typedef_pointee_size(
            ptr_var->type, pointee_type_from_pointer_typedef(ptr_var->type));
    }

    /* Typedef pointer or array-derived pointer */
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
        break;
    }

    return ptr_var->type->size ? ptr_var->type->size : PTR_SIZE;
}

/* A direct void pointer has no complete pointed-to object type. A pointer to
 * void pointer (void **) is different: its elements are pointer objects and
 * therefore have a known size.
 */
bool is_direct_void_pointer(const var_t *var)
{
    if (!var || !var->type || var->type->base_type != TYPE_void)
        return false;

    /* An array of void pointers decays to void ** before arithmetic. Its
     * elements are complete pointer objects, even though the declared base type
     * and direct declarator depth otherwise resemble void *.
     */
    if (var->array_size || var->has_unsized_array)
        return false;
    return var->ptr_level == 1 ||
           (var->ptr_level == 0 && var->type->ptr_level == 1);
}

bool is_direct_void_pointer_type(const type_t *type, int ptr_level)
{
    return type && type->base_type == TYPE_void &&
           (ptr_level == 1 || (ptr_level == 0 && type->ptr_level == 1));
}

/* Describe in @decayed the pointer to its first row that the multidimensional
 * array @var decays to, as pointer arithmetic on it yields.
 *
 * Returns false, and leaves @decayed unset, for any other operand.
 */
static bool decay_fixed_array_rows(var_t *decayed, const var_t *var)
{
    fixed_array_shape_t shape;

    if (!is_array_declarator(var) || !var->array_dim2 ||
        has_effective_pointer(var) || var->is_func)
        return false;
    memset(decayed, 0, sizeof(var_t));
    decayed->type = var->type;
    decayed->ptr_level = 1;
    shape = fixed_array_shape_from_var(var);
    fixed_array_shape_drop_outer(&shape);
    fixed_array_shape_to_pointee_var(decayed, &shape);
    return true;
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

    /* Functions are not objects, so no form of C99 pointer arithmetic may use a
     * function pointer. Keep this before the add/sub split below: only the
     * subtraction path performs the more specific compatible-pointee check.
     */
    if ((rs1 && rs1->is_func) || (rs2 && rs2->is_func))
        error_at("Pointer arithmetic requires object pointers",
                 cur_token_loc());

    if (is_direct_void_pointer(rs1) || is_direct_void_pointer(rs2))
        error_at("Pointer arithmetic on void* is invalid", cur_token_loc());

    /* Pointer arithmetic: differences (char*, int*, struct*, etc.),
     * addition/increment with scaling, and array indexing.
     */

    /* Check if both operands are pointers (pointer difference) */
    if (op == OP_sub) {
        /* If both are variables (not temporaries), look them up */
        var_t *orig_rs1 = rs1, *orig_rs2 = rs2;

        /* If they have names, they might be variable references - look them up
         */
        if (rs1->var_name[0]) {
            var_t *found = find_var(rs1->var_name, parent);
            if (found)
                orig_rs1 = found;
        }
        if (rs2->var_name[0]) {
            var_t *found = find_var(rs2->var_name, parent);
            if (found)
                orig_rs2 = found;
        }

        /* Check if both have ptr_level or typedef pointer type */
        bool rs1_is_ptr = is_pointer_like_value(orig_rs1) || orig_rs1->is_func;
        bool rs2_is_ptr = is_pointer_like_value(orig_rs2) || orig_rs2->is_func;

        /* If variable lookup failed, check the passed variables directly */
        if (!rs1_is_ptr)
            rs1_is_ptr = is_pointer_like_value(rs1) || rs1->is_func;
        if (!rs2_is_ptr)
            rs2_is_ptr = is_pointer_like_value(rs2) || rs2->is_func;

        if (rs1_is_ptr && rs2_is_ptr) {
            /* Both are pointers - this is pointer difference Determine element
             * size C99 6.5.6 confines pointer subtraction to pointers to
             * complete object types. A function pointer is pointer-like for
             * calls and comparisons, but it has no object elements to count.
             */
            if (orig_rs1->is_func || orig_rs2->is_func)
                error_at("Pointer subtraction requires object pointers",
                         cur_token_loc());
            type_t *left_pointee =
                pointee_type_from_pointer_typedef(orig_rs1->type);
            type_t *right_pointee =
                pointee_type_from_pointer_typedef(orig_rs2->type);

            /* An array operand has decayed to a pointer to its first element
             * (C99 6.3.2.1), one level deeper than its declaration; the element
             * of a deeper array is a row.
             */
            var_t left_decayed, right_decayed;

            if (decay_fixed_array_rows(&left_decayed, orig_rs1))
                orig_rs1 = &left_decayed;
            if (decay_fixed_array_rows(&right_decayed, orig_rs2))
                orig_rs2 = &right_decayed;
            int left_depth = orig_rs1->ptr_level + orig_rs1->type->ptr_level +
                             !!is_array_declarator(orig_rs1);
            int right_depth = orig_rs2->ptr_level + orig_rs2->type->ptr_level +
                              !!is_array_declarator(orig_rs2);
            bool left_row = is_pointee_array_pointer(orig_rs1);
            bool right_row = is_pointee_array_pointer(orig_rs2);

            if ((left_row || right_row) &&
                !pointee_array_shapes_compatible(orig_rs1, orig_rs2))
                error_at(
                    "Pointer subtraction requires compatible pointed-to types",
                    cur_token_loc());
            if (!compatible_decl_type(left_pointee, right_pointee) ||
                left_depth != right_depth)
                error_at(
                    "Pointer subtraction requires compatible pointed-to types",
                    cur_token_loc());

            element_size = PTR_SIZE; /* Default */

            element_size = left_row ? pointee_array_row_stride(orig_rs1)
                                    : get_pointer_element_size(orig_rs1);

            /* Perform subtraction first */
            var_t *diff = require_var(parent);
            diff->var_name = gen_name();
            add_insn(parent, *bb, OP_sub, diff, rs1, rs2, 0, NULL);

            /* Then divide by element size if needed */
            if (element_size > 1) {
                var_t *size_const = require_var(parent);
                size_const->var_name = gen_name();
                size_const->init_val = element_size;
                add_insn(parent, *bb, OP_load_constant, size_const, NULL, NULL,
                         0, NULL);

                var_t *result = require_var(parent);
                result->var_name = gen_name();
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
    if (is_pointer_like_value(rs1)) {
        ptr_var = rs1;
        int_var = rs2;
        element_size =
            is_pointee_array_pointer(rs1)
                ? pointee_array_row_stride(rs1)
                : (is_array_declarator(rs1) && rs1->array_dim2 &&
                           !has_effective_pointer(rs1) && !rs1->is_func
                       ? fixed_array_decay_stride(rs1)
                       : get_pointer_element_size(rs1));
    } else if (is_pointer_like_value(rs2)) {
        /* Only for addition (p + n == n + p) */
        if (op == OP_add) {
            ptr_var = rs2;
            int_var = rs1;
            element_size =
                is_pointee_array_pointer(rs2)
                    ? pointee_array_row_stride(rs2)
                    : (is_array_declarator(rs2) && rs2->array_dim2 &&
                               !has_effective_pointer(rs2) && !rs2->is_func
                           ? fixed_array_decay_stride(rs2)
                           : get_pointer_element_size(rs2));
            /* Swap operands so pointer is rs1 */
            rs1 = ptr_var;
            rs2 = int_var;
        }
    }

    /* If we need to scale the integer operand */
    if (ptr_var && element_size > 1) {
        /* Create multiplication by element size */
        var_t *size_const = require_var(parent);
        size_const->var_name = gen_name();
        size_const->init_val = element_size;
        add_insn(parent, *bb, OP_load_constant, size_const, NULL, NULL, 0,
                 NULL);

        var_t *scaled = require_var(parent);
        scaled->var_name = gen_name();
        add_insn(parent, *bb, OP_mul, scaled, int_var, size_const, 0, NULL);

        /* Use scaled value as rs2 */
        rs2 = scaled;
    }

    /* Perform the operation */
    var_t *vd = require_var(parent);
    /* Preserve pointer type metadata on results of pointer arithmetic */
    if (ptr_var) {
        vd->type = ptr_var->type;

        /* An array operand decays to a pointer before arithmetic. Retain that
         * extra level on the result so `slots + 0` for an array of pointer
         * typedefs cannot be mistaken for a direct callback value.
         */
        vd->ptr_level = ptr_var->ptr_level + !!ptr_var->array_size;
        if (is_pointee_array_pointer(ptr_var))
            copy_pointee_array_shape(vd, ptr_var);
        else if (is_array_declarator(ptr_var) && ptr_var->array_dim2 &&
                 !has_effective_pointer(ptr_var) && !ptr_var->is_func) {
            fixed_array_shape_t shape = fixed_array_shape_from_var(ptr_var);

            fixed_array_shape_drop_outer(&shape);
            fixed_array_shape_to_pointee_var(vd, &shape);
        }
    }
    vd->var_name = gen_name();
    opstack_push(vd);
    add_insn(parent, *bb, op, vd, rs1, rs2, 0, NULL);
}

/* Helper function to check if pointer arithmetic is needed */
bool is_pointer_operation(opcode_t op, var_t *rs1, var_t *rs2)
{
    if (op != OP_add && op != OP_sub)
        return false;

    return is_pointer_like_value(rs1) || is_pointer_like_value(rs2) ||
           (rs1 && rs1->is_func) || (rs2 && rs2->is_func);
}

/* The first unsigned slice has the existing int/short/char widths. Narrow
 * unsigned operands promote to int because int represents their full range; an
 * unsigned int operand gives the arithmetic result unsigned int.
 */
bool unsigned_int_operand(const var_t *var)
{
    return var && !var->ptr_level && var->type && var->type->is_unsigned &&
           var->type->size >= TY_int->size;
}

/* A source-level write invalidates the parser's constant-propagation cache.
 * This is distinct from const qualification, which controls write legality.
 */
void mark_var_mutated(var_t *var)
{
    if (var && !unevaluated_expression_depth)
        var->is_const = false;
}

/* The integer ranks currently represented by shecc are int, long (both 32-bit),
 * and long long (64-bit). Equal representation widths do not merge int and
 * long: C99 still gives long the higher rank.
 */
type_t *integer_common_type(const var_t *left, const var_t *right)
{
    /* The current type lattice has a 32-bit int/long tier and a distinct 64-bit
     * long-long tier. A 64-bit signed operand can represent every 32-bit
     * unsigned value; an unsigned 64-bit operand wins at its rank.
     */
    if ((left && left->type && left->type->size > TY_int->size) ||
        (right && right->type && right->type->size > TY_int->size)) {
        if ((left && left->type && left->type->size > TY_int->size &&
             left->type->is_unsigned) ||
            (right && right->type && right->type->size > TY_int->size &&
             right->type->is_unsigned))
            return TY_ulong_long;
        return TY_long_long;
    }

    if ((left && (left->type == TY_long || left->type == TY_ulong)) ||
        (right && (right->type == TY_long || right->type == TY_ulong))) {
        if ((left && left->type && left->type->is_unsigned) ||
            (right && right->type && right->type->is_unsigned))
            return TY_ulong;
        return TY_long;
    }

    if (unsigned_int_operand(left) || unsigned_int_operand(right))
        return TY_uint;
    return TY_int;
}

type_t *integer_binary_result_type(opcode_t op,
                                   const var_t *left,
                                   const var_t *right)
{
    if (op == OP_eq || op == OP_neq || op == OP_lt || op == OP_leq ||
        op == OP_gt || op == OP_geq)
        return TY_int;

    /* Shift counts are promoted, but do not participate in the usual arithmetic
     * conversions; the result has the promoted left type.
     */
    if (op == OP_lshift || op == OP_rshift)
        return left->type;
    return integer_common_type(left, right);
}

var_t *integer_promote_operand(block_t *parent, basic_block_t **bb, var_t *var)
{
    if (!var || var->ptr_level || !var->type || var->type->size >= TY_int->size)
        return var;
    return promote_unchecked(parent, bb, var, TY_int, 0);
}

/* Apply C99's usual arithmetic conversions after the individual integer
 * promotions. In particular this must materialize a zero-extension for an
 * unsigned int that meets a signed long long, and a sign-extension for a
 * negative int that meets unsigned long long. Merely giving the result the
 * common type leaves the machine operation to consume stale upper bits.
 */
void normalize_integer_binary_operands(block_t *parent,
                                       basic_block_t **bb,
                                       opcode_t op,
                                       var_t **left,
                                       var_t **right)
{
    type_t *common;

    if (op == OP_lshift || op == OP_rshift)
        return;

    /* Equality and relational operators also reach this helper. Their pointer
     * cases keep address semantics and are not usual arithmetic conversions.
     */
    if (is_pointer_like_value(left[0]) || is_pointer_like_value(right[0]))
        return;

    /* The ABI-visible 64-bit rank is distinct today. int and long are both
     * 32-bit in the current type model, so normalizing their signed/unsigned
     * combinations here would add conversions throughout the self-hosted
     * compiler without yet representing a distinct long rank.
     */
    if (get_size(left[0]) <= TY_int->size && get_size(right[0]) <= TY_int->size)
        return;

    common = integer_common_type(*left, *right);

    /* Do not manufacture no-op conversions. Besides bloating every unsigned
     * expression, doing so makes stage1's self-hosting input prohibitively
     * large. A conversion is observable here only across a width boundary, or
     * when a signed value is reinterpreted at an unsigned common rank.
     */
    if (get_size(left[0]) != common->size ||
        (!left[0]->type->is_unsigned && common->is_unsigned))
        left[0] = resize_to(parent, bb, left[0], common, 0);
    if (get_size(right[0]) != common->size ||
        (!right[0]->type->is_unsigned && common->is_unsigned))
        right[0] = resize_to(parent, bb, right[0], common, 0);
}

/* Whether the right operand of @op binds tighter than a following `+`. */
static bool operand_excludes_addition(opcode_t op)
{
    return op == OP_sub || op == OP_mul || op == OP_div || op == OP_mod;
}

void read_expr_body(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1, *rs2;
    opcode_t oper_stack[MAX_OPERATOR_STACK_SIZE];
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
    } else {
        if (oper_stack_idx >= MAX_OPERATOR_STACK_SIZE)
            fatal("Expression too complex: operator stack exhausted");
        oper_stack[oper_stack_idx++] = op;
    }
    reading_unary_operand = operand_excludes_addition(op);
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
                    reject_record_operand(rs1);
                    reject_record_operand(rs2);

                    /* Handle pointer arithmetic for addition and subtraction */
                    if (is_pointer_operation(top_op, rs1, rs2)) {
                        /* handle_pointer_arithmetic handles both pointer
                         * differences and regular pointer arithmetic internally
                         */
                        handle_pointer_arithmetic(parent, bb, top_op, rs1, rs2);
                        oper_stack_idx--;
                        continue;
                    }

                    rs1 = integer_promote_operand(parent, bb, rs1);
                    rs2 = integer_promote_operand(parent, bb, rs2);
                    normalize_integer_binary_operands(parent, bb, top_op, &rs1,
                                                      &rs2);
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    vd->type = integer_binary_result_type(top_op, rs1, rs2);
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
                 * previous opcode: prev_log_op == OP_log_and current opcode: op
                 * == OP_log_or current operand: b
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
                 */
                prev_log_op = op;
                pprev_log_op = 0;
            } else {
                /* For example: a || b && c
                 * previous opcode: prev_log_op == OP_log_or current opcode: op
                 * == OP_log_and current operand: b
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
        reading_unary_operand = operand_excludes_addition(op);
        read_expr_operand(parent, bb);
        if (!is_logical(op)) {
            if (oper_stack_idx >= MAX_OPERATOR_STACK_SIZE)
                fatal("Expression too complex: operator stack exhausted");
            oper_stack[oper_stack_idx++] = op;
        }
        op = get_operator();
    }

    while (oper_stack_idx > 0) {
        opcode_t top_op = oper_stack[--oper_stack_idx];
        rs2 = opstack_pop();
        rs1 = opstack_pop();
        reject_record_operand(rs1);
        reject_record_operand(rs2);

        /* Equality compares function pointers after the C99 function-to-
         * pointer conversion. A raw symbol has no SSA value and otherwise looks
         * like zero to the backend, making `function == 0` spuriously true.
         * Other arithmetic operations retain their function-pointer constraint
         * diagnostics below.
         */
        if (top_op == OP_eq || top_op == OP_neq) {
            rs1 = materialize_function_designator(parent, bb, rs1);
            rs2 = materialize_function_designator(parent, bb, rs2);
        }

        if ((top_op == OP_lt || top_op == OP_leq || top_op == OP_gt ||
             top_op == OP_geq) &&
            ((rs1 && (rs1->is_func || get_func_signature(rs1))) ||
             (rs2 && (rs2->is_func || get_func_signature(rs2)))))
            error_at("Relational comparison requires object pointers",
                     cur_token_loc());

        if (top_op == OP_eq || top_op == OP_neq) {
            func_t *left_signature = get_func_signature(rs1);
            func_t *right_signature = get_func_signature(rs2);

            if (left_signature || right_signature) {
                var_t *other = left_signature ? rs2 : rs1;

                if ((left_signature && right_signature &&
                     !compatible_function_signature(left_signature,
                                                    right_signature)) ||
                    (!get_func_signature(other) &&
                     !is_null_pointer_constant(other)))
                    error_at(
                        "Function pointer comparison requires compatible "
                        "pointers or null",
                        cur_token_loc());
            }
        }

        bool rs1_is_placeholder = is_array_literal_placeholder(rs1);
        bool rs2_is_placeholder = is_array_literal_placeholder(rs2);
        bool rs1_is_ptr_like =
            is_pointer_like_value(rs1) || (rs1 && rs1->is_func);
        bool rs2_is_ptr_like =
            is_pointer_like_value(rs2) || (rs2 && rs2->is_func);
        bool pointer_context = (rs1_is_ptr_like && !rs1_is_placeholder) ||
                               (rs2_is_ptr_like && !rs2_is_placeholder);

        /* Pointer arithmetic handling */
        if (pointer_context && is_pointer_operation(top_op, rs1, rs2)) {
            handle_pointer_arithmetic(parent, bb, top_op, rs1, rs2);
            continue; /* skip normal processing */
        }

        if ((top_op == OP_eq || top_op == OP_neq || top_op == OP_lt ||
             top_op == OP_leq || top_op == OP_gt || top_op == OP_geq) &&
            incompatible_character_pointer_conversion(rs1, rs2))
            error_at("incompatible character pointer types in comparison",
                     cur_token_loc());

        if (rs1_is_placeholder && rs2_is_placeholder) {
            rs1 = scalarize_array_literal(parent, bb, rs1, NULL);
            rs2 = scalarize_array_literal(parent, bb, rs2, NULL);
        } else {
            if (rs1_is_placeholder && !rs2_is_ptr_like)
                rs1 = scalarize_array_literal(
                    parent, bb, rs1, rs2 && rs2->type ? rs2->type : NULL);

            if (rs2_is_placeholder && !rs1_is_ptr_like)
                rs2 = scalarize_array_literal(
                    parent, bb, rs2, rs1 && rs1->type ? rs1->type : NULL);
        }
        rs1 = integer_promote_operand(parent, bb, rs1);
        rs2 = integer_promote_operand(parent, bb, rs2);
        normalize_integer_binary_operands(parent, bb, top_op, &rs1, &rs2);
        type_t *result_type = integer_binary_result_type(top_op, rs1, rs2);
        /* Constant folding for binary operations */
        if (rs1 && rs2 && rs1->is_const && !rs1->ptr_level && !rs1->is_global &&
            rs2->is_const && !rs2->ptr_level && !rs2->is_global &&
            !unsigned_int_operand(rs1) && !unsigned_int_operand(rs2) &&
            rs1->type->size <= TY_int->size &&
            rs2->type->size <= TY_int->size) {
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

            /* A shift count outside the promoted int width has no defined
             * result, and the compiler must not compute one in the host either:
             * leave such a shift to the target. Otherwise shift the unsigned
             * bit pattern, so a bit reaching the sign is not host undefined
             * behavior.
             */
            case OP_lshift:
                if (rs2->init_val < 0 || rs2->init_val >= 32)
                    folded = false;
                else
                    result =
                        (int) ((unsigned int) rs1->init_val << rs2->init_val);
                break;
            case OP_rshift:
                if (rs2->init_val < 0 || rs2->init_val >= 32)
                    folded = false;
                else
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
                vd->var_name = gen_name();
                vd->type = result_type;
                vd->is_const = true;
                vd->init_val = result;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);
            } else {
                /* Normal operation - folding failed or not supported */
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = result_type;
                opstack_push(vd);
                add_insn(parent, *bb, top_op, vd, rs1, rs2, 0, NULL);
            }
        } else {
            /* Normal operation */
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = result_type;
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

/* Nesting counter for read_expr(). The expression grammar descends recursively
 * through read_expr_operand(), so input nested deeply enough would run out of
 * machine stack before any diagnostic could be printed.
 */
int expr_depth = 0;

void read_expr(block_t *parent, basic_block_t **bb)
{
    expr_depth++;
    if (expr_depth > MAX_EXPR_DEPTH)
        error_at("Expression nesting too deep", cur_token_loc());
    read_expr_body(parent, bb);
    expr_depth--;
}


/* Whether @lvalue designates an integer object wider than int. The step and
 * result of ++ and -- on it must have its type: an int temporary kept only the
 * low word, which a 32-bit target stores back without a high word.
 */
static bool lvalue_is_wide_integer(const lvalue_t *lvalue)
{
    int level =
        lvalue->is_reference ? lvalue->value_ptr_level : lvalue->ptr_level;

    return !level && !lvalue->is_func && lvalue->type &&
           !lvalue->type->ptr_level && !is_record_type(lvalue->type) &&
           lvalue->type->size > TY_int->size;
}

/* Whether ++ and -- on @lvalue update a _Bool object. */
static bool lvalue_is_bool(const lvalue_t *lvalue)
{
    int level =
        lvalue->is_reference ? lvalue->value_ptr_level : lvalue->ptr_level;

    return !lvalue->is_func && is_bool_scalar(lvalue->type, level);
}

/* What follows a completed lvalue: pointer arithmetic on it, or an update or
 * read of the object it names. read_lvalue() finishes with this.
 */
static void lower_lvalue_tail(lvalue_t *lvalue,
                              var_t *var,
                              block_t *parent,
                              basic_block_t **bb,
                              opcode_t prefix_op,
                              bool allow_ptr_arith,
                              type_t *pointer_row_element_type)
{
    var_t *vd, *rs1, *rs2;

    /* Only handle pointer arithmetic if we have a pointer/array that hasn't
     * been dereferenced. After array indexing like arr[0], we have a value, not
     * a pointer.
     */
    bool pointee_row = is_pointee_array_pointer(var);
    if (allow_ptr_arith && lex_peek(T_plus, NULL) &&
        (var->ptr_level || is_array_declarator(var) || pointee_row) &&
        !lvalue->is_reference) {
        if (!is_array_declarator(var) &&
            is_direct_void_pointer_type(lvalue->type, lvalue->ptr_level))
            error_at("Pointer arithmetic on void* is invalid", cur_token_loc());
        while (lex_peek(T_plus, NULL) &&
               (var->ptr_level || is_array_declarator(var) || pointee_row)) {
            lex_expect(T_plus);
            if (lvalue->is_reference) {
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, rs1, NULL, lvalue->size,
                         NULL);
            }

            read_expr_operand(parent, bb);

            /* The element stepped over is the pointee. For a multi-level
             * pointer that pointee is itself a pointer, so the stride is
             * PTR_SIZE rather than the base type's width.
             */
            if (pointee_row) {
                lvalue->size = pointee_array_row_stride(var);
            } else if (is_array_declarator(var) && var->array_dim2 &&
                       !has_effective_pointer(var) && !var->is_func) {
                lvalue->size = fixed_array_decay_stride(var);
            } else if (var->ptr_level > 1 ||
                       (is_array_declarator(var) && var->ptr_level))
                lvalue->size = PTR_SIZE;
            else
                lvalue->size = lvalue->type->size;

            if (lvalue->size > 1) {
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->init_val = lvalue->size;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, vd, rs1, rs2, 0, NULL);
            }

            rs2 = opstack_pop();
            rs1 = opstack_pop();
            vd = require_var(parent);

            /* A pointer plus an integer is still a pointer. Array expressions
             * first decay to a pointer, adding one level to the declaration's
             * element indirection.
             */
            if (var->ptr_level || is_array_declarator(var) || pointee_row) {
                vd->type = lvalue->type;
                vd->ptr_level = var->ptr_level + !!is_array_declarator(var);
                if (pointee_row)
                    copy_pointee_array_shape(vd, var);
                else if (is_array_declarator(var) && var->array_dim2 &&
                         !has_effective_pointer(var) && !var->is_func) {
                    fixed_array_shape_t shape = fixed_array_shape_from_var(var);

                    fixed_array_shape_drop_outer(&shape);
                    fixed_array_shape_to_pointee_var(vd, &shape);
                }
            }
            vd->var_name = gen_name();
            vd->is_const_qualified = var->is_const_qualified;
            vd->pointer_const_mask = var->pointer_const_mask;
            vd->is_const_pointer =
                vd->ptr_level > 0 && vd->ptr_level <= 32 &&
                (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);
        }
    } else {
        /* Set and read only under 'is_reference'; the initializer says so to a
         * compiler that cannot correlate the two tests.
         */
        var_t *t = NULL;

        /* If operand is a reference, read the value and push to stack for the
         * incoming addition/subtraction. Otherwise, use the top element of
         * stack as the one of operands and the destination.
         */
        if (lvalue->is_reference) {
            rs1 = operand_stack[operand_stack_idx - 1];
            t = require_var(parent);
            t->var_name = gen_name();
            t->type = pointer_row_element_type
                          ? pointer_row_element_type
                          : pointee_type_from_pointer_typedef(lvalue->type);
            t->ptr_level = lvalue->value_ptr_level;
            if (pointer_row_element_type)
                t->is_const_qualified = lvalue->type->is_const_qualified;

            /* Retain a callback prototype even through a selected slot. A
             * direct loaded callback is callable, while unary `*` restores this
             * marker after consuming an extra object-pointer level.
             */
            t->func_signature = lvalue->type->func_signature;
            if (lvalue->pointee_func_signature) {
                /* `slots[i]` loads a callback slot, not a callable callback.
                 * Preserve the element's prototype until one unary `*` consumes
                 * this outer object-pointer level.
                 */
                t->ptr_level = 1;
                t->func_signature = NULL;
                t->pointee_func_signature = lvalue->pointee_func_signature;
                t->is_const_pointer = var->type->array_element_is_const_pointer;
                t->pointer_const_mask = t->is_const_pointer ? 1U : 0;
                t->is_volatile = var->type->array_element_is_volatile;
            }
            if (is_bitfield(lvalue->decl)) {
                /* Bit-fields are values, never independently addressable
                 * storage: extract their slice from the containing unit.
                 */
                t = read_bitfield_value(parent, bb, rs1, lvalue->decl);
                opstack_push(t);
                mark_value_reference(t, rs1, lvalue->decl);
            } else if (lvalue->is_func) {
                /* A function pointer typed by its record return type is a code
                 * address, loaded like any other pointer.
                 */
                add_insn(parent, *bb, OP_read, t, rs1, NULL, lvalue->size,
                         NULL);
                opstack_push(t);
                mark_value_reference(t, rs1, NULL);
            } else {
                /* A selected record is copied into storage of its own; a
                 * register holds no more than a pointer's worth of it.
                 */
                push_object_at(parent, bb, t, rs1, lvalue->size);
            }
        }
        if (prefix_op != OP_generic) {
            if ((prefix_op == OP_add || prefix_op == OP_sub) &&
                is_direct_void_pointer_type(lvalue->type, lvalue->ptr_level))
                error_at("Pointer arithmetic on void* is invalid",
                         cur_token_loc());
            vd = require_var(parent);
            vd->var_name = gen_name();

            /* For pointer arithmetic, increment by the size of pointed-to type
             */
            if (!lvalue->is_reference && is_pointee_array_pointer(var))
                vd->init_val = pointee_array_row_stride(var);
            else if (lvalue->ptr_level > 1)
                vd->init_val = PTR_SIZE;
            else if (lvalue->ptr_level)
                vd->init_val = lvalue->type->size;
            else if (lvalue->type && lvalue->type->ptr_level)
                vd->init_val = get_pointer_element_size(var);
            else
                vd->init_val = 1;
            if (lvalue_is_wide_integer(lvalue))
                vd->type = lvalue->type;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs2 = opstack_pop();
            if (lvalue->is_reference)
                rs1 = opstack_pop();
            else
                rs1 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            vd->var_name = gen_name();
            if (lvalue_is_wide_integer(lvalue))
                vd->type = lvalue->type;
            add_insn(parent, *bb, prefix_op, vd, rs1, rs2, 0, NULL);

            if (lvalue->is_reference) {
                rs1 = vd;
                vd = opstack_pop();

                /* A bit-field is updated inside its allocation unit, and the
                 * expression has the value the field then holds; a plain store
                 * of the sum overwrote the neighbouring fields.
                 */
                if (is_bitfield(lvalue->decl)) {
                    write_bitfield_value(parent, bb, vd, rs1, lvalue->decl);
                    rs1 = read_bitfield_value(parent, bb, vd, lvalue->decl);
                    rs1->is_bitfield = false;
                } else {
                    rs1 = convert_stored_value(parent, bb, rs1, t->type,
                                               t->ptr_level);

                    /* The column of arguments of the new insn of 'OP_write' is
                     * different from 'ph1_ir'
                     */
                    add_insn(parent, *bb, OP_write, NULL, vd, rs1, lvalue->size,
                             NULL);
                }
                /* Push the new value onto the operand stack */
                opstack_push(rs1);
            } else {
                rs1 = vd;
                vd = operand_stack[operand_stack_idx - 1];
                rs1 = resize_var(parent, bb, rs1, vd);
                mark_var_mutated(vd);
                add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
            }
        } else if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
            if (is_direct_void_pointer_type(lvalue->type, lvalue->ptr_level))
                error_at("Pointer arithmetic on void* is invalid",
                         cur_token_loc());

            /* Deferred postfix stores are plain OP_write instructions. A
             * bit-field instead needs a masked read-modify-write of its
             * containing allocation unit, so lower it here while retaining the
             * extracted pre-update value as the expression result.
             */
            if (lvalue->is_reference && is_bitfield(lvalue->decl)) {
                /* Consume the '--' as well: testing for '++' alone left it in
                 * the stream, so `bits.field--` failed to parse.
                 */
                opcode_t postfix_op = OP_sub;
                if (lex_accept(T_increment))
                    postfix_op = OP_add;
                else
                    lex_expect(T_decrement);
                var_t *old = opstack_pop();
                var_t *address = opstack_pop();
                var_t *one = bitfield_constant(parent, bb, 1);
                var_t *updated = require_var(parent);

                updated->var_name = gen_name();
                updated->type =
                    integer_binary_result_type(postfix_op, old, one);
                add_insn(parent, *bb, postfix_op, updated, old, one, 0, NULL);
                write_bitfield_value(parent, bb, address, updated,
                                     lvalue->decl);
                old->is_bitfield = false;
                opstack_push(old);
                return;
            }

            /* This arm appends up to five entries, so check for room once
             * before writing any of them.
             */
            if (se_idx + 5 > MAX_SIDE_EFFECT)
                error_at("Too many postfix operators in one statement",
                         next_token_loc());

            side_effect[se_idx].opcode = OP_load_constant;
            vd = require_var(parent);
            vd->var_name = gen_name();

            /* Calculate increment size based on pointer type */
            int increment_size = 1;
            if (!lvalue->is_reference && is_pointee_array_pointer(var)) {
                increment_size = pointee_array_row_stride(var);
            } else if (lvalue->ptr_level > 1 && !lvalue->is_reference) {
                increment_size = PTR_SIZE;
            } else if (lvalue->ptr_level && !lvalue->is_reference) {
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
            if (lvalue_is_wide_integer(lvalue))
                vd->type = lvalue->type;

            side_effect[se_idx].rd = vd;
            side_effect[se_idx].rs1 = NULL;
            side_effect[se_idx].rs2 = NULL;
            se_idx++;

            /* Consume whichever operator is actually there. Testing only for
             * '++' picks the right opcode but leaves a '--' in the stream, so
             * postfix decrement parsed only where the leftover token happened
             * to be harmless -- "i--;" as a statement worked, "a = i--" did
             * not.
             */
            opcode_t postfix_op = OP_sub;
            if (lex_accept(T_increment))
                postfix_op = OP_add;
            else
                lex_expect(T_decrement);
            side_effect[se_idx].opcode = postfix_op;
            side_effect[se_idx].rs2 = vd;
            if (lvalue->is_reference)
                side_effect[se_idx].rs1 = opstack_pop();
            else
                side_effect[se_idx].rs1 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            vd->var_name = gen_name();
            if (!lvalue->is_reference && is_pointee_array_pointer(var)) {
                vd->type = lvalue->type;
                vd->ptr_level = var->ptr_level;
                copy_pointee_array_shape(vd, var);
            } else if (lvalue_is_wide_integer(lvalue))
                vd->type = lvalue->type;
            side_effect[se_idx].rd = vd;
            se_idx++;

            /* The update converts back to the operand type, which for _Bool is
             * a comparison with zero: b++ leaves 1 and b-- on 0 leaves 1 (C99
             * 6.5.2.4). The deferred entries are raw instructions, so spell out
             * the comparison normalize_bool() would emit.
             */
            if (lvalue_is_bool(lvalue)) {
                var_t *zero = require_typed_var(parent, TY_int);
                var_t *truth = require_typed_var(parent, TY_bool);

                zero->var_name = gen_name();
                zero->init_val = 0;
                zero->is_const = true;
                side_effect[se_idx].opcode = OP_load_constant;
                side_effect[se_idx].rd = zero;
                side_effect[se_idx].rs1 = NULL;
                side_effect[se_idx].rs2 = NULL;
                se_idx++;

                truth->var_name = gen_name();
                side_effect[se_idx].opcode = OP_neq;
                side_effect[se_idx].rd = truth;
                side_effect[se_idx].rs1 = vd;
                side_effect[se_idx].rs2 = zero;
                se_idx++;
                vd = truth;
            }

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

/* Whether @var is a parameter of the function being parsed. A parameter
 * declared with an array declarator is adjusted to a pointer (C99 6.7.5.3), so
 * unlike an array it may be assigned.
 */
static bool is_function_parameter(const var_t *var, block_t *parent)
{
    if (!parent || !parent->func)
        return false;
    for (int i = 0; i < parent->func->num_params; i++) {
        var_t *param = &parent->func->param_defs[i];

        if (var == param || var->base == param)
            return true;
    }
    return false;
}

/* Return the address that an expression points to, or evaluate its value.
 *   x =;
 *   x[<expr>] =;
 *   x[expr].field =;
 *   x[expr]->field =;
 *
 * @allow_ptr_arith says whether a following "+ expr" belongs to this lvalue.
 * Normally it does, and the addend is scaled by the element size. The
 * dereference handlers pass false, because unary '*' binds tighter than '+': in
 * "*p + 1" the sum belongs to the enclosing expression, and reading it as
 * pointer arithmetic gives p[1] instead of one more than p[0]. It applies to
 * this lvalue alone -- an lvalue parsed further in, as a subscript or a call
 * argument, gets the normal behaviour from its own call.
 */
void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 basic_block_t **bb,
                 bool eval,
                 opcode_t prefix_op,
                 bool allow_ptr_arith)
{
    var_t *vd, *rs1, *rs2;
    var_t *loaded_scalar_row = NULL;
    type_t *pointer_row_element_type = NULL;
    bool pending_scalar_row_pointer_slot = false;
    bool pending_fixed_array_pointer_slot = false;
    bool is_address_got = false;
    bool is_member = false;
    int subscript_depth = 0;

    /* The decayed address of an array row selected by the latest subscript,
     * such as `m[1]` of `int m[2][3]`. NULL once the lvalue designates an
     * element or a member instead.
     */
    var_t *array_row = NULL;

    /* Whether the record holding the current member is const. A member, and an
     * element of an array member, is part of that record and shares its
     * qualification; the object a pointer member points to does not.
     */
    bool record_is_const = false;

    /* Callers pass a find_var() result, which is NULL for a name that was never
     * declared.
     */
    if (!var)
        error_at("Undeclared identifier", next_token_loc());

    /* C99 6.7.4 forbids an external-linkage inline definition from referencing
     * an identifier with internal linkage. File-scope `static` objects have
     * GLOBAL_BLOCK as their lexical owner; a static local has a function block
     * instead and is diagnosed at its definition below.
     */
    if (parent && parent->func && parent->func->is_inline &&
        !parent->func->is_static && var->is_static &&
        var->scope == GLOBAL_BLOCK)
        error_at(
            "external inline definition references internal-linkage object",
            next_token_loc());

    /* already peeked and have the variable */
    lex_expect(T_identifier);

    lvalue->type = var->type;
    lvalue->decl = var;
    lvalue->size = get_size(var);
    lvalue->ptr_level = var->ptr_level;
    lvalue->value_ptr_level = var->ptr_level + var->type->ptr_level;
    lvalue->is_func = var->is_func;
    lvalue->is_reference = false;
    lvalue->pointee_func_signature = NULL;

    /* A pointer hidden in a typedef keeps its depth on the type rather than the
     * declarator. Its outer const still makes the pointer object read-only,
     * just as for an explicitly spelled `T * const`.
     */
    lvalue->is_const_qualified =
        (var->ptr_level || (var->type && var->type->ptr_level) ||
         var->is_func || var->array_size || var->has_unsized_array)
            ? var->is_const_pointer
            : var->is_const_qualified;
    lvalue->pointer_const_mask = var->pointer_const_mask;

    opstack_push(var);

    if (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
        lex_peek(T_dot, NULL))
        lvalue->is_reference = true;

    while (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
           lex_peek(T_dot, NULL)) {
        array_row = NULL;
        if (lex_accept(T_open_square)) {
            int indexed_ptr_level;
            fixed_array_shape_t row_shape;
            bool indexes_fixed_array_pointer_slot;
            bool indexes_direct_pointee_array;
            bool indexes_scalar_pointee_row;
            bool indexes_loaded_scalar_pointee_row;
            bool indexes_pointee_row;

            /* if subscripted member's is not yet resolved, dereference to
             * resolve base address. e.g., dereference of "->" in "data->raw[0]"
             * would be performed here. The value depth counts the stars a
             * typedef hides, as in a `ptr_t p` member or `pp_t pp; pp[0][1]`.
             */
            if (lvalue->is_reference &&
                (lvalue->value_ptr_level || pending_fixed_array_pointer_slot) &&
                is_member) {
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                if (pending_scalar_row_pointer_slot) {
                    vd->type = var->type;
                    vd->ptr_level = 1;
                    copy_pointee_array_shape(vd, var);
                    loaded_scalar_row = vd;
                    pending_scalar_row_pointer_slot = false;
                } else if (pending_fixed_array_pointer_slot) {
                    /* A direct `int (*planes[])[rows][columns]` array selects a
                     * pointer slot before it selects a row. Load that slot once
                     * and retain its fixed pointee shape.
                     */
                    vd->type = var->type->pointee_array_element_type
                                   ? var->type->pointee_array_element_type
                                   : var->type;
                    vd->ptr_level = 1;
                    copy_pointee_array_shape(vd, var);
                    loaded_scalar_row = vd;
                    pending_fixed_array_pointer_slot = false;
                }
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, rs1, NULL, PTR_SIZE, NULL);

                /* The loaded value is the base for this index. A chain such as
                 * `int **p` still advances by pointer slots until its last
                 * indirection; a pointer-to-row then advances by base elements
                 * (or its preserved row extent below). An array of typedef
                 * pointers, as in `ptr_t rows[2]`, reaches here still holding
                 * the pointer alias, so step by its pointee instead.
                 */
                if (lvalue->value_ptr_level > 1) {
                    lvalue->size = PTR_SIZE;
                } else if (lvalue->type->ptr_level && !loaded_scalar_row &&
                           !lvalue->type->pointee_array_size) {
                    type_t *pointee =
                        pointee_type_from_pointer_typedef(lvalue->type);

                    lvalue->size =
                        pointer_typedef_pointee_size(lvalue->type, pointee);
                    lvalue->type = pointee;
                } else {
                    lvalue->size = lvalue->type->size;
                }
            }

            /* var must be either a pointer or an array of some type For typedef
             * pointers, check the type's ptr_level
             */
            bool is_typedef_pointer = (var->type && var->type->ptr_level > 0);
            if (var->ptr_level == 0 && !is_array_declarator(var) &&
                !is_typedef_pointer)
                error_at("Cannot apply square operator to non-pointer",
                         cur_token_loc());

            /* The selected value has one less indirection than the expression
             * being indexed. An array first decays to a pointer to its element;
             * after an earlier subscript, use that selected value as the next
             * indexing source.
             */
            if (subscript_depth)
                indexed_ptr_level = lvalue->value_ptr_level;
            else
                indexed_ptr_level = var->ptr_level + var->type->ptr_level +
                                    !!is_array_declarator(var);
            indexes_fixed_array_pointer_slot =
                !subscript_depth && is_direct_fixed_array_pointer_slot(var);
            indexes_direct_pointee_array =
                !subscript_depth && var->pointee_array_size > 0 &&
                var->pointee_array_element_ptr_level > 0 &&
                indexed_ptr_level == var->pointee_array_element_ptr_level + 1;

            /* `int (*p)[N]` selects an array row on its first subscript even
             * though that row's scalar elements have no pointer depth. Keep
             * this distinct from the pointer-element row case above: the row
             * decays to `int *` for exactly the following scalar subscript.
             */
            indexes_scalar_pointee_row =
                !subscript_depth && !is_array_declarator(var) &&
                var->pointee_array_size > 0 &&
                var->pointee_array_element_ptr_level == 0 &&
                effective_pointer_depth(var) == 1;
            indexes_loaded_scalar_pointee_row =
                loaded_scalar_row && subscript_depth == 1;
            indexes_pointee_row = indexes_direct_pointee_array ||
                                  indexes_scalar_pointee_row ||
                                  indexes_loaded_scalar_pointee_row;

            /* Selecting a row designates an array, which immediately decays
             * back to a pointer to its first element for a following postfix
             * subscript. Keep that element pointer depth instead of consuming
             * an indirection as though the row itself were a pointer object.
             */
            lvalue->value_ptr_level =
                indexes_pointee_row
                    ? indexed_ptr_level
                    : (indexed_ptr_level ? indexed_ptr_level - 1 : 0);

            /* if nested pointer, still pointer Also handle typedef pointers
             * which have ptr_level == 0
             */
            if (indexes_direct_pointee_array) {
                /* Preserve the row's base element separately. The row itself
                 * must retain its existing address lowering, but a following
                 * subscript reads a pointer element whose value cannot carry a
                 * lexical alias's outer row-pointer descriptor.
                 */
                pointer_row_element_type =
                    var->type->pointee_array_element_type
                        ? var->type->pointee_array_element_type
                        : var->type;
            } else if (!subscript_depth && is_array_declarator(var) &&
                       var->type->array_element_ptr_level > 0) {
                pointer_row_element_type = var->type->array_element_type
                                               ? var->type->array_element_type
                                               : var->type;
            } else if (indexes_scalar_pointee_row ||
                       indexes_loaded_scalar_pointee_row) {
                var_t *row_source =
                    indexes_loaded_scalar_pointee_row ? loaded_scalar_row : var;
                lvalue->type =
                    row_source->type->pointee_array_element_type
                        ? row_source->type->pointee_array_element_type
                        : row_source->type;
                lvalue->size = lvalue->type->size;
            } else if ((var->ptr_level <= 1 || is_typedef_pointer) &&
                       !is_array_declarator(var)) {
                /* For typedef pointers, get the size of the base type that the
                 * pointer points to
                 */
                if (lvalue->type->ptr_level > 0) {
                    type_t *pointee =
                        pointee_type_from_pointer_typedef(lvalue->type);

                    /* void pointers retain the existing byte-stride extension;
                     * every other typedef pointer advances by its actual
                     * pointee, including a tagged record reached through a
                     * typedef alias.
                     */
                    lvalue->size =
                        pointer_typedef_pointee_size(lvalue->type, pointee);
                    lvalue->type = pointee;
                } else {
                    lvalue->size = lvalue->type->size;
                }

                /* The pointee type above has every typedef star stripped. When
                 * the selected element is itself a pointer, as for `typedef int
                 * **ipp_t`, it still occupies a pointer slot.
                 */
                if (lvalue->value_ptr_level > 0)
                    lvalue->size = PTR_SIZE;
            }

            read_expr(parent, bb);

            /* Multiply by the remaining element extent. */
            int multiplier = lvalue->size;

            if (subscript_depth == 0 && var->array_dim2 > 0) {
                multiplier = fixed_array_subscript_stride(var, subscript_depth,
                                                          lvalue->size);
            } else if (subscript_depth == 0 && var->pointee_array_size > 0 &&
                       indexes_pointee_row) {
                multiplier = fixed_pointee_array_subscript_stride(
                    var, subscript_depth, lvalue->size);
            } else if (indexes_loaded_scalar_pointee_row) {
                multiplier = fixed_pointee_array_subscript_stride(
                    loaded_scalar_row, 0, lvalue->size);
            } else if (loaded_scalar_row) {
                multiplier = fixed_pointee_array_subscript_stride(
                    loaded_scalar_row, subscript_depth - 1, lvalue->size);
            } else if (subscript_depth >= 1 && var->pointee_array_size > 0) {
                multiplier = fixed_pointee_array_subscript_stride(
                    var, subscript_depth, lvalue->size);
            } else if ((subscript_depth == 1 && var->array_dim3 > 0) ||
                       (subscript_depth == 2 && var->array_dim4 > 0)) {
                multiplier = fixed_array_subscript_stride(var, subscript_depth,
                                                          lvalue->size);
            }

            if (multiplier != 1) {
                vd = require_var(parent);
                vd->init_val = multiplier;
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, vd, rs1, rs2, 0, NULL);
            }

            rs2 = opstack_pop();
            rs1 = opstack_pop();
            vd = require_var(parent);

            /* A subscript expression computes the address of its selected
             * element. Preserve that pointer provenance: unary '&' leaves an
             * already-addressable subscript alone, and pointer subtraction must
             * still know whether this is an int or struct element.
             */
            vd->type = lvalue->type;

            /* The computed address points at the selected element. If that
             * element is itself a pointer (for example, `int *items[]`), its
             * value indirection remains inside the address type: `&items[i]` is
             * `int **`, not `int *`.
             */
            vd->ptr_level = lvalue->ptr_level + 1;
            vd->var_name = gen_name();
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);

            lex_expect(T_close_square);
            is_address_got = true;

            /* Fewer subscripts than the array's rank select a row, an array
             * that decays to a pointer to its first element (C99 6.3.2.1)
             * rather than an object to load. The computed address is already
             * that pointer; a row of rows keeps its remaining bounds as the
             * pointee shape, so `m[1]` of `int m[2][2][3]` is `int (*)[3]`.
             */
            row_shape = fixed_array_shape_from_var(var);
            if (is_array_declarator(var) && !indexes_pointee_row &&
                !loaded_scalar_row && subscript_depth + 1 < row_shape.rank) {
                for (int i = 0; i < subscript_depth + 2; i++)
                    fixed_array_shape_drop_outer(&row_shape);
                if (row_shape.rank) {
                    fixed_array_shape_to_pointee_var(vd, &row_shape);
                    vd->pointee_array_element_ptr_level = var->ptr_level;
                }
                vd->is_const_qualified = var->is_const_qualified;
                lvalue->value_ptr_level = indexed_ptr_level;
                array_row = vd;
            }

            /* A following subscript loads only when this selected element is
             * itself a pointer (for example, `int **p; p[0][1]`). A
             * pointer-to-array selects a row, not a pointer object, so treating
             * every subscript as a member would dereference row data on its
             * next index.
             */
            is_member = !indexes_pointee_row && !array_row &&
                        lvalue->value_ptr_level > 0;
            subscript_depth++;
            lvalue->is_reference = !indexes_pointee_row && !array_row;
            if (subscript_depth == 1 && var->pointee_array_size > 0 &&
                var->pointee_array_element_ptr_level == 0 &&
                effective_pointer_depth(var) == 2)
                pending_scalar_row_pointer_slot = true;
            if (indexes_fixed_array_pointer_slot)
                pending_fixed_array_pointer_slot = true;
            if (loaded_scalar_row) {
                fixed_array_shape_t loaded_shape =
                    fixed_array_shape_from_pointee_var(loaded_scalar_row);

                if (subscript_depth >= 1 + loaded_shape.rank)
                    loaded_scalar_row = NULL;
            }

            /* A multidimensional slot array carries the slot descriptor on the
             * whole array. Only its final subscript selects an element; earlier
             * subscripts select rows that decay for the next index.
             */
            if (var->type->array_element_pointee_func_signature) {
                int array_dims = 1 + !!var->array_dim2 + !!var->array_dim3 +
                                 !!var->array_dim4;

                if (subscript_depth == array_dims)
                    lvalue->pointee_func_signature =
                        var->type->array_element_pointee_func_signature;
            }

            /* A subscript designates the pointee, whose qualification is the
             * declaration's base qualification rather than `* const`.
             */
            lvalue->is_const_qualified =
                var->is_const_qualified ||
                (lvalue->pointee_func_signature &&
                 var->type->array_element_is_const_pointer) ||
                (record_is_const && is_array_declarator(var));
        } else {
            char token[MAX_ID_LEN];

            if (lex_accept(T_arrow)) {
                /* The record is the pointee, qualified by the base of the
                 * pointer's declaration however the pointer itself is.
                 */
                record_is_const = var->is_const_qualified;

                /* resolve where the pointer points at from the calculated
                 * address in a structure.
                 */
                if (is_member) {
                    rs1 = opstack_pop();
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_read, vd, rs1, NULL, PTR_SIZE,
                             NULL);
                }
            } else {
                lex_expect(T_dot);
                record_is_const = lvalue->is_const_qualified;

                if (!is_address_got) {
                    rs1 = opstack_pop();
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_address_of, vd, rs1, NULL, 0,
                             NULL);

                    is_address_got = true;
                }
            }

            lex_ident(T_identifier, token);

            /* change type currently pointed to */
            var = find_member(token, lvalue->type);
            if (!var)
                error_at("Unknown struct or union member", next_token_loc());
            lvalue->type = var->type;
            lvalue->decl = var;

            /* The member, not the pointer row element it was selected from, now
             * types the value that is read.
             */
            pointer_row_element_type = NULL;
            lvalue->ptr_level = var->ptr_level;
            lvalue->value_ptr_level = var->ptr_level + var->type->ptr_level;
            lvalue->is_func = var->is_func;
            lvalue->size = get_size(var);

            /* As for a declared object, a pointer or array member is itself
             * const only through its outer qualifier.
             */
            lvalue->is_const_qualified =
                record_is_const ||
                ((var->ptr_level || var->type->ptr_level || var->is_func ||
                  var->array_size || var->has_unsized_array)
                     ? var->is_const_pointer
                     : var->is_const_qualified);
            subscript_depth = 0;

            /* if it is an array, get the address of first element instead of
             * its value. Any other member is an object to read, even when the
             * base was a row that decayed to its address, as in `rows[1]->x`.
             */
            lvalue->is_reference = !is_array_declarator(var);

            /* move pointer to offset of structure */
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->init_val = var->offset;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs2 = opstack_pop();
            rs1 = opstack_pop();
            vd = require_var(parent);
            vd->var_name = gen_name();

            /* A one-dimensional array member designates its first element's
             * address. Type it as that pointer, so the value can still be
             * subscripted after a grouping: `(record.items)[1]`.
             */
            if (is_array_declarator(var) && !var->array_dim2) {
                vd->type = var->type;
                vd->ptr_level = var->ptr_level + 1;
            }
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);

            is_address_got = true;
            is_member = true;
        }
    }

    lvalue->subscript_depth = subscript_depth;
    lvalue->is_array =
        array_row ||
        (!lvalue->is_reference && is_array_declarator(lvalue->decl) &&
         !lvalue->decl->is_func &&
         !is_function_parameter(lvalue->decl, parent));
    if (lvalue->is_array && lvalue_write_follows(prefix_op))
        error_at("assignment to expression with array type", next_token_loc());
    if (!lvalue->is_array && !lvalue->value_ptr_level &&
        is_record_type(lvalue->type) &&
        (prefix_op != OP_generic || lex_peek(T_increment, NULL) ||
         lex_peek(T_decrement, NULL)))
        error_at("Operand of record type requires a scalar value",
                 next_token_loc());

    if (!eval)
        return;

    if (lvalue->is_const_qualified &&
        (prefix_op != OP_generic || lex_peek(T_increment, NULL) ||
         lex_peek(T_decrement, NULL)))
        error_at("assignment of read-only location", next_token_loc());

    lower_lvalue_tail(lvalue, array_row ? array_row : var, parent, bb,
                      prefix_op, allow_ptr_arith, pointer_row_element_type);
}

void read_logical(opcode_t op, block_t *parent, basic_block_t **bb)
{
    var_t *vd;

    if (op != OP_log_and && op != OP_log_or)
        error_at("encounter an invalid logical opcode in read_logical()",
                 cur_token_loc());

    /* Test the operand before the logical-and/or operator */
    vd = opstack_pop();
    reject_record_operand(vd);
    vd = materialize_function_designator(parent, bb, vd);
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
         * If handling the expression, the basic blocks will connect to each
         * other as the following illustration:
         *
         * bb1 bb2 bb3
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
         * In this case, finalize_logical() should add some instructions to bb2
         * ~ bb5 and properly connect them to each other.
         *
         * Notice that
         * - bb1 has been handled by read_logical().
         * - bb2 is equivalent to '*bb'.
         * - bb3 needs to be created.
         * - bb4 is 'shared_bb'.
         * - bb5 needs to be created.
         *
         * Thus, here uses 'then', 'then_next', 'else_bb' and 'end' to
         * respectively point to bb2 ~ bb5. Subsequently, perform the mentioned
         * operations for finalizing.
         */
        then = *bb;
        then_next = bb_create(parent);
        else_bb = shared_bb;
        bb_connect(then, then_next, THEN);
        bb_connect(then, else_bb, ELSE);
        bb_connect(then_next, end, NEXT);
    } else if (op == OP_log_or) {
        /* For example: a || b
         *
         * Similar to handling logical-and operations, it should add some
         * instructions to the basic blocks and connect them to each other for
         * logical-or operations as in the figure:
         *
         * bb1 bb2 bb3
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
         * Similarly, here uses 'else_if', 'else_bb', 'then' and 'end' to
         * respectively point to bb2 ~ bb5, and then finishes the finalization.
         */
        then = shared_bb;
        else_if = *bb;
        else_bb = bb_create(parent);
        bb_connect(else_if, then, THEN);
        bb_connect(else_if, else_bb, ELSE);
        bb_connect(then, end, NEXT);
    } else
        error_at("encounter an invalid logical opcode in finalize_logical()",
                 cur_token_loc());
    bb_connect(else_bb, end, NEXT);

    /* Create the branch instruction for final logical-and/or operand */
    vd = opstack_pop();
    reject_record_operand(vd);
    add_insn(parent, op == OP_log_and ? then : else_if, OP_branch, NULL, vd,
             NULL, 0, NULL);

    /* If handling logical-and operation, here creates a true branch for the
     * logical-and operation and assigns a true value.
     *
     * Otherwise, create a false branch and assign a false value for logical-or
     * operation.
     */
    vd = require_var(parent);
    vd->var_name = gen_name();
    vd->init_val = op == OP_log_and;
    add_insn(parent, op == OP_log_and ? then_next : else_bb, OP_load_constant,
             vd, NULL, NULL, 0, NULL);

    log_op_res = require_var(parent);
    log_op_res->var_name = gen_name();
    add_insn(parent, op == OP_log_and ? then_next : else_bb, OP_assign,
             log_op_res, vd, NULL, 0, NULL);

    /* After assigning a value, go to the final basic block, this is done by BB
     * fallthrough.
     */

    /* Create the shared branch and assign the other value for the other
     * condition of a logical-and/or operation.
     *
     * If handing a logical-and operation, assign a false value. else, assign a
     * true value for a logical-or operation.
     */
    vd = require_var(parent);
    vd->var_name = gen_name();
    vd->init_val = op != OP_log_and;
    add_insn(parent, op == OP_log_and ? else_bb : then, OP_load_constant, vd,
             NULL, NULL, 0, NULL);

    add_insn(parent, op == OP_log_and ? else_bb : then, OP_assign, log_op_res,
             vd, NULL, 0, NULL);

    log_op_res->is_logical_ret = true;
    opstack_push(log_op_res);

    bb[0] = end;
}

/* Parse the full expression grammar used by control statements. This keeps
 * comma sequencing and conditional expressions consistent across if, loops, and
 * switch while argument and initializer lists retain their own delimiter rules.
 */
void read_control_expression(block_t *parent, basic_block_t **bb)
{
    if (!read_assignment_expression(parent, bb)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
    }

    while (lex_accept(T_comma)) {
        opstack_pop();
        perform_side_effect(parent, *bb);
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
    }

    /* A function designator in a controlling expression decays to its code
     * pointer before OP_branch tests it. Raw symbols intentionally have no
     * allocated value, so branching on one directly can observe zero or an
     * unrelated register instead of C99's non-null function address.
     */
    var_t *result = opstack_pop();
    result = materialize_function_designator(parent, bb, result);
    opstack_push(result);
}

/* Expression statements use the same full-expression grammar as controls: parse
 * an assignment or conditional expression, sequence top-level commas, then
 * discard only the final value after its side effects have been emitted.
 */
basic_block_t *read_full_expression_statement(block_t *parent,
                                              basic_block_t *bb)
{
    read_control_expression(parent, &bb);
    opstack_pop();
    perform_side_effect(parent, bb);
    lex_expect(T_semicolon);
    return bb;
}

/* This is deliberately narrower than expression assignment recognition: a
 * grouped pointer-to-row store has no general lvalue representation yet.
 */
static bool grouped_scalar_pointee_row_store_starts(block_t *parent)
{
    token_t *token = cur_token ? cur_token->next : NULL;
    int depth = 0;

    if (!token || token->kind != T_open_bracket || !(token = token->next) ||
        token->kind != T_asterisk || !(token = token->next) ||
        token->kind != T_identifier ||
        !names_scalar_row_pointer(token, parent) || !(token = token->next) ||
        token->kind != T_close_bracket || !(token = token->next) ||
        token->kind != T_open_square)
        return false;
    for (; token; token = token->next) {
        if (token->kind == T_open_square || token->kind == T_open_bracket ||
            token->kind == T_open_curly)
            depth++;
        else if (token->kind == T_close_square ||
                 token->kind == T_close_bracket ||
                 token->kind == T_close_curly) {
            if (!--depth)
                break;
        }
    }
    if (!token || !(token = token->next) ||
        (token->kind != T_assign && token->kind != T_pluseq &&
         token->kind != T_minuseq))
        return false;
    for (depth = 0, token = token->next; token; token = token->next) {
        if (token->kind == T_open_square || token->kind == T_open_bracket ||
            token->kind == T_open_curly)
            depth++;
        else if (token->kind == T_close_square ||
                 token->kind == T_close_bracket || token->kind == T_close_curly)
            depth--;
        else if (!depth && token->kind == T_comma)
            return false;
        else if (!depth && token->kind == T_semicolon)
            return true;
    }
    return false;
}

static basic_block_t *handle_grouped_scalar_pointee_row_store(block_t *parent,
                                                              basic_block_t *bb)
{
    char name[MAX_VAR_LEN];
    var_t *source, *row, *index, *address, *value;

    lex_expect(T_open_bracket);
    lex_expect(T_asterisk);
    lex_ident(T_identifier, name);
    source = find_var(name, parent);
    if (!source)
        error_at("Undeclared identifier", cur_token_loc());
    if (!lower_pointee_array_dereference(source, parent, &bb))
        error_at("Grouped pointer-to-array store requires a scalar fixed row",
                 cur_token_loc());
    row = opstack_pop();
    if (row->is_const_qualified)
        error_at("assignment of read-only location", cur_token_loc());
    lex_expect(T_close_bracket);
    lex_expect(T_open_square);
    if (!read_assignment_expression(parent, &bb)) {
        read_expr(parent, &bb);
        read_ternary_operation(parent, &bb);
    }
    index = opstack_pop();
    lex_expect(T_close_square);
    if (row->type->size != 1) {
        var_t *scale = require_var(parent);
        var_t *scaled = require_var(parent);
        scale->var_name = gen_name();
        scale->init_val = row->type->size;
        add_insn(parent, bb, OP_load_constant, scale, NULL, NULL, 0, NULL);
        scaled->var_name = gen_name();
        add_insn(parent, bb, OP_mul, scaled, index, scale, 0, NULL);
        index = scaled;
    }
    address = require_typed_ptr_var(parent, row->type, 1);
    address->var_name = gen_name();
    add_insn(parent, bb, OP_add, address, row, index, 0, NULL);
    opcode_t compound_op = OP_generic;
    if (lex_accept(T_pluseq))
        compound_op = OP_add;
    else if (lex_accept(T_minuseq))
        compound_op = OP_sub;
    else
        lex_expect(T_assign);
    if (!read_assignment_expression(parent, &bb)) {
        read_expr(parent, &bb);
        read_ternary_operation(parent, &bb);
    }
    value = opstack_pop();
    if (compound_op != OP_generic) {
        if (value->is_func || is_pointer_like_value(value) ||
            is_record_type(value->type))
            error_at("Invalid compound assignment operand", cur_token_loc());
        var_t *current = require_typed_var(parent, row->type);
        current->var_name = gen_name();
        add_insn(parent, bb, OP_read, current, address, NULL, row->type->size,
                 NULL);
        current = integer_promote_operand(parent, &bb, current);
        value = integer_promote_operand(parent, &bb, value);
        normalize_integer_binary_operands(parent, &bb, compound_op, &current,
                                          &value);
        var_t *updated = require_var(parent);
        updated->var_name = gen_name();
        updated->type = integer_binary_result_type(compound_op, current, value);
        add_insn(parent, bb, compound_op, updated, current, value, 0, NULL);
        value = resize_to(parent, &bb, updated, row->type, 0);
    }
    value = convert_stored_value(parent, &bb, value, row->type, 0);
    add_insn(parent, bb, OP_write, NULL, address, value, row->type->size, NULL);
    lex_expect(T_semicolon);
    return bb;
}

/* The middle operand of ?: is an expression, rather than merely an
 * assignment-expression, so top-level commas belong to the selected true
 * branch. Keep this small sequencing layer here until all full-expression entry
 * points share the core comma grammar.
 */
void read_conditional_true_expression(block_t *parent, basic_block_t **bb)
{
    if (!read_assignment_expression(parent, bb)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
    }

    while (lex_accept(T_comma)) {
        opstack_pop();
        perform_side_effect(parent, *bb);
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
    }
}

/* An integer constant expression with value zero is the sole scalar that can
 * form a conditional pointer expression. Keep this small predicate local to
 * `?:`: ordinary pointer conversions have their own qualifier diagnostics.
 */
bool is_null_pointer_constant(var_t *value)
{
    return value && value->is_const && !is_pointer_like_value(value) &&
           !value->is_func && !value->init_val && !value->init_val_hi;
}

void read_ternary_operation(block_t *parent, basic_block_t **bb)
{
    var_t *vd;

    if (!lex_accept(T_question))
        return;

    /* ternary-operator */
    vd = opstack_pop();
    reject_record_operand(vd);
    add_insn(parent, *bb, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    basic_block_t *end_ternary = bb_create(parent);
    basic_block_t *then_entry = then_;
    basic_block_t *else_entry = else_;

    /* true branch */
    read_conditional_true_expression(parent, &then_);
    bb_connect(*bb, then_entry, THEN);

    if (!lex_accept(T_colon)) {
        /* ternary operator in standard C needs three operands */
        error_at("Expected ':' in conditional expression", next_token_loc());
    }

    var_t *true_val = opstack_pop();

    /* false branch */
    if (!read_assignment_expression(parent, &else_))
        read_expr(parent, &else_);

    /* The third operand has conditional-expression grammar, making nested
     * conditionals right-associative: a ? b : c ? d : e.
     */
    read_ternary_operation(parent, &else_);
    bb_connect(*bb, else_entry, ELSE);
    var_t *false_val = opstack_pop();

    /* A function designator decays to a pointer in each conditional operand.
     * Raw function symbols have no storage/defining IR, so materialize both
     * branch values before the join assigns the selected callback. This is the
     * same representation used by function-pointer assignment, casts, and
     * returns; delaying it until after the join can leave OP_assign carrying a
     * symbol with no allocated value and make a later indirect call jump to
     * garbage.
     */
    true_val = materialize_function_designator(parent, &then_, true_val);
    false_val = materialize_function_designator(parent, &else_, false_val);
    func_t *true_signature = get_func_signature(true_val);
    func_t *false_signature = get_func_signature(false_val);
    func_t *true_pointee_signature = true_val->pointee_func_signature;
    func_t *false_pointee_signature = false_val->pointee_func_signature;
    bool true_array = is_array_literal_placeholder(true_val);
    bool false_array = is_array_literal_placeholder(false_val);
    bool true_ptr_like = is_pointer_like_value(true_val);
    bool false_ptr_like = is_pointer_like_value(false_val);

    /* The ternary result must look like whichever side is pointer-like. If the
     * "true" expression is still a raw array literal but the "false" side is a
     * plain scalar, materialize the literal now so both branches produce
     * comparable scalar SSA values.
     */
    true_val = scalarize_array_literal_if_needed(
        parent, &then_, true_val, false_val ? false_val->type : NULL,
        true_array && !false_ptr_like);

    /* Apply the same conversion symmetrically when only the false branch is a
     * literal array. This prevents OP_assign from trying to move array storage
     * into a scalar destination later in code generation.
     */
    false_val = scalarize_array_literal_if_needed(
        parent, &else_, false_val, true_val ? true_val->type : NULL,
        false_array && !true_ptr_like);

    if (is_record_object(true_val) || is_record_object(false_val)) {
        var_t *true_addr;
        var_t *false_addr;
        var_t *selected;

        if (!is_record_object(true_val) || !is_record_object(false_val) ||
            size_var(true_val) != size_var(false_val))
            error_at("Conditional record operands must have the same type",
                     cur_token_loc());

        /* A record operand is an object, not a register value, so joining the
         * two operands as scalars would keep only a truncated prefix. Select
         * the operand's address instead and copy the chosen object into a
         * temporary after the join, which dominates every later use.
         */
        true_addr = require_ref_var(parent, true_val->type, 0);
        false_addr = require_ref_var(parent, false_val->type, 0);
        selected = require_ref_var(parent, true_val->type, 0);
        true_addr->var_name = gen_name();
        false_addr->var_name = gen_name();
        selected->var_name = gen_name();
        add_insn(parent, then_, OP_address_of, true_addr, true_val, NULL, 0,
                 NULL);
        add_insn(parent, else_, OP_address_of, false_addr, false_val, NULL, 0,
                 NULL);
        add_insn(parent, then_, OP_assign, selected, true_addr, NULL, 0, NULL);
        add_insn(parent, else_, OP_assign, selected, false_addr, NULL, 0, NULL);
        bb_connect(then_, end_ternary, NEXT);
        bb_connect(else_, end_ternary, NEXT);

        vd = require_typed_var(parent, true_val->type);
        vd->var_name = gen_name();
        add_insn(parent, end_ternary, OP_allocat, vd, NULL, NULL, 0, NULL);
        emit_record_copy_from_address(parent, &end_ternary, vd, selected);
        opstack_push(vd);
        bb[0] = end_ternary;
        return;
    }

    vd = require_var(parent);
    vd->var_name = gen_name();
    if (true_pointee_signature || false_pointee_signature) {
        func_t *signature = true_pointee_signature ? true_pointee_signature
                                                   : false_pointee_signature;
        var_t *pointer_value = true_pointee_signature ? true_val : false_val;
        var_t *other_value = true_pointee_signature ? false_val : true_val;

        if ((true_pointee_signature && false_pointee_signature &&
             !compatible_function_signature(true_pointee_signature,
                                            false_pointee_signature)) ||
            (!other_value->pointee_func_signature &&
             !is_null_pointer_constant(other_value)))
            error_at("Conditional callback slots must be compatible or null",
                     cur_token_loc());

        /* A matching conditional slot remains non-callable until dereferenced,
         * so preserve its pointee signature rather than presenting the outer
         * pointer as a function pointer.
         */
        vd->type = pointer_value->type;
        vd->ptr_level = pointer_value->ptr_level;
        vd->pointee_func_signature = signature;
    } else if (true_signature || false_signature) {
        func_t *signature = true_signature ? true_signature : false_signature;
        var_t *pointer_value = true_signature ? true_val : false_val;
        var_t *other_value = true_signature ? false_val : true_val;

        if ((true_signature && false_signature &&
             !compatible_function_signature(true_signature, false_signature)) ||
            (!get_func_signature(other_value) &&
             !is_null_pointer_constant(other_value)))
            error_at("Conditional function pointers must be compatible or null",
                     cur_token_loc());

        /* The branch join is itself a function-pointer value: retain the
         * selected callback's pointer representation and prototype so a postfix
         * call on `(condition ? first : second)` lowers indirectly.
         */
        vd->type = pointer_value->type;
        vd->ptr_level = 1;
        vd->func_signature = signature;
    } else if (!true_ptr_like && !false_ptr_like && !true_val->ptr_level &&
               !false_val->ptr_level) {
        true_val = integer_promote_operand(parent, &then_, true_val);
        false_val = integer_promote_operand(parent, &else_, false_val);
        vd->type = integer_binary_result_type(OP_add, true_val, false_val);
        true_val = resize_to(parent, &then_, true_val, vd->type, 0);
        false_val = resize_to(parent, &else_, false_val, vd->type, 0);
    }
    add_insn(parent, then_, OP_assign, vd, true_val, NULL, 0, NULL);
    add_insn(parent, else_, OP_assign, vd, false_val, NULL, 0, NULL);

    /* Recursive conditionals advance then_/else_ to their completed branch
     * tails. Join those tails, not the original entries, to the outer end.
     */
    bb_connect(then_, end_ternary, NEXT);
    bb_connect(else_, end_ternary, NEXT);

    var_t *array_ref = NULL;
    if (is_array_literal_placeholder(true_val))
        array_ref = true_val;
    else if (is_array_literal_placeholder(false_val))
        array_ref = false_val;

    if (array_ref) {
        vd->array_size = array_ref->array_size;
        vd->init_val = array_ref->init_val;
        vd->type = array_ref->type;
    }

    vd->is_ternary_ret = true;
    opstack_push(vd);
    bb[0] = end_ternary;
}

bool read_body_assignment(char *token,
                          block_t *parent,
                          opcode_t prefix_op,
                          basic_block_t **bb,
                          var_t **assignment_result,
                          int lvalue_paren_depth)
{
    var_t *var = find_local_var(token, parent), *vd, *rs1, *rs2, *t;
    if (!var)
        var = find_global_var(token);
    if (assignment_result)
        assignment_result[0] = NULL;

    if (var) {
        int one = 0;
        opcode_t op = OP_generic;
        lvalue_t lvalue;
        int size = 0;

        /* has memory address that we want to set */
        read_lvalue(&lvalue, var, parent, bb, false, OP_generic, true);
        while (lvalue_paren_depth-- > 0)
            lex_expect(T_close_bracket);
        size = lvalue.size;

        if (lvalue.is_array && lvalue_write_follows(prefix_op))
            error_at("assignment to expression with array type",
                     next_token_loc());
        if (lvalue.is_const_qualified && lvalue_write_follows(prefix_op))
            error_at(lvalue.is_reference ? "assignment of read-only location"
                                         : "assignment of read-only variable",
                     next_token_loc());

        if (lex_accept(T_increment)) {
            op = OP_add;
            one = 1;
        } else if (lex_accept(T_decrement)) {
            op = OP_sub;
            one = 1;
        } else if (accept_compound_assign_op(&op)) {
            /* op now holds the arithmetic the operator applies */
        } else if (lex_peek(T_open_bracket, NULL)) {
            /* Dereference lvalue first if lvalue is a member access; otherwise,
             * pass the function pointer value on the stack to
             * read_indirect_call.
             */
            if (lvalue.is_reference) {
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, rs1, NULL, PTR_SIZE, NULL);
            }

            emit_indirect_call_result(lvalue.decl,
                                      get_func_signature(lvalue.decl), false,
                                      parent, bb);
            return true;
        } else if (prefix_op == OP_generic) {
            lex_expect(T_assign);
        } else {
            op = prefix_op;
            one = 1;
        }

        if (op != OP_generic) {
            int increment_size = 1;

            if ((op == OP_add || op == OP_sub) &&
                is_direct_void_pointer_type(lvalue.type, lvalue.ptr_level))
                error_at("Pointer arithmetic on void* is invalid",
                         cur_token_loc());

            /* if we have a pointer, shift it by element size But not if we are
             * operating on a dereferenced value (array indexing)
             */
            if (!one && (op == OP_add || op == OP_sub) &&
                !lvalue.is_reference && is_pointee_array_pointer(var))
                increment_size = pointee_array_row_stride(var);
            else if (lvalue.ptr_level > 1 && !lvalue.is_reference)
                increment_size = PTR_SIZE;
            else if (lvalue.ptr_level && !lvalue.is_reference)
                increment_size = lvalue.type->size;
            /* Also check for typedef pointers which have is_ptr == 0 */
            else if (!lvalue.is_reference && lvalue.type &&
                     lvalue.type->ptr_level > 0) {
                /* Keep typedef-hidden depth: a void ** alias advances over
                 * pointer objects, whereas a direct void * was rejected above
                 * and never reaches this scaling path.
                 */
                increment_size = get_pointer_element_size(var);
            }

            /* If operand is a reference, read the value and push to stack for
             * the incoming addition/subtraction. Otherwise, use the top element
             * of stack as the one of operands and the destination.
             */
            if (one == 1) {
                if (lvalue.is_reference) {
                    t = opstack_pop();
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    vd->type = pointee_type_from_pointer_typedef(lvalue.type);
                    vd->ptr_level = lvalue.value_ptr_level;
                    opstack_push(vd);
                    if (is_bitfield(lvalue.decl)) {
                        opstack_pop();
                        vd = read_bitfield_value(parent, bb, t, lvalue.decl);
                        opstack_push(vd);
                    } else {
                        add_insn(parent, *bb, OP_read, vd, t, NULL, lvalue.size,
                                 NULL);
                    }
                } else
                    t = operand_stack[operand_stack_idx - 1];

                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->init_val = increment_size;
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = vd;
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                add_insn(parent, *bb, op, vd, rs1, rs2, 0, NULL);

                if (lvalue.is_reference) {
                    if (is_bitfield(lvalue.decl))
                        write_bitfield_value(parent, bb, t, vd, lvalue.decl);
                    else {
                        vd = convert_stored_value(parent, bb, vd, lvalue.type,
                                                  lvalue.value_ptr_level);
                        add_insn(parent, *bb, OP_write, NULL, t, vd, size,
                                 NULL);
                    }
                } else {
                    vd = resize_var(parent, bb, vd, t);
                    mark_var_mutated(t);
                    add_insn(parent, *bb, OP_assign, t, vd, NULL, 0, NULL);
                }
                if (assignment_result) {
                    if (!lvalue.is_reference) {
                        assignment_result[0] = vd;
                    } else if (is_bitfield(lvalue.decl)) {
                        var_t *stored =
                            read_bitfield_value(parent, bb, t, lvalue.decl);
                        stored->is_bitfield = false;
                        assignment_result[0] = stored;
                    } else {
                        var_t *stored = require_typed_var(parent, lvalue.type);
                        stored->ptr_level = lvalue.value_ptr_level;
                        stored->var_name = gen_name();
                        add_insn(parent, *bb, OP_read, stored, t, NULL,
                                 lvalue.size, NULL);
                        assignment_result[0] = stored;
                    }
                }
            } else {
                if (lvalue.is_reference) {
                    t = opstack_pop();
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    vd->type = pointee_type_from_pointer_typedef(lvalue.type);
                    vd->ptr_level = lvalue.value_ptr_level;
                    opstack_push(vd);
                    if (is_bitfield(lvalue.decl)) {
                        opstack_pop();
                        vd = read_bitfield_value(parent, bb, t, lvalue.decl);
                        opstack_push(vd);
                    } else {
                        add_insn(parent, *bb, OP_read, vd, t, NULL, lvalue.size,
                                 NULL);
                    }
                } else
                    t = operand_stack[operand_stack_idx - 1];

                if (!read_assignment_expression(parent, bb)) {
                    read_expr(parent, bb);

                    /* read_expr stops before `?`; compound assignment has the
                     * same conditional-expression RHS grammar as ordinary
                     * assignment.
                     */
                    read_ternary_operation(parent, bb);
                }

                var_t *rhs_val = opstack_pop();
                reject_record_operand(rhs_val);
                rhs_val = scalarize_array_literal_if_needed(
                    parent, bb, rhs_val, lvalue.type,
                    !lvalue.ptr_level &&
                        !(lvalue.type && lvalue.type->ptr_level) &&
                        !lvalue.is_reference);

                /* Promote before scaling by the element size: the scaled
                 * product is typed int, so an unsigned char or unsigned short
                 * that is still in its narrow representation would otherwise
                 * lose its zero extension, and `sum += c` with c = 200 would
                 * add -56 to a wider left operand.
                 */
                rhs_val = integer_promote_operand(parent, bb, rhs_val);
                opstack_push(rhs_val);
                vd = require_var(parent);
                vd->init_val = increment_size;
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = integer_binary_result_type(OP_mul, rs1, rs2);
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, vd, rs1, rs2, 0, NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();

                /* Compound assignment performs the same integer promotions and
                 * usual arithmetic conversions as the corresponding binary
                 * operator, then converts the result back to the lvalue type.
                 * In particular, unsigned char and unsigned short must be
                 * promoted before unsigned division, rather than being consumed
                 * as sign-extended byte/halfword values by the backend.
                 */
                if (!is_pointer_operation(op, rs1, rs2)) {
                    rs1 = integer_promote_operand(parent, bb, rs1);
                    rs2 = integer_promote_operand(parent, bb, rs2);
                    normalize_integer_binary_operands(parent, bb, op, &rs1,
                                                      &rs2);
                }
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = integer_binary_result_type(op, rs1, rs2);
                add_insn(parent, *bb, op, vd, rs1, rs2, 0, NULL);

                if (lvalue.is_reference) {
                    if (is_bitfield(lvalue.decl))
                        write_bitfield_value(parent, bb, t, vd, lvalue.decl);
                    else {
                        /* Convert back to the object type before the store, as
                         * the direct-variable path does. The store alone
                         * narrows the bytes, but the expression value is this
                         * register, and a _Bool object must receive 0 or 1
                         * rather than the low byte of the sum.
                         */
                        vd = resize_to(parent, bb, vd, lvalue.type,
                                       lvalue.value_ptr_level);
                        add_insn(parent, *bb, OP_write, NULL, t, vd,
                                 lvalue.size, NULL);
                    }
                } else {
                    vd = resize_var(parent, bb, vd, t);
                    add_insn(parent, *bb, OP_assign, t, vd, NULL, 0, NULL);
                }
                if (assignment_result) {
                    /* Compound assignment has the value retained by its left
                     * operand. A bit-field store can truncate or sign-extend
                     * the arithmetic result, so its expression value must be
                     * reloaded just like an ordinary bit-field assignment.
                     */
                    if (is_bitfield(lvalue.decl)) {
                        var_t *stored =
                            read_bitfield_value(parent, bb, t, lvalue.decl);
                        stored->is_bitfield = false;
                        assignment_result[0] = stored;
                    } else {
                        assignment_result[0] = vd;
                    }
                }
            }
        } else {
            if (!read_assignment_expression(parent, bb)) {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
            }

            if (lvalue.is_func) {
                rs2 = opstack_pop();
                rs1 = opstack_pop();

                /* is_func labels both function symbols and function-pointer
                 * variables. A variable on the RHS must contribute its stored
                 * pointer value, rather than its identifier being lowered as a
                 * function address.
                 */
                if (rs2->is_func && find_var(rs2->var_name, parent) == rs2) {
                    t = require_ref_var(parent, rs2->type, rs2->ptr_level);
                    t->var_name = gen_name();
                    add_insn(parent, *bb, OP_address_of, t, rs2, NULL, 0, NULL);

                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    add_insn(parent, *bb, OP_read, vd, t, NULL, PTR_SIZE, NULL);
                    rs2 = vd;
                }

                /* Acquire destination address of lvalue if lvalue is a local
                 * variable.
                 */
                if (!lvalue.is_reference) {
                    var_t *addr =
                        require_ref_var(parent, lvalue.type, lvalue.ptr_level);
                    addr->var_name = gen_name();
                    add_insn(parent, *bb, OP_address_of, addr, rs1, NULL, 0,
                             NULL);
                    rs1 = addr;
                }

                add_insn(parent, *bb, OP_write, NULL, rs1, rs2, PTR_SIZE, NULL);
                if (assignment_result)
                    assignment_result[0] = rs2;
            } else if (lvalue.is_reference) {
                rs2 = opstack_pop();
                rs1 = opstack_pop();
                if (lvalue.pointee_func_signature) {
                    /* `slots[index]` denotes a callback slot whose descriptor
                     * is carried by the array, not by its scalar base type.
                     * Reconstruct that element target before writing so an
                     * ordinary indexed assignment observes the same prototype
                     * rule as a brace initializer.
                     */
                    var_t element_target = {0};

                    element_target.type = lvalue.decl->type;
                    element_target.ptr_level =
                        lvalue.decl->type->array_element_ptr_level;
                    element_target.pointee_func_signature =
                        lvalue.pointee_func_signature;
                    if (incompatible_pointee_callback_conversion(
                            rs2, &element_target))
                        error_at(
                            "incompatible callback slot types in array "
                            "assignment",
                            cur_token_loc());
                }
                bool record_store = is_record_type(lvalue.type) &&
                                    !lvalue.value_ptr_level &&
                                    is_record_object(rs2);

                /* A record and a scalar do not convert to each other. */
                if (!record_store && is_record_object(rs2))
                    error_at("incompatible types in assignment",
                             cur_token_loc());

                if (is_bitfield(lvalue.decl))
                    write_bitfield_value(parent, bb, rs1, rs2, lvalue.decl);
                else if (record_store)
                    emit_record_copy_to_address(parent, bb, rs1, rs2);
                else {
                    rs2 = convert_stored_value(parent, bb, rs2, lvalue.type,
                                               lvalue.value_ptr_level);
                    add_insn(parent, *bb, OP_write, NULL, rs1, rs2, size, NULL);
                }
                if (assignment_result && record_store) {
                    /* The stored record is a copy of the right operand. */
                    assignment_result[0] = rs2;
                } else if (assignment_result) {
                    /* The value of an assignment is the value retained by its
                     * left operand. A bit-field store may mask or sign-extend
                     * the source, so reload its extracted value instead of
                     * leaking the unconverted right operand.
                     */
                    if (is_bitfield(lvalue.decl)) {
                        var_t *stored =
                            read_bitfield_value(parent, bb, rs1, lvalue.decl);
                        stored->is_bitfield = false;
                        assignment_result[0] = stored;
                    } else {
                        var_t *stored = require_typed_var(parent, lvalue.type);
                        stored->ptr_level = lvalue.value_ptr_level;
                        stored->var_name = gen_name();
                        add_insn(parent, *bb, OP_read, stored, rs1, NULL, size,
                                 NULL);
                        assignment_result[0] = stored;
                    }
                }
            } else {
                rs1 = opstack_pop();
                vd = opstack_pop();
                rs1 = materialize_function_designator(parent, bb, rs1);
                if (is_record_object(vd) != is_record_object(rs1) &&
                    !rs1->is_func)
                    error_at("incompatible types in assignment",
                             cur_token_loc());
                if (is_record_object(vd) && is_record_object(rs1)) {
                    emit_record_copy(parent, bb, vd, rs1);

                    /* C99 assignment expressions have the assigned value,
                     * including struct and union copies. The source record is
                     * that value after the copy and can feed a grouped or
                     * larger expression without re-lowering the assignment.
                     */
                    if (assignment_result)
                        assignment_result[0] = rs1;
                } else if (incompatible_character_pointer_conversion(rs1, vd)) {
                    error_at(
                        "incompatible character pointer types in assignment",
                        cur_token_loc());
                } else if (incompatible_pointee_callback_conversion(rs1, vd)) {
                    error_at("incompatible callback slot types in assignment",
                             cur_token_loc());
                } else if (incompatible_const_pointer_conversion(rs1, vd)) {
                    diagnose_const_pointer_conversion(rs1, vd);
                } else {
                    rs1 = resize_var(parent, bb, rs1, vd);
                    mark_var_mutated(vd);
                    add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
                    if (assignment_result)
                        assignment_result[0] = rs1;
                }
            }
        }
        return true;
    }

    return false;
}

/* Assignment is right-associative and binds more weakly than the expression
 * parser's binary operators. Existing statement parsing has an lvalue-aware
 * lowering path; use a side-effect-free token scan to select that path before
 * parsing an expression, rather than attempting to rewind emitted IR.
 */
bool assignment_expression_follows(void)
{
    int depth = 0;

    for (token_t *t = cur_token->next; t; t = t->next) {
        switch (t->kind) {
        case T_open_bracket:
        case T_open_square:
        case T_open_curly:
            depth++;
            break;
        case T_close_bracket:
        case T_close_square:
        case T_close_curly:
            if (!depth)
                return false;
            depth--;
            break;
        case T_assign:
        case T_pluseq:
        case T_minuseq:
        case T_asteriskeq:
        case T_divideeq:
        case T_modeq:
        case T_lshifteq:
        case T_rshifteq:
        case T_andeq:
        case T_oreq:
        case T_xoreq:
            if (!depth)
                return true;
            break;
        case T_question:
            /* `?:` has lower precedence than assignment. An assignment in an
             * arm belongs to that conditional expression, not to the leading
             * lvalue that this scanner is classifying.
             */
            if (!depth)
                return false;
            break;
        case T_comma:
        case T_semicolon:
        case T_eof:
            if (!depth)
                return false;
            break;
        default:
            break;
        }
    }
    return false;
}

/* A compound literal begins with a parenthesized type name followed by `{`. Its
 * initializer may itself contain `=`, and the compound-literal lowering owns
 * the assignment which follows the closing brace.
 */
bool compound_literal_starts_expression(void)
{
    token_t *t = cur_token->next;
    int depth = 0;

    if (!t || t->kind != T_open_bracket)
        return false;
    for (; t; t = t->next) {
        if (t->kind == T_open_bracket)
            depth++;
        else if (t->kind == T_close_bracket && --depth == 0)
            return t->next && t->next->kind == T_open_curly;
    }
    return false;
}

/* Whether the assignment ahead has a parenthesized left operand followed by
 * postfix operators, `(*q).items[1] = v` or `(q.slots[1])->value = v`, that the
 * dedicated dereference and identifier paths below cannot follow. A plain
 * member chain on a dereference, `(*q).a.b = v`, stays with the dereference
 * path.
 */
static bool grouped_postfix_lvalue_follows(void)
{
    token_t *token = cur_token->next;
    token_t *inner;
    int depth = 0;

    if (!token || token->kind != T_open_bracket)
        return false;
    inner = token->next;
    for (; token; token = token->next) {
        if (token->kind == T_open_bracket)
            depth++;
        else if (token->kind == T_close_bracket && !--depth)
            break;
    }
    if (!token || !(token = token->next) ||
        (token->kind != T_dot && token->kind != T_arrow &&
         token->kind != T_open_square))
        return false;
    if (inner && inner->kind == T_asterisk) {
        while (token && token->kind == T_dot && token->next &&
               token->next->kind == T_identifier)
            token = token->next->next;
        if (token && token->kind != T_dot && token->kind != T_arrow &&
            token->kind != T_open_square)
            return false;
    }
    return true;
}

/* Assign through a left operand that the expression parser lowers as a value,
 * using the address that value records.
 */
static void read_grouped_postfix_assignment(block_t *parent, basic_block_t **bb)
{
    var_t *target;
    var_t *address;
    var_t *value;
    var_t *result;
    opcode_t compound_op = OP_generic;
    bool pointer_object;
    int size;

    read_expr_operand(parent, bb);
    target = opstack_pop();
    if (!target->is_compound_literal_reference || target->array_size ||
        target->is_func)
        error_at("Assignment requires a modifiable lvalue", cur_token_loc());
    pointer_object = effective_pointer_depth(target) > 0;
    if (pointer_object ? target->is_const_pointer : target->is_const_qualified)
        error_at("assignment of read-only location", cur_token_loc());
    address = target->compound_literal_address;
    size = pointer_object || target->func_signature ? PTR_SIZE
                                                    : target->type->size;
    if (!lex_accept(T_assign) && !accept_compound_assign_op(&compound_op))
        error_at("Expected assignment operator", cur_token_loc());
    if (!read_assignment_expression(parent, bb)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
    }
    value = opstack_pop();

    if (is_record_object(target)) {
        if (compound_op != OP_generic || !is_record_object(value))
            error_at("Invalid record assignment", cur_token_loc());
        emit_record_copy_to_address(parent, bb, address, value);
        opstack_push(value);
        return;
    }

    /* A scalar object takes an arithmetic operand, and a pointer object only
     * steps by an integer.
     */
    if (is_record_object(value) ||
        (compound_op != OP_generic &&
         (value->is_func || is_pointer_like_value(value) ||
          (pointer_object && compound_op != OP_add && compound_op != OP_sub))))
        error_at("Invalid assignment operand", cur_token_loc());

    if (compound_op != OP_generic) {
        var_t *current;
        var_t *combined;

        if (target->compound_literal_bitfield)
            current = read_bitfield_value(parent, bb, address,
                                          target->compound_literal_bitfield);
        else {
            current =
                require_typed_ptr_var(parent, target->type, target->ptr_level);
            current->var_name = gen_name();
            add_insn(parent, *bb, OP_read, current, address, NULL, size, NULL);
        }
        if (is_pointer_operation(compound_op, current, value)) {
            handle_pointer_arithmetic(parent, bb, compound_op, current, value);
            combined = opstack_pop();
        } else {
            current = integer_promote_operand(parent, bb, current);
            value = integer_promote_operand(parent, bb, value);
            normalize_integer_binary_operands(parent, bb, compound_op, &current,
                                              &value);
            combined = require_var(parent);
            combined->var_name = gen_name();
            combined->type =
                integer_binary_result_type(compound_op, current, value);
            add_insn(parent, *bb, compound_op, combined, current, value, 0,
                     NULL);
        }
        value = combined;
    }

    if (target->compound_literal_bitfield) {
        write_bitfield_value(parent, bb, address, value,
                             target->compound_literal_bitfield);
        result = read_bitfield_value(parent, bb, address,
                                     target->compound_literal_bitfield);
        result->is_bitfield = false;
        opstack_push(result);
        return;
    }
    if (!value->is_func)
        value = resize_to(parent, bb, value, target->type, target->ptr_level);
    add_insn(parent, *bb, OP_write, NULL, address, value, size, NULL);

    /* The assignment has the value the object then holds. */
    result = require_typed_ptr_var(parent, target->type, target->ptr_level);
    result->var_name = gen_name();
    result->func_signature = target->func_signature;
    add_insn(parent, *bb, OP_read, result, address, NULL, size, NULL);
    opstack_push(result);
}

bool read_assignment_expression(block_t *parent, basic_block_t **bb)
{
    char token[MAX_ID_LEN];
    var_t *result;
    int lvalue_paren_depth = 0;

    if (!assignment_expression_follows())
        return false;
    if (compound_literal_starts_expression())
        return false;
    if (grouped_postfix_lvalue_follows() ||
        (lex_peek(T_numeric, NULL) && cur_token->next->next &&
         cur_token->next->next->kind == T_open_square) ||
        (lex_peek(T_identifier, token) && find_var(token, parent) &&
         is_swapped_subscript_base(find_var(token, parent)))) {
        read_grouped_postfix_assignment(parent, bb);
        return true;
    }

    /* An assignment operator after a balanced parenthesized prefix means the
     * parentheses wrap the left operand: `(item) = value`, unlike `(item =
     * value)`, whose assignment is nested and therefore not seen by the
     * top-level scan above.
     */
    while (lex_accept(T_open_bracket))
        lvalue_paren_depth++;

    if (lex_peek(T_asterisk, NULL)) {
        var_t *address;
        var_t *object_address;
        var_t *value;
        var_t *field = NULL;
        opcode_t compound_op = OP_generic;
        int store_size;
        int grouped_fixed_array_dimensions = 0;
        type_t *value_type;
        int value_ptr_level;

        /* Consume one dereference. read_expr() then evaluates its operand as an
         * address: this also retains the established **pp and *(p + n)
         * statement semantics.
         */
        lex_expect(T_asterisk);
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
        address = opstack_pop();
        object_address = address;
        while (lvalue_paren_depth-- > 0)
            lex_expect(T_close_bracket);

        /* The ordinary `(*pointer)` assignment path historically stopped at the
         * grouping close. Admit only exact scalar fixed-array suffixes here,
         * retaining its common store/result lowering below.
         */
        if (lex_peek(T_open_square, NULL)) {
            var_t shape = {0};
            fixed_array_shape_t array_shape;
            int depth = 0;
            int dimensions;

            if (!address->pointee_array_size ||
                address->pointee_array_element_ptr_level ||
                effective_pointer_depth(address) != 1)
                error_at(
                    "Grouped subscript requires a scalar fixed array pointer",
                    cur_token_loc());
            shape.type = address->type->pointee_array_element_type
                             ? address->type->pointee_array_element_type
                             : address->type;
            array_shape = fixed_array_shape_from_pointee_var(address);
            fixed_array_shape_to_var(&shape, &array_shape);
            dimensions = array_shape.rank;
            grouped_fixed_array_dimensions = dimensions;
            if (dimensions > 4)
                error_at(
                    "Grouped fixed-array assignment supports at most four "
                    "dimensions",
                    cur_token_loc());
            while (lex_accept(T_open_square)) {
                var_t *index;
                var_t *scale;
                var_t *offset;
                var_t *next;
                int stride;

                if (depth >= dimensions)
                    error_at("Too many grouped array subscripts",
                             cur_token_loc());
                if (!read_assignment_expression(parent, bb)) {
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                }
                index = opstack_pop();
                lex_expect(T_close_square);
                stride = fixed_array_subscript_stride(&shape, depth,
                                                      shape.type->size);
                offset = index;
                if (stride != 1) {
                    scale = require_var(parent);
                    scale->var_name = gen_name();
                    scale->init_val = stride;
                    add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL,
                             0, NULL);
                    offset = require_var(parent);
                    offset->var_name = gen_name();
                    add_insn(parent, *bb, OP_mul, offset, index, scale, 0,
                             NULL);
                }
                next = require_typed_ptr_var(parent, shape.type, 1);
                next->var_name = gen_name();
                next->is_const_qualified = object_address->is_const_qualified ||
                                           shape.type->is_const_qualified;
                add_insn(parent, *bb, OP_add, next, address, offset, 0, NULL);
                address = next;
                depth++;
            }
            if (depth != dimensions)
                error_at("Grouped fixed-array assignment needs all subscripts",
                         cur_token_loc());
        }

        value_type = pointee_type_from_pointer_typedef(address->type);
        value_ptr_level = address->ptr_level > 1 ? address->ptr_level - 1 : 0;
        store_size = get_pointer_element_size(address);

        /* Parenthesized dereference may be followed by ordinary record-member
         * postfixes: `(*pointer).member = value`. Resolve their address before
         * selecting the shared scalar/bit-field store path.
         */
        if (lex_accept(T_dot)) {
            type_t *record_type = value_type;
            char field_name[MAX_ID_LEN];
            do {
                lex_ident(T_identifier, field_name);
                field = find_member(field_name, record_type);
                if (!field)
                    error_at("Unknown record member", cur_token_loc());
                address = compute_field_address(parent, bb, address, field);
                if (!lex_accept(T_dot))
                    break;
                if (!is_record_type(field->type) || field->ptr_level ||
                    field->array_size)
                    error_at("Member access requires a record",
                             cur_token_loc());
                record_type = field->type;
            } while (true);
            value_type = field->type;
            value_ptr_level = field->ptr_level;
            store_size = field->is_bitfield ? field->bit_storage_size
                                            : field->type->size;
            if (is_array_declarator(field))
                error_at("assignment to expression with array type",
                         cur_token_loc());
        }

        int address_depth = effective_pointer_depth(object_address);
        unsigned int address_mask =
            effective_pointer_const_mask(object_address);
        if ((field && field->is_const_qualified) ||
            address->is_const_qualified ||
            (address_depth > 1 && address_depth <= 32 &&
             (address_mask & (1U << (address_depth - 2)))))
            error_at("assignment of read-only location", cur_token_loc());
        if (!lex_accept(T_assign) && !accept_compound_assign_op(&compound_op))
            error_at("Expected assignment after pointer dereference",
                     cur_token_loc());
        if (grouped_fixed_array_dimensions >= 3 && compound_op != OP_generic &&
            compound_op != OP_add && compound_op != OP_sub &&
            compound_op != OP_mul && compound_op != OP_div &&
            compound_op != OP_mod && compound_op != OP_lshift &&
            compound_op != OP_rshift && compound_op != OP_bit_and &&
            compound_op != OP_bit_xor && compound_op != OP_bit_or)
            error_at("Unsupported rank-three/four grouped compound assignment",
                     cur_token_loc());

        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        value = opstack_pop();

        if (grouped_fixed_array_dimensions >= 3 && compound_op != OP_generic &&
            (value_ptr_level != 0 || is_record_type(value_type) ||
             value_type->is_floating || value->is_func ||
             is_pointer_like_value(value) || is_record_type(value->type) ||
             value->type->is_floating))
            error_at(
                "Invalid rank-three/four grouped compound assignment operand",
                cur_token_loc());

        if (compound_op != OP_generic) {
            var_t *current;
            if (field && is_bitfield(field)) {
                current = read_bitfield_value(parent, bb, address, field);
            } else {
                current = require_typed_var(parent, value_type);
                current->ptr_level = value_ptr_level;
                current->var_name = gen_name();
                add_insn(parent, *bb, OP_read, current, address, NULL,
                         store_size, NULL);
            }
            /* A pointer object steps by its element size. */
            if (is_pointer_operation(compound_op, current, value)) {
                handle_pointer_arithmetic(parent, bb, compound_op, current,
                                          value);
                value = opstack_pop();
            } else {
                current = integer_promote_operand(parent, bb, current);
                value = integer_promote_operand(parent, bb, value);
                normalize_integer_binary_operands(parent, bb, compound_op,
                                                  &current, &value);
                var_t *combined = require_var(parent);
                combined->var_name = gen_name();
                combined->type =
                    integer_binary_result_type(compound_op, current, value);
                add_insn(parent, *bb, compound_op, combined, current, value, 0,
                         NULL);
                value = combined;
            }
        }

        if (field && is_bitfield(field))
            write_bitfield_value(parent, bb, address, value, field);
        else if (is_record_type(value_type) && !value_ptr_level) {
            if (compound_op != OP_generic || !is_record_object(value))
                error_at("Invalid record assignment", cur_token_loc());
            emit_record_copy_to_address(parent, bb, address, value);
            opstack_push(value);
            return true;
        } else {
            value = convert_stored_value(parent, bb, value, value_type,
                                         value_ptr_level);
            add_insn(parent, *bb, OP_write, NULL, address, value, store_size,
                     NULL);
        }

        /* Reload after the store: assignment expressions observe the stored
         * object representation, including narrow integer conversion.
         */
        if (field && is_bitfield(field)) {
            result = read_bitfield_value(parent, bb, address, field);
            result->is_bitfield = false;
        } else {
            result = require_typed_var(parent, value_type);
            result->ptr_level = value_ptr_level;
            result->var_name = gen_name();
            add_insn(parent, *bb, OP_read, result, address, NULL, store_size,
                     NULL);
        }
        if (is_bool_scalar(result->type, result->ptr_level))
            result = normalize_bool(parent, bb, result);
        opstack_push(result);
        return true;
    }

    /* Identifier-rooted lvalues include direct objects, member access, and
     * subscripts through the shared lvalue parser.
     */
    if (!lex_peek(T_identifier, token))
        return false;

    if (!read_body_assignment(token, parent, OP_generic, bb, &result,
                              lvalue_paren_depth))
        return false;
    if (!result)
        error_at("Assignment expression does not yield a value",
                 cur_token_loc());
    opstack_push(result);
    return true;
}
