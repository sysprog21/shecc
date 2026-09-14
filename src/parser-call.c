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
            if (is_record_type(target->type) && !target->is_func &&
                !has_effective_pointer(target) && !target->array_size) {
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
            } else if (!is_pointer_like_value(param) &&
                       param->type->size <= TY_int->size) {
                /* A long long is not promoted, and promote() warns about a
                 * value wider than its int target.
                 */
                param = promote(parent, bb, param, TY_int, 0);
            }
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

        /* A comma separates two arguments (C99 6.5.2p1): it cannot end the
         * list, and two arguments cannot go without one. Neither is an
         * extension any compiler offers, so no mode accepts them.
         */
        if (lex_accept(T_close_bracket))
            break;
        lex_expect(T_comma);
        if (lex_peek(T_close_bracket, NULL))
            error_at("Expected an argument after ',' in function call",
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

        /* A call returning a pointer to an array of callbacks carries their
         * prototype on the result rather than on its scalar base type.
         */
        element->func_signature = base->type->func_signature
                                      ? base->type->func_signature
                                      : base->func_signature;
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
    if (op == OP_generic)
        return;
    *value = lower_reference_update(opstack_pop(), op, parent, bb);
    opstack_push(*value);
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
    base_type_t record_kind = accept_record_keyword();
    if (record_kind) {
        lex_ident(T_identifier, name);
        record = find_record_tag(name, parent, record_kind);
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
    type_t *type;

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
    type = read_type_name_specifiers(parent);
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

    /* A floating value would enter the integer-only IR and argument ABI,
     * whether the type name spells it directly or through a typedef.
     */
    if (type->is_floating && !result->ptr_level && !type->ptr_level)
        error_at("Floating point types are not yet supported", cur_token_loc());
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

/* Reject a record operand where C99 requires an arithmetic or scalar one: the
 * operators of 6.5.3.3 and 6.5.5 to 6.5.15, and a controlling expression. A
 * record reaching integer lowering has no register representation, which a
 * narrow backend reported by aborting.
 */
void reject_record_operand(const var_t *var)
{
    if (is_record_object(var))
        error_at("Operand of record type requires a scalar value",
                 cur_token_loc());
}

/* Whether @var is a declared object rather than a generated temporary. */
bool is_named_object(var_t *var, block_t *parent)
{
    return var->var_name[0] != '.' && find_var(var->var_name, parent) == var;
}

/* Record on @value, just loaded from @address, that it designates the object
 * stored there, so a following ++, -- or member selection reaches that object
 * rather than the temporary. @bitfield is the field of a bit-field object.
 */
void mark_value_reference(var_t *value, var_t *address, var_t *bitfield)
{
    value->is_compound_literal_reference = true;
    value->compound_literal_address = address;
    value->compound_literal_bitfield = bitfield;
}

/* Load the object of @value's type stored at @address into @value and push it.
 * A record is not a register value: one OP_read of its whole size loaded only a
 * word of it. Nor is it copied into storage of its own, which cost instructions
 * in proportion to its size even when only a member was then selected: @value
 * stands for the object in place, and whatever copies the record reads it
 * there.
 */
void push_object_at(block_t *parent,
                    basic_block_t **bb,
                    var_t *value,
                    var_t *address,
                    int size)
{
    if (is_record_object(value) && !value->func_signature &&
        !is_incomplete_record_object(value))
        value->defers_record_copy = true;
    else
        add_insn(parent, *bb, OP_read, value, address, NULL, size, NULL);
    mark_value_reference(value, address, NULL);
    opstack_push(value);
}

/* Apply ++ or -- (@op is OP_add or OP_sub) to @object, a value that records the
 * address it was loaded from, and store the result there.
 *
 * Returns the value the object then holds.
 */
var_t *lower_reference_update(var_t *object,
                              opcode_t op,
                              block_t *parent,
                              basic_block_t **bb)
{
    var_t *one;
    var_t *updated;

    if (!object->is_compound_literal_reference || is_record_object(object) ||
        object->array_size || object->is_func || object->func_signature)
        error_at("Increment or decrement requires a scalar modifiable lvalue",
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
    if (object->compound_literal_bitfield) {
        write_bitfield_value(parent, bb, object->compound_literal_address,
                             updated, object->compound_literal_bitfield);
        updated =
            read_bitfield_value(parent, bb, object->compound_literal_address,
                                object->compound_literal_bitfield);
        updated->is_bitfield = false;
    } else
        add_insn(parent, *bb, OP_write, NULL, object->compound_literal_address,
                 updated,
                 object->ptr_level || object->type->ptr_level
                     ? PTR_SIZE
                     : object->type->size,
                 NULL);
    return updated;
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

    /* A long long on a 32-bit target takes two ABI words starting at an even
     * one, both for AAPCS32 and for a variadic argument on RV32, and the callee
     * saves its argument words from an eight-byte aligned slot. Round the
     * cursor up to the pair's first word and step over both.
     */
    if (PTR_SIZE < 8 && !requested.ptr_level &&
        !is_record_type(requested.type) && requested.type->size == 8) {
        var_t *mask = require_typed_var(parent, TY_int);
        var_t *aligned = require_typed_ptr_var(parent, TY_int, 1);

        step->init_val = 7;
        step->is_const = true;
        add_insn(parent, *bb, OP_load_constant, step, NULL, NULL, 0, NULL);
        aligned->var_name = gen_name();
        add_insn(parent, *bb, OP_add, aligned, old, step, 0, NULL);
        mask->var_name = gen_name();
        mask->init_val = -8;
        mask->is_const = true;
        add_insn(parent, *bb, OP_load_constant, mask, NULL, NULL, 0, NULL);
        old = require_typed_ptr_var(parent, TY_int, 1);
        old->var_name = gen_name();
        add_insn(parent, *bb, OP_bit_and, old, aligned, mask, 0, NULL);
        step = require_typed_var(parent, TY_int);
        step->var_name = gen_name();
    }

    /* This direct IR add is byte-addressed; unlike parsed pointer arithmetic it
     * does not apply the pointee-size scale itself.
     */
    step->init_val = PTR_SIZE < 8 && !requested.ptr_level &&
                             !is_record_type(requested.type) &&
                             requested.type->size == 8
                         ? 8
                         : PTR_SIZE;
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

            /* An element's pointer depth is counted in full by
             * pointee_element_ptr_level. A typedef such as "typedef int
             * *pointer; typedef pointer row[2];" leaves that same depth on the
             * row's type_t as well, and building the element on top of it
             * counted the star twice: dereferencing the element then read a
             * pointer's width where an int was stored.
             */
            type_t *element_type =
                requested.pointee_element_ptr_level
                    ? pointee_type_from_pointer_typedef(requested.type)
                    : requested.type;

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
                    parent, element_type,
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
                        parent, element_type,
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

        /* The destination-pointer convention is shecc's own, so the callee must
         * be a shecc-defined function. That cannot be proven per pointer
         * object: a file-scope pointer, record member or array element can be
         * assigned anywhere. prune_unused_funcs() instead rejects taking the
         * address of any aggregate-returning function without a body, which
         * leaves every such pointer value naming a shecc-defined target.
         */
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

/* The value of an assignment to a bit-field: the field read back from the unit
 * just written. C11 6.5.16p3 permits that read without requiring it, so mark it
 * removable when nothing uses the value; otherwise "s.flag = 1;" on a volatile
 * record would access the object once more than the program asks.
 */
var_t *reload_assigned_bitfield(block_t *parent,
                                basic_block_t **bb,
                                var_t *address,
                                const var_t *field)
{
    insn_t *before = (*bb)->insn_list.tail;
    basic_block_t *start = *bb;
    var_t *value = read_bitfield_value(parent, bb, address, field);

    /* read_bitfield_value() emits the read first, into the given block. */
    insn_t *read = before ? before->next : start->insn_list.head;
    read->rd->is_assignment_reload = true;
    value->is_bitfield = false;
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
                 opcode_t op);

static bool is_function_parameter(const var_t *var, block_t *parent);

/* A pointer to a whole array: `int (*)[N]`, or `char *(*)[N]` whose elements
 * are themselves pointers. Arithmetic on it steps over complete arrays.
 *
 * Where the pointer is spelled directly, its own stars are the element's plus
 * one, and a pointer typedef element as in `ip (*p)[N]` only deepens the base.
 * Where a typedef such as `int *(*T)[N]` carries the whole declarator, its
 * element depth is relative to the typedef's stars, and any star on the object
 * makes it a pointer to T instead.
 */
static bool is_pointee_array_pointer(const var_t *var)
{
    int element_depth;

    if (!var || !var->type || var->pointee_array_size <= 0)
        return false;
    element_depth = var->pointee_array_element_ptr_level;
    if (var->type->pointee_array_size)
        return !var->ptr_level && var->type->ptr_level == element_depth + 1;
    return var->ptr_level == element_depth + 1;
}

static void copy_pointee_array_shape(var_t *destination, const var_t *source)
{
    fixed_array_shape_t shape = fixed_array_shape_from_pointee_var(source);

    fixed_array_shape_to_pointee_var(destination, &shape);
    destination->pointee_array_element_ptr_level =
        source->pointee_array_element_ptr_level;
}

/* The address of an operand that does not start with an identifier: `&*p`,
 * `&(*q).member`, `&2[arr]` or `&(int){1}`.
 */
static void take_expression_address(block_t *parent, basic_block_t **bb)
{
    var_t *operand;
    var_t *address;
    var_t *result;

    if (lex_accept(T_asterisk)) {
        /* &*E is E, except that it is not an lvalue; neither operator is
         * evaluated (C99 6.5.3.2p3).
         */
        read_expr_operand(parent, bb);
        operand = opstack_pop();
        if (operand->is_func) {
            opstack_push(operand);
            return;
        }
        if (!effective_pointer_depth(operand) && !operand->array_size)
            error_at("Cannot dereference from non-pointer typed variable",
                     cur_token_loc());
        result = require_typed_ptr_var(
            parent, operand->type, operand->ptr_level + !!operand->array_size);
        result->var_name = gen_name();
        result->is_const_qualified = operand->is_const_qualified;
        result->pointer_const_mask = operand->pointer_const_mask;
        result->func_signature = operand->func_signature;
        result->pointee_func_signature = operand->pointee_func_signature;
        result->pointee_array_size = operand->pointee_array_size;
        result->pointee_array_dim2 = operand->pointee_array_dim2;
        result->pointee_array_dim3 = operand->pointee_array_dim3;
        result->pointee_array_dim4 = operand->pointee_array_dim4;
        result->pointee_array_element_ptr_level =
            operand->pointee_array_element_ptr_level;
        add_insn(parent, *bb, OP_assign, result, operand, NULL, 0, NULL);
        opstack_push(result);
        return;
    }

    read_expr_operand(parent, bb);
    operand = opstack_pop();
    if (operand->is_func) {
        /* A function designator, possibly parenthesized, is its address. */
        opstack_push(operand);
        return;
    }
    if (operand->compound_literal_bitfield || operand->is_bitfield)
        error_at("cannot take address of bit-field", cur_token_loc());
    if (operand->is_compound_literal_reference) {
        address = operand->compound_literal_address;
    } else if (is_named_object(operand, parent) ||
               operand->is_compound_literal) {
        if (operand->is_register)
            error_at("cannot take address of register object", cur_token_loc());
        address = operand;
    } else {
        error_at("Address-of requires an lvalue operand", cur_token_loc());
    }
    result =
        require_typed_ptr_var(parent, operand->type, operand->ptr_level + 1);
    result->var_name = gen_name();
    result->is_const_qualified = operand->is_const_qualified;
    if (operand->func_signature)
        result->pointee_func_signature = operand->func_signature;
    add_insn(
        parent, *bb,
        address == operand && !operand->array_size ? OP_address_of : OP_assign,
        result, address, NULL, 0, NULL);
    opstack_push(result);
}

/* Maintain a stack of expression values and operators, depending on next
 * operators' priority. Either apply it or operator on stack first.
 */
void handle_address_of_operator(block_t *parent, basic_block_t **bb)
{
    char token[MAX_VAR_LEN];
    lvalue_t lvalue;
    var_t *vd, *rs1, *var = NULL;

    if (!lex_peek(T_identifier, token) ||
        ((var = find_var(token, parent)) && is_swapped_subscript_base(var))) {
        take_expression_address(parent, bb);
        return;
    }
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
    if (var && var->is_register)
        error_at("cannot take address of register object", next_token_loc());
    read_lvalue(&lvalue, var, parent, bb, false, OP_generic);

    if (is_bitfield(lvalue.decl))
        error_at("cannot take address of bit-field", next_token_loc());

    if (lvalue.subscript_depth && is_pointee_array_pointer(lvalue.decl)) {
        /* p[i] of a pointer to an array designates the array at that index, and
         * p[i][j] one of its rows unless the subscripts reach an element. The
         * subscripts have already computed that address; give it the designated
         * array as its pointee rather than taking the address of the temporary
         * holding it.
         */
        fixed_array_shape_t shape =
            fixed_array_shape_from_pointee_var(lvalue.decl);

        for (int i = 1; i < lvalue.subscript_depth; i++)
            fixed_array_shape_drop_outer(&shape);
        if (shape.rank) {
            rs1 = opstack_pop();
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = lvalue.decl->type;
            vd->ptr_level = lvalue.decl->ptr_level;
            vd->is_const_qualified = lvalue.decl->is_const_qualified;
            copy_pointee_array_shape(vd, lvalue.decl);
            fixed_array_shape_to_pointee_var(vd, &shape);
            add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
            opstack_push(vd);
            return;
        }
    }

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

        /* A parameter declared as an array is a pointer object (C99 6.7.5.3p7),
         * so its address is that of its own slot, one level above the element
         * pointer it holds.
         */
        bool array_parameter = lvalue.decl && !lvalue.subscript_depth &&
                               is_array_declarator(lvalue.decl) &&
                               is_function_parameter(lvalue.decl, parent);

        if (lvalue.decl && is_array_declarator(lvalue.decl) &&
            !array_parameter) {
            /* An array expression already denotes its backing address in the
             * IR. Taking OP_address_of of its variable would instead point at
             * the compiler's local array-base slot, so `&row` passed a pointer
             * to that slot rather than a pointer to row. Keep the same runtime
             * address while adding the source-level pointer indirection
             * required by the address-of operator.
             *
             * The result points to the whole array (C99 6.5.3.2p3), so it
             * carries the array's bounds as its pointee shape, the form a
             * declared `int (*p)[N]` has. Typed as a pointer to the first
             * element instead, `&arr + 1` advanced by a single element.
             */
            if (!lvalue.decl->is_flexible_array_member &&
                !lvalue.decl->pointee_array_size && !lvalue.decl->is_func) {
                fixed_array_shape_t shape =
                    fixed_array_shape_from_var(lvalue.decl);

                for (int i = 0; i < lvalue.subscript_depth; i++)
                    fixed_array_shape_drop_outer(&shape);
                fixed_array_shape_to_pointee_var(vd, &shape);
                vd->pointee_array_element_ptr_level = lvalue.decl->ptr_level;
            }
            add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
        } else {
            if (array_parameter)
                vd->ptr_level++;
            add_insn(parent, *bb, OP_address_of, vd, rs1, NULL, 0, NULL);
        }
    } else if (!operand_stack[operand_stack_idx - 1]->ptr_level) {
        /* A subscript types the address it computes, but a member selection
         * leaves a plain int temporary, since the lvalue is normally loaded
         * next. As the operand of '&' that address is the result: without the
         * member's pointer type, "&s.a + 1" advanced by one byte and "sizeof
         * &s.a" measured an int.
         */
        vd = operand_stack[operand_stack_idx - 1];
        vd->type = lvalue.type;
        vd->ptr_level = lvalue.ptr_level + 1;
    }
}

/* A pointer-to-array dereference designates the addressed row; it does not load
 * an object. Retain that row as an array descriptor so a following grouped
 * postfix subscript can use the ordinary array path. The row may hold pointers,
 * as `*p` does for `int *(*p)[2]`.
 */
static bool lower_pointee_array_dereference(var_t *source,
                                            block_t *parent,
                                            basic_block_t **bb)
{
    type_t *element_type;
    var_t *row;

    if (!is_pointee_array_pointer(source))
        return false;

    element_type = source->type->pointee_array_element_type
                       ? source->type->pointee_array_element_type
                       : source->type;
    row = require_var(parent);
    row->type = element_type;
    fixed_array_shape_t shape = fixed_array_shape_from_pointee_var(source);

    fixed_array_shape_to_var(row, &shape);
    row->ptr_level = source->pointee_array_element_ptr_level;
    row->is_const_qualified =
        source->is_const_qualified || element_type->is_const_qualified;
    row->var_name = gen_name();
    add_insn(parent, *bb, OP_assign, row, source, NULL, 0, NULL);
    opstack_push(row);
    return true;
}

static int pointee_array_row_stride(const var_t *var)
{
    type_t *element_type = var->type->pointee_array_element_type
                               ? var->type->pointee_array_element_type
                               : var->type;

    if (var->pointee_array_element_ptr_level)
        return var->pointee_array_size * PTR_SIZE;
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

static bool pointee_array_shapes_compatible(const var_t *left,
                                            const var_t *right)
{
    type_t *left_element;
    type_t *right_element;

    if (!is_pointee_array_pointer(left) || !is_pointee_array_pointer(right))
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

/* Whether unary `*` on @array reads through the pointer to its first element
 * that the array decays to. An array has no pointer level of its own, and the
 * dereference paths read it as an int-sized word: a record, a row, a pointer or
 * a long long element was wrongly loaded. An array of callbacks keeps its own
 * paths.
 */
static bool decays_when_dereferenced(const var_t *array)
{
    return is_array_declarator(array) && !array->is_func &&
           !array->func_signature && !array->pointee_func_signature;
}

/* The pointer to its first element that the array @array decays to, for a unary
 * `*` to read or store through.
 */
static var_t *decay_dereferenced_array(block_t *parent,
                                       basic_block_t **bb,
                                       var_t *array)
{
    var_t *pointer =
        require_typed_ptr_var(parent, array->type, array->ptr_level + 1);

    if (array->array_dim2) {
        fixed_array_shape_t shape = fixed_array_shape_from_var(array);

        fixed_array_shape_drop_outer(&shape);
        fixed_array_shape_to_pointee_var(pointer, &shape);
        pointer->pointee_array_element_ptr_level = array->ptr_level;
    }
    pointer->var_name = gen_name();
    pointer->is_const_qualified = array->is_const_qualified;
    add_insn(parent, *bb, OP_assign, pointer, array, NULL, 0, NULL);
    return pointer;
}

/* Whether the identifier at the next token is called, as in `*get(1)`, where
 * the dereference applies to the call result rather than to the name.
 */
static bool dereferenced_call_follows(void)
{
    return lex_peek(T_identifier, NULL) && cur_token->next->next &&
           cur_token->next->next->kind == T_open_bracket;
}

/* Dereference the pointer value @rs1 and push the object it designates. */
void push_dereference(block_t *parent, basic_block_t **bb, var_t *rs1)
{
    var_t *vd;
    int sz;

    if (decays_when_dereferenced(rs1))
        rs1 = decay_dereferenced_array(parent, bb, rs1);
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

    if (lower_pointee_array_dereference(rs1, parent, bb))
        return;

    /* For pointer dereference, we need to determine the target type and size.
     * Since we do not have full type tracking in expressions, use defaults
     */
    type_t *deref_type = rs1->type ? rs1->type : TY_int;
    int deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;

    /* require_deref_var() takes the *source* pointer level and returns a
     * variable one level shallower. Passing the already-decremented value
     * dropped two levels per dereference, so "**(q + 0)" on an int** ended up
     * reading an int-sized word where a pointer was stored. The sizes coincide
     * on the 32-bit targets, which is why it only surfaces here.
     */
    vd = require_deref_var(parent, deref_type, rs1->ptr_level);
    sz = deref_ptr > 0
             ? PTR_SIZE
             : pointer_typedef_pointee_size(
                   deref_type, pointee_type_from_pointer_typedef(deref_type));
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
    push_object_at(parent, bb, vd, rs1, sz);
}

void handle_single_dereference(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    int sz;

    if (lex_peek(T_open_bracket, NULL) || lex_peek(T_increment, NULL) ||
        lex_peek(T_decrement, NULL) || lex_peek(T_ampersand, NULL) ||
        dereferenced_call_follows()) {
        /* Handle general expression dereference: *(expr), and a prefix update,
         * which is itself a unary expression: `*++pointer` dereferences its
         * updated result. The group is a whole unary operand, so postfix
         * operators after it apply before the dereference: `*(*q).p` loads
         * through the member, and `*get(1)` through the call result.
         */
        read_expr_operand(parent, bb);
        push_dereference(parent, bb, opstack_pop());
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
            push_object_at(parent, bb, vd, rs1, sz);
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
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic);

        rs1 = opstack_pop();
        if (decays_when_dereferenced(rs1)) {
            push_dereference(parent, bb, rs1);
            return;
        }
        if (var->is_func) {
            /* C99 6.3.2.1 makes a function-pointer dereference a function
             * designator. The pointer object itself therefore needs one load,
             * not a dereference of its return type.
             */
            opstack_push(load_function_pointer_object(parent, bb, rs1));
            return;
        }
        if (lower_pointee_array_dereference(rs1, parent, bb))
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
        push_object_at(parent, bb, vd, rs1, sz);
    }
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
    if (lex_peek(T_open_bracket, NULL) || lex_peek(T_ampersand, NULL) ||
        dereferenced_call_follows()) {
        /* Handle ***(expr) case, with any postfix operators after the group */
        read_expr_operand(parent, bb);

        /* Apply dereferences one by one */
        for (int i = 0; i < deref_count; i++)
            push_dereference(parent, bb, opstack_pop());
    } else {
        /* Handle **pp, ***ppp case with simple identifier */
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        if (!lex_peek(T_identifier, token))
            error_at("Expected an identifier", next_token_loc());
        var_t *var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic);

        /* Apply dereferences one by one */
        for (int i = 0; i < deref_count; i++) {
            rs1 = opstack_pop();
            if (decays_when_dereferenced(rs1)) {
                push_dereference(parent, bb, rs1);
                continue;
            }

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
            push_object_at(parent, bb, vd, rs1, sz);
        }
    }
}

/* Lower a postfix member chain from the record at @address, of @record_type,
 * and push the selected value on the operand stack. The chain starts with `->`
 * when @arrow_first, @address then being the pointer operand, and with `.`
 * otherwise. @is_lvalue says the record designates an object; a chain that
 * follows a pointer member reaches an object in any case. An array member left
 * with dimensions unconsumed decays to a pointer, and a record member is pushed
 * through push_object_at(), as a record element is.
 */
void lower_member_postfix(var_t *address,
                          type_t *record_type,
                          bool arrow_first,
                          bool is_lvalue,
                          block_t *parent,
                          basic_block_t **bb)
{
    var_t *field = NULL;
    var_t *result;
    fixed_array_shape_t shape = {0};
    int depth = 0;

    while (lex_peek(T_dot, NULL) || lex_peek(T_arrow, NULL) ||
           (field && depth < shape.rank && lex_peek(T_open_square, NULL))) {
        char name[MAX_ID_LEN];

        if (lex_accept(T_open_square)) {
            int element_size = field->ptr_level || field->is_func
                                   ? PTR_SIZE
                                   : field->type->size;
            int stride = fixed_array_shape_stride(&shape, depth, element_size);
            var_t *index;

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
                add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL, 0,
                         NULL);
                scaled->var_name = gen_name();
                add_insn(parent, *bb, OP_mul, scaled, index, scale, 0, NULL);
                index = scaled;
            }
            var_t *indexed = require_typed_ptr_var(parent, field->type, 1);
            indexed->var_name = gen_name();
            add_insn(parent, *bb, OP_add, indexed, address, index, 0, NULL);
            address = indexed;
            depth++;
            if (depth == shape.rank && is_record_type(field->type) &&
                !field->ptr_level)
                record_type = field->type;
            continue;
        }

        if (!field) {
            /* The first selection applies to the operand itself. */
            if (!lex_accept(arrow_first ? T_arrow : T_dot))
                error_at(arrow_first ? "Cannot apply dot operator to pointer"
                                     : "Cannot apply arrow operator to record",
                         next_token_loc());
        } else if (lex_accept(T_arrow)) {
            /* Only a selected pointer-to-record member can be followed. */
            if (depth < shape.rank || field->ptr_level != 1 ||
                field->type->ptr_level || field->is_func ||
                !is_record_type(field->type))
                error_at("Invalid record member access", cur_token_loc());
            var_t *pointer = require_typed_ptr_var(parent, field->type, 1);

            pointer->var_name = gen_name();
            add_insn(parent, *bb, OP_read, pointer, address, NULL, PTR_SIZE,
                     NULL);
            address = pointer;
            record_type = field->type;
            is_lvalue = true;
        } else {
            lex_expect(T_dot);
        }
        if (!record_type)
            error_at("Member access requires a record", cur_token_loc());
        lex_ident(T_identifier, name);
        field = find_member(name, record_type);
        if (!field)
            error_at("Unknown struct or union member", cur_token_loc());
        address = compute_field_address(parent, bb, address, field);
        shape = fixed_array_shape_from_var(field);
        depth = 0;
        record_type = is_record_type(field->type) && !field->ptr_level &&
                              !field->array_size
                          ? field->type
                          : NULL;
    }

    if (depth < shape.rank && unevaluated_expression_depth) {
        /* sizeof observes the array itself, before any decay. */
        for (int i = 0; i < depth; i++)
            fixed_array_shape_drop_outer(&shape);
        result = require_typed_ptr_var(parent, field->type, field->ptr_level);
        fixed_array_shape_to_var(result, &shape);
        result->var_name = gen_name();
        opstack_push(result);
    } else if (depth < shape.rank) {
        for (int i = 0; i <= depth; i++)
            fixed_array_shape_drop_outer(&shape);
        result =
            require_typed_ptr_var(parent, field->type, field->ptr_level + 1);
        fixed_array_shape_to_pointee_var(result, &shape);
        result->pointee_array_element_ptr_level = field->ptr_level;
        result->var_name = gen_name();
        add_insn(parent, *bb, OP_assign, result, address, NULL, 0, NULL);
        opstack_push(result);
    } else if (is_bitfield(field)) {
        result = read_bitfield_value(parent, bb, address, field);
        opstack_push(result);
        if (is_lvalue)
            mark_value_reference(result, address, field);
    } else {
        result = require_typed_ptr_var(parent, field->type, field->ptr_level);
        result->var_name = gen_name();
        result->func_signature = field->func_signature;

        /* A function-pointer member is loaded as the pointer value a call
         * consumes, not as an object to be loaded again.
         */
        if (field->func_signature && !result->ptr_level)
            result->ptr_level = 1;
        push_object_at(
            parent, bb, result, address,
            field->ptr_level || field->type->ptr_level || field->is_func
                ? PTR_SIZE
                : field->type->size);
        if (!is_lvalue)
            result->is_compound_literal_reference = false;
    }
    result->is_const_qualified =
        field->is_const_qualified || field->type->is_const_qualified;
    if (!is_lvalue &&
        (lex_peek(T_assign, NULL) || lex_peek(T_increment, NULL) ||
         lex_peek(T_decrement, NULL)))
        error_at("member of a function call result is not assignable",
                 next_token_loc());
}
