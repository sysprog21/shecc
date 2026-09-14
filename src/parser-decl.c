/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Abstract type names, integer constant expressions, declarators, parameter
 * lists, and numeric, character and string literal operands.
 *
 * A fragment of the parser: parser.c includes it in order, so it sees every
 * definition that precedes it there and cannot be compiled on its own.
 */

/* Integer constant-expression parser.
 *
 * Array dimensions, and other places C requires an integer constant expression,
 * accept far more than a bare literal. These evaluate such an expression at
 * parse time without emitting any IR, folding through the same precedence table
 * (get_operator_prio()) and the same operator semantics (eval_expression_imm())
 * the rest of the parser already uses, so there is only one statement of what
 * C's operators mean.
 */
#define MAX_CONST_EXPR_OPS 16

int eval_expression_imm(opcode_t op, int op1, int op2);
int read_const_expr(block_t *scope);
int read_const_wstring_size(void);

void read_sizeof_function_prototype(void)
{
    func_t *func = arena_alloc_func();
    bool saved_sizeof_signature = parsing_sizeof_function_signature;

    parsing_sizeof_function_signature = true;
    read_parameter_list_decl(func, true);
    parsing_sizeof_function_signature = saved_sizeof_signature;
}

/* Consume a nested direct-abstract-declarator in forms such as `int
 * (*(*)(int))[2]` and `int (*(*(*)(int))(int))(int)`. The surrounding
 * declarator already consumed its leading `(` and pointer chain. Each recursive
 * invocation consumes its own parenthesized pointer declarator and function
 * suffix, then the enclosing `)` and that declarator's suffix. The complete
 * type remains pointer-sized, but its bounds and prototypes must still obey C99
 * syntax and the project's fixed-array-only policy.
 */
int read_sizeof_nested_function_pointer_suffix(block_t *scope,
                                               int *outer_array_size)
{
    int inner_ptr_count = 0;

    lex_expect(T_open_bracket);
    while (lex_accept(T_asterisk)) {
        inner_ptr_count++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }
    if (!inner_ptr_count)
        error_at("sizeof nested abstract declarator needs a pointer",
                 cur_token_loc());

    if (lex_peek(T_open_bracket, NULL))
        inner_ptr_count +=
            read_sizeof_nested_function_pointer_suffix(scope, outer_array_size);
    else {
        int direct_array_size = 0;

        while (lex_accept(T_open_square)) {
            int bound = read_const_expr(scope);

            lex_expect(T_close_square);
            if (bound <= 0)
                error_at("sizeof array type needs a positive constant bound",
                         cur_token_loc());
            if (bound > 0 && direct_array_size &&
                direct_array_size > INT_MAX / bound)
                error_at("sizeof array type is too large", cur_token_loc());
            if (bound > 0)
                direct_array_size =
                    direct_array_size ? direct_array_size * bound : bound;
        }
        if (direct_array_size && outer_array_size)
            *outer_array_size = direct_array_size;
        lex_expect(T_close_bracket);
        if (lex_peek(T_open_bracket, NULL)) {
            read_sizeof_function_prototype();
        } else if (lex_peek(T_open_square, NULL)) {
            do {
                int bound;

                lex_expect(T_open_square);
                bound = read_const_expr(scope);
                lex_expect(T_close_square);
                if (bound <= 0)
                    error_at(
                        "sizeof array type needs a positive constant bound",
                        cur_token_loc());
            } while (lex_peek(T_open_square, NULL));
        } else {
            error_at(
                "sizeof nested abstract declarator needs a function or "
                "array suffix",
                cur_token_loc());
        }
    }

    lex_expect(T_close_bracket);
    while (lex_peek(T_open_bracket, NULL))
        read_sizeof_function_prototype();
    while (lex_accept(T_open_square)) {
        int bound = read_const_expr(scope);

        lex_expect(T_close_square);
        if (bound <= 0)
            error_at("sizeof array type needs a positive constant bound",
                     cur_token_loc());
    }
    return inner_ptr_count;
}

/* Integer constant expressions may contain sizeof(type-name). This parser only
 * needs type metadata, so keep it separate from expression lowering and avoid
 * emitting the otherwise unevaluated sizeof IR into an array bound or
 * enumerator declaration.
 */
int read_const_sizeof_type(block_t *scope)
{
    char token[MAX_ID_LEN];
    type_t *type = NULL;
    int ptr_level = 0;
    int array_size = 0;
    int array_element_size = 0;
    bool is_unsigned = false;
    bool is_signed = false;
    int long_count = 0;

    lex_expect(T_open_bracket);
    if (lex_accept(T_struct) || lex_accept(T_union)) {
        lex_ident(T_identifier, token);
        type = find_type(token, 2);
    } else if (lex_accept(T_enum)) {
        lex_ident(T_identifier, token);
        type = find_type_tag(token, scope);
    } else {
        /* Declaration specifiers may appear in any order: all of "unsigned long
         * int", "long unsigned int", and "int unsigned" name the same type.
         */
        while (true) {
            if (lex_accept(T_unsigned)) {
                if (is_unsigned)
                    error_at("duplicate unsigned type specifier",
                             cur_token_loc());
                is_unsigned = true;
            } else if (lex_accept(T_signed)) {
                if (is_signed)
                    error_at("duplicate signed type specifier",
                             cur_token_loc());
                is_signed = true;
            } else if (lex_accept(T_long)) {
                long_count++;
            } else if (lex_accept(T_float)) {
                type = TY_float;
            } else if (lex_accept(T_double)) {
                type = TY_double;
            } else if (lex_accept(T_const) || lex_accept(T_volatile) ||
                       lex_accept(T_restrict)) {
                ;
            } else if (lex_peek(T_identifier, token)) {
                type_t *candidate = find_visible_type(token, scope);

                if (!candidate)
                    break;
                lex_expect(T_identifier);
                type = candidate;
            } else {
                break;
            }
        }
        if (is_unsigned && is_signed)
            error_at("both signed and unsigned specified", cur_token_loc());
        if (long_count > 2)
            error_at("too many long type specifiers", cur_token_loc());
        if (type == TY_float && (is_unsigned || is_signed || long_count))
            error_at("invalid float type specifiers", cur_token_loc());
        if (type == TY_double) {
            if (is_unsigned || is_signed || long_count > 1)
                error_at("invalid double type specifiers", cur_token_loc());
            if (long_count)
                type = TY_long_double;
        } else if (long_count) {
            type = is_unsigned ? TY_ulong : TY_long;
            if (long_count == 2)
                type = is_unsigned ? TY_ulong_long : TY_long_long;
        } else if (is_unsigned) {
            if (type == TY_char)
                type = TY_uchar;
            else if (type == TY_short)
                type = TY_ushort;
            else
                type = TY_uint;
        } else if (is_signed) {
            type = type == TY_char ? TY_schar : (type ? type : TY_int);
        }
    }
    if (!type)
        error_at(
            "sizeof in an integer constant expression requires a type name",
            cur_token_loc());
    while (lex_accept(T_asterisk)) {
        ptr_level++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }
    while (ptr_level == 0 && lex_accept(T_open_square)) {
        int bound = read_const_expr(scope);

        lex_expect(T_close_square);
        if (bound <= 0)
            error_at("sizeof array type needs a positive constant bound",
                     cur_token_loc());
        if (bound > 0 && array_size && array_size > INT_MAX / bound)
            error_at("sizeof array type is too large", cur_token_loc());
        if (bound > 0)
            array_size = array_size ? array_size * bound : bound;
    }
    if (lex_accept(T_open_bracket)) {
        int nested_array_size = 0;
        int nested_ptr_count = 0;
        int nested_function_ptr_count = 0;
        int nested_outer_array_size = 0;

        while (lex_accept(T_asterisk)) {
            nested_ptr_count++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
        if (!nested_ptr_count)
            error_at("sizeof abstract declarator needs a pointer",
                     cur_token_loc());
        if (lex_peek(T_open_bracket, NULL)) {
            nested_function_ptr_count =
                read_sizeof_nested_function_pointer_suffix(
                    scope, &nested_outer_array_size);
            if (nested_outer_array_size) {
                array_size = nested_outer_array_size;
                array_element_size = PTR_SIZE;
            } else
                ptr_level += nested_ptr_count + nested_function_ptr_count;
        } else
            while (lex_accept(T_open_square)) {
                int bound = read_const_expr(scope);

                lex_expect(T_close_square);
                if (bound <= 0)
                    error_at(
                        "sizeof array type needs a positive constant bound",
                        cur_token_loc());
                if (bound > 0 && nested_array_size &&
                    nested_array_size > INT_MAX / bound)
                    error_at("sizeof array type is too large", cur_token_loc());
                if (bound > 0)
                    nested_array_size =
                        nested_array_size ? nested_array_size * bound : bound;
            }
        if (!nested_function_ptr_count) {
            lex_expect(T_close_bracket);
            if (nested_array_size) {
                array_size = nested_array_size;
                array_element_size = PTR_SIZE;

                /* `int (*[2][3])(int)` is an array of function pointers: the
                 * parenthesized declarator owns the array bounds, while the
                 * following parameter list belongs to each pointed-to function.
                 * Consume that suffix before the enclosing sizeof parenthesis.
                 */
                if (lex_peek(T_open_bracket, NULL))
                    read_sizeof_function_prototype();
            } else {
                ptr_level += nested_ptr_count;
                while (lex_accept(T_open_square)) {
                    int bound = read_const_expr(scope);

                    lex_expect(T_close_square);
                    if (bound <= 0)
                        error_at(
                            "sizeof array type needs a positive constant bound",
                            cur_token_loc());
                }

                /* A pointer declarator followed by a parameter list names a
                 * function pointer. Its function type has no object
                 * representation, but the pointer itself is an object and
                 * sizeof is pointer-sized. Reuse the declaration parser for
                 * complete prototype syntax, then discard the otherwise-unused
                 * signature.
                 */
                if (lex_peek(T_open_bracket, NULL))
                    read_sizeof_function_prototype();
            }
        }
    }
    lex_expect(T_close_bracket);
    if (ptr_level)
        return PTR_SIZE;
    if (type == TY_void)
        error_at("sizeof cannot be applied to void", cur_token_loc());
    if (type->is_direct_function_type)
        error_at("sizeof(function) is invalid", cur_token_loc());
    if (!type->size && (!type->base_struct || !type->base_struct->size))
        error_at("sizeof cannot be applied to an incomplete type",
                 cur_token_loc());
    if (!array_size && !ptr_level && type->array_size)
        array_size = type->array_size;
    if (array_size)
        return array_size *
               (array_element_size ? array_element_size : type->size);
    if (type->size)
        return type->size;
    return type->base_struct->size;
}

/* Return the row-major offset contributed by a fixed terminal array member in
 * offsetof. A one-past designator is valid only for the final dimension.
 */
static int read_offsetof_array_subscripts(var_t *field, block_t *scope)
{
    int offset = 0;
    fixed_array_shape_t shape;
    int dimensions;

    if (!lex_accept(T_open_square))
        return 0;
    if (field->array_size <= 0)
        error_at("offsetof subscript requires a fixed array member",
                 cur_token_loc());
    shape = fixed_array_shape_from_var(field);
    dimensions = shape.rank;
    for (int dim = 0;; dim++) {
        int index = read_const_expr(scope);

        lex_expect(T_close_square);
        if (index < 0 || index > shape.bounds[dim])
            error_at("offsetof array subscript is out of bounds",
                     cur_token_loc());
        offset += index * fixed_array_shape_stride(&shape, dim, 1) *
                  field->type->size;
        if (!lex_accept(T_open_square))
            return offset;
        if (dim + 1 == dimensions)
            error_at("offsetof subscript exceeds array dimensions",
                     cur_token_loc());
        if (index == shape.bounds[dim])
            error_at("offsetof one-past array row cannot be subscripted",
                     cur_token_loc());
    }
}

int read_const_expr_operand(block_t *scope)
{
    char buffer[MAX_TOKEN_LEN];

    if (lex_accept(T_minus)) {
        int value = read_const_expr_operand(scope);

        if (checking_enum_constant && value == INT_MIN)
            error_at("Enumerator value exceeds int range", cur_token_loc());
        return -value;
    }
    if (lex_accept(T_plus))
        return read_const_expr_operand(scope);
    if (lex_accept(T_bit_not))
        return ~read_const_expr_operand(scope);
    if (lex_accept(T_log_not))
        return !read_const_expr_operand(scope);
    if (lex_accept(T_sizeof)) {
        if (lex_peek(T_wstring, NULL))
            return read_const_wstring_size();
        return read_const_sizeof_type(scope);
    }

    if (lex_accept(T_open_bracket)) {
        int res = read_const_expr(scope);
        lex_expect(T_close_bracket);
        return res;
    }
    if (lex_peek(T_numeric, buffer)) {
        lex_expect(T_numeric);
        return parse_numeric_constant(buffer);
    }
    if (lex_peek(T_char, buffer) || lex_peek(T_wchar, buffer)) {
        char unescaped[MAX_TOKEN_LEN];
        token_kind_t kind = lex_peek(T_wchar, NULL) ? T_wchar : T_char;
        lex_expect(kind);
        if (unescape_string(buffer, unescaped, MAX_TOKEN_LEN) < 0)
            error_at("Invalid escape sequence", cur_token_loc());
        return kind == T_wchar ? parse_wide_character_constant(buffer)
                               : parse_character_constant(buffer);
    }
    if (lex_peek(T_identifier, buffer)) {
        if (!strcmp(buffer, "__builtin_offsetof")) {
            char type_name[MAX_ID_LEN];
            char member_name[MAX_ID_LEN];
            type_t *record;
            var_t *field;
            int offset = 0;
            bool has_nested_member;

            lex_expect(T_identifier);
            lex_expect(T_open_bracket);
            if (lex_accept(T_struct) || lex_accept(T_union)) {
                lex_ident(T_identifier, type_name);
                record = find_type(type_name, 2);
            } else {
                lex_ident(T_identifier, type_name);
                record = find_type(type_name, true);
            }
            if (!is_record_type(record))
                error_at("offsetof requires a struct or union type",
                         cur_token_loc());
            lex_expect(T_comma);
            do {
                lex_ident(T_identifier, member_name);
                field = find_member(member_name, record);
                if (!field)
                    error_at("Unknown record member", cur_token_loc());
                offset += field->offset;
                offset += read_offsetof_array_subscripts(field, scope);
                record = field->type;
                has_nested_member = lex_accept(T_dot);
                if (has_nested_member &&
                    (field->ptr_level || !is_record_type(record)))
                    error_at("Nested offsetof member requires a record",
                             cur_token_loc());
            } while (has_nested_member);
            lex_expect(T_close_bracket);
            return offset;
        }
        lex_expect(T_identifier);
        constant_t *con = find_scoped_constant(buffer, scope);
        if (con)
            return con->value;
        error_at("Identifier is not an integer constant", next_token_loc());
    }
    error_at("Expected an integer constant expression", next_token_loc());
    return 0;
}

int read_const_expr(block_t *scope)
{
    opcode_t op_stack[MAX_CONST_EXPR_OPS];
    int val_stack[MAX_CONST_EXPR_OPS];
    int op_n = 0, val_n = 0;

    val_stack[val_n++] = read_const_expr_operand(scope);

    while (true) {
        opcode_t op = get_operator();

        if (op == OP_generic)
            break;

        /* The conditional binds loosest, so everything folded so far is its
         * condition, and its arms are whole constant expressions of their own.
         */
        if (op == OP_ternary) {
            while (op_n > 0) {
                val_n--;
                op_n--;
                val_stack[val_n - 1] = eval_expression_imm(
                    op_stack[op_n], val_stack[val_n - 1], val_stack[val_n]);
            }
            lex_expect(T_question);
            bool saved_checking = checking_enum_constant;
            checking_enum_constant = saved_checking && val_stack[0];
            int then_val = read_const_expr(scope);
            lex_expect(T_colon);
            checking_enum_constant = saved_checking && !val_stack[0];
            int else_val = read_const_expr(scope);
            checking_enum_constant = saved_checking;
            return val_stack[0] ? then_val : else_val;
        }

        /* Everything at least as tight as the operator just read is complete,
         * so fold it before pushing.
         */
        int prio = get_operator_prio(op);
        while (op_n > 0 && get_operator_prio(op_stack[op_n - 1]) >= prio) {
            val_n--;
            op_n--;
            val_stack[val_n - 1] = eval_expression_imm(
                op_stack[op_n], val_stack[val_n - 1], val_stack[val_n]);
        }
        if (op_n >= MAX_CONST_EXPR_OPS - 1)
            error_at("Constant expression nests too deeply", next_token_loc());
        op_stack[op_n++] = op;
        val_stack[val_n++] = read_const_expr_operand(scope);
    }

    while (op_n > 0) {
        val_n--;
        op_n--;
        val_stack[val_n - 1] = eval_expression_imm(
            op_stack[op_n], val_stack[val_n - 1], val_stack[val_n]);
    }
    return val_stack[0];
}

void read_inner_var_decl(var_t *vd,
                         bool anon,
                         bool is_param,
                         bool is_record_member)
{
    /* Preserve typedef pointer level - don't reset if already inherited */
    vd->init_val = 0;
    vd->has_direct_array_declarator = false;
    vd->has_direct_pointee_array_declarator = false;

    /* A plain typedef alias inherits every recorded bound, including the fourth
     * one. Fresh declarators start with no bound metadata; a derived suffix is
     * later composed against its base by the declaration path.
     */
    if (!vd->type || !vd->type->array_size)
        vd->array_dim4 = 0;
    if (is_param) {
        /* However, if the parsed variable is a function parameter, reset its
         * pointer level to zero.
         */
        vd->ptr_level = 0;
    }

    while (lex_accept(T_asterisk)) {
        vd->ptr_level++;

        /* Check for const after asterisk (e.g., int * const ptr). For now, we
         * just consume const qualifiers after pointer. Full support would
         * require tracking const-ness of the pointer itself vs the pointed-to
         * data separately.
         */
        while (true) {
            if (lex_accept(T_const)) {
                vd->is_const_pointer = true;
                if (vd->ptr_level <= 32)
                    vd->pointer_const_mask |= 1U << (vd->ptr_level - 1);
            } else if (lex_accept(T_volatile))
                vd->is_volatile = true;
            else if (lex_accept(T_restrict))
                ; /* restrict is an aliasing contract, not storage state. */
            else
                break;
        }
    }

    /* `typedef int row[2]; row *p` declares a pointer object, not an array
     * object. read_partial_var_decl() copied row's bounds into vd before the
     * declarator was known; move them to the pointer's pointee descriptor so
     * allocation loads p's value and postfix indexing still advances a row.
     */
    if (vd->ptr_level && vd->type && vd->type->array_size) {
        fixed_array_shape_t shape = fixed_array_shape_from_type(vd->type);
        fixed_array_shape_t empty_shape = {0};

        fixed_array_shape_to_pointee_var(vd, &shape);
        fixed_array_shape_to_var(vd, &empty_shape);
    }

    /* is it function pointer declaration? */
    if (lex_accept(T_open_bracket)) {
        func_t *func = arena_alloc_func();
        char temp_name[MAX_VAR_LEN];
        int nested_ptr_level = 0;

        do {
            lex_expect(T_asterisk);
            nested_ptr_level++;
            while (true) {
                if (lex_accept(T_const)) {
                    vd->parenthesized_function_pointer_const = true;
                    if (nested_ptr_level == 2)
                        vd->parenthesized_function_pointer_outer_const = true;
                    else
                        vd->parenthesized_function_pointer_inner_qualified =
                            true;
                    vd->is_const_pointer = true;
                    if (vd->ptr_level + nested_ptr_level <= 32)
                        vd->pointer_const_mask |=
                            1U << (vd->ptr_level + nested_ptr_level - 1);
                } else if (lex_accept(T_volatile)) {
                    if (nested_ptr_level == 2)
                        vd->parenthesized_function_pointer_outer_volatile =
                            true;
                    else
                        vd->parenthesized_function_pointer_inner_qualified =
                            true;
                    vd->is_volatile = true;
                } else if (lex_accept(T_restrict)) {
                    vd->parenthesized_function_pointer_restrict = true;
                    if (nested_ptr_level == 2)
                        vd->parenthesized_function_pointer_outer_restrict =
                            true;
                    else
                        vd->parenthesized_function_pointer_inner_qualified =
                            true;
                } else
                    break;
            }
        } while (lex_peek(T_asterisk, NULL));
        if (lex_peek(T_identifier, NULL)) {
            lex_ident(T_identifier, temp_name);
            vd->var_name = intern_string(temp_name);
        } else if (!anon || !is_param)
            lex_ident(T_identifier, temp_name);

        /* The array suffix belongs inside the parenthesized pointer declarator
         * in `int (*callbacks[2])(int)`. It is an array whose elements are
         * function pointers, not a function returning an array. Keep the
         * representation used by ordinary arrays so indexing and storage
         * allocation can share their existing paths.
         */
        for (int dim = 0; lex_accept(T_open_square); dim++) {
            bool parameter_static_bound = false;
            bool parameter_const_bound = false;

            if (dim >= 4)
                error_at("Array declarators support at most four dimensions",
                         cur_token_loc());
            while (lex_peek(T_static, NULL) || lex_peek(T_const, NULL) ||
                   lex_peek(T_volatile, NULL) || lex_peek(T_restrict, NULL)) {
                if (!is_param || dim)
                    error_at("array bracket qualifiers require a parameter",
                             cur_token_loc());
                if (lex_accept(T_static)) {
                    if (parameter_static_bound)
                        error_at("duplicate static array parameter qualifier",
                                 cur_token_loc());
                    parameter_static_bound = true;
                } else if (lex_accept(T_const))
                    parameter_const_bound = true;
                else if (lex_accept(T_volatile))
                    vd->is_volatile = true;
                else
                    lex_expect(T_restrict);
            }
            if (lex_peek(T_close_square, NULL)) {
                if (parameter_static_bound)
                    error_at("static array parameter needs a bound",
                             cur_token_loc());
                if (dim)
                    error_at(
                        "Only the outer function-pointer array bound may "
                        "be omitted",
                        cur_token_loc());
                vd->has_unsized_array = true;
            } else {
                int bound = read_const_expr(vd->scope);

                if (parameter_static_bound && bound <= 0)
                    error_at("static array parameter needs a positive bound",
                             cur_token_loc());
                if (bound <= 0)
                    error_at("Array size must be positive", cur_token_loc());
                if (dim == 0)
                    vd->array_size = bound;
                else {
                    if (dim == 1)
                        vd->array_dim2 = bound;
                    else if (dim == 2)
                        vd->array_dim3 = bound;
                    else if (dim == 3)
                        vd->array_dim4 = bound;
                    vd->array_size *= bound;
                }
            }
            if (parameter_const_bound && dim == 0) {
                vd->is_const_pointer = true;
                vd->pointer_const_mask |= 1U;
            }
            lex_expect(T_close_square);
        }
        lex_expect(T_close_bracket);

        /* A parenthesized pointer followed by array suffixes is a pointer to an
         * array, not a function pointer. Keep the pointee's row bounds apart
         * from the pointer object's own storage extent.
         */
        if (lex_peek(T_open_square, NULL)) {
            int dims = 0;

            /* The nested stars make this a pointer-to-array. Preserve whether
             * its array element was plain void before adding them; a typedef or
             * explicit pointer to void is a valid element.
             */
            bool void_array_element = vd->type &&
                                      vd->type->base_type == TYPE_void &&
                                      !effective_pointer_depth(vd);

            vd->ptr_level += nested_ptr_level;
            vd->pointee_array_element_ptr_level =
                vd->ptr_level - nested_ptr_level;
            vd->has_direct_pointee_array_declarator = true;
            while (lex_accept(T_open_square)) {
                int bound;

                if (dims >= 4)
                    error_at(
                        "Array declarators support at most four dimensions",
                        cur_token_loc());
                if (lex_peek(T_close_square, NULL))
                    error_at("Pointer-to-array needs a bound", cur_token_loc());
                bound = read_const_expr(vd->scope);
                if (bound <= 0)
                    error_at("Array size must be positive", cur_token_loc());
                if (dims == 0)
                    vd->pointee_array_size = bound;
                else {
                    if (dims == 1)
                        vd->pointee_array_dim2 = bound;
                    else if (dims == 2)
                        vd->pointee_array_dim3 = bound;
                    else
                        vd->pointee_array_dim4 = bound;
                    vd->pointee_array_size *= bound;
                }
                lex_expect(T_close_square);
                dims++;
            }
            if (dims && void_array_element)
                error_at("void type cannot be an array element",
                         cur_token_loc());
            return;
        }

        /* The return declaration was parsed before the parenthesized
         * declarator. Copy it into the syntax-only signature before the
         * function-pointer marker is set on vd.
         */
        memcpy(&func->return_def, vd, sizeof(var_t));
        func->returns_aggregate = is_record_type(func->return_def.type) &&
                                  !has_effective_pointer(&func->return_def);
        read_parameter_list_decl(func, true);
        vd->func_signature = func;
        vd->is_func = true;
        vd->parenthesized_function_pointer_level = nested_ptr_level;
        if (nested_ptr_level == 2) {
            /* `int (**slot)(int)` is an ordinary pointer object whose pointee
             * is the compact one-pointer callback representation. Keep that
             * outer object pointer in var_t; the final unary dereference
             * restores the callback signature before a call.
             */
            if (vd->parenthesized_function_pointer_inner_qualified)
                error_at(
                    "inner callback-slot pointer qualifiers are not yet "
                    "supported",
                    cur_token_loc());
            if (vd->parenthesized_function_pointer_restrict &&
                !vd->parenthesized_function_pointer_outer_restrict)
                error_at("callback-slot restrict typedef is not yet supported",
                         cur_token_loc());

            /* The second star is the outer slot pointer. After collapsing the
             * compact callback representation to one retained pointer level,
             * move that star's const bit from level two to level one.
             */
            if (vd->parenthesized_function_pointer_outer_const) {
                vd->pointer_const_mask &= ~2U;
                vd->pointer_const_mask |= 1U;
                vd->is_const_pointer = true;
            }
            vd->ptr_level += nested_ptr_level - 1;
            vd->pointee_func_signature = vd->func_signature;
            vd->func_signature = NULL;
            vd->is_func = false;
        }
    } else {
        /* Parameter declarations may use an abstract declarator in a prototype,
         * but a spelled identifier still has to be consumed even when callers
         * permit it to be omitted. Other anonymous type-name paths retain their
         * original no-identifier grammar.
         */
        if ((!anon || (is_param && lex_peek(T_identifier, NULL))) &&
            !lex_peek(T_colon, NULL)) {
            char temp_name[MAX_VAR_LEN];
            lex_ident(T_identifier, temp_name);
            vd->var_name = intern_string(temp_name);
            if (!lex_peek(T_open_bracket, NULL) && !is_param) {
                if (vd->is_global) {
                    opstack_push(vd);
                }
            }
        }
        if (!lex_peek(T_open_square, NULL) &&
            !(vd->type && vd->type->array_size)) {
            vd->array_size = 0;
            vd->array_dim2 = 0;
            vd->array_dim3 = 0;
            vd->array_dim4 = 0;
        }

        /* Every dimension multiplies into array_size, so "int matrix[3][4]"
         * becomes an array of 12 elements. The second dimension is kept
         * separately as well, because indexing needs the row length; further
         * dimensions only contribute to the total. A dimension left empty
         * contributes no size.
         */
        bool first_dim_empty = false;
        int dims = 0;

        /* Preserve the element shape before an unsized parameter array is
         * adjusted to a pointer. Arrays of void pointers remain valid.
         */
        bool void_array_element = vd->type &&
                                  vd->type->base_type == TYPE_void &&
                                  !effective_pointer_depth(vd);

        /* A block typedef's base array shape is retained in vd->type for
         * compose_block_typedef_array(). A new direct suffix contributes only
         * its own bounds; otherwise inherited inner bounds are appended twice
         * when aliases are composed.
         */
        if (parsing_block_typedef_declarator && vd->type &&
            vd->type->array_size && lex_peek(T_open_square, NULL)) {
            vd->array_size = 0;
            vd->array_dim2 = vd->array_dim3 = vd->array_dim4 = 0;
        }

        for (int dim = 0; lex_accept(T_open_square); dim++) {
            vd->has_direct_array_declarator = true;
            bool parameter_static_bound = false;
            bool parameter_const_bound = false;

            if (dim >= 4)
                error_at("Array declarators support at most four dimensions",
                         cur_token_loc());
            while (lex_peek(T_static, NULL) || lex_peek(T_const, NULL) ||
                   lex_peek(T_volatile, NULL) || lex_peek(T_restrict, NULL)) {
                if (!is_param || dim)
                    error_at("array bracket qualifiers require a parameter",
                             cur_token_loc());
                if (lex_accept(T_static)) {
                    if (parameter_static_bound || dim)
                        error_at(
                            "static is only valid in the outermost array "
                            "parameter bound",
                            cur_token_loc());
                    parameter_static_bound = true;
                } else if (lex_accept(T_const))
                    parameter_const_bound = true;
                else if (lex_accept(T_volatile))
                    vd->is_volatile = true;
                else
                    lex_expect(T_restrict);
            }
            if (lex_peek(T_close_square, NULL)) {
                if (parameter_static_bound)
                    error_at("static array parameter needs a bound",
                             cur_token_loc());

                /* An omitted leading size is only a pointer when nothing
                 * follows it: "int a[]" is "int *", but "int a[][4]" points at
                 * rows of four and is indexed exactly like "int a[3][4]".
                 * Raising the pointer level for the latter would scale the row
                 * index by a pointer instead of by the row, and add a
                 * dereference that is not there.
                 */
                if (dim == 0)
                    first_dim_empty = true;
                else
                    vd->ptr_level++;
            } else {
                int next_dim = read_const_expr(vd->scope);

                if (parameter_static_bound && next_dim <= 0)
                    error_at("static array parameter needs a positive bound",
                             cur_token_loc());

                if (dim == 0) {
                    vd->array_size = next_dim;
                } else {
                    if (dim == 1)
                        vd->array_dim2 = next_dim;
                    else if (dim == 2)
                        vd->array_dim3 = next_dim;
                    else if (dim == 3)
                        vd->array_dim4 = next_dim;
                    if (vd->array_size > 0)
                        vd->array_size *= next_dim;
                    else
                        vd->array_size = next_dim;
                }
            }
            if (parameter_const_bound && dim == 0) {
                vd->is_const_pointer = true;
                vd->pointer_const_mask |= 1U;
            }
            lex_expect(T_close_square);
            dims++;
        }
        if (first_dim_empty && is_record_member) {
            /* A record's omitted outer bound is its flexible array member.
             * Inner dimensions still describe the complete element type, so
             * retain their product and row stride for member indexing.
             */
            vd->has_unsized_array = true;
        } else if (first_dim_empty && !is_param) {
            /* An object declarator such as `char text[] = "hi"` has an inferred
             * outer bound. This also applies when it carries known inner
             * dimensions, e.g. `int table[][4]`.
             */
            vd->has_unsized_array = true;
        } else if (first_dim_empty && dims == 1) {
            /* Only a one-dimensional parameter needs the pointer adjustment; a
             * multidimensional parameter retains its inner row stride.
             */
            vd->ptr_level++;
        }

        /* Parameter adjustment turns `void values[]` into a pointer-shaped
         * declarator, but C99 still forbids void as an array element type. The
         * pre-suffix shape distinguishes it from an array of void pointers,
         * including a pointer typedef.
         */
        if (dims && void_array_element)
            error_at("void type cannot be an array element", cur_token_loc());

        /* C99 permits an object of a flexible-record type, but not an array
         * whose elements have no fixed extent. An explicitly pointer-typed
         * element remains valid, including through a pointer typedef.
         */
        if (vd->array_size > 0 && !vd->ptr_level && !vd->type->ptr_level &&
            type_has_flexible_array_member(vd->type))
            error_at(
                "A struct with a flexible array member cannot be an array "
                "element",
                cur_token_loc());

        /* The ordinary declarator spelling `result name(parameters)` is a
         * function type just as the parenthesized pointer form above is. Keep
         * this syntax-only signature on the declaration; block typedefs can
         * then bind a direct function alias without creating an object or IR.
         */
        if ((parsing_block_typedef_declarator ||
             (strict_c99 && parsing_for_initializer_declaration)) &&
            lex_peek(T_open_bracket, NULL) && !vd->array_size &&
            !vd->has_unsized_array) {
            func_t *func = arena_alloc_func();
            bool saved_block_typedef_declarator =
                parsing_block_typedef_declarator;

            memcpy(&func->return_def, vd, sizeof(var_t));
            func->returns_aggregate = is_record_type(func->return_def.type) &&
                                      !has_effective_pointer(&func->return_def);

            /* The enclosing typedef is the only declaration entitled to use
             * this direct-function syntax. Parameter declarators continue
             * through their established parsing path.
             */
            parsing_block_typedef_declarator = false;
            read_parameter_list_decl(func, true);
            parsing_block_typedef_declarator = saved_block_typedef_declarator;
            vd->func_signature = func;
            vd->is_func = true;
            vd->is_direct_function_declarator = true;
            return;
        }

        /* An ordinary declarator can name a function-pointer typedef. Its
         * prototype belongs to the typedef's type descriptor, while the object
         * retains the existing function-pointer representation.
         */
        if (vd->ptr_level == 1 && !vd->array_size &&
            vd->type->is_direct_function_type && vd->type->func_signature) {
            /* A pointer applied directly to a function typedef is a callable
             * function-pointer object. It is not a function designator, but
             * indirect-call lowering needs its prototype.
             */
            vd->func_signature = vd->type->func_signature;
            vd->is_func = false;
        } else if (vd->ptr_level || vd->type->ptr_level || vd->array_size) {
            /* A derived declarator is not itself a callable callback object.
             * Preserve no direct-call marker until dereference/subscript
             * lowering can carry the element prototype separately.
             */
            vd->func_signature = NULL;
            vd->is_func = false;
        } else {
            vd->is_func = vd->func_signature != NULL;
        }
    }

    /* The legacy flag remains the outermost pointer qualifier for lvalue
     * writes. Intermediate qualifiers are retained in pointer_const_mask.
     */
    if (vd->ptr_level > 0 && vd->ptr_level <= 32)
        vd->is_const_pointer =
            vd->pointer_const_mask & (1U << (vd->ptr_level - 1));

    /* C99 permits void only as a function return type, a pointer target, or the
     * separately validated lone parameter-list sentinel. A by-value void
     * declarator has no object size and must not reach allocation or layout.
     */
    if (vd->type && vd->type->base_type == TYPE_void &&
        !effective_pointer_depth(vd) && !vd->is_func &&
        !lex_peek(T_open_bracket, NULL))
        error_at("void type cannot define an object", cur_token_loc());
}

/* starting next_token, need to check the type */
void read_full_var_decl(var_t *vd,
                        bool anon,
                        bool is_param,
                        bool is_record_member)
{
    char type_name[MAX_ID_LEN];

    /* Callers which have already consumed a leading qualifier leave it on the
     * declaration. Keep it separate from qualification inherited from a
     * typedef: `const int_pointer` qualifies the pointer object, while an
     * unqualified `const_int_pointer` only qualifies the pointed-to object.
     */
    bool declaration_const = vd->is_const_qualified;
    bool declaration_inline = vd->is_inline;
    bool declaration_volatile = vd->is_volatile;
    bool is_signed = false;
    bool is_unsigned = false;
    bool is_const = false;
    bool is_inline = false;
    bool is_volatile = false;
    bool is_long = false;
    bool is_long_long = false;
    type_t *leading_scalar_type = NULL;

    /* The declaration dispatcher normally leaves the base type for this routine
     * to consume. C99 also permits the modifier after that base, e.g. `short
     * unsigned value`; retain the base while consuming its following scalar
     * modifiers instead of mistaking `unsigned` for the declarator name.
     */
    if (lex_peek(T_identifier, type_name) &&
        (!strcmp(type_name, "char") || !strcmp(type_name, "short") ||
         !strcmp(type_name, "int"))) {
        token_t *after_base = cur_token->next->next;

        while (after_base && after_base->kind == T_const)
            after_base = after_base->next;
        if (after_base &&
            (after_base->kind == T_signed || after_base->kind == T_unsigned)) {
            lex_expect(T_identifier);
            leading_scalar_type = find_type(type_name, true);
        }
    }

    /* C permits these declaration specifiers in either order. Consume the
     * scalar set as a group so `const unsigned int` and `unsigned const int`
     * follow the same path.
     */
    while (lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
           lex_peek(T_const, NULL) || lex_peek(T_inline, NULL) ||
           lex_peek(T_volatile, NULL) || lex_peek(T_long, NULL)) {
        if (lex_accept(T_signed)) {
            if (is_signed)
                error_at("duplicate signed type specifier", cur_token_loc());
            is_signed = true;
        } else if (lex_accept(T_unsigned)) {
            if (is_unsigned)
                error_at("duplicate unsigned type specifier", cur_token_loc());
            is_unsigned = true;
        } else if (lex_accept(T_const))
            is_const = true;
        else if (lex_accept(T_volatile))
            is_volatile = true;
        else if (lex_accept(T_inline)) {
            if (is_inline || declaration_inline)
                error_at("duplicate inline function specifier",
                         cur_token_loc());
            is_inline = true;
        } else {
            lex_expect(T_long);
            if (is_long_long)
                error_at("too many long type specifiers", cur_token_loc());
            if (is_long)
                is_long_long = true;
            else
                is_long = true;
        }
    }
    if (is_signed && is_unsigned)
        error_at("both signed and unsigned specified", cur_token_loc());
    if (leading_scalar_type == TY_int && lex_peek(T_identifier, type_name) &&
        !strcmp(type_name, "char"))
        error_at("int cannot be combined with char", cur_token_loc());
    bool is_enum_type = lex_accept(T_enum);
    if (is_enum_type && (is_signed || is_unsigned || is_long))
        error_at("enum type cannot be combined with integer specifiers",
                 cur_token_loc());
    bool is_record_type = lex_peek(T_struct, NULL) || lex_peek(T_union, NULL);
    bool is_union_type = lex_accept(T_union);
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (is_union_type)
        find_type_flag = 2;
    type_t *type;

    /* `signed` has the existing signed scalar semantics. C permits its `int`
     * spelling to be omitted, while `signed char` and `signed short` retain
     * their explicit base type.
     */
    if (parsing_sizeof_function_signature && lex_accept(T_float)) {
        if (is_signed || is_unsigned || is_long)
            error_at("invalid float type specifiers", cur_token_loc());
        type = TY_float;
    } else if (parsing_sizeof_function_signature && lex_accept(T_double)) {
        if (is_signed || is_unsigned || is_long_long)
            error_at("invalid double type specifiers", cur_token_loc());
        type = is_long ? TY_long_double : TY_double;
    } else if (is_enum_type) {
        lex_ident(T_identifier, type_name);
        type = find_type_tag(type_name, vd->scope);
    } else if (is_unsigned) {
        if (is_long) {
            if (lex_peek(T_identifier, type_name) && !strcmp(type_name, "int"))
                lex_expect(T_identifier);
            type = is_long_long ? TY_ulong_long : TY_ulong;
        } else if (leading_scalar_type == TY_char) {
            type = TY_uchar;
        } else if (leading_scalar_type == TY_short) {
            if (lex_peek(T_identifier, type_name) && !strcmp(type_name, "int"))
                lex_expect(T_identifier);
            type = TY_ushort;
        } else if (lex_peek(T_identifier, type_name) &&
                   (!strcmp(type_name, "char") || !strcmp(type_name, "short") ||
                    !strcmp(type_name, "int"))) {
            lex_expect(T_identifier);
            if (!strcmp(type_name, "char"))
                type = TY_uchar;
            else if (!strcmp(type_name, "short"))
                type = TY_ushort;
            else
                type = TY_uint;
        } else
            type = TY_uint; /* `unsigned` is `unsigned int` */
    } else if (is_long) {
        /* The current ABI gives long the same 32-bit representation as int,
         * while retaining its distinct C type and rank.
         */
        if (lex_peek(T_identifier, type_name) && !strcmp(type_name, "int"))
            lex_expect(T_identifier);
        type = is_long_long ? TY_long_long : TY_long;
    } else if (is_signed && leading_scalar_type &&
               (leading_scalar_type == TY_char ||
                leading_scalar_type == TY_short)) {
        if (leading_scalar_type == TY_short &&
            lex_peek(T_identifier, type_name) && !strcmp(type_name, "int"))
            lex_expect(T_identifier);
        type = leading_scalar_type == TY_char ? TY_schar : leading_scalar_type;
    } else if (is_signed && lex_peek(T_identifier, type_name) &&
               (!strcmp(type_name, "char") || !strcmp(type_name, "short") ||
                !strcmp(type_name, "int"))) {
        lex_expect(T_identifier);
        type = !strcmp(type_name, "char")
                   ? TY_schar
                   : (!strcmp(type_name, "short") ? TY_short : TY_int);
    } else if (is_signed && find_type_flag == 1 &&
               (!lex_peek(T_identifier, type_name) ||
                (strcmp(type_name, "int") && strcmp(type_name, "char") &&
                 strcmp(type_name, "short")))) {
        type = TY_int;
    } else if (leading_scalar_type) {
        type = leading_scalar_type;
    } else {
        lex_ident(T_identifier, type_name);
        type = find_type_flag == 2 ? find_record_tag(type_name, vd->scope)
                                   : find_visible_type(type_name, vd->scope);
        if (!type && find_type_flag == 2)
            type = find_type(type_name, 2);
        if (find_type_flag == 2 &&
            (!type || !is_record_type ||
             (is_union_type ? type->base_type != TYPE_union
                            : type->base_type != TYPE_struct)))
            error_at("Unknown struct/union type", cur_token_loc());
    }

    if (!type) {
        char message[MAX_LINE_LEN];

        snprintf(message, MAX_LINE_LEN, "Could not find type %s%s",
                 find_type_flag == 2 ? "struct/union " : "", type_name);
        error_at(message, cur_token_loc());
    }

    vd->type = type;
    vd->func_signature = type->ptr_level ? NULL : type->func_signature;
    vd->pointee_func_signature = type->pointee_func_signature;
    vd->is_func = vd->func_signature != NULL;
    vd->is_const_qualified = type->is_const_qualified;
    if (type->func_signature && !type->is_direct_function_type &&
        (type->pointer_const_mask & 1U)) {
        vd->is_const_pointer = true;
        vd->pointer_const_mask |= 1U;
    }
    if (type->array_size) {
        fixed_array_shape_t shape = fixed_array_shape_from_type(type);

        fixed_array_shape_to_var(vd, &shape);
    }
    if (type->pointee_array_size) {
        fixed_array_shape_t shape = fixed_array_shape_from_pointee_type(type);

        fixed_array_shape_to_pointee_var(vd, &shape);
        vd->pointee_array_element_ptr_level =
            type->pointee_array_element_ptr_level;
    }
    if (type->ptr_level && type->ptr_level <= 32)
        vd->is_const_pointer =
            type->pointer_const_mask & (1U << (type->ptr_level - 1));

    /* A qualifier may follow the base type as well as precede it: both "const
     * int" and "int const" qualify the object. Consume it before parsing
     * pointer declarators, where a following const instead qualifies the
     * pointer itself ("int * const").
     */
    while (true) {
        if (lex_accept(T_const))
            is_const = true;
        else if (lex_accept(T_volatile))
            is_volatile = true;
        else if (lex_accept(T_restrict)) {
            /* A typedef-hidden array may be qualified when its elements are
             * pointers. As for `const` and `volatile`, the qualifier then
             * applies to the element type rather than to an array object.
             */
            if (!type->ptr_level &&
                !(type->array_size && type->array_element_ptr_level &&
                  (!type->func_signature ||
                   type->array_element_pointee_func_signature)))
                error_at("restrict requires a pointer type", cur_token_loc());
        } else if (lex_accept(T_inline)) {
            if (is_inline || declaration_inline)
                error_at("duplicate inline function specifier",
                         cur_token_loc());
            is_inline = true;
        } else
            break;
    }
    vd->is_inline = declaration_inline || is_inline;
    vd->is_volatile =
        declaration_volatile || is_volatile || type->is_volatile_qualified;
    if (is_const || declaration_const) {
        if (type->ptr_level) {
            vd->is_const_pointer = true;
            if (type->pointee_func_signature && type->ptr_level == 1)
                vd->pointer_const_mask |= 1U;
        } else
            vd->is_const_qualified = true;
    }

    read_inner_var_decl(vd, anon, is_param, is_record_member);

    if (!parsing_sizeof_function_signature && vd->func_signature &&
        function_signature_has_floating(vd->func_signature))
        error_at("Floating point function types are not yet supported",
                 cur_token_loc());

    /* Typedef aliases preserve floating type identity for composition and
     * sizeof, but no floating value may enter the integer-only IR/ABI path.
     */
    if (vd->type && vd->type->is_floating && !parsing_sizeof_function_signature)
        error_at("Floating point types are not yet supported", cur_token_loc());

    /* A 32-bit target can carry a pointer to an eight-byte object without a
     * paired-value register representation. Keep direct wide objects, arrays,
     * parameters, returns, and function-pointer returns rejected until that
     * lowering exists, but admit declarations such as `long long *p` and
     * typedef aliases used exclusively behind a pointer.
     */
    if (PTR_SIZE < 8 && vd->type && vd->type->base_type == TYPE_long_long &&
        (!(vd->ptr_level || vd->type->ptr_level) || vd->is_func))
        error_at("long long value needs 64-bit target lowering",
                 cur_token_loc());
}

/* starting next_token, need to check the type */
void read_partial_var_decl(var_t *vd, var_t *template)
{
    UNUSED(template);
    read_inner_var_decl(vd, false, false, false);
}

void read_parameter_list_decl(func_t *func, bool anon)
{
    int vn = 0;
    lex_expect(T_open_bracket);

    char token[MAX_ID_LEN];
    /* C99's empty parameter list is deliberately not a prototype. */
    if (lex_accept(T_close_bracket))
        return;
    if (lex_peek(T_identifier, token) && !strcmp(token, "void")) {
        lex_next();
        if (lex_accept(T_close_bracket)) {
            func->has_prototype = true;
            return;
        }
        func->param_defs[vn].type = TY_void;
        func->param_defs[vn].scope = func->return_def.scope;
        read_inner_var_decl(&func->param_defs[vn], anon, true, false);
        if (!func->param_defs[vn].ptr_level && !func->param_defs[vn].is_func &&
            !func->param_defs[vn].array_size)
            error_at("'void' must be the only parameter and unnamed",
                     cur_token_loc());
        vn++;
        if (lex_accept(T_comma) && strict_c99 &&
            lex_peek(T_close_bracket, NULL))
            error_at("trailing comma in parameter list is not permitted in C99",
                     cur_token_loc());
    }

    if (floating_type_starts_here() && !parsing_sizeof_function_signature)
        error_at("Floating point types are not yet supported", cur_token_loc());

    while ((parsing_sizeof_function_signature &&
            (lex_peek(T_float, NULL) || lex_peek(T_double, NULL))) ||
           lex_peek(T_identifier, NULL) || lex_peek(T_const, NULL) ||
           lex_peek(T_volatile, NULL) || lex_peek(T_register, NULL) ||
           lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
           lex_peek(T_long, NULL) || lex_peek(T_struct, NULL) ||
           lex_peek(T_union, NULL) || lex_peek(T_enum, NULL)) {
        /* Check for const qualifier */
        bool is_const = false;
        bool is_register = false;
        if (lex_accept(T_const))
            is_const = true;
        if (lex_accept(T_register))
            is_register = true;

        if (vn >= MAX_PARAMS)
            error_at("Too many parameters", cur_token_loc());
        func->param_defs[vn].scope = func->return_def.scope;
        read_full_var_decl(&func->param_defs[vn], anon, true, false);
        if (func->param_defs[vn].is_inline)
            error_at("inline specifier requires a function declarator",
                     cur_token_loc());
        func->param_defs[vn].is_const_qualified |= is_const;
        func->param_defs[vn].is_register = is_register;
        func->param_defs[vn].is_aggregate_param =
            is_record_type(func->param_defs[vn].type) &&
            !func->param_defs[vn].ptr_level;
        vn++;
        if (lex_accept(T_comma) && strict_c99 &&
            lex_peek(T_close_bracket, NULL))
            error_at("trailing comma in parameter list is not permitted in C99",
                     cur_token_loc());
    }
    func->num_params = vn;

    /* Up to 'MAX_PARAMS' parameters are accepted for the variadic function. */
    if (lex_accept(T_elipsis)) {
        if (strict_c99 && vn == 0)
            error_at("ellipsis requires at least one named parameter in C99",
                     cur_token_loc());
        func->va_args = 1;
    }

    func->has_prototype = true;
    lex_expect(T_close_bracket);
}

void read_literal_param(block_t *parent, basic_block_t *bb)
{
    char combined[MAX_STRING_LEN];
    int length = read_concatenated_string(combined);
    const int index = write_string_symbol(combined, length);

    var_t *vd = require_typed_ptr_var(parent, TY_char, true);
    vd->var_name = gen_name();
    vd->init_val = index;
    vd->is_const_qualified = true;
    vd->is_string_literal = true;
    opstack_push(vd);
    /* String literals are now in .rodata section */
    add_insn(parent, bb, OP_load_rodata_address, vd, NULL, NULL, 0, NULL);
}

/* A character array initialized from a string owns writable object storage;
 * unlike a char * initializer, it must not retain the string literal's
 * read-only address. `parent` selects normal local stores or the synthetic
 * global block used by static-storage arrays.
 */
void parse_string_array_init(var_t *var, block_t *parent, basic_block_t **bb)
{
    char combined[MAX_STRING_LEN];
    int len;
    int count;

    len = read_concatenated_string(combined) + 1;
    if (var->has_unsized_array) {
        var->array_size = len;
        var->has_unsized_array = false;
    } else if (len > var->array_size)
        error_at("String initializer is too long for character array",
                 cur_token_loc());

    /* Elements past the string are zero. Static storage already starts out
     * zeroed, but an automatic array is reinitialized on every entry to its
     * declaration and must clear whatever the slot held before.
     */
    count = parent == GLOBAL_BLOCK ? len : var->array_size;
    for (int i = 0; i < count; i++) {
        var_t *value = require_var(parent);
        var_t *addr;

        value->var_name = gen_name();
        value->init_val = i < len ? (unsigned char) combined[i] : 0;
        value->is_const = true;
        add_insn(parent, *bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        addr = compute_element_address(parent, bb, var, i, 1);
        add_insn(parent, *bb, OP_write, NULL, addr, value, 1, NULL);
    }
}

void parse_wstring_array_init(var_t *var, block_t *parent, basic_block_t **bb)
{
    int values[MAX_STRING_LEN];
    int length;
    int units;

    length = read_wstring_units(values, MAX_STRING_LEN);

    units = length + 1;
    if (var->has_unsized_array) {
        var->array_size = units;
        var->has_unsized_array = false;
    } else if (units > var->array_size)
        error_at("Wide string initializer is too long for array",
                 cur_token_loc());

    for (int i = 0; i < var->array_size; i++) {
        var_t *value = require_typed_var(parent, var->type);
        var_t *addr;

        value->var_name = gen_name();
        value->init_val = i < length ? values[i] : 0;
        value->is_const = true;
        add_insn(parent, *bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        addr = compute_element_address(parent, bb, var, i, var->type->size);
        add_insn(parent, *bb, OP_write, NULL, addr, value, var->type->size,
                 NULL);
    }
}

bool numeric_has_unsigned_suffix(const char *token)
{
    for (int i = 0; token[i]; i++)
        if ((token[i] | 32) == 'u')
            return true;
    return false;
}

/* C99 permits U, L, LL, UL, ULL, LU, and LLU (case-insensitively). Keep this
 * separate from type selection: malformed suffixes must not become a valid wide
 * literal merely because their letters happen to be counted.
 */
bool numeric_suffix_is_valid(const char *suffix)
{
    int pos = 0;

    if ((suffix[pos] | 32) == 'u')
        pos++;
    if ((suffix[pos] | 32) == 'l') {
        pos++;
        if ((suffix[pos] | 32) == 'l')
            pos++;
    }
    if ((suffix[pos] | 32) == 'u')
        pos++;
    return suffix[pos] == '\0';
}

bool numeric_has_long_long_suffix(const char *token)
{
    int long_suffix_count = 0;

    for (int i = 0; token[i]; i++)
        if ((token[i] | 32) == 'l')
            long_suffix_count++;
    return long_suffix_count == 2;
}

int numeric_long_suffix_count(const char *token)
{
    int count = 0;

    for (int i = 0; token[i]; i++)
        if ((token[i] | 32) == 'l')
            count++;
    return count;
}

/* The file-scope constant evaluator is still word-sized. Decide from the token
 * spelling whether it must use the two-word literal path before that evaluator
 * consumes and narrows it.
 */
bool numeric_literal_needs_wide_path(const char *token)
{
    const char *digits = token;
    int count = 0;
    bool is_unsigned = numeric_has_unsigned_suffix(token);

    if (numeric_has_long_long_suffix(token))
        return true;
    if (digits[0] == '0' && (digits[1] | 32) == 'x') {
        digits += 2;
        while (isxdigit(digits[count]))
            count++;
        return count > 8;
    }
    if (digits[0] == '0' && (digits[1] | 32) == 'b') {
        digits += 2;
        while (digits[count] == '0' || digits[count] == '1')
            count++;
        return count > 32;
    }
    if (digits[0] == '0') {
        while (digits[count] >= '0' && digits[count] <= '7')
            count++;
        return count > 12 ||
               (count == 12 && strncmp(digits, "037777777777", count) > 0);
    }
    while (isdigit(digits[count]))
        count++;
    if (count > 10)
        return true;
    if (count < 10)
        return false;
    return strncmp(digits, is_unsigned ? "4294967295" : "2147483647", count) >
           0;
}

/* Some bootstrap stages still materialize the exact decimal 2^31 token as an
 * int bit pattern before the global initializer path sees it. This path was
 * selected from the original token spelling, so restore the required wide
 * candidate type before phase-2 chooses its constant-load width.
 */
void force_wide_global_literal_type(var_t *value, const char *token)
{
    /* The typed global path also handles a one-word unsigned literal such as
     * 0xffffffff: it must retain TY_uint for shifts, comparisons, and division,
     * but it is not a long-long candidate. Promoting it here made every 32-bit
     * target reject a valid C99 unsigned-int initializer merely because that
     * path was selected.
     */
    if (!numeric_literal_needs_wide_path(token))
        return;
    if (value->type->size >= 8)
        return;
    if (PTR_SIZE < 8)
        error_at("long long literal needs 64-bit target lowering",
                 cur_token_loc());
    value->type = numeric_has_unsigned_suffix(token) ||
                          (unsigned int) value->init_val_hi > 0x7fffffffU
                      ? TY_ulong_long
                      : TY_long_long;
}

/* Accumulate a 64-bit token in four 16-bit limbs. Each intermediate stays small
 * enough for the self-hosted compiler's unsigned arithmetic.
 */
bool numeric_mul_add_wide(unsigned int *hi,
                          unsigned int *lo,
                          unsigned int base,
                          unsigned int digit)
{
    unsigned int a = *lo & 0xffffU;
    unsigned int b = *lo >> 16;
    unsigned int c = *hi & 0xffffU;
    unsigned int d = *hi >> 16;
    unsigned int t;

    t = a * base + digit;
    a = t & 0xffffU;
    t = b * base + (t >> 16);
    b = t & 0xffffU;
    t = c * base + (t >> 16);
    c = t & 0xffffU;
    t = d * base + (t >> 16);
    if (t > 0xffffU)
        return false;
    *lo = (b << 16) | a;
    *hi = (t << 16) | c;
    return true;
}

/* The global-initializer fast path historically carries only an int value.
 * Route literals whose C99 candidate is unsigned through the typed path even
 * when they fit in one word: otherwise 0xffffffff is folded as -1 before a
 * right shift, comparison, or division sees its unsigned rank.
 */
bool numeric_literal_needs_typed_global_path(const char *token)
{
    unsigned int hi = 0;
    unsigned int lo = 0;
    int i = 0;
    int base = 10;
    bool is_decimal = true;

    if (numeric_has_unsigned_suffix(token))
        return true;
    if (token[0] == '0') {
        if ((token[1] | 32) == 'x') {
            i = 2;
            base = 16;
            is_decimal = false;
            while (isxdigit(token[i])) {
                char digit = token[i++];

                if (isdigit(digit))
                    digit -= '0';
                else
                    digit = (digit | 32) - 'a' + 10;
                numeric_mul_add_wide(&hi, &lo, base, digit);
            }
        } else if ((token[1] | 32) == 'b') {
            i = 2;
            base = 2;
            is_decimal = false;
            while (token[i] == '0' || token[i] == '1')
                numeric_mul_add_wide(&hi, &lo, base, token[i++] - '0');
        } else {
            base = 8;
            is_decimal = false;
            while (token[i] >= '0' && token[i] <= '7')
                numeric_mul_add_wide(&hi, &lo, base, token[i++] - '0');
        }
    }
    if (is_decimal)
        return false;
    return hi != 0 || lo > 0x7fffffffU;
}

void read_numeric_param(block_t *parent, basic_block_t *bb, bool is_neg)
{
    char token[MAX_TOKEN_LEN];
    unsigned int value = 0;
    unsigned int value_hi = 0;
    int i = 0;
    char c;
    int base = 10;
    bool is_decimal = true;
    bool has_unsigned_suffix;
    int long_suffix_count;
    bool is_long_long;

    lex_ident_n(T_numeric, token, MAX_TOKEN_LEN);
    has_unsigned_suffix = numeric_has_unsigned_suffix(token);
    long_suffix_count = numeric_long_suffix_count(token);
    is_long_long = long_suffix_count >= 2;

    if (token[0] == '-') {
        is_neg = !is_neg;
        i++;
    }
    if (token[0] == '0') {
        if ((token[1] | 32) == 'x') { /* hexdecimal */
            i = 2;
            base = 16;
            is_decimal = false;
            do {
                c = token[i++];
                if (isdigit(c))
                    c -= '0';
                else {
                    c |= 32; /* convert to lower case */
                    if (c >= 'a' && c <= 'f')
                        c = (c - 'a') + 10;
                    else
                        error_at("Invalid numeric constant", cur_token_loc());
                }

                if (!numeric_mul_add_wide(&value_hi, &value, base, c))
                    error_at("Integer literal exceeds supported range",
                             cur_token_loc());
            } while (isxdigit(token[i]));
        } else if ((token[1] | 32) == 'b') { /* binary */
            i = 2;
            base = 2;
            is_decimal = false;
            do {
                c = token[i++];
                if (c != '0' && c != '1')
                    error_at("Invalid binary constant", cur_token_loc());
                c -= '0';
                if (!numeric_mul_add_wide(&value_hi, &value, base, c))
                    error_at("Integer literal exceeds supported range",
                             cur_token_loc());
            } while (token[i] == '0' || token[i] == '1');
        } else { /* octal */
            base = 8;
            is_decimal = false;
            do {
                c = token[i++];
                if (c > '7')
                    error_at("Invalid numeric constant", cur_token_loc());
                c -= '0';
                if (!numeric_mul_add_wide(&value_hi, &value, base, c))
                    error_at("Integer literal exceeds supported range",
                             cur_token_loc());
            } while (isdigit(token[i]));
        }
    } else {
        do {
            c = token[i++] - '0';
            if (!numeric_mul_add_wide(&value_hi, &value, base, c))
                error_at("Integer literal exceeds supported range",
                         cur_token_loc());
        } while (isdigit(token[i]));
    }

    if (!numeric_suffix_is_valid(token + i))
        error_at("Invalid integer literal suffix", cur_token_loc());

    /* Decimal constants have only signed candidates unless they carry U: C99
     * may not silently select unsigned long long for 2^63 or above. The one
     * exception is the magnitude in the standard spelling of LLONG_MIN, where
     * the separately parsed unary minus consumes exactly 2^63.
     */
    if (is_decimal && !has_unsigned_suffix &&
        (value_hi > 0x80000000U ||
         (value_hi == 0x80000000U && (value != 0 || !is_neg))))
        error_at("Decimal integer literal exceeds signed long long range",
                 cur_token_loc());

    /* C99 gives the magnitude in -2147483648 type long long. A target without
     * paired 64-bit lowering cannot hold a long long value, yet the negated
     * result is INT_MIN, which int represents exactly; typing that one spelling
     * as int keeps INT_MIN usable there instead of rejecting it.
     */
    bool narrow_int_min = PTR_SIZE < 8 && is_neg && is_decimal &&
                          !has_unsigned_suffix && !long_suffix_count &&
                          !value_hi && value == 0x80000000U;

    var_t *vd = require_var(parent);
    vd->var_name = gen_name();
    if (narrow_int_min) {
        vd->type = TY_int;
    } else if (is_long_long || value_hi ||
               (is_decimal && !has_unsigned_suffix &&
                numeric_literal_needs_wide_path(token)) ||
               (is_decimal && !has_unsigned_suffix && value > 0x80000000U)) {
        if (PTR_SIZE < 8)
            error_at("long long literal needs 64-bit target lowering",
                     cur_token_loc());
        if (has_unsigned_suffix || (!is_decimal && value_hi > 0x7fffffffU))
            vd->type = TY_ulong_long;
        else
            vd->type = TY_long_long;
    } else if (has_unsigned_suffix || (!is_decimal && value > 0x7fffffffU))
        vd->type = long_suffix_count ? TY_ulong : TY_uint;
    else if (long_suffix_count)
        vd->type = TY_long;

    /* Unary minus is applied after candidate selection. In particular,
     * `-2147483648` is a negated long-long literal on this 32-bit-long ABI,
     * rather than a nonstandard signed-int bit pattern.
     */
    if (is_neg) {
        value = 0 - value;
        value_hi = ~value_hi + (value == 0);
    }
    vd->init_val = value;
    vd->init_val_hi = value_hi;
    vd->is_const = true;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

void read_char_param(block_t *parent, basic_block_t *bb)
{
    char literal[MAX_TOKEN_LEN], unescaped[MAX_TOKEN_LEN];

    lex_ident(T_char, literal);
    unescape_string(literal, unescaped, MAX_TOKEN_LEN);

    var_t *vd = require_typed_var(parent, TY_int);
    vd->var_name = gen_name();
    vd->init_val = parse_character_constant(literal);
    vd->is_const = true;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

/* The current execution wide-character representation is int. Decode the same
 * escape spelling as an ordinary character constant; a wide string needs
 * separate element-array lowering and is intentionally not accepted here.
 */
void read_wchar_param(block_t *parent, basic_block_t *bb)
{
    char literal[MAX_TOKEN_LEN], unescaped[MAX_TOKEN_LEN];

    lex_ident(T_wchar, literal);
    unescape_string(literal, unescaped, MAX_TOKEN_LEN);

    var_t *vd = require_typed_var(parent, TY_int);
    vd->var_name = gen_name();
    vd->init_val = parse_wide_character_constant(literal);
    vd->is_const = true;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

void read_wstring_param(block_t *parent, basic_block_t *bb)
{
    int values[MAX_STRING_LEN];
    int length = read_wstring_units(values, MAX_STRING_LEN);

    var_t *vd = require_typed_ptr_var(parent, find_type("wchar_t", true), 1);
    vd->var_name = gen_name();
    vd->init_val = write_wide_symbol(values, length);
    vd->is_const_qualified = true;
    vd->is_string_literal = true;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_rodata_address, vd, NULL, NULL, 0, NULL);
}
