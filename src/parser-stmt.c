/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Statements, and declarations and record, enum and typedef definitions in
 * block scope.
 *
 * A fragment of the parser: parser.c includes it in order, so it sees every
 * definition that precedes it there and cannot be compiled on its own.
 */

void perform_side_effect(block_t *parent, basic_block_t *bb)
{
    for (int i = 0; i < se_idx; i++) {
        insn_t *insn = &side_effect[i];
        add_insn(parent, bb, insn->opcode, insn->rd, insn->rs1, insn->rs2,
                 insn->sz, insn->str);
    }
    se_idx = 0;
}

basic_block_t *read_code_block(func_t *func,
                               block_t *parent,
                               basic_block_t *bb);

/* A switch dispatch is built while its labeled statements are read. Keep the
 * deferred no-match edge separate from the source-order body chain: labels may
 * occur after ordinary statements or inside nested compound statements.
 */
typedef struct switch_label_context {
    block_t *dispatch_parent;
    var_t *control;
    basic_block_t *dispatch_tail;
    basic_block_t *default_body;
    switch_case_value_t *seen_cases;
    bool has_default;
} switch_label_context_t;

switch_label_context_t switch_label_contexts[MAX_NESTING];
int switch_label_context_idx = 0;

void reject_label_followed_by_declaration_in_strict_c99(void)
{
    char name[MAX_ID_LEN];

    if (!strict_c99)
        return;

    if (lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
        lex_peek(T_restrict, NULL) || lex_peek(T_static, NULL) ||
        lex_peek(T_extern, NULL) || lex_peek(T_register, NULL) ||
        lex_peek(T_auto, NULL) || lex_peek(T_typedef, NULL) ||
        lex_peek(T_inline, NULL) || lex_peek(T_signed, NULL) ||
        lex_peek(T_unsigned, NULL) || lex_peek(T_long, NULL) ||
        lex_peek(T_struct, NULL) || lex_peek(T_union, NULL) ||
        lex_peek(T_enum, NULL) ||
        (lex_peek(T_identifier, name) && find_type(name, true)))
        error_at("a C99 label must precede a statement, not a declaration",
                 cur_token_loc());
}

basic_block_t *read_switch_label_statement(block_t *parent, basic_block_t *body)
{
    switch_label_context_t *context;
    basic_block_t *label_body;

    if (!switch_label_context_idx)
        error_at("case or default label outside switch", next_token_loc());
    context = &switch_label_contexts[switch_label_context_idx - 1];
    label_body = bb_create(parent);
    if (body)
        bb_connect(body, label_body, NEXT);

    if (lex_accept(T_default)) {
        if (context->has_default)
            error_at("duplicate default label in switch", cur_token_loc());
        context->has_default = true;
        context->default_body = label_body;
    } else {
        int case_val;
        var_t *constant;
        var_t *comparison;
        basic_block_t *next_dispatch;

        lex_expect(T_case);
        case_val = read_const_expr(parent);
        if (strict_c99) {
            switch_case_value_t *seen;

            for (seen = context->seen_cases; seen; seen = seen->next)
                if (seen->value == case_val)
                    error_at("duplicate case value in C99 switch",
                             cur_token_loc());
            seen = arena_alloc(GENERAL_ARENA, sizeof(*seen));
            seen->value = case_val;
            seen->next = context->seen_cases;
            context->seen_cases = seen;
        }

        constant = require_var(context->dispatch_parent);
        constant->var_name = gen_name();
        constant->init_val = case_val;
        add_insn(context->dispatch_parent, context->dispatch_tail,
                 OP_load_constant, constant, NULL, NULL, 0, NULL);

        comparison = require_var(context->dispatch_parent);
        comparison->var_name = gen_name();
        add_insn(context->dispatch_parent, context->dispatch_tail, OP_eq,
                 comparison, constant, context->control, 0, NULL);
        add_insn(context->dispatch_parent, context->dispatch_tail, OP_branch,
                 NULL, comparison, NULL, 0, NULL);
        bb_connect(context->dispatch_tail, label_body, THEN);
        next_dispatch = bb_create(context->dispatch_parent);
        bb_connect(context->dispatch_tail, next_dispatch, ELSE);
        context->dispatch_tail = next_dispatch;
    }
    lex_expect(T_colon);
    reject_label_followed_by_declaration_in_strict_c99();
    return label_body;
}

/* A switch, its cases, and the block they break out of. */
basic_block_t *handle_switch_statement(block_t *parent, basic_block_t *bb)
{
    basic_block_t *n = bb_create(parent);
    basic_block_t *body;
    basic_block_t *switch_end;
    switch_label_context_t *context;

    bb_connect(bb, n, NEXT);
    bb = n;

    lex_expect(T_open_bracket);
    read_control_expression(parent, &bb);
    lex_expect(T_close_bracket);
    var_t *control = operand_stack[operand_stack_idx - 1];

    /* C99 6.8.4.2 requires an integer controlling expression. After the
     * ordinary expression reader produces the operand, enforce that constraint
     * and apply integer promotions before deferred dispatch captures it.
     */
    if (!control->type || control->type == TY_void ||
        is_pointer_like_value(control) || control->is_func ||
        is_record_type(control->type))
        error_at("switch controlling expression must have integer type",
                 cur_token_loc());
    control = integer_promote_operand(parent, &bb, control);

    /* create exit jump for breaks */
    switch_end = bb_create(parent);
    break_bb_push(switch_end);
    if (switch_label_context_idx >= MAX_NESTING)
        fatal("Too many nested switch statements");
    context = &switch_label_contexts[switch_label_context_idx++];
    context->dispatch_parent = parent;
    context->control = control;
    context->dispatch_tail = bb;
    context->default_body = NULL;
    context->seen_cases = NULL;
    context->has_default = false;

    /* Statements before the first label are legal but are not an entry point of
     * the switch. A following label supplies the source-order fallthrough edge
     * without making those statements reachable from dispatch.
     */
    body = bb_create(parent);

    lex_expect(T_open_curly);
    while (!lex_accept(T_close_curly)) {
        body = read_body_statement(parent, body);
        perform_side_effect(parent, body);
    }

    /* Complete the deferred no-match dispatch after every case comparison is
     * known. This also supplies the natural exit edge for an empty switch.
     */
    bb_connect(context->dispatch_tail,
               context->has_default ? context->default_body : switch_end, NEXT);

    if (body)
        /* if the last label has no explicit break, connect it to the end */
        bb_connect(body, switch_end, NEXT);

    break_exit_idx--;
    switch_label_context_idx--;
    /* remove the expression in switch() */
    opstack_pop();

    int dangling = 1;
    for (int i = 0; i < switch_end->prev_idx; i++)
        if (switch_end->prev[i].bb)
            dangling = 0;

    if (dangling)
        return NULL;

    return switch_end;
}

/* A for loop: setup, condition, body and increment. */
basic_block_t *handle_for_statement(block_t *parent, basic_block_t *bb)
{
    char token[MAX_ID_LEN];
    type_t *type;
    var_t *vd;
    var_t *rs1;
    var_t *var;
    bool is_const = false;
    bool is_static = false;
    bool is_extern = false;
    bool is_register = false;
    bool is_auto = false;
    bool is_volatile = false;
    bool saw_decl_specifier = false;
    bool for_decl_semicolon_consumed = false;

    lex_expect(T_open_bracket);

    /* synthesize for loop block */
    block_t *blk = add_block(parent, parent->func);

    /* setup - execute once */
    basic_block_t *setup = bb_create(blk);
    bb_connect(bb, setup, NEXT);

    if (lex_peek(T_typedef, NULL)) {
        if (strict_c99)
            error_at("C99 for initializer cannot declare a typedef",
                     next_token_loc());

        /* The default-mode typedef extension owns the loop's synthetic block
         * scope. The typedef parser consumes its terminating semicolon and
         * emits no setup IR.
         */
        handle_block_typedef_statement(blk, setup);
        for_decl_semicolon_consumed = true;
    } else if (!lex_accept(T_semicolon)) {
        while (lex_peek(T_static, NULL) || lex_peek(T_extern, NULL) ||
               lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
               lex_peek(T_register, NULL) || lex_peek(T_auto, NULL)) {
            saw_decl_specifier = true;
            if (lex_accept(T_static)) {
                if (is_static)
                    error_at("duplicate static storage class specifier",
                             cur_token_loc());
                is_static = true;
            } else if (lex_accept(T_extern)) {
                if (is_extern)
                    error_at("duplicate extern storage class specifier",
                             cur_token_loc());
                is_extern = true;
            } else if (lex_accept(T_register)) {
                if (is_register)
                    error_at("duplicate register storage class specifier",
                             cur_token_loc());
                is_register = true;
            } else if (lex_accept(T_auto)) {
                if (is_auto)
                    error_at("duplicate auto storage class specifier",
                             cur_token_loc());
                is_auto = true;
            } else if (lex_accept(T_volatile)) {
                is_volatile = true;
            } else {
                lex_expect(T_const);
                is_const = true;
            }
        }
        if ((is_static && (is_register || is_extern || is_auto)) ||
            (is_register && (is_extern || is_auto)) || (is_extern && is_auto))
            error_at("incompatible storage class specifiers", cur_token_loc());

        bool has_builtin_type = lex_peek(T_signed, NULL) ||
                                lex_peek(T_unsigned, NULL) ||
                                lex_peek(T_long, NULL);
        bool has_identifier = lex_peek(T_identifier, token);
        bool has_record_type = lex_peek(T_struct, NULL) ||
                               lex_peek(T_union, NULL) ||
                               lex_peek(T_enum, NULL);
        bool has_enum_type = lex_peek(T_enum, NULL);
        bool has_tagged_record =
            lex_peek(T_struct, NULL) || lex_peek(T_union, NULL);
        if (has_enum_type) {
            /* read_full_var_decl() owns consuming and resolving `enum TAG`. Use
             * a scalar placeholder only to select its declaration path.
             */
            type = TY_int;
        } else if (has_tagged_record) {
            /* read_full_var_decl() owns consuming and resolving the tag. */
            type = TY_int;
        } else {
            type = has_builtin_type                    ? TY_int
                   : has_identifier || has_record_type ? find_type(token, 1)
                                                       : NULL;
        }
        if (!type && saw_decl_specifier)
            error_at("declaration specifier requires a type", cur_token_loc());
        if (type) {
            /* C99 6.8.5.3 admits only automatic or register object declarations
             * here. The default mode retains its historical block-scope
             * static/extern extension.
             */
            if (strict_c99 && (is_static || is_extern))
                error_at(
                    "C99 for initializer permits only auto or register objects",
                    cur_token_loc());
            var = require_typed_var(blk, type);
            var->is_static = is_static;
            var->is_register = is_register;
            var->is_global = is_static;
            var->is_const_qualified = is_const;
            var->is_volatile = is_volatile;
            parsing_for_initializer_declaration = true;
            read_full_var_decl(var, false, false, false);
            parsing_for_initializer_declaration = false;
            if (strict_c99 && var->is_func)
                error_at("C99 for initializer cannot declare a function",
                         cur_token_loc());
            if (is_extern) {
                if (var->is_func || lex_peek(T_open_bracket, NULL)) {
                    /* This helper consumes the declaration's semicolon. */
                    blk->locals.size--;
                    var->is_block_scope_function_declaration = true;
                    GLOBAL_BLOCK->locals.elements[GLOBAL_BLOCK->locals.size++] =
                        var;
                    read_global_function_declarator(GLOBAL_BLOCK, var, false);
                    var_t *alias = require_var(blk);
                    alias->var_name = var->var_name;
                    alias->is_extern_function_alias = true;
                    for_decl_semicolon_consumed = true;
                } else {
                    for (;;) {
                        var = bind_block_extern_object(blk, var);
                        if (lex_peek(T_assign, NULL))
                            error_at(
                                "extern declaration cannot have an initializer",
                                next_token_loc());
                        if (!lex_accept(T_comma))
                            break;
                        var = require_typed_var(blk, type);
                        var->is_const_qualified = is_const;
                        var->is_volatile = is_volatile;
                        read_partial_var_decl(var, NULL);
                    }
                }
            } else {
                if (is_incomplete_record_object(var))
                    error_at(
                        "Incomplete struct/union type cannot define an object",
                        cur_token_loc());
                add_insn(is_static ? GLOBAL_BLOCK : blk,
                         is_static ? GLOBAL_FUNC->bbs : setup, OP_allocat, var,
                         NULL, NULL, 0, NULL);
                add_symbol(setup, var);
                if (lex_accept(T_assign)) {
                    validate_string_array_initializer(var);
                    if (is_static) {
                        if (lex_peek(T_open_curly, NULL) &&
                            (var->array_size > 0 || var->has_unsized_array ||
                             var->ptr_level > 0))
                            parse_array_init(var, GLOBAL_BLOCK,
                                             &GLOBAL_FUNC->bbs, true);
                        else if (lex_peek(T_open_curly, NULL))
                            parse_global_record_init(var, GLOBAL_BLOCK);
                        else
                            read_global_assignment_var(var);
                    } else if ((var->has_unsized_array ||
                                var->array_size > 0) &&
                               is_char_array(var) && lex_peek(T_string, NULL)) {
                        parse_string_array_init(var, blk, &setup);
                    } else if ((var->has_unsized_array ||
                                var->array_size > 0) &&
                               is_wchar_array(var) &&
                               lex_peek(T_wstring, NULL)) {
                        parse_wstring_array_init(var, blk, &setup);
                    } else if (lex_peek(T_open_curly, NULL) &&
                               (var->array_size > 0 || var->has_unsized_array ||
                                var->ptr_level > 0)) {
                        parse_array_init(var, blk, &setup, true);
                    } else {
                        read_expr(blk, &setup);
                        read_ternary_operation(blk, &setup);

                        rs1 = resize_var(parent, &bb, opstack_pop(), var);
                        add_insn(blk, setup, OP_assign, var, rs1, NULL, 0,
                                 NULL);
                    }
                }
                while (lex_accept(T_comma)) {
                    var_t *nv;

                    /* add sequence point at T_comma */
                    perform_side_effect(blk, setup);

                    /* multiple (partial) declarations */
                    nv = require_typed_var(blk, type);
                    nv->is_static = is_static;
                    nv->is_register = is_register;
                    nv->is_global = is_static;
                    nv->is_const_qualified = is_const;
                    nv->is_volatile = is_volatile;
                    read_partial_var_decl(nv, var); /* partial */
                    if (is_incomplete_record_object(nv))
                        error_at(
                            "Incomplete struct/union type cannot define an "
                            "object",
                            cur_token_loc());
                    add_insn(is_static ? GLOBAL_BLOCK : blk,
                             is_static ? GLOBAL_FUNC->bbs : setup, OP_allocat,
                             nv, NULL, NULL, 0, NULL);
                    add_symbol(setup, nv);
                    if (lex_accept(T_assign)) {
                        validate_string_array_initializer(nv);
                        if (is_static) {
                            if (lex_peek(T_open_curly, NULL) &&
                                (nv->array_size > 0 || nv->has_unsized_array ||
                                 nv->ptr_level > 0))
                                parse_array_init(nv, GLOBAL_BLOCK,
                                                 &GLOBAL_FUNC->bbs, true);
                            else if (lex_peek(T_open_curly, NULL))
                                parse_global_record_init(nv, GLOBAL_BLOCK);
                            else
                                read_global_assignment_var(nv);
                        } else if ((nv->has_unsized_array ||
                                    nv->array_size > 0) &&
                                   is_char_array(nv) &&
                                   lex_peek(T_string, NULL)) {
                            parse_string_array_init(nv, blk, &setup);
                        } else if ((nv->has_unsized_array ||
                                    nv->array_size > 0) &&
                                   is_wchar_array(nv) &&
                                   lex_peek(T_wstring, NULL)) {
                            parse_wstring_array_init(nv, blk, &setup);
                        } else if (lex_peek(T_open_curly, NULL) &&
                                   (nv->array_size > 0 ||
                                    nv->has_unsized_array ||
                                    nv->ptr_level > 0)) {
                            parse_array_init(nv, blk, &setup, true);
                        } else {
                            read_expr(blk, &setup);

                            rs1 = resize_var(parent, &bb, opstack_pop(), nv);
                            add_insn(blk, setup, OP_assign, nv, rs1, NULL, 0,
                                     NULL);
                        }
                    }
                }
            }
        } else {
            read_control_expression(blk, &setup);
            opstack_pop();
            perform_side_effect(blk, setup);
        }

        if (!for_decl_semicolon_consumed)
            lex_expect(T_semicolon);
    }

    basic_block_t *cond_ = bb_create(blk);
    basic_block_t *for_end = bb_create(parent);
    basic_block_t *cond_start = cond_;
    break_bb_push(for_end);
    bb_connect(setup, cond_, NEXT);

    /* condition - check before the loop */
    if (!lex_accept(T_semicolon)) {
        read_control_expression(blk, &cond_);
        lex_expect(T_semicolon);
    } else {
        /* always true */
        vd = require_var(blk);
        vd->init_val = 1;
        vd->var_name = gen_name();
        opstack_push(vd);
        add_insn(blk, cond_, OP_load_constant, vd, NULL, NULL, 0, NULL);
    }
    bb_connect(cond_, for_end, ELSE);

    vd = opstack_pop();
    add_insn(blk, cond_, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *inc_ = bb_create(blk);
    continue_bb_push(inc_);

    /* increment after each loop */
    if (!lex_accept(T_close_bracket)) {
        read_control_expression(blk, &inc_);
        opstack_pop();
        perform_side_effect(blk, inc_);
        lex_expect(T_close_bracket);
    }

    /* loop body */
    basic_block_t *body_ = bb_create(blk);
    bb_connect(cond_, body_, THEN);
    body_ = read_body_statement(blk, body_);

    /* Normal fallthrough from the loop body goes through the increment block. A
     * continue statement may already have connected another predecessor to
     * inc_.
     */
    if (body_)
        bb_connect(body_, inc_, NEXT);

    /* An empty increment block still needs its back-edge when it is reachable
     * through normal fallthrough or continue.
     *
     * Do not connect a completely unreachable increment block, such as:
     *
     *     for (;;) {
     *         break;
     *     }
     */
    bool has_pred = false;
    for (int i = 0; i < inc_->prev_idx; i++) {
        if (inc_->prev[i].bb) {
            has_pred = true;
            break;
        }
    }
    if (has_pred)
        bb_connect(inc_, cond_start, NEXT);

    /* jump to increment */
    continue_pos_idx--;
    break_exit_idx--;
    return for_end;
}

/* A do-while loop, whose condition is tested after the body. */
basic_block_t *handle_do_statement(block_t *parent, basic_block_t *bb)
{
    var_t *vd;

    basic_block_t *n = bb_create(parent);
    bb_connect(bb, n, NEXT);
    bb = n;

    basic_block_t *cond_ = bb_create(parent);
    basic_block_t *do_while_end = bb_create(parent);

    continue_bb_push(cond_);
    break_bb_push(do_while_end);

    basic_block_t *do_body = read_body_statement(parent, bb);
    if (do_body)
        bb_connect(do_body, cond_, NEXT);

    lex_expect(T_while);
    lex_expect(T_open_bracket);
    read_control_expression(parent, &cond_);
    lex_expect(T_close_bracket);

    vd = opstack_pop();
    add_insn(parent, cond_, OP_branch, NULL, vd, NULL, 0, NULL);

    lex_expect(T_semicolon);

    for (int i = 0; i < cond_->prev_idx; i++) {
        if (cond_->prev[i].bb) {
            bb_connect(cond_, bb, THEN);
            bb_connect(cond_, do_while_end, ELSE);
            break;
        }
        /* if breaking out of loop, skip condition block */
    }

    continue_pos_idx--;
    break_exit_idx--;
    return do_while_end;
}

/* A local struct or union declaration. */
basic_block_t *handle_record_statement(block_t *parent,
                                       basic_block_t *bb,
                                       bool is_const,
                                       bool is_static)
{
    char token[MAX_ID_LEN];
    type_t *type;
    var_t *var;

    bool is_union = false;
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (find_type_flag == 1 && lex_accept(T_union)) {
        find_type_flag = 2;
        is_union = true;
    }
    lex_ident(T_identifier, token);
    type = find_type(token, find_type_flag);
    if (lex_peek(T_open_curly, NULL)) {
        int i = 0;
        int size = 0;
        int alignment = 1;
        int max_size = 0;
        bitfield_layout_t bits = {0};
        bool has_flexible_array_member = false;

        if (!type)
            type = add_type();
        set_type_name(type, token);
        type->base_type = is_union ? TYPE_union : TYPE_struct;

        lex_expect(T_open_curly);
        do {
            var_t *v = type_add_field(type, &i);
            var_t *last = v;

            read_full_var_decl(v, false, false, true);
            read_bitfield_width(v, parent);
            reject_flexible_array_member_container(v);
            mark_flexible_array_member(v, is_union);
            if (is_union) {
                v->offset = 0;
                int field_size = is_bitfield(v)
                                     ? (v->bit_width ? v->bit_storage_size : 0)
                                     : size_var(v);
                if (field_size > max_size)
                    max_size = field_size;
                if (alignment_var(v) > alignment)
                    alignment = alignment_var(v);
            } else {
                size = is_bitfield(v)
                           ? layout_bitfield_field(size, v, &alignment, &bits)
                           : layout_struct_field(
                                 flush_bitfield_layout(size, &bits), v,
                                 &alignment);
            }

            while (lex_accept(T_comma)) {
                if (!is_union && last->is_flexible_array_member)
                    error_at(
                        "Flexible array member must be the final struct member",
                        cur_token_loc());
                var_t *nv = type_add_field(type, &i);
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, false, true);
                read_bitfield_width(nv, parent);
                reject_flexible_array_member_container(nv);
                mark_flexible_array_member(nv, is_union);
                last = nv;
                if (is_union) {
                    nv->offset = 0;
                    int field_size =
                        is_bitfield(nv)
                            ? (nv->bit_width ? nv->bit_storage_size : 0)
                            : size_var(nv);
                    if (field_size > max_size)
                        max_size = field_size;
                    if (alignment_var(nv) > alignment)
                        alignment = alignment_var(nv);
                } else {
                    size =
                        is_bitfield(nv)
                            ? layout_bitfield_field(size, nv, &alignment, &bits)
                            : layout_struct_field(
                                  flush_bitfield_layout(size, &bits), nv,
                                  &alignment);
                }
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
        type->size =
            is_union ? ALIGN_UP(max_size, alignment)
                     : ALIGN_UP(flush_bitfield_layout(size, &bits), alignment);
        type->num_fields = i;
        type->has_flexible_array_member = has_flexible_array_member;

        if (lex_peek(T_semicolon, NULL)) {
            lex_expect(T_semicolon);
            return bb;
        }
    }
    if (!type && lex_peek(T_semicolon, NULL)) {
        /* A block-scope `struct tag;` or `union tag;` introduces an incomplete
         * tag. Its pointer declarators become valid immediately; the later
         * complete definition reuses this type record.
         */
        type = add_type();
        type->base_type = is_union ? TYPE_union : TYPE_struct;
        set_type_name(type, token);
        lex_expect(T_semicolon);
        return bb;
    }
    if (type) {
        var = require_typed_var(parent, type);
        var->is_const_qualified = is_const;
        var->is_static = is_static;
        var->is_global = is_static;
        read_partial_var_decl(var, NULL);
        if (is_incomplete_record_object(var))
            error_at("Incomplete struct/union type cannot define an object",
                     cur_token_loc());
        add_insn(is_static ? GLOBAL_BLOCK : parent,
                 is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, var, NULL, NULL,
                 0, NULL);
        add_symbol(bb, var);
        if (lex_accept(T_assign)) {
            validate_string_array_initializer(var);
            if (is_static && lex_peek(T_open_curly, NULL) &&
                (var->array_size > 0 || var->has_unsized_array ||
                 var->ptr_level > 0)) {
                parse_array_init(var, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
            } else if (is_static && lex_peek(T_open_curly, NULL)) {
                parse_global_record_init(var, GLOBAL_BLOCK);
            } else if (lex_peek(T_open_curly, NULL) &&
                       (var->array_size > 0 || var->has_unsized_array ||
                        var->ptr_level > 0)) {
                parse_array_init(var, parent, &bb, 1); /* Always emit code */
            } else if (lex_peek(T_open_curly, NULL) &&
                       (var->type->base_type == TYPE_struct ||
                        var->type->base_type == TYPE_union ||
                        var->type->base_type == TYPE_typedef)) {
                type_t *struct_type = var->type;
                if (struct_type->base_type == TYPE_typedef &&
                    struct_type->base_struct)
                    struct_type = struct_type->base_struct;

                var_t *struct_addr = require_var(parent);
                struct_addr->var_name = gen_name();
                add_insn(parent, bb, OP_address_of, struct_addr, var, NULL, 0,
                         NULL);
                lex_expect(T_open_curly);
                parse_struct_field_init(parent, &bb, struct_type, struct_addr,
                                        true);
                lex_expect(T_close_curly);
            } else {
                if (!read_assignment_expression(parent, &bb)) {
                    read_expr(parent, &bb);
                    read_ternary_operation(parent, &bb);
                }

                var_t *rhs = opstack_pop();
                rhs = scalarize_array_literal_if_needed(
                    parent, &bb, rhs, var->type,
                    !has_effective_pointer(var) && var->array_size == 0);

                emit_object_assignment(parent, &bb, var, rhs);
            }
        }
        while (lex_accept(T_comma)) {
            var_t *nv;

            /* add sequence point at T_comma */
            perform_side_effect(parent, bb);

            /* multiple (partial) declarations */
            nv = require_typed_var(parent, type);
            nv->is_static = is_static;
            nv->is_global = is_static;
            nv->is_const_qualified = is_const;
            read_inner_var_decl(nv, false, false, false);
            if (is_incomplete_record_object(nv))
                error_at("Incomplete struct/union type cannot define an object",
                         cur_token_loc());
            add_insn(is_static ? GLOBAL_BLOCK : parent,
                     is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, nv, NULL,
                     NULL, 0, NULL);
            add_symbol(bb, nv);
            if (lex_accept(T_assign)) {
                validate_string_array_initializer(nv);
                if (is_static && lex_peek(T_open_curly, NULL) &&
                    (nv->array_size > 0 || nv->has_unsized_array ||
                     nv->ptr_level > 0)) {
                    parse_array_init(nv, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
                } else if (is_static && lex_peek(T_open_curly, NULL)) {
                    parse_global_record_init(nv, GLOBAL_BLOCK);
                } else if (lex_peek(T_open_curly, NULL) &&
                           (nv->array_size > 0 || nv->has_unsized_array ||
                            nv->ptr_level > 0)) {
                    parse_array_init(nv, parent, &bb, true);
                } else if (lex_peek(T_open_curly, NULL) &&
                           (nv->type->base_type == TYPE_struct ||
                            nv->type->base_type == TYPE_union ||
                            nv->type->base_type == TYPE_typedef)) {
                    type_t *struct_type = nv->type;
                    if (struct_type->base_type == TYPE_typedef &&
                        struct_type->base_struct)
                        struct_type = struct_type->base_struct;

                    var_t *struct_addr = require_var(parent);
                    struct_addr->var_name = gen_name();
                    add_insn(parent, bb, OP_address_of, struct_addr, nv, NULL,
                             0, NULL);
                    lex_expect(T_open_curly);
                    parse_struct_field_init(parent, &bb, struct_type,
                                            struct_addr, true);
                    lex_expect(T_close_curly);
                } else {
                    read_expr(parent, &bb);
                    read_ternary_operation(parent, &bb);
                    var_t *rhs = opstack_pop();
                    rhs = scalarize_array_literal_if_needed(
                        parent, &bb, rhs, nv->type,
                        !has_effective_pointer(nv) && nv->array_size == 0);

                    emit_object_assignment(parent, &bb, nv, rhs);
                }
            }
        }
        lex_expect(T_semicolon);
        return bb;
    }
    error_at("Unknown struct/union type", next_token_loc());
}

basic_block_t *handle_enum_declarators(block_t *parent,
                                       basic_block_t *bb,
                                       type_t *type,
                                       bool is_const,
                                       bool is_static)
{
    for (;;) {
        var_t *var = require_typed_var(parent, type);

        var->is_const_qualified = is_const;
        var->is_static = is_static;
        var->is_global = is_static;
        read_partial_var_decl(var, NULL);
        add_insn(is_static ? GLOBAL_BLOCK : parent,
                 is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, var, NULL, NULL,
                 0, NULL);
        add_symbol(bb, var);

        if (lex_accept(T_assign)) {
            validate_string_array_initializer(var);
            if (is_static) {
                if (lex_peek(T_open_curly, NULL) &&
                    (var->array_size > 0 || var->has_unsized_array ||
                     var->ptr_level > 0)) {
                    parse_array_init(var, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs,
                                     true);
                } else {
                    read_global_assignment_var(var);
                }
            } else if ((var->has_unsized_array || var->array_size > 0) &&
                       is_char_array(var) && lex_peek(T_string, NULL)) {
                parse_string_array_init(var, parent, &bb);
            } else if ((var->has_unsized_array || var->array_size > 0) &&
                       is_wchar_array(var) && lex_peek(T_wstring, NULL)) {
                parse_wstring_array_init(var, parent, &bb);
            } else if (lex_peek(T_open_curly, NULL) &&
                       (var->array_size > 0 || var->has_unsized_array ||
                        var->ptr_level > 0)) {
                parse_array_init(var, parent, &bb, true);
            } else {
                read_expr(parent, &bb);
                read_ternary_operation(parent, &bb);

                var_t *rhs = opstack_pop();
                rhs = scalarize_array_literal_if_needed(
                    parent, &bb, rhs, var->type,
                    !has_effective_pointer(var) && var->array_size == 0);
                emit_object_assignment(parent, &bb, var, rhs);
            }
        }
        if (is_static)
            discard_global_declarator_operand(var);

        if (!lex_accept(T_comma))
            break;
        perform_side_effect(parent, bb);
    }
    lex_expect(T_semicolon);
    return bb;
}

/* C99 6.7.2.2 constrains every enumerator value to int range, while leaving the
 * compatible integer type of an enum implementation-defined. shecc deliberately
 * selects int for every target, so enum objects, parameters, returns, arrays,
 * and record fields use the ordinary int ABI consistently.
 */
void initialize_enum_type(type_t *type)
{
    type->base_type = TYPE_int;
    type->size = TY_int->size;
}

/* Advance an implicitly numbered enumerator without wrapping past C99's
 * required int domain. Callers invoke this only for an actual implicit value.
 */
int next_enum_value(int value)
{
    if (value == INT_MAX)
        error_at("Enumerator value exceeds int range", cur_token_loc());
    return value + 1;
}

int read_enum_constant(block_t *scope)
{
    bool saved_checking = checking_enum_constant;
    int value;

    if (typed_global_literal_appears_before_initializer_end(cur_token->next)) {
        pp_integer_t typed_value;
        block_t *saved_scope = pp_integer_constant_scope;
        token_t *last;

        pp_integer_constant_scope = scope;
        last = pp_read_constant_infix_expr(0, cur_token, &typed_value, true);
        pp_integer_constant_scope = saved_scope;
        cur_token = last;
        if ((typed_value.is_unsigned &&
             (typed_value.hi || typed_value.lo > 0x7fffffffU)) ||
            (!typed_value.is_unsigned &&
             typed_value.hi != (typed_value.lo & 0x80000000U ? ~0U : 0)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
        return typed_value.lo;
    }

    checking_enum_constant = true;
    value = read_const_expr(scope);
    checking_enum_constant = saved_checking;
    return value;
}

/* A block-scope enum definition contributes integer constants to the current
 * expression parser just as a file-scope definition does. Like a record
 * definition, it may introduce declarators after the closing brace.
 */
basic_block_t *handle_enum_statement(block_t *parent,
                                     basic_block_t *bb,
                                     bool is_const,
                                     bool is_static)
{
    char token[MAX_ID_LEN];
    int val = 0;
    type_t *type = NULL;
    bool has_tag = false;

    lex_expect(T_enum);
    if (lex_peek(T_identifier, token)) {
        lex_expect(T_identifier);
        type = find_local_type_tag(token, parent);
        has_tag = true;
    }
    if (!lex_peek(T_open_curly, NULL)) {
        if (!has_tag)
            error_at("Unknown enum type", next_token_loc());
        if (!type)
            type = find_type_tag(token, parent);
        if (!type)
            error_at("Unknown enum type", next_token_loc());
        return handle_enum_declarators(parent, bb, type, is_const, is_static);
    }
    if (!type)
        type = add_type();
    initialize_enum_type(type);
    if (has_tag) {
        set_type_name(type, token);
        if (!find_local_type_tag(token, parent))
            add_type_tag(parent, token, type);
    }
    lex_expect(T_open_curly);
    bool first = true;
    do {
        lex_ident(T_identifier, token);
        if (!first && !lex_peek(T_assign, NULL))
            val = next_enum_value(val);
        if (lex_accept(T_assign)) {
            val = read_enum_constant(parent);
        }
        add_scoped_constant(parent, token, val);
        first = false;
    } while (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL));
    lex_expect(T_close_curly);

    if (lex_accept(T_semicolon))
        return bb;
    return handle_enum_declarators(parent, bb, type, is_const, is_static);
}

/* Bind a block-scope extern declaration to the file-scope declaration table.
 *
 * The provisional declarator was added to @parent while its syntax was read. It
 * must not become an automatic object: an extern declaration has no local
 * storage. Move it to GLOBAL_BLOCK so the ordinary file-scope redeclaration
 * checks and eventual definition share one var_t, then leave a scope alias in
 * the current block. The alias matters when the declaration hides an outer
 * automatic object of the same name.
 */
var_t *bind_block_extern_object(block_t *parent, var_t *var)
{
    bool is_redeclaration;

    parent->locals.size--;
    var->is_global = true;
    var->is_static = false;
    GLOBAL_BLOCK->locals.elements[GLOBAL_BLOCK->locals.size++] = var;
    var =
        resolve_global_declarator(GLOBAL_BLOCK, var, false, &is_redeclaration);
    if (!is_redeclaration)
        add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0,
                 NULL);

    parent->locals.elements[parent->locals.size++] = var;
    return var;
}

static void reject_ordinary_typedef_collision(block_t *block, var_t *var)
{
    if (find_block_typedef(block, var->var_name))
        error_at("ordinary identifier conflicts with typedef name",
                 cur_token_loc());
}

/* The storage-class specifiers and qualifiers that lead a block-scope
 * declaration.
 */
typedef struct {
    bool is_const;
    bool is_static;
    bool is_extern;
    bool is_register;
    bool is_auto;
    bool is_volatile;
} block_decl_specifiers_t;

/* A block-scope declaration whose type handle_declaration() has resolved: every
 * declarator, its initializer, and its storage.
 *
 * Returns the block that follows.
 */
static basic_block_t *read_block_declarators(
    block_t *parent,
    basic_block_t *bb,
    type_t *type,
    const block_decl_specifiers_t *spec)
{
    var_t *var;

    var = require_typed_var(parent, type);
    var->is_static = spec->is_static;
    var->is_register = spec->is_register;
    var->is_global = spec->is_static;
    var->is_const_qualified = spec->is_const;
    var->is_volatile = spec->is_volatile;
    read_full_var_decl(var, false, false, false);
    reject_ordinary_typedef_collision(parent, var);

    /* A declaration spelled with signed, unsigned or long arrives with int as a
     * placeholder, and only the first declarator's specifiers resolve it. Every
     * later declarator in the list shares that resolved base type, as at file
     * scope; created from the placeholder, "unsigned int a, b;" gave b the type
     * int.
     */
    if (type == TY_int)
        type = var->type;

    /* A direct function typedef used without a star declares a function, not an
     * automatic object. Bind it through the file-scope function table and leave
     * a lexical function alias so it also hides an outer local object of the
     * same ordinary identifier.
     */
    if (var->is_func && var->type->is_direct_function_type) {
        for (;;) {
            if (spec->is_static || spec->is_register || spec->is_auto)
                error_at("invalid storage class for block function declaration",
                         cur_token_loc());
            if (spec->is_const || spec->is_volatile ||
                var->is_const_qualified || var->is_volatile)
                error_at("function type cannot be qualified", cur_token_loc());
            parent->locals.size--;
            var->is_block_scope_function_declaration = true;
            GLOBAL_BLOCK->locals.elements[GLOBAL_BLOCK->locals.size++] = var;
            read_global_function_declarator(GLOBAL_BLOCK, var, false);
            var_t *alias = require_var(parent);

            alias->var_name = var->var_name;
            alias->is_extern_function_alias = true;
            if (!lex_accept(T_comma))
                return bb;

            var = require_typed_var(parent, type);
            var->is_const_qualified = spec->is_const;
            var->is_volatile = spec->is_volatile;
            var->func_signature = type->func_signature;
            var->is_func = var->func_signature != NULL;
            read_partial_var_decl(var, NULL);
            reject_ordinary_typedef_collision(parent, var);
            if (!(var->is_func && var->type->is_direct_function_type))

                /* The comma list may continue with an object derived from the
                 * direct-function typedef, such as `unary_t declared,
                 * *callback`. The function declaration above has already been
                 * registered; hand this fully parsed object to the ordinary
                 * declaration lowering below.
                 */
                break;
        }
    }
    if (var->is_inline)
        error_at("inline specifier requires a function declarator",
                 next_token_loc());

    /* Decide after the full declarator has been read: both `const int` and `int
     * const`, and an outer `* const`, make the defined object non-modifiable.
     */
    if (spec->is_static && parent->func && parent->func->is_inline &&
        !parent->func->is_static && !var->is_const_qualified &&
        !var->is_const_pointer)
        error_at("external inline definition cannot define static object",
                 cur_token_loc());
    if (spec->is_extern) {
        if (var->is_func || lex_peek(T_open_bracket, NULL)) {
            /* The global helper owns function redeclaration compatibility and
             * parameter parsing. Unlike an object declaration it consumes the
             * trailing semicolon itself.
             */
            parent->locals.size--;
            var->is_block_scope_function_declaration = true;
            GLOBAL_BLOCK->locals.elements[GLOBAL_BLOCK->locals.size++] = var;
            read_global_function_declarator(GLOBAL_BLOCK, var, false);

            /* Keep a lexical marker so this declaration hides an outer
             * automatic object of the same name.
             */
            var_t *alias = require_var(parent);
            alias->var_name = var->var_name;
            alias->is_extern_function_alias = true;
            return bb;
        }
        for (;;) {
            var = bind_block_extern_object(parent, var);
            if (lex_peek(T_assign, NULL))
                error_at("extern declaration cannot have an initializer",
                         next_token_loc());
            if (!lex_accept(T_comma))
                break;
            var = require_typed_var(parent, type);
            var->is_const_qualified = spec->is_const;
            var->is_volatile = spec->is_volatile;
            read_partial_var_decl(var, NULL);
        }
        lex_expect(T_semicolon);
        return bb;
    }
    if (is_incomplete_record_object(var))
        error_at("Incomplete struct/union type cannot define an object",
                 cur_token_loc());
    add_insn(spec->is_static ? GLOBAL_BLOCK : parent,
             spec->is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, var, NULL,
             NULL, 0, NULL);
    add_symbol(bb, var);
    if (lex_accept(T_assign)) {
        validate_string_array_initializer(var);
        if (spec->is_static) {
            if (lex_peek(T_open_curly, NULL) &&
                (var->array_size > 0 || var->has_unsized_array ||
                 var->ptr_level > 0)) {
                /* A block-scope static has global storage duration, so its
                 * brace initializer belongs to the same constant-data lowering
                 * as a file-scope array.
                 */
                parse_array_init(var, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
            } else if (global_compound_literal_starts_here() &&
                       !(var->ptr_level || var->type->ptr_level) &&
                       is_record_type(var->type)) {
                parse_global_compound_record_init(var, GLOBAL_BLOCK);
            } else if (global_compound_literal_starts_here() &&
                       (var->ptr_level || var->type->ptr_level)) {
                parse_global_compound_array_init(var, GLOBAL_BLOCK);
            } else if (global_compound_literal_starts_here()) {
                parse_global_compound_scalar_init(var, GLOBAL_BLOCK);
            } else if (lex_peek(T_open_curly, NULL)) {
                parse_global_record_init(var, GLOBAL_BLOCK);
            } else {
                read_global_assignment_var(var);
            }
        } else if ((var->has_unsized_array || var->array_size > 0) &&
                   is_char_array(var) && lex_peek(T_string, NULL)) {
            parse_string_array_init(var, parent, &bb);
        } else if ((var->has_unsized_array || var->array_size > 0) &&
                   is_wchar_array(var) && lex_peek(T_wstring, NULL)) {
            parse_wstring_array_init(var, parent, &bb);
        } else if (lex_peek(T_open_curly, NULL) &&
                   (var->array_size > 0 || var->has_unsized_array ||
                    var->ptr_level > 0)) {
            /* Emit code for locals in functions */
            parse_array_init(var, parent, &bb, 1);
        } else if (lex_peek(T_open_curly, NULL) && is_record_type(var->type)) {
            type_t *struct_type = var->type;
            if (struct_type->base_type == TYPE_typedef &&
                struct_type->base_struct)
                struct_type = struct_type->base_struct;

            var_t *struct_addr = require_var(parent);
            struct_addr->var_name = gen_name();
            add_insn(parent, bb, OP_address_of, struct_addr, var, NULL, 0,
                     NULL);
            lex_expect(T_open_curly);
            parse_struct_field_init(parent, &bb, struct_type, struct_addr,
                                    true);
            lex_expect(T_close_curly);
        } else {
            if (!read_assignment_expression(parent, &bb)) {
                read_expr(parent, &bb);
                read_ternary_operation(parent, &bb);
            }

            var_t *expr_result = opstack_pop();

            /* Keep direct function-pointer initializers on their relocation
             * path, but every other initializer consumes a function designator
             * as its converted pointer value.
             */
            if (!var->is_func)
                expr_result =
                    materialize_function_designator(parent, &bb, expr_result);

            if (strict_c99 && is_array_literal_placeholder(expr_result) &&
                !has_effective_pointer(var) && var->array_size == 0)
                error_at(
                    "array compound literal cannot be used as a scalar in "
                    "C99",
                    cur_token_loc());

            /* Handle array compound literal to scalar assignment */
            if (expr_result && expr_result->array_size > 0 && !var->ptr_level &&
                var->array_size == 0 && var->type &&
                (var->type->base_type == TYPE_int ||
                 var->type->base_type == TYPE_short) &&
                expr_result->var_name[0] == '.') {
                /* Extract first element from compound literal array */
                var_t *first_elem = require_var(parent);
                first_elem->type = var->type;
                first_elem->var_name = gen_name();

                /* Read first element from array at offset 0 expr_result is the
                 * array itself, so we can read directly from it
                 */
                add_insn(parent, bb, OP_read, first_elem, expr_result, NULL,
                         var->type->size, NULL);
                expr_result = first_elem;
            }

            if (incompatible_pointee_callback_conversion(expr_result, var))
                error_at("incompatible callback slot types in initializer",
                         cur_token_loc());
            diagnose_const_pointer_conversion(expr_result, var);
            emit_object_assignment(parent, &bb, var, expr_result);
        }
    }
    if (spec->is_static)
        discard_global_declarator_operand(var);
    while (lex_accept(T_comma)) {
        var_t *nv;

        /* add sequence point at T_comma */
        perform_side_effect(parent, bb);

        /* multiple (partial) declarations */
        nv = require_typed_var(parent, type);
        nv->is_static = spec->is_static;
        nv->is_register = spec->is_register;
        nv->is_global = spec->is_static;
        nv->is_const_qualified = var->is_const_qualified;
        nv->is_volatile = var->is_volatile;
        read_partial_var_decl(nv, var); /* partial */
        reject_ordinary_typedef_collision(parent, nv);
        if (is_incomplete_record_object(nv))
            error_at("Incomplete struct/union type cannot define an object",
                     cur_token_loc());
        add_insn(spec->is_static ? GLOBAL_BLOCK : parent,
                 spec->is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, nv, NULL,
                 NULL, 0, NULL);
        add_symbol(bb, nv);
        if (lex_accept(T_assign)) {
            validate_string_array_initializer(nv);
            if (spec->is_static) {
                if (lex_peek(T_open_curly, NULL) &&
                    (nv->array_size > 0 || nv->has_unsized_array ||
                     nv->ptr_level > 0)) {
                    parse_array_init(nv, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
                } else if (global_compound_literal_starts_here() &&
                           !(nv->ptr_level || nv->type->ptr_level) &&
                           is_record_type(nv->type)) {
                    parse_global_compound_record_init(nv, GLOBAL_BLOCK);
                } else if (global_compound_literal_starts_here() &&
                           (nv->ptr_level || nv->type->ptr_level)) {
                    parse_global_compound_array_init(nv, GLOBAL_BLOCK);
                } else if (global_compound_literal_starts_here()) {
                    parse_global_compound_scalar_init(nv, GLOBAL_BLOCK);
                } else if (lex_peek(T_open_curly, NULL)) {
                    parse_global_record_init(nv, GLOBAL_BLOCK);
                } else {
                    read_global_assignment_var(nv);
                }
            } else if ((nv->has_unsized_array || nv->array_size > 0) &&
                       is_char_array(nv) && lex_peek(T_string, NULL)) {
                parse_string_array_init(nv, parent, &bb);
            } else if ((nv->has_unsized_array || nv->array_size > 0) &&
                       is_wchar_array(nv) && lex_peek(T_wstring, NULL)) {
                parse_wstring_array_init(nv, parent, &bb);
            } else if (lex_peek(T_open_curly, NULL) &&
                       (nv->array_size > 0 || nv->has_unsized_array ||
                        nv->ptr_level > 0)) {
                /* Emit code for locals */
                parse_array_init(nv, parent, &bb, 1);
            } else if (lex_peek(T_open_curly, NULL) &&
                       is_record_type(nv->type)) {
                type_t *struct_type = nv->type;
                if (struct_type->base_type == TYPE_typedef &&
                    struct_type->base_struct)
                    struct_type = struct_type->base_struct;

                var_t *struct_addr = require_var(parent);
                struct_addr->var_name = gen_name();
                add_insn(parent, bb, OP_address_of, struct_addr, nv, NULL, 0,
                         NULL);
                lex_expect(T_open_curly);
                parse_struct_field_init(parent, &bb, struct_type, struct_addr,
                                        true);
                lex_expect(T_close_curly);
            } else {
                if (!read_assignment_expression(parent, &bb)) {
                    read_expr(parent, &bb);
                    read_ternary_operation(parent, &bb);
                }

                emit_object_assignment(parent, &bb, nv, opstack_pop());
            }
        }
        if (spec->is_static)
            discard_global_declarator_operand(nv);
    }
    lex_expect(T_semicolon);
    return bb;
}

/* Everything a statement can still be: a declaration, an assignment, a call, or
 * an expression evaluated for its effect.
 */
basic_block_t *handle_declaration(block_t *parent, basic_block_t *bb)
{
    char token[MAX_ID_LEN];
    func_t *func;
    type_t *type;
    block_decl_specifiers_t spec = {0};

    while (lex_peek(T_static, NULL) || lex_peek(T_extern, NULL) ||
           lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
           lex_peek(T_register, NULL) || lex_peek(T_auto, NULL)) {
        if (lex_accept(T_static)) {
            if (spec.is_static)
                error_at("duplicate static storage class specifier",
                         cur_token_loc());
            spec.is_static = true;
        } else if (lex_accept(T_extern)) {
            if (spec.is_extern)
                error_at("duplicate extern storage class specifier",
                         cur_token_loc());
            spec.is_extern = true;
        } else if (lex_accept(T_register)) {
            if (spec.is_register)
                error_at("duplicate register storage class specifier",
                         cur_token_loc());
            spec.is_register = true;
        } else if (lex_accept(T_auto)) {
            if (spec.is_auto)
                error_at("duplicate auto storage class specifier",
                         cur_token_loc());
            spec.is_auto = true;
        } else if (lex_accept(T_volatile)) {
            spec.is_volatile = true;
        } else {
            lex_expect(T_const);
            spec.is_const = true;
        }
    }
    if ((spec.is_static &&
         (spec.is_register || spec.is_extern || spec.is_auto)) ||
        (spec.is_register && (spec.is_extern || spec.is_auto)) ||
        (spec.is_extern && spec.is_auto))
        error_at("incompatible storage class specifiers", cur_token_loc());

    if (floating_type_starts_here())
        error_at("Floating point types are not yet supported", cur_token_loc());

    if (lex_peek(T_enum, NULL))
        return handle_enum_statement(parent, bb, spec.is_const, spec.is_static);

    if (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL)) {
        if (spec.is_extern || spec.is_register)
            error_at("Unsupported storage class on record definition",
                     next_token_loc());
        return handle_record_statement(parent, bb, spec.is_const,
                                       spec.is_static);
    }

    /* must be an identifier or asterisk (for pointer dereference) */
    bool has_asterisk = lex_peek(T_asterisk, NULL);
    bool has_identifier = lex_peek(T_identifier, token);
    bool has_record_keyword =
        lex_peek(T_struct, NULL) || lex_peek(T_union, NULL);
    bool has_signed_keyword = lex_peek(T_signed, NULL);
    bool has_unsigned_keyword = lex_peek(T_unsigned, NULL);
    bool has_long_keyword = lex_peek(T_long, NULL);
    if (!spec.is_const && !has_identifier && !has_asterisk &&
        !has_record_keyword && !has_signed_keyword && !has_unsigned_keyword &&
        !has_long_keyword)
        error_at("Unexpected token", next_token_loc());

    /* is it a variable declaration? Special handling when statement starts with
     * asterisk
     */
    if (has_asterisk) {
        /* For "*identifier", check if identifier is a type. If not, it's a
         * dereference, not a declaration.
         */
        token_t *saved_token = cur_token;

        /* Skip the asterisk to peek at the identifier */
        lex_accept(T_asterisk);
        char next_ident[MAX_TOKEN_LEN];
        bool could_be_type = false;

        if (lex_peek(T_identifier, next_ident)) {
            /* Check if it's a type name */
            type = find_visible_type(next_ident, parent);
            if (type)
                could_be_type = true;
        }

        /* Restore position */
        cur_token = saved_token;

        /* If it's not a type, skip the declaration block */
        if (!could_be_type)
            type = NULL;
    } else {
        /* Normal type checking without asterisk */
        token_t *type_token = cur_token;
        if (lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
            lex_peek(T_long, NULL)) {
            type = TY_int;
        } else {
            int find_type_flag = lex_accept(T_struct) ? 2 : 1;
            if (find_type_flag == 1 && lex_accept(T_union))
                find_type_flag = 2;
            if (find_type_flag == 2)
                lex_peek(T_identifier, token);
            type = find_type_flag == 2 ? find_type(token, find_type_flag)
                                       : find_visible_type(token, parent);
            if (find_type_flag == 2)
                cur_token = type_token;
        }
    }

    if ((spec.is_static || spec.is_extern) && !type)
        error_at("Expected declaration after storage class specifier",
                 next_token_loc());

    if (type)
        return read_block_declarators(parent, bb, type, &spec);

    /* Keep the long-standing direct-call lowering only for a truly standalone
     * `function(...);` statement. An identifier-led call that is followed by an
     * operator or comma belongs to the full expression grammar below. The
     * self-hosted compiler still exercises this short path heavily, while the
     * bounded lookahead prevents it from stealing valid C99 expressions such as
     * `first(), second();` or `function() + 1;`.
     */
    var_t *local = find_local_var(token, parent);
    if (!has_asterisk && (!local || local->is_extern_function_alias)) {
        token_t *call = cur_token->next;
        token_t *end = call && call->next ? call->next : NULL;
        int depth = 0;
        bool matched_call = false;

        if (end && end->kind == T_open_bracket) {
            for (; end; end = end->next) {
                if (end->kind == T_open_bracket)
                    depth++;
                else if (end->kind == T_close_bracket && !--depth) {
                    end = end->next;
                    matched_call = true;
                    break;
                }
            }
        }
        if (matched_call && end && end->kind == T_semicolon && call) {
            func = find_visible_func(token, parent);
            if (func) {
                lex_expect(T_identifier);
                emit_direct_call_result(func, false, parent, &bb);
                perform_side_effect(parent, bb);
                lex_expect(T_semicolon);
                return bb;
            }
        }
    }

    /* A declaration has already returned above. Resolve a label before routing
     * every remaining non-declaration statement through the full C99 expression
     * grammar.
     */
    if (lex_peek(T_identifier, token) && cur_token->next->next &&
        cur_token->next->next->kind == T_colon) {
        lex_accept(T_identifier);
        token_t *id_tk = cur_token;

        lex_expect(T_colon);
        const label_t *l = find_label(token);
        if (l)
            error_at("label redefinition", &id_tk->location);
        reject_label_followed_by_declaration_in_strict_c99();
        basic_block_t *n = bb_create(parent);
        bb_connect(bb, n, NEXT);
        add_label(token, n);
        add_insn(parent, n, OP_label, NULL, NULL, NULL, 0, token);
        return n;
    }
    return read_full_expression_statement(parent, bb);
}

/* Lexical typedef aliases are declaration-only bindings on the current block,
 * never ordinary objects or global type names. This bounded parser admits
 * selected direct-function, callback-pointer, and fixed callback-array forms;
 * remaining derived declarations await the shared declarator path.
 */
basic_block_t *handle_block_typedef_statement(block_t *parent,
                                              basic_block_t *bb)
{
    var_t decl = {0};

    lex_expect(T_typedef);
    decl.scope = parent;
    parsing_block_typedef_declarator = true;
    read_full_var_decl(&decl, false, false, false);
    parsing_block_typedef_declarator = false;
    do {
        type_t *base = decl.type;
        type_t *alias = add_type();
        func_t *direct_function_signature = decl.func_signature;
        func_t *callback_signature = decl.func_signature;
        func_t *callback_slot_signature = decl.pointee_func_signature;
        bool direct_array = decl.has_direct_array_declarator;
        bool direct_pointee_array = decl.has_direct_pointee_array_declarator;
        bool inherited_pointee_array = base->pointee_array_size != 0;
        bool direct_function_alias =
            decl.is_direct_function_declarator && decl.is_func &&
            decl.func_signature && !decl.ptr_level && !decl.array_size &&
            !decl.pointee_array_size && !base->ptr_level &&
            !is_record_type(base) && !base->is_floating &&
            !direct_function_signature->va_args &&
            !direct_function_signature->returns_aggregate;
        bool callback_pointer_alias =
            decl.is_func && decl.func_signature &&
            decl.parenthesized_function_pointer_level == 1 && !decl.ptr_level &&
            !decl.array_size && !decl.pointee_array_size && !base->ptr_level &&
            !is_record_type(base) && !base->is_floating &&
            !callback_signature->va_args &&
            !callback_signature->returns_aggregate;
        bool callback_pointer_realias =
            decl.is_func && decl.func_signature &&
            !decl.parenthesized_function_pointer_level && !decl.ptr_level &&
            !decl.array_size && !decl.pointee_array_size && !base->ptr_level &&
            base->func_signature && !base->is_direct_function_type;

        /* `int (**slot_t)(int)` is a pointer-to-callback object, not a callable
         * callback pointer. The shared declarator parser has already normalized
         * the extra star into decl.ptr_level and retained the prototype as
         * pointee metadata. Keep this first exact typedef form equally narrow:
         * scalar/void, no arrays or qualifiers, and no deeper pointer
         * composition.
         */
        bool callback_slot_alias =
            callback_slot_signature &&
            decl.parenthesized_function_pointer_level == 2 &&
            decl.ptr_level == 1 && !decl.array_size &&
            !decl.pointee_array_size && !base->ptr_level &&
            !is_record_type(base) && !base->is_floating &&
            !decl.is_const_qualified &&
            !decl.parenthesized_function_pointer_inner_qualified &&
            !callback_slot_signature->va_args &&
            !callback_slot_signature->returns_aggregate;
        bool callback_slot_realias =
            decl.pointee_func_signature &&
            !decl.parenthesized_function_pointer_level && !decl.ptr_level &&
            !decl.array_size && !decl.pointee_array_size &&
            base->ptr_level == 1 && base->pointee_func_signature &&
            !decl.is_const_qualified &&
            !decl.parenthesized_function_pointer_inner_qualified &&
            !decl.parenthesized_function_pointer_restrict;
        bool callback_slot_array_alias =
            callback_slot_signature &&
            decl.parenthesized_function_pointer_level == 2 &&
            decl.ptr_level == 1 && decl.array_size > 0 &&
            !decl.has_unsized_array && !decl.pointee_array_size &&
            !base->ptr_level && !base->array_size && !is_record_type(base) &&
            !base->is_floating && !decl.is_const_qualified &&
            (!decl.is_const_pointer ||
             decl.parenthesized_function_pointer_outer_const) &&
            (!decl.pointer_const_mask ||
             (decl.parenthesized_function_pointer_outer_const &&
              decl.pointer_const_mask == 1U)) &&
            (!decl.is_volatile ||
             decl.parenthesized_function_pointer_outer_volatile) &&
            !decl.parenthesized_function_pointer_inner_qualified &&
            (!decl.parenthesized_function_pointer_restrict ||
             decl.parenthesized_function_pointer_outer_restrict) &&
            !callback_slot_signature->va_args &&
            !callback_slot_signature->returns_aggregate;
        bool callback_slot_array_realias =
            !decl.parenthesized_function_pointer_level && !decl.ptr_level &&
            !decl.pointee_array_size && base->array_size > 0 &&
            base->array_element_ptr_level == 1 &&
            base->array_element_pointee_func_signature &&
            !decl.is_const_pointer && !decl.pointer_const_mask &&
            !decl.parenthesized_function_pointer_restrict;
        bool callback_array_alias =
            decl.is_func && decl.func_signature &&
            decl.parenthesized_function_pointer_level == 1 &&
            decl.array_size > 0 && !decl.ptr_level &&
            !decl.pointee_array_size && !base->ptr_level &&
            !is_record_type(base) && !base->is_floating &&
            !callback_signature->va_args &&
            !callback_signature->returns_aggregate;
        bool callback_array_realias =
            decl.is_func && decl.func_signature &&
            !decl.parenthesized_function_pointer_level && !direct_array &&
            !decl.ptr_level && !decl.pointee_array_size &&
            base->array_size > 0 && base->array_element_ptr_level == 1 &&
            base->func_signature && !base->is_direct_function_type;

        if (decl.parenthesized_function_pointer_level > 1 &&
            !callback_slot_alias && !callback_slot_array_alias)
            error_at(
                "deeper block callback-pointer typedef is not yet supported",
                cur_token_loc());
        if (callback_pointer_alias &&
            decl.parenthesized_function_pointer_restrict)
            error_at("restrict requires a pointer to an object type",
                     cur_token_loc());
        if (callback_array_alias &&
            (decl.is_const_pointer || (decl.pointer_const_mask & 1U) ||
             decl.parenthesized_function_pointer_const || decl.is_volatile ||
             decl.parenthesized_function_pointer_restrict))
            error_at(
                "qualified block callback-array typedef is not yet supported",
                cur_token_loc());
        if (direct_array && base->func_signature &&
            !base->is_direct_function_type &&
            (decl.is_const_qualified || decl.is_volatile))
            error_at(
                "qualified block callback-array typedef is not yet supported",
                cur_token_loc());
        if (direct_array && base->array_size &&
            base->array_element_ptr_level == 1 && base->func_signature &&
            !base->is_direct_function_type)
            error_at(
                "derived block callback-array typedef is not yet supported",
                cur_token_loc());

        /* Keep pointer-to-array aliases intentionally narrow until their
         * composition rules share the full declarator path. The supported forms
         * are exactly fixed scalar rows and fixed one-pointer-element rows,
         * plus a plain re-alias of a completed type.
         */
        if (decl.has_unsized_array ||
            ((decl.is_func || decl.func_signature) && !direct_function_alias &&
             !callback_pointer_alias && !callback_pointer_realias &&
             !callback_array_alias && !callback_array_realias &&
             !callback_slot_alias && !callback_slot_realias &&
             !callback_slot_array_alias && !callback_slot_array_realias) ||
            (base->pointee_func_signature && !callback_slot_realias) ||
            (base->array_element_pointee_func_signature &&
             !callback_slot_array_realias) ||
            (direct_array &&
             ((base->ptr_level && !inherited_pointee_array) ||
              (inherited_pointee_array && decl.array_dim2) ||
              (decl.ptr_level && (decl.array_dim2 || decl.ptr_level > 1)))) ||
            (direct_pointee_array &&
             (base->ptr_level || base->array_size ||
              decl.ptr_level != decl.pointee_array_element_ptr_level + 1 ||
              decl.pointee_array_element_ptr_level > 1)) ||
            (inherited_pointee_array &&
             (decl.ptr_level || direct_pointee_array)))
            error_at("block typedef derived declarator is not yet supported",
                     cur_token_loc());
        if (lex_peek(T_assign, NULL))
            error_at("typedef declaration cannot have an initializer",
                     next_token_loc());
        memcpy(alias, base, sizeof(*alias));
        alias->base_type = TYPE_typedef;
        alias->base_struct = is_record_type(base) ? base : base->base_struct;
        alias->ptr_level = base->ptr_level + decl.ptr_level;
        alias->pointer_const_mask =
            base->pointer_const_mask |
            (decl.pointer_const_mask << base->ptr_level);
        alias->is_const_qualified = decl.is_const_qualified;
        if (callback_pointer_realias && decl.is_const_qualified) {
            /* A callback alias itself is a pointer type, even though its
             * compact descriptor stores no ordinary pointer depth.
             */
            alias->pointer_const_mask |= 1U;
            alias->is_const_qualified = false;
        }
        alias->is_volatile_qualified =
            decl.is_volatile || base->is_volatile_qualified;
        if (direct_function_alias) {
            alias->func_signature = decl.func_signature;
            alias->is_direct_function_type = true;
        }
        if (callback_pointer_alias) {
            alias->size = PTR_SIZE;
            alias->func_signature = decl.func_signature;
            alias->is_direct_function_type = false;
        }
        if (callback_slot_alias) {
            alias->size = PTR_SIZE;
            alias->alignment = PTR_SIZE;
            alias->func_signature = NULL;
            alias->pointee_func_signature = callback_slot_signature;
            alias->is_direct_function_type = false;
        }
        if (callback_slot_array_alias) {
            alias->ptr_level = 0;
            alias->size = PTR_SIZE;
            alias->alignment = PTR_SIZE;
            alias->func_signature = NULL;
            alias->pointee_func_signature = NULL;
            alias->is_direct_function_type = false;
            alias->pointer_const_mask = 0;
            alias->is_volatile_qualified = false;
            compose_block_typedef_array(alias, base, &decl);
            alias->array_element_ptr_level = 1;
            alias->array_element_pointee_func_signature =
                callback_slot_signature;
            alias->array_element_is_const_pointer =
                decl.parenthesized_function_pointer_outer_const;
            alias->array_element_is_volatile =
                decl.parenthesized_function_pointer_outer_volatile;
        }
        if (callback_slot_array_realias && direct_array) {
            /* Wrapping a completed slot-array alias prepends bounds but must
             * keep its element as a callback slot. compose_* records the
             * declaration's zero direct pointer depth, so restore the completed
             * element descriptor after composition.
             */
            compose_block_typedef_array(alias, base, &decl);
            alias->array_element_ptr_level = base->array_element_ptr_level;
            alias->array_element_pointee_func_signature =
                base->array_element_pointee_func_signature;
        }
        if (callback_slot_array_realias) {
            /* Qualifying an array typedef qualifies its element type. Keep that
             * fact on the final callback-slot lvalue rather than making the
             * array object itself const or volatile. `restrict` has no runtime
             * representation, but was validated while parsing.
             */
            alias->is_const_qualified = false;
            alias->is_volatile_qualified = false;
            alias->array_element_is_const_pointer |= decl.is_const_qualified;
            alias->array_element_is_volatile |= decl.is_volatile;
        }
        if (callback_slot_realias && decl.is_const_pointer) {
            /* `slot_t const` qualifies the typedef-hidden outer slot pointer,
             * so do not shift the declaration's compact bit past it.
             */
            alias->pointer_const_mask |= 1U;
            alias->is_const_qualified = false;
        }
        if (callback_array_alias) {
            alias->size = PTR_SIZE;
            alias->alignment = PTR_SIZE;
            alias->func_signature = decl.func_signature;
            alias->is_direct_function_type = false;
        }
        if (direct_array || callback_array_alias) {
            compose_block_typedef_array(alias, base, &decl);
            if (callback_array_alias)
                alias->array_element_ptr_level = 1;
            if (direct_array && inherited_pointee_array) {
                /* A direct array suffix around `int (*row_t)[N]` stores
                 * pointer-to-row elements. Keep the row descriptor on the
                 * completed array and mark the selected element as that pointer
                 * slot so its later load retains the row stride.
                 */
                alias->array_element_ptr_level = base->ptr_level;
                alias->array_element_type =
                    base->pointee_array_element_type
                        ? base->pointee_array_element_type
                        : base;
            }
        }
        if (direct_pointee_array) {
            alias->pointee_array_size = decl.pointee_array_size;
            alias->pointee_array_dim2 = decl.pointee_array_dim2;
            alias->pointee_array_dim3 = decl.pointee_array_dim3;
            alias->pointee_array_dim4 = decl.pointee_array_dim4;
            alias->pointee_array_element_ptr_level =
                decl.pointee_array_element_ptr_level;
            alias->pointee_array_element_type = base;
        }
        if (alias->ptr_level)
            alias->size = PTR_SIZE;
        alias->type_name[0] = '\0';
        add_block_typedef(parent, decl.var_name, alias);
        if (!lex_accept(T_comma))
            break;
        decl = (var_t) {0};
        decl.scope = parent;
        decl.type = base;

        /* A comma continuation may itself be the bounded direct function
         * typedef form, e.g. `typedef int *p_t, unary_t(int);`.
         */
        parsing_block_typedef_declarator = true;
        read_partial_var_decl(&decl, NULL);
        parsing_block_typedef_declarator = false;
    } while (true);
    lex_expect(T_semicolon);
    return bb;
}

basic_block_t *read_body_statement(block_t *parent, basic_block_t *bb)
{
    /* statement can be:
     *   function call, variable declaration, assignment operation,
     *   keyword, block
     */

    if (lex_peek(T_case, NULL) || lex_peek(T_default, NULL))
        return read_switch_label_statement(parent, bb);

    if (!bb)
        printf("Warning: unreachable code detected\n");

    if (lex_peek(T_open_curly, NULL))
        return read_code_block(parent->func, parent, bb);

    if (lex_accept(T_return)) {
        return handle_return_statement(parent, bb);
    }

    if (lex_accept(T_if)) {
        return handle_if_statement(parent, bb);
    }

    if (lex_accept(T_while)) {
        return handle_while_statement(parent, bb);
    }

    if (lex_accept(T_switch))
        return handle_switch_statement(parent, bb);

    if (lex_accept(T_break)) {
        if (!break_exit_idx)
            error_at("'break' outside of a loop or switch", cur_token_loc());
        bb_connect(bb, break_bb[break_exit_idx - 1], NEXT);
        lex_expect(T_semicolon);
        return NULL;
    }

    if (lex_accept(T_continue)) {
        if (!continue_pos_idx)
            error_at("'continue' outside of a loop", cur_token_loc());
        bb_connect(bb, continue_bb[continue_pos_idx - 1], NEXT);
        lex_expect(T_semicolon);
        return NULL;
    }

    if (lex_accept(T_for))
        return handle_for_statement(parent, bb);

    if (lex_accept(T_do))
        return handle_do_statement(parent, bb);

    if (lex_accept(T_goto))
        return handle_goto_statement(parent, bb);

    if (lex_peek(T_typedef, NULL))
        return handle_block_typedef_statement(parent, bb);

    /* empty statement */
    if (lex_accept(T_semicolon))
        return bb;

    if (grouped_scalar_pointee_row_store_starts())
        return handle_grouped_scalar_pointee_row_store(parent, bb);

    /* These cannot begin a declaration, so they are unambiguously expression
     * statements. Identifiers remain delegated to handle_declaration(), which
     * first checks whether they name a type.
     */
    if (lex_peek(T_open_bracket, NULL) || lex_peek(T_numeric, NULL) ||
        lex_peek(T_char, NULL) || lex_peek(T_string, NULL) ||
        lex_peek(T_wstring, NULL) || lex_peek(T_ampersand, NULL) ||
        lex_peek(T_asterisk, NULL) || lex_peek(T_plus, NULL) ||
        lex_peek(T_minus, NULL) || lex_peek(T_increment, NULL) ||
        lex_peek(T_decrement, NULL) || lex_peek(T_log_not, NULL) ||
        lex_peek(T_bit_not, NULL))
        return read_full_expression_statement(parent, bb);

    /* struct/union variable declaration */
    if (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL))
        return handle_record_statement(parent, bb, false, false);

    if (lex_peek(T_enum, NULL))
        return handle_enum_statement(parent, bb, false, false);

    /* Handle const qualifier for local variable declarations */
    return handle_declaration(parent, bb);
}

/* Nesting counter for read_code_block(), which recurses through
 * read_body_statement() for every nested block.
 */
int block_depth = 0;

basic_block_t *read_code_block(func_t *func, block_t *parent, basic_block_t *bb)
{
    block_t *blk = add_block(parent, func);

    if (bb)
        bb->scope = blk;

    block_depth++;
    if (block_depth > MAX_BLOCK_DEPTH)
        error_at("Block nesting too deep", cur_token_loc());

    lex_expect(T_open_curly);

    while (!lex_accept(T_close_curly)) {
        bb = read_body_statement(blk, bb);
        perform_side_effect(blk, bb);
    }

    block_depth--;
    return bb;
}

void var_add_killed_bb(var_t *var, basic_block_t *bb);

void read_func_body(func_t *func)
{
    block_t *blk = add_block(NULL, func);
    func->bbs = bb_create(blk);
    func->exit = bb_create(blk);

    if (func->returns_aggregate) {
        func->sret_def.type = func->return_def.type;
        func->sret_def.ptr_level = 1;
        func->sret_def.var_name = "__shecc_sret";
        func->sret_def.base = &func->sret_def;
        add_symbol(func->bbs, &func->sret_def);
        var_add_killed_bb(&func->sret_def, func->bbs);
    }

    for (int i = 0; i < func->num_params; i++) {
        /* arguments */
        func->param_defs[i].is_aggregate_param =
            is_record_type(func->param_defs[i].type) &&
            !func->param_defs[i].ptr_level;
        add_symbol(func->bbs, &func->param_defs[i]);
        func->param_defs[i].base = &func->param_defs[i];
        var_add_killed_bb(&func->param_defs[i], func->bbs);
    }
    basic_block_t *body = read_code_block(func, NULL, func->bbs);
    if (body)
        bb_connect(body, func->exit, NEXT);

    for (int i = 0; i < backpatch_bb_idx; i++) {
        basic_block_t *bb = backpatch_bb[i];
        insn_t *g = bb->insn_list.tail;
        label_t *label = find_label(g->str);
        if (!label)
            error_at("goto label undefined", cur_token_loc());

        label->used = true;
        bb_connect(bb, label->bb, NEXT);
    }

    for (int i = 0; i < label_idx; i++) {
        const label_t *label = &labels[i];
        if (label->used)
            continue;

        printf("Warning: unused label %s\n", label->label_name);
    }

    backpatch_bb_idx = 0;
    label_idx = 0;
}
