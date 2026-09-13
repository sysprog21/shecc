/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Direct and indirect calls, the offsetof and va_arg builtins, bit-field
 * values, and the address-of and dereference operators.
 *
 * A fragment of the parser: parser.c includes it in order, so it sees every
 * definition that precedes it there and cannot be compiled on its own.
 */

void read_logical(opcode_t op, block_t *parent, basic_block_t **bb);
var_t *materialize_function_designator(block_t *parent,
                                       basic_block_t **bb,
                                       var_t *value);
void read_func_parameters_with_sret(func_t *func,
                                    var_t *sret,
                                    block_t *parent,
                                    basic_block_t **bb)
{
    int param_num = 0;
    var_t *params[MAX_PARAMS], *param;

    lex_expect(T_open_bracket);
    while (!lex_accept(T_close_bracket)) {
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }

        param = opstack_pop();
        param = materialize_function_designator(parent, bb, param);

        /* Writing past 'params' corrupts this frame, and the damage only
         * surfaces later as a wrong argument value. The check has to come
         * before the conversions below: those index func->param_defs[], a
         * MAX_PARAMS-element array embedded in func_t, so an over-long argument
         * list reads past it and dereferences a garbage type pointer -- the
         * compiler crashed instead of reporting the limit.
         */
        if (param_num >= MAX_PARAMS)
            error_at("Too many arguments in function call", cur_token_loc());

        if (func && param_num < func->num_params) {
            var_t *target = &func->param_defs[param_num];
            if (incompatible_character_pointer_conversion(param, target))
                error_at(
                    "incompatible character pointer types for function "
                    "argument",
                    cur_token_loc());
            if (incompatible_pointee_callback_conversion(param, target))
                error_at(
                    "incompatible callback slot types for function "
                    "argument",
                    cur_token_loc());
            diagnose_const_pointer_conversion(param, target);
            if (is_record_type(target->type) &&
                !has_effective_pointer(target)) {
                /* A record parameter is a by-value object. Keep that promise
                 * without teaching every backend its native aggregate ABI: give
                 * the callee an addressable caller-side copy in one
                 * pointer-sized ABI slot.
                 */
                if (!is_record_object(param))
                    error_at("Record argument required", cur_token_loc());

                var_t *copy = require_typed_var(parent, target->type);
                copy->var_name = gen_name();
                add_insn(parent, *bb, OP_allocat, copy, NULL, NULL, 0, NULL);
                emit_record_copy(parent, bb, copy, param);

                param = require_ref_var(parent, target->type, 0);
                param->var_name = gen_name();
                add_insn(parent, *bb, OP_address_of, param, copy, NULL, 0,
                         NULL);
            } else if (!has_effective_pointer(target) && !target->array_size) {
                param =
                    scalarize_array_literal(parent, bb, param, target->type);
            }
        }

        /* Handle parameter type conversion whenever the callee has a known
         * prototype. Function-pointer declarations retain one too.
         */
        if (func && param_num >= func->num_params && func->va_args) {
            /* Default promotions apply to scalar varargs, but pointer-like
             * values (including array literals) must flow through unchanged so
             * "%p" and friends see an address rather than a scalarized value.
             */
            if (is_record_type(param->type) && !has_effective_pointer(param)) {
                if (!is_record_object(param))
                    error_at("Record argument required", cur_token_loc());
                var_t *copy = require_typed_var(parent, param->type);

                copy->var_name = gen_name();
                add_insn(parent, *bb, OP_allocat, copy, NULL, NULL, 0, NULL);
                emit_record_copy(parent, bb, copy, param);
                param = require_ref_var(parent, param->type, 0);
                param->var_name = gen_name();
                add_insn(parent, *bb, OP_address_of, param, copy, NULL, 0,
                         NULL);
            } else if (!is_pointer_like_value(param))
                param = promote(parent, bb, param, TY_int, 0);
        } else if (func && param_num < func->num_params) {
            /* Only a declared parameter has a type to convert towards. Beyond
             * num_params the param_defs[] entry was never filled in, so its
             * type is NULL and resize_var() dereferenced it: passing more
             * arguments than a non-variadic function declares crashed the
             * compiler instead of compiling or diagnosing the call.
             */
            if (!is_record_type(func->param_defs[param_num].type) ||
                func->param_defs[param_num].ptr_level)
                param =
                    resize_var(parent, bb, param, &func->param_defs[param_num]);
        }

        params[param_num++] = param;
        if (lex_accept(T_comma) && strict_c99 &&
            lex_peek(T_close_bracket, NULL))
            error_at("trailing comma in function call is not permitted in C99",
                     cur_token_loc());
    }

    if (func && func->has_prototype &&
        (param_num < func->num_params ||
         (!func->va_args && param_num > func->num_params)))
        error_at(param_num < func->num_params
                     ? "Too few arguments in function call"
                     : "Too many arguments in function call",
                 cur_token_loc());

    if (sret)
        add_insn(parent, *bb, OP_push, NULL, sret, NULL, param_num + 1, NULL);
    for (int i = 0; i < param_num; i++) {
        /* The operand should keep alive before calling function. Pass the
         * number of remained parameters to allocator to extend their liveness.
         */
        add_insn(parent, *bb, OP_push, NULL, params[i], NULL, param_num - i,
                 NULL);
    }
}

void read_func_call_with_sret(func_t *func,
                              var_t *sret,
                              block_t *parent,
                              basic_block_t **bb)
{
    /* direct function call */
    read_func_parameters_with_sret(func, sret, parent, bb);

    add_insn(parent, *bb, OP_call, NULL, NULL, NULL, 0,
             func->return_def.var_name);
}

void read_func_call(func_t *func, block_t *parent, basic_block_t **bb)
{
    read_func_call_with_sret(func, NULL, parent, bb);
}

/* A call returning `row *`, where `typedef int row[2]`, carries the row shape
 * on the return declarator rather than the scalar element type. Preserve that
 * shape on OP_func_ret and consume immediate postfix subscripts in one shared
 * path for direct, indirect, and grouped calls.
 */
void copy_call_result_array_shape(var_t *result, const var_t *return_def)
{
    fixed_array_shape_t shape;

    if (!result || !return_def)
        return;
    shape = return_def->pointee_array_size
                ? fixed_array_shape_from_pointee_var(return_def)
                : fixed_array_shape_from_type(return_def->type);
    fixed_array_shape_to_pointee_var(result, &shape);
    result->pointee_array_element_ptr_level =
        return_def->pointee_array_element_ptr_level
            ? return_def->pointee_array_element_ptr_level
            : return_def->type->array_element_ptr_level;
    result->is_const_qualified =
        return_def->is_const_qualified || return_def->type->is_const_qualified;
}

void lower_call_result_array_postfix(var_t **value,
                                     block_t *parent,
                                     basic_block_t **bb)
{
    var_t *base;
    var_t *address;
    fixed_array_shape_t shape;
    int dimensions;
    int depth = 0;

    if (!value || !(base = *value) || !base->pointee_array_size ||
        !lex_peek(T_open_square, NULL))
        return;

    shape = fixed_array_shape_from_pointee_var(base);

    /* One subscript selects the pointed-to array; its outer bound and every
     * inner bound then consume their own postfix subscript.
     */
    dimensions = shape.rank + 1;
    opstack_pop();
    address = base;
    do {
        var_t *index;
        int stride =
            base->pointee_array_element_ptr_level ? PTR_SIZE : base->type->size;

        if (depth >= dimensions)
            error_at("Too many subscripts for function result",
                     cur_token_loc());
        if (depth == 0) {
            stride *= base->pointee_array_size;
        } else
            stride = fixed_array_shape_stride(&shape, depth - 1, stride);

        lex_expect(T_open_square);
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        index = opstack_pop();
        lex_expect(T_close_square);
        if (stride != 1) {
            var_t *scale = require_var(parent);
            var_t *scaled = require_var(parent);

            scale->var_name = gen_name();
            scale->init_val = stride;
            add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL, 0, NULL);
            scaled->var_name = gen_name();
            add_insn(parent, *bb, OP_mul, scaled, index, scale, 0, NULL);
            index = scaled;
        }
        var_t *indexed = require_typed_ptr_var(parent, base->type, 1);
        indexed->var_name = gen_name();
        add_insn(parent, *bb, OP_add, indexed, address, index, 0, NULL);
        address = indexed;
        depth++;
    } while (lex_peek(T_open_square, NULL));

    if (depth == dimensions) {
        var_t *element = require_typed_var(parent, base->type);
        opcode_t compound_op = OP_generic;
        bool assignment;

        element->ptr_level = base->pointee_array_element_ptr_level;
        element->func_signature = base->type->func_signature;
        element->is_const_qualified =
            base->is_const_qualified || base->type->is_const_qualified;
        element->var_name = gen_name();
        add_insn(parent, *bb, OP_read, element, address, NULL,
                 element->ptr_level ? PTR_SIZE : base->type->size, NULL);
        element->is_compound_literal_reference = true;
        element->compound_literal_address = address;

        assignment =
            lex_accept(T_assign) || accept_compound_assign_op(&compound_op);
        if (assignment) {
            var_t *assigned;

            if (element->is_const_qualified)
                error_at("assignment of read-only location", cur_token_loc());
            if (!read_assignment_expression(parent, bb)) {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
            }
            assigned = opstack_pop();
            if (compound_op != OP_generic) {
                if (is_pointer_operation(compound_op, element, assigned)) {
                    handle_pointer_arithmetic(parent, bb, compound_op, element,
                                              assigned);
                    assigned = opstack_pop();
                } else {
                    var_t *current =
                        integer_promote_operand(parent, bb, element);

                    assigned = integer_promote_operand(parent, bb, assigned);
                    normalize_integer_binary_operands(parent, bb, compound_op,
                                                      &current, &assigned);
                    var_t *combined = require_var(parent);

                    combined->var_name = gen_name();
                    combined->type = integer_binary_result_type(
                        compound_op, current, assigned);
                    add_insn(parent, *bb, compound_op, combined, current,
                             assigned, 0, NULL);
                    assigned = combined;
                }
            }
            assigned = resize_var(parent, bb, assigned, element);
            add_insn(parent, *bb, OP_write, NULL, address, assigned,
                     element->ptr_level ? PTR_SIZE : base->type->size, NULL);
            *value = assigned;
            opstack_push(*value);
            return;
        }

        if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
            opcode_t op = lex_accept(T_increment) ? OP_add : OP_sub;
            var_t *one;
            var_t *updated;

            if (element->is_const_qualified)
                error_at("assignment of read-only location", cur_token_loc());
            one = require_typed_var(parent, TY_int);
            one->var_name = gen_name();
            one->init_val = 1;
            add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
            if (is_pointer_operation(op, element, one)) {
                handle_pointer_arithmetic(parent, bb, op, element, one);
                updated = opstack_pop();
            } else {
                updated = require_var(parent);
                updated->var_name = gen_name();
                updated->type = integer_binary_result_type(op, element, one);
                add_insn(parent, *bb, op, updated, element, one, 0, NULL);
            }
            updated = resize_var(parent, bb, updated, element);
            add_insn(parent, *bb, OP_write, NULL, address, updated,
                     element->ptr_level ? PTR_SIZE : base->type->size, NULL);
        }
        *value = element;
    } else {
        *value = address;
    }
    opstack_push(*value);
    var_t *callable = *value;
    if (callable->func_signature && lex_peek(T_open_bracket, NULL)) {
        func_t *signature = callable->func_signature;

        var_t *result =
            emit_indirect_call_result(callable, signature, true, parent, bb);
        *value = result;
        lower_call_result_array_postfix(value, parent, bb);
    }
}

void lower_call_result_prefix_update(var_t **value,
                                     opcode_t op,
                                     block_t *parent,
                                     basic_block_t **bb)
{
    var_t *object;
    var_t *one;
    var_t *updated;

    if (op == OP_generic)
        return;
    object = opstack_pop();
    if (!object->is_compound_literal_reference)
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
        updated = opstack_pop();
    } else {
        updated = require_var(parent);
        updated->var_name = gen_name();
        updated->type = integer_binary_result_type(op, object, one);
        add_insn(parent, *bb, op, updated, object, one, 0, NULL);
    }
    updated = resize_var(parent, bb, updated, object);
    add_insn(parent, *bb, OP_write, NULL, object->compound_literal_address,
             updated, object->ptr_level ? PTR_SIZE : object->type->size, NULL);
    *value = updated;
    opstack_push(updated);
}

/* The freestanding <stddef.h> maps offsetof(type, member-designator) here. Keep
 * it unevaluated: no null pointer or temporary object is materialized.
 */
void read_builtin_offsetof(block_t *parent, basic_block_t **bb)
{
    char name[MAX_ID_LEN];
    type_t *record;
    var_t *field;
    var_t *result;
    int offset = 0;
    bool has_nested_member;

    lex_expect(T_identifier);
    lex_expect(T_open_bracket);
    if (lex_accept(T_struct) || lex_accept(T_union)) {
        lex_ident(T_identifier, name);
        record = find_type(name, 2);
    } else {
        lex_ident(T_identifier, name);
        record = find_type(name, true);
    }
    if (!is_record_type(record))
        error_at("offsetof requires a struct or union type", cur_token_loc());
    lex_expect(T_comma);
    do {
        lex_ident(T_identifier, name);
        field = find_member(name, record);
        if (!field)
            error_at("Unknown record member", cur_token_loc());
        offset += field->offset;
        offset += read_offsetof_array_subscripts(field, parent);
        record = field->type;
        has_nested_member = lex_accept(T_dot);
        if (has_nested_member && (field->ptr_level || !is_record_type(record)))
            error_at("Nested offsetof member requires a record",
                     cur_token_loc());
    } while (has_nested_member);
    lex_expect(T_close_bracket);
    result = require_typed_var(parent, find_type("size_t", true));
    result->var_name = gen_name();
    result->init_val = offset;
    result->is_const = true;
    opstack_push(result);
    add_insn(parent, *bb, OP_load_constant, result, NULL, NULL, 0, NULL);
}

/* Parse a C99 type name for the va_arg intrinsic. A type name has no
 * identifier, but it may have an abstract declarator: `int (*)[4]` and `int
 * (*)(int)` are pointer types, whereas `int *[4]` and `int (int)` are
 * array/function types and are not objects that va_arg may fetch. The current
 * IR only needs the base type and pointer depth for a pointer value; consume
 * the remaining abstract-declarator syntax here so type names are not
 * accidentally restricted to the spelling of a declaration.
 */
typedef struct va_arg_type {
    type_t *type;
    int ptr_level;
    bool is_array;
    bool is_function;
    func_t *func_signature;
    int pointee_array_size;
    int pointee_array_dim2;
    int pointee_array_dim3;
    int pointee_array_dim4;
    int pointee_element_size;
    int pointee_element_ptr_level;
} va_arg_type_t;

void read_builtin_va_arg_type(block_t *parent, va_arg_type_t *result)
{
    char name[MAX_ID_LEN];
    type_t *type;
    bool is_signed = false;
    bool is_unsigned = false;
    int long_count = 0;
    bool is_short = false;
    bool is_char = false;
    bool saw_base = false;

    (void) parent;
    result->ptr_level = 0;
    result->is_array = false;
    result->is_function = false;
    result->func_signature = NULL;
    result->pointee_array_size = 0;
    result->pointee_array_dim2 = 0;
    result->pointee_array_dim3 = 0;
    result->pointee_array_dim4 = 0;
    result->pointee_element_size = 0;
    result->pointee_element_ptr_level = 0;
    while (true) {
        if (lex_accept(T_const) || lex_accept(T_volatile))
            continue;
        if (lex_accept(T_signed)) {
            if (is_signed)
                error_at("duplicate signed type specifier", cur_token_loc());
            is_signed = true;
            continue;
        }
        if (lex_accept(T_unsigned)) {
            if (is_unsigned)
                error_at("duplicate unsigned type specifier", cur_token_loc());
            is_unsigned = true;
            continue;
        }
        if (lex_accept(T_long)) {
            long_count++;
            continue;
        }
        if (lex_peek(T_identifier, name) && !strcmp(name, "short")) {
            if (is_short)
                error_at("duplicate short type specifier", cur_token_loc());
            lex_expect(T_identifier);
            is_short = true;
            continue;
        }
        if (lex_peek(T_identifier, name) &&
            (!strcmp(name, "int") || !strcmp(name, "char"))) {
            if (saw_base)
                error_at("duplicate type specifier", cur_token_loc());
            is_char = !strcmp(name, "char");
            saw_base = true;
            lex_expect(T_identifier);
            continue;
        }
        break;
    }
    if (is_signed && is_unsigned)
        error_at("both signed and unsigned specified", cur_token_loc());
    if (long_count > 2 || (is_short && long_count))
        error_at("invalid va_arg integer type", cur_token_loc());

    if (lex_accept(T_struct) || lex_accept(T_union)) {
        if (is_signed || is_unsigned || long_count || is_short)
            error_at("record type cannot have integer specifiers",
                     cur_token_loc());
        lex_ident(T_identifier, name);
        type = find_type(name, 2);
    } else if (lex_accept(T_enum)) {
        if (is_signed || is_unsigned || long_count || is_short || saw_base)
            error_at("enum type cannot have integer specifiers",
                     cur_token_loc());
        lex_ident(T_identifier, name);
        type = find_type_tag(name, parent);
    } else {
        if (!saw_base && !is_signed && !is_unsigned && !long_count &&
            !is_short) {
            lex_ident(T_identifier, name);
            type = find_type(name, true);
            goto parsed_base_type;
        }

        if (long_count)
            type = is_unsigned ? (long_count == 2 ? TY_ulong_long : TY_ulong)
                               : (long_count == 2 ? TY_long_long : TY_long);
        else if (is_short)
            type = is_unsigned ? TY_ushort : TY_short;
        else if (is_char)
            type = is_unsigned ? TY_uchar : (is_signed ? TY_schar : TY_char);
        else
            type = is_unsigned ? TY_uint : TY_int;
    }
parsed_base_type:
    if (!type)
        error_at("Unknown va_arg type", cur_token_loc());
    while (lex_accept(T_const) || lex_accept(T_volatile))
        ;
    while (lex_accept(T_asterisk)) {
        result->ptr_level++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }

    /* An array typedef is not itself an object type suitable for va_arg, but
     * `typedef int row[2]; row *` is a pointer to a row. Preserve the row
     * extent separately: type_t supplies the scalar element type while the
     * derived declarator supplies the stride for postfix indexing.
     */
    if (type->array_size) {
        type_t *element_type;

        if (type->base_type == TYPE_void)
            error_at("va_arg pointer-to-array cannot have void elements",
                     cur_token_loc());

        /* A pointer introduced by a typedef lives on type_t rather than the
         * abstract declarator's direct-star count. Both spellings make an array
         * typedef into a pointer-to-array object for va_arg.
         */
        if (!(result->ptr_level + type->ptr_level))
            error_at("va_arg array type is not an object type",
                     cur_token_loc());
        result->pointee_array_size = type->array_size;
        result->pointee_array_dim2 = type->array_dim2;
        result->pointee_array_dim3 = type->array_dim3;
        result->pointee_array_dim4 = type->array_dim4;

        /* A pointer-to-array typedef has already folded its bound into the
         * alias size. Its va_arg result still indexes scalar elements.
         */
        element_type = pointee_type_from_pointer_typedef(type);
        result->pointee_element_size = element_type->size;
        result->pointee_element_ptr_level = type->array_element_ptr_level;
    }

    /* Parenthesized abstract pointer declarators keep the pointer next to the
     * base type even when it is followed by an array or prototype suffix.
     */
    if (lex_accept(T_open_bracket)) {
        int nested_ptr = 0;
        int return_ptr_level = result->ptr_level;
        bool has_function_suffix = false;

        while (lex_accept(T_asterisk)) {
            nested_ptr++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
        if (!nested_ptr)
            error_at("va_arg type name needs an abstract pointer declarator",
                     cur_token_loc());
        result->ptr_level += nested_ptr;
        lex_expect(T_close_bracket);

        while (lex_peek(T_open_square, NULL) ||
               lex_peek(T_open_bracket, NULL)) {
            if (lex_accept(T_open_square)) {
                int dims = 0;
                int total = 0;

                if (type->base_type == TYPE_void)
                    error_at(
                        "va_arg pointer-to-array cannot have void elements",
                        cur_token_loc());

                if (nested_ptr != 1)
                    error_at(
                        "va_arg pointer-element array type is not yet "
                        "supported",
                        cur_token_loc());

                /* `int (*)[2][3]`: the parenthesized star denotes the value
                 * fetched from va_list; each suffix describes that pointer's
                 * pointee. Keep the familiar flattened-array representation
                 * used by normal declarations so the first subscript advances a
                 * whole row (or plane), not one scalar element.
                 */
                do {
                    int bound;

                    if (dims >= 4)
                        error_at(
                            "Array type names support at most four dimensions",
                            cur_token_loc());
                    if (lex_peek(T_close_square, NULL))
                        error_at("pointer-to-array type needs a bound",
                                 cur_token_loc());
                    bound = read_const_expr(parent);
                    if (bound <= 0)
                        error_at("pointer-to-array bound must be positive",
                                 cur_token_loc());
                    lex_expect(T_close_square);
                    if (!dims)
                        total = bound;
                    else {
                        total *= bound;
                        if (dims == 1)
                            result->pointee_array_dim2 = bound;
                        else if (dims == 2)
                            result->pointee_array_dim3 = bound;
                        else
                            result->pointee_array_dim4 = bound;
                    }
                    dims++;
                } while (lex_accept(T_open_square));
                result->pointee_array_size = total;
                result->pointee_element_size = type->size;
                result->pointee_element_ptr_level =
                    return_ptr_level + type->ptr_level;
            } else {
                func_t *func = arena_alloc_func();

                /* This is a function suffix on the pointed-to declarator.
                 * Preserve its prototype just as a named function-pointer
                 * declaration does, so the value returned by va_arg remains
                 * directly callable.
                 */
                func->return_def.type = type;
                func->return_def.ptr_level = return_ptr_level;
                func->returns_aggregate =
                    is_record_type(type) && !return_ptr_level;
                read_parameter_list_decl(func, true);
                result->func_signature = func;
                has_function_suffix = true;
            }
        }

        /* Stars before the parenthesized declarator belong to the function's
         * return type (`int *(*)(int)`), not to the pointer-sized function
         * pointer value.
         */
        if (has_function_suffix)
            result->ptr_level = nested_ptr;
    }

    /* A suffix after the ordinary pointer chain makes the result an array or a
     * function, not a pointer to one. Parse it so the diagnostic below is about
     * va_arg's object requirement instead of a stray closing token.
     */
    while (lex_peek(T_open_square, NULL) || lex_peek(T_open_bracket, NULL)) {
        if (lex_accept(T_open_square)) {
            result->is_array = true;
            if (!lex_peek(T_close_square, NULL))
                read_const_expr(parent);
            lex_expect(T_close_square);
        } else {
            int depth = 0;

            result->is_function = true;
            do {
                if (lex_accept(T_open_bracket))
                    depth++;
                else if (lex_accept(T_close_bracket))
                    depth--;
                else
                    lex_next();
            } while (depth > 0);
        }
    }
    result->type = type;
}

void emit_record_copy_from_address(block_t *parent,
                                   basic_block_t **bb,
                                   var_t *dest,
                                   var_t *src_addr)
{
    var_t *dest_addr = require_ref_var(parent, dest->type, 0);

    dest_addr->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, dest_addr, dest, NULL, 0, NULL);
    emit_record_copy_between(parent, bb, dest_addr, src_addr, size_var(dest));
}

void read_builtin_va_arg(block_t *parent, basic_block_t **bb)
{
    va_arg_type_t requested;
    var_t *ap_addr;
    var_t *old;
    var_t *step;
    var_t *next;

    lex_expect(T_identifier);
    lex_expect(T_open_bracket);
    if (!read_assignment_expression(parent, bb)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
    }
    ap_addr = opstack_pop();
    if (!has_effective_pointer(ap_addr))
        error_at("va_arg first argument must be va_list lvalue",
                 cur_token_loc());
    lex_expect(T_comma);
    read_builtin_va_arg_type(parent, &requested);
    lex_expect(T_close_bracket);
    if (requested.type->base_type == TYPE_void && !requested.ptr_level)
        error_at("va_arg cannot request void", cur_token_loc());
    if (is_record_type(requested.type) && !requested.ptr_level &&
        !requested.type->num_fields)
        error_at("va_arg cannot request incomplete record type",
                 cur_token_loc());
    if (requested.is_array || requested.is_function)
        error_at("va_arg type must be an object type", cur_token_loc());

    old = require_typed_ptr_var(parent, TY_int, 1);
    old->var_name = gen_name();
    add_insn(parent, *bb, OP_read, old, ap_addr, NULL, PTR_SIZE, NULL);
    step = require_typed_var(parent, TY_int);
    step->var_name = gen_name();

    /* This direct IR add is byte-addressed; unlike parsed pointer arithmetic it
     * does not apply the pointee-size scale itself.
     */
    step->init_val = PTR_SIZE;
    step->is_const = true;
    add_insn(parent, *bb, OP_load_constant, step, NULL, NULL, 0, NULL);
    next = require_typed_ptr_var(parent, TY_int, 1);
    next->var_name = gen_name();
    add_insn(parent, *bb, OP_add, next, old, step, 0, NULL);
    add_insn(parent, *bb, OP_write, NULL, ap_addr, next, PTR_SIZE, NULL);

    if (is_record_type(requested.type) && !requested.ptr_level) {
        var_t *source = require_ref_var(parent, requested.type, 0);
        var_t *result = require_typed_var(parent, requested.type);

        source->var_name = gen_name();
        add_insn(parent, *bb, OP_read, source, old, NULL, PTR_SIZE, NULL);
        result->var_name = gen_name();
        add_insn(parent, *bb, OP_allocat, result, NULL, NULL, 0, NULL);
        emit_record_copy_from_address(parent, bb, result, source);
        opstack_push(result);
    } else {
        var_t *result =
            require_typed_ptr_var(parent, requested.type, requested.ptr_level);

        result->var_name = gen_name();
        add_insn(parent, *bb, OP_read, result, old, NULL,
                 requested.ptr_level ? PTR_SIZE : requested.type->size, NULL);
        result->func_signature = requested.func_signature;
        opstack_push(result);

        /* A builtin result is a temporary, not a named lvalue, so it never
         * reaches read_lvalue()'s multidimensional subscript lowering. Apply
         * the derived pointee bounds here. For `int (*)[2][3]`, index zero
         * advances 6 ints, index one advances 3 ints, and the final index reads
         * the scalar selected by the address.
         */
        if (requested.pointee_array_size && lex_peek(T_open_square, NULL)) {
            int dims[4] = {
                requested.pointee_array_size, requested.pointee_array_dim2,
                requested.pointee_array_dim3, requested.pointee_array_dim4};
            int dimension_count = 1 + !!requested.pointee_array_dim2 +
                                  !!requested.pointee_array_dim3 +
                                  !!requested.pointee_array_dim4;
            int depth = 0;
            var_t *base = opstack_pop();

            while (lex_accept(T_open_square)) {
                var_t *index;
                var_t *address;
                int element_size = requested.pointee_element_ptr_level
                                       ? PTR_SIZE
                                   : requested.pointee_element_size
                                       ? requested.pointee_element_size
                                       : requested.type->size;
                int multiplier = element_size;

                /* The first subscript selects the array object itself; its
                 * elements are reached by one further subscript than there are
                 * declared array dimensions.
                 */
                if (depth > dimension_count)
                    error_at("Too many subscripts for pointer-to-array",
                             cur_token_loc());
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
                index = opstack_pop();
                lex_expect(T_close_square);
                if (!depth)
                    multiplier *= dims[0];
                else
                    for (int i = depth; i < dimension_count; i++)
                        multiplier *= dims[i];
                if (multiplier != 1) {
                    var_t *scale = require_typed_var(parent, TY_int);
                    scale->var_name = gen_name();
                    scale->init_val = multiplier;
                    scale->is_const = true;
                    add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL,
                             0, NULL);
                    var_t *scaled = require_var(parent);
                    scaled->var_name = gen_name();
                    add_insn(parent, *bb, OP_mul, scaled, index, scale, 0,
                             NULL);
                    index = scaled;
                }
                address = require_typed_ptr_var(
                    parent, requested.type,
                    requested.pointee_element_ptr_level + 1);
                address->var_name = gen_name();
                add_insn(parent, *bb, OP_add, address, base, index, 0, NULL);
                depth++;
                if (depth == dimension_count + 1 &&
                    !requested.pointee_element_ptr_level &&
                    is_record_type(requested.type)) {
                    /* A record element is an object: copy it whole rather than
                     * read one scalar of the record's size, which drops bytes
                     * and is a width no narrow backend can load.
                     */
                    var_t *value = require_typed_var(parent, requested.type);

                    value->var_name = gen_name();
                    add_insn(parent, *bb, OP_allocat, value, NULL, NULL, 0,
                             NULL);
                    emit_record_copy_from_address(parent, bb, value, address);
                    base = value;
                } else if (depth == dimension_count + 1) {
                    var_t *value = require_typed_ptr_var(
                        parent, requested.type,
                        requested.pointee_element_ptr_level);
                    value->var_name = gen_name();
                    add_insn(parent, *bb, OP_read, value, address, NULL,
                             element_size, NULL);
                    base = value;
                } else
                    base = address;
            }
            opstack_push(base);
        }

        if (requested.func_signature && lex_peek(T_open_bracket, NULL)) {
            result = emit_indirect_call_result(result, requested.func_signature,
                                               true, parent, bb);
        }
    }
}

/* Function-pointer prototypes are stored as opaque data in var_t so defs.h
 * remains parseable before func_t is complete.
 */
func_t *get_func_signature(var_t *var)
{
    if (!var)
        return NULL;
    return var->func_signature;
}

/* A function-pointer object is represented as an is_func declaration, while a
 * value read from that object is an ordinary pointer-sized SSA value carrying
 * the declaration's prototype. Keeping the two distinct matters for calls: the
 * former must be addressed and loaded; the latter can be passed to the
 * indirect-call instruction directly.
 */
var_t *load_function_pointer_object(block_t *parent,
                                    basic_block_t **bb,
                                    var_t *object)
{
    /* SSA versions of parameters already contain the incoming pointer value.
     * Preserve it through an ordinary pointer temporary: is_func denotes a
     * storage object, whereas the indirect-call backend expects a value.
     */
    if (parent && parent->func && !object->address_taken) {
        for (int i = 0; i < parent->func->num_params; i++) {
            var_t *param = &parent->func->param_defs[i];

            if (object == param || object->base == param) {
                func_t *param_signature = get_func_signature(object);
                var_t *value = require_typed_ptr_var(
                    parent, param_signature->return_def.type, 1);

                value->var_name = gen_name();
                value->func_signature = param_signature;
                add_insn(parent, *bb, OP_assign, value, object, NULL, 0, NULL);
                return value;
            }
        }
    }

    var_t *address = require_ref_var(parent, object->type, object->ptr_level);
    func_t *signature = get_func_signature(object);
    var_t *target = require_typed_ptr_var(
        parent, signature ? signature->return_def.type : object->type, 1);

    address->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, address, object, NULL, 0, NULL);
    target->var_name = gen_name();
    target->func_signature = object->func_signature;
    target->func_target = object->func_target;
    target->func_target_invalid = object->func_target_invalid;
    add_insn(parent, *bb, OP_read, target, address, NULL, PTR_SIZE, NULL);
    return target;
}

/* C99 function designators decay to a pointer value in an argument expression.
 * A raw symbol deliberately has no storage or defining IR so global
 * initializers can emit a relocation; feeding it straight to OP_push makes
 * register allocation reload an unallocated local. Reuse the established
 * temporary-object path, which lowers the symbol through OP_address_of_func and
 * then reads the resulting pointer-sized value.
 */
var_t *materialize_function_designator(block_t *parent,
                                       basic_block_t **bb,
                                       var_t *value)
{
    func_t *func;
    var_t *object;

    if (!value || !value->is_func)
        return value;
    if (find_var(value->var_name, parent) == value)
        return load_function_pointer_object(parent, bb, value);
    func = find_func(value->var_name);
    if (!func)
        return value;

    object = require_typed_ptr_var(parent, func->return_def.type,
                                   func->return_def.ptr_level);
    object->var_name = gen_name();
    object->is_func = true;
    object->func_signature = func;
    object->func_target = func;
    add_insn(parent, *bb, OP_allocat, object, NULL, NULL, 0, NULL);
    emit_object_assignment(parent, bb, object, value);
    return load_function_pointer_object(parent, bb, object);
}

void read_indirect_call_with_sret(var_t *callee,
                                  func_t *signature,
                                  var_t *sret,
                                  block_t *parent,
                                  basic_block_t **bb)
{
    /* A function pointer carries its parsed prototype on the declaration. This
     * makes indirect calls obey the same record-by-value lowering and scalar
     * conversions as a direct call. Legacy/unprototyped pointers keep the old
     * generic behaviour. Keep the evaluated target as the OP_indirect source:
     * the call's liveness then protects it while ABI argument staging reuses
     * the argument registers.
     */
    var_t *target = opstack_pop();

    UNUSED(callee);

    read_func_parameters_with_sret(signature, sret, parent, bb);

    add_insn(parent, *bb, OP_indirect, NULL, target, NULL, 0, NULL);
}

void read_indirect_call(var_t *callee, block_t *parent, basic_block_t **bb)
{
    read_indirect_call_with_sret(callee, get_func_signature(callee), NULL,
                                 parent, bb);
}

/* Aggregate calls write their value to caller-owned storage. The object is a
 * temporary expression value, not a modifiable lvalue; callers only receive it
 * on the operand stack after the call has completed.
 */
var_t *prepare_aggregate_call_result(block_t *parent,
                                     basic_block_t **bb,
                                     func_t *signature,
                                     var_t **sret)
{
    var_t *result = require_typed_var(parent, signature->return_def.type);
    var_t *destination;

    result->var_name = gen_name();
    add_insn(parent, *bb, OP_allocat, result, NULL, NULL, 0, NULL);

    destination = require_ref_var(parent, signature->return_def.type, 0);
    destination->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, destination, result, NULL, 0, NULL);
    *sret = destination;
    return result;
}

/* Keep the ABI-only aggregate destination coupled to call emission. Call sites
 * that merely discard an aggregate result still must provide it.
 */
var_t *emit_direct_call_result(func_t *func,
                               bool want_value,
                               block_t *parent,
                               basic_block_t **bb)
{
    func_t *returned_signature = func->return_def.type->func_signature;
    var_t *result = NULL;

    if (func->returns_aggregate) {
        var_t *sret;
        func->aggregate_call_used = true;
        result = prepare_aggregate_call_result(parent, bb, func, &sret);
        read_func_call_with_sret(func, sret, parent, bb);
    } else {
        read_func_call(func, parent, bb);
        if (want_value) {
            result = require_typed_ptr_var(
                parent,
                returned_signature ? returned_signature->return_def.type
                                   : func->return_def.type,
                returned_signature ? 1 : func->return_def.ptr_level);
            result->var_name = gen_name();
            result->func_signature = returned_signature;
            copy_call_result_array_shape(result, &func->return_def);
            add_insn(parent, *bb, OP_func_ret, result, NULL, NULL, 0, NULL);
        }
    }
    if (want_value)
        opstack_push(result);
    return result;
}

var_t *emit_indirect_call_result(var_t *callee,
                                 func_t *signature,
                                 bool want_value,
                                 block_t *parent,
                                 basic_block_t **bb)
{
    var_t *result = NULL;

    if (signature && signature->returns_aggregate) {
        var_t *sret;
        func_t *target = callee ? callee->func_target : NULL;

        if (callee->func_target_invalid || !target || !target->bbs ||
            !target->returns_aggregate)
            error_at(
                "aggregate-return indirect call requires a shecc-defined "
                "target",
                cur_token_loc());
        result = prepare_aggregate_call_result(parent, bb, signature, &sret);
        read_indirect_call_with_sret(callee, signature, sret, parent, bb);
    } else {
        read_indirect_call(callee, parent, bb);
        if (want_value && signature) {
            result = require_typed_ptr_var(parent, signature->return_def.type,
                                           signature->return_def.ptr_level);
            result->var_name = gen_name();
            result->func_signature = signature->return_def.type->func_signature;
            copy_call_result_array_shape(result, &signature->return_def);
            add_insn(parent, *bb, OP_func_ret, result, NULL, NULL, 0, NULL);
        }
    }
    if (want_value)
        opstack_push(result);
    return result;
}

var_t *bitfield_constant(block_t *parent, basic_block_t **bb, unsigned value)
{
    var_t *result = require_var(parent);
    result->var_name = gen_name();
    result->init_val = (int) value;
    result->is_const = true;
    result->type = TY_uint;
    add_insn(parent, *bb, OP_load_constant, result, NULL, NULL, 0, NULL);
    return result;
}

var_t *bitfield_binary(block_t *parent,
                       basic_block_t **bb,
                       opcode_t op,
                       var_t *left,
                       var_t *right,
                       type_t *type)
{
    var_t *result = require_var(parent);
    result->var_name = gen_name();
    result->type = type;
    add_insn(parent, *bb, op, result, left, right, 0, NULL);
    return result;
}

unsigned bitfield_mask(const var_t *field)
{
    return field->bit_width == 32 ? ~0U : (1U << field->bit_width) - 1;
}

var_t *read_bitfield_value(block_t *parent,
                           basic_block_t **bb,
                           var_t *address,
                           const var_t *field)
{
    var_t *value = require_var(parent);
    value->var_name = gen_name();
    value->type = TY_uint;
    add_insn(parent, *bb, OP_read, value, address, NULL,
             field->bit_storage_size, NULL);
    if (field->bit_offset)
        value = bitfield_binary(
            parent, bb, OP_rshift, value,
            bitfield_constant(parent, bb, field->bit_offset), TY_uint);
    if (field->bit_width != 32)
        value = bitfield_binary(
            parent, bb, OP_bit_and, value,
            bitfield_constant(parent, bb, bitfield_mask(field)), TY_uint);
    if (!is_bool_type(field->type) && !field->type->is_unsigned &&
        field->bit_width != 32) {
        unsigned sign_bit = 1U << (field->bit_width - 1);

        /* Avoid depending on a target's right-shift instruction being
         * arithmetic: (x ^ sign) - sign is sign extension in pure integer
         * arithmetic for every width below the allocation unit.
         */
        value =
            bitfield_binary(parent, bb, OP_bit_xor, value,
                            bitfield_constant(parent, bb, sign_bit), TY_int);
        value =
            bitfield_binary(parent, bb, OP_sub, value,
                            bitfield_constant(parent, bb, sign_bit), TY_int);
    }
    if (is_bool_type(field->type))
        value = normalize_bool(parent, bb, value);

    /* C99's integer promotions apply to bit-fields too. A narrow unsigned int
     * bit-field fits in int on the supported targets, so retain its extracted
     * value but mark it as int before ordinary expression lowering.
     */
    value->type = field->type->is_unsigned &&
                          field->bit_width < field->bit_storage_size * 8
                      ? TY_int
                      : field->type;

    /* Retain this constraint-only provenance until an operator creates a new
     * expression value. `sizeof` must reject a direct bit-field operand.
     */
    value->is_bitfield = true;
    return value;
}

void write_bitfield_value(block_t *parent,
                          basic_block_t **bb,
                          var_t *address,
                          var_t *value,
                          const var_t *field)
{
    if (is_bool_type(field->type))
        value = normalize_bool(parent, bb, value);
    var_t *old = require_var(parent);
    old->var_name = gen_name();
    old->type = TY_uint;
    add_insn(parent, *bb, OP_read, old, address, NULL, field->bit_storage_size,
             NULL);
    value = bitfield_binary(parent, bb, OP_bit_and, value,
                            bitfield_constant(parent, bb, bitfield_mask(field)),
                            TY_uint);
    if (field->bit_offset)
        value = bitfield_binary(
            parent, bb, OP_lshift, value,
            bitfield_constant(parent, bb, field->bit_offset), TY_uint);
    unsigned mask = bitfield_mask(field) << field->bit_offset;
    var_t *clear = require_var(parent);
    clear->var_name = gen_name();
    clear->type = TY_uint;
    add_insn(parent, *bb, OP_bit_not, clear,
             bitfield_constant(parent, bb, mask), NULL, 0, NULL);
    old = bitfield_binary(parent, bb, OP_bit_and, old, clear, TY_uint);
    value = bitfield_binary(parent, bb, OP_bit_or, old, value, TY_uint);
    add_insn(parent, *bb, OP_write, NULL, address, value,
             field->bit_storage_size, NULL);
}

void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 basic_block_t **bb,
                 bool eval,
                 opcode_t op,
                 bool allow_ptr_arith);

/* Maintain a stack of expression values and operators, depending on next
 * operators' priority. Either apply it or operator on stack first.
 */
void handle_address_of_operator(block_t *parent, basic_block_t **bb)
{
    char token[MAX_VAR_LEN];
    lvalue_t lvalue;
    var_t *vd, *rs1;

    if (!lex_peek(T_identifier, token))
        error_at("Expected an identifier", next_token_loc());
    if (!strcmp(token, "__func__")) {
        if (!parent->func || !parent->func->return_def.var_name[0])
            error_at("__func__ is only defined inside a function",
                     next_token_loc());
        lex_expect(T_identifier);
        vd = require_typed_ptr_var(parent, TY_char, 1);
        vd->var_name = gen_name();
        vd->init_val = write_symbol(parent->func->return_def.var_name);
        vd->is_string_literal = true;
        vd->is_func_name_array_address = true;
        opstack_push(vd);
        add_insn(parent, *bb, OP_load_rodata_address, vd, NULL, NULL, 0, NULL);
        return;
    }

    /* A function designator already converts to its address. Preserve the
     * symbol instead of sending it through lvalue lookup, which only knows
     * objects and previously made `&function` fail outside file-scope scalar
     * initializers.
     */
    func_t *addressed_func = find_visible_func(token, parent);
    if (addressed_func) {
        if (parent->func && parent->func->is_inline &&
            !parent->func->is_static && addressed_func->is_static)
            error_at(
                "external inline definition references internal-linkage "
                "function",
                next_token_loc());
        lex_expect(T_identifier);
        vd = require_func_symbol_var(parent);
        vd->is_func = true;
        vd->var_name = intern_string(token);
        opstack_push(vd);
        return;
    }
    var_t *var = find_var(token, parent);
    if (var && var->is_register)
        error_at("cannot take address of register object", next_token_loc());
    read_lvalue(&lvalue, var, parent, bb, false, OP_generic, true);

    if (is_bitfield(lvalue.decl))
        error_at("cannot take address of bit-field", next_token_loc());

    if (!lvalue.is_reference) {
        rs1 = opstack_pop();
        vd = require_ref_var(parent, lvalue.type, lvalue.ptr_level);
        vd->var_name = gen_name();

        /* Address-of moves a pointer object's qualification one level inward:
         * `&p`, for `int * const p`, is `int * const *`, not `const int **`.
         */
        vd->is_const_qualified = lvalue.decl ? lvalue.decl->is_const_qualified
                                             : lvalue.is_const_qualified;
        vd->pointer_const_mask = lvalue.pointer_const_mask;
        vd->pointee_func_signature =
            lvalue.decl ? (lvalue.decl->pointee_func_signature
                               ? lvalue.decl->pointee_func_signature
                               : lvalue.decl->func_signature)
                        : NULL;
        opstack_push(vd);
        if (lvalue.decl && is_array_declarator(lvalue.decl)) {
            /* An array expression already denotes its backing address in the
             * IR. Taking OP_address_of of its variable would instead point at
             * the compiler's local array-base slot, so `&row` passed a pointer
             * to that slot rather than a pointer to row. Keep the same runtime
             * address while adding the source-level pointer indirection
             * required by the address-of operator.
             */
            add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
        } else
            add_insn(parent, *bb, OP_address_of, vd, rs1, NULL, 0, NULL);
    }
}

/* A scalar pointer-to-array dereference designates the addressed row; it does
 * not load a scalar object. Retain that row as an array descriptor so a
 * following grouped postfix subscript can use the ordinary array path.
 */
static bool lower_scalar_pointee_array_dereference(var_t *source,
                                                   block_t *parent,
                                                   basic_block_t **bb)
{
    type_t *element_type;
    var_t *row;

    if (!source || !source->type || !source->pointee_array_size ||
        source->pointee_array_element_ptr_level ||
        effective_pointer_depth(source) != 1)
        return false;

    element_type = source->type->pointee_array_element_type
                       ? source->type->pointee_array_element_type
                       : source->type;
    row = require_var(parent);
    row->type = element_type;
    fixed_array_shape_t shape = fixed_array_shape_from_pointee_var(source);

    fixed_array_shape_to_var(row, &shape);
    row->is_const_qualified =
        source->is_const_qualified || element_type->is_const_qualified;
    row->var_name = gen_name();
    add_insn(parent, *bb, OP_assign, row, source, NULL, 0, NULL);
    opstack_push(row);
    return true;
}

static void copy_pointee_array_shape(var_t *destination, const var_t *source)
{
    fixed_array_shape_t shape = fixed_array_shape_from_pointee_var(source);

    fixed_array_shape_to_pointee_var(destination, &shape);
    destination->pointee_array_element_ptr_level =
        source->pointee_array_element_ptr_level;
}

static bool is_scalar_pointee_array_pointer(const var_t *var)
{
    return var && var->type && var->pointee_array_size > 0 &&
           !var->pointee_array_element_ptr_level &&
           effective_pointer_depth(var) == 1;
}

static int scalar_pointee_array_row_stride(const var_t *var)
{
    type_t *element_type = var->type->pointee_array_element_type
                               ? var->type->pointee_array_element_type
                               : var->type;

    return var->pointee_array_size * element_type->size;
}

/* Byte extent remaining after direct-array subscript depth. This is shared by
 * ordinary postfix indexing and the bounded grouped pointer-to-array path.
 */
static int fixed_array_shape_stride(const fixed_array_shape_t *shape,
                                    int subscript_depth,
                                    int element_size)
{
    int stride = element_size;

    for (int i = subscript_depth + 1; i < shape->rank; i++)
        stride *= shape->bounds[i];
    return stride;
}

static bool fixed_array_shape_drop_outer(fixed_array_shape_t *shape)
{
    if (!shape->rank)
        return false;
    for (int i = 1; i < shape->rank; i++)
        shape->bounds[i - 1] = shape->bounds[i];
    shape->rank--;
    return true;
}

static int fixed_array_subscript_stride(const var_t *var,
                                        int subscript_depth,
                                        int element_size)
{
    fixed_array_shape_t shape = fixed_array_shape_from_var(var);

    return fixed_array_shape_stride(&shape, subscript_depth, element_size);
}

static int fixed_pointee_array_subscript_stride(const var_t *var,
                                                int subscript_depth,
                                                int element_size)
{
    fixed_array_shape_t shape = fixed_array_shape_from_pointee_var(var);

    if (subscript_depth == 0)
        return var->pointee_array_size * element_size;
    return fixed_array_shape_stride(&shape, subscript_depth - 1, element_size);
}

static int fixed_array_decay_stride(const var_t *var)
{
    return fixed_array_subscript_stride(var, 0, var->type->size);
}

static bool scalar_pointee_array_shapes_compatible(const var_t *left,
                                                   const var_t *right)
{
    type_t *left_element;
    type_t *right_element;

    if (!is_scalar_pointee_array_pointer(left) ||
        !is_scalar_pointee_array_pointer(right))
        return false;
    left_element = left->type->pointee_array_element_type
                       ? left->type->pointee_array_element_type
                       : left->type;
    right_element = right->type->pointee_array_element_type
                        ? right->type->pointee_array_element_type
                        : right->type;
    return left->pointee_array_size == right->pointee_array_size &&
           left->pointee_array_dim2 == right->pointee_array_dim2 &&
           left->pointee_array_dim3 == right->pointee_array_dim3 &&
           left->pointee_array_dim4 == right->pointee_array_dim4 &&
           left->pointee_array_element_ptr_level ==
               right->pointee_array_element_ptr_level &&
           compatible_decl_type(left_element, right_element);
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

        if (rs1->is_func_name_array_address) {
            /* The address of an array has the same runtime value as its first
             * element. Dereferencing it restores the array, which immediately
             * decays back to char * in this expression context.
             */
            vd = require_typed_ptr_var(parent, TY_char, 1);
            vd->var_name = gen_name();
            vd->is_string_literal = true;
            opstack_push(vd);
            add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
            return;
        }

        if (lower_scalar_pointee_array_dereference(rs1, parent, bb))
            return;

        /* For pointer dereference, we need to determine the target type and
         * size. Since we do not have full type tracking in expressions, use
         * defaults
         */
        type_t *deref_type = rs1->type ? rs1->type : TY_int;
        int deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;

        /* require_deref_var() takes the *source* pointer level and returns a
         * variable one level shallower. Passing the already-decremented value
         * dropped two levels per dereference, so "**(q + 0)" on an int** ended
         * up reading an int-sized word where a pointer was stored. The sizes
         * coincide on the 32-bit targets, which is why it only surfaces here.
         */
        vd = require_deref_var(parent, deref_type, rs1->ptr_level);
        if (deref_ptr > 0)
            sz = PTR_SIZE;
        else
            sz = deref_type->size;
        vd->var_name = gen_name();
        vd->is_const_qualified = rs1->is_const_qualified;
        vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
        vd->is_const_pointer =
            vd->ptr_level > 0 && vd->ptr_level <= 32 &&
            (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));

        /* Pointer arithmetic can produce a pointer to a callback typedef. The
         * actual dereference consumes that object-pointer level and yields the
         * pointer-valued callback result for a following postfix call.
         */
        if (deref_type && deref_type->func_signature &&
            effective_pointer_depth(rs1) == 1) {
            vd->func_signature = deref_type->func_signature;
            vd->ptr_level = 1;
            sz = PTR_SIZE;
        }
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
    } else if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
        /* A prefix update is itself a unary expression, so `*++pointer` and
         * `*--pointer` dereference its updated pointer result just like
         * `*(pointer + 1)` dereferences a parenthesized operand.
         */
        read_expr_operand(parent, bb);
        rs1 = opstack_pop();
        type_t *deref_type = rs1->type ? rs1->type : TY_int;
        int deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;

        vd = require_deref_var(parent, deref_type, rs1->ptr_level);
        sz = deref_ptr > 0 ? PTR_SIZE
                           : pointer_typedef_pointee_size(
                                 deref_type,
                                 pointee_type_from_pointer_typedef(deref_type));
        vd->var_name = gen_name();
        vd->is_const_qualified = rs1->is_const_qualified;
        vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
        vd->is_const_pointer =
            vd->ptr_level > 0 && vd->ptr_level <= 32 &&
            (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));
        if (deref_type && deref_type->func_signature &&
            effective_pointer_depth(rs1) == 1) {
            vd->func_signature = deref_type->func_signature;
            vd->ptr_level = 1;
            sz = PTR_SIZE;
        }
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
    } else {
        /* Handle simple identifier dereference: *var */
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        if (!lex_peek(T_identifier, token))
            error_at("Expected an identifier", next_token_loc());

        /* Builtins are expression operands, not objects in the local symbol
         * table. Keep the identifier lvalue path below for `*pointer`
         * assignments, but let a pointer-valued builtin such as `*va_arg(...)`
         * use the same rvalue dereference lowering as `*(expression)`.
         */
        if (!strcmp(token, "__builtin_va_arg")) {
            type_t *deref_type;
            int deref_ptr;

            read_expr_operand(parent, bb);
            rs1 = opstack_pop();
            deref_type = rs1->type ? rs1->type : TY_int;
            deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;
            vd = require_deref_var(parent, deref_type, rs1->ptr_level);
            sz = deref_ptr > 0 ? PTR_SIZE : deref_type->size;
            vd->var_name = gen_name();
            vd->is_const_qualified = rs1->is_const_qualified;
            vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
            vd->is_const_pointer =
                vd->ptr_level > 0 && vd->ptr_level <= 32 &&
                (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
            return;
        }
        var_t *var = find_var(token, parent);

        /* A raw function name is a function designator, not an object lvalue.
         * Dereferencing it is a no-op in C99 (`*f` is another designator), so
         * route it through the same symbol representation used by ordinary
         * direct calls instead of asking read_lvalue() to find object storage.
         */
        if (!var && find_visible_func(token, parent)) {
            lex_expect(T_identifier);
            rs1 = require_func_symbol_var(parent);
            rs1->var_name = intern_string(token);
            rs1->is_func = true;
            opstack_push(rs1);
            return;
        }
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic, false);

        rs1 = opstack_pop();
        if (var->is_func) {
            /* C99 6.3.2.1 makes a function-pointer dereference a function
             * designator. The pointer object itself therefore needs one load,
             * not a dereference of its return type.
             */
            opstack_push(load_function_pointer_object(parent, bb, rs1));
            return;
        }
        if (lower_scalar_pointee_array_dereference(rs1, parent, bb))
            return;

        /* A member function-pointer typedef was already loaded by
         * read_lvalue(). Unary `*` turns that pointer into a function
         * designator; it does not read from the code address.
         */
        if (rs1->func_signature && !effective_pointer_depth(rs1)) {
            rs1->ptr_level = 1;
            opstack_push(rs1);
            return;
        }

        /* `read_lvalue()` may have resolved a member expression. Derive the
         * final indirection from its evaluated value rather than from the
         * initial record object, so `*record.pointer` dereferences the member
         * pointer instead of attempting to dereference the record itself.
         */
        type_t *deref_type = rs1->type ? rs1->type : TY_int;
        int deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;

        vd = require_deref_var(parent, deref_type, rs1->ptr_level);
        sz = deref_ptr > 0 ? PTR_SIZE : deref_type->size;
        vd->var_name = gen_name();
        vd->is_const_qualified = rs1->is_const_qualified;
        vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
        vd->is_const_pointer =
            vd->ptr_level > 0 && vd->ptr_level <= 32 &&
            (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));

        /* `*slot` is the function-pointer value when slot is a pointer to a
         * callback typedef. The outer declarator's signature was deliberately
         * cleared to prevent the invalid `slot(...)` form, so restore the
         * element signature only after the real dereference has occurred.
         */
        if ((rs1->func_signature || rs1->pointee_func_signature ||
             (deref_type && deref_type->func_signature)) &&
            effective_pointer_depth(rs1) == 1) {
            vd->func_signature = rs1->func_signature
                                     ? rs1->func_signature
                                     : (rs1->pointee_func_signature
                                            ? rs1->pointee_func_signature
                                            : deref_type->func_signature);
            vd->ptr_level = 1;
            sz = PTR_SIZE;
        }
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
    }
}

/* Scan ahead for an assignment operator at the top level of the statement that
 * starts at the current token, stopping at its terminating semicolon.
 *
 * A statement beginning with '*' is either a store through a pointer or a plain
 * expression, and the two need opposite treatment of the leading asterisk.
 * Deciding by looking at tokens keeps the choice free of side effects: by the
 * time an expression has been parsed, its instructions have already been
 * emitted and there is no way back.
 */
bool stmt_starts_assignment(void)
{
    int depth = 0;

    for (token_t *t = cur_token->next; t; t = t->next) {
        switch (t->kind) {
        case T_open_bracket:
        case T_open_square:
            depth++;
            break;
        case T_close_bracket:
        case T_close_square:
            depth--;
            break;
        case T_semicolon:
        case T_open_curly:
        case T_close_curly:
        case T_eof:
            return false;
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
            if (depth == 0)
                return true;
            break;
        default:
            break;
        }
    }
    return false;
}

void handle_multiple_dereference(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    int sz;

    /* Handle consecutive asterisks for multiple dereference: **pp, ***ppp, and
     * the parenthesized ***(expr) form.
     */
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

            vd = require_deref_var(parent, deref_type, rs1->ptr_level);
            if (deref_ptr > 0)
                sz = PTR_SIZE;
            else
                sz = deref_type->size;
            vd->var_name = gen_name();
            vd->is_const_qualified = rs1->is_const_qualified;
            vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
            vd->is_const_pointer =
                vd->ptr_level > 0 && vd->ptr_level <= 32 &&
                (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));

            /* A parenthesized pointer-arithmetic result can designate a
             * callback object just like `*slot`. Restore its prototype only
             * after consuming exactly that final object-pointer indirection.
             */
            if (deref_type && deref_type->func_signature &&
                effective_pointer_depth(rs1) == 1) {
                vd->func_signature = deref_type->func_signature;
                vd->ptr_level = 1;
                sz = PTR_SIZE;
            }
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
        }
    } else {
        /* Handle **pp, ***ppp case with simple identifier */
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        if (!lex_peek(T_identifier, token))
            error_at("Expected an identifier", next_token_loc());
        var_t *var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic, false);

        /* Apply dereferences one by one */
        for (int i = 0; i < deref_count; i++) {
            rs1 = opstack_pop();

            /* A member lvalue has already been read into rs1. Each unary
             * asterisk must consume that evaluated pointer, not re-derive its
             * type from the initial record identifier.
             */
            type_t *deref_type = rs1->type ? rs1->type : TY_int;
            int deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;

            vd = require_deref_var(parent, deref_type, rs1->ptr_level);
            sz = deref_ptr > 0
                     ? PTR_SIZE
                     : pointer_typedef_pointee_size(
                           deref_type,
                           pointee_type_from_pointer_typedef(deref_type));
            vd->var_name = gen_name();
            vd->is_const_qualified = rs1->is_const_qualified;
            vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
            vd->is_const_pointer =
                vd->ptr_level > 0 && vd->ptr_level <= 32 &&
                (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));

            /* Only the final dereference of `**slots` (or deeper spelling)
             * reaches the callback object. Earlier reads still produce a
             * pointer-to-callback and must not be callable.
             */
            if (i + 1 == deref_count &&
                (rs1->func_signature ||
                 (deref_type && deref_type->func_signature)) &&
                effective_pointer_depth(rs1) == 1) {
                vd->func_signature = rs1->func_signature
                                         ? rs1->func_signature
                                         : deref_type->func_signature;
                vd->ptr_level = 1;
                sz = PTR_SIZE;
            }
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
        }
    }
}
