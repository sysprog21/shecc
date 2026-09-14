/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* The sizeof operator and the type-only walks over its operands.
 *
 * A fragment of the parser: parser.c includes it in order, so it sees every
 * definition that precedes it there and cannot be compiled on its own.
 */

void read_expr_operand(block_t *parent, basic_block_t **bb);

/* Consume one adjacent-string-literal sequence and return the size of its C99
 * array object, including the terminating null byte. This is deliberately
 * independent of expression lowering: a string literal decays in an ordinary
 * expression, but not when it is the operand of sizeof.
 */
int read_sizeof_string_literal(void)
{
    char literal[MAX_STRING_LEN];
    char unescaped[MAX_STRING_LEN];
    int size = 1;

    do {
        int length;

        /* Count the decoded bytes the decoder reports: strlen() of the buffer
         * would stop at an embedded null character that is part of the array.
         */
        lex_ident(T_string, literal);
        length = unescape_string(literal, unescaped, MAX_STRING_LEN);
        if (length < 0)
            error_at("Invalid escape sequence", cur_token_loc());
        size += length;
    } while (lex_peek(T_string, NULL));

    return size;
}

/* Starting at the first token inside the parenthesis of "sizeof (", count the
 * grouping parentheses around an adjacent sequence of string literals of the
 * given kind.
 *
 * Return -1 unless the operand is exactly such a grouped literal sequence
 * closed by its groups and by sizeof's own parenthesis.
 */
int sizeof_grouped_literal_depth(token_t *token, token_kind_t kind)
{
    int depth = 0;

    while (token && token->kind == T_open_bracket) {
        depth++;
        token = token->next;
    }
    if (!token || token->kind != kind)
        return -1;
    while (token && token->kind == kind)
        token = token->next;
    for (int i = 0; i <= depth; i++) {
        if (!token || token->kind != T_close_bracket)
            return -1;
        token = token->next;
    }
    return depth;
}

/* Push the value of a sizeof expression. VLA is intentionally outside shecc's
 * C99 scope, so every admitted sizeof result is an integer constant expression;
 * mark it so constant folding and the null pointer constant test treat every
 * operand form alike.
 */
static void push_sizeof_result(block_t *parent, basic_block_t *bb, int size)
{
    var_t *result = require_var(parent);

    result->init_val = size;
    result->is_const = true;
    result->var_name = gen_name();
    opstack_push(result);
    add_insn(parent, bb, OP_load_constant, result, NULL, NULL, 0, NULL);
}

int sizeof_array_object(const var_t *array)
{
    int element_size = array->type->size;

    if (array->ptr_level || array->type->ptr_level || array->is_func)
        element_size = PTR_SIZE;
    return array->array_size * element_size;
}

/* What a type-only walk over a sizeof operand has seen. */
typedef struct {
    int operators;   /* unary, postfix and cast operators applied */
    int members;     /* record member selections */
    bool addressed;  /* the operand so far is "&array" */
    bool designator; /* the operand so far designates a function */
    bool rvalue;     /* the operand so far is a cast result, not an lvalue */
} sizeof_walk_t;

/* Callback typedefs keep their function types in metadata the walk does not
 * rebuild; indirections through them stay with ordinary expression lowering.
 */
static bool sizeof_walk_function_type(const type_t *type)
{
    return type->func_signature || type->pointee_func_signature ||
           type->is_direct_function_type;
}

/* Apply one indirection, from '*', '[]' or '->', to the walked operand: an
 * array selects its first element, a pointer its pointee, and a callback object
 * the function it designates.
 *
 * Return false for a form the walk does not model, and reject an operand that
 * has no pointer or array type with @message.
 */
static bool sizeof_walk_indirect(var_t *object,
                                 sizeof_walk_t *walk,
                                 char *message,
                                 token_t *op)
{
    bool addressed = walk->addressed;
    int depth;

    walk->addressed = false;
    walk->rvalue = false;
    if (walk->designator)
        return true;
    if (object->array_size > 0) {
        fixed_array_shape_t shape = fixed_array_shape_from_var(object);

        fixed_array_shape_drop_outer(&shape);
        fixed_array_shape_to_var(object, &shape);
        return true;
    }
    if (object->has_unsized_array || object->is_flexible_array_member) {
        object->has_unsized_array = false;
        object->is_flexible_array_member = false;
        return true;
    }
    if (object->pointee_func_signature ||
        sizeof_walk_function_type(object->type))
        return false;
    depth = effective_pointer_depth(object);
    if (object->is_func && !depth) {
        walk->designator = true;
        return true;
    }
    if (depth <= 0)
        error_at(message, &op->location);
    if (object->pointee_array_size > 0 &&
        depth == object->pointee_array_element_ptr_level + 1) {
        /* A pointer to an array designates the complete array, so the
         * dereference restores its bounds rather than decaying them.
         */
        fixed_array_shape_t shape = fixed_array_shape_from_pointee_var(object);

        if (!addressed && object->type->pointee_array_element_type)
            object->type = object->type->pointee_array_element_type;
        fixed_array_shape_to_var(object, &shape);
        object->ptr_level = object->pointee_array_element_ptr_level;
        object->pointee_array_size = 0;
        object->pointee_array_dim2 = 0;
        object->pointee_array_dim3 = 0;
        object->pointee_array_dim4 = 0;
        object->pointee_array_element_ptr_level = 0;
    } else {
        object->type = pointee_type_from_pointer_typedef(object->type);
        object->ptr_level = depth - 1;
    }
    return true;
}

/* Apply '&' to the walked operand. */
static void sizeof_walk_address(var_t *object, sizeof_walk_t *walk, token_t *op)
{
    if (walk->designator) {
        /* The address of a function is a callback object. */
        walk->designator = false;
        object->is_func = true;
        return;
    }
    if (walk->rvalue)
        error_at("lvalue required as unary '&' operand", &op->location);
    if (is_bitfield(object))
        error_at("cannot take address of bit-field", &op->location);
    if (object->is_register)
        error_at("cannot take address of register object", &op->location);
    object->has_unsized_array = false;
    object->is_flexible_array_member = false;
    walk->addressed = object->array_size > 0;
    if (walk->addressed) {
        fixed_array_shape_t shape = fixed_array_shape_from_var(object);
        fixed_array_shape_t scalar = {0};

        fixed_array_shape_to_pointee_var(object, &shape);
        object->pointee_array_element_ptr_level = object->ptr_level;
        fixed_array_shape_to_var(object, &scalar);
    }
    object->ptr_level++;
}

/* Say whether the token after a '(' in a sizeof operand begins a type name, so
 * that the parenthesis opens a cast rather than a grouping.
 */
static bool sizeof_cast_starts_at(block_t *scope, const token_t *token)
{
    if (!token)
        return false;
    if (token->kind == T_identifier)
        return find_visible_type(token->literal, scope);
    return token->kind == T_struct || token->kind == T_union ||
           token->kind == T_enum || token->kind == T_signed ||
           token->kind == T_unsigned || token->kind == T_long ||
           token->kind == T_const || token->kind == T_volatile ||
           token->kind == T_float || token->kind == T_double;
}

/* Consume the type name and ')' of a cast whose '(' is already consumed. Only
 * integer, pointer and void types are modeled; return NULL for any other.
 */
static type_t *read_sizeof_cast_type(block_t *scope, int *ptr_level)
{
    type_t *type = read_type_name_specifiers(scope);
    int stars = 0;

    while (type && lex_accept(T_asterisk)) {
        stars++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }
    *ptr_level = stars;
    if (!type || type->is_floating || type->array_size ||
        sizeof_walk_function_type(type) || (!stars && is_record_type(type)) ||
        !lex_accept(T_close_bracket))
        return NULL;
    return type;
}

/* A sizeof operand needs the declared type of an lvalue, before ordinary
 * expression lowering decays an array or evaluates a postfix operand. Keep this
 * descriptor as a copy of the declaration metadata: no IR value is ever
 * constructed while walking it.
 *
 * The walk covers identifiers, grouping, '*', '&', casts, member selections and
 * subscripts. It returns false, without consuming anything, for an operand it
 * does not model, which then keeps the ordinary unevaluated-expression path.
 * @allow_cast is false for the operand of an unparenthesized sizeof: in "sizeof
 * (T) x" the parenthesis holds a type name, and a cast expression is not a
 * unary expression.
 */
static bool scan_sizeof_postfix_operand(block_t *scope,
                                        token_t **cursor,
                                        var_t *object,
                                        sizeof_walk_t *walk,
                                        bool allow_cast)
{
    token_t *token = *cursor;
    token_t *op = token;

    if (!token)
        return false;
    if (token->kind == T_asterisk) {
        token = token->next;
        if (!scan_sizeof_postfix_operand(scope, &token, object, walk, true) ||
            !sizeof_walk_indirect(
                object, walk, "Cannot dereference non-pointer in sizeof", op))
            return false;
        walk->operators++;
    } else if (token->kind == T_ampersand) {
        token = token->next;
        if (!scan_sizeof_postfix_operand(scope, &token, object, walk, true))
            return false;
        sizeof_walk_address(object, walk, op);
        walk->operators++;
    } else if (token->kind == T_open_bracket &&
               sizeof_cast_starts_at(scope, token->next)) {
        token_t *saved_token = cur_token;
        var_t operand;
        sizeof_walk_t operand_walk = {0};
        type_t *type;
        int ptr_level;

        if (!allow_cast)
            return false;

        /* Borrow the lexer to read the type name, then put it back. */
        cur_token = token;
        type = read_sizeof_cast_type(scope, &ptr_level);
        token = cur_token->next;
        cur_token = saved_token;
        if (!type)
            return false;

        /* A literal operand, as in "(struct S *) 0", does not affect the type
         * of the cast either.
         */
        if (token && (token->kind == T_numeric || token->kind == T_char)) {
            token = token->next;
            if (token && (token->kind == T_dot || token->kind == T_arrow ||
                          token->kind == T_open_square))
                return false;
        } else if (!scan_sizeof_postfix_operand(scope, &token, &operand,
                                                &operand_walk, true))
            return false;
        memset(object, 0, sizeof(*object));
        object->type = type;
        object->ptr_level = ptr_level;
        walk->addressed = false;
        walk->designator = false;
        walk->rvalue = true;
        walk->operators++;
    } else if (token->kind == T_open_bracket) {
        token = token->next;
        if (!scan_sizeof_postfix_operand(scope, &token, object, walk, true) ||
            !token || token->kind != T_close_bracket)
            return false;
        token = token->next;
    } else if (token->kind == T_identifier) {
        var_t *root = find_var(token->literal, scope);
        func_t *target = find_visible_func(token->literal, scope);
        func_t *func = scope->func;

        /* C99 6.7.4 constrains every reference an external inline definition
         * makes, including one sizeof does not evaluate.
         */
        bool external_inline = func && func->is_inline && !func->is_static;

        if (root && !root->is_extern_function_alias) {
            if (root->is_direct_function_declarator ||
                (root->type->is_direct_function_type &&
                 !effective_pointer_depth(root)))
                return false;
            if (external_inline && root->is_static &&
                root->scope == GLOBAL_BLOCK)
                error_at(
                    "external inline definition references "
                    "internal-linkage object",
                    &token->location);
            memcpy(object, root, sizeof(*object));
        } else if (target) {
            if (external_inline && target->is_static)
                error_at(
                    "external inline definition references "
                    "internal-linkage function",
                    &token->location);
            memset(object, 0, sizeof(*object));
            object->type = TY_int;
            object->is_func = true;
            walk->designator = true;
        } else
            return false;
        token = token->next;
    } else
        return false;

    while (token && (token->kind == T_dot || token->kind == T_arrow ||
                     token->kind == T_open_square)) {
        op = token;
        walk->operators++;
        if (walk->designator)
            return false;
        if (token->kind == T_dot || token->kind == T_arrow) {
            bool rvalue = walk->rvalue;
            var_t *field;

            token = token->next;
            if (!token || token->kind != T_identifier)
                return false;
            if (op->kind == T_arrow) {
                if (!sizeof_walk_indirect(
                        object, walk,
                        "Member reference through '->' requires a pointer", op))
                    return false;
                rvalue = false;
            }
            if (walk->designator || effective_pointer_depth(object) ||
                is_array_declarator(object) || object->has_unsized_array ||
                !is_record_type(object->type))
                error_at("Member reference base is not a struct or union",
                         &op->location);
            field = find_member(token->literal, object->type);
            if (!field)
                error_at("Unknown struct or union member", &token->location);
            memcpy(object, field, sizeof(*object));
            walk->members++;
            walk->addressed = false;
            walk->rvalue = rvalue;
            token = token->next;
        } else {
            int depth = 0;

            /* The subscript itself is read, unevaluated, by the consuming pass;
             * only its extent matters here.
             */
            for (; token; token = token->next) {
                if (token->kind == T_open_square)
                    depth++;
                else if (token->kind == T_close_square && --depth == 0) {
                    token = token->next;
                    break;
                }
            }
            if (depth)
                return false;
            if (object->is_func && !is_array_declarator(object) &&
                !effective_pointer_depth(object))
                return false;
            if (!sizeof_walk_indirect(
                    object, walk, "Cannot apply square operator to non-pointer",
                    op))
                return false;
        }
    }
    *cursor = token;
    return true;
}

/* The preceding scan proves the exact token shape and type constraints before
 * this pass advances the lexer. Each subscript is read in a detached block so
 * its side effects remain unevaluated. @open_consumed says the operand's
 * opening parenthesis was already taken as the one following sizeof.
 */
static void consume_sizeof_postfix_operand(block_t *parent,
                                           basic_block_t **bb,
                                           bool open_consumed)
{
    if (open_consumed) {
        consume_sizeof_postfix_operand(parent, bb, false);
        lex_expect(T_close_bracket);
    } else if (lex_accept(T_asterisk) || lex_accept(T_ampersand))
        consume_sizeof_postfix_operand(parent, bb, false);
    else if (lex_peek(T_open_bracket, NULL) &&
             sizeof_cast_starts_at(parent, cur_token->next->next)) {
        int ptr_level;

        lex_expect(T_open_bracket);
        read_sizeof_cast_type(parent, &ptr_level);
        if (!lex_accept(T_numeric) && !lex_accept(T_char))
            consume_sizeof_postfix_operand(parent, bb, false);
    } else if (lex_accept(T_open_bracket)) {
        consume_sizeof_postfix_operand(parent, bb, false);
        lex_expect(T_close_bracket);
    } else
        lex_expect(T_identifier);

    while (lex_peek(T_dot, NULL) || lex_peek(T_arrow, NULL) ||
           lex_peek(T_open_square, NULL)) {
        if (lex_accept(T_dot) || lex_accept(T_arrow))
            lex_expect(T_identifier);
        else {
            basic_block_t *unevaluated_bb;
            int saved_side_effects;

            lex_expect(T_open_square);
            unevaluated_bb = bb_create(parent);
            saved_side_effects = se_idx;
            read_expr(parent, &unevaluated_bb);
            read_ternary_operation(parent, &unevaluated_bb);
            opstack_pop();
            se_idx = saved_side_effects;
            lex_expect(T_close_square);
        }
    }
}

/* The operand must end where sizeof's operand ends: at the closing parenthesis
 * of a parenthesized one, unless a postfix operator continues past it, and
 * otherwise before anything that would keep it going as a call or an update.
 */
static bool sizeof_operand_tail_ends(const token_t *tail, bool parenthesized)
{
    if (!tail)
        return false;
    if (!parenthesized)
        return tail->kind != T_open_bracket && tail->kind != T_increment &&
               tail->kind != T_decrement;
    tail = tail->kind == T_close_bracket ? tail->next : NULL;
    return tail && tail->kind != T_dot && tail->kind != T_arrow &&
           tail->kind != T_open_square && tail->kind != T_open_bracket &&
           tail->kind != T_increment && tail->kind != T_decrement;
}

/* A non-mutating admission check for callers that need to hand a global sizeof
 * operand to the detached local parser. It admits only a member array, so type
 * names and scalar forms remain owned by their existing global constant paths.
 */
static bool can_scan_sizeof_postfix_extent(block_t *scope,
                                           token_t *start,
                                           bool parenthesized,
                                           bool allow_flexible)
{
    token_t *tail = start;
    var_t object;
    sizeof_walk_t walk = {0};

    return scan_sizeof_postfix_operand(scope, &tail, &object, &walk,
                                       parenthesized) &&
           walk.members && sizeof_operand_tail_ends(tail, parenthesized) &&
           !walk.designator && !effective_pointer_depth(&object) &&
           (object.array_size > 0 ||
            (allow_flexible && object.is_flexible_array_member));
}

/* Walk a local sizeof operand from @start and say whether this walker owns it:
 * any operand it models that applies an operator, or that still designates an
 * array, possibly incomplete, or a callback object. Without a parenthesis it
 * owns a bare scalar identifier too: ordinary lowering of an identifier takes a
 * following "+ n" as pointer arithmetic, which made "sizeof p + 1" the size of
 * "p + 1". Inside one, that identifier keeps the unevaluated-expression path.
 */
static bool walk_local_sizeof_operand(block_t *parent,
                                      token_t *start,
                                      bool parenthesized,
                                      var_t *object,
                                      sizeof_walk_t *walk)
{
    token_t *tail = start;

    if (!scan_sizeof_postfix_operand(parent, &tail, object, walk,
                                     parenthesized) ||
        !sizeof_operand_tail_ends(tail, parenthesized))
        return false;
    return !parenthesized || walk->operators > 0 || object->array_size > 0 ||
           object->has_unsized_array || (object->is_func && !walk->designator);
}

/* Every sizeof operand made of an identifier, grouping, dereference, address,
 * cast, member selections and constant or runtime subscripts goes through here:
 * the result is the declared extent of what the operand designates, which
 * ordinary expression lowering would decay or evaluate.
 */
static bool read_sizeof_postfix_operand(block_t *parent,
                                        basic_block_t **bb,
                                        bool parenthesized)
{
    bool open_consumed = false;
    var_t object;
    sizeof_walk_t walk = {0};
    int size;

    if (!walk_local_sizeof_operand(parent, cur_token->next, parenthesized,
                                   &object, &walk)) {
        sizeof_walk_t regrouped = {0};

        /* In "sizeof (*p).member" the parenthesis taken as sizeof's own opens a
         * grouping inside a longer unary expression. Walk it again from that
         * parenthesis as an operand without one.
         */
        if (!parenthesized || cur_token->kind != T_open_bracket ||
            !walk_local_sizeof_operand(parent, cur_token, false, &object,
                                       &regrouped))
            return false;
        walk = regrouped;
        open_consumed = true;
    }
    if (walk.designator)
        error_at("sizeof(function) is invalid", cur_token_loc());
    if (object.is_flexible_array_member)
        error_at("sizeof cannot be applied to a flexible array member",
                 cur_token_loc());
    if (object.has_unsized_array)
        error_at("sizeof cannot be applied to an incomplete array",
                 cur_token_loc());
    if (is_bitfield(&object))
        error_at("sizeof cannot be applied to a bit-field", cur_token_loc());

    consume_sizeof_postfix_operand(parent, bb, open_consumed);
    if (parenthesized && !open_consumed)
        lex_expect(T_close_bracket);
    if (object.array_size > 0) {
        size = sizeof_array_object(&object);
    } else if (object.ptr_level || object.type->ptr_level || object.is_func) {
        size = PTR_SIZE;
    } else {
        if (object.type == TY_void)
            error_at("sizeof(void) is invalid", cur_token_loc());
        if (is_incomplete_record_object(&object))
            error_at("sizeof cannot be applied to an incomplete record type",
                     cur_token_loc());
        size = object.type->size;
    }
    push_sizeof_result(parent, *bb, size);
    return true;
}

static bool is_direct_fixed_array_pointer_slot(const var_t *pointer)
{
    return pointer && is_array_declarator(pointer) && !pointer->array_dim2 &&
           pointer->pointee_array_size > 0 &&
           !pointer->pointee_array_element_ptr_level &&
           ((pointer->ptr_level == 1 && !pointer->type->ptr_level) ||
            (!pointer->ptr_level && pointer->type->ptr_level == 1 &&
             pointer->type->array_element_ptr_level == 1));
}

void handle_sizeof_operator(block_t *parent, basic_block_t **bb)
{
    char token[MAX_ID_LEN];
    int ptr_cnt = 0;
    int array_size = 0;
    token_t *sizeof_tk = cur_token;
    type_t *type = NULL;
    bool is_function = false;

    bool parenthesized = lex_accept(T_open_bracket);

    /* A string literal is an array, not a pointer, before the array-to-pointer
     * conversion that ordinary expression lowering applies. Parenthesized
     * sizeof may take the fast path only when the literal sequence, possibly
     * grouped again, is the complete operand: neither sizeof's own parenthesis
     * nor any grouping inside it is part of the array's extent.
     */
    int string_groups = 0;
    int wstring_groups = 0;

    if (parenthesized) {
        string_groups = sizeof_grouped_literal_depth(cur_token->next, T_string);
        wstring_groups =
            sizeof_grouped_literal_depth(cur_token->next, T_wstring);
    }
    if ((lex_peek(T_string, NULL) || string_groups > 0) && string_groups >= 0) {
        int size;

        for (int i = 0; i < string_groups; i++)
            lex_expect(T_open_bracket);
        size = read_sizeof_string_literal();
        for (int i = 0; i < string_groups; i++)
            lex_expect(T_close_bracket);
        if (parenthesized)
            lex_expect(T_close_bracket);
        push_sizeof_result(parent, *bb, size);
        return;
    }
    if ((lex_peek(T_wstring, NULL) || wstring_groups > 0) &&
        wstring_groups >= 0) {
        int size;

        for (int i = 0; i < wstring_groups; i++)
            lex_expect(T_open_bracket);
        size = read_const_wstring_size();
        for (int i = 0; i < wstring_groups; i++)
            lex_expect(T_close_bracket);
        if (parenthesized)
            lex_expect(T_close_bracket);
        push_sizeof_result(parent, *bb, size);
        return;
    }

    if (read_sizeof_postfix_operand(parent, bb, parenthesized))
        return;

    /* The type-name alternative requires parentheses, but C99 also permits a
     * unary expression directly after sizeof. Keep direct array identifiers
     * from decaying before their extent is observed.
     */
    if (!parenthesized) {
        if (parent->func && lex_peek(T_identifier, token) &&
            !strcmp(token, "__func__")) {
            lex_expect(T_identifier);
            push_sizeof_result(parent, *bb,
                               strlen(parent->func->return_def.var_name) + 1);
            return;
        }

        if (lex_peek(T_identifier, token)) {
            var_t *array = find_var(token, parent);

            if (array && array->array_size > 0) {
                lex_expect(T_identifier);
                push_sizeof_result(parent, *bb, sizeof_array_object(array));
                return;
            }
        }

        basic_block_t *unevaluated_bb = bb_create(parent);
        int saved_side_effects = se_idx;
        unevaluated_expression_depth++;
        read_expr_operand(parent, &unevaluated_bb);
        unevaluated_expression_depth--;
        se_idx = saved_side_effects;
        var_t *expr_var = opstack_pop();
        if (is_bitfield(expr_var))
            error_at("sizeof cannot be applied to a bit-field",
                     &sizeof_tk->location);
        type = expr_var->type;
        ptr_cnt = expr_var->ptr_level;
        array_size = expr_var->array_size;
        is_function = expr_var->is_func;
        if (type == TY_void && ptr_cnt == 0)
            error_at("sizeof(void) is invalid", &sizeof_tk->location);
        if (is_function && ptr_cnt == 0)
            error_at("sizeof(function) is invalid", &sizeof_tk->location);

        int size = type->size;

        if (array_size > 0)
            size = array_size * type->size;
        if (ptr_cnt)
            size = PTR_SIZE;
        push_sizeof_result(parent, *bb, size);
        return;
    }

    /* C99 specifies __func__ as if each function contained a distinct `static
     * const char []` initialized with its unadorned name. The normal expression
     * lowering materializes its address, but sizeof must retain the array
     * extent rather than observing that decayed pointer.
     */
    if (parent->func && lex_peek(T_identifier, token) &&
        !strcmp(token, "__func__") && cur_token->next &&
        cur_token->next->next &&
        cur_token->next->next->kind == T_close_bracket) {
        lex_expect(T_identifier);
        lex_expect(T_close_bracket);
        push_sizeof_result(parent, *bb,
                           strlen(parent->func->return_def.var_name) + 1);
        return;
    }

    /* A bare array identifier is the one expression form that must retain its
     * declared extent for sizeof; ordinary expression parsing intentionally
     * decays it to a pointer. cur_token is the opening parenthesis here.
     */
    if (lex_peek(T_identifier, token) && cur_token->next &&
        cur_token->next->next &&
        cur_token->next->next->kind == T_close_bracket) {
        var_t *array = find_var(token, parent);

        if (array && array->array_size > 0) {
            lex_expect(T_identifier);
            lex_expect(T_close_bracket);
            push_sizeof_result(parent, *bb, sizeof_array_object(array));
            return;
        }
    }

    /* Check if this is sizeof(type) or sizeof(expression) */
    bool has_signed_type = false;
    bool has_unsigned_type = false;
    int long_type_count = 0;
    type_t *leading_scalar_type = NULL;

    if (lex_peek(T_identifier, token) &&
        (!strcmp(token, "char") || !strcmp(token, "short") ||
         !strcmp(token, "int"))) {
        token_t *after_base = cur_token->next->next;

        while (after_base && after_base->kind == T_const)
            after_base = after_base->next;
        if (after_base &&
            (after_base->kind == T_signed || after_base->kind == T_unsigned)) {
            lex_expect(T_identifier);
            leading_scalar_type = find_type(token, true);
        }
    }
    while (lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
           lex_peek(T_long, NULL) || lex_peek(T_const, NULL) ||
           lex_peek(T_volatile, NULL)) {
        if (lex_accept(T_signed)) {
            if (has_signed_type)
                error_at("duplicate signed type specifier", cur_token_loc());
            has_signed_type = true;
        } else if (lex_accept(T_unsigned)) {
            if (has_unsigned_type)
                error_at("duplicate unsigned type specifier", cur_token_loc());
            has_unsigned_type = true;
        } else if (lex_accept(T_long))
            long_type_count++;
        else if (lex_accept(T_const))
            ;
        else
            lex_expect(T_volatile);
    }
    if (long_type_count > 2)
        error_at("too many long type specifiers", cur_token_loc());
    if (has_signed_type && has_unsigned_type)
        error_at("both signed and unsigned specified", cur_token_loc());
    if (leading_scalar_type == TY_int && lex_peek(T_identifier, token) &&
        !strcmp(token, "char"))
        error_at("int cannot be combined with char", cur_token_loc());
    bool has_long_type = long_type_count > 0;
    bool has_enum_type = lex_accept(T_enum);
    if (has_enum_type &&
        (has_signed_type || has_unsigned_type || has_long_type))
        error_at("enum type cannot be combined with integer specifiers",
                 cur_token_loc());
    base_type_t record_kind = accept_record_keyword();

    /* `sizeof` only consumes object representation metadata, so it can admit
     * the C99 floating type names before value arithmetic and ABI lowering
     * exist. Keep all expression/declaration admission gates intact.
     */
    if (lex_accept(T_float)) {
        if (has_signed_type || has_unsigned_type || has_long_type ||
            has_enum_type || record_kind)
            error_at("invalid float type specifiers", cur_token_loc());
        type = TY_float;
    } else if (lex_accept(T_double)) {
        if (has_signed_type || has_unsigned_type || has_enum_type ||
            record_kind || long_type_count > 1)
            error_at("invalid double type specifiers", cur_token_loc());
        type = has_long_type ? TY_long_double : TY_double;
    } else if (has_enum_type) {
        lex_ident(T_identifier, token);
        type = reference_enum_tag(token, parent);
    } else if (has_long_type) {
        type = has_unsigned_type ? TY_ulong : TY_long;
        if (long_type_count > 1) {
            /* sizeof only consumes type metadata. It does not materialize a
             * long-long value, so 32-bit targets can correctly report the
             * required eight-byte object size before their paired-register
             * value ABI is implemented.
             */
            if (has_unsigned_type)
                type = TY_ulong_long;
            else
                type = TY_long_long;
        }
        lex_accept(T_signed);
        lex_accept(T_const);
        if (lex_peek(T_identifier, token) && !strcmp(token, "int"))
            lex_expect(T_identifier);
    } else if (has_unsigned_type) {
        if (leading_scalar_type == TY_char) {
            type = TY_uchar;
        } else if (leading_scalar_type == TY_short) {
            if (lex_peek(T_identifier, token) && !strcmp(token, "int"))
                lex_expect(T_identifier);
            type = TY_ushort;
        } else if (lex_peek(T_identifier, token) &&
                   (!strcmp(token, "int") || !strcmp(token, "char") ||
                    !strcmp(token, "short"))) {
            lex_expect(T_identifier);
            if (!strcmp(token, "char"))
                type = TY_uchar;
            else if (!strcmp(token, "short"))
                type = TY_ushort;
            else
                type = TY_uint;
        } else
            type = TY_uint;
    } else if (has_signed_type) {
        if (leading_scalar_type == TY_char || leading_scalar_type == TY_short) {
            if (leading_scalar_type == TY_short &&
                lex_peek(T_identifier, token) && !strcmp(token, "int"))
                lex_expect(T_identifier);
            type =
                leading_scalar_type == TY_char ? TY_schar : leading_scalar_type;
        } else if (lex_peek(T_identifier, token) &&
                   (!strcmp(token, "int") || !strcmp(token, "char") ||
                    !strcmp(token, "short"))) {
            lex_expect(T_identifier);
            type = !strcmp(token, "char") ? TY_schar : find_type(token, true);
        } else
            type = TY_int;
    } else if (leading_scalar_type) {
        type = leading_scalar_type;
    } else if (lex_peek(T_identifier, token)) {
        /* Try to parse as a type first */
        type = record_kind ? find_record_tag(token, parent, record_kind)
                           : find_visible_type(token, parent);
        if (type) {
            /* sizeof(type) */
            lex_expect(T_identifier);
        }
    }

    /* A type name continues with its abstract declarator; only the derivation
     * it applies last decides the object size.
     */
    if (type) {
        int elements;
        sizeof_derivation_t derivation =
            read_sizeof_abstract_declarator(parent, &elements);
        int size = sizeof_type_name_size(type, derivation, elements);

        lex_expect(T_close_bracket);
        push_sizeof_result(parent, *bb, size);
        return;
    }

    /* sizeof(expression) - parse the expression and get its type */
    basic_block_t *unevaluated_bb = bb_create(parent);
    int saved_side_effects = se_idx;
    unevaluated_expression_depth++;
    if (!read_assignment_expression(parent, &unevaluated_bb)) {
        read_expr(parent, &unevaluated_bb);
        read_ternary_operation(parent, &unevaluated_bb);
    }
    unevaluated_expression_depth--;
    se_idx = saved_side_effects;
    var_t *expr_var = opstack_pop();
    if (is_bitfield(expr_var))
        error_at("sizeof cannot be applied to a bit-field",
                 &sizeof_tk->location);
    type = expr_var->type;
    ptr_cnt = expr_var->ptr_level;
    array_size = expr_var->array_size;
    is_function = expr_var->is_func;
    if (is_incomplete_record_object(expr_var))
        error_at("sizeof cannot be applied to an incomplete record type",
                 &sizeof_tk->location);

    if (!type)
        error_at("Unable to determine type in sizeof", &sizeof_tk->location);
    if (type == TY_void && ptr_cnt == 0)
        error_at("sizeof(void) is invalid", &sizeof_tk->location);
    if ((is_function || type->is_direct_function_type) && ptr_cnt == 0)
        error_at("sizeof(function) is invalid", &sizeof_tk->location);

    if (!array_size && !ptr_cnt && type->array_size)
        array_size = type->array_size;
    int size = type->size;

    if (array_size > 0)
        size = array_size * type->size;
    if (ptr_cnt)
        size = PTR_SIZE;
    lex_expect(T_close_bracket);
    push_sizeof_result(parent, *bb, size);
}
