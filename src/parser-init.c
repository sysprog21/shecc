/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Addresses of elements and members, record copies, aggregate and string
 * initializers, and compound literals.
 *
 * A fragment of the parser: parser.c includes it in order, so it sees every
 * definition that precedes it there and cannot be compiled on its own.
 */

void read_parameter_list_decl(func_t *func, bool anon);
void read_indirect_call(var_t *callee, block_t *parent, basic_block_t **bb);
var_t *integer_promote_operand(block_t *parent, basic_block_t **bb, var_t *var);
var_t *resolve_global_declarator(block_t *block,
                                 var_t *var,
                                 bool is_static,
                                 bool *is_redeclaration);
bool read_global_function_declarator(block_t *block,
                                     var_t *var,
                                     bool is_static,
                                     bool allow_definition);
var_t *bind_block_extern_object(block_t *parent, var_t *var);
int read_const_expr(block_t *scope);
var_t *read_wide_global_literal_expression(block_t *parent,
                                           basic_block_t *bb,
                                           block_t *scope);

/* Forward declaration for ternary handling used by initializers */
void read_ternary_operation(block_t *parent, basic_block_t **bb);

/* Parse array initializer to determine size for implicit arrays and optionally
 * emit initialization code.
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
    offset->var_name = gen_name();
    offset->init_val = index * elem_size;
    add_insn(parent, *bb, OP_load_constant, offset, NULL, NULL, 0, NULL);

    var_t *addr = require_var(parent);
    addr->var_name = gen_name();
    add_insn(parent, *bb, OP_add, addr, base_addr, offset, 0, NULL);
    return addr;
}

var_t *compute_field_address(block_t *parent,
                             basic_block_t **bb,
                             var_t *struct_addr,
                             const var_t *field)
{
    if (field->offset == 0)
        return struct_addr;

    var_t *offset = require_var(parent);
    offset->var_name = gen_name();
    offset->init_val = field->offset;
    add_insn(parent, *bb, OP_load_constant, offset, NULL, NULL, 0, NULL);

    var_t *addr = require_var(parent);
    addr->var_name = gen_name();
    add_insn(parent, *bb, OP_add, addr, struct_addr, offset, 0, NULL);
    return addr;
}

/* A record assignment is a value copy, not the scalar OP_assign used for
 * ordinary variables. Keep the lowering in phase 1 so every backend can use its
 * existing 1-, 2-, and 4-byte indirect accesses.
 */
bool is_record_object(const var_t *var)
{
    return var && !var->ptr_level && !var->array_size && var->type &&
           (var->type->base_type == TYPE_struct ||
            var->type->base_type == TYPE_union ||
            (var->type->base_type == TYPE_typedef &&
             var->type->num_fields > 0));
}

/* Copy size bytes between two known record addresses in 4-, 2- and 1-byte
 * slices, the widest indirect accesses every backend encodes. A record is an
 * object, not an integer value, so one OP_write of its whole size would leave
 * narrow backends with a store width they cannot emit.
 */
void emit_record_copy_between(block_t *parent,
                              basic_block_t **bb,
                              var_t *dest_addr,
                              var_t *src_addr,
                              int size)
{
    for (int offset = 0; offset < size;) {
        int width = 1;
        if (size - offset >= 4)
            width = 4;
        else if (size - offset >= 2)
            width = 2;
        var_t *src_part =
            compute_element_address(parent, bb, src_addr, offset, 1);
        var_t *dest_part =
            compute_element_address(parent, bb, dest_addr, offset, 1);
        var_t *value = require_var(parent);

        value->var_name = gen_name();
        add_insn(parent, *bb, OP_read, value, src_part, NULL, width, NULL);
        add_insn(parent, *bb, OP_write, NULL, dest_part, value, width, NULL);
        offset += width;
    }
}

/* Copy a record into an address which is already known, such as a nested record
 * member.
 */
void emit_record_copy_to_address(block_t *parent,
                                 basic_block_t **bb,
                                 var_t *dest_addr,
                                 var_t *src)
{
    var_t *src_addr = require_ref_var(parent, src->type, 0);

    src_addr->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, src_addr, src, NULL, 0, NULL);
    emit_record_copy_between(parent, bb, dest_addr, src_addr, size_var(src));
}

void emit_record_copy(block_t *parent,
                      basic_block_t **bb,
                      var_t *dest,
                      var_t *src)
{
    var_t *dest_addr = require_ref_var(parent, dest->type, 0);
    var_t *src_addr = require_ref_var(parent, src->type, 0);

    dest_addr->var_name = gen_name();
    src_addr->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, dest_addr, dest, NULL, 0, NULL);
    add_insn(parent, *bb, OP_address_of, src_addr, src, NULL, 0, NULL);
    emit_record_copy_between(parent, bb, dest_addr, src_addr, size_var(dest));
}

void emit_object_assignment(block_t *parent,
                            basic_block_t **bb,
                            var_t *dest,
                            var_t *src)
{
    if (is_record_object(dest) && is_record_object(src)) {
        emit_record_copy(parent, bb, dest, src);
    } else if (dest->is_func && src->is_func &&
               find_var(src->var_name, parent) != src) {
        /* Function symbols are not scalar values: materialize their final
         * address through OP_write, which the backend patches after laying out
         * all functions. This is also needed for a declaration initializer such
         * as `int (*fn)(int) = target;`.
         */
        var_t *dest_addr = require_ref_var(parent, dest->type, dest->ptr_level);
        dest_addr->var_name = gen_name();
        add_insn(parent, *bb, OP_address_of, dest_addr, dest, NULL, 0, NULL);
        add_insn(parent, *bb, OP_write, NULL, dest_addr, src, PTR_SIZE, NULL);
    } else {
        src = resize_var(parent, bb, src, dest);
        add_insn(parent, *bb, OP_assign, dest, src, NULL, 0, NULL);
    }
}

type_t *read_type_name_specifiers(block_t *scope);
int read_const_expr_operand(block_t *scope);
int read_global_address_offset(block_t *scope,
                               block_t *parent,
                               basic_block_t *bb);

/* While the operand of a pointer cast in a static initializer is read, the size
 * of what the cast pointer points to, and 0 otherwise. An offset that follows
 * the operand advances the converted pointer, so "(char *) array + 1" is one
 * byte past the array whatever its element type.
 */
int global_pointer_cast_stride = 0;

/* Say whether cur_token->next opens a cast to an object pointer type in a
 * static initializer. C99 6.6 lets an address constant and an integer constant
 * be converted by such a cast.
 */
bool global_pointer_cast_starts_here(block_t *scope)
{
    token_t *token = cur_token->next;
    token_kind_t previous = T_open_bracket;
    bool has_type = false;
    bool is_pointer = false;

    if (!token || token->kind != T_open_bracket)
        return false;
    for (token = token->next; token && token->kind != T_close_bracket;
         token = token->next) {
        if (token->kind == T_asterisk) {
            if (!has_type)
                return false;
            is_pointer = true;
        } else if (token->kind == T_identifier) {
            bool is_tag = previous == T_struct || previous == T_union ||
                          previous == T_enum;
            type_t *type =
                is_tag ? NULL : find_visible_type(token->literal, scope);

            if (!is_tag && !type)
                return false;
            if (type && (type->func_signature || type->is_direct_function_type))
                return false;
            if (type && type->ptr_level)
                is_pointer = true;
            has_type = true;
        } else if (token->kind == T_struct || token->kind == T_union ||
                   token->kind == T_enum || token->kind == T_signed ||
                   token->kind == T_unsigned || token->kind == T_long ||
                   token->kind == T_float || token->kind == T_double) {
            has_type = true;
        } else if (token->kind != T_const && token->kind != T_volatile &&
                   token->kind != T_restrict)
            return false;
        previous = token->kind;
    }
    return token && has_type && is_pointer;
}

/* Consume a cast that global_pointer_cast_starts_here() recognized and return
 * the size of what its pointer type points to.
 */
int read_global_pointer_cast(block_t *scope)
{
    type_t *type;
    int depth = 0;
    int size;

    lex_expect(T_open_bracket);
    type = read_type_name_specifiers(scope);
    while (lex_accept(T_asterisk)) {
        depth++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }
    lex_expect(T_close_bracket);
    if (!type)
        error_at("Unknown type in pointer cast", cur_token_loc());
    if (depth + type->ptr_level > 1)
        return PTR_SIZE;
    type = pointee_type_from_pointer_typedef(type);
    if (type == TY_void)
        return 1;
    size = type->size;
    if (!size && type->base_struct)
        size = type->base_struct->size;
    if (type->array_size)
        size *= type->array_size;
    return size;
}

/* Say whether the operand of a pointer cast in a static initializer is itself
 * an address constant, rather than an integer constant converted to a pointer.
 */
bool global_address_operand_starts_here(block_t *scope)
{
    char name[MAX_ID_LEN];

    if (lex_peek(T_ampersand, NULL) || lex_peek(T_string, NULL) ||
        global_pointer_cast_starts_here(scope) ||
        grouped_global_function_designator_starts_here(false) ||
        global_function_address_dereference_starts_here())
        return true;
    if (lex_peek(T_identifier, name)) {
        var_t *object = find_var(name, scope);

        return find_visible_func(name, scope) ||
               (object && object->is_global && object->array_size);
    }
    return false;
}

/* Read an integer constant operand of a pointer cast with @stride, and any
 * offset that follows it, as the address constant the cast produces.
 */
var_t *read_global_cast_integer_address(block_t *parent,
                                        basic_block_t *bb,
                                        block_t *scope,
                                        int stride)
{
    var_t *address = require_var(parent);
    int value = read_const_expr_operand(scope);

    if (lex_peek(T_plus, NULL) || lex_peek(T_minus, NULL))
        value += read_global_address_offset(scope, parent, bb) * stride;
    address->var_name = gen_name();
    address->init_val = value;
    address->is_const = true;
    add_insn(parent, bb, OP_load_constant, address, NULL, NULL, 0, NULL);
    return address;
}

/* Lower the designator that follows "&object" in a static initializer: member
 * selections and constant subscripts in any order, then an optional constant
 * offset. @object_addr is the address of @object; the address of the designated
 * subobject is returned and that subobject is left in @object. The scalar and
 * the aggregate initializer readers both come through here, so the forms they
 * accept, the scope names resolve in, and the diagnostics cannot drift apart.
 *
 * With @decays there is no '&': the designator must name an array row, such as
 * `matrix[1]`, whose conversion to a pointer to its first element is the
 * address constant, and an offset then steps over that row's elements.
 */
var_t *read_global_address_designator(block_t *scope,
                                      block_t *parent,
                                      basic_block_t **bb,
                                      var_t **object,
                                      var_t *object_addr,
                                      bool decays)
{
    var_t *target = *object;
    fixed_array_shape_t shape = fixed_array_shape_from_var(target);
    int subscripts = 0;

    for (;;) {
        if (lex_accept(T_dot)) {
            char field_name[MAX_ID_LEN];
            var_t *field;

            lex_ident(T_identifier, field_name);
            field = find_member(field_name, target->type);
            if (!field)
                error_at("Unknown struct or union member", cur_token_loc());
            object_addr = compute_field_address(parent, bb, object_addr, field);
            target = field;
            shape = fixed_array_shape_from_var(target);
            subscripts = 0;
        } else if (lex_accept(T_open_square)) {
            int element_size =
                target->ptr_level ? PTR_SIZE : target->type->size;
            int index;

            if (!target->array_size || subscripts >= shape.rank)
                error_at("Subscripted global address needs an array object",
                         cur_token_loc());
            index = read_const_expr(scope);
            lex_expect(T_close_square);
            object_addr = compute_element_address(
                parent, bb, object_addr, index,
                fixed_array_shape_stride(&shape, subscripts, element_size));
            subscripts++;
        } else
            break;
        object_addr->ptr_level = target->ptr_level + 1;
        object_addr->is_global_address = true;
    }

    /* A trailing offset after a partial multidimensional subscript advances by
     * the remaining row or plane, the complete pointed-to object, rather than
     * by its scalar leaf. read_global_address_offset() takes the sign as part
     * of the constant expression.
     */
    if (decays && (!target->array_size || subscripts >= shape.rank))
        error_at("Global initializer requires a constant address",
                 cur_token_loc());
    if (lex_peek(T_plus, NULL) || lex_peek(T_minus, NULL)) {
        int element_size = target->ptr_level ? PTR_SIZE : target->type->size;
        int index = read_global_address_offset(scope, parent, *bb);
        int stride = global_pointer_cast_stride
                         ? global_pointer_cast_stride
                         : fixed_array_shape_stride(
                               &shape, subscripts - !decays, element_size);

        object_addr =
            compute_element_address(parent, bb, object_addr, index, stride);
        object_addr->ptr_level = target->ptr_level + 1;
        object_addr->is_global_address = true;
    }
    *object = target;
    return object_addr;
}

var_t *parse_global_constant_value(block_t *parent, basic_block_t **bb)
{
    var_t *val = NULL;
    block_t *scope = global_constant_initializer_scope
                         ? global_constant_initializer_scope
                         : parent;
    bool address_dereference =
        global_function_address_dereference_starts_here();
    token_t *address_dereference_identifier = NULL;
    bool explicit_address;
    bool grouped_function_designator = false;

    if (address_dereference) {
        address_dereference_identifier =
            consume_global_function_address_dereference();
        if (!find_visible_func(address_dereference_identifier->literal, scope))
            error_at("Function address requires a visible declaration",
                     cur_token_loc());
        val = require_func_symbol_var(parent);
        val->var_name = intern_string(address_dereference_identifier->literal);
        val->is_func = true;
        return val;
    }
    if (global_pointer_cast_starts_here(scope)) {
        int saved_stride = global_pointer_cast_stride;
        int stride = read_global_pointer_cast(scope);

        /* In a chain of casts the outermost one decides the stride. */
        if (saved_stride)
            stride = saved_stride;
        if (!global_address_operand_starts_here(scope))
            return read_global_cast_integer_address(parent, *bb, scope, stride);
        global_pointer_cast_stride = stride;
        val = parse_global_constant_value(parent, bb);
        global_pointer_cast_stride = saved_stride;
        return val;
    }
    explicit_address = lex_accept(T_ampersand);

    if (grouped_global_function_designator_starts_here(false)) {
        lex_expect(T_open_bracket);
        grouped_function_designator = true;
    }

    char constant_name[MAX_ID_LEN];

    if (!explicit_address && !grouped_function_designator &&
        (lex_peek(T_numeric, NULL) || lex_peek(T_minus, NULL) ||
         lex_peek(T_plus, NULL) || lex_peek(T_bit_not, NULL) ||
         lex_peek(T_log_not, NULL) || lex_peek(T_open_bracket, NULL) ||
         lex_peek(T_sizeof, NULL) || lex_peek(T_char, NULL) ||
         lex_peek(T_wchar, NULL) ||
         (lex_peek(T_identifier, constant_name) &&
          find_scoped_constant(constant_name, scope)))) {
        /* Any integer constant expression, including casts, sizeof and grouped
         * subexpressions. The two-word reader keeps a wide value's high word.
         */
        val = read_wide_global_literal_expression(parent, *bb, scope);
    } else if (explicit_address || grouped_function_designator ||
               lex_peek(T_identifier, NULL)) {
        char name[MAX_ID_LEN];

        if (lex_peek(T_identifier, name)) {
            func_t *func = find_visible_func(name, scope);
            if (func) {
                lex_expect(T_identifier);
                if (grouped_function_designator)
                    lex_expect(T_close_bracket);
                val = require_func_symbol_var(parent);
                val->var_name = intern_string(name);
                val->is_func = true;
                return val;
            }
            if (explicit_address) {
                /* A block-scope static initializer lowers through this global
                 * constant path, yet its names, subscripts, and offsets resolve
                 * in the declaration's lexical scope. The is_global test still
                 * rejects the address of an automatic object.
                 */
                var_t *object = find_var(name, scope);

                if (object && object->is_global) {
                    lex_expect(T_identifier);
                    val = require_ref_var(parent, object->type,
                                          object->ptr_level);
                    val->var_name = gen_name();
                    val->is_global_address = true;
                    add_insn(parent, *bb, OP_address_of, val, object, NULL, 0,
                             NULL);
                    return read_global_address_designator(scope, parent, bb,
                                                          &object, val, false);
                }
            } else {
                var_t *object = find_var(name, scope);
                bool subscripted = cur_token->next->next &&
                                   cur_token->next->next->kind == T_open_square;

                /* A static array decays to an address constant (C99 6.6p7),
                 * optionally offset by an integer constant, and a subscripted
                 * row of one decays just as the whole array does.
                 */
                if (object && object->is_global &&
                    (subscripted ? object->array_dim2 : object->array_size)) {
                    lex_expect(T_identifier);
                    val = require_ref_var(parent, object->type,
                                          object->ptr_level);
                    val->var_name = gen_name();
                    val->is_global_address = true;
                    add_insn(parent, *bb, OP_address_of, val, object, NULL, 0,
                             NULL);
                    return read_global_address_designator(scope, parent, bb,
                                                          &object, val, true);
                }
            }
        }
        if (explicit_address)
            error_at("Expected a global object or function after '&'",
                     cur_token_loc());
        error_at("Global aggregate initializer requires a constant value",
                 cur_token_loc());
    } else if (lex_peek(T_string, NULL)) {
        /* A character-pointer member has the same constant-expression form as a
         * standalone global pointer: retain the rodata address, including
         * adjacent-literal concatenation, for the aggregate store.
         */
        read_literal_param(parent, *bb);
        val = opstack_pop();
    } else {
        error_at("Global array initialization requires constant values",
                 next_token_loc());
    }

    return val;
}

/* Skip one static initializer value without lowering it: every token up to the
 * ',' or '}' that ends it, outside the parentheses and brackets it contains.
 * The value is read for real, and diagnosed, when its code is emitted.
 */
void consume_global_constant_syntax(void)
{
    int depth = 0;
    bool consumed = false;

    while (cur_token->next) {
        token_kind_t kind = cur_token->next->kind;

        if (kind == T_open_curly ||
            (!depth &&
             (kind == T_comma || kind == T_close_curly || kind == T_semicolon)))
            break;
        if (kind == T_open_bracket || kind == T_open_square) {
            depth++;
        } else if (kind == T_close_bracket || kind == T_close_square) {
            if (!depth)
                break;
            depth--;
        }
        lex_next();
        consumed = true;
    }
    if (!consumed)
        error_at("Global array initialization requires constant values",
                 next_token_loc());
}

bool is_record_type(const type_t *type)
{
    return type &&
           (type->base_type == TYPE_struct || type->base_type == TYPE_union ||
            (type->base_type == TYPE_typedef && type->num_fields > 0));
}

void parse_struct_field_init(block_t *parent,
                             basic_block_t **bb,
                             type_t *struct_type,
                             var_t *target_addr,
                             bool emit_code);
bool parse_struct_field_values(block_t *parent,
                               basic_block_t **bb,
                               type_t *struct_type,
                               var_t *target_addr,
                               bool emit_code,
                               bool elided,
                               var_t *first_value);
bool parse_unbraced_record_init(block_t *parent,
                                basic_block_t **bb,
                                type_t *record_type,
                                var_t *addr,
                                bool emit_code);
bool unbraced_record_starts_here(const var_t *elem);
bool string_row_starts_here(const var_t *array);
void parse_string_row_init(block_t *parent,
                           basic_block_t **bb,
                           const var_t *array,
                           var_t *target_addr,
                           int start,
                           bool emit_code);

/* Store zero into every byte of elements [from, to) of the array at @base. */
void emit_zero_elements(block_t *parent,
                        basic_block_t **bb,
                        var_t *base,
                        int from,
                        int to,
                        int elem_size)
{
    var_t *zero;

    if (from >= to)
        return;
    zero = require_var(parent);
    zero->var_name = gen_name();
    zero->init_val = 0;
    add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
    for (int i = from; i < to; i++) {
        var_t *elem_addr =
            compute_element_address(parent, bb, base, i, elem_size);
        for (int offset = 0; offset < elem_size; offset++) {
            var_t *byte_addr =
                compute_element_address(parent, bb, elem_addr, offset, 1);
            add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1, NULL);
        }
    }
}

/* Read an `[index] =` designator for one of @bound slots, if one is next. */
bool accept_slot_designator(block_t *scope, int bound, int *slot)
{
    int index;

    if (!lex_accept(T_open_square))
        return false;
    index = read_const_expr(scope);
    lex_expect(T_close_square);
    if (index < 0 || index >= bound)
        error_at("Array designator index is out of bounds", cur_token_loc());
    lex_expect(T_assign);
    *slot = index;
    return true;
}

/* Parse one row of a two-dimensional array. An unbraced row returns true when a
 * brace-elided record element consumed the comma after the row's last
 * initializer, so the enclosing list must not expect it again. Only a braced
 * row owns designators; in an unbraced one they belong to the enclosing list.
 * As in the plane and hyperplane helpers below, `filled` is one past the
 * highest slot written, so after designators in any order the zero fill covers
 * exactly the slots nothing wrote.
 */
bool parse_array_field_row_values(block_t *parent,
                                  basic_block_t **bb,
                                  const var_t *field,
                                  var_t *target_addr,
                                  int start,
                                  bool emit_code,
                                  bool braced)
{
    int count = 0;
    int filled = 0;
    bool comma_consumed = false;
    int elem_size =
        (field->ptr_level || field->is_func) ? PTR_SIZE : field->type->size;

    if (braced) {
        lex_expect(T_open_curly);
        reject_empty_initializer_in_strict_c99();
    }

    /* The whole row may be one string literal, optionally in its own braces. */
    if (string_row_starts_here(field)) {
        parse_string_row_init(parent, bb, field, target_addr, start, emit_code);
        if (braced) {
            lex_accept(T_comma);
            lex_expect(T_close_curly);
        }
        return false;
    }
    while (!lex_peek(T_close_curly, NULL)) {
        var_t *value = NULL;
        var_t *elem_addr;

        if (braced)
            accept_slot_designator(field->scope ? field->scope : parent,
                                   field->array_dim2, &count);
        if (count >= field->array_dim2)
            error_at("Too many elements in array initializer",
                     next_token_loc());
        if (emit_code)
            emit_zero_elements(parent, bb, target_addr, start + filled,
                               start + count, elem_size);

        elem_addr = compute_element_address(parent, bb, target_addr,
                                            start + count, elem_size);
        comma_consumed = false;
        if (lex_peek(T_open_curly, NULL) && is_record_type(field->type)) {
            type_t *record_type = field->type;
            if (record_type->base_type == TYPE_typedef &&
                record_type->base_struct)
                record_type = record_type->base_struct;
            lex_expect(T_open_curly);
            parse_struct_field_init(parent, bb, record_type, elem_addr,
                                    emit_code);
            lex_expect(T_close_curly);
        } else if (unbraced_record_starts_here(field)) {
            comma_consumed = parse_unbraced_record_init(parent, bb, field->type,
                                                        elem_addr, emit_code);
        } else if (parent == GLOBAL_BLOCK) {
            if (emit_code)
                value = parse_global_constant_value(parent, bb);
            else
                consume_global_constant_syntax();
        } else {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
            value = opstack_pop();
        }

        if (value && emit_code) {
            var_t *stored = value->is_func
                                ? value
                                : resize_to(parent, bb, value, field->type,
                                            field->ptr_level);
            add_insn(parent, *bb, OP_write, NULL, elem_addr, stored, elem_size,
                     NULL);
        }

        count++;
        if (count > filled)
            filled = count;
        if (count == field->array_dim2) {
            if (braced && (comma_consumed || lex_accept(T_comma)) &&
                !lex_peek(T_close_curly, NULL)) {
                if (lex_peek(T_open_square, NULL))
                    continue;
                error_at("Too many elements in array initializer",
                         next_token_loc());
            }
            break;
        }
        if (!comma_consumed && !lex_accept(T_comma))
            break;
        comma_consumed = true;
    }
    if (braced) {
        lex_expect(T_close_curly);
        comma_consumed = false;
    }

    if (emit_code)
        emit_zero_elements(parent, bb, target_addr, start + filled,
                           start + field->array_dim2, elem_size);
    return comma_consumed;
}

void parse_array_field_row_init(block_t *parent,
                                basic_block_t **bb,
                                const var_t *field,
                                var_t *target_addr,
                                int start,
                                bool emit_code)
{
    parse_array_field_row_values(parent, bb, field, target_addr, start,
                                 emit_code, true);
}

/* Parse one braced plane of a three-dimensional array. Rows reuse the
 * two-dimensional helper, which also supplies C99's trailing zero fill.
 */
void parse_array_field_plane_values(block_t *parent,
                                    basic_block_t **bb,
                                    const var_t *field,
                                    var_t *target_addr,
                                    int start,
                                    bool emit_code,
                                    bool braced)
{
    var_t row;
    int rows = 0;
    int filled = 0;
    int row_width = field->array_dim3;
    int elem_size =
        (field->ptr_level || field->is_func) ? PTR_SIZE : field->type->size;

    memcpy(&row, field, sizeof(row));
    row.array_dim2 = row_width;
    row.array_dim3 = 0;
    if (braced) {
        lex_expect(T_open_curly);
        reject_empty_initializer_in_strict_c99();
    }
    while (!lex_peek(T_close_curly, NULL)) {
        accept_slot_designator(field->scope ? field->scope : parent,
                               field->array_dim2, &rows);
        if (rows >= field->array_dim2)
            error_at("Too many rows in array initializer", next_token_loc());
        if (emit_code)
            emit_zero_elements(parent, bb, target_addr,
                               start + filled * row_width,
                               start + rows * row_width, elem_size);
        bool row_comma = parse_array_field_row_values(
            parent, bb, &row, target_addr, start + rows * row_width, emit_code,
            lex_peek(T_open_curly, NULL));
        rows++;
        if (rows > filled)
            filled = rows;
        if (rows == field->array_dim2) {
            if (braced && (row_comma || lex_accept(T_comma)) &&
                !lex_peek(T_close_curly, NULL)) {
                if (lex_peek(T_open_square, NULL))
                    continue;
                error_at("Too many rows in array initializer",
                         next_token_loc());
            }
            break;
        }
        if (!row_comma && !lex_accept(T_comma))
            break;
    }
    if (braced)
        lex_expect(T_close_curly);

    if (emit_code)
        emit_zero_elements(parent, bb, target_addr, start + filled * row_width,
                           start + field->array_dim2 * row_width, elem_size);
}

void parse_array_field_plane_init(block_t *parent,
                                  basic_block_t **bb,
                                  const var_t *field,
                                  var_t *target_addr,
                                  int start,
                                  bool emit_code)
{
    parse_array_field_plane_values(parent, bb, field, target_addr, start,
                                   emit_code, true);
}

/* Parse one braced outer slice of a four-dimensional array. Each contained
 * three-dimensional plane reuses the existing plane parser, so row bounds and
 * trailing zero fill remain identical at every nesting level.
 */
void parse_array_field_hyperplane_init(block_t *parent,
                                       basic_block_t **bb,
                                       const var_t *field,
                                       var_t *target_addr,
                                       int start,
                                       bool emit_code)
{
    var_t plane;
    int planes = 0;
    int filled = 0;
    int plane_size = field->array_dim3 * field->array_dim4;
    int elem_size =
        (field->ptr_level || field->is_func) ? PTR_SIZE : field->type->size;

    memcpy(&plane, field, sizeof(plane));
    plane.array_size = plane_size;
    plane.array_dim2 = field->array_dim3;
    plane.array_dim3 = field->array_dim4;
    plane.array_dim4 = 0;
    lex_expect(T_open_curly);
    reject_empty_initializer_in_strict_c99();
    while (!lex_peek(T_close_curly, NULL)) {
        accept_slot_designator(field->scope ? field->scope : parent,
                               field->array_dim2, &planes);
        if (planes >= field->array_dim2)
            error_at("Too many planes in array initializer", next_token_loc());
        if (emit_code)
            emit_zero_elements(parent, bb, target_addr,
                               start + filled * plane_size,
                               start + planes * plane_size, elem_size);
        parse_array_field_plane_values(parent, bb, &plane, target_addr,
                                       start + planes * plane_size, emit_code,
                                       lex_peek(T_open_curly, NULL));
        planes++;
        if (planes > filled)
            filled = planes;
        if (!lex_accept(T_comma))
            break;
    }
    lex_expect(T_close_curly);

    if (emit_code)
        emit_zero_elements(parent, bb, target_addr, start + filled * plane_size,
                           start + field->array_dim2 * plane_size, elem_size);
}

/* Read a string literal and every adjacent one after it, decoded and joined
 * into @combined, which holds MAX_STRING_LEN bytes (C99 translation phase 6).
 * Each piece is decoded in place at the end of what came before, so the
 * capacity handed to the decoder and the buffer it writes are the same object.
 * Returns the joined length in bytes as reported by the decoder, which counts
 * an embedded null character; strlen() of @combined would stop at it.
 */
int read_concatenated_string(char *combined)
{
    char literal[MAX_STRING_LEN];
    int used;

    lex_ident(T_string, literal);
    used = unescape_string(literal, combined, MAX_STRING_LEN);
    if (used < 0)
        error_at("Concatenated string literal too long", cur_token_loc());
    while (lex_peek(T_string, NULL)) {
        int added;

        lex_ident(T_string, literal);
        added =
            unescape_string(literal, combined + used, MAX_STRING_LEN - used);
        if (added < 0 || used + added >= MAX_STRING_LEN - 1)
            error_at("Concatenated string literal too long", cur_token_loc());
        used += added;
    }
    return used;
}

/* An array member can be initialized directly by a string literal just like a
 * standalone character array. The member has no independent var_t storage, so
 * write through its already-computed field address rather than routing this
 * through parse_string_array_init().
 */
void parse_string_field_init(block_t *parent,
                             basic_block_t **bb,
                             const var_t *field,
                             var_t *target_addr,
                             bool emit_code)
{
    char combined[MAX_STRING_LEN];
    int len;

    /* The terminating null is dropped when only the characters fit (C99
     * 6.7.8p14).
     */
    len = read_concatenated_string(combined) + 1;
    if (len - 1 > field->array_size)
        error_at("String initializer is too long for character array",
                 cur_token_loc());
    if (!emit_code)
        return;

    for (int i = 0; i < field->array_size; i++) {
        var_t *value = require_var(parent);
        var_t *addr = compute_element_address(parent, bb, target_addr, i, 1);

        value->var_name = gen_name();
        value->init_val = i < len ? (unsigned char) combined[i] : 0;
        value->is_const = true;
        add_insn(parent, *bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        add_insn(parent, *bb, OP_write, NULL, addr, value, 1, NULL);
    }
}

bool is_char_array(const var_t *var)
{
    return var && !var->ptr_level && !var->array_dim2 && !var->array_dim3 &&
           !var->array_dim4 &&
           compatible_decl_type(var->type, find_type("char", true));
}

bool is_wchar_array(const var_t *var)
{
    return var && !var->ptr_level && !var->array_dim2 && !var->array_dim3 &&
           !var->array_dim4 &&
           compatible_decl_type(var->type, find_type("wchar_t", true));
}

/* C99 permits a string literal initializer only for an array of matching
 * character units. Diagnose this before the ordinary assignment path, which
 * treats a literal as a pointer and otherwise produces a misleading error.
 */
void validate_string_array_initializer(const var_t *var)
{
    if (!var || var->ptr_level ||
        !(var->array_size > 0 || var->has_unsized_array) ||
        !(lex_peek(T_string, NULL) || lex_peek(T_wstring, NULL)))
        return;
    if ((lex_peek(T_string, NULL) && !is_char_array(var)) ||
        (lex_peek(T_wstring, NULL) && !is_wchar_array(var)))
        error_at(
            "String literal initializer has incompatible array element type",
            cur_token_loc());
}

void parse_wstring_field_init(block_t *parent,
                              basic_block_t **bb,
                              const var_t *field,
                              var_t *target_addr,
                              bool emit_code)
{
    int values[MAX_STRING_LEN];
    int length;

    length = read_wstring_units(values, MAX_STRING_LEN);
    if (length > field->array_size)
        error_at("Wide string initializer is too long for array",
                 cur_token_loc());
    if (!emit_code)
        return;

    for (int i = 0; i < field->array_size; i++) {
        var_t *value = require_typed_var(parent, field->type);
        var_t *addr = compute_element_address(parent, bb, target_addr, i,
                                              field->type->size);

        value->var_name = gen_name();
        value->init_val = i < length ? values[i] : 0;
        value->is_const = true;
        add_insn(parent, *bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        add_insn(parent, *bb, OP_write, NULL, addr, value, field->type->size,
                 NULL);
    }
}

/* A string literal also initializes one innermost row of a multidimensional
 * character array: `char names[2][4] = { "ab", "cd" }` fills each row of four
 * bytes, zero-padded, rather than storing the literal's address into a char.
 * Return whether the next initializer is such a string for @array. A literal
 * that only begins a larger expression, as in `"ab"[0]`, remains a scalar.
 */
bool string_row_starts_here(const var_t *array)
{
    token_t *after = cur_token->next;
    token_kind_t kind = after ? after->kind : T_eof;

    if (!array || has_effective_pointer(array) || array->is_func ||
        !array->array_dim2 || is_record_type(array->type) ||
        (kind != T_string && kind != T_wstring))
        return false;
    while (after->next && after->next->kind == kind)
        after = after->next;
    if (!after->next ||
        (after->next->kind != T_comma && after->next->kind != T_close_curly))
        return false;
    if ((kind == T_string &&
         !compatible_decl_type(array->type, find_type("char", true))) ||
        (kind == T_wstring &&
         !compatible_decl_type(array->type, find_type("wchar_t", true))))
        error_at(
            "String literal initializer has incompatible array element type",
            next_token_loc());
    return true;
}

/* Elements in the innermost row of a multidimensional @array. */
int string_row_width(const var_t *array)
{
    if (array->array_dim4)
        return array->array_dim4;
    return array->array_dim3 ? array->array_dim3 : array->array_dim2;
}

/* Initialize the innermost row of @array that starts at flat element @start
 * from the string literal string_row_starts_here() accepted. The row must begin
 * on a row boundary; a string cannot initialize a single element.
 */
void parse_string_row_init(block_t *parent,
                           basic_block_t **bb,
                           const var_t *array,
                           var_t *target_addr,
                           int start,
                           bool emit_code)
{
    var_t row;
    var_t *row_addr = NULL;
    int width = string_row_width(array);

    if (start % width)
        error_at("String literal cannot initialize a single array element",
                 next_token_loc());
    memcpy(&row, array, sizeof(row));
    row.array_size = width;
    row.array_dim2 = row.array_dim3 = row.array_dim4 = 0;
    if (emit_code)
        row_addr = compute_element_address(parent, bb, target_addr, start,
                                           array->type->size);
    if (lex_peek(T_wstring, NULL))
        parse_wstring_field_init(parent, bb, &row, row_addr, emit_code);
    else
        parse_string_field_init(parent, bb, &row, row_addr, emit_code);
}

void parse_array_field_init(block_t *parent,
                            basic_block_t **bb,
                            const var_t *field,
                            var_t *target_addr,
                            bool emit_code)
{
    int count = 0;

    /* One past the highest element written so far. A designator can move
     * backwards, so zero fill starts here rather than at `count`, which would
     * clear rows that an earlier designator already stored.
     */
    int filled = 0;
    int elem_size =
        (field->ptr_level || field->is_func) ? PTR_SIZE : field->type->size;

    lex_expect(T_open_curly);
    reject_empty_initializer_in_strict_c99();
    while (!lex_peek(T_close_curly, NULL)) {
        var_t *value = NULL;
        var_t slice;
        bool comma_consumed = false;

        /* `slice` is the array whose one element the next initializer fills.
         * Without a designator that is the member itself. Each subscript of a
         * designator such as `[1] = { ... }` or `[1][0] = 3` descends one
         * level, so the braced row, plane and scalar paths below see exactly
         * the shape the designator names and reordered rows stay valid.
         */
        memcpy(&slice, field, sizeof(slice));
        if (lex_accept(T_open_square)) {
            var_t element;

            memcpy(&element, field, sizeof(element));
            count = 0;
            do {
                int stride = fixed_array_inner_count(&element);
                int index =
                    read_const_expr(field->scope ? field->scope : parent);

                lex_expect(T_close_square);
                if (index < 0 || index >= element.array_size / stride)
                    error_at("Array designator index is out of bounds",
                             cur_token_loc());
                count += index * stride;
                memcpy(&slice, &element, sizeof(slice));
                fixed_array_var_drop_outer(&element);
            } while (element.array_size && lex_accept(T_open_square));
            lex_expect(T_assign);
        }

        if (count >= field->array_size)
            error_at("Too many elements in array initializer",
                     next_token_loc());
        if (emit_code)
            emit_zero_elements(parent, bb, target_addr, filled, count,
                               elem_size);

        var_t *elem_addr =
            compute_element_address(parent, bb, target_addr, count, elem_size);
        if (slice.array_dim4 && lex_peek(T_open_curly, NULL)) {
            parse_array_field_hyperplane_init(parent, bb, &slice, target_addr,
                                              count, emit_code);
            count += fixed_array_inner_count(&slice);
        } else if (slice.array_dim3 && lex_peek(T_open_curly, NULL)) {
            parse_array_field_plane_init(parent, bb, &slice, target_addr, count,
                                         emit_code);
            count += fixed_array_inner_count(&slice);
        } else if (slice.array_dim2 && lex_peek(T_open_curly, NULL)) {
            parse_array_field_row_init(parent, bb, &slice, target_addr, count,
                                       emit_code);
            count += fixed_array_inner_count(&slice);
        } else if (string_row_starts_here(&slice)) {
            parse_string_row_init(parent, bb, &slice, target_addr, count,
                                  emit_code);
            count += string_row_width(&slice);
        } else {
            if (lex_peek(T_open_curly, NULL) && is_record_type(field->type)) {
                type_t *record_type = field->type;
                if (record_type->base_type == TYPE_typedef &&
                    record_type->base_struct)
                    record_type = record_type->base_struct;
                lex_expect(T_open_curly);
                parse_struct_field_init(parent, bb, record_type, elem_addr,
                                        emit_code);
                lex_expect(T_close_curly);
            } else if (unbraced_record_starts_here(field)) {
                comma_consumed = parse_unbraced_record_init(
                    parent, bb, field->type, elem_addr, emit_code);
            } else if (parent == GLOBAL_BLOCK) {
                if (emit_code)
                    value = parse_global_constant_value(parent, bb);
                else
                    consume_global_constant_syntax();
            } else {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
                value = opstack_pop();
            }

            if (value && emit_code) {
                var_t *stored = value->is_func
                                    ? value
                                    : resize_to(parent, bb, value, field->type,
                                                field->ptr_level);
                add_insn(parent, *bb, OP_write, NULL, elem_addr, stored,
                         elem_size, NULL);
            }
            count++;
        }

        if (count > filled)
            filled = count;
        if (!comma_consumed && !lex_accept(T_comma))
            break;
    }
    lex_expect(T_close_curly);

    if (emit_code)
        emit_zero_elements(parent, bb, target_addr, filled, field->array_size,
                           elem_size);
}

type_t *resolve_record_type(type_t *type)
{
    if (type->base_type == TYPE_typedef && type->base_struct)
        return type->base_struct;
    return type;
}

/* Initialize the record at @addr from @value, an initializer already read that
 * does not begin with a brace. An expression of the record's own type
 * initializes the whole record. Anything else, or no value at all, starts a
 * brace-elided list that takes one initializer per member from the enclosing
 * list (C99 6.7.8p13 and p20).
 *
 * Returns true when that list consumed the comma after its last initializer,
 * which the enclosing loop must then not expect.
 */
bool parse_record_from_value(block_t *parent,
                             basic_block_t **bb,
                             type_t *record_type,
                             var_t *addr,
                             bool emit_code,
                             var_t *value)
{
    record_type = resolve_record_type(record_type);
    if (is_record_object(value) &&
        resolve_record_type(value->type) == record_type) {
        if (emit_code)
            emit_record_copy_to_address(parent, bb, addr, value);
        return false;
    }
    return parse_struct_field_values(parent, bb, record_type, addr, emit_code,
                                     true, value);
}

/* A record slot whose initializer does not begin with a brace. Reading a block
 * scope expression first is the only way to tell a record value from the first
 * scalar of an elided list. Static storage takes constants only, which are
 * never records, and a string always belongs to a member, so neither is read
 * ahead: the elided list then parses it in the member's own context.
 */
bool parse_unbraced_record_init(block_t *parent,
                                basic_block_t **bb,
                                type_t *record_type,
                                var_t *addr,
                                bool emit_code)
{
    var_t *value = NULL;

    if (parent != GLOBAL_BLOCK && !lex_peek(T_string, NULL) &&
        !lex_peek(T_wstring, NULL)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
        value = opstack_pop();
    }
    return parse_record_from_value(parent, bb, record_type, addr, emit_code,
                                   value);
}

/* Whether an initializer starting at the next token for a slot of @elem's type
 * is a brace-elided record, rather than a braced one or a designator that
 * belongs to the enclosing list.
 */
bool unbraced_record_starts_here(const var_t *elem)
{
    return !elem->ptr_level && !elem->is_func && is_record_type(elem->type) &&
           !lex_peek(T_open_curly, NULL) && !lex_peek(T_dot, NULL) &&
           !lex_peek(T_open_square, NULL);
}

void parse_struct_field_init(block_t *parent,
                             basic_block_t **bb,
                             type_t *struct_type,
                             var_t *target_addr,
                             bool emit_code)
{
    parse_struct_field_values(parent, bb, struct_type, target_addr, emit_code,
                              false, NULL);
}

/* Parse the members of the record at @target_addr. A braced list runs to its
 * closing brace, which the caller consumes. An @elided list has no braces of
 * its own: it stops once every member has an initializer, or at a closing brace
 * or designator that belongs to the enclosing list, and @first_value is an
 * initializer the caller already read for the first scalar member.
 */
bool parse_struct_field_values(block_t *parent,
                               basic_block_t **bb,
                               type_t *struct_type,
                               var_t *target_addr,
                               bool emit_code,
                               bool elided,
                               var_t *first_value)
{
    int field_idx = 0;
    int initializer_count = 0;
    bool comma_consumed = false;
    bool is_union =
        struct_type->base_type == TYPE_union || struct_type->is_union;

    if (!elided)
        reject_empty_initializer_in_strict_c99();

    /* Both of these back a "field" pointer that outlives the designator block
     * they are filled in from, so they have to live as long as the loop that
     * dereferences it rather than as long as that block.
     */
    var_t array_element;
    var_t pending_array_element;
    var_t *pending_array_base = NULL;
    int pending_array_index = 0;
    int pending_array_count = 0;
    int pending_array_elem_size = 0;
    bool has_pending_array_element = false;

    /* A static aggregate is zero-initialized before its initializer runs. A
     * bit-field store must nevertheless preserve earlier fields in its shared
     * allocation unit. Collect constant slices here and emit one direct store
     * per unit after the whole initializer has been parsed, avoiding a global
     * setup read before the global-pointer frame is established.
     */
    var_t *global_bitfield_unit[MAX_FIELDS];
    unsigned global_bitfield_value[MAX_FIELDS];
    int global_bitfield_count = 0;

    /* Zero the complete struct before processing fields. Positional
     * initializers could defer this until the first omitted member, but a
     * designator may skip forward or return to an earlier member.
     */
    if (emit_code && parent != GLOBAL_BLOCK &&
        struct_type->base_type != TYPE_union && !struct_type->is_union) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);

        for (int i = 0; i < struct_type->num_fields; i++) {
            var_t *field = &struct_type->fields[i];
            var_t *field_addr =
                compute_field_address(parent, bb, target_addr, field);
            int field_size = size_var(field);

            for (int offset = 0; offset < field_size; offset++) {
                var_t *byte_addr =
                    compute_element_address(parent, bb, field_addr, offset, 1);
                add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1, NULL);
            }
        }
    }

    if (first_value || !lex_peek(T_close_curly, NULL)) {
        for (;;) {
            var_t *field_val_raw = NULL;
            var_t *field = NULL;
            var_t *designated_field_addr = NULL;
            bool consumed_pending_array_element = false;
            bool nested_comma_consumed = false;

            if (!first_value && lex_accept(T_dot)) {
                char field_name[MAX_ID_LEN];
                type_t *designator_type = struct_type;
                var_t *designator_base = target_addr;
                bool first = true;

                /* A new member designator ends any array element sequence
                 * started by an earlier `.a[i] =`, so the positional
                 * initializers after it follow the newly named member instead
                 * of resuming the stale array slots.
                 */
                has_pending_array_element = false;

                for (;;) {
                    bool found = false;

                    lex_ident(T_identifier, field_name);
                    for (int i = 0; i < designator_type->num_fields; i++) {
                        if (!strcmp(designator_type->fields[i].var_name,
                                    field_name)) {
                            if (first)
                                field_idx = i;
                            field = &designator_type->fields[i];
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        error_at("Unknown field in record initializer",
                                 cur_token_loc());

                    designated_field_addr = compute_field_address(
                        parent, bb, designator_base, field);
                    if (lex_accept(T_open_square)) {
                        int index, element_count, linear_index, stride;
                        int total_element_count;
                        int elem_size;
                        var_t *array_base_addr = designated_field_addr;

                        if (!field->array_size)
                            error_at(
                                "Array designator requires an array member",
                                cur_token_loc());
                        index = read_const_expr(parent);
                        lex_expect(T_close_square);
                        total_element_count = field->array_size;
                        element_count = total_element_count;
                        stride = 1;
                        if (field->array_dim2) {
                            stride = field->array_dim2;
                            if (field->array_dim3) {
                                stride *= field->array_dim3;
                                if (field->array_dim4)
                                    stride *= field->array_dim4;
                            }
                            element_count /= stride;
                        }
                        if (index < 0 || index >= element_count)
                            error_at("Array designator index is out of bounds",
                                     cur_token_loc());
                        elem_size = (field->ptr_level || field->is_func)
                                        ? PTR_SIZE
                                        : field->type->size;
                        linear_index = index * stride;
                        designated_field_addr = compute_element_address(
                            parent, bb, designated_field_addr, linear_index,
                            elem_size);
                        memcpy(&array_element, field, sizeof(var_t));
                        fixed_array_var_drop_outer(&array_element);
                        field = &array_element;

                        /* Each subscript leaves the remaining inner array in
                         * `field`, so rows, planes, and a final scalar can all
                         * share the same bounds and continuation path.
                         */
                        while (field->array_size && lex_accept(T_open_square)) {
                            index = read_const_expr(parent);
                            lex_expect(T_close_square);
                            stride = field->array_dim2 ? field->array_dim2 : 1;
                            if (field->array_dim3)
                                stride *= field->array_dim3;
                            element_count = field->array_size / stride;
                            if (index < 0 || index >= element_count)
                                error_at(
                                    "Array designator index is out of bounds",
                                    cur_token_loc());
                            designated_field_addr = compute_element_address(
                                parent, bb, designated_field_addr,
                                index * stride, elem_size);
                            linear_index += index * stride;
                            fixed_array_var_drop_outer(&array_element);
                        }
                        if (!field->array_size) {
                            memcpy(&pending_array_element, field,
                                   sizeof(var_t));
                            pending_array_base = array_base_addr;
                            pending_array_index = linear_index + 1;
                            pending_array_count = total_element_count;
                            pending_array_elem_size = elem_size;
                            has_pending_array_element = true;
                        }
                    }
                    if (!lex_accept(T_dot))
                        break;
                    if (!is_record_type(field->type))
                        error_at("Nested designator requires a record member",
                                 cur_token_loc());
                    designator_type = field->type;
                    if (designator_type->base_type == TYPE_typedef &&
                        designator_type->base_struct)
                        designator_type = designator_type->base_struct;
                    designator_base = designated_field_addr;
                    first = false;
                }
                lex_expect(T_assign);
            } else if (has_pending_array_element) {
                field = &pending_array_element;
                designated_field_addr = compute_element_address(
                    parent, bb, pending_array_base, pending_array_index,
                    pending_array_elem_size);
                consumed_pending_array_element = true;
            }

            /* An unnamed bit-field is padding, not an aggregate member. It
             * participates in layout but consumes no positional initializer;
             * otherwise `{ low, high }` would incorrectly assign `high` to the
             * padding field and leave the named field zero-initialized.
             */
            if (!field && !has_pending_array_element)
                while (field_idx < struct_type->num_fields &&
                       is_bitfield(&struct_type->fields[field_idx]) &&
                       !struct_type->fields[field_idx].var_name[0])
                    field_idx++;

            if (field_idx >= struct_type->num_fields ||
                ((struct_type->base_type == TYPE_union ||
                  struct_type->is_union) &&
                 initializer_count > 0 && !consumed_pending_array_element))
                error_at("Too many elements in record initializer",
                         next_token_loc());

            if (!field && field_idx < struct_type->num_fields)
                field = &struct_type->fields[field_idx];

            /* A scalar that meets an array member without braces initializes
             * the member's first element, and the following scalars fill the
             * rest before the next member (C99 6.7.8p20). Writing it at the
             * member's aggregate size instead corrupts the elements and gives
             * narrow backends a store width they cannot encode.
             */
            if (field && !designated_field_addr && !has_pending_array_element &&
                field->array_size &&
                (first_value ||
                 (!lex_peek(T_open_curly, NULL) &&
                  ((!field->ptr_level && is_record_type(field->type)) ||
                   has_effective_pointer(field) ||
                   (!lex_peek(T_string, NULL) &&
                    !lex_peek(T_wstring, NULL)))))) {
                pending_array_base =
                    compute_field_address(parent, bb, target_addr, field);
                pending_array_elem_size = (field->ptr_level || field->is_func)
                                              ? PTR_SIZE
                                              : field->type->size;
                pending_array_count = field->array_size;
                pending_array_index = 0;
                memcpy(&pending_array_element, field, sizeof(var_t));
                pending_array_element.array_size = 0;
                pending_array_element.array_dim2 = 0;
                pending_array_element.array_dim3 = 0;
                pending_array_element.array_dim4 = 0;
                field = &pending_array_element;
                designated_field_addr = compute_element_address(
                    parent, bb, pending_array_base, 0, pending_array_elem_size);
                has_pending_array_element = true;
                consumed_pending_array_element = true;
            }

            if (!first_value && field && field->array_size &&
                !has_effective_pointer(field) &&
                (lex_peek(T_string, NULL) || lex_peek(T_wstring, NULL)) &&
                ((lex_peek(T_string, NULL) && !is_char_array(field)) ||
                 (lex_peek(T_wstring, NULL) && !is_wchar_array(field))))
                error_at(
                    "String literal initializer has incompatible array element "
                    "type",
                    cur_token_loc());
            if (first_value) {
                /* The caller read this value for the first scalar of an elided
                 * record. A record member takes it on to that record's own
                 * first member; see parse_record_from_value().
                 */
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);

                if (!field->ptr_level && is_record_type(field->type))
                    nested_comma_consumed = parse_record_from_value(
                        parent, bb, field->type, field_addr, emit_code,
                        first_value);
                else
                    field_val_raw = first_value;
                first_value = NULL;
            } else if (field && field->array_size && is_char_array(field) &&
                       lex_peek(T_string, NULL)) {
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);
                parse_string_field_init(parent, bb, field, field_addr,
                                        emit_code);
            } else if (field && field->array_size && !field->ptr_level &&
                       compatible_decl_type(field->type,
                                            find_type("wchar_t", true)) &&
                       lex_peek(T_wstring, NULL)) {
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);
                parse_wstring_field_init(parent, bb, field, field_addr,
                                         emit_code);
            } else if (field && lex_peek(T_open_curly, NULL) &&
                       field->array_size) {
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);
                parse_array_field_init(parent, bb, field, field_addr,
                                       emit_code);
            } else if (field && lex_peek(T_open_curly, NULL) &&
                       is_record_type(field->type)) {
                type_t *nested_type = field->type;
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);

                if (nested_type->base_type == TYPE_typedef &&
                    nested_type->base_struct)
                    nested_type = nested_type->base_struct;
                lex_expect(T_open_curly);
                parse_struct_field_init(parent, bb, nested_type, field_addr,
                                        emit_code);
                lex_expect(T_close_curly);
            } else if (field && !field->array_size &&
                       unbraced_record_starts_here(field)) {
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);

                nested_comma_consumed = parse_unbraced_record_init(
                    parent, bb, field->type, field_addr, emit_code);
            } else if (parent == GLOBAL_BLOCK) {
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
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);

                if (is_record_type(field->type) &&
                    is_record_object(field_val_raw)) {
                    emit_record_copy_to_address(parent, bb, field_addr,
                                                field_val_raw);
                } else if (field_val_raw->is_func) {
                    /* Keep a function designator intact until global lowering
                     * can patch its final code address. Converting it through a
                     * scalar temporary loses that relocation provenance, and at
                     * block scope resize_to() sees neither the designator nor a
                     * function pointer member as pointer-sized, so on LP64 it
                     * truncated the code address to an int.
                     */
                    add_insn(parent, *bb, OP_write, NULL, field_addr,
                             field_val_raw, PTR_SIZE, NULL);
                } else {
                    var_t *field_val = resize_to(parent, bb, field_val_raw,
                                                 field->type, field->ptr_level);
                    int field_size = size_var(field);
                    if (is_bitfield(field) && parent == GLOBAL_BLOCK) {
                        int unit = -1;
                        for (int i = 0; i < global_bitfield_count; i++)
                            if (global_bitfield_unit[i]->offset ==
                                    field->offset &&
                                global_bitfield_unit[i]->bit_storage_size ==
                                    field->bit_storage_size) {
                                unit = i;
                                break;
                            }
                        if (unit < 0) {
                            unit = global_bitfield_count++;
                            global_bitfield_unit[unit] = field;
                            global_bitfield_value[unit] = 0;
                        }
                        unsigned mask = bitfield_mask(field)
                                        << field->bit_offset;
                        unsigned init_val =
                            is_bool_type(field->type)
                                ? field_val_raw->init_val != 0
                                : (unsigned) field_val_raw->init_val;
                        global_bitfield_value[unit] =
                            (global_bitfield_value[unit] & ~mask) |
                            ((init_val & bitfield_mask(field))
                             << field->bit_offset);
                    } else if (is_bitfield(field))
                        write_bitfield_value(parent, bb, field_addr, field_val,
                                             field);
                    else
                        add_insn(parent, *bb, OP_write, NULL, field_addr,
                                 field_val, field_size, NULL);
                }
            }

            if (has_pending_array_element) {
                if (consumed_pending_array_element)
                    pending_array_index++;
                if (pending_array_index >= pending_array_count) {
                    has_pending_array_element = false;
                    field_idx++;
                }
            } else {
                field_idx++;
            }
            initializer_count++;

            /* An elided record ends with its last member and leaves the comma
             * that follows to the enclosing list.
             */
            if (elided && !has_pending_array_element) {
                while (field_idx < struct_type->num_fields &&
                       is_bitfield(&struct_type->fields[field_idx]) &&
                       !struct_type->fields[field_idx].var_name[0])
                    field_idx++;
                if (field_idx >= struct_type->num_fields || is_union) {
                    comma_consumed = nested_comma_consumed;
                    break;
                }
            }
            if (!nested_comma_consumed && !lex_accept(T_comma))
                break;
            comma_consumed = true;
            if (lex_peek(T_close_curly, NULL))
                break;
            if (elided &&
                (lex_peek(T_dot, NULL) || lex_peek(T_open_square, NULL)))
                break;
            comma_consumed = false;
        }
    }

    if (emit_code && parent == GLOBAL_BLOCK) {
        for (int i = 0; i < global_bitfield_count; i++) {
            var_t *unit = global_bitfield_unit[i];
            var_t *unit_addr =
                compute_field_address(parent, bb, target_addr, unit);
            var_t *value =
                bitfield_constant(parent, bb, global_bitfield_value[i]);
            add_insn(parent, *bb, OP_write, NULL, unit_addr, value,
                     unit->bit_storage_size, NULL);
        }
    }
    return comma_consumed;
}

void parse_array_literal_expr(block_t *parent, basic_block_t **bb)
{
    var_t *array_var = require_var(parent);
    array_var->var_name = gen_name();
    array_var->is_compound_literal = true;

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
        if (strict_c99 && parent->func->return_def.type &&
            (parent->func->return_def.type->base_type != TYPE_void ||
             has_effective_pointer(&parent->func->return_def)))
            error_at("non-void function requires a return expression in C99",
                     cur_token_loc());
        add_insn(parent, bb, OP_return, NULL, NULL, NULL, 0, NULL);
        bb_connect(bb, parent->func->exit, NEXT);
        return NULL;
    }

    if (strict_c99 && parent->func->return_def.type &&
        parent->func->return_def.type->base_type == TYPE_void &&
        !has_effective_pointer(&parent->func->return_def))
        error_at("void function cannot return an expression in C99",
                 cur_token_loc());

    if (!read_assignment_expression(parent, &bb)) {
        read_expr(parent, &bb);
        read_ternary_operation(parent, &bb);
    }
    while (lex_accept(T_comma)) {
        opstack_pop();
        perform_side_effect(parent, bb);
        if (!read_assignment_expression(parent, &bb)) {
            read_expr(parent, &bb);
            read_ternary_operation(parent, &bb);
        }
    }
    lex_expect(T_semicolon);

    var_t *rs1 = opstack_pop();

    /* Handle array compound literals in return context. Convert array compound
     * literals to their first element value.
     */
    if (is_array_literal_placeholder(rs1) && strict_c99 &&
        !has_effective_pointer(&parent->func->return_def))
        error_at("array compound literal cannot be used as a scalar in C99",
                 cur_token_loc());

    if (rs1 && rs1->array_size > 0 && rs1->var_name[0] == '.' && !strict_c99) {
        var_t *val = require_var(parent);
        val->type = rs1->type;
        val->init_val = rs1->init_val;
        val->var_name = gen_name();
        add_insn(parent, bb, OP_load_constant, val, NULL, NULL, 0, NULL);
        rs1 = val;
    }

    /* "return i++" yields the value i held before the increment, yet the
     * increment still has to happen before the function leaves. Applying the
     * pending side effects before the value was read returned the modified
     * variable instead, so take a copy first and return that.
     */
    if (se_idx > 0 && rs1) {
        var_t *snapshot = require_var(parent);
        snapshot->type = rs1->type;
        snapshot->ptr_level = rs1->ptr_level;
        snapshot->var_name = gen_name();
        add_insn(parent, bb, OP_assign, snapshot, rs1, NULL, 0, NULL);
        rs1 = snapshot;
    }
    perform_side_effect(parent, bb);

    /* Every ordinary use of a function designator converts it to a pointer.
     * This includes scalar conversions such as `_Bool f(void) { return cb; }`,
     * not only callback-pointer returns.
     */
    rs1 = materialize_function_designator(parent, &bb, rs1);
    if (parent->func->return_def.type->func_signature) {
        rs1->func_signature = parent->func->return_def.type->func_signature;
    }

    if (parent->func->returns_aggregate) {
        if (!is_record_object(rs1) ||
            !compatible_decl_type(rs1->type, parent->func->return_def.type))
            error_at("incompatible record return expression", cur_token_loc());
        emit_record_copy_to_address(parent, &bb, &parent->func->sret_def, rs1);
        add_insn(parent, bb, OP_return, NULL, NULL, NULL, 0, NULL);
        bb_connect(bb, parent->func->exit, NEXT);
        return NULL;
    }

    /* A return expression is converted to the function's declared type just
     * like an assignment. This is particularly important for _Bool: a pointer
     * return value must become 0 or 1 before it crosses the ABI boundary,
     * rather than leaving an address in the low return byte.
     */
    if (!parent->func->return_def.type->func_signature)
        rs1 = resize_to(parent, &bb, rs1, parent->func->return_def.type,
                        parent->func->return_def.ptr_level);

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
    read_control_expression(parent, &bb);
    lex_expect(T_close_bracket);

    var_t *vd = opstack_pop();
    reject_record_operand(vd);
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

    continue_bb_push(bb);

    basic_block_t *cond = bb;
    lex_expect(T_open_bracket);
    read_control_expression(parent, &bb);
    lex_expect(T_close_bracket);

    var_t *vd = opstack_pop();
    reject_record_operand(vd);
    add_insn(parent, bb, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    bb_connect(bb, then_, THEN);
    bb_connect(bb, else_, ELSE);
    break_bb_push(else_);

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
     * after: a = b + c; goto label; c *= d;
     *
     * before: a = b + c; if (1)
     *     goto label;
     * c *= d;
     */

    char token[MAX_ID_LEN];
    if (!lex_peek(T_identifier, token))
        error_at("Expected identifier after 'goto'", next_token_loc());

    lex_expect(T_identifier);
    lex_expect(T_semicolon);

    basic_block_t *fake_if = bb_create(parent);
    bb_connect(bb, fake_if, NEXT);
    var_t *val = require_var(parent);
    val->var_name = gen_name();
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
        error_at("Too many forward-referenced labels", cur_token_loc());

    backpatch_bb[backpatch_bb_idx++] = then_;
    return else_;
}

int read_const_expr(block_t *scope);

/* Whether @token starts a string literal that is the whole brace-enclosed
 * initializer of the character array @var: `"ab"}` or `"ab",}`.
 */
static bool string_ends_braced_initializer(const var_t *var, token_t *token)
{
    token_kind_t kind;

    if (!token)
        return false;
    kind = token->kind;
    if (!((kind == T_string && is_char_array(var)) ||
          (kind == T_wstring && is_wchar_array(var))))
        return false;
    while (token->next && token->next->kind == kind)
        token = token->next;
    token = token->next;
    if (token && token->kind == T_comma)
        token = token->next;
    return token && token->kind == T_close_curly;
}

void parse_array_init(var_t *var,
                      block_t *parent,
                      basic_block_t **bb,
                      bool emit_code)
{
    int count = 0;
    int inferred_size = 0;
    var_t *base_addr = NULL;

    /* An omitted outer bound of a multidimensional declaration already has an
     * inner-dimension product in `array_size`. It is nevertheless inferred from
     * the initializer, just like a one-dimensional `int a[]`.
     */
    bool is_implicit = (var->array_size == 0 || var->has_unsized_array);
    block_t *initializer_scope = parent;

    if (parent == GLOBAL_BLOCK && var->scope && var->scope != GLOBAL_BLOCK)
        initializer_scope = var->scope;
    block_t *saved_initializer_scope = global_constant_initializer_scope;
    if (parent == GLOBAL_BLOCK)
        global_constant_initializer_scope = initializer_scope;

    /* Elements of a pointer array are pointer-sized. Using the base type's
     * width strided "char *a[2] = {...}" by one byte, so every element but the
     * first got a bogus address. `ptr_level` describes the array element type
     * even when its outer bound is inferred.
     */
    int elem_size = var->type->size;
    if (var->ptr_level > 0 || var->is_func)
        elem_size = PTR_SIZE;

    /* A character array's string literal may be enclosed in braces (C99
     * 6.7.8p14): `char s[3] = {"ab"}` holds the characters, not the literal's
     * address converted to a char.
     */
    if (lex_peek(T_open_curly, NULL) &&
        string_ends_braced_initializer(var, cur_token->next->next)) {
        lex_expect(T_open_curly);
        if (lex_peek(T_wstring, NULL))
            parse_wstring_array_init(var, parent, bb);
        else
            parse_string_array_init(var, parent, bb);
        lex_accept(T_comma);
        lex_expect(T_close_curly);
        global_constant_initializer_scope = saved_initializer_scope;
        return;
    }

    if (emit_code)
        base_addr = var;

    /* Reordered array designators can leave holes both before and after a
     * written element. Initialize the whole automatic array first, then let
     * explicit elements overwrite their slots. Byte stores also cover record
     * elements without relying on a backend-wide aggregate store.
     */
    if (parent != GLOBAL_BLOCK && emit_code && !is_implicit) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        for (int i = 0; i < var->array_size; i++) {
            var_t *elem_addr =
                compute_element_address(parent, bb, base_addr, i, elem_size);
            for (int offset = 0; offset < elem_size; offset++) {
                var_t *byte_addr =
                    compute_element_address(parent, bb, elem_addr, offset, 1);
                add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1, NULL);
            }
        }
    }

    lex_expect(T_open_curly);
    reject_empty_initializer_in_strict_c99();
    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            var_t *val = NULL;
            var_t designated_array;
            var_t *initializer_var = var;
            int prior_count = count;
            bool elided_comma = false;

            if (lex_accept(T_open_square)) {
                int index;
                int stride = 1;
                int elements;
                int remaining_dims = 0;

                index = read_const_expr(initializer_scope);
                lex_expect(T_close_square);
                if (var->array_dim2) {
                    stride = var->array_dim2;
                    remaining_dims = 1;
                    if (var->array_dim3) {
                        stride *= var->array_dim3;
                        remaining_dims = 2;
                        if (var->array_dim4) {
                            stride *= var->array_dim4;
                            remaining_dims = 3;
                        }
                    }
                }
                elements = var->array_size / stride;
                if (index < 0 || (!is_implicit && index >= elements))
                    error_at("Array designator index is out of bounds",
                             cur_token_loc());
                count = index * stride;

                /* A complete multidimensional designator names a scalar
                 * element. Keep the outer-only form on the existing braced
                 * slice path, but turn every complete path into the row-major
                 * flat offset used by the ordinary store path.
                 */
                while (remaining_dims && lex_accept(T_open_square)) {
                    index = read_const_expr(initializer_scope);
                    lex_expect(T_close_square);
                    if (remaining_dims == 3) {
                        elements = var->array_dim2;
                        stride = var->array_dim3 * var->array_dim4;
                    } else if (remaining_dims == 2) {
                        elements = var->array_dim2;
                        stride = var->array_dim3;
                        if (var->array_dim4) {
                            elements = var->array_dim3;
                            stride = var->array_dim4;
                        }
                    } else {
                        elements = var->array_dim2;
                        if (var->array_dim3)
                            elements = var->array_dim3;
                        if (var->array_dim4)
                            elements = var->array_dim4;
                        stride = 1;
                    }
                    if (index < 0 || index >= elements)
                        error_at("Array designator index is out of bounds",
                                 cur_token_loc());
                    count += index * stride;
                    remaining_dims--;
                }
                if (remaining_dims == 1 &&
                    (var->array_dim3 || var->array_dim4)) {
                    /* A path ending one dimension short of a scalar names a
                     * row. Present it to the braced-row helper rather than
                     * asking a scalar initializer to consume `{ ... }`.
                     */
                    int row_width =
                        var->array_dim4 ? var->array_dim4 : var->array_dim3;
                    memcpy(&designated_array, var, sizeof(designated_array));
                    designated_array.array_size = row_width;
                    designated_array.array_dim2 = row_width;
                    designated_array.array_dim3 = 0;
                    designated_array.array_dim4 = 0;
                    initializer_var = &designated_array;
                }
                lex_expect(T_assign);
            }

            if (!is_implicit && count >= var->array_size)
                error_at("Too many elements in array initializer",
                         next_token_loc());

            if (initializer_var->array_dim4 && lex_peek(T_open_curly, NULL)) {
                parse_array_field_hyperplane_init(parent, bb, initializer_var,
                                                  base_addr, count, emit_code);
                count += fixed_array_inner_count(initializer_var);
                if (is_implicit && count > inferred_size)
                    inferred_size = count;
                if (!lex_accept(T_comma))
                    break;
                continue;
            } else if (initializer_var->array_dim3 &&
                       lex_peek(T_open_curly, NULL)) {
                parse_array_field_plane_init(parent, bb, initializer_var,
                                             base_addr, count, emit_code);
                count += fixed_array_inner_count(initializer_var);
                if (is_implicit && count > inferred_size)
                    inferred_size = count;
                if (!lex_accept(T_comma))
                    break;
                continue;
            } else if (initializer_var->array_dim2 &&
                       lex_peek(T_open_curly, NULL)) {
                parse_array_field_row_init(parent, bb, initializer_var,
                                           base_addr, count, emit_code);
                count += fixed_array_inner_count(initializer_var);
                if (is_implicit && count > inferred_size)
                    inferred_size = count;
                if (!lex_accept(T_comma))
                    break;
                continue;
            } else if (string_row_starts_here(initializer_var)) {
                if (is_implicit && emit_code && parent != GLOBAL_BLOCK)
                    emit_zero_elements(parent, bb, base_addr, inferred_size,
                                       count, elem_size);
                parse_string_row_init(parent, bb, initializer_var, base_addr,
                                      count, emit_code);
                count += string_row_width(initializer_var);
                if (is_implicit && count > inferred_size)
                    inferred_size = count;
                if (!lex_accept(T_comma) || lex_peek(T_close_curly, NULL))
                    break;
                continue;
            } else if (lex_peek(T_open_curly, NULL) &&
                       is_record_type(var->type)) {
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
            } else if (emit_code && unbraced_record_starts_here(var)) {
                /* Without braces a record element takes one initializer per
                 * member, so `struct p a[2] = { 1, 2, 3, 4 }` fills a[0] from 1
                 * and 2 before a[1] starts. The record parser stops after its
                 * last member and leaves the comma to this loop.
                 */
                var_t *elem_addr = compute_element_address(
                    parent, bb, base_addr, count, elem_size);

                if (is_implicit && parent != GLOBAL_BLOCK)
                    emit_zero_elements(parent, bb, base_addr, inferred_size,
                                       count, elem_size);
                elided_comma = parse_unbraced_record_init(parent, bb, var->type,
                                                          elem_addr, emit_code);
            } else {
                /* A global initializer is restricted to simple constants, but
                 * it still has to be stored. Consuming the tokens and dropping
                 * the value left every global array zero-filled, while the same
                 * initializer on a local worked.
                 */
                if (parent == GLOBAL_BLOCK &&
                    (lex_peek(T_ampersand, NULL) ||
                     grouped_global_function_designator_starts_here(true) ||
                     global_function_address_dereference_starts_here() ||
                     lex_peek(T_open_bracket, NULL) ||
                     lex_peek(T_sizeof, NULL) || lex_peek(T_plus, NULL) ||
                     lex_peek(T_bit_not, NULL) || lex_peek(T_log_not, NULL))) {
                    /* Addresses, casts, sizeof and grouped or unary constant
                     * expressions share the reader of aggregate members.
                     */
                    val = parse_global_constant_value(parent, bb);
                } else {
                    char initializer_name[MAX_ID_LEN];
                    char global_token[MAX_ID_LEN];
                    bool function_initializer =
                        lex_peek(T_identifier, initializer_name) &&
                        find_visible_func(initializer_name, initializer_scope);
                    var_t *object_constant = NULL;

                    if (parent == GLOBAL_BLOCK &&
                        lex_peek(T_identifier, global_token))
                        object_constant =
                            find_var(global_token, initializer_scope);

                    /* A static array named by a block-scope static's element is
                     * an address constant (C99 6.6p7), not an integer one, and
                     * takes the array-to-pointer path below.
                     */
                    if (parent == GLOBAL_BLOCK &&
                        initializer_scope != GLOBAL_BLOCK &&
                        !lex_peek(T_string, NULL) && !function_initializer &&
                        !lex_peek(T_ampersand, NULL) &&
                        !(object_constant && object_constant->is_global &&
                          object_constant->array_size)) {
                        /* Storage for a block-scope static lives globally,
                         * while its initializer is an integer constant
                         * expression in the surrounding block. Resolve local
                         * enumerators before emitting the global setup-store
                         * value.
                         */
                        if (typed_global_literal_appears_before_initializer_end(
                                cur_token->next)) {
                            /* read_const_expr() evaluates in an int, which
                             * drops the high word of a wide element.
                             */
                            val = read_wide_global_literal_expression(
                                GLOBAL_BLOCK, *bb, var->scope);
                        } else {
                            val = require_var(GLOBAL_BLOCK);
                            val->var_name = gen_name();
                            val->init_val = read_const_expr(var->scope);
                            val->is_const = true;
                            add_insn(GLOBAL_BLOCK, *bb, OP_load_constant, val,
                                     NULL, NULL, 0, NULL);
                        }
                    } else {
                        if (parent == GLOBAL_BLOCK) {
                            char token[MAX_ID_LEN];
                            bool enum_constant =
                                lex_peek(T_identifier, token) &&
                                find_scoped_constant(token, parent);
                            bool function_constant =
                                lex_peek(T_identifier, token) &&
                                find_visible_func(token, initializer_scope);

                            if (!lex_peek(T_numeric, NULL) &&
                                !lex_peek(T_minus, NULL) &&
                                !lex_peek(T_string, NULL) &&
                                !lex_peek(T_char, NULL) &&
                                !lex_peek(T_wchar, NULL) && !enum_constant &&
                                !function_constant &&
                                !lex_peek(T_ampersand, NULL) &&
                                !(object_constant &&
                                  object_constant->is_global &&
                                  object_constant->array_size))
                                error_at(
                                    "Global array initialization requires "
                                    "constant "
                                    "values",
                                    next_token_loc());
                        }

                        if (parent == GLOBAL_BLOCK && object_constant &&
                            object_constant->is_global &&
                            object_constant->array_size) {
                            int stride = (object_constant->ptr_level ||
                                          object_constant->is_func)
                                             ? PTR_SIZE
                                             : object_constant->type->size;

                            /* Array-to-pointer conversion is a permitted
                             * address constant in static aggregate
                             * initializers. Preserve its row stride here rather
                             * than reading an object value, which global setup
                             * cannot do before GP is established.
                             */
                            val = require_ref_var(parent, object_constant->type,
                                                  object_constant->ptr_level);
                            val->var_name = gen_name();
                            lex_ident(T_identifier, global_token);
                            add_insn(parent, *bb, OP_address_of, val,
                                     object_constant, NULL, 0, NULL);
                            if (object_constant->array_dim2)
                                stride *= object_constant->array_dim2;
                            if (object_constant->array_dim3)
                                stride *= object_constant->array_dim3;
                            if (object_constant->array_dim4)
                                stride *= object_constant->array_dim4;
                            if (object_constant->array_dim2 &&
                                lex_peek(T_open_square, NULL)) {
                                var_t *row = object_constant;

                                val->is_global_address = true;
                                val = read_global_address_designator(
                                    initializer_scope, parent, bb, &row, val,
                                    true);
                            } else if (lex_accept(T_plus)) {
                                int index = read_const_expr(initializer_scope);

                                val = compute_element_address(parent, bb, val,
                                                              index, stride);
                            } else if (lex_accept(T_minus)) {
                                int index = read_const_expr(initializer_scope);

                                val = compute_element_address(parent, bb, val,
                                                              -index, stride);
                            }
                        } else {
                            read_expr(parent, bb);
                            read_ternary_operation(parent, bb);
                            val = opstack_pop();
                        }
                    }
                }
            }

            if (is_implicit && count >= MAX_IMPLICIT_ARRAY)
                error_at("Too many elements in array initializer",
                         next_token_loc());

            if (val && var->type->array_element_pointee_func_signature) {
                /* The array descriptor is not an element descriptor. Build the
                 * slot type that this scalar initializer is about to store so
                 * callback prototype validation remains elementwise.
                 */
                var_t element_target = {0};

                element_target.type = var->type;
                element_target.ptr_level = var->type->array_element_ptr_level;
                element_target.pointee_func_signature =
                    var->type->array_element_pointee_func_signature;
                if (incompatible_pointee_callback_conversion(val,
                                                             &element_target))
                    error_at(
                        "incompatible callback slot types in array "
                        "initializer",
                        cur_token_loc());
            }

            if (val && emit_code && (is_implicit || count < var->array_size)) {
                /* Keep a function symbol intact until OP_address_of_func can
                 * emit its deferred relocation. Treating an array-of-callback
                 * element as an `int` conversion loses that provenance.
                 */
                var_t *v;

                if (val->is_func || var->ptr_level > 0)
                    v = val;
                else
                    v = resize_to(parent, bb, val, var->type, 0);

                /* A forward designator leaves a gap. Explicit arrays were
                 * zeroed before parsing; inferred local arrays do not have a
                 * known bound yet, so zero just the newly skipped elements.
                 * Global storage begins zeroed.
                 */
                if (is_implicit && parent != GLOBAL_BLOCK &&
                    count > prior_count) {
                    var_t *zero = require_var(parent);
                    zero->var_name = gen_name();
                    zero->init_val = 0;
                    add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0,
                             NULL);
                    for (int i = prior_count; i < count; i++) {
                        var_t *gap_addr = compute_element_address(
                            parent, bb, base_addr, i, elem_size);
                        for (int offset = 0; offset < elem_size; offset++) {
                            var_t *byte_addr = compute_element_address(
                                parent, bb, gap_addr, offset, 1);
                            add_insn(parent, *bb, OP_write, NULL, byte_addr,
                                     zero, 1, NULL);
                        }
                    }
                }

                var_t *elem_addr = compute_element_address(
                    parent, bb, base_addr, count, elem_size);

                /* A direct long long element is wider than a pointer on a
                 * 32-bit target, where it is written as a register pair.
                 */
                if (elem_size <= PTR_SIZE ||
                    (elem_size == 8 && !var->ptr_level && !var->is_func &&
                     !is_record_type(var->type))) {
                    add_insn(parent, *bb, OP_write, NULL, elem_addr, v,
                             elem_size, NULL);
                } else {
                    fatal("Unsupported: array element wider than a pointer");
                }
            }

            count++;
            if (is_implicit && count > inferred_size)
                inferred_size = count;
            if (!elided_comma && !lex_accept(T_comma))
                break;
            if (lex_peek(T_close_curly, NULL))
                break;
        }
    }

    lex_expect(T_close_curly);

    if (is_implicit) {
        /* `count` walks scalars, so a brace-elided list that stops inside a
         * row, as in `int m[][2] = { 1, 2, 3 }`, leaves inferred_size short of
         * a whole outer element. The bound counts outer elements, so round up
         * and zero the rest of that last element in automatic storage.
         */
        int inner = fixed_array_inner_count(var);
        int whole = (inferred_size + inner - 1) / inner * inner;

        if (emit_code && parent != GLOBAL_BLOCK)
            emit_zero_elements(parent, bb, base_addr, inferred_size, whole,
                               elem_size);
        inferred_size = whole;
        var->array_size = inferred_size;
        var->has_unsized_array = false;
    }
    global_constant_initializer_scope = saved_initializer_scope;
}

void parse_array_compound_literal(var_t *var,
                                  block_t *parent,
                                  basic_block_t **bb)
{
    int elem_size = var->type->size;
    int count = 0;

    reject_empty_initializer_in_strict_c99();

    /* A compound literal may spell either an inferred bound, ``int[]``, or an
     * actual array type, ``int[4]``. The latter is not merely syntax: omitted
     * members are zero-initialized and an excess initializer is a constraint
     * violation. Keep the parsed bound until the initializer has been consumed;
     * previously this routine reset it and silently turned every declared-bound
     * literal into an inferred-size one.
     */
    int declared_size = var->array_size;
    int inferred_size = 0;
    var->init_val = 0;

    /* The opening brace is already consumed. See parse_array_init(). */
    if (string_ends_braced_initializer(var, cur_token->next)) {
        if (lex_peek(T_wstring, NULL))
            parse_wstring_array_init(var, parent, bb);
        else
            parse_string_array_init(var, parent, bb);
        lex_accept(T_comma);
        lex_expect(T_close_curly);
        return;
    }

    /* A designated element can leave holes before or after it, so initialize
     * the declared object before parsing any explicit elements.
     */
    if (declared_size) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        for (int i = 0; i < declared_size; i++) {
            var_t *elem_addr =
                compute_element_address(parent, bb, var, i, elem_size);
            for (int offset = 0; offset < elem_size; offset++) {
                var_t *byte_addr =
                    compute_element_address(parent, bb, elem_addr, offset, 1);
                add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1, NULL);
            }
        }
    }

    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            bool elided_comma = false;

            if (lex_accept(T_open_square)) {
                count = read_const_expr(parent);
                lex_expect(T_close_square);
                lex_expect(T_assign);
            }
            if (declared_size && count >= declared_size)
                error_at("Too many elements in array compound literal",
                         next_token_loc());
            if (!declared_size && count >= MAX_IMPLICIT_ARRAY)
                error_at("Too many elements in array compound literal",
                         next_token_loc());

            /* An inferred-bound array gets its size only after the closing
             * brace. Still zero every gap before storing a designator so the
             * automatic object obeys C99's aggregate initialization rule.
             * inferred_size is one past the highest initialized slot, so a
             * later backward designator cannot make a forward gap overwrite an
             * earlier explicit value.
             */
            if (!declared_size && count > inferred_size) {
                var_t *zero = require_var(parent);
                zero->var_name = gen_name();
                zero->init_val = 0;
                add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0,
                         NULL);
                for (int i = inferred_size; i < count; i++) {
                    var_t *gap_addr =
                        compute_element_address(parent, bb, var, i, elem_size);
                    for (int offset = 0; offset < elem_size; offset++) {
                        var_t *byte_addr = compute_element_address(
                            parent, bb, gap_addr, offset, 1);
                        add_insn(parent, *bb, OP_write, NULL, byte_addr, zero,
                                 1, NULL);
                    }
                }
            }

            var_t *elem_addr =
                compute_element_address(parent, bb, var, count, elem_size);
            if (lex_peek(T_open_curly, NULL) && is_record_type(var->type)) {
                /* The array compound literal owns a real aggregate object, just
                 * like an ordinary array initializer. A braced element must
                 * therefore be lowered through the shared record path; treating
                 * it as an expression rejected the opening brace and made
                 * (struct S[]){ { ... }, { ... } } unusable.
                 */
                type_t *record_type = var->type;
                if (record_type->base_type == TYPE_typedef &&
                    record_type->base_struct)
                    record_type = record_type->base_struct;

                lex_expect(T_open_curly);
                parse_struct_field_init(parent, bb, record_type, elem_addr,
                                        true);
                lex_expect(T_close_curly);
            } else if (unbraced_record_starts_here(var)) {
                elided_comma = parse_unbraced_record_init(parent, bb, var->type,
                                                          elem_addr, true);
            } else {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
                var_t *value = opstack_pop();
                if (count == 0)
                    var->init_val = value->init_val;

                var_t *store_val = resize_to(parent, bb, value, var->type, 0);
                add_insn(parent, *bb, OP_write, NULL, elem_addr, store_val,
                         elem_size, NULL);
            }

            if (!declared_size) {
                if (count + 1 > inferred_size)
                    inferred_size = count + 1;
            }
            count++;
            if (!elided_comma && !lex_accept(T_comma))
                break;
            if (lex_peek(T_close_curly, NULL))
                break;
        }
    }

    lex_expect(T_close_curly);

    var->array_size = declared_size ? declared_size : inferred_size;
}

/* Identify compiler-emitted temporaries that hold array compound literals. They
 * keep array metadata without pointer indirection and are marked via
 * is_compound_literal when synthesized.
 */
bool is_array_literal_placeholder(const var_t *var)
{
    return var && var->array_size > 0 && !var->ptr_level &&
           var->is_compound_literal;
}

bool is_pointer_like_value(var_t *var)
{
    return var && (var->ptr_level || var->array_size ||
                   (var->type && var->type->ptr_level > 0));
}

/* Lower a compiler-emitted array literal placeholder (marked via
 * is_compound_literal) into a scalar temporary when later IR expects a plain
 * value instead of addressable storage. This keeps SSA joins uniform when only
 * one branch originates from an array literal.
 */
var_t *scalarize_array_literal(block_t *parent,
                               basic_block_t **bb,
                               var_t *array_var,
                               type_t *hint_type)
{
    if (!is_array_literal_placeholder(array_var))
        return array_var;

    if (strict_c99)
        error_at("array compound literal cannot be used as a scalar in C99",
                 cur_token_loc());

    /* Array literal placeholders carry the literal's natural type; default to
     * int when the parser left the type unset.
     */
    type_t *literal_type = array_var->type ? array_var->type : TY_int;
    int literal_size = literal_type->size;
    if (literal_size <= 0)
        literal_size = TY_int->size;

    /* A caller-provided hint (e.g., assignment target) dictates the result type
     * when available so we reuse wider/narrower scalar destinations.
     */
    type_t *result_type = hint_type ? hint_type : literal_type;
    if (!result_type)
        result_type = TY_int;

    /* Create a new scalar temporary, giving it a unique name and copying over
     * the literal data so downstream code can treat it like a normal value.
     */
    var_t *scalar = require_typed_var(parent, result_type);
    scalar->ptr_level = 0;
    scalar->var_name = gen_name();
    scalar->init_val = array_var->init_val;

    /* Materialize the literal data into the scalar temporary via an OP_read. */
    add_insn(parent, *bb, OP_read, scalar, array_var, NULL, literal_size, NULL);

    return scalar;
}

/* Centralized guard for lowering array literal placeholders when a scalar value
 * is expected, keeping the scattered special cases consistent.
 */
var_t *scalarize_array_literal_if_needed(block_t *parent,
                                         basic_block_t **bb,
                                         var_t *value,
                                         type_t *hint_type,
                                         bool needs_scalar)
{
    if (!needs_scalar)
        return value;

    return scalarize_array_literal(parent, bb, value, hint_type);
}
