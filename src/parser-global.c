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
        parse_array_init(var, block, &GLOBAL_FUNC->bbs, true);
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
    var_t *object = find_global_var(var->var_name);

    if (object && object != var)
        error_at("function declaration conflicts with global object",
                 next_token_loc());

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
    var_reset_subscripts(&func->return_def);
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
    }

    if (check_decl) {
        if (!compatible_decl_type(func->return_def.type,
                                  func_tmp.return_def.type) ||
            func->return_def.ptr_level != func_tmp.return_def.ptr_level ||
            func->return_def.is_const_qualified !=
                func_tmp.return_def.is_const_qualified)
            error_at("conflicting types for function declaration",
                     next_token_loc());
        if (func->has_prototype && func_tmp.has_prototype) {
            if (func->num_params != func_tmp.num_params ||
                func->va_args != func_tmp.va_args)
                error_at("conflicting types for function declaration",
                         next_token_loc());
            for (int i = 0; i < func->num_params; i++) {
                const var_t *now = &func->param_defs[i];
                const var_t *before = &func_tmp.param_defs[i];
                if (!compatible_function_param_decl(now, before))
                    error_at("conflicting types for function declaration",
                             next_token_loc());
            }
        } else if (strict_c99 && func->has_prototype &&
                   !func_tmp.has_prototype) {
            /* A variadic prototype and parameters promoted from char, short, or
             * _Bool cannot be compatible with an earlier empty parameter list
             * declaration. Keep the historical extension outside strict C99
             * mode, where existing old-style sources rely on it.
             */
            if (func->va_args)
                error_at("conflicting types for function declaration",
                         next_token_loc());
            for (int i = 0; i < func->num_params; i++)
                if (parameter_changes_under_default_promotion(
                        &func->param_defs[i]))
                    error_at("conflicting types for function declaration",
                             next_token_loc());
        } else if (!func->has_prototype && func_tmp.has_prototype) {
            /* An empty-list definition has no named parameters. It cannot
             * define a function previously declared with fixed parameters or an
             * ellipsis, even though a non-defining `f()` declaration may
             * coexist with that prototype.
             */
            if (lex_peek(T_open_curly, NULL) &&
                (func_tmp.num_params || func_tmp.va_args))
                error_at("conflicting types for function declaration",
                         next_token_loc());

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
        if (check_decl && func_tmp.bbs)
            error_at("redefinition of function", next_token_loc());
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
    var_t *previous = NULL;

    *is_redeclaration = false;

    /* The ordinary identifier namespace is shared with functions. This is
     * intentionally before object redeclaration handling: a function is not a
     * compatible tentative definition of an object, even when both happen to
     * have the same declared scalar type.
     */
    if (find_func(var->var_name))
        error_at("global object declaration conflicts with function",
                 next_token_loc());

    for (int i = 0; i + 1 < block->locals.size; i++) {
        var_t *candidate = block->locals.elements[i];
        if (!strcmp(candidate->var_name, var->var_name)) {
            previous = candidate;
            break;
        }
    }
    if (!previous)
        return var;

    *is_redeclaration = true;

    if (!compatible_decl_type(previous->type, var->type) ||
        (!!previous->pointee_func_signature != !!var->pointee_func_signature) ||
        (previous->pointee_func_signature &&
         !compatible_function_signature(previous->pointee_func_signature,
                                        var->pointee_func_signature)) ||
        previous->ptr_level != var->ptr_level ||
        previous->array_size != var->array_size ||
        previous->array_dim2 != var->array_dim2 ||
        previous->array_dim3 != var->array_dim3 ||
        previous->array_dim4 != var->array_dim4 ||
        previous->pointee_array_size != var->pointee_array_size ||
        previous->pointee_array_dim2 != var->pointee_array_dim2 ||
        previous->pointee_array_dim3 != var->pointee_array_dim3 ||
        previous->pointee_array_dim4 != var->pointee_array_dim4 ||
        previous->is_const_qualified != var->is_const_qualified ||
        previous->is_volatile != var->is_volatile)
        error_at("conflicting types for global declaration", next_token_loc());
    if (!previous->is_static && is_static)
        error_at("static declaration follows non-static declaration",
                 next_token_loc());
    if (lex_peek(T_assign, NULL) && previous->has_initializer)
        error_at("redefinition of global variable", next_token_loc());

    /* Scalar declarators were placed on the operand stack by
     * read_inner_var_decl(). Its later initializer lowering pops that entry, so
     * point it at the shared object rather than the discarded declaration.
     */
    if (operand_stack_idx && operand_stack[operand_stack_idx - 1] == var)
        operand_stack[operand_stack_idx - 1] = previous;
    block->locals.size--;
    return previous;
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
    bool is_redeclaration;
    var_t *nv = require_typed_var(block, decl_type);
    nv->is_global = true;
    nv->is_static = is_static;
    nv->is_const_qualified = is_const;
    nv->is_volatile = is_volatile;
    read_inner_var_decl(nv, false, false, false);
    nv->is_extern = is_extern && !lex_peek(T_assign, NULL);
    if (lex_peek(T_open_bracket, NULL))
        return read_global_function_declarator(block, nv, is_static,
                                               allow_definition);
    bool is_definition = !nv->is_extern;
    if (is_definition && is_incomplete_record_object(nv))
        error_at("Incomplete struct/union type cannot define an object",
                 cur_token_loc());
    nv = resolve_global_declarator(block, nv, is_static, &is_redeclaration);
    if (is_definition && (!is_redeclaration || nv->is_extern)) {
        nv->is_extern = false;
        add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, nv, NULL, NULL, 0, NULL);
    }
    read_global_init_var(nv, block);
    discard_global_declarator_operand(nv);
    return false;
}

void consume_global_compound_literal(void);

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
    parse_struct_field_init(block, &GLOBAL_FUNC->bbs, record_type, var, true);
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
                        : find_type(type_name, 1);
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
    char type_name[MAX_ID_LEN];
    type_t *compound_type;

    UNUSED(block);

    lex_expect(T_open_bracket);
    lex_ident(T_identifier, type_name);
    compound_type = find_type(type_name, true);
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
    char type_name[MAX_ID_LEN];
    type_t *element_type;
    var_t *array;
    int element_ptr_level = 0;

    lex_expect(T_open_bracket);
    base_type_t record_kind = accept_record_keyword();
    lex_ident(T_identifier, type_name);
    element_type = record_kind
                       ? find_record_tag(type_name, var->scope, record_kind)
                       : find_type(type_name, 1);
    while (lex_accept(T_asterisk)) {
        element_ptr_level++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }

    /* `(int (*[])[2]){...}` is an array whose elements are pointers to rows.
     * The inner suffix supplies the backing array bound; the suffixes after `)`
     * describe each pointer element's pointee.
     */
    if (lex_accept(T_open_bracket)) {
        int pointee_dims = 0;

        if (element_ptr_level || !lex_peek(T_asterisk, NULL))
            error_at("Array compound literal needs a pointer declarator",
                     cur_token_loc());
        do {
            lex_expect(T_asterisk);
            element_ptr_level++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        } while (lex_peek(T_asterisk, NULL));
        lex_expect(T_open_square);

        array = require_typed_var(GLOBAL_BLOCK, element_type);
        array->var_name = gen_name();
        array->is_global = true;
        array->ptr_level = element_ptr_level;
        if (!lex_peek(T_close_square, NULL)) {
            array->array_size = read_const_expr(GLOBAL_BLOCK);
            if (array->array_size <= 0)
                error_at("Array compound literal needs a positive bound",
                         cur_token_loc());
        } else {
            array->has_unsized_array = true;
        }
        lex_expect(T_close_square);
        lex_expect(T_close_bracket);
        while (lex_accept(T_open_square)) {
            int bound = read_const_expr(GLOBAL_BLOCK);

            if (pointee_dims >= 4)
                error_at("Array declarators support at most four dimensions",
                         cur_token_loc());
            if (bound <= 0)
                error_at("Array size must be positive", cur_token_loc());
            if (pointee_dims == 0)
                array->pointee_array_size = bound;
            else {
                if (pointee_dims == 1)
                    array->pointee_array_dim2 = bound;
                else if (pointee_dims == 2)
                    array->pointee_array_dim3 = bound;
                else
                    array->pointee_array_dim4 = bound;
                array->pointee_array_size *= bound;
            }
            lex_expect(T_close_square);
            pointee_dims++;
        }
        lex_expect(T_close_bracket);

        if (!element_type || element_type != var->type ||
            var->ptr_level != element_ptr_level + 1 ||
            var->pointee_array_size != array->pointee_array_size ||
            var->pointee_array_dim2 != array->pointee_array_dim2 ||
            var->pointee_array_dim3 != array->pointee_array_dim3 ||
            var->pointee_array_dim4 != array->pointee_array_dim4)
            error_at("Incompatible array compound literal", cur_token_loc());
        if (!lex_peek(T_open_curly, NULL))
            error_at("Array compound literal needs an initializer",
                     next_token_loc());
        add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_allocat, array, NULL, NULL,
                 0, NULL);
        parse_array_init(array, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
        add_insn(block, GLOBAL_FUNC->bbs, OP_assign, var, array, NULL, 0, NULL);
        return;
    }
    array = require_typed_var(GLOBAL_BLOCK, element_type);
    array->var_name = gen_name();
    array->is_global = true;
    array->ptr_level = element_ptr_level;
    for (int dim = 0; lex_accept(T_open_square); dim++) {
        int bound;

        if (dim >= 4)
            error_at("Array compound literal supports at most four dimensions",
                     cur_token_loc());
        if (lex_peek(T_close_square, NULL)) {
            if (dim)
                error_at("Only the outer array bound may be inferred",
                         cur_token_loc());
            array->has_unsized_array = true;
            lex_expect(T_close_square);
            continue;
        }
        bound = read_const_expr(GLOBAL_BLOCK);
        if (bound <= 0)
            error_at("Array compound literal needs a positive bound",
                     cur_token_loc());
        if (!dim)
            array->array_size = bound;
        else {
            if (dim == 1)
                array->array_dim2 = bound;
            else if (dim == 2)
                array->array_dim3 = bound;
            else
                array->array_dim4 = bound;
            array->array_size *= bound;
        }
        lex_expect(T_close_square);
    }
    lex_expect(T_close_bracket);

    /* The backing array has an outer compound-literal bound and the typedef
     * element's inner row shape. parse_array_init() keeps that shape on var_t
     * rather than type_t.
     */
    if (element_type && element_type->array_size) {
        int trailing = 1;

        if (element_type->array_dim2)
            trailing *= element_type->array_dim2;
        if (element_type->array_dim3)
            trailing *= element_type->array_dim3;
        if (element_type->array_dim4)
            trailing *= element_type->array_dim4;
        if (element_type->array_dim4)
            error_at("Array compound literal supports at most four dimensions",
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
    if (!lex_peek(T_open_curly, NULL))
        error_at("Array compound literal needs an initializer",
                 next_token_loc());

    add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_allocat, array, NULL, NULL, 0,
             NULL);
    parse_array_init(array, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
    add_insn(block, GLOBAL_FUNC->bbs, OP_assign, var, array, NULL, 0, NULL);
}

/* Struct and union objects accept brace initializers, unlike scalar globals.
 * Keep their continuation declarators on the same path as the first one so that
 * linkage, qualifiers, and declarator-specific modifiers cannot diverge.
 */
bool read_global_record_declarator(block_t *block,
                                   type_t *decl_type,
                                   bool is_const,
                                   bool is_static,
                                   bool is_volatile,
                                   bool is_extern,
                                   bool allow_definition)
{
    bool is_redeclaration;
    var_t *var = require_typed_var(block, decl_type);
    var->is_global = true;
    var->is_static = is_static;
    var->is_const_qualified = is_const;
    var->is_volatile = is_volatile;
    read_inner_var_decl(var, false, false, false);
    var->is_extern = is_extern && !lex_peek(T_assign, NULL);
    if (lex_peek(T_open_bracket, NULL))
        return read_global_function_declarator(block, var, is_static,
                                               allow_definition);
    bool is_definition = !var->is_extern;
    if (is_definition && is_incomplete_record_object(var))
        error_at("Incomplete struct/union type cannot define an object",
                 cur_token_loc());
    var = resolve_global_declarator(block, var, is_static, &is_redeclaration);
    if (is_definition && (!is_redeclaration || var->is_extern)) {
        var->is_extern = false;
        add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0, NULL);
    }

    if (!lex_accept(T_assign)) {
        discard_global_declarator_operand(var);
        return false;
    }

    var->has_initializer = true;

    if (lex_peek(T_open_curly, NULL) &&
        (var->array_size > 0 || var->has_unsized_array || var->ptr_level > 0)) {
        parse_array_init(var, block, &GLOBAL_FUNC->bbs, true);
    } else if (global_compound_literal_starts_here() &&
               (var->ptr_level || var->type->ptr_level)) {
        parse_global_compound_array_init(var, block);
    } else if (global_compound_literal_starts_here()) {
        parse_global_compound_record_init(var, block);
    } else if (lex_peek(T_open_curly, NULL)) {
        parse_global_record_init(var, block);
    } else {
        read_global_assignment_var(var);
    }
    discard_global_declarator_operand(var);
    return false;
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
                                 bool is_extern,
                                 bool is_record)
{
    bool first = true;

    read_type_qualifiers(&is_const, &is_volatile, false);
    do {
        bool ended =
            is_record
                ? read_global_record_declarator(block, decl_type, is_const,
                                                is_static, is_volatile,
                                                is_extern, first)
                : read_global_declarator(block, decl_type, is_const, is_static,
                                         is_volatile, is_extern, first);

        if (ended)
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
    bool is_redeclaration;
    var_t *var = require_var(block);
    var->is_global = true;
    var->is_static = is_static;
    var->is_inline = is_inline;
    var->is_const_qualified = is_const;
    var->is_volatile = is_volatile;

    /* new function, or variables under parent */
    read_full_var_decl(var, false, false, false);
    var->is_extern = is_extern && !lex_peek(T_assign, NULL);

    if (lex_peek(T_open_bracket, NULL)) {
        if (read_global_function_declarator(block, var, is_static, true))
            return;
    } else {
        bool is_definition = !var->is_extern;
        if (var->is_inline)
            error_at("inline specifier requires a function declarator",
                     next_token_loc());
        if (is_definition && is_incomplete_record_object(var))
            error_at("Incomplete struct/union type cannot define an object",
                     cur_token_loc());
        var =
            resolve_global_declarator(block, var, is_static, &is_redeclaration);
        if (is_definition && (!is_redeclaration || var->is_extern)) {
            var->is_extern = false;
            add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0,
                     NULL);
        }

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
            } else if (lex_peek(T_char, NULL) || lex_peek(T_wchar, NULL)) {
                lex_next();
            } else {
                error_at(
                    "Global struct initialization requires constant values",
                    next_token_loc());
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
    nv->var_name = "";
    nv->ptr_level = 0;
    nv->is_func = false;
    nv->is_global = false;
    nv->is_const_qualified = false;
    nv->array_size = 0;
    nv->offset = offset;
    nv->is_bitfield = false;
    nv->bit_width = 0;
    nv->bit_offset = 0;
    nv->bit_storage_size = 0;
    nv->init_val = 0;
    nv->base = NULL;
    nv->subscript = 0;
    var_reset_subscripts(nv);
    nv->is_compound_literal = false;
}

/* The declarators of a file-scope typedef whose base type is not a record or
 * enum definition: scalar, pointer, array and function aliases.
 */
static void read_global_typedef_declarators(block_t *block,
                                            bool typedef_const,
                                            bool typedef_volatile)
{
    char base_type[MAX_ID_LEN];
    const type_t *base;
    type_t *type = add_type();
    bool is_signed = false;
    bool is_unsigned = false;
    bool is_long = false;
    bool is_long_long = false;
    type_t *leading_scalar_type = NULL;

    if (lex_peek(T_identifier, base_type) &&
        (!strcmp(base_type, "char") || !strcmp(base_type, "short") ||
         !strcmp(base_type, "int"))) {
        token_t *after_base = cur_token->next->next;

        while (after_base && after_base->kind == T_const)
            after_base = after_base->next;
        if (after_base &&
            (after_base->kind == T_signed || after_base->kind == T_unsigned)) {
            lex_expect(T_identifier);
            leading_scalar_type = find_type(base_type, true);
        }
    }

    /* Typedef declarations use the same freely ordered scalar specifier set as
     * object declarations. Keeping this in a loop admits C99 spellings such as
     * `long unsigned long` rather than treating the second specifier as the
     * typedef name.
     */
    while (lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
           lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
           lex_peek(T_long, NULL)) {
        if (lex_accept(T_const))
            typedef_const = true;
        else if (lex_accept(T_volatile))
            typedef_volatile = true;
        else if (lex_accept(T_signed)) {
            if (is_signed)
                error_at("duplicate signed type specifier", cur_token_loc());
            is_signed = true;
        } else if (lex_accept(T_unsigned)) {
            if (is_unsigned)
                error_at("duplicate unsigned type specifier", cur_token_loc());
            is_unsigned = true;
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
    if (leading_scalar_type == TY_int && lex_peek(T_identifier, base_type) &&
        !strcmp(base_type, "char"))
        error_at("int cannot be combined with char", cur_token_loc());

    bool is_float = lex_accept(T_float);
    bool is_double = lex_accept(T_double);

    if (is_float) {
        if (is_signed || is_unsigned || is_long)
            error_at("invalid float type specifiers", cur_token_loc());
        base = TY_float;
    } else if (is_double) {
        if (is_signed || is_unsigned || is_long_long)
            error_at("invalid double type specifiers", cur_token_loc());
        base = is_long ? TY_long_double : TY_double;
    } else if (is_long) {
        if (lex_peek(T_identifier, base_type) && !strcmp(base_type, "int"))
            lex_expect(T_identifier);
        if (is_long_long)
            base = is_unsigned ? TY_ulong_long : TY_long_long;
        else
            base = is_unsigned ? TY_ulong : TY_long;
    } else if (is_unsigned) {
        if (leading_scalar_type == TY_char) {
            base = TY_uchar;
        } else if (leading_scalar_type == TY_short) {
            if (lex_peek(T_identifier, base_type) && !strcmp(base_type, "int"))
                lex_expect(T_identifier);
            base = TY_ushort;
        } else {
            if (lex_peek(T_identifier, base_type) &&
                (!strcmp(base_type, "int") || !strcmp(base_type, "char") ||
                 !strcmp(base_type, "short"))) {
                lex_expect(T_identifier);
                if (!strcmp(base_type, "char"))
                    base = TY_uchar;
                else if (!strcmp(base_type, "short"))
                    base = TY_ushort;
                else
                    base = TY_uint;
            } else {
                base = TY_uint;
            }
        }
    } else if (is_signed && lex_peek(T_identifier, base_type) &&
               (!strcmp(base_type, "char") || !strcmp(base_type, "short") ||
                !strcmp(base_type, "int"))) {
        lex_expect(T_identifier);
        base = !strcmp(base_type, "char")
                   ? TY_schar
                   : (!strcmp(base_type, "short") ? TY_short : TY_int);
    } else if (is_signed && (leading_scalar_type == TY_char ||
                             leading_scalar_type == TY_short)) {
        if (leading_scalar_type == TY_short &&
            lex_peek(T_identifier, base_type) && !strcmp(base_type, "int"))
            lex_expect(T_identifier);
        base = leading_scalar_type == TY_char ? TY_schar : leading_scalar_type;
    } else if (is_signed &&
               (!lex_peek(T_identifier, base_type) ||
                (strcmp(base_type, "int") && strcmp(base_type, "char") &&
                 strcmp(base_type, "short")))) {
        base = TY_int;
    } else if (leading_scalar_type) {
        base = leading_scalar_type;
    } else {
        lex_ident(T_identifier, base_type);
        base = find_type(base_type, true);
    }
    if (!base)
        error_at("Unable to find base type", cur_token_loc());

    /* `typedef int const ci_t;` and `typedef int volatile vi_t;` qualify the
     * alias just as the leading spelling does.
     */
    read_type_qualifiers(&typedef_const, &typedef_volatile,
                         base->ptr_level != 0);
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
    type->is_const_qualified = typedef_const || base->is_const_qualified;
    type->is_volatile_qualified =
        typedef_volatile || base->is_volatile_qualified;
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

    /* Handle pointer types in typedef: typedef char *string; */
    while (lex_accept(T_asterisk)) {
        type->ptr_level++;
        type->size = PTR_SIZE;
        while (true) {
            if (lex_accept(T_const)) {
                if (type->ptr_level <= 32)
                    type->pointer_const_mask |= 1U << (type->ptr_level - 1);
            } else if (lex_accept(T_volatile)) {
                /* As for an object declarator, a volatile pointer marks the
                 * whole declaration volatile.
                 */
                type->is_volatile_qualified = true;
            } else if (lex_accept(T_restrict)) {
                ;
            } else
                break;
        }
    }

    /* A parenthesized declarator is the function-pointer form: `typedef int
     * (*callback_t)(int)`. Parse it through the normal declarator reader so its
     * prototype has exactly the same shape as an object declaration, then
     * retain that syntax-only signature on the alias for each later object or
     * parameter declaration.
     */
    if (lex_peek(T_open_bracket, NULL)) {
        var_t declarator = {0};
        bool saved_sizeof_signature = parsing_sizeof_function_signature;

        declarator.type = (type_t *) base;
        declarator.scope = block;
        declarator.ptr_level = type->ptr_level;

        /* A callback typedef carries only a function signature. Its floating
         * parameters do not materialize values until a call, which remains
         * rejected by the ordinary floating gates.
         */
        parsing_sizeof_function_signature = true;
        read_inner_var_decl(&declarator, false, false, false);
        parsing_sizeof_function_signature = saved_sizeof_signature;
        if (!declarator.is_func)
            error_at(
                "Typedef parenthesized declarator must be a function "
                "pointer",
                cur_token_loc());
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
        type->ptr_level = 0;
        type->pointer_const_mask = declarator.pointer_const_mask;
        type->is_volatile_qualified = declarator.is_volatile;
        lex_expect(T_semicolon);
        return;
    }

    lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);

    /* A typedef declarator may wrap an existing array typedef: `typedef row
     * matrix[2]`. Gather its leading bounds first, then prepend them to the
     * base alias's bounds instead of overwriting the inner extent.
     */
    fixed_array_shape_t base_shape = fixed_array_shape_from_type(type);
    fixed_array_shape_t decl_shape = {0};
    while (lex_accept(T_open_square)) {
        int bound;

        if (decl_shape.rank >= MAX_FIXED_ARRAY_RANK)
            error_at("Array declarators support at most four dimensions",
                     cur_token_loc());
        if (lex_peek(T_close_square, NULL))
            error_at("Typedef array needs a positive bound", cur_token_loc());
        bound = read_const_expr(block);
        if (bound <= 0)
            error_at("Typedef array needs a positive bound", cur_token_loc());
        decl_shape.bounds[decl_shape.rank++] = bound;
        lex_expect(T_close_square);
    }
    if (decl_shape.rank) {
        fixed_array_shape_t shape =
            fixed_array_shape_prepend(&decl_shape, &base_shape);

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
    lex_expect(T_semicolon);
}

/* A file-scope typedef, after its keyword: record, untagged enum, callback
 * pointer and scalar aliases. Each form reads a single declarator; a
 * comma-separated declarator list is only parsed by the block-scope typedef
 * reader.
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
    if (lex_accept(T_enum)) {
        int val = 0;
        type_t *type = add_type();

        initialize_enum_type(type);
        lex_expect(T_open_curly);
        bool first = true;
        do {
            lex_ident(T_identifier, token);
            if (!first && !lex_peek(T_assign, NULL))
                val = next_enum_value(val);
            if (lex_accept(T_assign))
                val = read_enum_constant(block);
            add_constant(token, val);
            first = false;
        } while (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL));
        lex_expect(T_close_curly);
        read_type_qualifiers(&typedef_const, &typedef_volatile, false);
        type->is_const_qualified = typedef_const;
        type->is_volatile_qualified = typedef_volatile;
        lex_ident(T_identifier, token);
        set_type_name(type, token);
        lex_expect(T_semicolon);
    } else if (lex_accept(T_struct)) {
        int i = 0, size = 0, alignment = 1;
        bitfield_layout_t bits = {0};
        bool has_struct_def = false;
        bool has_flexible_array_member = false;
        type_t *tag = NULL, *type = add_type();

        /* is struct definition? */
        if (lex_peek(T_identifier, token)) {
            lex_expect(T_identifier);

            tag = local_record_tag(token, GLOBAL_BLOCK, TYPE_struct);
        }

        /* typedef with struct definition */
        if (lex_accept(T_open_curly)) {
            has_struct_def = true;
            do {
                var_t *v = type_add_field(type, &i);
                var_t *last = v;
                read_full_var_decl(v, false, false, true);
                read_bitfield_width(v, block);
                reject_flexible_array_member_container(v);
                mark_flexible_array_member(v, false);
                size = is_bitfield(v)
                           ? layout_bitfield_field(size, v, &alignment, &bits)
                           : layout_struct_field(
                                 flush_bitfield_layout(size, &bits), v,
                                 &alignment);

                /* Handle multiple variable declarations with same base type */
                while (lex_accept(T_comma)) {
                    if (last->is_flexible_array_member)
                        error_at(
                            "Flexible array member must be the final "
                            "struct member",
                            cur_token_loc());
                    var_t *nv = type_add_field(type, &i);
                    initialize_struct_field(nv, v, 0);
                    read_inner_var_decl(nv, false, false, true);
                    read_bitfield_width(nv, block);
                    reject_flexible_array_member_container(nv);
                    mark_flexible_array_member(nv, false);
                    last = nv;
                    size =
                        is_bitfield(nv)
                            ? layout_bitfield_field(size, nv, &alignment, &bits)
                            : layout_struct_field(
                                  flush_bitfield_layout(size, &bits), nv,
                                  &alignment);
                }

                lex_expect(T_semicolon);
                if (last->is_flexible_array_member) {
                    if (!lex_peek(T_close_curly, NULL))
                        error_at(
                            "Flexible array member must be the final "
                            "struct member",
                            cur_token_loc());
                    if (i == 1)
                        error_at(
                            "Struct needs a named member before its "
                            "flexible array member",
                            cur_token_loc());
                    has_flexible_array_member = true;
                }
            } while (!lex_accept(T_close_curly));
        }

        read_type_qualifiers(&typedef_const, &typedef_volatile, false);
        while (lex_accept(T_asterisk)) {
            type->ptr_level++;
            type->size = PTR_SIZE;
        }
        lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);
        size = flush_bitfield_layout(size, &bits);
        type->alignment = type->ptr_level ? PTR_SIZE : alignment;
        type->size = type->ptr_level ? PTR_SIZE : ALIGN_UP(size, alignment);
        type->num_fields = i;
        type->base_type = TYPE_typedef;
        type->has_flexible_array_member = has_flexible_array_member;

        if (tag && has_struct_def == 1) {
            strcpy(token, tag->type_name);
            memcpy(tag, type, sizeof(type_t));
            tag->base_type = TYPE_struct;
            set_type_name(tag, token);
        } else {
            /* If it is a forward declaration, build a connection between
             * structure tag and alias. In 'find_type', it will retrieve
             * infomation from base structure for alias.
             */
            type->base_struct = tag;
        }

        /* Only the alias is qualified; the tag copy above must stay plain. */
        type->is_const_qualified = typedef_const;
        type->is_volatile_qualified = typedef_volatile;

        lex_expect(T_semicolon);
    } else if (lex_accept(T_union)) {
        int i = 0, max_size = 0, alignment = 1;
        bool has_union_def = false;
        bool has_flexible_array_member = false;
        type_t *tag = NULL, *type = add_type();

        /* is union definition? */
        if (lex_peek(T_identifier, token)) {
            lex_expect(T_identifier);

            tag = local_record_tag(token, GLOBAL_BLOCK, TYPE_union);
        }

        /* typedef with union definition */
        if (lex_accept(T_open_curly)) {
            has_union_def = true;
            do {
                var_t *v = type_add_field(type, &i);
                read_full_var_decl(v, false, false, true);
                read_bitfield_width(v, block);
                has_flexible_array_member |=
                    is_flexible_array_member_container(v);
                mark_flexible_array_member(v, true);
                v->offset = 0; /* All union fields start at offset 0 */
                int field_size = is_bitfield(v)
                                     ? (v->bit_width ? v->bit_storage_size : 0)
                                     : size_var(v);
                if (field_size > max_size)
                    max_size = field_size;
                if (alignment_var(v) > alignment)
                    alignment = alignment_var(v);

                /* Handle multiple variable declarations with same base type */
                while (lex_accept(T_comma)) {
                    var_t *nv = type_add_field(type, &i);
                    /* All union fields start at offset 0 */
                    initialize_struct_field(nv, v, 0);
                    read_inner_var_decl(nv, false, false, true);
                    read_bitfield_width(nv, block);
                    has_flexible_array_member |=
                        is_flexible_array_member_container(nv);
                    mark_flexible_array_member(nv, true);
                    field_size =
                        is_bitfield(nv)
                            ? (nv->bit_width ? nv->bit_storage_size : 0)
                            : size_var(nv);
                    if (field_size > max_size)
                        max_size = field_size;
                    if (alignment_var(nv) > alignment)
                        alignment = alignment_var(nv);
                }

                lex_expect(T_semicolon);
            } while (!lex_accept(T_close_curly));
        }

        read_type_qualifiers(&typedef_const, &typedef_volatile, false);
        while (lex_accept(T_asterisk)) {
            type->ptr_level++;
            type->size = PTR_SIZE;
        }
        lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);
        type->alignment = type->ptr_level ? PTR_SIZE : alignment;
        type->size = type->ptr_level ? PTR_SIZE : ALIGN_UP(max_size, alignment);
        type->num_fields = i;
        type->base_type = TYPE_typedef;
        type->is_union = true;
        type->has_flexible_array_member = has_flexible_array_member;

        if (tag && has_union_def == 1) {
            strcpy(token, tag->type_name);
            memcpy(tag, type, sizeof(type_t));
            tag->base_type = TYPE_union;
            set_type_name(tag, token);
        } else {
            /* If it is a forward declaration, build a connection between union
             * tag and alias. In 'find_type', it will retrieve information from
             * base union for alias.
             */
            type->base_struct = tag;
        }

        type->is_const_qualified = typedef_const;
        type->is_volatile_qualified = typedef_volatile;

        lex_expect(T_semicolon);
    } else {
        read_global_typedef_declarators(block, typedef_const, typedef_volatile);
    }
}

void read_global_statement(void)
{
    char token[MAX_ID_LEN];
    block_t *block = GLOBAL_BLOCK; /* global block */
    bool is_const = false;
    bool is_static = false;
    bool is_extern = false;
    bool is_inline = false;
    bool is_volatile = false;

    /* These specifiers may appear in either order. */
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

    if (lex_accept(T_struct)) {
        int i = 0, size = 0, alignment = 1;
        bitfield_layout_t bits = {0};
        bool has_flexible_array_member = false;

        lex_ident(T_identifier, token);
        type_t *type = local_record_tag(token, GLOBAL_BLOCK, TYPE_struct);

        /* variable declaration using existing struct tag? */
        if (!lex_peek(T_open_curly, NULL)) {
            /* A declaration with no declarator only declares the tag. At file
             * scope a repeated one names the same type, so it is valid whether
             * the tag is new, forward declared, or already complete.
             */
            if (lex_accept(T_semicolon))
                return;

            read_global_declarator_list(block, type, is_const, is_static,
                                        is_volatile, is_extern, true);
            return;
        }

        lex_expect(T_open_curly);
        do {
            var_t *v = type_add_field(type, &i);
            var_t *last = v;
            read_full_var_decl(v, false, false, true);
            read_bitfield_width(v, block);
            reject_flexible_array_member_container(v);
            mark_flexible_array_member(v, false);
            size = is_bitfield(v)
                       ? layout_bitfield_field(size, v, &alignment, &bits)
                       : layout_struct_field(flush_bitfield_layout(size, &bits),
                                             v, &alignment);

            /* Handle multiple variable declarations with same base type */
            while (lex_accept(T_comma)) {
                if (last->is_flexible_array_member)
                    error_at(
                        "Flexible array member must be the final struct member",
                        cur_token_loc());
                var_t *nv = type_add_field(type, &i);
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, false, true);
                read_bitfield_width(nv, block);
                reject_flexible_array_member_container(nv);
                mark_flexible_array_member(nv, false);
                last = nv;
                size = is_bitfield(nv)
                           ? layout_bitfield_field(size, nv, &alignment, &bits)
                           : layout_struct_field(
                                 flush_bitfield_layout(size, &bits), nv,
                                 &alignment);
            }

            lex_expect(T_semicolon);
            if (last->is_flexible_array_member) {
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

        size = flush_bitfield_layout(size, &bits);
        type->alignment = alignment;
        type->size = ALIGN_UP(size, alignment);
        type->num_fields = i;
        type->has_flexible_array_member = has_flexible_array_member;

        /* A record definition may be followed by its declarators, as in "struct
         * pair { int x, y; } first, *second;".
         */
        if (!lex_accept(T_semicolon))
            read_global_declarator_list(block, type, is_const, is_static,
                                        is_volatile, is_extern, true);
    } else if (lex_accept(T_union)) {
        int i = 0, max_size = 0, alignment = 1;
        bool has_flexible_array_member = false;

        lex_ident(T_identifier, token);
        type_t *type = local_record_tag(token, GLOBAL_BLOCK, TYPE_union);

        /* A tagged union declaration may name an already-complete tag, just
         * like `struct tag object;`. Do not require a second definition body
         * before routing its declarators through the shared record path.
         */
        if (!lex_peek(T_open_curly, NULL)) {
            /* As for struct, a bare tag declaration may repeat a known tag. */
            if (lex_accept(T_semicolon))
                return;

            read_global_declarator_list(block, type, is_const, is_static,
                                        is_volatile, is_extern, true);
            return;
        }

        lex_expect(T_open_curly);
        do {
            var_t *v = type_add_field(type, &i);
            read_full_var_decl(v, false, false, true);
            read_bitfield_width(v, block);
            has_flexible_array_member |= is_flexible_array_member_container(v);
            mark_flexible_array_member(v, true);
            v->offset = 0; /* All union fields start at offset 0 */
            int field_size = is_bitfield(v)
                                 ? (v->bit_width ? v->bit_storage_size : 0)
                                 : size_var(v);
            if (field_size > max_size)
                max_size = field_size;
            if (alignment_var(v) > alignment)
                alignment = alignment_var(v);

            /* Handle multiple variable declarations with same base type */
            while (lex_accept(T_comma)) {
                var_t *nv = type_add_field(type, &i);
                /* All union fields start at offset 0 */
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, false, true);
                read_bitfield_width(nv, block);
                has_flexible_array_member |=
                    is_flexible_array_member_container(nv);
                mark_flexible_array_member(nv, true);
                field_size = is_bitfield(nv)
                                 ? (nv->bit_width ? nv->bit_storage_size : 0)
                                 : size_var(nv);
                if (field_size > max_size)
                    max_size = field_size;
                if (alignment_var(nv) > alignment)
                    alignment = alignment_var(nv);
            }

            lex_expect(T_semicolon);
        } while (!lex_accept(T_close_curly));

        type->alignment = alignment;
        type->size = ALIGN_UP(max_size, alignment);
        type->num_fields = i;
        type->has_flexible_array_member = has_flexible_array_member;

        if (!lex_accept(T_semicolon))
            read_global_declarator_list(block, type, is_const, is_static,
                                        is_volatile, is_extern, true);
    } else if (lex_accept(T_enum)) {
        /* An enum definition is a declaration in its own right; it need not
         * introduce a typedef. Its enumerators are integer constants and may
         * use the same integer constant expressions accepted for array bounds
         * and case labels.
         */
        int val = 0;
        bool has_tag = false;
        type_t *type;

        if (lex_peek(T_identifier, token)) {
            lex_expect(T_identifier);
            has_tag = true;
        }
        if (!lex_peek(T_open_curly, NULL)) {
            if (!has_tag)
                error_at("Expected enum tag or definition", cur_token_loc());
            type = find_enum_tag(token, GLOBAL_BLOCK);
            if (!type)
                error_at("Unknown enum type", cur_token_loc());
            read_global_declarator_list(block, type, is_const, is_static,
                                        is_volatile, is_extern, false);
            return;
        }
        type = has_tag ? local_enum_tag(token, GLOBAL_BLOCK) : NULL;
        if (!type)
            type = add_type();

        initialize_enum_type(type);

        /* Register the tag at file scope as well, so that a struct or union
         * specifier reusing the name, in any scope, sees an enum tag.
         */
        if (has_tag) {
            set_type_name(type, token);
            if (!find_local_type_tag(token, GLOBAL_BLOCK))
                add_type_tag(GLOBAL_BLOCK, token, type);
        }
        lex_expect(T_open_curly);
        bool first = true;
        do {
            lex_ident(T_identifier, token);
            if (!first && !lex_peek(T_assign, NULL))
                val = next_enum_value(val);
            if (lex_accept(T_assign))
                val = read_enum_constant(block);
            add_constant(token, val);
            first = false;
        } while (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL));
        lex_expect(T_close_curly);
        if (!lex_accept(T_semicolon))
            read_global_declarator_list(block, type, is_const, is_static,
                                        is_volatile, is_extern, false);
    } else if (lex_accept(T_typedef)) {
        read_global_typedef(block);
    } else if (lex_peek(T_identifier, NULL) || lex_peek(T_signed, NULL) ||
               lex_peek(T_unsigned, NULL) || lex_peek(T_long, NULL)) {
        read_global_decl(block, is_const, is_static, is_extern, is_inline,
                         is_volatile);
    } else
        error_at("Syntax error in global statement", next_token_loc());
}

void parse_internal(void)
{
    /* set starting point of global stack manually */
    GLOBAL_FUNC = add_func("", true);

    /* The first global slot retains the synthetic global-frame pointer. It must
     * occupy a full target pointer, not the historic 32-bit word.
     */
    GLOBAL_FUNC->stack_size = PTR_SIZE;
    GLOBAL_FUNC->bbs = arena_calloc(BB_ARENA, 1, sizeof(basic_block_t));
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
     * long has a distinct type and an eight-byte object representation. Parser
     * admission and target lowering are staged separately so 32-bit backends
     * never silently truncate it.
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
    type_t *TY_size = add_named_type("size_t");
    TY_size->base_type = PTR_SIZE == 8 ? TYPE_long_long : TYPE_long;
    TY_size->size = PTR_SIZE;
    TY_size->is_unsigned = true;

    type_t *TY_ptrdiff = add_named_type("ptrdiff_t");
    TY_ptrdiff->base_type = PTR_SIZE == 8 ? TYPE_long_long : TYPE_long;
    TY_ptrdiff->size = PTR_SIZE;

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
    type_t *TY_wchar = add_named_type("wchar_t");
    TY_wchar->base_type = TYPE_int;
    TY_wchar->size = TY_int->size;

    type_t *TY_sig_atomic = add_named_type("sig_atomic_t");
    TY_sig_atomic->base_type = TYPE_int;
    TY_sig_atomic->size = TY_int->size;

    type_t *TY_wint = add_named_type("wint_t");
    TY_wint->base_type = TYPE_int;
    TY_wint->size = TY_int->size;
    TY_wint->is_unsigned = true;

    /* C99 <stdint.h> aliases share the target scalar representations. Keep them
     * named in the builtin table so declarations, casts, sizeof, and prototypes
     * use the same ABI metadata as their underlying types.
     */
    type_t *TY_int8 = add_named_type("int8_t");
    TY_int8->base_type = TYPE_char;
    TY_int8->size = 1;
    TY_int8->is_signed_char = true;
    type_t *TY_uint8 = add_named_type("uint8_t");
    TY_uint8->base_type = TYPE_char;
    TY_uint8->size = 1;
    TY_uint8->is_unsigned = true;
    type_t *TY_int16 = add_named_type("int16_t");
    TY_int16->base_type = TYPE_short;
    TY_int16->size = 2;
    type_t *TY_uint16 = add_named_type("uint16_t");
    TY_uint16->base_type = TYPE_short;
    TY_uint16->size = 2;
    TY_uint16->is_unsigned = true;
    type_t *TY_int32 = add_named_type("int32_t");
    TY_int32->base_type = TYPE_int;
    TY_int32->size = 4;
    type_t *TY_uint32 = add_named_type("uint32_t");
    TY_uint32->base_type = TYPE_int;
    TY_uint32->size = 4;
    TY_uint32->is_unsigned = true;
    type_t *TY_int64 = add_named_type("int64_t");
    TY_int64->base_type = TYPE_long_long;
    TY_int64->size = 8;
    type_t *TY_uint64 = add_named_type("uint64_t");
    TY_uint64->base_type = TYPE_long_long;
    TY_uint64->size = 8;
    TY_uint64->is_unsigned = true;
    type_t *TY_intmax = add_named_type("intmax_t");
    TY_intmax->base_type = TYPE_long_long;
    TY_intmax->size = 8;
    type_t *TY_uintmax = add_named_type("uintmax_t");
    TY_uintmax->base_type = TYPE_long_long;
    TY_uintmax->size = 8;
    TY_uintmax->is_unsigned = true;
    type_t *TY_intptr = add_named_type("intptr_t");
    TY_intptr->base_type = PTR_SIZE == 8 ? TYPE_long_long : TYPE_long;
    TY_intptr->size = PTR_SIZE;
    type_t *TY_uintptr = add_named_type("uintptr_t");
    TY_uintptr->base_type = PTR_SIZE == 8 ? TYPE_long_long : TYPE_long;
    TY_uintptr->size = PTR_SIZE;
    TY_uintptr->is_unsigned = true;

    type_t *TY_int_least8 = add_named_type("int_least8_t");
    TY_int_least8->base_type = TY_int8->base_type;
    TY_int_least8->size = TY_int8->size;
    TY_int_least8->is_signed_char = true;
    type_t *TY_uint_least8 = add_named_type("uint_least8_t");
    TY_uint_least8->base_type = TY_uint8->base_type;
    TY_uint_least8->size = TY_uint8->size;
    TY_uint_least8->is_unsigned = true;
    type_t *TY_int_least16 = add_named_type("int_least16_t");
    TY_int_least16->base_type = TYPE_short;
    TY_int_least16->size = 2;
    type_t *TY_uint_least16 = add_named_type("uint_least16_t");
    TY_uint_least16->base_type = TYPE_short;
    TY_uint_least16->size = 2;
    TY_uint_least16->is_unsigned = true;
    type_t *TY_int_least32 = add_named_type("int_least32_t");
    TY_int_least32->base_type = TYPE_int;
    TY_int_least32->size = 4;
    type_t *TY_uint_least32 = add_named_type("uint_least32_t");
    TY_uint_least32->base_type = TYPE_int;
    TY_uint_least32->size = 4;
    TY_uint_least32->is_unsigned = true;
    type_t *TY_int_least64 = add_named_type("int_least64_t");
    TY_int_least64->base_type = TYPE_long_long;
    TY_int_least64->size = 8;
    type_t *TY_uint_least64 = add_named_type("uint_least64_t");
    TY_uint_least64->base_type = TYPE_long_long;
    TY_uint_least64->size = 8;
    TY_uint_least64->is_unsigned = true;
    type_t *TY_int_fast8 = add_named_type("int_fast8_t");
    TY_int_fast8->base_type = TYPE_int;
    TY_int_fast8->size = 4;
    type_t *TY_uint_fast8 = add_named_type("uint_fast8_t");
    TY_uint_fast8->base_type = TYPE_int;
    TY_uint_fast8->size = 4;
    TY_uint_fast8->is_unsigned = true;
    type_t *TY_int_fast16 = add_named_type("int_fast16_t");
    TY_int_fast16->base_type = TYPE_int;
    TY_int_fast16->size = 4;
    type_t *TY_uint_fast16 = add_named_type("uint_fast16_t");
    TY_uint_fast16->base_type = TYPE_int;
    TY_uint_fast16->size = 4;
    TY_uint_fast16->is_unsigned = true;
    type_t *TY_int_fast32 = add_named_type("int_fast32_t");
    TY_int_fast32->base_type = TYPE_int;
    TY_int_fast32->size = 4;
    type_t *TY_uint_fast32 = add_named_type("uint_fast32_t");
    TY_uint_fast32->base_type = TYPE_int;
    TY_uint_fast32->size = 4;
    TY_uint_fast32->is_unsigned = true;
    type_t *TY_int_fast64 = add_named_type("int_fast64_t");
    TY_int_fast64->base_type = TYPE_long_long;
    TY_int_fast64->size = 8;
    type_t *TY_uint_fast64 = add_named_type("uint_fast64_t");
    TY_uint_fast64->base_type = TYPE_long_long;
    TY_uint_fast64->size = 8;
    TY_uint_fast64->is_unsigned = true;

    /* builtin type _Bool was introduced in C99 specification, it is more
     * well-known as macro type bool, which is defined in <std_bool.h> (in
     * shecc, it is defined in 'lib/c.c').
     */
    TY_bool = add_named_type("_Bool");
    TY_bool->base_type = TYPE_char;
    TY_bool->size = 1;
    TY_bool->is_bool = true;

    GLOBAL_BLOCK = add_block(NULL, NULL); /* global block */
    elf_add_symbol("", 0);                /* undef symbol */

    if (dynlink) {
        /* In dynamic mode, __syscall won't be implemented.
         *
         * Simply declare a 'syscall' function as follows if the program needs
         * to use 'syscall':
         *
         * int syscall(int number, ...);
         *
         * shecc will treat it as an external function, and the compiled program
         * will eventually use the implementation provided by the external C
         * library.
         *
         * If shecc supports the 'long' data type in the future, it would be
         * better to declare syscall using its original prototype:
         *
         * long syscall(long number, ...);
         */
    } else {
        /* Linux syscall */
        func_t *func = add_func("__syscall", true);
        func->return_def.type = TY_int;
        func->num_params = 0;
        func->va_args = 1;
        func->bbs = NULL;
        /* Otherwise, allocate a basic block to implement in static mode. */
        func->bbs = arena_calloc(BB_ARENA, 1, sizeof(basic_block_t));
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
