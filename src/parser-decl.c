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

/* The outermost derivation a sizeof abstract declarator applies. It alone
 * decides the object size: a pointer hides the type it points to, an array only
 * multiplies its element, and a function type has no object size at all.
 */
typedef enum {
    SIZEOF_DERIVED_NONE,
    SIZEOF_DERIVED_POINTER,
    SIZEOF_DERIVED_FUNCTION
} sizeof_derivation_t;

/* Consume one array bound of a sizeof type name after its '[' and return the
 * element count scaled by it. VLA is outside shecc's C99 scope.
 */
static int read_sizeof_array_bound(block_t *scope, int elements)
{
    int bound = read_const_expr(scope);

    lex_expect(T_close_square);
    if (bound <= 0)
        error_at("sizeof array type needs a positive constant bound",
                 cur_token_loc());
    if (elements > INT_MAX / bound)
        error_at("sizeof array type is too large", cur_token_loc());
    return elements * bound;
}

/* Consume the abstract declarator of a sizeof type name, such as the "*[2]" of
 * `int *[2]` or the "(*(*[2])(int))[3]" of `int (*(*[2])(int))[3]`, and return
 * its outermost derivation. *elements receives the element count of the arrays
 * that derivation builds, or 1.
 *
 * A declarator reads inside out: the pointers of one level apply first, then
 * that level's suffixes, and a parenthesized inner declarator applies last. An
 * inner pointer or function derivation therefore decides the whole type, while
 * an inner declarator with neither only adds array bounds.
 */
sizeof_derivation_t read_sizeof_abstract_declarator(block_t *scope,
                                                    int *elements)
{
    sizeof_derivation_t derivation = SIZEOF_DERIVED_NONE;
    sizeof_derivation_t inner = SIZEOF_DERIVED_NONE;
    int inner_elements = 1;
    int count = 1;
    bool nested = false;
    bool has_array = false;

    while (lex_accept(T_asterisk)) {
        derivation = SIZEOF_DERIVED_POINTER;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }

    /* A parenthesis opens an inner declarator only where one can begin;
     * otherwise it is the parameter list of a function suffix.
     */
    if (lex_peek(T_open_bracket, NULL) && cur_token->next->next &&
        (cur_token->next->next->kind == T_asterisk ||
         cur_token->next->next->kind == T_open_bracket ||
         cur_token->next->next->kind == T_open_square)) {
        lex_expect(T_open_bracket);
        inner = read_sizeof_abstract_declarator(scope, &inner_elements);
        lex_expect(T_close_bracket);
        nested = true;
    }

    while (true) {
        if (lex_accept(T_open_square)) {
            if (derivation == SIZEOF_DERIVED_FUNCTION)
                error_at("function cannot return an array", cur_token_loc());
            count = read_sizeof_array_bound(scope, count);
            has_array = true;
        } else if (lex_peek(T_open_bracket, NULL)) {
            if (derivation == SIZEOF_DERIVED_FUNCTION)
                error_at("function cannot return a function", cur_token_loc());
            if (has_array)
                error_at("array of functions is invalid", cur_token_loc());
            read_sizeof_function_prototype();
            derivation = SIZEOF_DERIVED_FUNCTION;
        } else
            break;
    }

    if (nested && inner != SIZEOF_DERIVED_NONE) {
        *elements = inner_elements;
        return inner;
    }
    if (count > INT_MAX / inner_elements)
        error_at("sizeof array type is too large", cur_token_loc());
    *elements = count * inner_elements;
    return derivation;
}

/* Return the size of a sizeof type name: its specifier type with the derivation
 * read by read_sizeof_abstract_declarator() applied.
 */
int sizeof_type_name_size(const type_t *type,
                          sizeof_derivation_t derivation,
                          int elements)
{
    int size;

    if (derivation == SIZEOF_DERIVED_FUNCTION)
        error_at("sizeof(function) is invalid", cur_token_loc());
    if (derivation == SIZEOF_DERIVED_POINTER) {
        size = PTR_SIZE;
    } else {
        if (type == TY_void)
            error_at("sizeof(void) is invalid", cur_token_loc());
        if (type->is_direct_function_type)
            error_at("sizeof(function) is invalid", cur_token_loc());

        /* A typedef names a record through base_struct; an incomplete record
         * has no size to report.
         */
        size = type->size;
        if (!size && type->base_struct)
            size = type->base_struct->size;
        if (!size)
            error_at("sizeof cannot be applied to an incomplete type",
                     cur_token_loc());
        if (type->array_size) {
            if (size > INT_MAX / type->array_size)
                error_at("sizeof array type is too large", cur_token_loc());
            size *= type->array_size;
        }
    }
    if (size > INT_MAX / elements)
        error_at("sizeof array type is too large", cur_token_loc());
    return size * elements;
}

/* Consume the scalar type specifiers at the next token, together with the
 * qualifiers mixed in among them, which set @is_const and @is_volatile, and the
 * inline specifier when @is_inline is not NULL. C99 6.7.2p2 lets these words
 * appear in any order: "unsigned long int", "long unsigned int" and "int
 * unsigned long" all name the same type.
 *
 * Returns the scalar type named, or NULL when no type word was read, which
 * leaves a struct, union, enum or typedef name to the caller.
 */
type_t *read_scalar_type_specifiers(bool *is_const,
                                    bool *is_volatile,
                                    bool *is_inline)
{
    char token[MAX_ID_LEN];
    type_t *type;
    bool is_signed = false;
    bool is_unsigned = false;
    int long_count = 0;
    bool has_int = false;
    type_t *base = NULL;

    while (true) {
        if (lex_accept(T_signed)) {
            if (is_signed)
                error_at("duplicate signed type specifier", cur_token_loc());
            is_signed = true;
        } else if (lex_accept(T_unsigned)) {
            if (is_unsigned)
                error_at("duplicate unsigned type specifier", cur_token_loc());
            is_unsigned = true;
        } else if (lex_accept(T_long)) {
            if (++long_count > 2)
                error_at("too many long type specifiers", cur_token_loc());
        } else if (lex_accept(T_const)) {
            *is_const = true;
        } else if (lex_accept(T_volatile)) {
            *is_volatile = true;
        } else if (is_inline && lex_accept(T_inline)) {
            if (*is_inline)
                error_at("duplicate inline function specifier",
                         cur_token_loc());
            *is_inline = true;
        } else if (lex_peek(T_identifier, token) && !strcmp(token, "int")) {
            /* `char`, `short` and `int` reach the parser as identifiers. `int`
             * may also accompany `short` or `long`.
             */
            if (has_int)
                error_at("duplicate type specifier", next_token_loc());
            lex_expect(T_identifier);
            has_int = true;
        } else if (lex_peek(T_float, NULL) || lex_peek(T_double, NULL) ||
                   (lex_peek(T_identifier, token) &&
                    (!strcmp(token, "char") || !strcmp(token, "short")))) {
            if (base)
                error_at("duplicate type specifier", next_token_loc());
            if (lex_accept(T_float))
                base = TY_float;
            else if (lex_accept(T_double))
                base = TY_double;
            else {
                lex_expect(T_identifier);
                base = token[0] == 'c' ? TY_char : TY_short;
            }
        } else
            break;
    }
    if (!base && !has_int && !is_signed && !is_unsigned && !long_count)
        return NULL;
    if (is_signed && is_unsigned)
        error_at("both signed and unsigned specified", cur_token_loc());
    if (base == TY_float && (is_signed || is_unsigned || long_count || has_int))
        error_at("invalid float type specifiers", cur_token_loc());
    if (base == TY_double &&
        (is_signed || is_unsigned || long_count > 1 || has_int))
        error_at("invalid double type specifiers", cur_token_loc());
    if (base == TY_char && has_int)
        error_at("int cannot be combined with char", cur_token_loc());
    if (base == TY_char && long_count)
        error_at("long cannot be combined with char", cur_token_loc());
    if (base == TY_short && long_count)
        error_at("long cannot be combined with short", cur_token_loc());
    if (lex_peek(T_enum, NULL))
        error_at("enum type cannot be combined with integer specifiers",
                 next_token_loc());
    if (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL))
        error_at("record type cannot be combined with integer specifiers",
                 next_token_loc());

    if (base == TY_float)
        type = TY_float;
    else if (base == TY_double)
        type = long_count ? TY_long_double : TY_double;
    else if (base == TY_char)
        type = is_unsigned ? TY_uchar : (is_signed ? TY_schar : TY_char);
    else if (base == TY_short)
        type = is_unsigned ? TY_ushort : TY_short;
    else if (long_count == 2)
        type = is_unsigned ? TY_ulong_long : TY_long_long;
    else if (long_count)
        type = is_unsigned ? TY_ulong : TY_long;
    else
        type = is_unsigned ? TY_uint : TY_int;
    return type;
}

/* Consume the specifier list of a type name used only for its metadata, and
 * return the type it names, or NULL when none of it names a type.
 */
type_t *read_type_name_specifiers(block_t *scope)
{
    char token[MAX_ID_LEN];
    bool is_const = false;
    bool is_volatile = false;
    type_t *type = read_scalar_type_specifiers(&is_const, &is_volatile, NULL);
    base_type_t record_kind;

    if (type)
        return type;
    record_kind = accept_record_keyword();
    if (record_kind) {
        lex_ident(T_identifier, token);
        type = find_record_tag(token, scope, record_kind);
    } else if (lex_accept(T_enum)) {
        lex_ident(T_identifier, token);
        type = reference_enum_tag(token, scope);
    } else if (lex_peek(T_identifier, token)) {
        type = find_visible_type(token, scope);
        if (type)
            lex_expect(T_identifier);
    }
    /* A qualifier may follow the record, enum or typedef name as well. */
    while (lex_accept(T_const) || lex_accept(T_volatile) ||
           lex_accept(T_restrict))
        ;
    return type;
}

/* Integer constant expressions may contain sizeof(type-name). This parser only
 * needs type metadata, so keep it separate from expression lowering and avoid
 * emitting the otherwise unevaluated sizeof IR into an array bound or
 * enumerator declaration.
 */
int read_const_sizeof_type(block_t *scope)
{
    type_t *type;
    sizeof_derivation_t derivation;
    int elements;

    lex_expect(T_open_bracket);
    type = read_type_name_specifiers(scope);
    if (!type)
        error_at(
            "sizeof in an integer constant expression requires a type name",
            cur_token_loc());
    derivation = read_sizeof_abstract_declarator(scope, &elements);
    lex_expect(T_close_bracket);
    return sizeof_type_name_size(type, derivation, elements);
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
    if (lex_accept(T_sizeof))
        return read_sizeof_constant(scope);

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
            base_type_t record_kind = accept_record_keyword();
            if (record_kind) {
                lex_ident(T_identifier, type_name);
                record = find_record_tag(type_name, scope, record_kind);
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
        bool inner_array = false;

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

            inner_array = true;
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

        /* With neither a parameter list nor an array suffix after it, the
         * parentheses only group the declarator: `int (*p)` is `int *p`, and
         * `int (*a[2])` is `int *a[2]`.
         */
        if (!lex_peek(T_open_bracket, NULL)) {
            if (vd->type->array_size && !vd->ptr_level) {
                fixed_array_shape_t shape =
                    fixed_array_shape_from_type(vd->type);
                fixed_array_shape_t empty_shape = {0};

                /* A pointer to an array typedef, as after the plain stars. */
                if (inner_array)
                    error_at(
                        "array of pointers to an array typedef is not yet "
                        "supported",
                        cur_token_loc());
                fixed_array_shape_to_pointee_var(vd, &shape);
                fixed_array_shape_to_var(vd, &empty_shape);
            }
            vd->ptr_level += nested_ptr_level;
            vd->parenthesized_function_pointer_const = false;
            vd->parenthesized_function_pointer_restrict = false;
            vd->parenthesized_function_pointer_outer_const = false;
            vd->parenthesized_function_pointer_outer_volatile = false;
            vd->parenthesized_function_pointer_outer_restrict = false;
            vd->parenthesized_function_pointer_inner_qualified = false;
            if (vd->ptr_level == 1 && !vd->array_size &&
                vd->type->is_direct_function_type && vd->type->func_signature)
                vd->func_signature = vd->type->func_signature;
            else
                vd->func_signature = NULL;
            vd->is_func = false;
            if (vd->array_size > 0 && !vd->ptr_level && !vd->type->ptr_level &&
                type_has_flexible_array_member(vd->type))
                error_at(
                    "A struct with a flexible array member cannot be an "
                    "array element",
                    cur_token_loc());

            /* As for an unparenthesized name, the global initializer parser
             * finds a file-scope object on the operand stack.
             */
            if (vd->is_global && vd->var_name && !is_param)
                opstack_push(vd);
            if (vd->ptr_level > 0 && vd->ptr_level <= 32)
                vd->is_const_pointer =
                    vd->pointer_const_mask & (1U << (vd->ptr_level - 1));
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

        /* An object whose type is an array typedef is an array of that
         * typedef's rows: `typedef int row[2]; row m[3]` is `int m[3][2]`. Only
         * the first declarator had row's bounds copied in by
         * read_full_var_decl(), so restate them for `row a, b` too. A direct
         * suffix is read on its own and the row bounds appended to it after the
         * loop below. Block typedefs compose their own aliases in
         * compose_block_typedef_array().
         */
        fixed_array_shape_t element_shape = {0};
        if (vd->type && vd->type->array_size && !vd->ptr_level &&
            !parsing_block_typedef_declarator) {
            element_shape = fixed_array_shape_from_type(vd->type);
            fixed_array_shape_to_var(vd, &element_shape);
            if (lex_peek(T_open_square, NULL)) {
                vd->array_size = 0;
                vd->array_dim2 = vd->array_dim3 = vd->array_dim4 = 0;
            } else
                element_shape.rank = 0;
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
        if (element_shape.rank) {
            fixed_array_shape_t decl_shape = fixed_array_shape_from_var(vd);
            fixed_array_shape_t shape;

            /* An omitted outer bound counts as one row, which is how an unsized
             * `int t[][2]` records its inner extent as well.
             */
            if (first_dim_empty) {
                if (!decl_shape.rank)
                    decl_shape.rank = 1;
                decl_shape.bounds[0] = 1;
            }
            shape = fixed_array_shape_prepend(&decl_shape, &element_shape);
            fixed_array_shape_to_var(vd, &shape);
            dims += element_shape.rank;
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

/* C99 6.7p2 lets the storage-class specifiers appear anywhere among the
 * declaration specifiers: "int static x;" is "static int x;" and "unsigned
 * register int z;" is "register unsigned int z;". The order carries no meaning,
 * and every declaration reader looks for storage classes before the type, so
 * move each one that follows another specifier to the front of the declaration,
 * right after the current token.
 *
 * The scan covers only tokens that can continue the specifiers: qualifiers,
 * inline, scalar keywords, a struct, union or enum specifier with its body, and
 * one type name. A scalar type word may follow another, as in "int short", but
 * any other identifier after the type is the declarator, which ends the scan,
 * so "int x static;" stays an error.
 */
void hoist_storage_class_specifiers(void)
{
    token_t *insert = cur_token;
    token_t *prev = cur_token;
    bool saw_specifier = false;
    bool saw_type_name = false;

    while (prev->next) {
        token_t *tk = prev->next;
        token_kind_t kind = tk->kind;

        if (kind == T_static || kind == T_extern || kind == T_register ||
            kind == T_auto || kind == T_typedef) {
            if (saw_specifier) {
                prev->next = tk->next;
                tk->next = insert->next;
                insert->next = tk;
            } else
                prev = tk;
            insert = tk;
            continue;
        }
        if (kind == T_struct || kind == T_union || kind == T_enum) {
            prev = tk;
            if (prev->next && prev->next->kind == T_identifier)
                prev = prev->next;
            if (prev->next && prev->next->kind == T_open_curly) {
                int depth = 0;

                do {
                    prev = prev->next;
                    if (!prev)
                        return;
                    if (prev->kind == T_open_curly)
                        depth++;
                    else if (prev->kind == T_close_curly)
                        depth--;
                } while (depth);
            }
            saw_specifier = true;
            saw_type_name = true;
            continue;
        }
        if (kind == T_identifier) {
            /* `char`, `short` and `int` are keywords that reach the parser as
             * identifiers, so they never begin the declarator.
             */
            if (saw_type_name && strcmp(tk->literal, "int") &&
                strcmp(tk->literal, "short") && strcmp(tk->literal, "char"))
                return;
            saw_type_name = true;
        } else if (kind != T_const && kind != T_volatile &&
                   kind != T_restrict && kind != T_inline && kind != T_signed &&
                   kind != T_unsigned && kind != T_long && kind != T_float &&
                   kind != T_double)
            return;
        saw_specifier = true;
        prev = tk;
    }
}

/* C99 6.7 lets declaration specifiers appear in any order, so a type qualifier
 * may follow a struct, union, enum or typedef name as well as precede it:
 * `struct S volatile s` qualifies s exactly as `volatile struct S s` does.
 * Callers that read such a specifier themselves use this to collect the
 * qualifiers after it, leaving the declarator's stars and their own qualifiers
 * to read_inner_var_decl(). restrict qualifies only a pointer, so it is valid
 * here only when @base_is_pointer says the specifier names one.
 */
void read_type_qualifiers(bool *is_const,
                          bool *is_volatile,
                          bool base_is_pointer)
{
    for (;;) {
        if (lex_accept(T_const))
            *is_const = true;
        else if (lex_accept(T_volatile))
            *is_volatile = true;
        else if (lex_accept(T_restrict)) {
            if (!base_is_pointer)
                error_at("restrict requires a pointer type", cur_token_loc());
        } else
            return;
    }
}

void read_full_var_decl(var_t *vd,
                        bool anon,
                        bool is_param,
                        bool is_record_member);

/* The member list of a struct or union of @kind, starting at its opening brace,
 * declared in block @parent or at file scope when @parent is NULL. A tagged
 * definition completes the tag @token of that scope; an untagged one names a
 * type no other declaration can reach, so it needs no tag table entry. A member
 * may itself define a record, whose tag then belongs to the same scope, since a
 * member list opens no scope of its own.
 *
 * Returns the completed record type.
 */
type_t *read_record_body(block_t *parent,
                         base_type_t kind,
                         bool has_tag,
                         char token[])
{
    type_t *type;
    bool is_union = kind == TYPE_union;
    int i = 0;
    int size = 0;
    int alignment = 1;
    int max_size = 0;
    bitfield_layout_t bits = {0};
    bool has_flexible_array_member = false;
    block_t *scope = parent ? parent : GLOBAL_BLOCK;

    if (has_tag) {
        type = local_record_tag(token, scope, kind);
        begin_record_definition(type);
    } else {
        type = add_type();
        type->base_type = kind;
    }

    lex_expect(T_open_curly);
    do {
        var_t *v = type_add_field(type, &i);
        var_t *last = v;

        /* A member's type names tags visible in this scope. */
        v->scope = parent;
        read_full_var_decl(v, false, false, true);

        /* Each declarator of the member declaration, sharing its specifier. */
        while (true) {
            read_bitfield_width(last, scope);

            /* C99 6.7.2.1p2 keeps a struct with a flexible array member out of
             * a struct or array, but a union may hold one and then inherits it.
             */
            if (is_union)
                has_flexible_array_member |=
                    is_flexible_array_member_container(last);
            else
                reject_flexible_array_member_container(last);
            mark_flexible_array_member(last, is_union);
            if (is_union) {
                last->offset = 0;
                int field_size =
                    is_bitfield(last)
                        ? (last->bit_width ? last->bit_storage_size : 0)
                        : size_var(last);
                if (field_size > max_size)
                    max_size = field_size;
                if (alignment_var(last) > alignment)
                    alignment = alignment_var(last);
            } else {
                size =
                    is_bitfield(last)
                        ? layout_bitfield_field(size, last, &alignment, &bits)
                        : layout_struct_field(
                              flush_bitfield_layout(size, &bits), last,
                              &alignment);
            }
            if (!lex_accept(T_comma))
                break;
            if (!is_union && last->is_flexible_array_member)
                error_at(
                    "Flexible array member must be the final struct member",
                    cur_token_loc());
            last = type_add_field(type, &i);
            initialize_struct_field(last, v, 0);
            read_inner_var_decl(last, false, false, true);
        }

        lex_expect(T_semicolon);
        if (!is_union && last->is_flexible_array_member) {
            if (!lex_peek(T_close_curly, NULL))
                error_at(
                    "Flexible array member must be the final struct member",
                    cur_token_loc());
            if (i == 1)
                error_at(
                    "Struct needs a named member before its flexible array "
                    "member",
                    cur_token_loc());
            has_flexible_array_member = true;
        }
    } while (!lex_accept(T_close_curly));

    type->alignment = alignment;
    type->size = is_union
                     ? ALIGN_UP(max_size, alignment)
                     : ALIGN_UP(flush_bitfield_layout(size, &bits), alignment);
    type->num_fields = i;
    type->is_union = is_union;
    type->has_flexible_array_member = has_flexible_array_member;
    return type;
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
    bool is_const = false;
    bool is_inline = declaration_inline;
    bool is_volatile = false;
    type_t *type =
        read_scalar_type_specifiers(&is_const, &is_volatile, &is_inline);

    if (!type && lex_accept(T_enum)) {
        lex_ident(T_identifier, type_name);
        type = reference_enum_tag(type_name, vd->scope);
    } else if (!type) {
        base_type_t record_kind = accept_record_keyword();

        if (record_kind && is_record_member &&
            (lex_peek(T_open_curly, NULL) ||
             (lex_peek(T_identifier, type_name) &&
              cur_token->next->next->kind == T_open_curly))) {
            /* A member may define the record it has, tagged or not, as in
             * `struct { int a; } in;`.
             */
            bool has_tag = lex_accept(T_identifier);

            type = read_record_body(vd->scope, record_kind, has_tag, type_name);
        } else {
            lex_ident(T_identifier, type_name);
            type = record_kind
                       ? reference_record_tag(type_name, vd->scope, record_kind)
                       : find_visible_type(type_name, vd->scope);
        }
    }

    if (!type) {
        char message[MAX_LINE_LEN];

        snprintf(message, MAX_LINE_LEN, "Could not find type %s", type_name);
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
            if (is_inline)
                error_at("duplicate inline function specifier",
                         cur_token_loc());
            is_inline = true;
        } else
            break;
    }
    vd->is_inline = is_inline;
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
}

/* starting next_token, need to check the type */
void read_partial_var_decl(var_t *vd, var_t *template)
{
    UNUSED(template);
    read_inner_var_decl(vd, false, false, false);
}

/* Consume what follows one parameter declaration. Parameters are separated by
 * commas, and a comma must introduce another parameter or the ellipsis: C99
 * 6.7.5 has no trailing comma in a parameter list, in any dialect gcc accepts.
 *
 * Returns true when a comma was read, so another parameter or '...' follows.
 */
static bool read_parameter_separator(void)
{
    if (!lex_accept(T_comma))
        return false;
    if (lex_peek(T_close_bracket, NULL))
        error_at("trailing comma in parameter list", cur_token_loc());
    return true;
}

void read_parameter_list_decl(func_t *func, bool anon)
{
    int vn = 0;
    bool expect_parameter = false;
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
        expect_parameter = read_parameter_separator();
    }

    if (floating_type_starts_here() && !parsing_sizeof_function_signature)
        error_at("Floating point types are not yet supported", cur_token_loc());

    while ((vn == 0 || expect_parameter) &&
           ((parsing_sizeof_function_signature &&
             (lex_peek(T_float, NULL) || lex_peek(T_double, NULL))) ||
            lex_peek(T_identifier, NULL) || lex_peek(T_const, NULL) ||
            lex_peek(T_volatile, NULL) || lex_peek(T_register, NULL) ||
            lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
            lex_peek(T_long, NULL) || lex_peek(T_struct, NULL) ||
            lex_peek(T_union, NULL) || lex_peek(T_enum, NULL))) {
        /* Check for const qualifier */
        bool is_const = false;
        bool is_register = false;
        hoist_storage_class_specifiers();
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
        expect_parameter = read_parameter_separator();
    }
    func->num_params = vn;

    /* Up to 'MAX_PARAMS' parameters are accepted for the variadic function.
     * After a named parameter the ellipsis needs its own comma.
     */
    if ((vn == 0 || expect_parameter) && lex_accept(T_elipsis)) {
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
    } else if (len - 1 > var->array_size)
        error_at("String initializer is too long for character array",
                 cur_token_loc());

    /* The terminating null is dropped when the array has room only for the
     * characters (C99 6.7.8p14), so never store past the array.
     */
    if (len > var->array_size)
        len = var->array_size;

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

    /* As for a char array, the terminating null may be dropped (C99 6.7.8p15).
     */
    units = length + 1;
    if (var->has_unsized_array) {
        var->array_size = units;
        var->has_unsized_array = false;
    } else if (length > var->array_size)
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
