/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Function definitions, file-scope declarations, and the parser entry point.
 *
 * A fragment of the parser: parser.c includes it in order, so it sees every
 * definition that precedes it there and cannot be compiled on its own.
 */

/* Emit the optional initializer of a global declarator. Arrays and pointers
 * written with a brace list go through the array initializer; everything else
 * is a scalar constant.
 */
void read_global_init_var(var_t *var, block_t *block)
{
    if (!lex_accept(T_assign))
        return;

    var->has_initializer = true;

    if (lex_peek(T_open_curly, NULL) &&
        (var->array_size > 0 || var->has_unsized_array || var->ptr_level > 0))
        parse_array_init(var, block, &GLOBAL_FUNC->bbs);
    else if (global_compound_literal_starts_here() &&
             !(var->ptr_level || var->type->ptr_level) &&
             is_record_type(var->type))
        parse_global_compound_record_init(var, block);
    else if (global_compound_literal_starts_here() &&
             (var->ptr_level || var->type->ptr_level))
        parse_global_compound_array_init(var, block);
    else if (global_compound_literal_starts_here())
        parse_global_compound_scalar_init(var, block);
    else if (lex_peek(T_open_curly, NULL) && is_record_type(var->type))
        parse_global_record_init(var, block);
    else
        read_global_assignment_var(var);
}

/* The declaration list of an old-style definition, `int f(x, y) int x; char *y;
 * {`, which gives each identifier of the list its type (C99 6.9.1p6). Only
 * register may be named as storage class, and C99 has no implicit int for a
 * parameter the list leaves undeclared.
 */
static void read_identifier_list_declarations(func_t *func)
{
    if (!lex_peek(T_open_curly, NULL) && !lex_peek(T_identifier, NULL) &&
        !lex_peek(T_register, NULL) && !lex_peek(T_const, NULL) &&
        !lex_peek(T_volatile, NULL) && !lex_peek(T_signed, NULL) &&
        !lex_peek(T_unsigned, NULL) && !lex_peek(T_long, NULL) &&
        !lex_peek(T_struct, NULL) && !lex_peek(T_union, NULL) &&
        !lex_peek(T_enum, NULL))
        error_at("identifier list requires a function definition",
                 next_token_loc());

    while (!lex_peek(T_open_curly, NULL)) {
        var_t base = {0};
        bool is_register = lex_accept(T_register);

        base.scope = func->return_def.scope;
        read_full_var_decl(&base, false, true, false);
        for (var_t *decl = &base;;) {
            var_t *param = NULL;

            for (int i = 0; i < func->num_params; i++)
                if (!strcmp(func->param_defs[i].var_name, decl->var_name))
                    param = &func->param_defs[i];
            if (!param)
                error_at("declaration of a name not in the identifier list",
                         cur_token_loc());
            if (param->type)
                error_at("duplicate parameter declaration", cur_token_loc());
            memcpy(param, decl, sizeof(var_t));
            param->is_register = is_register;
            param->is_aggregate_param =
                is_record_type(param->type) && !param->ptr_level;
            if (!lex_accept(T_comma))
                break;
            decl = &base;
            memset(decl, 0, sizeof(var_t));
            decl->scope = func->return_def.scope;
            decl->type = param->type;
            read_inner_var_decl(decl, false, true, false);
        }
        lex_expect(T_semicolon);
    }
    for (int i = 0; i < func->num_params; i++)
        if (!func->param_defs[i].type)
            error_at("parameter type defaults to int, which C99 removed",
                     next_token_loc());
}

/* Whether the old-style parameter @defined agrees with the prototype parameter
 * @declared: the prototype has the type the default argument promotions give
 * the old-style one (C99 6.7.5.3p15).
 */
static bool identifier_list_param_matches(const var_t *defined,
                                          const var_t *declared)
{
    if (parameter_changes_under_default_promotion(defined))
        return !declared->ptr_level && !declared->type->ptr_level &&
               !declared->is_func && declared->type == TY_int;
    return compatible_function_param_decl(defined, declared);
}

static void error_named_declaration(const char *message, const var_t *var)
{
    char diagnostic[MAX_LINE_LEN];

    snprintf(diagnostic, sizeof(diagnostic), "%s '%s'", message,
             file_scope_var_name(var));
    error_at(diagnostic, next_token_loc());
}

static void error_conflicting_function_declaration(const var_t *var)
{
    error_named_declaration("conflicting types for function declaration", var);
}

/* A declarator's base type is already known when this runs. Keeping function
 * completion independent of how that type was spelled lets enum, record, and
 * ordinary scalar declarations share linkage and redeclaration checks.
 */
bool read_global_function_declarator(block_t *block,
                                     var_t *var,
                                     bool is_static,
                                     bool allow_definition)
{
    /* Functions and objects share C's ordinary identifier namespace at file
     * scope. `var` is the provisional declarator for this function, so a
     * different matching global object is a conflict rather than a function
     * redeclaration. Without this check the back end emitted colliding labels
     * and the resulting program could jump through object storage.
     */
    const char *source_name = file_scope_var_name(var);
    var_t *unit_decl = find_tu_declaration(source_name);
    var_t *object =
        unit_decl && !find_func(unit_decl->var_name)
            ? unit_decl
            : (!is_static && !unit_decl ? find_global_var(source_name) : NULL);

    if (find_block_ordinary(CURRENT_TU_SCOPE, var->var_name,
                            ORDINARY_CONSTANT | ORDINARY_TYPEDEF, NULL))
        error_at("function declaration conflicts with an ordinary identifier",
                 next_token_loc());
    if (object && object != var)
        error_at("function declaration conflicts with global object",
                 next_token_loc());

    if (unit_decl && find_func(unit_decl->var_name) && unit_decl->is_static) {
        var->source_name = unit_decl->source_name;
        var->var_name = unit_decl->var_name;
    } else if (is_static && !unit_decl) {
        assign_internal_linkage_name(var, source_name);
    }

    add_tu_global_var(var);

    func_t *func = find_func(var->var_name);
    func_t func_tmp;
    bool check_decl = false;
    bool inherited_direct_function_type =
        var->is_func && var->type->is_direct_function_type;
    func_t *inherited_signature = var->func_signature;

    if (func) {
        memcpy(&func_tmp, func, sizeof(func_t));
        check_decl = true;
    } else {
        func = add_func(var->var_name, false);
    }
    if (!var->is_block_scope_function_declaration)
        func->is_block_scope_only_declaration = false;
    else if (!check_decl)
        func->is_block_scope_only_declaration = true;

    if (inherited_direct_function_type) {
        memcpy(&func->return_def, &inherited_signature->return_def,
               sizeof(var_t));
        func->return_def.var_name = var->var_name;
    } else {
        memcpy(&func->return_def, var, sizeof(var_t));
    }
    func->returns_aggregate = is_record_type(func->return_def.type) &&
                              !has_effective_pointer(&func->return_def);

    /* A typedef can hide an array return type: `typedef int row[2]; row
     * f(void);` has no array suffix after the function declarator, but C99
     * still forbids a function from returning an array. Count pointer depth
     * carried by both the declarator and its typedef, so a typedef-hidden
     * pointer to that array remains a legal return type.
     */
    if (!effective_pointer_depth(&func->return_def) && func->return_def.type &&
        func->return_def.type->array_size)
        error_at("function cannot return an array type", next_token_loc());
    if (check_decl && !func_tmp.is_static && is_static)
        error_at("static declaration follows non-static declaration",
                 next_token_loc());
    func->is_static = check_decl && func_tmp.is_static ? true : is_static;
    func->is_inline = var->is_inline;
    block->locals.size--;

    /* Parse this declarator independently of any earlier declaration. A later
     * `f()` must not inherit stale parameter slots while a later typed list may
     * legitimately refine an earlier unprototyped declaration. A block direct
     * function typedef has already parsed its prototype, so copy that exact
     * syntax-only signature instead of trying to consume another suffix.
     */
    func->num_params = 0;
    func->va_args = 0;
    func->has_prototype = false;
    memset(func->param_defs, 0, sizeof(func->param_defs));
    if (inherited_direct_function_type) {
        func->num_params = inherited_signature->num_params;
        func->va_args = inherited_signature->va_args;
        func->has_prototype = inherited_signature->has_prototype;
        memcpy(func->param_defs, inherited_signature->param_defs,
               sizeof(func->param_defs));
    } else {
        /* Parameter identifiers are optional in declarations. Definitions check
         * for them below before body lowering makes parameter symbols visible.
         */
        read_parameter_list_decl(func, true);
        if (!func->has_prototype && func->num_params)
            read_identifier_list_declarations(func);
    }

    if (check_decl) {
        if (!compatible_decl_type(func->return_def.type,
                                  func_tmp.return_def.type) ||
            func->return_def.ptr_level != func_tmp.return_def.ptr_level ||
            func->return_def.is_const_qualified !=
                func_tmp.return_def.is_const_qualified)
            error_conflicting_function_declaration(var);
        if (func->has_prototype && func_tmp.has_prototype) {
            if (func->num_params != func_tmp.num_params ||
                func->va_args != func_tmp.va_args)
                error_conflicting_function_declaration(var);
            for (int i = 0; i < func->num_params; i++) {
                const var_t *now = &func->param_defs[i];
                const var_t *before = &func_tmp.param_defs[i];
                if (!compatible_function_param_decl(now, before))
                    error_conflicting_function_declaration(var);
            }
        } else if (func->has_prototype && !func_tmp.has_prototype &&
                   func_tmp.num_params) {
            /* A prototype after an old-style definition agrees with its
             * promoted parameter types.
             */
            if (func->num_params != func_tmp.num_params || func->va_args)
                error_conflicting_function_declaration(var);
            for (int i = 0; i < func->num_params; i++)
                if (!identifier_list_param_matches(&func_tmp.param_defs[i],
                                                   &func->param_defs[i]))
                    error_conflicting_function_declaration(var);
        } else if (strict_c99 && func->has_prototype &&
                   !func_tmp.has_prototype) {
            /* A variadic prototype and parameters promoted from char, short, or
             * _Bool cannot be compatible with an earlier empty parameter list
             * declaration. Keep the historical extension outside strict C99
             * mode, where existing old-style sources rely on it.
             */
            if (func->va_args)
                error_conflicting_function_declaration(var);
            for (int i = 0; i < func->num_params; i++)
                if (parameter_changes_under_default_promotion(
                        &func->param_defs[i]))
                    error_conflicting_function_declaration(var);
        } else if (!func->has_prototype && func->num_params &&
                   func_tmp.has_prototype) {
            /* An old-style definition of a function declared with a prototype
             * keeps its own parameter names and that prototype.
             */
            if (func->num_params != func_tmp.num_params || func_tmp.va_args)
                error_conflicting_function_declaration(var);
            for (int i = 0; i < func->num_params; i++)
                if (!identifier_list_param_matches(&func->param_defs[i],
                                                   &func_tmp.param_defs[i]))
                    error_conflicting_function_declaration(var);
            func->has_prototype = true;
        } else if (!func->has_prototype && func_tmp.has_prototype) {
            /* An empty-list definition has no named parameters. It cannot
             * define a function previously declared with fixed parameters or an
             * ellipsis, even though a non-defining `f()` declaration may
             * coexist with that prototype.
             */
            if (lex_peek(T_open_curly, NULL) &&
                (func_tmp.num_params || func_tmp.va_args))
                error_conflicting_function_declaration(var);

            /* A prior prototype remains visible after a compatible `f()`
             * declaration and still constrains subsequent calls.
             */
            memcpy(func->param_defs, func_tmp.param_defs,
                   sizeof(func->param_defs));
            func->num_params = func_tmp.num_params;
            func->va_args = func_tmp.va_args;
            func->has_prototype = true;
        }
    }

    if (lex_peek(T_open_curly, NULL)) {
        /* C99 6.9.1 admits function definitions only as external declarations.
         * Lowering one here would nest its body inside the enclosing function.
         */
        if (var->is_block_scope_function_declaration)
            error_at("function definition is not allowed at block scope",
                     next_token_loc());
        if (!allow_definition)
            error_at("function definition must be the only declarator",
                     next_token_loc());
        if (inherited_direct_function_type &&
            !var->is_direct_function_declarator)
            error_at("function definition cannot take its type from a typedef",
                     next_token_loc());
        if (check_decl && func_tmp.bbs)
            error_named_declaration("redefinition of function", var);
        if (is_incomplete_record_object(&func->return_def))
            error_at("function definition cannot return incomplete record type",
                     next_token_loc());
        for (int i = 0; i < func->num_params; i++)
            if (!func->param_defs[i].var_name ||
                !func->param_defs[i].var_name[0])
                error_at("function definition parameter requires an identifier",
                         next_token_loc());
            else if (!func->param_defs[i].array_size &&
                     !func->param_defs[i].has_unsized_array &&
                     is_incomplete_record_object(&func->param_defs[i]))
                error_at("Incomplete struct/union type cannot define an object",
                         next_token_loc());
        read_func_body(func);
        return true;
    }

    /* A prototype may be followed by further declarators of either kind, as in
     * "int f(void), g(int), value;". Leave the comma to the list reader.
     */
    if (lex_peek(T_comma, NULL))
        return false;
    if (!lex_accept(T_semicolon))
        error_at("Syntax error in global declaration", next_token_loc());
    return true;
}

/* A compatible repeated file-scope declaration names the same object. The
 * parser creates a provisional var_t while reading its declarator, so discard
 * that entry before emitting allocation or initializer IR and keep the first
 * declaration's storage.
 */
var_t *resolve_global_declarator(block_t *block,
                                 var_t *var,
                                 bool is_static,
                                 bool *is_redeclaration)
{
    const char *source_name = file_scope_var_name(var);
    var_t *unit_decl = find_tu_declaration(source_name);
    var_t *previous = NULL;

    *is_redeclaration = false;

    if (find_block_ordinary(CURRENT_TU_SCOPE, var->var_name,
                            ORDINARY_CONSTANT | ORDINARY_TYPEDEF, NULL))
        error_at(
            "global object declaration conflicts with an ordinary identifier",
            next_token_loc());

    /* The ordinary identifier namespace is shared with functions. This is
     * intentionally before object redeclaration handling: a function is not a
     * compatible tentative definition of an object, even when both happen to
     * have the same declared scalar type.
     */
    if ((unit_decl && find_func(unit_decl->var_name)) ||
        (!is_static && !unit_decl && find_func(var->var_name)))
        error_at("global object declaration conflicts with function",
                 next_token_loc());

    if (unit_decl && unit_decl->is_static) {
        var->source_name = unit_decl->source_name;
        var->var_name = unit_decl->var_name;
    } else if (is_static && !unit_decl) {
        assign_internal_linkage_name(var, source_name);
    }

    for (int i = 0; i + 1 < block->locals.size; i++) {
        var_t *candidate = block->locals.elements[i];
        if (!strcmp(candidate->var_name, var->var_name)) {
            previous = candidate;
            break;
        }
    }
    if (!previous) {
        add_tu_global_var(var);
        return var;
    }

    *is_redeclaration = true;

    /* An array whose outer bound is omitted is compatible with any bound on the
     * same element type (C99 6.7.5.2p6). Inner bounds must still agree; the
     * outer product then follows from them, so compare it only when both
     * declarations spell it.
     */
    bool previous_is_array =
        previous->array_size > 0 || previous->has_unsized_array;
    bool var_is_array = var->array_size > 0 || var->has_unsized_array;
    bool either_unsized = previous->has_unsized_array || var->has_unsized_array;

    if (!compatible_decl_type(previous->type, var->type) ||
        (!!previous->pointee_func_signature != !!var->pointee_func_signature) ||
        (previous->pointee_func_signature &&
         !compatible_function_signature(previous->pointee_func_signature,
                                        var->pointee_func_signature)) ||
        previous->ptr_level != var->ptr_level ||
        previous_is_array != var_is_array ||
        (!either_unsized && previous->array_size != var->array_size) ||
        previous->array_dim2 != var->array_dim2 ||
        previous->array_dim3 != var->array_dim3 ||
        previous->array_dim4 != var->array_dim4 ||
        previous->pointee_array_size != var->pointee_array_size ||
        previous->pointee_array_dim2 != var->pointee_array_dim2 ||
        previous->pointee_array_dim3 != var->pointee_array_dim3 ||
        previous->pointee_array_dim4 != var->pointee_array_dim4 ||
        previous->is_const_qualified != var->is_const_qualified ||
        previous->is_volatile != var->is_volatile)
        error_named_declaration("conflicting types for global declaration",
                                var);
    if (!previous->is_static && is_static)
        error_at("static declaration follows non-static declaration",
                 next_token_loc());
    if (lex_peek(T_assign, NULL) && previous->has_initializer)
        error_named_declaration("redefinition of global variable", var);

    /* A later bound completes the shared object's type. */
    if (previous->has_unsized_array && !var->has_unsized_array) {
        previous->array_size = var->array_size;
        previous->has_unsized_array = false;
    }

    /* Scalar declarators were placed on the operand stack by
     * read_inner_var_decl(). Its later initializer lowering pops that entry, so
     * point it at the shared object rather than the discarded declaration.
     */
    if (operand_stack_idx && operand_stack[operand_stack_idx - 1] == var)
        operand_stack[operand_stack_idx - 1] = previous;
    block->locals.size--;
    add_tu_global_var(previous);
    return previous;
}

static var_t *prepare_global_object_definition(block_t *block,
                                               var_t *var,
                                               bool is_static)
{
    bool is_redeclaration;
    bool is_definition = !var->is_extern;

    if (is_definition && is_incomplete_record_object(var))
        error_at("Incomplete struct/union type cannot define an object",
                 cur_token_loc());
    var = resolve_global_declarator(block, var, is_static, &is_redeclaration);
    if (is_definition && (!is_redeclaration || var->is_extern)) {
        var->is_extern = false;
        add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0, NULL);
    }
    return var;
}

static var_t *read_global_declarator_start(block_t *block,
                                           type_t *decl_type,
                                           bool is_const,
                                           bool is_static,
                                           bool is_volatile,
                                           bool is_extern,
                                           bool is_inline,
                                           bool full)
{
    var_t *var =
        full ? require_var(block) : require_typed_var(block, decl_type);

    var->is_global = true;
    var->is_static = is_static;
    var->is_inline = is_inline;
    var->is_const_qualified = is_const;
    var->is_volatile = is_volatile;
    if (full)
        read_full_var_decl(var, false, false, false);
    else
        read_inner_var_decl(var, false, false, false);
    var->is_extern = is_extern && !lex_peek(T_assign, NULL);
    return var;
}

/* Read one declarator after the first in a global declaration. Each shares the
 * declaration's base type: "int a = 1, b, c = 3;".
 */
bool read_global_declarator(block_t *block,
                            type_t *decl_type,
                            bool is_const,
                            bool is_static,
                            bool is_volatile,
                            bool is_extern,
                            bool allow_definition)
{
    var_t *nv =
        read_global_declarator_start(block, decl_type, is_const, is_static,
                                     is_volatile, is_extern, false, false);
    if (lex_peek(T_open_bracket, NULL) ||
        (nv->is_func && nv->type->is_direct_function_type))
        return read_global_function_declarator(block, nv, is_static,
                                               allow_definition);
    nv = prepare_global_object_definition(block, nv, is_static);
    read_global_init_var(nv, block);
    discard_global_declarator_operand(nv);
    return false;
}

/* Lower a scalar record initializer into the global initializer block. Global
 * array elements already use parse_struct_field_init(); scalar records need the
 * same field-address writes rather than merely consuming their braces.
 */
void parse_global_record_init(var_t *var, block_t *block)
{
    type_t *record_type = var->type;
    block_t *saved_initializer_scope = global_constant_initializer_scope;
    if (var->scope)
        global_constant_initializer_scope = var->scope;
    if (record_type->base_type == TYPE_typedef && record_type->base_struct)
        record_type = record_type->base_struct;

    lex_expect(T_open_curly);
    parse_struct_field_init(block, &GLOBAL_FUNC->bbs, record_type, var);
    lex_expect(T_close_curly);
    global_constant_initializer_scope = saved_initializer_scope;
}

/* At file scope a compound literal has static storage duration. The target
 * object is already global, so a record compound literal can use its normal
 * constant aggregate lowering after consuming the spelled type name.
 */
void parse_global_compound_record_init(var_t *var, block_t *block)
{
    char type_name[MAX_ID_LEN];
    type_t *compound_type, *target_type;

    lex_expect(T_open_bracket);
    base_type_t record_kind = accept_record_keyword();
    lex_ident(T_identifier, type_name);
    lex_expect(T_close_bracket);

    compound_type = record_kind
                        ? find_record_tag(type_name, var->scope, record_kind)
                        : find_visible_type(type_name, var->scope);
    target_type = var->type;
    if (target_type->base_type == TYPE_typedef && target_type->base_struct)
        target_type = target_type->base_struct;
    if (compound_type && compound_type->base_type == TYPE_typedef &&
        compound_type->base_struct)
        compound_type = compound_type->base_struct;
    if (!compound_type || !is_record_type(compound_type) ||
        compound_type != target_type)
        error_at("Incompatible record compound literal", cur_token_loc());

    if (!lex_peek(T_open_curly, NULL))
        error_at("Record compound literal needs an initializer",
                 next_token_loc());
    parse_global_record_init(var, block);
}

/* A scalar compound literal at file scope also has static storage duration. Its
 * sole initializer is the target object's constant initializer, so no temporary
 * storage is necessary after validating the spelled scalar type.
 */
void parse_global_compound_scalar_init(var_t *var, block_t *block)
{
    type_t *compound_type;

    UNUSED(block);

    /* The type name may be spelled with keywords, as in `(unsigned long)`. */
    lex_expect(T_open_bracket);
    compound_type =
        read_type_name_specifiers(var->scope ? var->scope : GLOBAL_BLOCK);
    lex_expect(T_close_bracket);

    if (!compound_type || is_record_type(compound_type) || var->ptr_level ||
        compound_type != var->type)
        error_at("Incompatible scalar compound literal", cur_token_loc());

    lex_expect(T_open_curly);
    if (lex_peek(T_close_curly, NULL))
        error_at("Scalar compound literal needs an initializer",
                 next_token_loc());
    read_global_assignment_var(var);
    if (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL))
        error_at("Too many elements in scalar compound literal",
                 next_token_loc());
    lex_expect(T_close_curly);
}

/* An array compound literal at file scope is an unnamed static array. Keep that
 * array in the global initializer block so its backing storage survives for the
 * full program, then initialize the declared pointer with its base.
 */
void parse_global_compound_array_init(var_t *var, block_t *block)
{
    type_t *element_type;
    var_t *array;
    int element_ptr_level = 0;
    paren_type_name_t shape = {0};
    bool parenthesized_array;

    /* The element type may be qualified or spelled with several keywords, as in
     * `(const unsigned char[])`.
     */
    lex_expect(T_open_bracket);
    element_type =
        read_type_name_specifiers(var->scope ? var->scope : GLOBAL_BLOCK);
    while (lex_accept(T_asterisk)) {
        element_ptr_level++;
        skip_type_qualifiers();
    }

    parenthesized_array = read_compound_array_shape(
                              GLOBAL_BLOCK, element_ptr_level, true, &shape) &&
                          shape.parenthesized_array;
    array = name_var(require_typed_var(GLOBAL_BLOCK, element_type));
    array->is_global = true;
    array->ptr_level =
        parenthesized_array ? shape.array_element_ptr_level : element_ptr_level;
    array->array_size = shape.array_size;
    array->array_dim2 = shape.array_dim2;
    array->array_dim3 = shape.array_dim3;
    array->array_dim4 = shape.array_dim4;
    array->has_unsized_array = shape.array_outer_unsized;
    if (parenthesized_array) {
        array->pointee_array_size = shape.pointee_array_size;
        array->pointee_array_dim2 = shape.pointee_array_dim2;
        array->pointee_array_dim3 = shape.pointee_array_dim3;
        array->pointee_array_dim4 = shape.pointee_array_dim4;
        lex_expect(T_close_bracket);

        if (!element_type || element_type != var->type ||
            var->ptr_level != array->ptr_level + 1 ||
            var->pointee_array_size != array->pointee_array_size ||
            var->pointee_array_dim2 != array->pointee_array_dim2 ||
            var->pointee_array_dim3 != array->pointee_array_dim3 ||
            var->pointee_array_dim4 != array->pointee_array_dim4)
            error_at("Incompatible array compound literal", cur_token_loc());
    } else {
        lex_expect(T_close_bracket);

        /* The backing array has an outer compound-literal bound and the typedef
         * element's inner row shape. parse_array_init() keeps that shape on
         * var_t rather than type_t.
         */
        if (element_type && element_type->array_size) {
            int trailing = fixed_array_trailing_count(element_type->array_dim2,
                                                      element_type->array_dim3,
                                                      element_type->array_dim4);
            if (element_type->array_dim4)
                error_at(
                    "Array compound literal supports at most four dimensions",
                    cur_token_loc());
            array->array_dim2 = element_type->array_size / trailing;
            array->array_dim3 = element_type->array_dim2;
            array->array_dim4 = element_type->array_dim3;
            if (array->array_size)
                array->array_size *= element_type->array_size;
        }

        if (!element_type || element_type != var->type ||
            element_ptr_level + 1 != var->ptr_level ||
            var->pointee_array_size != array->array_dim2)
            error_at("Incompatible array compound literal", cur_token_loc());
    }
    if (!lex_peek(T_open_curly, NULL))
        error_at("Array compound literal needs an initializer",
                 next_token_loc());

    add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_allocat, array, NULL, NULL, 0,
             NULL);
    parse_array_init(array, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs);
    add_insn(block, GLOBAL_FUNC->bbs, OP_assign, var, array, NULL, 0, NULL);
}

/* Read the declarators of a file-scope declaration through its terminating
 * semicolon, or through the body of a function definition. Every declarator
 * shares the base type, and object and function declarators may be mixed, as in
 * "int f(void), g(int), value;"; a definition must stand alone. The caller has
 * consumed a record or enum specifier, which qualifiers may still follow, as in
 * "struct S volatile s;".
 */
void read_global_declarator_list(block_t *block,
                                 type_t *decl_type,
                                 bool is_const,
                                 bool is_static,
                                 bool is_volatile,
                                 bool is_extern)
{
    bool first = true;

    read_type_qualifiers(&is_const, &is_volatile, false);
    do {
        if (read_global_declarator(block, decl_type, is_const, is_static,
                                   is_volatile, is_extern, first))
            return;
        first = false;
    } while (lex_accept(T_comma));
    lex_expect(T_semicolon);
}

void read_global_decl(block_t *block,
                      bool is_const,
                      bool is_static,
                      bool is_extern,
                      bool is_inline,
                      bool is_volatile)
{
    var_t *var =
        read_global_declarator_start(block, NULL, is_const, is_static,
                                     is_volatile, is_extern, is_inline, true);

    /* `unary_t f;` with a function typedef declares the function f. */
    if (lex_peek(T_open_bracket, NULL) ||
        (var->is_func && var->type->is_direct_function_type)) {
        if (read_global_function_declarator(block, var, is_static, true))
            return;
    } else {
        if (var->is_inline)
            error_at("inline specifier requires a function declarator",
                     next_token_loc());
        var = prepare_global_object_definition(block, var, is_static);

        /* is a variable */
        if (lex_peek(T_assign, NULL)) {
            read_global_init_var(var, block);
        } else if (lex_peek(T_semicolon, NULL)) {
        } else if (!lex_peek(T_comma, NULL)) {
            error_at("Syntax error in global declaration", next_token_loc());
        }
        discard_global_declarator_operand(var);
    }

    /* Continuation: "int a = 1, b, c = 3;" or "int f(void), g(int);". Every
     * declarator after the first shares this declaration's base type and is
     * handled exactly like the first, mirroring the struct-tagged global path.
     */
    while (lex_accept(T_comma))
        if (read_global_declarator(block, var->type, var->is_const_qualified,
                                   is_static, var->is_volatile, is_extern,
                                   false))
            return;

    lex_expect(T_semicolon);
}

void initialize_struct_field(var_t *nv, var_t *v, int offset)
{
    *nv = (var_t) {.type = v->type, .var_name = "", .offset = offset};
}

/* The scalar or typedef-name specifier of a file-scope typedef, and any
 * qualifiers mixed in among its keywords, which set @typedef_const and
 * @typedef_volatile.
 *
 * Returns the specified type.
 */
static const type_t *read_global_typedef_base(bool *typedef_const,
                                              bool *typedef_volatile)
{
    char base_type[MAX_ID_LEN];
    const type_t *base =
        read_scalar_type_specifiers(typedef_const, typedef_volatile, NULL);

    if (!base) {
        lex_ident(T_identifier, base_type);
        base = find_visible_type(base_type, CURRENT_TU_SCOPE);
    }
    if (!base)
        error_at("Unable to find base type", cur_token_loc());
    return base;
}

/* One declarator of a file-scope typedef whose specifier resolved to @base,
 * qualified by @typedef_const and @typedef_volatile: a scalar, pointer, array
 * or function alias, or one of an enum or record.
 */
static void read_global_typedef_declarator(block_t *block,
                                           bool typedef_const,
                                           bool typedef_volatile,
                                           const type_t *base)
{
    type_t *type = add_type();

    type->base_type = base->base_type;
    type->size = base->size;

    /* A typedef of a record typedef remains a record type. Sharing its
     * immutable member table preserves ordinary `alias.member` and
     * `pointer_alias->member` lookup instead of turning the alias into a scalar
     * descriptor with no fields.
     */
    type->fields = base->fields;
    type->num_fields = base->num_fields;
    type->base_struct = base->base_struct;
    if (base->base_type == TYPE_typedef && base->num_fields &&
        !type->base_struct)
        type->base_struct = (type_t *) base;
    type->alignment = base->alignment;
    type->is_union = base->is_union;
    type->has_flexible_array_member = base->has_flexible_array_member;
    type->ptr_level = base->ptr_level;
    type->pointer_const_mask = base->pointer_const_mask;
    type->pointer_volatile_mask = base->pointer_volatile_mask;
    type->is_const_qualified = typedef_const || base->is_const_qualified;
    type->is_volatile_qualified = base->is_volatile_qualified;
    if (typedef_volatile) {
        int depth = base->ptr_level + (base->func_signature &&
                                       !base->is_direct_function_type &&
                                       !base->ptr_level);
        if (depth && depth <= 32)
            type->pointer_volatile_mask |= 1U << (depth - 1);
        else
            type->is_volatile_qualified = true;
    }

    /* `const ptr_t` qualifies the pointer that the base typedef hides, not its
     * pointee.
     */
    if (typedef_const && base->ptr_level && base->ptr_level <= 32 &&
        !base->func_signature && !base->pointee_func_signature) {
        type->is_const_qualified = base->is_const_qualified;
        type->pointer_const_mask |= 1U << (base->ptr_level - 1);
    }
    type->is_unsigned = base->is_unsigned;
    type->is_floating = base->is_floating;
    type->is_signed_char = base->is_signed_char;
    type->is_bool = base->is_bool;
    type->array_size = base->array_size;
    type->array_dim2 = base->array_dim2;
    type->array_dim3 = base->array_dim3;
    type->array_dim4 = base->array_dim4;
    type->array_element_ptr_level = base->array_element_ptr_level;
    type->array_element_type = base->array_element_type;
    type->func_signature = base->func_signature;

    /* A tag is not a typedef name. As for `typedef struct S alias`, the alias
     * reaches the record through base_struct, which also sees a later
     * completion of the tag.
     */
    if (base->base_type == TYPE_struct || base->base_type == TYPE_union) {
        type->base_type = TYPE_typedef;
        type->base_struct = (type_t *) base;
        type->is_union = base->base_type == TYPE_union;
    }

    /* Handle pointer types in typedef: typedef char *string; */
    unsigned int star_const_mask = 0;

    while (lex_accept(T_asterisk)) {
        type->ptr_level++;
        type->size = PTR_SIZE;
        while (true) {
            if (lex_accept(T_const)) {
                if (type->ptr_level <= 32) {
                    type->pointer_const_mask |= 1U << (type->ptr_level - 1);
                    star_const_mask |= 1U << (type->ptr_level - 1);
                }
            } else if (lex_accept(T_volatile)) {
                if (type->ptr_level <= 32)
                    type->pointer_volatile_mask |= 1U << (type->ptr_level - 1);
            } else if (lex_accept(T_restrict)) {
                ;
            } else
                break;
        }
    }

    if (type->ptr_level == 1)
        alias_callback_array_pointer(type, base, star_const_mask);

    /* A parenthesized declarator is the function-pointer form: `typedef int
     * (*callback_t)(int)`. Parse it through the normal declarator reader so its
     * prototype has exactly the same shape as an object declaration, then
     * retain that syntax-only signature on the alias for each later object or
     * parameter declaration.
     */
    if (lex_peek(T_open_bracket, NULL)) {
        var_t declarator = {0};
        bool saved_sizeof_signature = parsing_sizeof_function_signature;

        init_type_name_decl(&declarator, (type_t *) base, typedef_const,
                            typedef_volatile);
        declarator.scope = block;
        declarator.ptr_level = type->ptr_level - base->ptr_level;
        declarator.pointer_const_mask =
            base->ptr_level < 32 ? type->pointer_const_mask >> base->ptr_level
                                 : 0;
        declarator.pointer_volatile_mask = type->pointer_volatile_mask;
        /* A returned compact callback keeps its own qualifiers on base. */
        if (base->func_signature && !base->is_direct_function_type &&
            !type->ptr_level) {
            declarator.pointer_const_mask = 0;
            declarator.pointer_volatile_mask = 0;
        }
        declarator.is_const_qualified = type->is_const_qualified;
        declarator.is_volatile = type->is_volatile_qualified;

        /* A callback typedef carries only a function signature. Its floating
         * parameters do not materialize values until a call, which remains
         * rejected by the ordinary floating gates.
         */
        parsing_sizeof_function_signature = true;
        read_inner_var_decl(&declarator, false, false, false);
        parsing_sizeof_function_signature = saved_sizeof_signature;

        /* Without a parameter list or an array suffix the parentheses only
         * group pointers: `typedef int (*int_ptr)` is `typedef int *int_ptr`.
         */
        if (!declarator.is_func && !declarator.array_size &&
            !declarator.has_unsized_array && !declarator.pointee_array_size &&
            !base->func_signature && !base->array_size) {
            strncpy(type->type_name, declarator.var_name, MAX_TYPE_LEN - 1);
            type->type_name[MAX_TYPE_LEN - 1] = '\0';
            type->ptr_level = effective_pointer_depth(&declarator);
            type->size = PTR_SIZE;
            if (base->ptr_level < 32)
                type->pointer_const_mask |= declarator.pointer_const_mask
                                            << base->ptr_level;
            type->pointer_volatile_mask |= declarator.pointer_volatile_mask;
            if (declarator.is_volatile)
                type->is_volatile_qualified = true;

            /* `typedef int (**slot_t)(int)` names a callback slot. */
            type->pointee_func_signature = declarator.pointee_func_signature;
            add_tu_typedef(type);
            return;
        }

        /* `typedef int (*row_ptr)[2]` points to a whole row. As a block-scope
         * alias does, keep the pointer-sized descriptor and carry the row
         * bounds and element separately. The spelled `int (*(*rows_t)[2])(int)`
         * reads its row against the unnamed callback type, which the declarator
         * then carries.
         */
        if (declarator.type && declarator.type->func_signature &&
            !declarator.type->is_direct_function_type)
            base = declarator.type;
        if (!declarator.is_func &&
            declarator.has_direct_pointee_array_declarator &&
            !declarator.array_size && !base->array_size &&
            (!base->func_signature ||
             (!base->is_direct_function_type && !base->ptr_level)) &&
            !base->pointee_func_signature &&
            effective_pointer_depth(&declarator) ==
                declarator.pointee_array_element_ptr_level + base->ptr_level +
                    1 &&
            declarator.pointee_array_element_ptr_level + base->ptr_level <= 1) {
            strncpy(type->type_name, declarator.var_name, MAX_TYPE_LEN - 1);
            type->type_name[MAX_TYPE_LEN - 1] = '\0';

            /* A callback typedef base, `fn_t (*rows_t)[2]`, is the row's
             * element with its prototype and qualifiers; the row pointer itself
             * is no callback.
             */
            if (base->func_signature)
                alias_callback_row_pointer(type, base, 0);

            /* A pointer typedef base, `ptr_t (*rows_t)[2]`, is spelled out as
             * `int *(*rows_t)[2]`: the declarator already counts its stars on
             * the row's element, which then points to the base's pointee.
             */
            type->ptr_level = effective_pointer_depth(&declarator);
            type->size = PTR_SIZE;
            if (base->ptr_level < 32)
                type->pointer_const_mask |= declarator.pointer_const_mask
                                            << base->ptr_level;
            type->pointer_volatile_mask |= declarator.pointer_volatile_mask;
            declarator.pointee_array_element_ptr_level += base->ptr_level;
            copy_pointee_array_shape_to_type(type, &declarator);
            type->pointee_array_element_type =
                base->ptr_level
                    ? pointee_type_from_pointer_typedef((type_t *) base)
                    : (type_t *) base;
            add_tu_typedef(type);
            return;
        }
        if (!declarator.is_func)
            error_at(
                "Typedef parenthesized declarator must be a function "
                "pointer",
                cur_token_loc());

        /* `typedef int (*get_t(void))(int)` names a function type whose return
         * is a callback, which the declarator reader already built.
         */
        if (declarator.is_direct_function_declarator) {
            memcpy(type, declarator.type, sizeof(type_t));
            strncpy(type->type_name, declarator.var_name, MAX_TYPE_LEN - 1);
            type->type_name[MAX_TYPE_LEN - 1] = '\0';
            add_tu_typedef(type);
            return;
        }
        strncpy(type->type_name, declarator.var_name, MAX_TYPE_LEN - 1);
        type->type_name[MAX_TYPE_LEN - 1] = '\0';
        type->size = PTR_SIZE;
        type->alignment = PTR_SIZE;
        type->func_signature = declarator.func_signature;

        /* `typedef int (*row[2])(int)` is an array typedef whose elements are
         * callback pointers. The signature describes each element, while the
         * bounds are needed later when a pointer-to-row is indexed (including
         * after a call result).
         */
        type->array_size = declarator.array_size;
        type->array_dim2 = declarator.array_dim2;
        type->array_dim3 = declarator.array_dim3;
        type->array_dim4 = declarator.array_dim4;
        type->array_element_ptr_level = declarator.array_size ? 1 : 0;

        /* Stars before the parenthesized callback declarator belong to the
         * callback's return type. That depth is retained in its parsed
         * signature; the typedef alias itself is the pointer-sized callback
         * object, not a derived pointer alias.
         */
        int return_depth = declarator.ptr_level + declarator.type->ptr_level;
        type->ptr_level = 0;
        type->pointer_const_mask =
            declarator.ptr_level < 32
                ? declarator.pointer_const_mask >> declarator.ptr_level
                : 0;
        type->pointer_volatile_mask =
            return_depth < 32 ? declarator.pointer_volatile_mask >> return_depth
                              : 0;
        type->is_const_qualified = false;
        type->is_volatile_qualified = false;
        add_tu_typedef(type);
        return;
    }

    lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);

    /* `typedef int unary_t(int)` names a function type. Mirror the block-scope
     * direct function alias: its scalar or void base is only the return type,
     * and the prototype stays on the descriptor.
     */
    if (lex_peek(T_open_bracket, NULL) && !base->ptr_level &&
        !base->array_size && !base->func_signature && !is_record_type(base) &&
        !base->is_floating) {
        func_t *func = arena_alloc_func();

        /* The stars of `char *name_t(void)` belong to the return type. */
        func->return_def.type = (type_t *) base;
        func->return_def.ptr_level = type->ptr_level;
        func->return_def.pointer_const_mask = type->pointer_const_mask;
        func->return_def.pointer_volatile_mask = type->pointer_volatile_mask;
        func->return_def.scope = block;
        type->ptr_level = 0;
        type->pointer_const_mask = 0;
        type->pointer_volatile_mask = 0;
        type->size = base->size;
        read_parameter_list_decl(func, true);
        type->base_type = TYPE_typedef;
        type->func_signature = func;
        type->is_direct_function_type = true;
        add_tu_typedef(type);
        return;
    }

    /* A typedef declarator may wrap an existing array typedef: `typedef row
     * matrix[2]`. Gather its leading bounds first, then prepend them to the
     * base alias's bounds instead of overwriting the inner extent.
     */
    fixed_array_shape_t base_shape = fixed_array_shape_from_type(type);
    fixed_array_shape_t decl_shape = {0};
    read_array_shape_bounds(
        block, &decl_shape, "Array declarators support at most four dimensions",
        "Typedef array needs a positive bound",
        "Typedef array needs a positive bound", true, false);
    if (decl_shape.rank) {
        fixed_array_shape_t shape =
            fixed_array_shape_prepend(&decl_shape, &base_shape);

        if (!type->ptr_level && !type->size && type->base_struct &&
            !type->base_struct->size)
            error_at("Typedef array element has incomplete record type",
                     cur_token_loc());

        fixed_array_shape_to_type(type, &shape);

        /* At the point an array typedef is introduced, ptr_level describes each
         * array element. A later alias may add a pointer to the whole array, so
         * preserve this separately.
         */
        type->array_element_ptr_level = type->ptr_level;
        type->array_element_type =
            base->array_element_type
                ? base->array_element_type
                : (base->ptr_level
                       ? pointee_type_from_pointer_typedef((type_t *) base)
                       : (type_t *) base);
    }
    add_tu_typedef(type);
}

/* The declarator list of a file-scope typedef whose specifier resolved to
 * @base, qualified by @typedef_const and @typedef_volatile, through its
 * terminating semicolon. The specifier and its qualifiers apply to each
 * declarator.
 */
static void read_global_typedef_declarators(block_t *block,
                                            bool typedef_const,
                                            bool typedef_volatile,
                                            const type_t *base)
{
    do {
        read_global_typedef_declarator(block, typedef_const, typedef_volatile,
                                       base);
    } while (lex_accept(T_comma));
    lex_expect(T_semicolon);
}

/* The record that a later declarator of a file-scope record typedef derives
 * from: the @first alias when it names the record itself, the record a pointer
 * alias reaches through base_struct, or else a nameless copy of that alias
 * without its pointer, @size bytes wide and aligned to @alignment.
 */
static const type_t *global_record_typedef_base(type_t *first,
                                                int size,
                                                int alignment)
{
    if (!first->ptr_level)
        return first;
    if (first->base_struct)
        return first->base_struct;

    type_t *record = add_type();

    memcpy(record, first, sizeof(type_t));
    record->type_name[0] = '\0';
    record->ptr_level = 0;
    record->pointer_const_mask = 0;
    record->size = size;
    record->alignment = alignment;
    return record;
}

/* Whether the first declarator of a file-scope record typedef is only stars and
 * a name, which the record branch below completes in place. Any other form,
 * such as `typedef struct P rows[2]`, derives from the record through
 * read_global_typedef_declarator() instead.
 */
static bool global_record_typedef_declarator_is_plain(void)
{
    token_t *token = cur_token->next;

    while (token && token->kind == T_asterisk)
        token = token->next;
    return token && token->kind == T_identifier && token->next &&
           token->next->kind != T_open_square &&
           token->next->kind != T_open_bracket;
}

/* A file-scope typedef, after its keyword: record, enum, callback pointer and
 * scalar aliases, each with a comma-separated declarator list.
 */
static void read_global_typedef(block_t *block)
{
    char token[MAX_ID_LEN];
    bool typedef_const = false;
    bool typedef_volatile = false;

    /* Qualifiers may lead the type specifier or follow it. Each branch reads
     * the trailing ones after its specifier and records both on the alias,
     * which every declaration spelled with it then inherits.
     */
    read_type_qualifiers(&typedef_const, &typedef_volatile, false);
    if (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL)) {
        base_type_t kind = accept_record_keyword();
        bool has_tag = lex_peek(T_identifier, token);
        type_t *type = add_type();
        type_t *tag = NULL;
        type_t *record = NULL;

        if (has_tag)
            lex_expect(T_identifier);

        /* A member list completes the tag, or an untagged record, which the
         * first declarator then names; a bare tag may still be incomplete.
         */
        if (lex_peek(T_open_curly, NULL))
            record = read_record_body(NULL, kind, has_tag, token);
        else if (has_tag)
            tag = local_record_tag(token, CURRENT_TU_SCOPE, kind);

        read_type_qualifiers(&typedef_const, &typedef_volatile, false);
        bool is_plain = global_record_typedef_declarator_is_plain();
        while (is_plain && lex_accept(T_asterisk))
            type->ptr_level++;
        if (is_plain)
            lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);

        /* A defining alias carries the layout itself; a forward one reaches the
         * tag through base_struct, which in 'find_type' also sees a later
         * completion of it.
         */
        int size = record ? record->size : 0;
        int alignment = record ? record->alignment : 1;

        type->alignment = type->ptr_level ? PTR_SIZE : alignment;
        type->size = type->ptr_level ? PTR_SIZE : size;
        if (record) {
            type->fields = record->fields;
            type->num_fields = record->num_fields;
            type->has_flexible_array_member = record->has_flexible_array_member;
            if (has_tag)
                tag = record;

            /* A pointer alias also reaches its record through base_struct, as a
             * forward one does, so that a dereference such as sizeof(*p) yields
             * the record even when it has no tag to be found by.
             */
            if (type->ptr_level)
                type->base_struct = record;
        } else
            type->base_struct = tag;
        type->base_type = TYPE_typedef;
        type->is_union = kind == TYPE_union;

        /* Only the alias is qualified; the tag must stay plain. */
        type->is_const_qualified = typedef_const;
        type->is_volatile_qualified = typedef_volatile;
        add_tu_typedef(type);

        if (!is_plain) {
            read_global_typedef_declarators(block, typedef_const,
                                            typedef_volatile, tag ? tag : type);
        } else if (lex_accept(T_comma)) {
            /* Later declarators derive from the record as a typedef of it
             * would.
             */
            read_global_typedef_declarators(
                block, typedef_const, typedef_volatile,
                global_record_typedef_base(type, size, alignment));
        } else
            lex_expect(T_semicolon);
    } else {
        /* An enum alias may name an existing tag, or define a tagged or
         * untagged enum, and derive a pointer or array from it like any other
         * base.
         */
        bool is_definition;
        const type_t *base =
            lex_peek(T_enum, NULL)
                ? read_enum_specifier(NULL, &is_definition)
                : read_global_typedef_base(&typedef_const, &typedef_volatile);

        /* `typedef int const ci_t;` and `typedef int volatile vi_t;` qualify
         * the alias just as the leading spelling does.
         */
        read_type_qualifiers(&typedef_const, &typedef_volatile,
                             base->ptr_level != 0);
        read_global_typedef_declarators(block, typedef_const, typedef_volatile,
                                        base);
    }
}

void read_global_statement(void)
{
    char token[MAX_ID_LEN];
    block_t *block = GLOBAL_BLOCK; /* merged object/function namespace */
    bool is_const = false;
    bool is_static = false;
    bool is_extern = false;
    bool is_inline = false;
    bool is_volatile = false;

    /* These specifiers may appear in either order, and a storage class may also
     * follow the type.
     */
    hoist_storage_class_specifiers();
    while (lex_peek(T_const, NULL) || lex_peek(T_static, NULL) ||
           lex_peek(T_extern, NULL) || lex_peek(T_inline, NULL) ||
           lex_peek(T_volatile, NULL)) {
        if (lex_accept(T_const))
            is_const = true;
        else if (lex_accept(T_volatile))
            is_volatile = true;
        else if (lex_accept(T_static)) {
            if (is_static)
                error_at("duplicate static storage class specifier",
                         cur_token_loc());
            is_static = true;
        } else if (lex_accept(T_inline)) {
            if (is_inline)
                error_at("duplicate inline function specifier",
                         cur_token_loc());
            is_inline = true;
        } else {
            lex_expect(T_extern);
            if (is_extern)
                error_at("duplicate extern storage class specifier",
                         cur_token_loc());
            is_extern = true;
        }
    }
    if (is_static && is_extern)
        error_at("static and extern storage classes cannot be combined",
                 cur_token_loc());

    /* typedef is a storage-class specifier too (6.7.1p2), so it admits no other
     * one.
     */
    if ((is_static || is_extern) && lex_peek(T_typedef, NULL))
        error_at("typedef cannot be combined with another storage class",
                 next_token_loc());

    if (floating_type_starts_here())
        error_at("Floating point types are not yet supported", cur_token_loc());

    /* `inline` is a function specifier, not a general declaration qualifier.
     * Scalar declarators are checked by read_global_decl(), but record, enum,
     * and typedef declarations bypass that path entirely. Rejecting those forms
     * here keeps every file-scope declaration category under the same C99
     * constraint.
     */
    if (is_inline && (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL) ||
                      lex_peek(T_enum, NULL) || lex_peek(T_typedef, NULL)))
        error_at("inline specifier requires a function declarator",
                 cur_token_loc());

    if (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL)) {
        base_type_t kind = accept_record_keyword();
        bool has_tag = lex_peek(T_identifier, token);
        type_t *type;

        if (has_tag)
            lex_expect(T_identifier);
        else if (!lex_peek(T_open_curly, NULL))
            error_at("Expected struct or union tag or definition",
                     next_token_loc());

        /* variable declaration using existing record tag? */
        if (!lex_peek(T_open_curly, NULL)) {
            type = local_record_tag(token, CURRENT_TU_SCOPE, kind);

            /* A declaration with no declarator only declares the tag. At file
             * scope a repeated one names the same type, so it is valid whether
             * the tag is new, forward declared, or already complete.
             */
            if (lex_accept(T_semicolon))
                return;

            read_global_declarator_list(block, type, is_const, is_static,
                                        is_volatile, is_extern);
            return;
        }

        type = read_record_body(NULL, kind, has_tag, token);

        /* A record definition may be followed by its declarators, as in "struct
         * pair { int x, y; } first, *second;".
         */
        if (!lex_accept(T_semicolon))
            read_global_declarator_list(block, type, is_const, is_static,
                                        is_volatile, is_extern);
    } else if (lex_peek(T_enum, NULL)) {
        bool is_definition;
        type_t *type = read_enum_specifier(NULL, &is_definition);

        /* A definition may stand alone; a reference to a tag needs a
         * declarator, since it can neither declare nor complete the tag.
         */
        if (!is_definition && lex_peek(T_semicolon, NULL))
            error_at(
                "enum declaration without an enumerator list declares "
                "nothing",
                next_token_loc());
        if (!is_definition || !lex_accept(T_semicolon))
            read_global_declarator_list(block, type, is_const, is_static,
                                        is_volatile, is_extern);
    } else if (lex_accept(T_typedef)) {
        read_global_typedef(block);
    } else if (lex_peek(T_identifier, NULL) || lex_peek(T_signed, NULL) ||
               lex_peek(T_unsigned, NULL) || lex_peek(T_long, NULL)) {
        read_global_decl(block, is_const, is_static, is_extern, is_inline,
                         is_volatile);
    } else
        error_at("Syntax error in global statement", next_token_loc());
}

static type_t *add_builtin_scalar(const char *name,
                                  base_type_t base_type,
                                  int size,
                                  bool is_unsigned,
                                  bool is_signed_char)
{
    type_t *type = add_named_type((char *) name);

    type->base_type = base_type;
    type->size = size;
    type->is_unsigned = is_unsigned;
    type->is_signed_char = is_signed_char;
    return type;
}

void parse_internal(void)
{
    /* set starting point of global stack manually */
    GLOBAL_FUNC = add_func("", true);

    /* The first global slot retains the synthetic global-frame pointer. It must
     * occupy a full target pointer, not the historic 32-bit word.
     */
    GLOBAL_FUNC->stack_size = PTR_SIZE;
    GLOBAL_FUNC->bbs = arena_alloc_bb();
    GLOBAL_FUNC->bbs->belong_to = GLOBAL_FUNC; /* Prevent nullptr deref in RA */
    GLOBAL_FUNC->bbs->elf_offset = -1;         /* not yet emitted */

    /* built-in types */
    TY_void = add_named_type("void");
    TY_void->base_type = TYPE_void;
    TY_void->size = 0;

    TY_char = add_named_type("char");
    TY_char->base_type = TYPE_char;
    TY_char->size = 1;

    TY_schar = add_named_type("signed char");
    TY_schar->base_type = TYPE_char;
    TY_schar->size = 1;
    TY_schar->is_signed_char = true;

    TY_uchar = add_named_type("unsigned char");
    TY_uchar->base_type = TYPE_char;
    TY_uchar->size = 1;
    TY_uchar->is_unsigned = true;

    TY_int = add_named_type("int");
    TY_int->base_type = TYPE_int;
    TY_int->size = 4;

    TY_uint = add_named_type("unsigned int");
    TY_uint->base_type = TYPE_int;
    TY_uint->size = 4;
    TY_uint->is_unsigned = true;

    /* Keep C99 floating scalar identities in the type table before their IR and
     * ABI lowering are admitted. The LP64 targets use their ABI's 16-byte
     * long-double object slot; the current 32-bit soft-float targets use an
     * 8-byte long double until their target-specific representation is
     * implemented.
     */
    TY_float = add_named_type("float");
    TY_float->base_type = TYPE_float;
    TY_float->size = 4;
    TY_float->is_floating = true;

    TY_double = add_named_type("double");
    TY_double->base_type = TYPE_double;
    TY_double->size = 8;
    TY_double->is_floating = true;

    TY_long_double = add_named_type("long double");
    TY_long_double->base_type = TYPE_long_double;
    TY_long_double->size = PTR_SIZE == 8 ? 16 : 8;
    TY_long_double->is_floating = true;

    /* long has the same current ABI width as int, but it remains a distinct C
     * type: redeclarations and the usual arithmetic conversions depend on rank,
     * not just representation size.
     */
    TY_long = add_named_type("long");
    TY_long->base_type = TYPE_long;
    TY_long->size = 4;

    TY_ulong = add_named_type("unsigned long");
    TY_ulong->base_type = TYPE_long;
    TY_ulong->size = 4;
    TY_ulong->is_unsigned = true;

    TY_short = add_named_type("short");
    TY_short->base_type = TYPE_short;
    TY_short->size = 2;

    TY_ushort = add_named_type("unsigned short");
    TY_ushort->base_type = TYPE_short;
    TY_ushort->size = 2;
    TY_ushort->is_unsigned = true;

    /* Unlike `long`, which deliberately shares the current 32-bit int ABI, long
     * long has a distinct type and an eight-byte object representation.
     */
    TY_long_long = add_named_type("long long");
    TY_long_long->base_type = TYPE_long_long;
    TY_long_long->size = 8;

    TY_ulong_long = add_named_type("unsigned long long");
    TY_ulong_long->base_type = TYPE_long_long;
    TY_ulong_long->size = 8;
    TY_ulong_long->is_unsigned = true;

    /* C99's <stddef.h> names target-sized integer typedefs. Keep them in the
     * builtin type table so declarations, casts, sizeof, and prototypes use the
     * same pointer-width representation as the rest of the compiler.
     */
    add_builtin_scalar("size_t", PTR_SIZE == 8 ? TYPE_long_long : TYPE_long,
                       PTR_SIZE, true, false);
    add_builtin_scalar("ptrdiff_t", PTR_SIZE == 8 ? TYPE_long_long : TYPE_long,
                       PTR_SIZE, false, false);

    /* <stdarg.h> belongs to C99's freestanding library. Its va_list is the
     * compiler ABI's int-based cursor, so retain that scalar base while
     * recording the pointer depth directly in the builtin typedef.
     */
    type_t *TY_va_list = add_named_type("va_list");
    TY_va_list->base_type = TYPE_int;
    TY_va_list->ptr_level = 1;
    TY_va_list->size = PTR_SIZE;

    /* The execution wide-character type is int until wide literal lowering is
     * implemented; declaring the C99 typedef remains useful independently.
     */
    add_builtin_scalar("wchar_t", TYPE_int, TY_int->size, false, false);
    add_builtin_scalar("sig_atomic_t", TYPE_int, TY_int->size, false, false);
    add_builtin_scalar("wint_t", TYPE_int, TY_int->size, true, false);

    /* C99 <stdint.h> aliases share the target scalar representations. Keep them
     * named in the builtin table so declarations, casts, sizeof, and prototypes
     * use the same ABI metadata as their underlying types.
     */
    type_t *TY_int8 = add_builtin_scalar("int8_t", TYPE_char, 1, false, true);
    type_t *TY_uint8 = add_builtin_scalar("uint8_t", TYPE_char, 1, true, false);
    add_builtin_scalar("int16_t", TYPE_short, 2, false, false);
    add_builtin_scalar("uint16_t", TYPE_short, 2, true, false);
    add_builtin_scalar("int32_t", TYPE_int, 4, false, false);
    add_builtin_scalar("uint32_t", TYPE_int, 4, true, false);
    add_builtin_scalar("int64_t", TYPE_long_long, 8, false, false);
    add_builtin_scalar("uint64_t", TYPE_long_long, 8, true, false);
    add_builtin_scalar("intmax_t", TYPE_long_long, 8, false, false);
    add_builtin_scalar("uintmax_t", TYPE_long_long, 8, true, false);
    add_builtin_scalar("intptr_t", PTR_SIZE == 8 ? TYPE_long_long : TYPE_long,
                       PTR_SIZE, false, false);
    add_builtin_scalar("uintptr_t", PTR_SIZE == 8 ? TYPE_long_long : TYPE_long,
                       PTR_SIZE, true, false);

    add_builtin_scalar("int_least8_t", TY_int8->base_type, TY_int8->size, false,
                       true);
    add_builtin_scalar("uint_least8_t", TY_uint8->base_type, TY_uint8->size,
                       true, false);
    add_builtin_scalar("int_least16_t", TYPE_short, 2, false, false);
    add_builtin_scalar("uint_least16_t", TYPE_short, 2, true, false);
    add_builtin_scalar("int_least32_t", TYPE_int, 4, false, false);
    add_builtin_scalar("uint_least32_t", TYPE_int, 4, true, false);
    add_builtin_scalar("int_least64_t", TYPE_long_long, 8, false, false);
    add_builtin_scalar("uint_least64_t", TYPE_long_long, 8, true, false);
    add_builtin_scalar("int_fast8_t", TYPE_int, 4, false, false);
    add_builtin_scalar("uint_fast8_t", TYPE_int, 4, true, false);
    add_builtin_scalar("int_fast16_t", TYPE_int, 4, false, false);
    add_builtin_scalar("uint_fast16_t", TYPE_int, 4, true, false);
    add_builtin_scalar("int_fast32_t", TYPE_int, 4, false, false);
    add_builtin_scalar("uint_fast32_t", TYPE_int, 4, true, false);
    add_builtin_scalar("int_fast64_t", TYPE_long_long, 8, false, false);
    add_builtin_scalar("uint_fast64_t", TYPE_long_long, 8, true, false);

    /* builtin type _Bool was introduced in C99 specification, it is more
     * well-known as macro type bool, which is defined in <std_bool.h> (in
     * shecc, it is defined in 'lib/c.c').
     */
    TY_bool = add_named_type("_Bool");
    TY_bool->base_type = TYPE_char;
    TY_bool->size = 1;
    TY_bool->is_bool = true;

    builtin_types_idx = types_idx;

    GLOBAL_BLOCK = add_block(NULL, NULL); /* global block */
    CURRENT_TU_SCOPE = add_block(NULL, NULL);
    reset_tu_ordinary_index();
    elf_add_symbol("", 0); /* undef symbol */

    if (dynlink) {
        /* Dynamic callers declare the libc entry with a machine-word ABI:
         * intptr_t syscall(intptr_t number, ...); intptr_t preserves
         * pointer-valued results on every target.
         */
    } else {
        /* Linux syscall */
        func_t *func = add_func("__syscall", true);
        func->return_def.type = PTR_SIZE == 8 ? TY_long_long : TY_int;
        func->num_params = 0;
        func->va_args = 1;
        func->bbs = NULL;
        /* Otherwise, allocate a basic block to implement in static mode. */
        func->bbs = arena_alloc_bb();
        func->bbs->elf_offset = -1; /* not yet emitted */
    }

    /* Add a global object to the .data section.
     *
     * This object saves the global stack pointer, so it is written back as a
     * pointer and must reserve a full one: on an LP64 target the historic
     * 32-bit word left four bytes belonging to the next global.
     */
    elf_write_ptr(elf_data, 0);

    /* lexer initialization */
    do {
        if (lex_accept(T_translation_unit)) {
            CURRENT_TU_SCOPE = add_block(NULL, NULL);
            reset_tu_ordinary_index();
            continue;
        }
        read_global_statement();
    } while (!lex_accept(T_eof));

    /* Aggregate returns use shecc's internal destination-pointer convention,
     * not the platform ABI's aggregate classification. A direct call to a
     * declaration-only function would otherwise quietly cross that boundary
     * with incompatible arguments. Indirect calls separately require tracked
     * provenance proving that their target is shecc-defined.
     */
    for (func_t *func = FUNC_LIST.head; func; func = func->next)
        if (func->aggregate_call_used && !func->bbs)
            error_at("aggregate-return call requires a shecc-defined function",
                     cur_token_loc());
}

void parse(token_t *tk)
{
    token_t head;
    head.kind = T_start;
    head.next = tk;
    cur_token = &head;

    parse_internal();
}
