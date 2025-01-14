/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* cfront does not accept structure as an argument, pass pointer */
void bb_forward_traversal(bb_traversal_args_t *args)
{
    args->bb->visited++;

    if (args->preorder_cb)
        args->preorder_cb(args->fn, args->bb);

    /* 'args' is a reference, do not modify it */
    bb_traversal_args_t next_args;
    memcpy(&next_args, args, sizeof(bb_traversal_args_t));

    if (args->bb->next) {
        if (args->bb->next->visited < args->fn->visited) {
            next_args.bb = args->bb->next;
            bb_forward_traversal(&next_args);
        }
    }
    if (args->bb->then_) {
        if (args->bb->then_->visited < args->fn->visited) {
            next_args.bb = args->bb->then_;
            bb_forward_traversal(&next_args);
        }
    }
    if (args->bb->else_) {
        if (args->bb->else_->visited < args->fn->visited) {
            next_args.bb = args->bb->else_;
            bb_forward_traversal(&next_args);
        }
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

    for (int i = 0; i < MAX_BB_PRED; i++) {
        if (!args->bb->prev[i].bb)
            continue;
        if (args->bb->prev[i].bb->visited < args->fn->visited) {
            /* 'args' is a reference, do not modify it */
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
    if (fn->bbs == bb)
        return;

    basic_block_t *prev = fn->bbs;
    basic_block_t *curr = prev->rpo_next;
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
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->postorder_cb = bb_index_rpo;
        bb_forward_traversal(args);

        fn->visited++;
        args->postorder_cb = bb_reverse_index;
        bb_forward_traversal(args);

        fn->visited++;
        args->postorder_cb = bb_build_rpo;
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

/* Find the immediate dominator of each basic block to build the dominator tree.
 *
 * Once the dominator tree is built, we can perform the more advanced
 * optimiaztion according to the liveness analysis and the reachability
 * analysis, e.g. common subexpression elimination, loop optimiaztion or dead
 * code elimination .
 *
 * Reference:
 *   Cooper, Keith D.; Harvey, Timothy J.; Kennedy, Ken (2001).
 *   "A Simple, Fast Dominance Algorithm"
 */
void build_idom()
{
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        bool changed;

        fn->bbs->idom = fn->bbs;

        do {
            changed = false;

            for (basic_block_t *bb = fn->bbs->rpo_next; bb; bb = bb->rpo_next) {
                /* pick one predecessor */
                basic_block_t *pred;
                for (int i = 0; i < MAX_BB_PRED; i++) {
                    if (!bb->prev[i].bb)
                        continue;
                    if (!bb->prev[i].bb->idom)
                        continue;
                    pred = bb->prev[i].bb;
                    break;
                }

                for (int i = 0; i < MAX_BB_PRED; i++) {
                    if (!bb->prev[i].bb)
                        continue;
                    if (bb->prev[i].bb == pred)
                        continue;
                    if (bb->prev[i].bb->idom)
                        pred = intersect(bb->prev[i].bb, pred);
                }
                if (bb->idom != pred) {
                    bb->idom = pred;
                    changed = true;
                }
            }
        } while (changed);
    }
}

bool dom_connect(basic_block_t *pred, basic_block_t *succ)
{
    if (succ->dom_prev)
        return false;

    int i;
    for (i = 0; i < MAX_BB_DOM_SUCC; i++) {
        if (pred->dom_next[i] == succ)
            return false;
        if (!pred->dom_next[i])
            break;
    }

    if (i > MAX_BB_DOM_SUCC - 1) {
        printf("Error: too many predecessors\n");
        abort();
    }

    pred->dom_next[i++] = succ;
    succ->dom_prev = pred;
    return true;
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
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->preorder_cb = bb_build_dom;
        bb_forward_traversal(args);
    }
    free(args);
}

void bb_build_df(fn_t *fn, basic_block_t *bb)
{
    UNUSED(fn);

    int cnt = 0;
    for (int i = 0; i < MAX_BB_PRED; i++) {
        if (bb->prev[i].bb)
            cnt++;
    }
    if (cnt <= 0)
        return;

    for (int i = 0; i < MAX_BB_PRED; i++) {
        if (bb->prev[i].bb) {
            for (basic_block_t *curr = bb->prev[i].bb; curr != bb->idom;
                 curr = curr->idom)
                curr->DF[curr->df_idx++] = bb;
        }
    }
}

void build_df()
{
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->postorder_cb = bb_build_df;
        bb_forward_traversal(args);
    }
    free(args);
}

basic_block_t *reverse_intersect(basic_block_t *i, basic_block_t *j)
{
    while (i != j) {
        while (i->rpo_r > j->rpo_r)
            i = i->r_idom;
        while (j->rpo_r > i->rpo_r)
            j = j->r_idom;
    }
    return i;
}

void build_r_idom()
{
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        bool changed;

        fn->exit->r_idom = fn->exit;

        do {
            changed = false;

            for (basic_block_t *bb = fn->exit->rpo_r_next; bb;
                 bb = bb->rpo_r_next) {
                /* pick one predecessor */
                basic_block_t *pred;
                if (bb->next && bb->next->r_idom) {
                    pred = bb->next;
                } else if (bb->else_ && bb->else_->r_idom) {
                    pred = bb->else_;
                } else if (bb->then_ && bb->then_->r_idom) {
                    pred = bb->then_;
                }

                if (bb->next && bb->next != pred && bb->next->r_idom) {
                    pred = reverse_intersect(bb->next, pred);
                }
                if (bb->else_ && bb->else_ != pred && bb->else_->r_idom) {
                    pred = reverse_intersect(bb->else_, pred);
                }
                if (bb->then_ && bb->then_ != pred && bb->then_->r_idom) {
                    pred = reverse_intersect(bb->then_, pred);
                }
                if (bb->r_idom != pred) {
                    bb->r_idom = pred;
                    changed = true;
                }
            }
        } while (changed);
    }
}

bool rdom_connect(basic_block_t *pred, basic_block_t *succ)
{
    if (succ->rdom_prev)
        return false;
    int i;
    for (i = 0; i < MAX_BB_RDOM_SUCC; i++) {
        if (pred->rdom_next[i] == succ)
            return false;
        if (!pred->rdom_next[i])
            break;
    }

    if (i > MAX_BB_RDOM_SUCC - 1) {
        printf("Error: too many predecessors\n");
        abort();
    }

    pred->rdom_next[i++] = succ;
    succ->rdom_prev = pred;
    return true;
}

void bb_build_rdom(fn_t *fn, basic_block_t *bb)
{
    for (basic_block_t *curr = bb; curr != fn->exit; curr = curr->r_idom) {
        if (!rdom_connect(curr->r_idom, curr))
            break;
    }
}

void build_rdom()
{
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->exit;

        fn->visited++;
        args->preorder_cb = bb_build_rdom;
        bb_backward_traversal(args);
    }
    free(args);
}

void bb_build_rdf(fn_t *fn, basic_block_t *bb)
{
    UNUSED(fn);

    int cnt = 0;
    if (bb->next)
        cnt++;
    if (bb->then_)
        cnt++;
    if (bb->else_)
        cnt++;
    if (cnt <= 0)
        return;

    if (bb->next) {
        for (basic_block_t *curr = bb->next; curr != bb->r_idom;
             curr = curr->r_idom)
            curr->RDF[curr->rdf_idx++] = bb;
    }
    if (bb->else_) {
        for (basic_block_t *curr = bb->else_; curr != bb->r_idom;
             curr = curr->r_idom)
            curr->RDF[curr->rdf_idx++] = bb;
    }
    if (bb->then_) {
        for (basic_block_t *curr = bb->then_; curr != bb->r_idom;
             curr = curr->r_idom)
            curr->RDF[curr->rdf_idx++] = bb;
    }
}

void build_rdf()
{
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->exit;

        fn->visited++;
        args->postorder_cb = bb_build_rdf;
        bb_backward_traversal(args);
    }
    free(args);
}

void use_chain_add_tail(insn_t *i, var_t *var)
{
    use_chain_t *u = calloc(1, sizeof(use_chain_t));
    if (!u) {
        printf("calloc failed\n");
        abort();
    }

    u->insn = i;
    if (!var->users_head)
        var->users_head = u;
    else
        var->users_tail->next = u;
    u->prev = var->users_tail;
    var->users_tail = u;
}

void use_chain_delete(use_chain_t *u, var_t *var)
{
    if (u->prev)
        u->prev->next = u->next;
    else {
        var->users_head = u->next;
        u->next->prev = NULL;
    }
    if (u->next)
        u->next->prev = u->prev;
    else {
        var->users_tail = u->prev;
        u->prev->next = NULL;
    }
    free(u);
}

void use_chain_build()
{
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        for (basic_block_t *bb = fn->bbs; bb; bb = bb->rpo_next) {
            for (insn_t *i = bb->insn_list.head; i; i = i->next) {
                if (i->rs1)
                    use_chain_add_tail(i, i->rs1);
                if (i->rs2)
                    use_chain_add_tail(i, i->rs2);
            }
        }
    }
}

bool var_check_killed(var_t *var, basic_block_t *bb)
{
    for (int i = 0; i < bb->live_kill_idx; i++) {
        if (bb->live_kill[i] == var)
            return true;
    }
    return false;
}

void bb_add_killed_var(basic_block_t *bb, var_t *var)
{
    bool found = false;
    for (int i = 0; i < bb->live_kill_idx; i++) {
        if (bb->live_kill[i] == var) {
            found = true;
            break;
        }
    }
    if (found)
        return;

    bb->live_kill[bb->live_kill_idx++] = var;
}

void var_add_killed_bb(var_t *var, basic_block_t *bb)
{
    bool found = false;
    ref_block_t *ref;
    for (ref = var->ref_block_list.head; ref; ref = ref->next) {
        if (ref->bb == bb) {
            found = true;
            break;
        }
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
    bool found = false;
    symbol_t *sym;
    for (sym = fn->global_sym_list.head; sym; sym = sym->next) {
        if (sym->var == var) {
            found = true;
            break;
        }
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
    UNUSED(fn);

    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
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
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->postorder_cb = bb_solve_globals;
        bb_forward_traversal(args);
    }
    free(args);
}

bool var_check_in_scope(var_t *var, block_t *block)
{
    func_t *fn = block->func;

    while (block) {
        for (int i = 0; i < block->next_local; i++) {
            var_t *locals = block->locals;
            if (var == &locals[i])
                return true;
        }
        block = block->parent;
    }

    for (int i = 0; i < fn->num_params; i++) {
        if (&fn->param_defs[i] == var)
            return true;
    }

    return false;
}

bool insert_phi_insn(basic_block_t *bb, var_t *var)
{
    bool found = false;
    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
        if ((insn->opcode == OP_phi) && (insn->rd == var)) {
            found = true;
            break;
        }
    }
    if (found)
        return false;

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
        head->prev = n;
        n->next = head;
        bb->insn_list.head = n;
    }
    return true;
}

void solve_phi_insertion()
{
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        for (symbol_t *sym = fn->global_sym_list.head; sym; sym = sym->next) {
            var_t *var = sym->var;

            basic_block_t *work_list[64];
            int work_list_idx = 0;

            for (ref_block_t *ref = var->ref_block_list.head; ref;
                 ref = ref->next)
                work_list[work_list_idx++] = ref->bb;

            for (int i = 0; i < work_list_idx; i++) {
                basic_block_t *bb = work_list[i];
                for (int j = 0; j < bb->df_idx; j++) {
                    basic_block_t *df = bb->DF[j];
                    if (!var_check_in_scope(var, df->scope))
                        continue;

                    bool is_decl = false;
                    for (symbol_t *s = df->symbol_list.head; s; s = s->next) {
                        if (s->var == var) {
                            is_decl = true;
                            break;
                        }
                    }

                    if (is_decl)
                        continue;

                    if (df == fn->exit)
                        continue;

                    if (var->is_global)
                        continue;

                    if (insert_phi_insn(df, var)) {
                        bool found = false;

                        /* Restrict phi insertion of ternary operation, and
                         * logical-and/or operation.
                         *
                         * The ternary and logical-and/or operations don't
                         * create new scope, so prevent temporary variable from
                         * propagating through the dominance tree.
                         */
                        if (var->is_ternary_ret || var->is_logical_ret)
                            continue;

                        for (int l = 0; l < work_list_idx; l++)
                            if (work_list[l] == df) {
                                found = true;
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
    var_t *v = *var;
    if (!v->base)
        v->base = v;
    if (v->is_global)
        return;

    int i = v->base->rename.counter++;
    v->base->rename.stack[v->base->rename.stack_idx++] = i;
    var_t *vd = require_var(block);
    memcpy(vd, *var, sizeof(var_t));
    vd->base = *var;
    vd->subscript = i;
    v->subscripts[v->subscripts_idx++] = vd;
    var[0] = vd;
}

var_t *get_stack_top_subscript_var(var_t *var)
{
    if (var->base->rename.stack_idx < 1)
        error("Index is less than 1");

    int sub = var->base->rename.stack[var->base->rename.stack_idx - 1];
    for (int i = 0; i < var->base->subscripts_idx; i++) {
        if (var->base->subscripts[i]->subscript == sub)
            return var->base->subscripts[i];
    }

    abort();
}

void rename_var(var_t **var)
{
    var_t *v = *var;
    if (!v->base)
        v->base = v;
    if (v->is_global)
        return;

    var[0] = get_stack_top_subscript_var(*var);
}

void pop_name(var_t *var)
{
    if (var->is_global)
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
    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
        if (insn->opcode == OP_phi)
            new_name(bb->scope, &insn->rd);
        else {
            if (insn->rs1)
                rename_var(&insn->rs1);
            if (insn->rs2)
                if (!insn->rs2->is_func)
                    rename_var(&insn->rs2);
            if (insn->rd)
                new_name(bb->scope, &insn->rd);
        }
    }

    if (bb->next) {
        for (insn_t *insn = bb->next->insn_list.head; insn; insn = insn->next) {
            if (insn->opcode == OP_phi)
                append_phi_operand(insn, insn->rd, bb);
        }
    }

    if (bb->then_) {
        for (insn_t *insn = bb->then_->insn_list.head; insn;
             insn = insn->next) {
            if (insn->opcode == OP_phi)
                append_phi_operand(insn, insn->rd, bb);
        }
    }

    if (bb->else_) {
        for (insn_t *insn = bb->else_->insn_list.head; insn;
             insn = insn->next) {
            if (insn->opcode == OP_phi)
                append_phi_operand(insn, insn->rd, bb);
        }
    }

    for (int i = 0; i < MAX_BB_DOM_SUCC; i++) {
        if (!bb->dom_next[i])
            break;
        bb_solve_phi_params(bb->dom_next[i]);
    }

    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
        if (insn->opcode == OP_phi)
            pop_name(insn->rd);
        else if (insn->rd)
            pop_name(insn->rd);
    }
}

void solve_phi_params()
{
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        for (int i = 0; i < fn->func->num_params; i++) {
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
    n->belong_to = bb;

    insn_t *tail = bb->insn_list.tail;
    if (!tail) {
        bb->insn_list.head = n;
        bb->insn_list.tail = n;
    } else {
        /* insert it before branch instruction */
        if (tail->opcode == OP_branch) {
            if (tail->prev) {
                tail->prev->next = n;
                n->prev = tail->prev;
            } else
                bb->insn_list.head = n;

            n->next = tail;
            tail->prev = n;
        } else {
            tail->next = n;
            bb->insn_list.tail = n;
        }
    }
}

void bb_unwind_phi(fn_t *fn, basic_block_t *bb)
{
    UNUSED(fn);

    insn_t *insn;
    for (insn = bb->insn_list.head; insn; insn = insn->next) {
        if (insn->opcode != OP_phi)
            break;

        for (phi_operand_t *operand = insn->phi_ops; operand;
             operand = operand->next)
            append_unwound_phi_insn(operand->from, insn->rd, operand->var);
        /* TODO: Release dangling phi instruction */
    }

    bb->insn_list.head = insn;
    if (!insn)
        bb->insn_list.tail = NULL;
    else
        insn->prev = NULL;
}

void unwind_phi()
{
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->preorder_cb = bb_unwind_phi;
        bb_forward_traversal(args);
    }
    free(args);
}

/*
 * The current cfonrt does not yet support string literal addressing, which
 * results in the omission of basic block visualization during the stage-1 and
 * stage-2 bootstrapping phases.
 */
#ifdef __SHECC__
#else
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

    bool next_ = false, then_ = false, else_ = false;
    if (bb->next)
        next_ = true;
    if (bb->then_)
        then_ = true;
    if (bb->else_)
        else_ = true;
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

            for (phi_operand_t *op = insn->phi_ops->next; op; op = op->next) {
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

    for (int i = 0; i < MAX_BB_PRED; i++)
        if (bb->prev[i].bb)
            bb_dump_connection(fd, bb->prev[i].bb, bb, bb->prev[i].type);
}

void dump_cfg(char name[])
{
    FILE *fd = fopen(name, "w");

    fprintf(fd, "strict digraph CFG {\n");
    fprintf(fd, "node [shape=box]\n");
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
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
    for (int i = 0; i < MAX_BB_DOM_SUCC; i++) {
        if (!bb->dom_next[i])
            break;
        dom_dump(fd, bb->dom_next[i]);
        fprintf(fd, "\"%p\":s->\"%p\":n\n", bb, bb->dom_next[i]);
    }
}

void dump_dom(char name[])
{
    FILE *fd = fopen(name, "w");

    fprintf(fd, "strict digraph DOM {\n");
    fprintf(fd, "node [shape=box]\n");
    fprintf(fd, "splines=polyline\n");
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        fprintf(fd, "subgraph cluster_%p {\n", fn);
        fprintf(fd, "label=\"%p\"\n", fn);
        dom_dump(fd, fn->bbs);
        fprintf(fd, "}\n");
    }
    fprintf(fd, "}\n");
    fclose(fd);
}
#endif

void ssa_build(int dump_ir)
{
    build_rpo();
    build_idom();
    build_dom();
    build_df();

    solve_globals();
    solve_phi_insertion();
    solve_phi_params();

#ifdef __SHECC__
#else
    if (dump_ir) {
        dump_cfg("CFG.dot");
        dump_dom("DOM.dot");
    }
#endif

    unwind_phi();
}

/* Common Subexpression Elimination (CSE) */
/* TODO: release detached insns node */
bool cse(insn_t *insn, basic_block_t *bb)
{
    if (insn->opcode != OP_read)
        return false;

    insn_t *prev = insn->prev;
    if (!prev)
        return false;
    if (prev->opcode != OP_add)
        return false;
    if (prev->rd != insn->rs1)
        return false;

    var_t *def = insn->rd, *base = prev->rs1, *idx = prev->rs2;
    if (base->is_global || idx->is_global)
        return false;

    use_chain_t *rs1_delete_user = NULL;
    use_chain_t *rs2_delete_user = NULL;
    for (use_chain_t *user = base->users_head; user; user = user->next) {
        insn_t *i = user->insn;

        /* Delete the use chain nodes found in the last loop */
        if (rs1_delete_user) {
            use_chain_delete(rs1_delete_user, rs1_delete_user->insn->rs1);
            rs1_delete_user = NULL;
        }
        if (rs2_delete_user) {
            use_chain_delete(rs2_delete_user, rs2_delete_user->insn->rs2);
            rs2_delete_user = NULL;
        }
        if (i == prev)
            continue;
        if (i->opcode != OP_add)
            continue;
        if (!i->next)
            continue;
        if (i->next->opcode != OP_read)
            continue;
        if (i->rs1 != base || i->rs2 != idx)
            continue;
        basic_block_t *i_bb = i->belong_to;
        bool check_dom = 0;
        /* Check if the instructions are under the same dominate tree */
        for (;; i_bb = i_bb->idom) {
            if (i_bb == bb) {
                check_dom = true;
                break;
            }
            if (i_bb == i_bb->idom)
                break;
        }
        if (!check_dom)
            continue;

        i->next->opcode = OP_assign;
        i->next->rs1 = def;
        if (i->prev) {
            i->prev->next = i->next;
            i->next->prev = i->prev;
        } else {
            i->belong_to->insn_list.head = i->next;
            i->next->prev = NULL;
        }
        i->next->opcode = OP_assign;
        i->next->rs1 = def;
        /* Prepare information for deleting use chain nodes */
        rs1_delete_user = user;
        for (rs2_delete_user = i->rs2->users_head;
             rs2_delete_user->insn != rs1_delete_user->insn;
             rs2_delete_user = rs2_delete_user->next)
            ;
    }
    return true;
}

bool mark_const(insn_t *insn)
{
    if (insn->opcode == OP_load_constant) {
        insn->rd->is_const = true;
        return false;
    }
    if (insn->opcode != OP_assign)
        return false;

    /* The global variable is unique and has no subscripts in our SSA. Do NOT
     * evaluate its value.
     */
    if (insn->rd->is_global)
        return false;
    if (!insn->rs1->is_const) {
        if (!insn->prev)
            return false;
        if (insn->prev->opcode != OP_load_constant)
            return false;
        if (insn->rs1 != insn->prev->rd)
            return false;
    }

    insn->opcode = OP_load_constant;
    insn->rd->is_const = true;
    insn->rd->init_val = insn->rs1->init_val;
    insn->rs1 = NULL;
    return true;
}

bool eval_const_arithmetic(insn_t *insn)
{
    if (!insn->rs1)
        return false;
    if (!insn->rs1->is_const)
        return false;
    if (!insn->rs2)
        return false;
    if (!insn->rs2->is_const)
        return false;

    int res;
    int l = insn->rs1->init_val, r = insn->rs2->init_val;

    switch (insn->opcode) {
    case OP_add:
        res = l + r;
        break;
    case OP_sub:
        res = l - r;
        break;
    case OP_mul:
        res = l * r;
        break;
    case OP_div:
        res = l / r;
        break;
    case OP_mod:
        res = l % r;
        break;
    default:
        return false;
    }

    insn->rs1 = NULL;
    insn->rs2 = NULL;
    insn->rd->is_const = 1;
    insn->rd->init_val = res;
    insn->opcode = OP_load_constant;
    return true;
}

bool const_folding(insn_t *insn)
{
    if (mark_const(insn))
        return true;
    if (eval_const_arithmetic(insn))
        return true;
    return false;
}

/* initial mark useful instruction */
int dce_init_mark(insn_t *insn, insn_t *work_list[], int work_list_idx)
{
    int mark_num = 0;
    /*
     * mark instruction "useful" if it sets a return value, affects the value in
     * a storage location, or it is a function call.
     */
    switch (insn->opcode) {
    case OP_return:
    case OP_write:
    case OP_address_of:
    case OP_unwound_phi:
    case OP_allocat:
        insn->useful = true;
        insn->belong_to->useful = true;
        work_list[work_list_idx + mark_num] = insn;
        mark_num++;
        break;
    case OP_indirect:
    case OP_call:
        insn->useful = true;
        insn->belong_to->useful = true;
        work_list[work_list_idx + mark_num] = insn;
        mark_num++;
        /* mark precall and postreturn sequences at calls */
        if (insn->next && insn->next->opcode == OP_func_ret) {
            insn->next->useful = true;
            work_list[work_list_idx + mark_num] = insn;
            mark_num++;
        }
        while (insn->prev && insn->prev->opcode == OP_push) {
            insn = insn->prev;
            insn->useful = true;
            work_list[work_list_idx + mark_num] = insn;
            mark_num++;
        }
        break;
    default:
        if (!insn->rd)
            break;
        /* if the instruction affects a global value, set "useful" */
        if (insn->rd->is_global && !insn->useful) {
            insn->useful = true;
            insn->belong_to->useful = true;
            work_list[work_list_idx + mark_num] = insn;
            mark_num++;
        }
        break;
    }
    return mark_num;
}

/* Dead Code Elimination (DCE) */
void dce_insn(basic_block_t *bb)
{
    insn_t *work_list[2048];
    int work_list_idx = 0;

    /* initially analyze current bb*/
    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
        int mark_num = dce_init_mark(insn, work_list, work_list_idx);
        work_list_idx += mark_num;
        if (work_list_idx > 2048 - 1) {
            printf("size of work_list in DCE is not enough\n");
            abort();
        }
    }

    while (work_list_idx != 0) {
        insn_t *curr = work_list[--work_list_idx];
        insn_t *rs1_insn, *rs2_insn;

        /* trace back where rs1 and rs2 are assigned values */
        if (curr->rs1 && curr->rs1->last_assign) {
            rs1_insn = curr->rs1->last_assign;
            if (!rs1_insn->useful) {
                rs1_insn->useful = true;
                rs1_insn->belong_to->useful = true;
                work_list[work_list_idx++] = rs1_insn;
            }
        }
        if (curr->rs2 && curr->rs2->last_assign) {
            rs2_insn = curr->rs2->last_assign;
            if (!rs2_insn->useful) {
                rs2_insn->useful = true;
                rs2_insn->belong_to->useful = true;
                work_list[work_list_idx++] = rs2_insn;
            }
        }

        basic_block_t *rdf;
        for (int i = 0; i < curr->belong_to->rdf_idx; i++) {
            rdf = curr->belong_to->RDF[i];
            if (!rdf)
                break;
            insn_t *tail = rdf->insn_list.tail;
            if (tail->opcode == OP_branch && !tail->useful) {
                tail->useful = true;
                rdf->useful = true;
                work_list[work_list_idx++] = tail;
            }
        }
    }
}

void dce_sweep()
{
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        for (basic_block_t *bb = fn->bbs; bb; bb = bb->rpo_next) {
            for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
                if (insn->useful)
                    continue;
                /*
                 * If a branch instruction is useless, redirect to the
                 * reverse immediate dominator of this basic block and
                 * remove the branch instruction. Later, register allocation
                 * will insert a jump instruction.
                 */
                if (insn->opcode == OP_branch) {
                    basic_block_t *jump_bb = bb->r_idom;
                    bb_disconnect(bb, bb->then_);
                    bb_disconnect(bb, bb->else_);
                    while (jump_bb != bb->belong_to->exit) {
                        if (jump_bb->useful) {
                            bb_connect(bb, jump_bb, NEXT);
                            break;
                        }
                        jump_bb = jump_bb->r_idom;
                    }
                }
                /* remove useless instructions */
                if (insn->next)
                    insn->next->prev = insn->prev;
                else
                    bb->insn_list.tail = insn->prev;
                if (insn->prev)
                    insn->prev->next = insn->next;
                else
                    bb->insn_list.head = insn->next;
            }
        }
    }
}

void build_reversed_rpo();

void optimize()
{
    /* build rdf information for DCE */
    build_reversed_rpo();
    build_r_idom();
    build_rdom();
    build_rdf();

    use_chain_build();

    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        /* basic block level (control flow) optimizations */

        for (basic_block_t *bb = fn->bbs; bb; bb = bb->rpo_next) {
            /* instruction level optimizations */
            for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
                /* record the instruction assigned value to rd */
                if (insn->rd)
                    insn->rd->last_assign = insn;
                if (cse(insn, bb))
                    continue;
                if (const_folding(insn))
                    continue;
                /* more optimizations */
            }
        }
    }

    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        for (basic_block_t *bb = fn->bbs; bb; bb = bb->rpo_next) {
            dce_insn(bb);
        }
    }
    dce_sweep();
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
    if (fn->exit == bb)
        return;

    basic_block_t *prev = fn->exit;
    basic_block_t *curr = fn->exit->rpo_r_next;
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
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        fn->bb_cnt = 0;
        args->fn = fn;
        args->bb = fn->exit;

        fn->visited++;
        args->postorder_cb = bb_index_reversed_rpo;
        bb_backward_traversal(args);

        fn->visited++;
        args->postorder_cb = bb_reverse_reversed_index;
        bb_backward_traversal(args);

        fn->visited++;
        args->postorder_cb = bb_build_reversed_rpo;
        bb_backward_traversal(args);
    }
    free(args);
}

void bb_reset_live_kill_idx(fn_t *fn, basic_block_t *bb)
{
    UNUSED(fn);
    bb->live_kill_idx = 0;
}

void add_live_gen(basic_block_t *bb, var_t *var)
{
    if (var->is_global)
        return;

    for (int i = 0; i < bb->live_gen_idx; i++) {
        if (bb->live_gen[i] == var)
            return;
    }
    bb->live_gen[bb->live_gen_idx++] = var;
}

void update_consumed(insn_t *insn, var_t *var)
{
    if (insn->idx > var->consumed)
        var->consumed = insn->idx;
}

void bb_solve_locals(fn_t *fn, basic_block_t *bb)
{
    UNUSED(fn);

    int i = 0;
    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
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
    for (int i = 0; i < bb->live_in_idx; i++) {
        if (bb->live_in[i] == var)
            return;
    }
    bb->live_in[bb->live_in_idx++] = var;
}

void compute_live_in(basic_block_t *bb)
{
    bb->live_in_idx = 0;

    for (int i = 0; i < bb->live_out_idx; i++) {
        if (var_check_killed(bb->live_out[i], bb))
            continue;
        add_live_in(bb, bb->live_out[i]);
    }
    for (int i = 0; i < bb->live_gen_idx; i++)
        add_live_in(bb, bb->live_gen[i]);
}

int merge_live_in(var_t *live_out[], int live_out_idx, basic_block_t *bb)
{
    for (int i = 0; i < bb->live_in_idx; i++) {
        int found = 0;
        for (int j = 0; j < live_out_idx; j++) {
            if (live_out[j] == bb->live_in[i]) {
                found = 1;
                break;
            }
        }
        if (!found)
            live_out[live_out_idx++] = bb->live_in[i];
    }
    return live_out_idx;
}

bool recompute_live_out(basic_block_t *bb)
{
    var_t *live_out[MAX_ANALYSIS_STACK_SIZE];
    int live_out_idx = 0;

    if (bb->next) {
        compute_live_in(bb->next);
        live_out_idx = merge_live_in(live_out, live_out_idx, bb->next);
    }
    if (bb->then_) {
        compute_live_in(bb->then_);
        live_out_idx = merge_live_in(live_out, live_out_idx, bb->then_);
    }
    if (bb->else_) {
        compute_live_in(bb->else_);
        live_out_idx = merge_live_in(live_out, live_out_idx, bb->else_);
    }

    if (bb->live_out_idx != live_out_idx) {
        memcpy(bb->live_out, live_out, HOST_PTR_SIZE * live_out_idx);
        bb->live_out_idx = live_out_idx;
        return true;
    }

    for (int i = 0; i < live_out_idx; i++) {
        int same = 0;
        for (int j = 0; j < bb->live_out_idx; j++) {
            if (live_out[i] == bb->live_out[j]) {
                same = 1;
                break;
            }
        }
        if (!same) {
            memcpy(bb->live_out, live_out, HOST_PTR_SIZE * live_out_idx);
            bb->live_out_idx = live_out_idx;
            return true;
        }
    }
    return false;
}

void liveness_analysis()
{
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->preorder_cb = bb_reset_live_kill_idx;
        bb_forward_traversal(args);

        for (int i = 0; i < fn->func->num_params; i++)
            bb_add_killed_var(fn->bbs, fn->func->param_defs[i].subscripts[0]);

        fn->visited++;
        args->preorder_cb = bb_solve_locals;
        bb_forward_traversal(args);
    }
    free(args);

    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        basic_block_t *bb = fn->exit;
        bool changed;
        do {
            changed = false;
            for (bb = fn->exit; bb; bb = bb->rpo_r_next)
                changed |= recompute_live_out(bb);
        } while (changed);
    }
}

void bb_release(fn_t *fn, basic_block_t *bb)
{
    UNUSED(fn);

    insn_t *insn = bb->insn_list.head;
    insn_t *next_insn;
    while (insn) {
        next_insn = insn->next;
        free(insn);
        insn = next_insn;
    }

    /* disconnect all predecessors */
    for (int i = 0; i < MAX_BB_PRED; i++) {
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
    bb_traversal_args_t *args = calloc(1, sizeof(bb_traversal_args_t));
    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        args->fn = fn;
        args->bb = fn->bbs;

        fn->visited++;
        args->postorder_cb = bb_release;
        bb_forward_traversal(args);
    }
    free(args);
}
