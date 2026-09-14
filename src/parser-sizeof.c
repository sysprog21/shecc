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
    int members;       /* record member selections */
    int subscripts;    /* array subscripts */
    bool element_leaf; /* the last subscript selected a non-array element */
    bool addressed;    /* the operand so far is "&array" */
} sizeof_walk_t;

/* A sizeof operand needs the declared type of an lvalue, before ordinary
 * expression lowering decays an array or evaluates a postfix operand. Keep this
 * descriptor as a copy of the declaration metadata: no IR value is ever
 * constructed while walking it.
 */
static bool scan_sizeof_postfix_operand(block_t *scope,
                                        token_t **cursor,
                                        var_t *object,
                                        sizeof_walk_t *walk)
{
    token_t *token = *cursor;

    if (!token)
        return false;
    if (token->kind == T_asterisk) {
        bool addressed;
        int depth;

        token = token->next;
        if (!scan_sizeof_postfix_operand(scope, &token, object, walk))
            return false;
        addressed = walk->addressed;
        depth = effective_pointer_depth(object);
        if (depth <= 0)
            return false;
        if (object->pointee_array_size > 0 &&
            depth == object->pointee_array_element_ptr_level + 1) {
            /* A pointer to an array designates the complete array, so the
             * dereference restores its bounds rather than decaying them.
             */
            fixed_array_shape_t shape =
                fixed_array_shape_from_pointee_var(object);

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
        walk->addressed = false;
        walk->element_leaf = false;
    } else if (token->kind == T_ampersand) {
        token = token->next;
        if (!scan_sizeof_postfix_operand(scope, &token, object, walk))
            return false;
        walk->addressed = object->array_size > 0;
        if (walk->addressed) {
            fixed_array_shape_t shape = fixed_array_shape_from_var(object);
            fixed_array_shape_t scalar = {0};

            fixed_array_shape_to_pointee_var(object, &shape);
            object->pointee_array_element_ptr_level = object->ptr_level;
            fixed_array_shape_to_var(object, &scalar);
        }
        object->ptr_level++;
        walk->element_leaf = false;
    } else if (token->kind == T_open_bracket) {
        token = token->next;
        if (!scan_sizeof_postfix_operand(scope, &token, object, walk) ||
            !token || token->kind != T_close_bracket)
            return false;
        token = token->next;
    } else if (token->kind == T_identifier) {
        var_t *root = find_var(token->literal, scope);

        if (!root)
            return false;
        memcpy(object, root, sizeof(*object));
        token = token->next;
    } else
        return false;

    while (token && (token->kind == T_dot || token->kind == T_arrow ||
                     token->kind == T_open_square)) {
        walk->addressed = false;
        if (token->kind == T_dot || token->kind == T_arrow) {
            bool through_pointer = token->kind == T_arrow;
            type_t *record;
            var_t *field;

            token = token->next;
            if (!token || token->kind != T_identifier ||
                (through_pointer && effective_pointer_depth(object) != 1) ||
                (!through_pointer && effective_pointer_depth(object) != 0))
                return false;
            record = through_pointer
                         ? pointee_type_from_pointer_typedef(object->type)
                         : object->type;
            if (!is_record_type(record))
                return false;
            field = find_member(token->literal, record);
            if (!field)
                return false;
            memcpy(object, field, sizeof(*object));
            walk->members++;
            walk->element_leaf = false;
            token = token->next;
        } else {
            int depth = 0;
            fixed_array_shape_t shape;

            /* An array may legitimately have pointer elements. Reject a scalar
             * pointer here, but retain the element pointer depth until after
             * this array subscript so a following `->` can consume it.
             */
            if (object->array_size <= 0)
                return false;
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
            shape = fixed_array_shape_from_var(object);
            if (!fixed_array_shape_drop_outer(&shape))
                return false;
            fixed_array_shape_to_var(object, &shape);
            walk->subscripts++;
            walk->element_leaf = !object->array_size;
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
    else if (lex_accept(T_open_bracket)) {
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
 * of a parenthesized one, and otherwise before anything that would keep it
 * going as a call or an update.
 */
static bool sizeof_operand_tail_ends(const token_t *tail, bool parenthesized)
{
    return tail && (parenthesized ? tail->kind == T_close_bracket
                                  : tail->kind != T_open_bracket &&
                                        tail->kind != T_increment &&
                                        tail->kind != T_decrement);
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

    return scan_sizeof_postfix_operand(scope, &tail, &object, &walk) &&
           walk.members && sizeof_operand_tail_ends(tail, parenthesized) &&
           !effective_pointer_depth(&object) &&
           (object.array_size > 0 ||
            (allow_flexible && object.is_flexible_array_member));
}

/* Walk a local sizeof operand from @start and say whether this walker owns it:
 * an operand that still designates an array, whatever the path to it, or a
 * single element selected from one. Anything else keeps the ordinary
 * unevaluated-expression path.
 */
static bool walk_local_sizeof_operand(block_t *parent,
                                      token_t *start,
                                      bool parenthesized,
                                      var_t *object)
{
    token_t *tail = start;
    sizeof_walk_t walk = {0};

    if (!scan_sizeof_postfix_operand(parent, &tail, object, &walk) ||
        !sizeof_operand_tail_ends(tail, parenthesized))
        return false;
    if (object->is_flexible_array_member)
        return walk.members > 0;
    return object->array_size > 0 || walk.element_leaf;
}

/* Every sizeof operand made of an identifier, grouping, dereference, address,
 * member selections and constant or runtime subscripts goes through here: the
 * result is the declared extent of what the operand designates, which ordinary
 * expression lowering would decay or evaluate.
 */
static bool read_sizeof_postfix_operand(block_t *parent,
                                        basic_block_t **bb,
                                        bool parenthesized)
{
    bool open_consumed = false;
    var_t object;
    int size;

    if (!walk_local_sizeof_operand(parent, cur_token->next, parenthesized,
                                   &object)) {
        /* In "sizeof (*p).member" the parenthesis taken as sizeof's own opens a
         * grouping inside a longer unary expression. Walk it again from that
         * parenthesis as an operand without one.
         */
        if (!parenthesized || cur_token->kind != T_open_bracket ||
            !walk_local_sizeof_operand(parent, cur_token, false, &object))
            return false;
        open_consumed = true;
    }
    if (object.is_flexible_array_member)
        error_at("sizeof cannot be applied to a flexible array member",
                 cur_token_loc());
    if (is_bitfield(&object))
        error_at("sizeof cannot be applied to a bit-field", cur_token_loc());

    consume_sizeof_postfix_operand(parent, bb, open_consumed);
    if (parenthesized && !open_consumed)
        lex_expect(T_close_bracket);
    if (object.array_size > 0)
        size = sizeof_array_object(&object);
    else if (object.ptr_level || object.type->ptr_level || object.is_func)
        size = PTR_SIZE;
    else
        size = object.type->size;
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
    int array_element_size = 0;
    token_t *sizeof_tk = cur_token;
    type_t *type = NULL;
    bool is_function = false;

    bool parenthesized = lex_accept(T_open_bracket);

    /* A string literal is an array, not a pointer, before the array-to-pointer
     * conversion that ordinary expression lowering applies. Parenthesized
     * sizeof may take the fast path only when the literal sequence is the
     * complete operand.
     */
    token_t *after_string = cur_token->next;
    while (after_string && after_string->kind == T_string)
        after_string = after_string->next;
    if (lex_peek(T_string, NULL) &&
        (!parenthesized ||
         (after_string && after_string->kind == T_close_bracket))) {
        int size = read_sizeof_string_literal();

        if (parenthesized)
            lex_expect(T_close_bracket);
        push_sizeof_result(parent, *bb, size);
        return;
    }

    token_t *after_wstring = cur_token->next;
    while (after_wstring && after_wstring->kind == T_wstring)
        after_wstring = after_wstring->next;
    if (lex_peek(T_wstring, NULL) &&
        (!parenthesized ||
         (after_wstring && after_wstring->kind == T_close_bracket))) {
        int size = read_const_wstring_size();

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
        read_expr_operand(parent, &unevaluated_bb);
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
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (find_type_flag == 1 && lex_accept(T_union))
        find_type_flag = 2;

    /* `sizeof` only consumes object representation metadata, so it can admit
     * the C99 floating type names before value arithmetic and ABI lowering
     * exist. Keep all expression/declaration admission gates intact.
     */
    if (lex_accept(T_float)) {
        if (has_signed_type || has_unsigned_type || has_long_type ||
            has_enum_type || find_type_flag != 1)
            error_at("invalid float type specifiers", cur_token_loc());
        type = TY_float;
    } else if (lex_accept(T_double)) {
        if (has_signed_type || has_unsigned_type || has_enum_type ||
            find_type_flag != 1 || long_type_count > 1)
            error_at("invalid double type specifiers", cur_token_loc());
        type = has_long_type ? TY_long_double : TY_double;
    } else if (has_enum_type) {
        lex_ident(T_identifier, token);
        type = find_type_tag(token, parent);
        if (!type)
            error_at("Unknown enum type", cur_token_loc());
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
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
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
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
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
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
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
    } else if (leading_scalar_type) {
        type = leading_scalar_type;
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
    } else if (lex_peek(T_identifier, token)) {
        /* Try to parse as a type first */
        type = find_type_flag == 2 ? find_type(token, find_type_flag)
                                   : find_visible_type(token, parent);
        if (type) {
            /* sizeof(type) */
            lex_expect(T_identifier);
            while (lex_accept(T_asterisk)) {
                ptr_cnt++;
                while (lex_accept(T_const) || lex_accept(T_volatile) ||
                       lex_accept(T_restrict))
                    ;
            }
        }
    }

    /* The integer branches above consume their own abstract pointer declarators
     * for historical reasons. Float/double type names select a type before that
     * legacy code, so finish the common suffix here.
     */
    while (type && lex_accept(T_asterisk)) {
        ptr_cnt++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }

    /* VLA support is intentionally outside this compiler's C99 scope, but a
     * type name in sizeof may still carry fixed abstract array bounds. Keep the
     * simple direct form here rather than treating '[' as an expression token
     * after the scalar type name.
     */
    while (type && ptr_cnt == 0 && lex_accept(T_open_square)) {
        int bound = read_const_expr(parent);

        lex_expect(T_close_square);
        if (bound <= 0)
            error_at("sizeof array type needs a positive constant bound",
                     cur_token_loc());
        if (bound > 0 && array_size && array_size > INT_MAX / bound)
            error_at("sizeof array type is too large", cur_token_loc());
        if (bound > 0)
            array_size = array_size ? array_size * bound : bound;
    }

    if (type && lex_accept(T_open_bracket)) {
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
                    parent, &nested_outer_array_size);
            if (nested_outer_array_size) {
                array_size = nested_outer_array_size;
                array_element_size = PTR_SIZE;
            } else
                ptr_cnt += nested_ptr_count + nested_function_ptr_count;
        } else
            while (lex_accept(T_open_square)) {
                int bound = read_const_expr(parent);

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

                /* The grouped array declarator in `int (*[2][3])(int)` leaves
                 * its pointed-to function suffix outside the group. sizeof only
                 * needs pointer-sized elements, but still must parse that valid
                 * C99 prototype completely.
                 */
                if (lex_peek(T_open_bracket, NULL))
                    read_sizeof_function_prototype();
            } else {
                ptr_cnt += nested_ptr_count;
                while (lex_accept(T_open_square)) {
                    int bound = read_const_expr(parent);

                    lex_expect(T_close_square);
                    if (bound <= 0)
                        error_at(
                            "sizeof array type needs a positive constant bound",
                            cur_token_loc());
                }

                /* A pointer declarator followed by a parameter list names a
                 * function pointer. sizeof observes the pointer object, not the
                 * function type, so no expression lowering or function ABI is
                 * needed. Reuse the declaration parser for prototype syntax.
                 */
                if (lex_peek(T_open_bracket, NULL))
                    read_sizeof_function_prototype();
            }
        }
    }

    if (!type) {
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
    }

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
        size =
            array_size * (array_element_size ? array_element_size : type->size);
    if (ptr_cnt)
        size = PTR_SIZE;
    lex_expect(T_close_bracket);
    push_sizeof_result(parent, *bb, size);
}
