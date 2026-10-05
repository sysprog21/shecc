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
var_t *read_wide_global_literal_primary(block_t *parent,
                                        basic_block_t *bb,
                                        block_t *scope);
bool subscripted_string_literal_starts_here(void);
bool string_element_appears_before_initializer_end(token_t *token);
var_t *read_string_literal_element_address(block_t *parent,
                                           basic_block_t *bb,
                                           block_t *scope);
bool string_address_offset_starts_here(void);
var_t *read_string_address_offset(block_t *parent,
                                  basic_block_t *bb,
                                  block_t *scope);

static void parse_array_init_at(var_t *var,
                                block_t *parent,
                                basic_block_t **bb,
                                var_t *base_addr,
                                bool sparse_zero);

static bool initializer_separator(bool comma_consumed)
{
    return comma_consumed || lex_accept(T_comma);
}

/* Forward declaration for ternary handling used by initializers */
void read_ternary_operation(block_t *parent, basic_block_t **bb);

/* Parse array initializer to determine size for implicit arrays and optionally
 * emit initialization code.
 */
static var_t *compute_byte_offset_address(block_t *parent,
                                          basic_block_t **bb,
                                          var_t *base_addr,
                                          int byte_offset)
{
    if (byte_offset == 0)
        return base_addr;

    var_t *offset = load_constant(parent, *bb, byte_offset, TY_int);

    var_t *addr = require_named_var(parent);
    add_insn(parent, *bb, OP_add, addr, base_addr, offset, 0, NULL);
    return addr;
}

var_t *compute_element_address(block_t *parent,
                               basic_block_t **bb,
                               var_t *base_addr,
                               int index,
                               int elem_size)
{
    return compute_byte_offset_address(parent, bb, base_addr,
                                       index * elem_size);
}

var_t *compute_field_address(block_t *parent,
                             basic_block_t **bb,
                             var_t *struct_addr,
                             const var_t *field)
{
    return compute_byte_offset_address(parent, bb, struct_addr, field->offset);
}

/* A record assignment is a value copy, not the scalar OP_assign used for
 * ordinary variables. Keep the lowering in phase 1 so every backend can use its
 * existing 1-, 2-, and 4-byte indirect accesses.
 */
bool is_record_object(const var_t *var)
{
    return var && !effective_pointer_depth(var) && !var->array_size &&
           var->type &&
           (var->type->base_type == TYPE_struct ||
            var->type->base_type == TYPE_union ||
            (var->type->base_type == TYPE_typedef &&
             var->type->num_fields > 0));
}

/* Choose the widest indirect access allowed by the remaining bytes and
 * alignment. Widths descend by halves, matching backend scalar stores.
 */
static int byte_access_width(int remaining, int alignment, int offset)
{
    for (int width = 4; width > 1; width /= 2)
        if (remaining >= width && alignment >= width && !(alignment % width) &&
            !(offset % width))
            return width;
    return 1;
}

static int initializer_alignment(const var_t *var)
{
    if (!var)
        return 1;
    return var->is_func || effective_pointer_depth(var)
               ? PTR_SIZE
               : alignment_type(var->type);
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
        int width = byte_access_width(size - offset, PTR_SIZE, offset);
        var_t *src_part =
            compute_element_address(parent, bb, src_addr, offset, 1);
        var_t *dest_part =
            compute_element_address(parent, bb, dest_addr, offset, 1);
        var_t *value = require_named_var(parent);
        add_insn(parent, *bb, OP_read, value, src_part, NULL, width, NULL);
        add_insn(parent, *bb, OP_write, NULL, dest_part, value, width, NULL);
        offset += width;
    }
}

/* The address of the bytes of record value @record. A record whose copy was
 * deferred still has them at the object it was read from, and a copy out of it
 * reads them there.
 */
var_t *record_value_address(block_t *parent, basic_block_t *bb, var_t *record)
{
    var_t *address;

    if (record->defers_record_copy)
        return record->compound_literal_address;
    address = name_var(require_ref_var(parent, record->type, 0));
    add_insn(parent, bb, OP_address_of, address, record, NULL, 0, NULL);
    return address;
}

/* Copy a record into an address which is already known, such as a nested record
 * member.
 */
void emit_record_copy_to_address(block_t *parent,
                                 basic_block_t **bb,
                                 var_t *dest_addr,
                                 var_t *src)
{
    var_t *src_addr = record_value_address(parent, *bb, src);

    emit_record_copy_between(parent, bb, dest_addr, src_addr, size_var(src));
}

void emit_record_copy(block_t *parent,
                      basic_block_t **bb,
                      var_t *dest,
                      var_t *src)
{
    var_t *dest_addr = require_ref_var(parent, dest->type, 0);
    var_t *src_addr;

    dest_addr->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, dest_addr, dest, NULL, 0, NULL);
    src_addr = record_value_address(parent, *bb, src);
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
        add_insn(parent, *bb, OP_address_of, dest_addr, dest, NULL, 0, NULL);
        add_insn(parent, *bb, OP_write, NULL, dest_addr, src, PTR_SIZE, NULL);
    } else {
        src = resize_var(parent, bb, src, dest);
        add_insn(parent, *bb, OP_assign, dest, src, NULL, 0, NULL);
    }
}

type_t *read_type_name_specifiers(block_t *scope);
void read_type_name_decl(block_t *scope, var_t *decl);
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

/* While the operand of a cast to a function pointer type in a static
 * initializer is read, the prototype that cast names, and NULL otherwise. The
 * cast, not the designated function, is converted to the initialized object.
 */
func_t *global_function_cast_signature = NULL;

bool abstract_function_pointer_follows(void);
func_t *read_abstract_function_pointer(const var_t *return_decl,
                                       int *pointer_level);

/* If a cast to a function pointer type, `(int (*)(int))` or `(callback_t)`,
 * starts at the next token of a static initializer, consume it and return the
 * prototype it names. Otherwise consume nothing and return NULL.
 */
func_t *read_global_function_pointer_cast(block_t *scope)
{
    token_t *start = cur_token;
    func_t *signature = NULL;
    type_t *type;
    int stars = 0;
    int pointer_level = 0;
    var_t return_decl;

    if (!lex_accept(T_open_bracket))
        return NULL;
    read_type_name_decl(scope, &return_decl);
    type = return_decl.type;
    stars = return_decl.ptr_level;
    if (type) {
        if (abstract_function_pointer_follows()) {
            signature =
                read_abstract_function_pointer(&return_decl, &pointer_level);
            if (pointer_level != 1)
                signature = NULL;
        } else if (type->func_signature && !type->is_direct_function_type &&
                   !stars) {
            signature = type->func_signature;
        } else if (type->is_direct_function_type && stars == 1) {
            signature = type->func_signature;
        }
    }
    if (!signature || !lex_accept(T_close_bracket)) {
        cur_token = start;
        return NULL;
    }
    return signature;
}

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
    bool function_type = false;
    int depth = 0;

    if (!token || token->kind != T_open_bracket)
        return false;
    for (token = token->next; token && token->kind != T_close_bracket;
         token = token->next) {
        if (token->kind == T_asterisk) {
            if (!has_type)
                return false;
            is_pointer = true;
            depth++;
        } else if (token->kind == T_open_bracket && has_type) {
            /* A pointer to function pointers, `(int (**)(void))`, points to
             * pointer objects: two or more stars, then a parameter list.
             */
            int stars;

            token = function_pointer_declarator_suffix(token, &stars);
            if (stars < 2 || !token || token->kind != T_close_bracket ||
                !(token = token->next) || token->kind != T_open_bracket)
                return false;
            token =
                skip_balanced_delimiter(token, T_open_bracket, T_close_bracket);
            if (!token || !(token = token->next))
                return false;
            return token->kind == T_close_bracket;
        } else if (token->kind == T_identifier) {
            bool is_tag = previous == T_struct || previous == T_union ||
                          previous == T_enum;
            type_t *type =
                is_tag ? NULL : find_visible_type(token->literal, scope);

            if (!is_tag && !type)
                return false;
            if (type && type->is_direct_function_type)
                function_type = true;

            /* A callback typedef takes a star to point to a pointer object. */
            if (type && type->ptr_level && !type->func_signature)
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

    /* Without a star, a callback typedef is the function pointer cast that
     * read_global_function_pointer_cast() reads, and so is a function typedef
     * with one; a second star, `(fnty **)`, points to a pointer object.
     */
    return token && has_type && is_pointer && (!function_type || depth > 1);
}

/* Consume a cast that global_pointer_cast_starts_here() recognized and return
 * the size of what its pointer type points to. @slot_signature receives the
 * prototype of the function pointers that type points to, as `(int (**)(void))`
 * or `(callback_t *)` do, and NULL for any other pointer.
 */
int read_global_pointer_cast(block_t *scope, func_t **slot_signature)
{
    var_t return_decl;
    type_t *type;
    int depth = 0;
    int size;

    *slot_signature = NULL;
    lex_expect(T_open_bracket);
    read_type_name_decl(scope, &return_decl);
    type = return_decl.type;
    depth = return_decl.ptr_level;
    if (!type)
        error_at("Unknown type in pointer cast", cur_token_loc());

    /* A pointer to function pointers, spelled out or through a callback
     * typedef, steps over pointer objects.
     */
    if (abstract_function_pointer_follows()) {
        int pointer_level;
        func_t *signature =
            read_abstract_function_pointer(&return_decl, &pointer_level);

        lex_expect(T_close_bracket);
        if (pointer_level == 2)
            *slot_signature = signature;
        return PTR_SIZE;
    }
    lex_expect(T_close_bracket);
    if (type->func_signature &&
        (type->is_direct_function_type ? depth == 2
                                       : !type->ptr_level && depth == 1))
        *slot_signature = type->func_signature;
    if (depth + type->ptr_level > 1 || type->func_signature)
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

/* Read an integer constant operand of a pointer cast with @stride and
 * @slot_signature, and any offset that follows it, as the address constant the
 * cast produces.
 */
var_t *read_global_cast_integer_address(block_t *parent,
                                        basic_block_t *bb,
                                        block_t *scope,
                                        int stride,
                                        func_t *slot_signature)
{
    var_t *address = require_var(parent);
    char name[MAX_ID_LEN];
    unsigned int lo;
    unsigned int hi;

    /* A pointer is as wide as the target's: read the operand in two words so a
     * 64-bit target keeps its high half, as (char *) 0x100000000 needs. Only an
     * identifier other than an enumerator, such as offsetof, takes the
     * word-sized reader.
     */
    if (lex_peek(T_identifier, name) && !find_scoped_constant(name, scope)) {
        lo = read_const_expr_operand(scope);
        hi = lo & 0x80000000U ? ~0U : 0;
    } else {
        var_t *operand = read_wide_global_literal_primary(parent, bb, scope);

        lo = operand->init_val;
        hi = operand->init_val_hi;
        if (operand->type->size <= TY_int->size)
            hi = operand->type->is_unsigned || !(lo & 0x80000000U) ? 0 : ~0U;
    }
    if (lex_peek(T_plus, NULL) || lex_peek(T_minus, NULL)) {
        int offset = read_global_address_offset(scope, parent, bb) * stride;
        unsigned int sum = lo + offset;

        hi += (offset < 0 ? ~0U : 0) + (sum < lo);
        lo = sum;
    }
    address->var_name = gen_name();
    address->init_val = lo;
    address->init_val_hi = PTR_SIZE == 8 ? hi : 0;
    address->is_const = true;

    /* The cast's result is a pointer, not the integer it was spelled with, and
     * a pointer to function pointers keeps their prototype.
     */
    address->ptr_level = 1;
    address->pointee_func_signature = slot_signature;
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
    int element_size = target->ptr_level ? PTR_SIZE : target->type->size;
    int stride = global_pointer_cast_stride
                     ? global_pointer_cast_stride
                     : fixed_array_shape_stride(&shape, subscripts - !decays,
                                                element_size);

    if (lex_peek(T_plus, NULL) || lex_peek(T_minus, NULL)) {
        int index = read_global_address_offset(scope, parent, *bb);

        object_addr =
            compute_element_address(parent, bb, object_addr, index, stride);
        object_addr->ptr_level = target->ptr_level + 1;
        object_addr->is_global_address = true;
    }
    object_addr->address_stride = stride;
    *object = target;
    return object_addr;
}

static var_t *read_global_object_address(block_t *scope,
                                         block_t *parent,
                                         basic_block_t **bb,
                                         var_t *object,
                                         bool decays)
{
    var_t *address =
        name_var(require_ref_var(parent, object->type, object->ptr_level));

    address->is_global_address = true;
    add_insn(parent, *bb, OP_address_of, address, object, NULL, 0, NULL);
    return read_global_address_designator(scope, parent, bb, &object, address,
                                          decays);
}

/* The block that names in an initializer lowered into @parent resolve in. A
 * block-scope static is lowered through GLOBAL_BLOCK, yet its designators and
 * constants belong to the lexical scope of its declaration.
 */
block_t *initializer_name_scope(block_t *parent)
{
    if (parent == GLOBAL_BLOCK && global_constant_initializer_scope)
        return global_constant_initializer_scope;
    return parent;
}

/* Nonzero while the constant expression reader has this function read one
 * address operand of the expression it is already reading.
 */
int global_tested_operand_depth;

/* Whether the element ahead is an address constant that an operator only
 * tests, as in `"a" && 1`, `1 ? "a" : "b"` or `array || 0`. Such an element is
 * a constant expression, not the address itself, so the whole expression
 * belongs to the constant expression reader.
 */
static bool global_tested_operand_starts_here(block_t *scope)
{
    char name[MAX_ID_LEN];
    token_t *after;

    if (global_tested_operand_depth || !cur_token->next)
        return false;
    if (!lex_peek(T_string, NULL) &&
        !(lex_peek(T_identifier, name) &&
          (find_visible_func(name, scope) || find_var(name, scope))))
        return false;
    after = cur_token->next->next;
    return after && (after->kind == T_log_and || after->kind == T_log_or ||
                     after->kind == T_question);
}

var_t *parse_global_constant_value(block_t *parent, basic_block_t **bb)
{
    var_t *val = NULL;
    block_t *scope = initializer_name_scope(parent);
    bool address_dereference =
        global_function_address_dereference_starts_here();
    token_t *address_dereference_identifier = NULL;
    bool explicit_address;
    bool grouped_function_designator = false;

    /* A cast to a function pointer type converts the function designator or
     * null pointer constant after it, which then carries the cast's prototype.
     */
    func_t *cast_signature = read_global_function_pointer_cast(scope);

    if (cast_signature) {
        val = parse_global_constant_value(parent, bb);
        if (val && val->is_func)
            val->func_signature = cast_signature;
        else if (val && !effective_pointer_depth(val)) {
            /* An integer cast to a function pointer type is an explicit
             * conversion: keep the cast's type on the value, so the element
             * store does not take it for an implicit one.
             */
            val->ptr_level = 1;
            val->func_signature = cast_signature;
        }
        return val;
    }

    if (address_dereference) {
        address_dereference_identifier =
            consume_global_function_address_dereference();
        func_t *addressed =
            find_visible_func(address_dereference_identifier->literal, scope);
        if (!addressed)
            error_at("Function address requires a visible declaration",
                     cur_token_loc());
        val = function_designator_value(parent, addressed);
        return val;
    }
    if (global_pointer_cast_starts_here(scope)) {
        int saved_stride = global_pointer_cast_stride;
        func_t *slot_signature;
        int stride = read_global_pointer_cast(scope, &slot_signature);

        /* In a chain of casts the outermost one decides the stride. */
        if (saved_stride)
            stride = saved_stride;
        if (!global_address_operand_starts_here(scope))
            return read_global_cast_integer_address(parent, *bb, scope, stride,
                                                    slot_signature);
        global_pointer_cast_stride = stride;
        val = parse_global_constant_value(parent, bb);
        global_pointer_cast_stride = saved_stride;
        return val;
    }
    explicit_address = lex_accept(T_ampersand);
    if (explicit_address && subscripted_string_literal_starts_here())
        return read_string_literal_element_address(parent, *bb, scope);
    if (!explicit_address && string_address_offset_starts_here())
        return read_string_address_offset(parent, *bb, scope);

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
         lex_peek(T_wchar, NULL) || subscripted_string_literal_starts_here() ||
         global_tested_operand_starts_here(scope) ||
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
                val = function_designator_value(parent, func);
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
                    return read_global_object_address(scope, parent, bb, object,
                                                      false);
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
                    return read_global_object_address(scope, parent, bb, object,
                                                      true);
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

bool is_record_type(const type_t *type)
{
    /* A callback typedef whose function returns a record keeps that record's
     * members for its prototype, but names a pointer, not a record.
     */
    if (type && type->func_signature && !type->is_direct_function_type)
        return false;

    /* A pointer typedef that defines its record, `typedef struct r {...} *rp`,
     * copies the record's fields but names a pointer, not a record.
     */
    return type &&
           (type->base_type == TYPE_struct || type->base_type == TYPE_union ||
            (type->base_type == TYPE_typedef && type->num_fields > 0 &&
             !type->ptr_level));
}

void parse_struct_field_init(block_t *parent,
                             basic_block_t **bb,
                             type_t *struct_type,
                             var_t *target_addr);
bool parse_struct_field_values(block_t *parent,
                               basic_block_t **bb,
                               type_t *struct_type,
                               var_t *target_addr,
                               bool elided,
                               bool cleared,
                               var_t *first_value);
bool parse_unbraced_record_init(block_t *parent,
                                basic_block_t **bb,
                                type_t *record_type,
                                var_t *addr,
                                bool cleared);
bool unbraced_record_starts_here(const var_t *elem);
bool string_row_starts_here(const var_t *array);
void parse_string_row_init(block_t *parent,
                           basic_block_t **bb,
                           const var_t *array,
                           var_t *target_addr,
                           int start);

/* Store @zero into each byte at @addr, using @alignment to select the widest
 * safe indirect store.
 */
void emit_zero_bytes(block_t *parent,
                     basic_block_t **bb,
                     var_t *addr,
                     int size,
                     int alignment,
                     var_t *zero)
{
    for (int offset = 0; offset < size;) {
        int width = byte_access_width(size - offset, alignment, offset);
        var_t *part = compute_element_address(parent, bb, addr, offset, 1);

        add_insn(parent, *bb, OP_write, NULL, part, zero, width, NULL);
        offset += width;
    }
}

var_t *emit_zero_constant(block_t *parent, basic_block_t **bb)
{
    return load_constant(parent, *bb, 0, TY_int);
}

/* Store zero into every byte of elements [from, to) of the array at @base. */
void emit_zero_elements(block_t *parent,
                        basic_block_t **bb,
                        var_t *base,
                        const var_t *element,
                        int from,
                        int to,
                        int elem_size)
{
    var_t *zero;

    if (from >= to)
        return;
    zero = emit_zero_constant(parent, bb);
    for (int i = from; i < to; i++) {
        var_t *address =
            compute_element_address(parent, bb, base, i, elem_size);

        if (elem_size == 1 ||
            (elem_size <= 4 && element && element->type &&
             (element->is_func || effective_pointer_depth(element) ||
              !is_record_type(element->type))))
            add_insn(parent, *bb, OP_write, NULL, address, zero, elem_size,
                     NULL);
        else
            emit_zero_bytes(parent, bb, address, elem_size,
                            initializer_alignment(element), zero);
    }
}

/* The width of one element of @array: a pointer for an array of pointers or of
 * function designators, and the element type's size otherwise.
 */
int array_element_size(const var_t *array)
{
    return (array->ptr_level || array->is_func) ? PTR_SIZE : array->type->size;
}

/* Read an array designator such as `[1]` or `[1][0]` for @array, if one is
 * next. Each subscript is checked against its own bound and descends one level,
 * until a scalar is reached or, when @max_depth is nonzero, that many
 * subscripts have been read. An @unbounded outer bound is still being inferred
 * from the initializer and is not checked. The caller consumes whatever
 * follows.
 *
 * Returns the number of subscripts read. When there were any, *@flat is the
 * row-major element offset of the designated subobject and @slice, if given,
 * holds the shape of the array it is one element of.
 */
int read_array_designator(block_t *scope,
                          const var_t *array,
                          int max_depth,
                          bool unbounded,
                          int *flat,
                          var_t *slice)
{
    var_t element;
    int depth = 0;
    int offset = 0;

    memcpy(&element, array, sizeof(element));
    while ((!max_depth || depth < max_depth) &&
           (!depth || element.array_size) && lex_accept(T_open_square)) {
        int stride = fixed_array_inner_count(&element);
        int index = read_const_expr(scope);

        lex_expect(T_close_square);
        if (index < 0 ||
            ((depth || !unbounded) && index >= element.array_size / stride))
            error_at("Array designator index is out of bounds",
                     cur_token_loc());
        offset += index * stride;
        if (slice)
            memcpy(slice, &element, sizeof(element));

        /* An unbounded outer dimension has no extent to drop yet: descend from
         * a single outer element, whose inner bounds are known.
         */
        if (!depth && unbounded)
            element.array_size = stride;
        fixed_array_var_drop_outer(&element);
        depth++;
    }
    if (depth)
        *flat = offset;
    return depth;
}

/* Read an `[index] =` designator for one of @bound slots, if one is next. */
bool accept_slot_designator(block_t *scope, int bound, int *slot)
{
    var_t slots = {0};

    slots.array_size = bound;
    if (!read_array_designator(scope, &slots, 1, false, slot, NULL))
        return false;
    lex_expect(T_assign);
    return true;
}

static var_t *read_array_scalar_initializer(block_t *parent,
                                            basic_block_t **bb,
                                            bool allow_global_constant,
                                            block_t *global_array_scope)
{
    if (parent == GLOBAL_BLOCK && allow_global_constant) {
        char name[MAX_ID_LEN];

        if (global_array_scope && lex_peek(T_identifier, name) &&
            !global_tested_operand_starts_here(global_array_scope) &&
            !find_scoped_constant(name, global_array_scope) &&
            !find_visible_func(name, global_array_scope)) {
            var_t *object = find_var(name, global_array_scope);

            if (!object || !object->is_global || !object->array_size)
                error_at(
                    global_array_scope == GLOBAL_BLOCK
                        ? "Global array initialization requires constant values"
                        : "Identifier is not an integer constant",
                    next_token_loc());
        }
        return parse_global_constant_value(parent, bb);
    }
    read_expr(parent, bb);
    read_ternary_operation(parent, bb);
    return opstack_pop();
}

static bool parse_array_element_init(block_t *parent,
                                     basic_block_t **bb,
                                     const var_t *field,
                                     var_t *address,
                                     int elem_size,
                                     bool overwritten,
                                     var_t **parsed_value)
{
    var_t *value = NULL;
    bool comma_consumed = false;

    if (parsed_value)
        *parsed_value = NULL;

    if (lex_peek(T_open_curly, NULL) && is_record_type(field->type)) {
        type_t *record_type = resolve_record_type(field->type);
        lex_expect(T_open_curly);
        parse_struct_field_init(parent, bb, record_type, address);
        lex_expect(T_close_curly);
    } else if (unbraced_record_starts_here(field)) {
        comma_consumed = parse_unbraced_record_init(parent, bb, field->type,
                                                    address, overwritten);
    } else {
        value = read_array_scalar_initializer(parent, bb, !parsed_value, NULL);
    }

    if (value) {
        if (parsed_value)
            *parsed_value = value;
        var_t *stored = value->is_func
                            ? value
                            : resize_to(parent, bb, value, field->type,
                                        parsed_value ? 0 : field->ptr_level);
        add_insn(parent, *bb, OP_write, NULL, address, stored, elem_size, NULL);
    }
    return comma_consumed;
}

/* Parse one row of a two-dimensional array. An unbraced row returns true when a
 * brace-elided record element consumed the comma after the row's last
 * initializer, so the enclosing list must not expect it again. Only a braced
 * row owns designators; in an unbraced one they belong to the enclosing list.
 * `filled` is one past the highest slot written, so after designators in any
 * order the zero fill covers exactly the slots nothing wrote.
 */
bool parse_array_field_row_values(block_t *parent,
                                  basic_block_t **bb,
                                  const var_t *field,
                                  var_t *target_addr,
                                  int start,
                                  bool braced)
{
    int count = 0;
    int filled = 0;
    bool comma_consumed = false;
    int elem_size = array_element_size(field);

    if (braced) {
        lex_expect(T_open_curly);
        reject_empty_initializer_in_strict_c99();
    }

    /* The whole row may be one string literal, optionally in its own braces. */
    if (string_row_starts_here(field)) {
        parse_string_row_init(parent, bb, field, target_addr, start);
        if (braced) {
            lex_accept(T_comma);
            lex_expect(T_close_curly);
        }
        return false;
    }
    while (!lex_peek(T_close_curly, NULL)) {
        var_t *elem_addr;

        if (braced)
            accept_slot_designator(initializer_name_scope(parent),
                                   field->array_dim2, &count);
        if (count >= field->array_dim2)
            error_at("Too many elements in array initializer",
                     next_token_loc());
        emit_zero_elements(parent, bb, target_addr, field, start + filled,
                           start + count, elem_size);

        elem_addr = compute_element_address(parent, bb, target_addr,
                                            start + count, elem_size);
        comma_consumed = parse_array_element_init(
            parent, bb, field, elem_addr, elem_size, count < filled, NULL);

        count++;
        if (count > filled)
            filled = count;
        if (count == field->array_dim2) {
            if (braced && initializer_separator(comma_consumed) &&
                !lex_peek(T_close_curly, NULL)) {
                if (lex_peek(T_open_square, NULL))
                    continue;
                error_at("Too many elements in array initializer",
                         next_token_loc());
            }
            break;
        }
        if (!initializer_separator(comma_consumed))
            break;
        comma_consumed = true;
    }
    if (braced) {
        lex_expect(T_close_curly);
        comma_consumed = false;
    }

    emit_zero_elements(parent, bb, target_addr, field, start + filled,
                       start + field->array_dim2, elem_size);
    return comma_consumed;
}

/* Parse nested array dimensions recursively; the row parser owns scalar
 * elements and string rows, while each outer level handles its designators and
 * zero-filled gaps.
 */
static bool parse_nested_array_values(block_t *parent,
                                      basic_block_t **bb,
                                      const var_t *array,
                                      var_t *target_addr,
                                      int start,
                                      bool braced)
{
    var_t child;
    int count = 0;
    int filled = 0;
    int stride;
    int elem_size = array_element_size(array);

    if (!array->array_dim3)
        return parse_array_field_row_values(parent, bb, array, target_addr,
                                            start, braced);
    memcpy(&child, array, sizeof(child));
    fixed_array_var_drop_outer(&child);
    stride = fixed_array_inner_count(&child);
    if (braced) {
        lex_expect(T_open_curly);
        reject_empty_initializer_in_strict_c99();
    }
    while (!lex_peek(T_close_curly, NULL)) {
        bool child_comma;

        accept_slot_designator(initializer_name_scope(parent),
                               array->array_dim2, &count);
        if (count >= array->array_dim2)
            error_at(array->array_dim4 ? "Too many planes in array initializer"
                                       : "Too many rows in array initializer",
                     next_token_loc());
        emit_zero_elements(parent, bb, target_addr, array,
                           start + filled * stride, start + count * stride,
                           elem_size);
        child_comma = parse_nested_array_values(parent, bb, &child, target_addr,
                                                start + count * stride,
                                                lex_peek(T_open_curly, NULL));
        count++;
        if (count > filled)
            filled = count;
        if (array->array_dim4) {
            if (!lex_accept(T_comma))
                break;
            continue;
        } else if (count == array->array_dim2) {
            if (braced && initializer_separator(child_comma) &&
                !lex_peek(T_close_curly, NULL)) {
                if (lex_peek(T_open_square, NULL))
                    continue;
                error_at("Too many rows in array initializer",
                         next_token_loc());
            }
            break;
        } else if (!initializer_separator(child_comma)) {
            break;
        }
    }
    if (braced)
        lex_expect(T_close_curly);
    emit_zero_elements(parent, bb, target_addr, array, start + filled * stride,
                       start + array->array_dim2 * stride, elem_size);
    return false;
}

static int parse_nested_array_initializer(block_t *parent,
                                          basic_block_t **bb,
                                          const var_t *array,
                                          var_t *target_addr,
                                          int start)
{
    if (!array->array_dim2 || !lex_peek(T_open_curly, NULL))
        return 0;
    parse_nested_array_values(parent, bb, array, target_addr, start, true);
    return fixed_array_inner_count(array);
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

static void emit_string_array_units(block_t *parent,
                                    basic_block_t **bb,
                                    const var_t *array,
                                    var_t *base,
                                    int count,
                                    const void *units,
                                    int initialized,
                                    bool wide)
{
    int size = array->type->size;

    for (int i = 0; i < count; i++) {
        var_t *value =
            wide ? require_typed_var(parent, array->type) : require_var(parent);
        var_t *address;

        value->var_name = gen_name();
        value->init_val = i >= initialized ? 0
                          : wide           ? ((const int *) units)[i]
                                           : ((const unsigned char *) units)[i];
        value->is_const = true;
        add_insn(parent, *bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        address = compute_element_address(parent, bb, base, i, size);
        add_insn(parent, *bb, OP_write, NULL, address, value, size, NULL);
    }
}

static void initialize_string_array(const var_t *array,
                                    block_t *parent,
                                    basic_block_t **bb,
                                    var_t *base,
                                    const void *units,
                                    int length,
                                    bool wide,
                                    var_t *infer_bound)
{
    /* Both decoders omit the terminator. Narrow arrays initialize it when it
     * fits; wide arrays leave its zero-filled unit to the array storage.
     */
    int inferred_size = length + 1;
    int initialized = wide ? length : inferred_size;
    int array_size = array->array_size;

    if (infer_bound && infer_bound->has_unsized_array) {
        array_size = inferred_size;
        infer_bound->array_size = array_size;
        infer_bound->has_unsized_array = false;
    } else if (length > array_size)
        error_at(wide ? "Wide string initializer is too long for array"
                      : "String initializer is too long for character array",
                 cur_token_loc());

    if (infer_bound && !wide && initialized > array_size)
        initialized = array_size;
    int count = infer_bound && !wide && parent == GLOBAL_BLOCK ? initialized
                                                               : array_size;
    emit_string_array_units(parent, bb, array, base, count, units, initialized,
                            wide);
}

/* Decode and lower a string initializer; target_addr may be a field address,
 * while infer_bound is set only for a declared array with unknown size.
 */
static void parse_string_array_init_to(const var_t *array,
                                       block_t *parent,
                                       basic_block_t **bb,
                                       var_t *target_addr,
                                       var_t *infer_bound,
                                       bool wide)
{
    union {
        char narrow[MAX_STRING_LEN];
        int wide[MAX_STRING_LEN];
    } units;
    int length = wide ? read_wstring_units(units.wide, MAX_STRING_LEN)
                      : read_concatenated_string(units.narrow);

    initialize_string_array(
        array, parent, bb, target_addr,
        wide ? (const void *) units.wide : (const void *) units.narrow, length,
        wide, infer_bound);
}

/* A member has no independent var_t storage, so use its computed address. */
void parse_string_field_init(block_t *parent,
                             basic_block_t **bb,
                             const var_t *field,
                             var_t *target_addr)
{
    parse_string_array_init_to(field, parent, bb, target_addr, NULL, false);
}

bool is_char_array(const var_t *var)
{
    return var && !var->ptr_level && !var->array_dim2 && !var->array_dim3 &&
           !var->array_dim4 &&
           compatible_decl_type(var->type, find_builtin_type("char"));
}

bool is_wchar_array(const var_t *var)
{
    return var && !var->ptr_level && !var->array_dim2 && !var->array_dim3 &&
           !var->array_dim4 &&
           compatible_decl_type(var->type, find_builtin_type("wchar_t"));
}

static void parse_braced_string_array_init(var_t *var,
                                           block_t *parent,
                                           basic_block_t **bb)
{
    if (lex_peek(T_wstring, NULL))
        parse_wstring_array_init(var, parent, bb);
    else
        parse_string_array_init(var, parent, bb);
    lex_accept(T_comma);
    lex_expect(T_close_curly);
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
                              var_t *target_addr)
{
    parse_string_array_init_to(field, parent, bb, target_addr, NULL, true);
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
         !compatible_decl_type(array->type, find_builtin_type("char"))) ||
        (kind == T_wstring &&
         !compatible_decl_type(array->type, find_builtin_type("wchar_t"))))
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
                           int start)
{
    var_t row;
    int width = string_row_width(array);
    var_t *row_addr;

    if (start % width)
        error_at("String literal cannot initialize a single array element",
                 next_token_loc());
    memcpy(&row, array, sizeof(row));
    row.array_size = width;
    row.array_dim2 = row.array_dim3 = row.array_dim4 = 0;
    row_addr = compute_element_address(parent, bb, target_addr, start,
                                       array->type->size);
    if (lex_peek(T_wstring, NULL))
        parse_wstring_field_init(parent, bb, &row, row_addr);
    else
        parse_string_field_init(parent, bb, &row, row_addr);
}

void parse_array_field_init(block_t *parent,
                            basic_block_t **bb,
                            const var_t *field,
                            var_t *target_addr)
{
    var_t array;

    memcpy(&array, field, sizeof(array));
    if (parent == GLOBAL_BLOCK)
        array.scope = initializer_name_scope(parent);
    parse_array_init_at(&array, parent, bb, target_addr, true);
}

/* Initialize the record at @addr from @value, an initializer already read that
 * does not begin with a brace. An expression of the record's own type
 * initializes the whole record. Anything else, or no value at all, starts a
 * brace-elided list that takes one initializer per member from the enclosing
 * list (C99 6.7.8p13 and p20).
 *
 * @cleared says the record's automatic storage is already zero: the object that
 * holds it was cleared up front, and a designator may since have stored some of
 * its members, which the elided list must keep (C99 6.7.8p19).
 *
 * Returns true when that list consumed the comma after its last initializer,
 * which the enclosing loop must then not expect.
 */
bool parse_record_from_value(block_t *parent,
                             basic_block_t **bb,
                             type_t *record_type,
                             var_t *addr,
                             var_t *value,
                             bool cleared)
{
    record_type = resolve_record_type(record_type);
    if (is_record_object(value) &&
        resolve_record_type(value->type) == record_type) {
        emit_record_copy_to_address(parent, bb, addr, value);
        return false;
    }
    return parse_struct_field_values(parent, bb, record_type, addr, true,
                                     cleared, value);
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
                                bool cleared)
{
    var_t *value = NULL;

    if (parent != GLOBAL_BLOCK && !lex_peek(T_string, NULL) &&
        !lex_peek(T_wstring, NULL)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
        value = opstack_pop();
    }
    return parse_record_from_value(parent, bb, record_type, addr, value,
                                   cleared);
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

/* The address an initializer of @member in the record at @record_addr stores
 * through: @designated when a designator or a brace-elided array already
 * selected the member or one of its elements, and the member itself otherwise.
 */
var_t *member_address(block_t *parent,
                      basic_block_t **bb,
                      var_t *record_addr,
                      const var_t *member,
                      var_t *designated)
{
    if (designated)
        return designated;
    return compute_field_address(parent, bb, record_addr, member);
}

/* The index of the first member of @record from @index on that takes an
 * initializer. An unnamed bit-field is padding rather than a member: it takes
 * part in layout but consumes no positional initializer, so the second value of
 * `{low, high}` reaches the named field after it.
 */
int skip_unnamed_bitfields(const type_t *record, int index)
{
    while (index < record->num_fields && is_bitfield(&record->fields[index]) &&
           !record->fields[index].var_name[0])
        index++;
    return index;
}

void parse_struct_field_init(block_t *parent,
                             basic_block_t **bb,
                             type_t *struct_type,
                             var_t *target_addr)
{
    parse_struct_field_values(parent, bb, struct_type, target_addr, false,
                              false, NULL);
}

/* The constant bit-field slices of a static record, one entry per allocation
 * unit, keyed by the unit's byte offset in the record that owns the list.
 */
typedef struct {
    var_t *unit[MAX_FIELDS];
    var_t *addr[MAX_FIELDS];
    int offset[MAX_FIELDS];
    unsigned value[MAX_FIELDS];
    int count;
} static_bitfield_units_t;

/* Set just before a member designator's nested record list is parsed: that list
 * adds its slices to these units, at this byte offset, so a later designator of
 * the same unit keeps the bits an earlier one stored.
 */
static_bitfield_units_t *designated_bitfield_units;
int designated_bitfield_base;

/* Parse the members of the record at @target_addr. A braced list runs to its
 * closing brace, which the caller consumes. An @elided list has no braces of
 * its own: it stops once every member has an initializer, or at a closing brace
 * or designator that belongs to the enclosing list, and @first_value is an
 * initializer the caller already read for the first scalar member. A @cleared
 * record is not zeroed again; see parse_record_from_value().
 */
bool parse_struct_field_values(block_t *parent,
                               basic_block_t **bb,
                               type_t *struct_type,
                               var_t *target_addr,
                               bool elided,
                               bool cleared,
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
    static_bitfield_units_t own_units;
    static_bitfield_units_t *units = &own_units;
    int units_base = 0;

    own_units.count = 0;
    if (designated_bitfield_units) {
        units = designated_bitfield_units;
        units_base = designated_bitfield_base;
        designated_bitfield_units = NULL;
    }

    /* Zero the complete struct before processing fields. Positional
     * initializers could defer this until the first omitted member, but a
     * designator may skip forward or return to an earlier member. A union is
     * cleared whole: its member arrays take positional and designated elements
     * through the same pending sequence, which leaves the elements it does not
     * reach alone.
     */
    if (parent != GLOBAL_BLOCK && !cleared) {
        var_t *zero = emit_zero_constant(parent, bb);

        if (is_union)
            emit_zero_bytes(parent, bb, target_addr, struct_type->size,
                            alignment_type(struct_type), zero);
        for (int i = 0; !is_union && i < struct_type->num_fields; i++) {
            var_t *field = &struct_type->fields[i];

            emit_zero_bytes(
                parent, bb,
                compute_field_address(parent, bb, target_addr, field),
                size_var(field), initializer_alignment(field), zero);
        }
    }

    if (first_value || !lex_peek(T_close_curly, NULL)) {
        for (;;) {
            var_t *field_val_raw = NULL;
            var_t *field = NULL;
            var_t *field_addr = NULL;
            bool consumed_pending_array_element = false;
            bool nested_comma_consumed = false;
            bool nested_designator = false;
            bool designated = false;

            if (!first_value && lex_accept(T_dot)) {
                char field_name[MAX_ID_LEN];

                /* The row-major offset of the subobject an array designator
                 * selected, or -1 when the designator names a member only.
                 */
                int selected_index = -1;

                /* A new member designator ends any array element sequence
                 * started by an earlier `.a[i] =`, so the positional
                 * initializers after it follow the newly named member instead
                 * of resuming the stale array slots.
                 */
                has_pending_array_element = false;
                designated = true;

                lex_ident(T_identifier, field_name);
                for (int i = 0; i < struct_type->num_fields; i++) {
                    if (!strcmp(struct_type->fields[i].var_name, field_name)) {
                        field_idx = i;
                        field = &struct_type->fields[i];
                        break;
                    }
                }
                if (!field)
                    error_at("Unknown field in record initializer",
                             cur_token_loc());

                field_addr =
                    compute_field_address(parent, bb, target_addr, field);
                designated_bitfield_base = units_base + field->offset;
                if (lex_peek(T_open_square, NULL)) {
                    int linear_index = 0;
                    int total_element_count = field->array_size;
                    int elem_size = array_element_size(field);
                    var_t *array_base_addr = field_addr;

                    if (!field->array_size)
                        error_at("Array designator requires an array member",
                                 next_token_loc());

                    /* The subscripts leave the remaining inner array in
                     * `field`, so rows, planes, and a final scalar can all
                     * share the same bounds and continuation path.
                     */
                    read_array_designator(initializer_name_scope(parent), field,
                                          0, false, &linear_index,
                                          &array_element);
                    fixed_array_var_drop_outer(&array_element);
                    field = &array_element;
                    field_addr = compute_element_address(
                        parent, bb, field_addr, linear_index, elem_size);
                    designated_bitfield_base += linear_index * elem_size;
                    pending_array_base = array_base_addr;
                    pending_array_count = total_element_count;
                    pending_array_elem_size = elem_size;
                    selected_index = linear_index;
                    if (!field->array_size) {
                        memcpy(&pending_array_element, field, sizeof(var_t));
                        pending_array_index = linear_index + 1;
                        has_pending_array_element = true;
                    }
                }

                /* A further member designator, as in `.in.a[1] = 3` or
                 * `.arr[1].x = 1`, descends into the record just selected. That
                 * record's own list takes the rest of the designator and the
                 * positional values after it, as a brace-elided record does, so
                 * they fill its following members before this list resumes
                 * after it (C99 6.7.8p17 and p18).
                 */
                if (lex_peek(T_dot, NULL)) {
                    if (field->array_size || field->ptr_level ||
                        !is_record_type(field->type))
                        error_at("Nested designator requires a record member",
                                 next_token_loc());
                    designated_bitfield_units = units;
                    nested_comma_consumed = parse_struct_field_values(
                        parent, bb, resolve_record_type(field->type),
                        field_addr, true, true, NULL);
                    nested_designator = true;
                } else {
                    lex_expect(T_assign);
                }

                /* A designator that selects a row or plane, as in `.a[1] = 3`,
                 * names a subobject that is itself an array. Without braces its
                 * initializer is brace-elided exactly as for a positional array
                 * member: the value fills the first element of that subobject
                 * and the following ones continue in row-major order through
                 * the rest of the member.
                 */
                if (!nested_designator && selected_index >= 0 &&
                    field->array_size && !lex_peek(T_open_curly, NULL) &&
                    ((!field->ptr_level && is_record_type(field->type)) ||
                     has_effective_pointer(field) ||
                     (!lex_peek(T_string, NULL) &&
                      !lex_peek(T_wstring, NULL)))) {
                    memcpy(&pending_array_element, field, sizeof(var_t));
                    pending_array_element.array_size = 0;
                    pending_array_element.array_dim2 = 0;
                    pending_array_element.array_dim3 = 0;
                    pending_array_element.array_dim4 = 0;
                    field = &pending_array_element;
                    pending_array_index = selected_index;
                    has_pending_array_element = true;
                    consumed_pending_array_element = true;
                }
            } else if (has_pending_array_element) {
                field = &pending_array_element;
                field_addr = compute_element_address(
                    parent, bb, pending_array_base, pending_array_index,
                    pending_array_elem_size);
                consumed_pending_array_element = true;
            }

            if (!field && !has_pending_array_element)
                field_idx = skip_unnamed_bitfields(struct_type, field_idx);

            /* A union takes one positional initializer, but every designator
             * names a member again, the same one or another, and the last wins
             * (C99 6.7.8p19): `{ .p.x = 1, .p.y = 2 }` sets both.
             */
            if (field_idx >= struct_type->num_fields ||
                (is_union && initializer_count > 0 && !designated &&
                 !consumed_pending_array_element))
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
            if (field && !field_addr && !has_pending_array_element &&
                field->array_size &&
                (first_value ||
                 (!lex_peek(T_open_curly, NULL) &&
                  ((!field->ptr_level && is_record_type(field->type)) ||
                   has_effective_pointer(field) ||
                   (!lex_peek(T_string, NULL) &&
                    !lex_peek(T_wstring, NULL)))))) {
                pending_array_base =
                    compute_field_address(parent, bb, target_addr, field);
                pending_array_elem_size = array_element_size(field);
                pending_array_count = field->array_size;
                pending_array_index = 0;
                memcpy(&pending_array_element, field, sizeof(var_t));
                pending_array_element.array_size = 0;
                pending_array_element.array_dim2 = 0;
                pending_array_element.array_dim3 = 0;
                pending_array_element.array_dim4 = 0;
                field = &pending_array_element;
                field_addr = compute_element_address(
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
            if (nested_designator) {
                /* The selected record's list already took its initializers. */
            } else if (first_value) {
                /* The caller read this value for the first scalar of an elided
                 * record. A record member takes it on to that record's own
                 * first member; see parse_record_from_value().
                 */
                if (!field->ptr_level && is_record_type(field->type))
                    nested_comma_consumed = parse_record_from_value(
                        parent, bb, field->type,
                        member_address(parent, bb, target_addr, field,
                                       field_addr),
                        first_value, true);
                else
                    field_val_raw = first_value;
                first_value = NULL;
            } else if (field && field->array_size && is_char_array(field) &&
                       lex_peek(T_string, NULL)) {
                parse_string_field_init(
                    parent, bb, field,
                    member_address(parent, bb, target_addr, field, field_addr));
            } else if (field && field->array_size && !field->ptr_level &&
                       compatible_decl_type(field->type,
                                            find_builtin_type("wchar_t")) &&
                       lex_peek(T_wstring, NULL)) {
                parse_wstring_field_init(
                    parent, bb, field,
                    member_address(parent, bb, target_addr, field, field_addr));
            } else if (field && lex_peek(T_open_curly, NULL) &&
                       field->array_size) {
                parse_array_field_init(
                    parent, bb, field,
                    member_address(parent, bb, target_addr, field, field_addr));
            } else if (field && lex_peek(T_open_curly, NULL) &&
                       is_record_type(field->type)) {
                type_t *nested_type = resolve_record_type(field->type);
                lex_expect(T_open_curly);
                parse_struct_field_init(
                    parent, bb, nested_type,
                    member_address(parent, bb, target_addr, field, field_addr));
                lex_expect(T_close_curly);
            } else if (field && !field->array_size &&
                       unbraced_record_starts_here(field)) {
                nested_comma_consumed = parse_unbraced_record_init(
                    parent, bb, field->type,
                    member_address(parent, bb, target_addr, field, field_addr),
                    true);
            } else {
                field_val_raw =
                    read_array_scalar_initializer(parent, bb, true, NULL);
            }

            if (field_val_raw) {
                field_addr =
                    member_address(parent, bb, target_addr, field, field_addr);
                diagnose_function_pointer_conversion(field_val_raw, field);
                if (field_val_raw->pointee_func_signature)
                    diagnose_callback_slot_initializer(field_val_raw, field);
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
                        for (int i = 0; i < units->count; i++)
                            if (units->offset[i] ==
                                    units_base + field->offset &&
                                units->unit[i]->bit_storage_size ==
                                    field->bit_storage_size) {
                                unit = i;
                                break;
                            }
                        if (unit < 0) {
                            if (units->count >= MAX_FIELDS)
                                error_at("Too many bit-fields in initializer",
                                         cur_token_loc());
                            unit = units->count++;
                            units->unit[unit] = field;
                            units->addr[unit] = field_addr;
                            units->offset[unit] = units_base + field->offset;
                            units->value[unit] = 0;
                        }
                        unsigned mask = bitfield_mask(field)
                                        << field->bit_offset;
                        unsigned init_val =
                            is_bool_type(field->type)
                                ? field_val_raw->init_val != 0
                                : (unsigned) field_val_raw->init_val;
                        units->value[unit] = (units->value[unit] & ~mask) |
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
                field_idx = skip_unnamed_bitfields(struct_type, field_idx);
                if (field_idx >= struct_type->num_fields || is_union) {
                    comma_consumed = nested_comma_consumed;
                    break;
                }
            }
            if (!initializer_separator(nested_comma_consumed))
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

    /* A nested designator's list leaves its units to the list it came from. */
    if (parent == GLOBAL_BLOCK && units == &own_units) {
        for (int i = 0; i < own_units.count; i++) {
            var_t *value = bitfield_constant(parent, bb, own_units.value[i]);
            add_insn(parent, *bb, OP_write, NULL, own_units.addr[i], value,
                     own_units.unit[i]->bit_storage_size, NULL);
        }
    }
    return comma_consumed;
}

void parse_array_literal_expr(block_t *parent, basic_block_t **bb)
{
    var_t *array_var = require_named_var(parent);
    array_var->is_compound_literal = true;

    int element_count = 0;
    var_t *first_element = NULL;

    if (!lex_peek(T_close_curly, NULL)) {
        first_element = read_array_scalar_initializer(parent, bb, false, NULL);
        element_count = 1;

        while (lex_accept(T_comma)) {
            if (lex_peek(T_close_curly, NULL))
                break;

            read_array_scalar_initializer(parent, bb, false, NULL);
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

    read_comma_expression(parent, &bb, false);
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
        var_t *val = name_var(require_typed_var(parent, rs1->type));
        val->init_val = rs1->init_val;
        add_insn(parent, bb, OP_load_constant, val, NULL, NULL, 0, NULL);
        rs1 = val;
    }

    /* Every ordinary use of a function designator converts it to a pointer.
     * This includes scalar conversions such as `_Bool f(void) { return cb; }`,
     * not only callback-pointer returns.
     */
    rs1 = materialize_function_designator(parent, &bb, rs1);
    diagnose_function_pointer_conversion(rs1, &parent->func->return_def);
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
    diagnose_integer_to_pointer_conversion(rs1, &parent->func->return_def,
                                           false);
    if (!parent->func->return_def.type->func_signature)
        rs1 = resize_to(parent, &bb, rs1, parent->func->return_def.type,
                        parent->func->return_def.ptr_level);

    add_insn(parent, bb, OP_return, NULL, rs1, NULL, 0, NULL);
    bb_connect(bb, parent->func->exit, NEXT);
    return NULL;
}

static void emit_control_branch(block_t *parent, basic_block_t *bb)
{
    var_t *condition = opstack_pop();

    reject_record_operand(condition);
    add_insn(parent, bb, OP_branch, NULL, condition, NULL, 0, NULL);
}

static void read_control_branch(block_t *parent, basic_block_t **bb)
{
    lex_expect(T_open_bracket);
    read_control_expression(parent, bb);
    lex_expect(T_close_bracket);
    emit_control_branch(parent, *bb);
}

basic_block_t *handle_if_statement(block_t *parent, basic_block_t *bb)
{
    basic_block_t *n = bb_create(parent);
    bb_connect(bb, n, NEXT);
    bb = n;

    read_control_branch(parent, &bb);

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
    read_control_branch(parent, &bb);

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

basic_block_t *handle_goto_statement(basic_block_t *bb)
{
    char token[MAX_ID_LEN];
    if (!lex_peek(T_identifier, token))
        error_at("Expected identifier after 'goto'", next_token_loc());
    lex_expect(T_identifier);
    lex_expect(T_semicolon);

    label_t *label = find_label(token);
    if (label) {
        label->used = true;
        bb_connect(bb, label->bb, NEXT);
    } else {
        if (backpatch_bb_idx >= MAX_LABELS)
            error_at("Too many forward-referenced labels", cur_token_loc());
        backpatch_label[backpatch_bb_idx] = intern_string(token);
        backpatch_bb[backpatch_bb_idx++] = bb;
    }
    return NULL;
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

/* Reject a string literal that is the whole initializer of one element of the
 * array @var, as in `char s[3] = {"ab", "c"}` or `int s[3] = {"ab"}`. A string
 * literal initializes a character array only as that array's whole initializer
 * (C99 6.7.8p14), and an element that is not a pointer cannot take its address
 * either. A literal that only begins a larger expression, such as `"ab"[0]`, is
 * an ordinary scalar.
 */
static void reject_string_element_initializer(const var_t *var)
{
    token_t *token = cur_token->next;
    token_kind_t kind = token ? token->kind : T_eof;

    if (var->ptr_level || var->is_func || has_effective_pointer(var) ||
        (kind != T_string && kind != T_wstring))
        return;
    while (token->next && token->next->kind == kind)
        token = token->next;
    if (!token->next ||
        (token->next->kind != T_comma && token->next->kind != T_close_curly))
        return;
    if ((kind == T_string &&
         compatible_decl_type(var->type, find_builtin_type("char"))) ||
        (kind == T_wstring &&
         compatible_decl_type(var->type, find_builtin_type("wchar_t"))))
        error_at("String literal cannot initialize a single array element",
                 next_token_loc());
    error_at("String literal initializer has incompatible array element type",
             next_token_loc());
}

void parse_array_init(var_t *var, block_t *parent, basic_block_t **bb)
{
    parse_array_init_at(var, parent, bb, var, false);
}

static void parse_array_init_at(var_t *var,
                                block_t *parent,
                                basic_block_t **bb,
                                var_t *base_addr,
                                bool sparse_zero)
{
    int count = 0;
    int inferred_size = 0;
    int initialized_end = 0;

    /* An omitted outer bound of a multidimensional declaration already has an
     * inner-dimension product in `array_size`. It is nevertheless inferred from
     * the initializer, just like a one-dimensional `int a[]`.
     */
    bool is_implicit = (var->array_size == 0 || var->has_unsized_array);
    bool runtime_initializer =
        parent != GLOBAL_BLOCK || var->is_compound_literal;
    block_t *initializer_scope = parent;

    if (parent == GLOBAL_BLOCK && var->scope && var->scope != GLOBAL_BLOCK)
        initializer_scope = var->scope;
    block_t *saved_initializer_scope = global_constant_initializer_scope;
    if (!runtime_initializer)
        global_constant_initializer_scope = initializer_scope;

    /* Elements of a pointer array are pointer-sized. Using the base type's
     * width strided "char *a[2] = {...}" by one byte, so every element but the
     * first got a bogus address. `ptr_level` describes the array element type
     * even when its outer bound is inferred.
     */
    int elem_size = array_element_size(var);

    /* A character array's string literal may be enclosed in braces (C99
     * 6.7.8p14): `char s[3] = {"ab"}` holds the characters, not the literal's
     * address converted to a char.
     */
    if (lex_peek(T_open_curly, NULL) &&
        string_ends_braced_initializer(var, cur_token->next->next)) {
        lex_expect(T_open_curly);
        parse_braced_string_array_init(var, parent, bb);
        global_constant_initializer_scope = saved_initializer_scope;
        return;
    }

    /* Reordered designators can leave holes before or after written elements.
     * Ordinary automatic arrays are cleared up front; field arrays clear only
     * skipped ranges.
     */
    if (runtime_initializer && !is_implicit && !sparse_zero)
        emit_zero_elements(parent, bb, base_addr, var, 0, var->array_size,
                           elem_size);

    lex_expect(T_open_curly);
    reject_empty_initializer_in_strict_c99();
    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            var_t *val = NULL;
            var_t designated_array;
            var_t *initializer_var = var;
            bool elided_comma = false;

            /* Whether a member designator follows the subscripts, as in `[1].k
             * = 3`, naming a member of one record element.
             */
            bool member_designator = false;

            /* A designator such as `[1][0]` moves to the row-major offset of
             * the subobject it names, and the braced row, plane and string
             * paths below then see the array that subobject is an element of,
             * rather than the whole declaration.
             */
            if (read_array_designator(initializer_scope, var, 0, is_implicit,
                                      &count, &designated_array)) {
                initializer_var = &designated_array;
                member_designator = lex_peek(T_dot, NULL) &&
                                    !designated_array.array_dim2 &&
                                    !var->ptr_level && !var->is_func &&
                                    is_record_type(var->type);
                if (!member_designator)
                    lex_expect(T_assign);
            }

            if (!is_implicit && count >= var->array_size)
                error_at("Too many elements in array initializer",
                         next_token_loc());

            /* Zero any skipped range from the highest initialized slot. */
            if (runtime_initializer && (is_implicit || sparse_zero))
                emit_zero_elements(
                    parent, bb, base_addr, var,
                    is_implicit ? inferred_size : initialized_end, count,
                    elem_size);

            int nested = parse_nested_array_initializer(
                parent, bb, initializer_var, base_addr, count);
            if (nested) {
                count += nested;
                if (count > initialized_end)
                    initialized_end = count;
                if (is_implicit && count > inferred_size)
                    inferred_size = count;
                if (!lex_accept(T_comma))
                    break;
                continue;
            } else if (string_row_starts_here(initializer_var)) {
                parse_string_row_init(parent, bb, initializer_var, base_addr,
                                      count);
                count += string_row_width(initializer_var);
                if (count > initialized_end)
                    initialized_end = count;
                if (is_implicit && count > inferred_size)
                    inferred_size = count;
                if (!lex_accept(T_comma) || lex_peek(T_close_curly, NULL))
                    break;
                continue;
            } else if (member_designator) {
                /* The record element's own list starts at that designator and
                 * takes the positional values after it, as a brace-elided
                 * record does, up to the next designator of this array. The
                 * element keeps what an earlier `[1].fn = f` stored, so only an
                 * inferred-bound element not yet reached is cleared first.
                 */
                var_t *elem_addr = compute_element_address(
                    parent, bb, base_addr, count, elem_size);

                if (runtime_initializer && (is_implicit || sparse_zero) &&
                    count >= (is_implicit ? inferred_size : initialized_end))
                    emit_zero_elements(parent, bb, base_addr, var, count,
                                       count + 1, elem_size);
                elided_comma = parse_struct_field_values(
                    parent, bb, resolve_record_type(var->type), elem_addr, true,
                    true, NULL);
            } else if (is_record_type(var->type) &&
                       (lex_peek(T_open_curly, NULL) ||
                        unbraced_record_starts_here(var))) {
                /* The shared element parser handles braced records and
                 * brace-elided member lists. It leaves a consumed comma for
                 * this loop and preserves members written by earlier
                 * designators.
                 */
                var_t *elem_addr = compute_element_address(
                    parent, bb, base_addr, count, elem_size);
                elided_comma = parse_array_element_init(
                    parent, bb, var, elem_addr, elem_size,
                    (!is_implicit && !sparse_zero) || count < initialized_end,
                    NULL);
            } else {
                reject_string_element_initializer(var);
                val = read_array_scalar_initializer(
                    parent, bb, !runtime_initializer, initializer_scope);
            }

            if (is_implicit && count >= MAX_IMPLICIT_ARRAY)
                error_at("Too many elements in array initializer",
                         next_token_loc());

            /* An element of an array of callbacks is a callback object. */
            if (val && !var->type->array_element_pointee_func_signature &&
                !var->pointee_func_signature &&
                (var->func_signature ||
                 (var->type->func_signature &&
                  !var->type->is_direct_function_type && !var->ptr_level))) {
                var_t element = {0};

                element.type = var->type;
                element.ptr_level = var->func_signature ? var->ptr_level : 0;
                element.is_func = true;
                element.func_signature = var->func_signature
                                             ? var->func_signature
                                             : var->type->func_signature;
                diagnose_function_pointer_conversion(val, &element);
            } else if (val &&
                       (val->is_func || val->func_signature ||
                        (val->type && val->type->func_signature)) &&
                       effective_pointer_depth(var) && !var->is_func &&
                       !var->func_signature && !var->pointee_func_signature &&
                       !var->type->func_signature &&
                       !var->type->array_element_pointee_func_signature) {
                /* An element of an array of object pointers takes no function
                 * pointer, not even one an explicit cast produced.
                 */
                var_t element = {0};

                element.type = var->type;
                element.ptr_level = var->ptr_level;
                diagnose_function_pointer_conversion(val, &element);
            }

            /* A pointer to function pointers keeps its prototype through a
             * cast, as `(int (**)(void)) 4` does, and an element compares it.
             */
            func_t *element_slot =
                var->type->array_element_pointee_func_signature;

            if (val && (element_slot || (val->pointee_func_signature &&
                                         var->pointee_func_signature))) {
                /* The array descriptor is not an element descriptor. Build the
                 * slot type that this scalar initializer is about to store so
                 * callback prototype validation remains elementwise.
                 */
                var_t element_target = {0};

                element_target.type = var->type;
                element_target.ptr_level =
                    element_slot ? var->type->array_element_ptr_level
                                 : var->ptr_level;
                element_target.pointee_func_signature =
                    element_slot ? element_slot : var->pointee_func_signature;
                if (incompatible_pointee_callback_conversion(val,
                                                             &element_target))
                    error_at(
                        "incompatible callback slot types in array "
                        "initializer",
                        cur_token_loc());
            }

            if (val && (is_implicit || count < var->array_size)) {
                /* Keep a function symbol intact until OP_address_of_func can
                 * emit its deferred relocation. Treating an array-of-callback
                 * element as an `int` conversion loses that provenance.
                 */
                var_t *v;

                if (val->is_func || var->ptr_level > 0)
                    v = val;
                else
                    v = resize_to(parent, bb, val, var->type, 0);

                bool already_zeroed =
                    parent != GLOBAL_BLOCK && !is_implicit && !sparse_zero &&
                    count >= initialized_end && !var->is_volatile &&
                    !var->type->is_volatile_qualified &&
                    !var->type->array_element_is_volatile && !var->ptr_level &&
                    !var->is_func && !is_record_type(var->type) &&
                    v->is_const && !v->init_val && !v->init_val_hi;

                /* A direct long long element is wider than a pointer on a
                 * 32-bit target, where it is written as a register pair.
                 */
                if (!already_zeroed) {
                    var_t *elem_addr = compute_element_address(
                        parent, bb, base_addr, count, elem_size);

                    if (elem_size <= PTR_SIZE ||
                        (elem_size == 8 && !var->ptr_level && !var->is_func &&
                         !is_record_type(var->type))) {
                        add_insn(parent, *bb, OP_write, NULL, elem_addr, v,
                                 elem_size, NULL);
                    } else {
                        fatal(
                            "Unsupported: array element wider than a pointer");
                    }
                }
            }

            if (var->is_compound_literal && count == 0 && val)
                var->init_val = val->init_val;

            count++;
            if (count > initialized_end)
                initialized_end = count;
            if (is_implicit && count > inferred_size)
                inferred_size = count;
            if (!initializer_separator(elided_comma) ||
                lex_peek(T_close_curly, NULL))
                break;
        }
    }

    lex_expect(T_close_curly);

    if (runtime_initializer && sparse_zero && !is_implicit)
        emit_zero_elements(parent, bb, base_addr, var, initialized_end,
                           var->array_size, elem_size);

    if (is_implicit) {
        /* `count` walks scalars, so a brace-elided list that stops inside a
         * row, as in `int m[][2] = { 1, 2, 3 }`, leaves inferred_size short of
         * a whole outer element. The bound counts outer elements, so round up
         * and zero the rest of that last element in automatic storage.
         */
        int inner = fixed_array_inner_count(var);
        int whole = (inferred_size + inner - 1) / inner * inner;

        if (runtime_initializer)
            emit_zero_elements(parent, bb, base_addr, var, inferred_size, whole,
                               elem_size);
        inferred_size = whole;
        var->array_size = inferred_size;
        var->has_unsized_array = false;
    }
    global_constant_initializer_scope = saved_initializer_scope;
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
    var_t *scalar = name_var(require_typed_ptr_var(parent, result_type, 0));
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
