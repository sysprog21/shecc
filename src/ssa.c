/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */
#include <stdio.h>
#include <string.h>

#include "defs.h"
#include "globals.c"

/* SCCP (Sparse Conditional Constant Propagation) optimization */
#include "opt-sccp.c"

/* Configuration constants - replace magic numbers */
#define PHI_WORKLIST_SIZE 128
#define DCE_WORKLIST_SIZE 2048

/* Dead store elimination window size */
#define OVERWRITE_WINDOW 3

void var_list_ensure_capacity(var_list_t *list, int min_capacity)
{
    if (list->capacity >= min_capacity)
        return;

    int new_capacity = list->capacity ? list->capacity : HOST_PTR_SIZE;

    while (new_capacity < min_capacity)
        new_capacity <<= 1;

    var_t **new_elements = arena_alloc(BB_ARENA, new_capacity * HOST_PTR_SIZE);

    if (list->elements)
        memcpy(new_elements, list->elements, list->size * HOST_PTR_SIZE);

    list->elements = new_elements;
    list->capacity = new_capacity;
}

void var_list_add_var(var_list_t *list, var_t *var)
{
    for (int i = 0; i < list->size; i++) {
        if (list->elements[i] == var)
            return;
    }

    var_list_ensure_capacity(list, list->size + 1);
    list->elements[list->size++] = var;
}

void var_list_assign_array(var_list_t *list, var_t **data, int count)
{
    var_list_ensure_capacity(list, count);
    memcpy(list->elements, data, count * HOST_PTR_SIZE);
    list->size = count;
}

/* cfront does not accept structure as an argument, pass pointer */
void bb_forward_traversal(bb_traversal_args_t *args)
{
    args->bb->visited++;

    if (args->preorder_cb)
        args->preorder_cb(args->func, args->bb);

    /* 'args' is a reference, do not modify it */
    bb_traversal_args_t next_args;
    memcpy(&next_args, args, sizeof(bb_traversal_args_t));

    if (args->bb->next) {
        if (args->bb->next->visited < args->func->visited) {
            next_args.bb = args->bb->next;
            bb_forward_traversal(&next_args);
        }
    }
    if (args->bb->then_) {
        if (args->bb->then_->visited < args->func->visited) {
            next_args.bb = args->bb->then_;
            bb_forward_traversal(&next_args);
        }
    }
    if (args->bb->else_) {
        if (args->bb->else_->visited < args->func->visited) {
            next_args.bb = args->bb->else_;
            bb_forward_traversal(&next_args);
        }
    }

    if (args->postorder_cb)
        args->postorder_cb(args->func, args->bb);
}

/* cfront does not accept structure as an argument, pass pointer */
void bb_backward_traversal(bb_traversal_args_t *args)
{
    args->bb->visited++;

    if (args->preorder_cb)
        args->preorder_cb(args->func, args->bb);

    for (int i = 0; i < MAX_BB_PRED; i++) {
        if (!args->bb->prev[i].bb)
            continue;
        if (args->bb->prev[i].bb->visited < args->func->visited) {
            /* 'args' is a reference, do not modify it */
            bb_traversal_args_t next_args;
            memcpy(&next_args, args, sizeof(bb_traversal_args_t));

            next_args.bb = args->bb->prev[i].bb;
            bb_backward_traversal(&next_args);
        }
    }

    if (args->postorder_cb)
        args->postorder_cb(args->func, args->bb);
}

void bb_index_rpo(func_t *func, basic_block_t *bb)
{
    bb->rpo = func->bb_cnt++;
}

void bb_reverse_index(func_t *func, basic_block_t *bb)
{
    bb->rpo = func->bb_cnt - bb->rpo;
}

void bb_build_rpo(func_t *func, basic_block_t *bb)
{
    if (func->bbs == bb)
        return;

    basic_block_t *prev = func->bbs;
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

void build_rpo(void)
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        args->func = func;
        args->bb = func->bbs;

        func->visited++;
        args->postorder_cb = bb_index_rpo;
        bb_forward_traversal(args);

        func->visited++;
        args->postorder_cb = bb_reverse_index;
        bb_forward_traversal(args);

        func->visited++;
        args->postorder_cb = bb_build_rpo;
        bb_forward_traversal(args);
    }
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
void build_idom(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        bool changed;

        func->bbs->idom = func->bbs;

        do {
            changed = false;

            for (basic_block_t *bb = func->bbs->rpo_next; bb;
                 bb = bb->rpo_next) {
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

    if (i > MAX_BB_DOM_SUCC - 1)
        fatal("Too many predecessors in dominator tree");

    pred->dom_next[i++] = succ;
    succ->dom_prev = pred;
    return true;
}

void bb_build_dom(func_t *func, basic_block_t *bb)
{
    basic_block_t *curr = bb;
    while (curr != func->bbs) {
        if (!dom_connect(curr->idom, curr))
            break;
        curr = curr->idom;
    }
}

void build_dom(void)
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        args->func = func;
        args->bb = func->bbs;

        func->visited++;
        args->preorder_cb = bb_build_dom;
        bb_forward_traversal(args);
    }
}

void bb_build_df(func_t *func, basic_block_t *bb)
{
    UNUSED(func);

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

void build_df(void)
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        args->func = func;
        args->bb = func->bbs;

        func->visited++;
        args->postorder_cb = bb_build_df;
        bb_forward_traversal(args);
    }
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

void build_r_idom(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        bool changed;

        func->exit->r_idom = func->exit;

        do {
            changed = false;

            for (basic_block_t *bb = func->exit->rpo_r_next; bb;
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

                if (bb->next && bb->next != pred && bb->next->r_idom)
                    pred = reverse_intersect(bb->next, pred);
                if (bb->else_ && bb->else_ != pred && bb->else_->r_idom)
                    pred = reverse_intersect(bb->else_, pred);
                if (bb->then_ && bb->then_ != pred && bb->then_->r_idom)
                    pred = reverse_intersect(bb->then_, pred);
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

    if (i > MAX_BB_RDOM_SUCC - 1)
        fatal("Too many predecessors in reverse dominator tree");

    pred->rdom_next[i++] = succ;
    succ->rdom_prev = pred;
    return true;
}

void bb_build_rdom(func_t *func, basic_block_t *bb)
{
    for (basic_block_t *curr = bb; curr != func->exit; curr = curr->r_idom) {
        if (!rdom_connect(curr->r_idom, curr))
            break;
    }
}

void build_rdom(void)
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        args->func = func;
        args->bb = func->exit;

        func->visited++;
        args->preorder_cb = bb_build_rdom;
        bb_backward_traversal(args);
    }
}

void bb_build_rdf(func_t *func, basic_block_t *bb)
{
    UNUSED(func);

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

void build_rdf(void)
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        args->func = func;
        args->bb = func->exit;

        func->visited++;
        args->postorder_cb = bb_build_rdf;
        bb_backward_traversal(args);
    }
}

void use_chain_add_tail(insn_t *i, var_t *var)
{
    use_chain_t *u = arena_calloc(INSN_ARENA, 1, sizeof(use_chain_t));

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
}

void use_chain_build(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
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
    for (int i = 0; i < bb->live_kill.size; i++) {
        if (bb->live_kill.elements[i] == var)
            return true;
    }
    return false;
}

void bb_add_killed_var(basic_block_t *bb, var_t *var)
{
    var_list_add_var(&bb->live_kill, var);
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

    ref = arena_calloc(GENERAL_ARENA, 1, sizeof(ref_block_t));
    ref->bb = bb;
    if (!var->ref_block_list.head)
        var->ref_block_list.head = ref;
    else
        var->ref_block_list.tail->next = ref;

    var->ref_block_list.tail = ref;
}

void fn_add_global(func_t *func, var_t *var)
{
    bool found = false;
    symbol_t *sym;
    for (sym = func->global_sym_list.head; sym; sym = sym->next) {
        if (sym->var == var) {
            found = true;
            break;
        }
    }
    if (found)
        return;

    sym = arena_alloc_symbol();
    sym->var = var;
    if (!func->global_sym_list.head) {
        sym->index = 0;
        func->global_sym_list.head = sym;
        func->global_sym_list.tail = sym;
    } else {
        sym->index = func->global_sym_list.tail->index + 1;
        func->global_sym_list.tail->next = sym;
        func->global_sym_list.tail = sym;
    }
}

void bb_solve_globals(func_t *func, basic_block_t *bb)
{
    UNUSED(func);

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

void solve_globals(void)
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        args->func = func;
        args->bb = func->bbs;

        func->visited++;
        args->postorder_cb = bb_solve_globals;
        bb_forward_traversal(args);
    }
}

bool var_check_in_scope(var_t *var, block_t *block)
{
    func_t *func = block->func;

    while (block) {
        for (int i = 0; i < block->locals.capacity; i++) {
            if (var == block->locals.elements[i])
                return true;
        }
        block = block->parent;
    }

    for (int i = 0; i < func->num_params; i++) {
        if (&func->param_defs[i] == var)
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
    insn_t *n = arena_calloc(INSN_ARENA, 1, sizeof(insn_t));
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

void solve_phi_insertion(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        for (symbol_t *sym = func->global_sym_list.head; sym; sym = sym->next) {
            var_t *var = sym->var;

            basic_block_t *work_list[PHI_WORKLIST_SIZE];
            int work_list_idx = 0;

            for (ref_block_t *ref = var->ref_block_list.head; ref;
                 ref = ref->next) {
                if (work_list_idx >= PHI_WORKLIST_SIZE - 1)
                    fatal("PHI worklist overflow");
                work_list[work_list_idx++] = ref->bb;
            }

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

                    if (df == func->exit)
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

                        for (int l = 0; l < work_list_idx; l++) {
                            if (work_list[l] == df) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            if (work_list_idx >= PHI_WORKLIST_SIZE - 1)
                                fatal("PHI worklist overflow");
                            work_list[work_list_idx++] = df;
                        }
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
        return var; /* fallback: use base when no prior definition */

    int sub = var->base->rename.stack[var->base->rename.stack_idx - 1];
    for (int i = 0; i < var->base->subscripts_idx; i++) {
        if (var->base->subscripts[i]->subscript == sub)
            return var->base->subscripts[i];
    }

    fatal("Failed to find subscript variable on rename stack");
    return NULL; /* unreachable, but silences compiler warning */
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
    phi_operand_t *op = arena_calloc(GENERAL_ARENA, 1, sizeof(phi_operand_t));
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

void solve_phi_params(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        for (int i = 0; i < func->num_params; i++) {
            /* FIXME: Direct argument renaming in SSA construction phase may
             * interfere with later optimization passes
             */
            var_t *var = require_var(func->bbs->scope);
            var_t *base = &func->param_defs[i];
            memcpy(var, base, sizeof(var_t));
            var->base = base;
            var->subscript = 0;

            base->rename.stack[base->rename.stack_idx++] =
                base->rename.counter++;
            base->subscripts[base->subscripts_idx++] = var;
        }

        bb_solve_phi_params(func->bbs);
    }
}

void append_unwound_phi_insn(basic_block_t *bb, var_t *dest, var_t *rs)
{
    insn_t *n = arena_calloc(INSN_ARENA, 1, sizeof(insn_t));
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

void bb_unwind_phi(func_t *func, basic_block_t *bb)
{
    UNUSED(func);

    insn_t *insn;
    for (insn = bb->insn_list.head; insn; insn = insn->next) {
        if (insn->opcode != OP_phi)
            break;

        for (phi_operand_t *operand = insn->phi_ops; operand;
             operand = operand->next)
            append_unwound_phi_insn(operand->from, insn->rd, operand->var);
    }

    bb->insn_list.head = insn;
    if (!insn)
        bb->insn_list.tail = NULL;
    else
        insn->prev = NULL;
}

void unwind_phi(void)
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        args->func = func;
        args->bb = func->bbs;

        func->visited++;
        args->preorder_cb = bb_unwind_phi;
        bb_forward_traversal(args);
    }
}

bool is_dominate(basic_block_t *pred, basic_block_t *succ)
{
    int i;
    bool found = false;
    for (i = 0; i < MAX_BB_DOM_SUCC; i++) {
        if (!pred->dom_next[i])
            break;
        if (pred->dom_next[i] == succ) {
            found = true;
            break;
        }
        found |= is_dominate(pred->dom_next[i], succ);
    }

    return found;
}

/*
 * For any variable, the basic block that defines it must dominate all the
 * basic blocks where it is used; otherwise, it is an invalid cross-block
 * initialization.
 */
void bb_check_var_cross_init(func_t *func, basic_block_t *bb)
{
    UNUSED(func);

    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
        if (insn->opcode != OP_allocat)
            continue;

        var_t *var = insn->rd;
        ref_block_t *ref;
        for (ref = var->ref_block_list.head; ref; ref = ref->next) {
            if (ref->bb == bb)
                continue;

            if (!is_dominate(bb, ref->bb))
                printf("Warning: Variable '%s' cross-initialized\n",
                       var->var_name);
        }
    }
}

/**
 * A variable's initialization lives in a basic block that does not dominate
 * all of its uses, so control flow can reach a use without first passing
 * through its initialization (i.e., a possibly-uninitialized use).
 *
 * For Example:
 * // Jumps directly to 'label', skipping the declaration below
 * goto label;
 * if (1) {
 *     // This line is never executed when 'goto' is taken
 *     int x;
 * label:
 *     // Uses 'x' after its declaration was bypassed
 *     x = 5;
 * }
 */
void check_var_cross_init()
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        args->func = func;
        args->bb = func->bbs;

        func->visited++;
        args->postorder_cb = bb_check_var_cross_init;
        bb_forward_traversal(args);
    }
}

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
        str = "%s_%p:s->%s_%p:n\n";
        break;
    case THEN:
        str = "%s_%p:sw->%s_%p:n\n";
        break;
    case ELSE:
        str = "%s_%p:se->%s_%p:n\n";
        break;
    default:
        fatal("Unknown basic block connection type");
    }

    char *pred;
    void *pred_id;
    if (curr->insn_list.tail) {
        pred = "insn";
        pred_id = curr->insn_list.tail;
    } else {
        pred = "pseudo";
        pred_id = curr;
    }

    char *succ;
    void *succ_id;
    if (next->insn_list.tail) {
        succ = "insn";
        succ_id = next->insn_list.head;
    } else {
        succ = "pseudo";
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
        fatal("Unknown opcode in operator string conversion");
        return ""; /* unreachable, but silences compiler warning */
    }
}

void bb_dump(FILE *fd, func_t *func, basic_block_t *bb)
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
    fprintf(fd, "label=\"BasicBlock %p (%s)\"\n", bb, bb->bb_label_name);

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
            case OP_load_rodata_address:
                sprintf(str, "<%s<SUB>%d</SUB> := [.rodata] + %d>",
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
            case OP_jump:
                sprintf(str, "<JUMP>");
                break;
            case OP_label:
                sprintf(str, "<LABEL>");
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
            case OP_trunc:
                sprintf(str, "<%s<SUB>%d</SUB> := trunc %s<SUB>%d</SUB>, %d>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript, insn->sz);
                break;
            case OP_sign_ext:
                sprintf(str,
                        "<%s<SUB>%d</SUB> := sign_ext %s<SUB>%d</SUB>, %d>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript, insn->sz);
                break;
            case OP_cast:
                sprintf(str, "<%s<SUB>%d</SUB> := cast %s<SUB>%d</SUB>>",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript);
                break;
            default:
                fatal("Unknown opcode in instruction dump");
            }
            fprintf(fd, "insn_%p [label=%s]\n", insn, str);
        }

        if (insn->next)
            fprintf(fd, "insn_%p->insn_%p [weight=100]\n", insn, insn->next);
    }
    fprintf(fd, "}\n");

    if (bb->next && bb->next->visited < func->visited) {
        bb_dump(fd, func, bb->next);
        bb_dump_connection(fd, bb, bb->next, NEXT);
    }
    if (bb->then_ && bb->then_->visited < func->visited) {
        bb_dump(fd, func, bb->then_);
        bb_dump_connection(fd, bb, bb->then_, THEN);
    }
    if (bb->else_ && bb->else_->visited < func->visited) {
        bb_dump(fd, func, bb->else_);
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
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        func->visited++;
        fprintf(fd, "subgraph cluster_%p {\n", func);
        fprintf(fd, "label=\"%p (%s)\"\n", func, func->return_def.var_name);
        bb_dump(fd, func, func->bbs);
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
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        fprintf(fd, "subgraph cluster_%p {\n", func);
        fprintf(fd, "label=\"%p\"\n", func);
        dom_dump(fd, func->bbs);
        fprintf(fd, "}\n");
    }
    fprintf(fd, "}\n");
    fclose(fd);
}
#endif

void ssa_build(void)
{
    build_rpo();
    build_idom();
    build_dom();
    build_df();

    solve_globals();

    check_var_cross_init();

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

/* Check if operation can be subject to CSE */
bool is_cse_candidate(insn_t *insn)
{
    switch (insn->opcode) {
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_div:
    case OP_mod:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_log_and:
    case OP_log_or:
    case OP_eq:
    case OP_neq:
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
        return true;
    default:
        return false;
    }
}

/* Common Subexpression Elimination (CSE) */
/* Enhanced to support general binary operations */
bool cse(insn_t *insn, basic_block_t *bb)
{
    /* Handle array access pattern: add + read */
    if (insn->opcode == OP_read) {
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

        /* Look for identical add+read patterns */
        for (use_chain_t *user = base->users_head; user; user = user->next) {
            insn_t *i = user->insn;
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

            /* Check dominance */
            basic_block_t *i_bb = i->belong_to;
            bool check_dom = false;
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

            /* Replace with assignment */
            i->next->opcode = OP_assign;
            i->next->rs1 = def;
            if (i->prev) {
                i->prev->next = i->next;
                i->next->prev = i->prev;
            } else {
                i->belong_to->insn_list.head = i->next;
                i->next->prev = NULL;
            }
        }
        return true;
    }

    /* Handle general binary operations */
    if (!is_cse_candidate(insn))
        return false;

    if (!insn->rs1 || !insn->rs2 || !insn->rd)
        return false;

    /* Don't CSE operations with global variables */
    if (insn->rs1->is_global || insn->rs2->is_global)
        return false;

    /* Look for identical binary operations */
    for (insn_t *other = bb->insn_list.head; other; other = other->next) {
        if (other == insn)
            break; /* Only consider earlier instructions */

        if (other->opcode != insn->opcode)
            continue;
        if (!other->rs1 || !other->rs2 || !other->rd)
            continue;

        /* Check if operands match */
        bool operands_match = false;
        if (other->rs1 == insn->rs1 && other->rs2 == insn->rs2) {
            operands_match = true;
        } else if (insn->opcode == OP_add || insn->opcode == OP_mul ||
                   insn->opcode == OP_bit_and || insn->opcode == OP_bit_or ||
                   insn->opcode == OP_bit_xor || insn->opcode == OP_log_and ||
                   insn->opcode == OP_log_or || insn->opcode == OP_eq ||
                   insn->opcode == OP_neq) {
            /* Commutative operations */
            if (other->rs1 == insn->rs2 && other->rs2 == insn->rs1) {
                operands_match = true;
            }
        }

        if (operands_match) {
            /* Replace current instruction with assignment */
            insn->opcode = OP_assign;
            insn->rs1 = other->rd;
            insn->rs2 = NULL;
            return true;
        }
    }

    return false;
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
        if (r == 0)
            return false; /* avoid division by zero */
        res = l / r;
        break;
    case OP_mod:
        if (r == 0)
            return false; /* avoid modulo by zero */
        res = l % r;
        break;
    case OP_lshift:
        res = l << r;
        break;
    case OP_rshift:
        res = l >> r;
        break;
    case OP_bit_and:
        res = l & r;
        break;
    case OP_bit_or:
        res = l | r;
        break;
    case OP_bit_xor:
        res = l ^ r;
        break;
    case OP_log_and:
        res = l && r;
        break;
    case OP_log_or:
        res = l || r;
        break;
    case OP_eq:
        res = l == r;
        break;
    case OP_neq:
        res = l != r;
        break;
    case OP_lt:
        res = l < r;
        break;
    case OP_leq:
        res = l <= r;
        break;
    case OP_gt:
        res = l > r;
        break;
    case OP_geq:
        res = l >= r;
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

bool eval_const_unary(insn_t *insn)
{
    if (!insn->rs1)
        return false;
    if (!insn->rs1->is_const)
        return false;

    int res;
    int val = insn->rs1->init_val;

    switch (insn->opcode) {
    case OP_negate:
        res = -val;
        break;
    case OP_bit_not:
        res = ~val;
        break;
    case OP_log_not:
        res = !val;
        break;
    default:
        return false;
    }

    insn->rs1 = NULL;
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
    if (eval_const_unary(insn))
        return true;
    return false;
}

/* Check if a basic block is unreachable */
bool is_block_unreachable(basic_block_t *bb)
{
    if (!bb)
        return true;

    /* Entry block is always reachable */
    if (!bb->idom && bb->belong_to && bb == bb->belong_to->bbs)
        return false;

    /* If block has no immediate dominator and it is not the entry block, it is
     * unreachable
     */
    if (!bb->idom)
        return true;

    /* If block was never visited during dominator tree construction, it is
     * unreachable
     */
    if (!bb->visited)
        return true;

    return false;
}

/* Check if a variable escapes (is used outside the function) */
bool var_escapes(var_t *var)
{
    if (!var)
        return true; /* conservative: assume it escapes */

    /* Global variables always escape */
    if (var->is_global)
        return true;

    /* Function definitions escape */
    if (var->is_func)
        return true;

    /* Conservative approach - assume all variables escape to avoid issues */
    /* This ensures we don't eliminate stores that might be needed */
    return true;
}

/* initial mark useful instruction */
int dce_init_mark(insn_t *insn, insn_t *work_list[], int work_list_idx)
{
    int mark_num = 0;
    /* mark instruction "useful" if it sets a return value, affects the value in
     * a storage location, or it is a function call.
     */
    switch (insn->opcode) {
    case OP_return:
        insn->useful = true;
        insn->belong_to->useful = true;
        work_list[work_list_idx + mark_num] = insn;
        mark_num++;
        break;
    case OP_write:
    case OP_store:
        /* Only mark stores/writes to escaping variables as useful */
        if (!insn->rd || var_escapes(insn->rd)) {
            insn->useful = true;
            insn->belong_to->useful = true;
            work_list[work_list_idx + mark_num] = insn;
            mark_num++;
        }
        break;
    case OP_global_store:
        /* Global stores always escape */
        insn->useful = true;
        insn->belong_to->useful = true;
        work_list[work_list_idx + mark_num] = insn;
        mark_num++;
        break;
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
            work_list[work_list_idx + mark_num] = insn->next;
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
    insn_t *work_list[DCE_WORKLIST_SIZE];
    int work_list_idx = 0;

    /* initially analyze current bb */
    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
        int mark_num = dce_init_mark(insn, work_list, work_list_idx);
        work_list_idx += mark_num;
        if (work_list_idx > DCE_WORKLIST_SIZE - 1)
            fatal("DCE worklist size exceeded");
    }

    /* Process worklist - marking dependencies as useful */
    while (work_list_idx != 0) {
        insn_t *curr = work_list[--work_list_idx];

        /* Skip if already processed to avoid redundant work */
        if (!curr)
            continue;

        /* Mark instruction as useful and add to worklist */
        insn_t *dep_insn = NULL;

        /* trace back where rs1 is assigned */
        if (curr->rs1 && curr->rs1->last_assign) {
            dep_insn = curr->rs1->last_assign;
            if (!dep_insn->useful) {
                dep_insn->useful = true;
                dep_insn->belong_to->useful = true;
                if (work_list_idx < DCE_WORKLIST_SIZE - 1)
                    work_list[work_list_idx++] = dep_insn;
                else
                    fatal("DCE worklist overflow");
            }
        }

        /* trace back where rs2 is assigned */
        if (curr->rs2 && curr->rs2->last_assign) {
            dep_insn = curr->rs2->last_assign;
            if (!dep_insn->useful) {
                dep_insn->useful = true;
                dep_insn->belong_to->useful = true;
                if (work_list_idx < DCE_WORKLIST_SIZE - 1)
                    work_list[work_list_idx++] = dep_insn;
                else
                    fatal("DCE worklist overflow");
            }
        }

        /* For phi nodes, mark all operands as useful */
        if (curr->opcode == OP_phi && curr->useful) {
            for (phi_operand_t *phi_op = curr->phi_ops; phi_op;
                 phi_op = phi_op->next) {
                if (phi_op->var && phi_op->var->last_assign &&
                    !phi_op->var->last_assign->useful) {
                    phi_op->var->last_assign->useful = true;
                    phi_op->var->last_assign->belong_to->useful = true;
                    if (work_list_idx < DCE_WORKLIST_SIZE - 1)
                        work_list[work_list_idx++] = phi_op->var->last_assign;
                    else
                        fatal("DCE worklist overflow");
                }
            }
        }

        basic_block_t *rdf;
        for (int i = 0; i < curr->belong_to->rdf_idx; i++) {
            rdf = curr->belong_to->RDF[i];
            if (!rdf)
                break;
            insn_t *tail = rdf->insn_list.tail;
            if (tail && tail->opcode == OP_branch && !tail->useful) {
                tail->useful = true;
                rdf->useful = true;
                work_list[work_list_idx++] = tail;
                if (work_list_idx > DCE_WORKLIST_SIZE - 1)
                    fatal("DCE worklist overflow");
            }
        }
    }
}

void dce_sweep(void)
{
    int total_eliminated = 0; /* Track effectiveness */

    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            /* Skip unreachable blocks entirely */
            if (is_block_unreachable(bb)) {
                /* Count instructions being eliminated */
                for (insn_t *insn = bb->insn_list.head; insn;
                     insn = insn->next) {
                    if (!insn->useful)
                        total_eliminated++;
                    insn->useful = false;
                }
                /* Mark entire block as dead */
                bb->useful = false;
                continue;
            }

            insn_t *insn = bb->insn_list.head;
            while (insn) {
                insn_t *next = insn->next;
                if (!insn->useful) {
                    total_eliminated++;
                    /* If a branch instruction is useless, redirect to the
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
                insn = next;
            }
        }
    }
}

void build_reversed_rpo();

void optimize(void)
{
    /* build rdf information for DCE */
    build_reversed_rpo();
    build_r_idom();
    build_rdom();
    build_rdf();

    use_chain_build();

    /* Run SCCP optimization multiple times for full propagation */
    bool sccp_changed = true;
    int sccp_iterations = 0;
    while (sccp_changed && sccp_iterations < 5) {
        sccp_changed = false;
        for (func_t *func = FUNC_LIST.head; func; func = func->next) {
            /* Skip function declarations without bodies */
            if (!func->bbs)
                continue;

            if (simple_sccp(func))
                sccp_changed = true;
        }
        sccp_iterations++;
    }

    /* Run constant cast optimization for truncation */
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        optimize_constant_casts(func);
    }

    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        /* basic block level (control flow) optimizations */

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            /* instruction level optimizations */
            for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
                /* record the instruction assigned value to rd */
                if (insn->rd)
                    insn->rd->last_assign = insn;

                /* Apply optimizations in order */
                if (const_folding(insn)) /* First: fold constants */
                    continue;
                if (cse(insn, bb)) /* Then: eliminate common subexpressions */
                    continue;

                /* Eliminate redundant assignments: x = x */
                if (insn->opcode == OP_assign && insn->rd && insn->rs1 &&
                    insn->rd == insn->rs1) {
                    /* Convert to no-op that DCE will remove */
                    insn->rd = NULL;
                    insn->rs1 = NULL;
                    continue;
                }

                /* Improved dead store elimination */
                if (insn->opcode == OP_store || insn->opcode == OP_write ||
                    insn->opcode == OP_global_store) {
                    if (insn->rd && !insn->rd->is_global) {
                        /* Look for overwrites within a small window */
                        insn_t *check = insn->next;
                        int distance = 0;
                        bool found_overwrite = false;

                        while (check && distance < OVERWRITE_WINDOW) {
                            /* Stop at control flow changes */
                            if (check->opcode == OP_branch ||
                                check->opcode == OP_jump ||
                                check->opcode == OP_call ||
                                check->opcode == OP_return) {
                                break;
                            }

                            /* Check if there's a use of the stored location */
                            if ((check->opcode == OP_load ||
                                 check->opcode == OP_read) &&
                                check->rs1 == insn->rd) {
                                break; /* Store is needed */
                            }

                            /* Found overwrite */
                            if ((check->opcode == OP_store ||
                                 check->opcode == OP_write ||
                                 check->opcode == OP_global_store) &&
                                check->rd == insn->rd) {
                                found_overwrite = true;
                                break;
                            }

                            check = check->next;
                            distance++;
                        }

                        if (found_overwrite) {
                            /* Mark for removal by DCE */
                            insn->useful = false;
                        }
                    }
                }

                /* Safety guards for division and modulo optimizations */
                if (insn->rs1 && insn->rs2 && insn->rs1 == insn->rs2) {
                    /* x / x = 1 (with zero-check guard) */
                    if (insn->opcode == OP_div && insn->rd) {
                        /* Only optimize if we can prove x is non-zero */
                        bool is_safe = false;
                        if (insn->rs1->is_const && insn->rs1->init_val != 0) {
                            is_safe = true;
                        }

                        if (is_safe) {
                            insn->opcode = OP_load_constant;
                            insn->rd->is_const = true;
                            insn->rd->init_val = 1;
                            insn->rs1 = NULL;
                            insn->rs2 = NULL;
                        }
                    }
                    /* x % x = 0 (with zero-check guard) */
                    else if (insn->opcode == OP_mod && insn->rd) {
                        /* Only optimize if we can prove x is non-zero */
                        bool is_safe = false;
                        if (insn->rs1->is_const && insn->rs1->init_val != 0) {
                            is_safe = true;
                        }

                        if (is_safe) {
                            insn->opcode = OP_load_constant;
                            insn->rd->is_const = true;
                            insn->rd->init_val = 0;
                            insn->rs1 = NULL;
                            insn->rs2 = NULL;
                        }
                    }
                }

                /* Enhanced algebraic simplifications */
                /* Self-operation optimizations */
                if (insn->rs1 && insn->rs2 && insn->rs1 == insn->rs2) {
                    /* x - x = 0 */
                    if (insn->opcode == OP_sub && insn->rd) {
                        insn->opcode = OP_load_constant;
                        insn->rd->is_const = true;
                        insn->rd->init_val = 0;
                        insn->rs1 = NULL;
                        insn->rs2 = NULL;
                    }
                    /* x ^ x = 0 */
                    else if (insn->opcode == OP_bit_xor && insn->rd) {
                        insn->opcode = OP_load_constant;
                        insn->rd->is_const = true;
                        insn->rd->init_val = 0;
                        insn->rs1 = NULL;
                        insn->rs2 = NULL;
                    }
                    /* x & x = x */
                    else if (insn->opcode == OP_bit_and && insn->rd) {
                        insn->opcode = OP_assign;
                        insn->rs2 = NULL;
                    }
                    /* x | x = x */
                    else if (insn->opcode == OP_bit_or && insn->rd) {
                        insn->opcode = OP_assign;
                        insn->rs2 = NULL;
                    }
                    /* x == x = 1 */
                    else if (insn->opcode == OP_eq && insn->rd) {
                        insn->opcode = OP_load_constant;
                        insn->rd->is_const = true;
                        insn->rd->init_val = 1;
                        insn->rs1 = NULL;
                        insn->rs2 = NULL;
                    }
                    /* x != x = 0 */
                    else if (insn->opcode == OP_neq && insn->rd) {
                        insn->opcode = OP_load_constant;
                        insn->rd->is_const = true;
                        insn->rd->init_val = 0;
                        insn->rs1 = NULL;
                        insn->rs2 = NULL;
                    }
                    /* x < x = 0, x > x = 0 */
                    else if ((insn->opcode == OP_lt || insn->opcode == OP_gt) &&
                             insn->rd) {
                        insn->opcode = OP_load_constant;
                        insn->rd->is_const = true;
                        insn->rd->init_val = 0;
                        insn->rs1 = NULL;
                        insn->rs2 = NULL;
                    }
                    /* x <= x = 1, x >= x = 1 */
                    else if ((insn->opcode == OP_leq ||
                              insn->opcode == OP_geq) &&
                             insn->rd) {
                        insn->opcode = OP_load_constant;
                        insn->rd->is_const = true;
                        insn->rd->init_val = 1;
                        insn->rs1 = NULL;
                        insn->rs2 = NULL;
                    }
                }

                /* Identity and constant optimizations */
                if (insn->rs2 && insn->rs2->is_const && insn->rd) {
                    int val = insn->rs2->init_val;

                    /* x + 0 = x, x - 0 = x, x | 0 = x, x ^ 0 = x */
                    if (val == 0) {
                        if (insn->opcode == OP_add || insn->opcode == OP_sub ||
                            insn->opcode == OP_bit_or ||
                            insn->opcode == OP_bit_xor) {
                            insn->opcode = OP_assign;
                            insn->rs2 = NULL;
                        }
                        /* x * 0 = 0, x & 0 = 0 */
                        else if (insn->opcode == OP_mul ||
                                 insn->opcode == OP_bit_and) {
                            insn->opcode = OP_load_constant;
                            insn->rd->is_const = true;
                            insn->rd->init_val = 0;
                            insn->rs1 = NULL;
                            insn->rs2 = NULL;
                        }
                        /* x << 0 = x, x >> 0 = x */
                        else if (insn->opcode == OP_lshift ||
                                 insn->opcode == OP_rshift) {
                            insn->opcode = OP_assign;
                            insn->rs2 = NULL;
                        }
                    }
                    /* x * 1 = x, x / 1 = x */
                    else if (val == 1) {
                        if (insn->opcode == OP_mul || insn->opcode == OP_div) {
                            insn->opcode = OP_assign;
                            insn->rs2 = NULL;
                        }
                        /* x % 1 = 0 */
                        else if (insn->opcode == OP_mod) {
                            insn->opcode = OP_load_constant;
                            insn->rd->is_const = true;
                            insn->rd->init_val = 0;
                            insn->rs1 = NULL;
                            insn->rs2 = NULL;
                        }
                    }
                    /* x & -1 = x (all bits set) */
                    else if (val == -1) {
                        if (insn->opcode == OP_bit_and) {
                            insn->opcode = OP_assign;
                            insn->rs2 = NULL;
                        }
                        /* x | -1 = -1 */
                        else if (insn->opcode == OP_bit_or) {
                            insn->opcode = OP_load_constant;
                            insn->rd->is_const = true;
                            insn->rd->init_val = -1;
                            insn->rs1 = NULL;
                            insn->rs2 = NULL;
                        }
                        /* x * -1 = -x */
                        else if (insn->opcode == OP_mul) {
                            insn->opcode = OP_negate;
                            insn->rs2 = NULL;
                        }
                    }
                }

                /* Multi-instruction analysis and optimization */
                /* Store-to-load forwarding */
                if (insn->opcode == OP_load && insn->rs1 && insn->rd) {
                    insn_t *search = insn->prev;
                    int search_limit = 10; /* Look back up to 10 instructions */

                    while (search && search_limit > 0) {
                        /* Found a recent store to the same location */
                        if ((search->opcode == OP_store ||
                             search->opcode == OP_write ||
                             search->opcode == OP_global_store) &&
                            search->rd == insn->rs1 && search->rs1) {
                            /* Check for intervening calls or branches */
                            bool safe_to_forward = true;
                            insn_t *check = search->next;

                            while (check && check != insn) {
                                if (check->opcode == OP_call ||
                                    check->opcode == OP_indirect ||
                                    check->opcode == OP_branch ||
                                    check->opcode == OP_jump) {
                                    safe_to_forward = false;
                                    break;
                                }
                                check = check->next;
                            }

                            if (safe_to_forward) {
                                /* Forward the stored value */
                                insn->opcode = OP_assign;
                                insn->rs1 = search->rs1;
                                insn->rs2 = NULL;
                                break;
                            }
                        }

                        /* Stop at control flow changes */
                        if (search->opcode == OP_call ||
                            search->opcode == OP_branch ||
                            search->opcode == OP_jump ||
                            search->opcode == OP_indirect) {
                            break;
                        }

                        search = search->prev;
                        search_limit--;
                    }
                }

                /* Redundant load elimination */
                if (insn->opcode == OP_load && insn->rs1 && insn->rd) {
                    insn_t *search = bb->insn_list.head;

                    while (search && search != insn) {
                        /* Found an earlier load from the same location */
                        if (search->opcode == OP_load &&
                            search->rs1 == insn->rs1 && search->rd) {
                            /* Check if location wasn't modified between loads
                             */
                            bool safe_to_reuse = true;
                            insn_t *check = search->next;

                            while (check && check != insn) {
                                /* Check for stores to the same location */
                                if ((check->opcode == OP_store ||
                                     check->opcode == OP_global_store ||
                                     check->opcode == OP_write) &&
                                    check->rd == insn->rs1) {
                                    safe_to_reuse = false;
                                    break;
                                }
                                /* Function calls might modify memory */
                                if (check->opcode == OP_call ||
                                    check->opcode == OP_indirect) {
                                    safe_to_reuse = false;
                                    break;
                                }
                                check = check->next;
                            }

                            if (safe_to_reuse) {
                                /* Replace with assignment from previous load */
                                insn->opcode = OP_assign;
                                insn->rs1 = search->rd;
                                insn->rs2 = NULL;
                                break;
                            }
                        }
                        search = search->next;
                    }
                }

                /* Strength reduction for power-of-2 operations */
                if (insn->rs2 && insn->rs2->is_const && insn->rd) {
                    int val = insn->rs2->init_val;

                    /* Check if value is power of 2 */
                    if (val > 0 && (val & (val - 1)) == 0) {
                        /* Count trailing zeros to get shift amount */
                        int shift = 0;
                        int temp = val;
                        while ((temp & 1) == 0) {
                            temp >>= 1;
                            shift++;
                        }

                        /* x * power_of_2 = x << shift */
                        if (insn->opcode == OP_mul) {
                            insn->opcode = OP_lshift;
                            insn->rs2->init_val = shift;
                        }
                        /* x / power_of_2 = x >> shift (unsigned) */
                        else if (insn->opcode == OP_div) {
                            insn->opcode = OP_rshift;
                            insn->rs2->init_val = shift;
                        }
                        /* x % power_of_2 = x & (power_of_2 - 1) */
                        else if (insn->opcode == OP_mod) {
                            insn->opcode = OP_bit_and;
                            insn->rs2->init_val = val - 1;
                        }
                    }
                }

                /* more optimizations */
            }
        }
    }

    /* Phi node optimization - eliminate trivial phi nodes */
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
                if (insn->opcode == OP_phi && insn->phi_ops) {
                    /* Count unique operands and check if all are the same */
                    var_t *first_var = insn->phi_ops->var;
                    bool all_same = true;
                    bool all_const = true;
                    int const_val = 0;
                    int num_ops = 0;

                    for (phi_operand_t *op = insn->phi_ops; op; op = op->next) {
                        num_ops++;
                        /* Check if all same variable */
                        if (op->var != first_var) {
                            all_same = false;
                        }
                        /* Check if all same constant */
                        if (op->var && op->var->is_const) {
                            if (op == insn->phi_ops) {
                                const_val = op->var->init_val;
                            } else if (op->var->init_val != const_val) {
                                all_const = false;
                            }
                        } else {
                            all_const = false;
                        }
                    }

                    /* Trivial phi: all operands are the same variable */
                    if (all_same && first_var && insn->rd) {
                        insn->opcode = OP_assign;
                        insn->rs1 = first_var;
                        insn->rs2 = NULL;
                        insn->phi_ops = NULL;
                    }
                    /* Constant phi: all operands have the same constant value
                     */
                    else if (all_const && num_ops > 0 && insn->rd) {
                        insn->opcode = OP_load_constant;
                        insn->rd->is_const = true;
                        insn->rd->init_val = const_val;
                        insn->rs1 = NULL;
                        insn->rs2 = NULL;
                        insn->phi_ops = NULL;
                    }
                }
            }
        }
    }

    /* Mark useful instructions */
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            dce_insn(bb);
        }
    }

    /* Eliminate dead instructions */
    dce_sweep();
}

void bb_index_reversed_rpo(func_t *func, basic_block_t *bb)
{
    bb->rpo_r = func->bb_cnt++;
}

void bb_reverse_reversed_index(func_t *func, basic_block_t *bb)
{
    bb->rpo_r = func->bb_cnt - bb->rpo_r;
}

void bb_build_reversed_rpo(func_t *func, basic_block_t *bb)
{
    if (func->exit == bb)
        return;

    basic_block_t *prev = func->exit;
    basic_block_t *curr = func->exit->rpo_r_next;
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

void build_reversed_rpo(void)
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        func->bb_cnt = 0;
        args->func = func;
        args->bb = func->exit;

        func->visited++;
        args->postorder_cb = bb_index_reversed_rpo;
        bb_backward_traversal(args);

        func->visited++;
        args->postorder_cb = bb_reverse_reversed_index;
        bb_backward_traversal(args);

        func->visited++;
        args->postorder_cb = bb_build_reversed_rpo;
        bb_backward_traversal(args);
    }
}

void bb_reset_live_kill_idx(func_t *func, basic_block_t *bb)
{
    UNUSED(func);
    bb->live_kill.size = 0;
}

void add_live_gen(basic_block_t *bb, var_t *var);
void update_consumed(insn_t *insn, var_t *var);

/* Combined function to reset and solve locals in one pass */
void bb_reset_and_solve_locals(func_t *func, basic_block_t *bb)
{
    UNUSED(func);

    /* Reset live_kill list */
    bb->live_kill.size = 0;

    /* Solve locals */
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
        if (insn->rd && insn->opcode != OP_unwound_phi)
            bb_add_killed_var(bb, insn->rd);
    }
}

void add_live_gen(basic_block_t *bb, var_t *var)
{
    if (var->is_global)
        return;

    var_list_add_var(&bb->live_gen, var);
}

void update_consumed(insn_t *insn, var_t *var)
{
    if (insn->idx > var->consumed)
        var->consumed = insn->idx;
}

void bb_solve_locals(func_t *func, basic_block_t *bb)
{
    UNUSED(func);

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
    var_list_add_var(&bb->live_in, var);
}

void compute_live_in(basic_block_t *bb)
{
    bb->live_in.size = 0;

    for (int i = 0; i < bb->live_out.size; i++) {
        var_t *var = bb->live_out.elements[i];
        if (var_check_killed(var, bb))
            continue;
        add_live_in(bb, var);
    }
    for (int i = 0; i < bb->live_gen.size; i++)
        add_live_in(bb, bb->live_gen.elements[i]);
}

int merge_live_in(var_t *live_out[], int live_out_idx, basic_block_t *bb)
{
    /* Early exit for empty live_in */
    if (bb->live_in.size == 0)
        return live_out_idx;

    /* Optimize for common case of small sets */
    if (live_out_idx < 16) {
        /* For small sets, simple linear search is fast enough */
        for (int i = 0; i < bb->live_in.size; i++) {
            bool found = false;
            var_t *var = bb->live_in.elements[i];
            for (int j = 0; j < live_out_idx; j++) {
                if (live_out[j] == var) {
                    found = true;
                    break;
                }
            }
            if (!found && live_out_idx < MAX_ANALYSIS_STACK_SIZE)
                live_out[live_out_idx++] = var;
        }
    } else {
        /* For larger sets, check bounds and use optimized loop */
        for (int i = 0; i < bb->live_in.size; i++) {
            bool found = false;
            var_t *var = bb->live_in.elements[i];
            /* Unroll inner loop for better performance */
            int j;
            for (j = 0; j + 3 < live_out_idx; j += 4) {
                if (live_out[j] == var || live_out[j + 1] == var ||
                    live_out[j + 2] == var || live_out[j + 3] == var) {
                    found = true;
                    break;
                }
            }
            /* Handle remaining elements */
            if (!found) {
                for (; j < live_out_idx; j++) {
                    if (live_out[j] == var) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found && live_out_idx < MAX_ANALYSIS_STACK_SIZE)
                live_out[live_out_idx++] = var;
        }
    }
    return live_out_idx;
}

bool recompute_live_out(basic_block_t *bb)
{
    var_t *live_out[MAX_ANALYSIS_STACK_SIZE];
    int live_out_idx = 0;

    /* Compute union of successor live_in sets */
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

    /* Quick check: if sizes differ, sets must be different */
    if (bb->live_out.size != live_out_idx) {
        var_list_assign_array(&bb->live_out, live_out, live_out_idx);
        return true;
    }

    /* Size is same, need to check if contents are identical */
    /* Optimize by checking if first few elements match (common case) */
    if (live_out_idx > 0) {
        /* Quick check first element */
        bool first_found = false;
        for (int j = 0; j < bb->live_out.size; j++) {
            if (live_out[0] == bb->live_out.elements[j]) {
                first_found = true;
                break;
            }
        }
        if (!first_found) {
            var_list_assign_array(&bb->live_out, live_out, live_out_idx);
            return true;
        }
    }

    /* Full comparison */
    for (int i = 0; i < live_out_idx; i++) {
        int same = 0;
        for (int j = 0; j < bb->live_out.size; j++) {
            if (live_out[i] == bb->live_out.elements[j]) {
                same = 1;
                break;
            }
        }
        if (!same) {
            var_list_assign_array(&bb->live_out, live_out, live_out_idx);
            return true;
        }
    }
    return false;
}

void liveness_analysis(void)
{
    bb_traversal_args_t *args = arena_alloc_traversal_args();
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        args->func = func;
        args->bb = func->bbs;

        /* Combined traversal: reset and solve locals in one pass */
        func->visited++;
        args->preorder_cb = bb_reset_and_solve_locals;
        bb_forward_traversal(args);

        /* Add function parameters as killed in entry block */
        for (int i = 0; i < func->num_params; i++)
            bb_add_killed_var(func->bbs, func->param_defs[i].subscripts[0]);
    }

    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        basic_block_t *bb = func->exit;
        bool changed;
        do {
            changed = false;
            for (bb = func->exit; bb; bb = bb->rpo_r_next)
                changed |= recompute_live_out(bb);
        } while (changed);
    }
}
