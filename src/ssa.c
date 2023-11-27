/* cfront does not accept structure as an argument, pass pointer */
void bb_forward_traversal(bb_traversal_args_t *args)
{
    args->bb->visited++;

    if (args->preorder_cb)
        args->preorder_cb(args->fn, args->bb);

    /* `args` is a reference, do not modify it */
    bb_traversal_args_t next_args;
    memcpy(&next_args, args, sizeof(bb_traversal_args_t));

    if (args->bb->next && args->bb->next->visited < args->fn->visited) {
        next_args.bb = args->bb->next;
        bb_forward_traversal(&next_args);
    }
    if (args->bb->then_ && args->bb->then_->visited < args->fn->visited) {
        next_args.bb = args->bb->then_;
        bb_forward_traversal(&next_args);
    }
    if (args->bb->else_ && args->bb->else_->visited < args->fn->visited) {
        next_args.bb = args->bb->else_;
        bb_forward_traversal(&next_args);
    }

    if (args->postorder_cb)
        args->postorder_cb(args->fn, args->bb);
}

/* cfront does not accept structure as an argument, pass pointer */
void bb_backward_traversal(bb_traversal_args_t *args)
{
    args->bb->visited++;

    if (args->preorder_cb)
        args->preorder_cb(args->fn, args->bb);

    int i;
    for (i = 0; i < MAX_BB_PRED; i++) {
        if (!args->bb->prev[i].bb)
            continue;
        if (args->bb->prev[i].bb->visited < args->fn->visited) {
            /* `args` is a reference, do not modify it */
            bb_traversal_args_t next_args;
            memcpy(&next_args, args, sizeof(bb_traversal_args_t));

            next_args.bb = args->bb->prev[i].bb;
            bb_backward_traversal(&next_args);
        }
    }

    if (args->postorder_cb)
        args->postorder_cb(args->fn, args->bb);
}

void bb_index_rpo(fn_t *fn, basic_block_t *bb)
{
    bb->rpo = fn->bb_cnt++;
}

void bb_reverse_index(fn_t *fn, basic_block_t *bb)
{
    bb->rpo = fn->bb_cnt - bb->rpo;
}

void bb_build_rpo(fn_t *fn, basic_block_t *bb)
{
    basic_block_t *prev, *curr;

    if (fn->bbs == bb)
        return;

    prev = fn->bbs;
    curr = prev->rpo_next;
    for (; curr; curr = curr->rpo_next) {
        if (curr->rpo < bb->rpo) {
            prev = curr;
            continue;
        }
        bb->rpo_next = curr;
        prev->rpo_next = bb;
        prev = curr;
        return;
    }

    prev->rpo_next = bb;
}

void build_rpo()
{
    fn_t *fn;
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->postorder_cb = &bb_index_rpo;
        bb_forward_traversal(args);

        fn->visited++;
        args->postorder_cb = &bb_reverse_index;
        bb_forward_traversal(args);

        fn->visited++;
        args->postorder_cb = &bb_build_rpo;
        bb_forward_traversal(args);
    }
    free(args);
}

basic_block_t *intersect(basic_block_t *i, basic_block_t *j)
{
    while (i != j) {
        while (i->rpo > j->rpo)
            i = i->idom;
        while (j->rpo > i->rpo)
            j = j->idom;
    }
    return i;
}

/**
 * Find the immediate dominator of each basic block to build the dominator tree.
 *
 * Once the dominator tree is built, we can perform the more advanced
 * optimiaztion according to the liveness analysis and the reachability
 * analysis, e.g. common subexpression elimination, loop optimiaztion or dead
 * code elimination .
 *
 * Reference: Cooper, Keith D.; Harvey, Timothy J.; Kennedy, Ken (2001). "A
 *            Simple, Fast Dominance Algorithm"
 */
void build_idom()
{
    fn_t *fn;
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        int changed;

        fn->bbs->idom = fn->bbs;

        do {
            changed = 0;

            basic_block_t *bb;
            for (bb = fn->bbs->rpo_next; bb; bb = bb->rpo_next) {
                /* pick one predecessor */
                basic_block_t *pred;
                int i;
                for (i = 0; i < MAX_BB_PRED; i++) {
                    if (!bb->prev[i].bb)
                        continue;
                    if (!bb->prev[i].bb->idom)
                        continue;
                    pred = bb->prev[i].bb;
                    break;
                }

                for (i = 0; i < MAX_BB_PRED; i++) {
                    if (!bb->prev[i].bb)
                        continue;
                    if (bb->prev[i].bb == pred)
                        continue;
                    if (bb->prev[i].bb->idom)
                        pred = intersect(bb->prev[i].bb, pred);
                }
                if (bb->idom != pred) {
                    bb->idom = pred;
                    changed = 1;
                }
            }
        } while (changed);
    }
}

int dom_connect(basic_block_t *pred, basic_block_t *succ)
{
    if (succ->dom_prev)
        return 0;

    int i, found = 0;
    for (i = 0; i < MAX_BB_DOM_SUCC; i++) {
        if (pred->dom_next[i] == succ) {
            found = 1;
            return 0;
        } else if (!pred->dom_next[i])
            break;
    }

    if (i > MAX_BB_DOM_SUCC - 1) {
        printf("Error: too many predecessors\n");
        abort();
    }

    pred->dom_next[i++] = succ;
    succ->dom_prev = pred;
    return 1;
}

void bb_build_dom(fn_t *fn, basic_block_t *bb)
{
    basic_block_t *curr = bb;
    while (curr != fn->bbs) {
        if (!dom_connect(curr->idom, curr))
            break;
        curr = curr->idom;
    }
}

void build_dom()
{
    fn_t *fn;
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->preorder_cb = &bb_build_dom;
        bb_forward_traversal(args);
    }
    free(args);
}

void bb_build_df(fn_t *fn, basic_block_t *bb)
{
    int i, cnt = 0;
    for (i = 0; i < MAX_BB_PRED; i++)
        if (bb->prev[i].bb)
            cnt++;

    if (cnt > 1)
        for (i = 0; i < MAX_BB_PRED; i++)
            if (bb->prev[i].bb) {
                basic_block_t *curr;
                for (curr = bb->prev[i].bb; curr != bb->idom; curr = curr->idom)
                    curr->DF[curr->df_idx++] = bb;
            }
}

void build_df()
{
    fn_t *fn;
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->postorder_cb = &bb_build_df;
        bb_forward_traversal(args);
    }
    free(args);
}

int var_check_killed(var_t *var, basic_block_t *bb)
{
    int i;
    for (i = 0; i < bb->live_kill_idx; i++)
        if (bb->live_kill[i] == var)
            return 1;
    return 0;
}

void bb_add_killed_var(basic_block_t *bb, var_t *var)
{
    int i, found = 0;
    for (i = 0; i < bb->live_kill_idx; i++)
        if (bb->live_kill[i] == var) {
            found = 1;
            break;
        }

    if (found)
        return;

    bb->live_kill[bb->live_kill_idx++] = var;
}

void var_add_killed_bb(var_t *var, basic_block_t *bb)
{
    int found = 0;
    ref_block_t *ref;
    for (ref = var->ref_block_list.head; ref; ref = ref->next)
        if (ref->bb == bb) {
            found = 1;
            break;
        }

    if (found)
        return;

    ref = calloc(1, sizeof(ref_block_t));
    ref->bb = bb;
    if (!var->ref_block_list.head)
        var->ref_block_list.head = ref;
    else
        var->ref_block_list.tail->next = ref;

    var->ref_block_list.tail = ref;
}

void fn_add_global(fn_t *fn, var_t *var)
{
    int found = 0;
    symbol_t *sym;
    for (sym = fn->global_sym_list.head; sym; sym = sym->next)
        if (sym->var == var) {
            found = 1;
            break;
        }

    if (found)
        return;

    sym = calloc(1, sizeof(symbol_t));
    sym->var = var;
    if (!fn->global_sym_list.head) {
        sym->index = 0;
        fn->global_sym_list.head = sym;
        fn->global_sym_list.tail = sym;
    } else {
        sym->index = fn->global_sym_list.tail->index + 1;
        fn->global_sym_list.tail->next = sym;
        fn->global_sym_list.tail = sym;
    }
}

void bb_solve_globals(fn_t *fn, basic_block_t *bb)
{
    insn_t *insn;
    for (insn = bb->insn_list.head; insn; insn = insn->next) {
        if (insn->rs1)
            if (!var_check_killed(insn->rs1, bb))
                fn_add_global(bb->belong_to, insn->rs1);
        if (insn->rs2)
            if (!var_check_killed(insn->rs2, bb))
                fn_add_global(bb->belong_to, insn->rs2);
        if (insn->rd) {
            bb_add_killed_var(bb, insn->rd);
            var_add_killed_bb(insn->rd, bb);
        }
    }
}

void solve_globals()
{
    fn_t *fn;
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->postorder_cb = &bb_solve_globals;
        bb_forward_traversal(args);
    }
    free(args);
}

int var_check_in_scope(var_t *var, block_t *block)
{
    func_t *fn = block->func;

    while (block) {
        int i;
        for (i = 0; i < block->next_local; i++)
            if (&(block->locals[i]) == var)
                return 1;
        block = block->parent;
    }

    int i;
    for (i = 0; i < fn->num_params; i++)
        if (&fn->param_defs[i] == var)
            return 1;

    return 0;
}

int insert_phi_insn(basic_block_t *bb, var_t *var)
{
    insn_t *insn;
    int found = 0;
    for (insn = bb->insn_list.head; insn; insn = insn->next)
        if (insn->opcode == OP_phi)
            if (insn->rd == var) {
                found = 1;
                break;
            }
    if (found)
        return 0;

    insn_t *head = bb->insn_list.head;
    insn_t *n = calloc(1, sizeof(insn_t));
    n->opcode = OP_phi;
    n->rd = var;
    n->rs1 = var;
    n->rs2 = var;
    if (!head) {
        bb->insn_list.head = n;
        bb->insn_list.tail = n;
    } else {
        n->next = head;
        bb->insn_list.head = n;
    }
    return 1;
}

void solve_phi_insertion()
{
    fn_t *fn;
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        symbol_t *sym;
        for (sym = fn->global_sym_list.head; sym; sym = sym->next) {
            var_t *var = sym->var;

            basic_block_t *work_list[64];
            int work_list_idx = 0;

            ref_block_t *ref;
            for (ref = var->ref_block_list.head; ref; ref = ref->next)
                work_list[work_list_idx++] = ref->bb;

            int i;
            for (i = 0; i < work_list_idx; i++) {
                basic_block_t *bb = work_list[i];
                int j;
                for (j = 0; j < bb->df_idx; j++) {
                    basic_block_t *df = bb->DF[j];
                    if (!var_check_in_scope(var, df->scope))
                        continue;

                    int is_decl = 0;
                    symbol_t *s;
                    for (s = df->symbol_list.head; s; s = s->next)
                        if (s->var == var) {
                            is_decl = 1;
                            break;
                        }

                    if (is_decl)
                        continue;

                    if (df == fn->exit)
                        continue;

                    if (var->is_global)
                        continue;

                    if (insert_phi_insn(df, var)) {
                        int l, found = 0;
                        for (l = 0; l < work_list_idx; l++)
                            if (work_list[l] == df) {
                                found = 1;
                                break;
                            }
                        if (!found)
                            work_list[work_list_idx++] = df;
                    }
                }
            }
        }
    }
}

var_t *require_var(block_t *blk);

void new_name(block_t *block, var_t **var)
{
    if (!(*var)->base)
        (*var)->base = *var;
    if ((*var)->is_global)
        return;

    int i = (*var)->base->rename.counter++;
    (*var)->base->rename.stack[(*var)->base->rename.stack_idx++] = i;

    var_t *vd = require_var(block);
    memcpy(vd, *var, sizeof(var_t));
    vd->base = *var;
    vd->subscript = i;
    (*var)->subscripts[(*var)->subscripts_idx++] = vd;
    *var = vd;
}

var_t *get_stack_top_subscript_var(var_t *var)
{
    int sub = var->base->rename.stack[var->base->rename.stack_idx - 1];
    int i;
    for (i = 0; i < var->base->subscripts_idx; i++)
        if (var->base->subscripts[i]->subscript == sub)
            return var->base->subscripts[i];

    abort();
}

void rename_var(block_t *block, var_t **var)
{
    if (!(*var)->base)
        (*var)->base = *var;
    if ((*var)->is_global)
        return;

    *var = get_stack_top_subscript_var(*var);
}

void pop_name(var_t *var)
{
    if ((*var).is_global)
        return;
    var->base->rename.stack_idx--;
}

void append_phi_operand(insn_t *insn, var_t *var, basic_block_t *bb_from)
{
    phi_operand_t *op = calloc(1, sizeof(phi_operand_t));
    op->from = bb_from;
    op->var = get_stack_top_subscript_var(var);

    phi_operand_t *tail = insn->phi_ops;
    if (tail) {
        while (tail->next)
            tail = tail->next;
        tail->next = op;
    } else
        insn->phi_ops = op;
}

void bb_solve_phi_params(basic_block_t *bb)
{
    insn_t *insn;
    for (insn = bb->insn_list.head; insn; insn = insn->next)
        if (insn->opcode == OP_phi)
            new_name(bb->scope, &insn->rd);
        else {
            if (insn->rs1)
                rename_var(bb->scope, &insn->rs1);
            if (insn->rs2)
                if (!insn->rs2->is_func)
                    rename_var(bb->scope, &insn->rs2);
            if (insn->rd)
                new_name(bb->scope, &insn->rd);
        }

    if (bb->next)
        for (insn = bb->next->insn_list.head; insn; insn = insn->next)
            if (insn->opcode == OP_phi)
                append_phi_operand(insn, insn->rd, bb);

    if (bb->then_)
        for (insn = bb->then_->insn_list.head; insn; insn = insn->next)
            if (insn->opcode == OP_phi)
                append_phi_operand(insn, insn->rd, bb);

    if (bb->else_)
        for (insn = bb->else_->insn_list.head; insn; insn = insn->next)
            if (insn->opcode == OP_phi)
                append_phi_operand(insn, insn->rd, bb);

    int i;
    for (i = 0; i < MAX_BB_DOM_SUCC; i++) {
        if (!bb->dom_next[i])
            break;
        bb_solve_phi_params(bb->dom_next[i]);
    }

    for (insn = bb->insn_list.head; insn; insn = insn->next)
        if (insn->opcode == OP_phi)
            pop_name(insn->rd);
        else if (insn->rd)
            pop_name(insn->rd);
}

void solve_phi_params()
{
    fn_t *fn;
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        int i;
        for (i = 0; i < fn->func->num_params; i++) {
            /* FIXME: Rename arguments directly, might be not good here. */
            var_t *var = require_var(fn->bbs->scope);
            var_t *base = &fn->func->param_defs[i];
            memcpy(var, base, sizeof(var_t));
            var->base = base;
            var->subscript = 0;

            base->rename.stack[base->rename.stack_idx++] =
                base->rename.counter++;
            base->subscripts[base->subscripts_idx++] = var;
        }

        bb_solve_phi_params(fn->bbs);
    }
}

void append_unwound_phi_insn(basic_block_t *bb, var_t *dest, var_t *rs)
{
    insn_t *n = calloc(1, sizeof(insn_t));
    n->opcode = OP_unwound_phi;
    n->rd = dest;
    n->rs1 = rs;

    insn_t *tail = bb->insn_list.tail;
    if (!tail) {
        bb->insn_list.head = n;
        bb->insn_list.tail = n;
    } else {
        /* insert it before branch instruction */
        if (tail->opcode == OP_branch) {
            insn_t *prev = bb->insn_list.head;
            while (prev->next != tail)
                prev = prev->next;
            prev->next = n;
            n->next = tail;
        } else {
            bb->insn_list.tail->next = n;
            bb->insn_list.tail = n;
        }
    }
}

void bb_unwind_phi(fn_t *fn, basic_block_t *bb)
{
    insn_t *insn;
    for (insn = bb->insn_list.head; insn; insn = insn->next) {
        if (insn->opcode != OP_phi)
            break;

        phi_operand_t *operand;
        for (operand = insn->phi_ops; operand; operand = operand->next) {
            append_unwound_phi_insn(operand->from, insn->rd, operand->var);
        }
        /* TODO: Release dangling phi instruction */
    }

    bb->insn_list.head = insn;
    if (!insn)
        bb->insn_list.tail = NULL;
}

void unwind_phi()
{
    fn_t *fn;
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->preorder_cb = &bb_unwind_phi;
        bb_forward_traversal(args);
    }
    free(args);
}

void bb_dump_connection(FILE *fd,
                        basic_block_t *curr,
                        basic_block_t *next,
                        bb_connection_type_t type)
{
    char *str;

    switch (type) {
    case NEXT:
        str = &"%s_%p:s->%s_%p:n\n"[0];
        break;
    case THEN:
        str = &"%s_%p:sw->%s_%p:n\n"[0];
        break;
    case ELSE:
        str = &"%s_%p:se->%s_%p:n\n"[0];
        break;
    default:
        abort();
    }

    char *pred;
    void *pred_id;
    if (curr->insn_list.tail) {
        pred = &"insn"[0];
        pred_id = curr->insn_list.tail;
    } else {
        pred = &"pseudo"[0];
        pred_id = curr;
    }

    char *succ;
    void *succ_id;
    if (next->insn_list.tail) {
        succ = &"insn"[0];
        succ_id = next->insn_list.head;
    } else {
        succ = &"pseudo"[0];
        succ_id = next;
    }

    fprintf(fd, str, pred, pred_id, succ, succ_id);
}

/* escape character for the tag in dot file */
char *get_insn_op(insn_t *insn)
{
    switch (insn->opcode) {
    case OP_add:
        return "+";
    case OP_sub:
        return "-";
    case OP_mul:
        return "*";
    case OP_div:
        return "/";
    case OP_mod:
        return "%%";
    case OP_lshift:
        return "&lt;&lt;";
    case OP_rshift:
        return "&gt;&gt;";
    case OP_eq:
        return "==";
    case OP_neq:
        return "!=";
    case OP_gt:
        return "&gt;";
    case OP_lt:
        return "&lt;";
    case OP_geq:
        return "&gt;=";
    case OP_leq:
        return "&lt;=";
    case OP_bit_and:
        return "&amp;";
    case OP_bit_or:
        return "|";
    case OP_bit_xor:
        return "^";
    case OP_log_and:
        return "&amp;&amp;";
    case OP_log_or:
        return "||";
    default:
        printf("Unknown opcode");
        abort();
    }
}

void bb_dump(FILE *fd, fn_t *fn, basic_block_t *bb)
{
    bb->visited++;

    int next_ = 0, then_ = 0, else_ = 0;
    if (bb->next)
        next_ = 1;
    if (bb->then_)
        then_ = 1;
    if (bb->else_)
        else_ = 1;
    if (then_ && !else_)
        printf("Warning: missing false branch\n");
    if (!then_ && else_)
        printf("Warning: missing true branch\n");
    if (next_ && (then_ || else_))
        printf("Warning: normal BB with condition\n");

    fprintf(fd, "subgraph cluster_%p {\n", bb);
    fprintf(fd, "label=\"BasicBlock %p\"\n", bb);

    insn_t *insn = bb->insn_list.head;
    if (!insn)
        fprintf(fd, "pseudo_%p [label=\"pseudo\"]\n", bb);
    if (!insn && (then_ || else_))
        printf("Warning: pseudo node should only have NEXT\n");

    for (; insn; insn = insn->next) {
        if (insn->opcode == OP_phi) {
            fprintf(fd, "insn_%p [label=", insn);
            fprintf(fd, "<%s<SUB>%d</SUB> := PHI(%s<SUB>%d</SUB>",
                    insn->rd->var_name, insn->rd->subscript,
                    insn->phi_ops->var->var_name,
                    insn->phi_ops->var->subscript);

            phi_operand_t *op;
            for (op = insn->phi_ops->next; op; op = op->next) {
                fprintf(fd, ", %s<SUB>%d</SUB>", op->var->var_name,
                        op->var->subscript);
            }
            fprintf(fd, ")>]\n");
        } else {
            char str[256];
            switch (insn->opcode) {
            case OP_allocat:
                sprintf(str, "<%s<SUB>%d</SUB> := ALLOC>", insn->rd->var_name,
                        insn->rd->subscript);
                break;
            case OP_load_constant:
                sprintf(str, "<%s<SUB>%d</SUB> := CONST %d>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rd->init_val);
                break;
            case OP_load_data_address:
                sprintf(str, "<%s<SUB>%d</SUB> := [.data] + %d>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rd->init_val);
                break;
            case OP_address_of:
                sprintf(str, "<%s<SUB>%d</SUB> := &amp;%s<SUB>%d</SUB>>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript);
                break;
            case OP_assign:
                sprintf(str, "<%s<SUB>%d</SUB> := %s<SUB>%d</SUB>>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript);
                break;
            case OP_read:
                sprintf(str, "<%s<SUB>%d</SUB> := (%s<SUB>%d</SUB>)>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript);
                break;
            case OP_write:
                if (insn->rs2->is_func)
                    sprintf(str, "<(%s<SUB>%d</SUB>) := %s>",
                            insn->rs1->var_name, insn->rs1->subscript,
                            insn->rs2->var_name);
                else
                    sprintf(str, "<(%s<SUB>%d</SUB>) := %s<SUB>%d</SUB>>",
                            insn->rs1->var_name, insn->rs1->subscript,
                            insn->rs2->var_name, insn->rs2->subscript);
                break;
            case OP_branch:
                sprintf(str, "<BRANCH %s<SUB>%d</SUB>>", insn->rs1->var_name,
                        insn->rs1->subscript);
                break;
            case OP_push:
                sprintf(str, "<PUSH %s<SUB>%d</SUB>>", insn->rs1->var_name,
                        insn->rs1->subscript);
                break;
            case OP_call:
                sprintf(str, "<CALL @%s>", insn->str);
                break;
            case OP_indirect:
                sprintf(str, "<INDIRECT CALL>");
                break;
            case OP_return:
                if (insn->rs1)
                    sprintf(str, "<RETURN %s<SUB>%d</SUB>>",
                            insn->rs1->var_name, insn->rs1->subscript);
                else
                    sprintf(str, "<RETURN>");
                break;
            case OP_func_ret:
                sprintf(str, "<%s<SUB>%d</SUB> := RETURN VALUE>",
                        insn->rd->var_name, insn->rd->subscript);
                break;
            case OP_add:
            case OP_sub:
            case OP_mul:
            case OP_div:
            case OP_mod:
            case OP_lshift:
            case OP_rshift:
            case OP_eq:
            case OP_neq:
            case OP_gt:
            case OP_lt:
            case OP_geq:
            case OP_leq:
            case OP_bit_and:
            case OP_bit_or:
            case OP_bit_xor:
            case OP_log_and:
            case OP_log_or:
                sprintf(
                    str,
                    "<%s<SUB>%d</SUB> := %s<SUB>%d</SUB> %s %s<SUB>%d</SUB>>",
                    insn->rd->var_name, insn->rd->subscript,
                    insn->rs1->var_name, insn->rs1->subscript,
                    get_insn_op(insn), insn->rs2->var_name,
                    insn->rs2->subscript);
                break;
            case OP_negate:
                sprintf(str, "<%s<SUB>%d</SUB> := -%s<SUB>%d</SUB>>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript);
                break;
            case OP_bit_not:
                sprintf(str, "<%s<SUB>%d</SUB> := ~%s<SUB>%d</SUB>>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript);
                break;
            case OP_log_not:
                sprintf(str, "<%s<SUB>%d</SUB> := !%s<SUB>%d</SUB>>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript);
                break;
            default:
                printf("Unknown opcode\n");
                abort();
            }
            fprintf(fd, "insn_%p [label=%s]\n", insn, str);
        }

        if (insn->next)
            fprintf(fd, "insn_%p->insn_%p [weight=100]\n", insn, insn->next);
    }
    fprintf(fd, "}\n");

    if (bb->next && bb->next->visited < fn->visited) {
        bb_dump(fd, fn, bb->next);
        bb_dump_connection(fd, bb, bb->next, NEXT);
    }
    if (bb->then_ && bb->then_->visited < fn->visited) {
        bb_dump(fd, fn, bb->then_);
        bb_dump_connection(fd, bb, bb->then_, THEN);
    }
    if (bb->else_ && bb->else_->visited < fn->visited) {
        bb_dump(fd, fn, bb->else_);
        bb_dump_connection(fd, bb, bb->else_, ELSE);
    }

    int i;
    for (i = 0; i < MAX_BB_PRED; i++)
        if (bb->prev[i].bb)
            bb_dump_connection(fd, bb->prev[i].bb, bb, bb->prev[i].type);
}

void dump_cfg(char name[])
{
    FILE *fd = fopen(name, "w");
    fn_t *fn;

    fprintf(fd, "strict digraph CFG {\n");
    fprintf(fd, "node [shape=box]\n");
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        fn->visited++;
        fprintf(fd, "subgraph cluster_%p {\n", fn);
        fprintf(fd, "label=\"%p\"\n", fn);
        bb_dump(fd, fn, fn->bbs);
        fprintf(fd, "}\n");
    }
    fprintf(fd, "}\n");
    fclose(fd);
}

void dom_dump(FILE *fd, basic_block_t *bb)
{
    fprintf(fd, "\"%p\"\n", bb);
    int i;
    for (i = 0; i < MAX_BB_DOM_SUCC; i++) {
        if (!bb->dom_next[i])
            break;
        dom_dump(fd, bb->dom_next[i]);
        fprintf(fd, "\"%p\":s->\"%p\":n\n", bb, bb->dom_next[i]);
    }
}

void dump_dom(char name[])
{
    FILE *fd = fopen(name, "w");
    fn_t *fn;

    fprintf(fd, "strict digraph DOM {\n");
    fprintf(fd, "node [shape=box]\n");
    fprintf(fd, "splines=polyline\n");
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        fprintf(fd, "subgraph cluster_%p {\n", fn);
        fprintf(fd, "label=\"%p\"\n", fn);
        dom_dump(fd, fn->bbs);
        fprintf(fd, "}\n");
    }
    fprintf(fd, "}\n");
    fclose(fd);
}

void ssa_build(int dump_ir)
{
    build_rpo();
    build_idom();
    build_dom();
    build_df();

    solve_globals();
    solve_phi_insertion();
    solve_phi_params();

    if (dump_ir) {
        dump_cfg("CFG.dot");
        dump_dom("DOM.dot");
    }

    unwind_phi();
}

void bb_index_reversed_rpo(fn_t *fn, basic_block_t *bb)
{
    bb->rpo_r = fn->bb_cnt++;
}

void bb_reverse_reversed_index(fn_t *fn, basic_block_t *bb)
{
    bb->rpo_r = fn->bb_cnt - bb->rpo_r;
}

void bb_build_reversed_rpo(fn_t *fn, basic_block_t *bb)
{
    basic_block_t *prev, *curr;
    if (fn->exit == bb)
        return;

    prev = fn->exit;
    curr = fn->exit->rpo_r_next;
    for (; curr; curr = curr->rpo_r_next) {
        if (curr->rpo_r < bb->rpo_r) {
            prev = curr;
            continue;
        }
        bb->rpo_r_next = curr;
        prev->rpo_r_next = bb;
        prev = curr;
        return;
    }

    prev->rpo_r_next = bb;
}

void build_reversed_rpo()
{
    fn_t *fn;
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        fn->bb_cnt = 0;
        args->fn = fn;
        args->bb = fn->exit;

        fn->visited++;
        args->postorder_cb = &bb_index_reversed_rpo;
        bb_backward_traversal(args);

        fn->visited++;
        args->postorder_cb = &bb_reverse_reversed_index;
        bb_backward_traversal(args);

        fn->visited++;
        args->postorder_cb = &bb_build_reversed_rpo;
        bb_backward_traversal(args);
    }
    free(args);
}

void bb_reset_live_kill_idx(fn_t *fn, basic_block_t *bb)
{
    bb->live_kill_idx = 0;
}

void add_live_gen(basic_block_t *bb, var_t *var)
{
    if (var->is_global)
        return;

    int i;
    for (i = 0; i < bb->live_gen_idx; i++)
        if (bb->live_gen[i] == var)
            return;
    bb->live_gen[bb->live_gen_idx++] = var;
}

void update_consumed(insn_t *insn, var_t *var)
{
    if (insn->idx > var->consumed)
        var->consumed = insn->idx;
}

void bb_solve_locals(fn_t *fn, basic_block_t *bb)
{
    int i = 0;
    insn_t *insn;
    for (insn = bb->insn_list.head; insn; insn = insn->next) {
        insn->idx = i++;

        if (insn->rs1) {
            if (!var_check_killed(insn->rs1, bb))
                add_live_gen(bb, insn->rs1);
            update_consumed(insn, insn->rs1);
        }
        if (insn->rs2) {
            if (!var_check_killed(insn->rs2, bb))
                add_live_gen(bb, insn->rs2);
            update_consumed(insn, insn->rs2);
        }
        if (insn->rd)
            if (insn->opcode != OP_unwound_phi)
                bb_add_killed_var(bb, insn->rd);
    }
}

void add_live_in(basic_block_t *bb, var_t *var)
{
    int i;
    for (i = 0; i < bb->live_in_idx; i++)
        if (bb->live_in[i] == var)
            return;
    bb->live_in[bb->live_in_idx++] = var;
}

void compute_live_in(basic_block_t *bb)
{
    bb->live_in_idx = 0;

    int i;
    for (i = 0; i < bb->live_out_idx; i++) {
        if (var_check_killed(bb->live_out[i], bb))
            continue;
        add_live_in(bb, bb->live_out[i]);
    }
    for (i = 0; i < bb->live_gen_idx; i++)
        add_live_in(bb, bb->live_gen[i]);
}

void merge_live_in(var_t *live_out[], int *live_out_idx, basic_block_t *bb)
{
    int i;
    for (i = 0; i < bb->live_in_idx; i++) {
        int j, found = 0;
        for (j = 0; j < *live_out_idx; j++)
            if (live_out[j] == bb->live_in[i]) {
                found = 1;
                break;
            }
        if (!found)
            live_out[(*live_out_idx)++] = bb->live_in[i];
    }
}

int recompute_live_out(fn_t *fn, basic_block_t *bb)
{
    var_t *live_out[64];
    int live_out_idx = 0;

    if (bb->next) {
        compute_live_in(bb->next);
        merge_live_in(live_out, &live_out_idx, bb->next);
    }
    if (bb->then_) {
        compute_live_in(bb->then_);
        merge_live_in(live_out, &live_out_idx, bb->then_);
    }
    if (bb->else_) {
        compute_live_in(bb->else_);
        merge_live_in(live_out, &live_out_idx, bb->else_);
    }

    if (bb->live_out_idx != live_out_idx) {
        memcpy(bb->live_out, live_out, sizeof(var_t *) * live_out_idx);
        bb->live_out_idx = live_out_idx;
        return 1;
    }

    int i;
    for (i = 0; i < live_out_idx; i++) {
        int j, same = 0;
        for (j = 0; j < bb->live_out_idx; j++)
            if (live_out[i] == bb->live_out[j]) {
                same = 1;
                break;
            }
        if (!same) {
            memcpy(bb->live_out, live_out, sizeof(var_t *) * live_out_idx);
            bb->live_out_idx = live_out_idx;
            return 1;
        }
    }
    return 0;
}

void liveness_analysis()
{
    build_reversed_rpo();

    fn_t *fn;
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->preorder_cb = &bb_reset_live_kill_idx;
        bb_forward_traversal(args);

        int i;
        for (i = 0; i < fn->func->num_params; i++)
            bb_add_killed_var(fn->bbs, fn->func->param_defs[i].subscripts[0]);

        fn->visited++;
        args->preorder_cb = bb_solve_locals;
        bb_forward_traversal(args);
    }
    free(args);

    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        basic_block_t *bb = fn->exit;
        int changed;
        do {
            changed = 0;
            for (bb = fn->exit; bb; bb = bb->rpo_r_next)
                changed |= recompute_live_out(fn, bb);
        } while (changed);
    }
}

void bb_release(fn_t *fn, basic_block_t *bb)
{
    insn_t *insn = bb->insn_list.head;
    insn_t *next_insn;
    while (insn) {
        next_insn = insn->next;
        free(insn);
        insn = next_insn;
    }

    /* disconnect all predecessors */
    int i;
    for (i = 0; i < MAX_BB_PRED; i++) {
        if (!bb->prev[i].bb)
            continue;
        switch (bb->prev[i].type) {
        case NEXT:
            bb->prev[i].bb->next = NULL;
            break;
        case THEN:
            bb->prev[i].bb->then_ = NULL;
            break;
        case ELSE:
            bb->prev[i].bb->else_ = NULL;
            break;
        default:
            abort();
        }

        bb->prev[i].bb = NULL;
    }
    free(bb);
}

void ssa_release()
{
    fn_t *fn;
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->postorder_cb = &bb_release;
        bb_forward_traversal(args);
    }
    free(args);
}
