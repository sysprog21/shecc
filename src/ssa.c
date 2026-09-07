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

/* Constant cast optimization. Despite the file name this is not SCCP:
 * there is no lattice and no CFG-edge worklist anywhere in the tree.
 */
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

    /* arena_realloc() extends the block in place when this list was the last
     * thing allocated, which the liveness sets often are; allocating a fresh
     * array and copying abandoned the old one in the arena every time.
     */
    list->elements = arena_realloc(BB_ARENA, (char *) list->elements,
                                   list->capacity * HOST_PTR_SIZE,
                                   new_capacity * HOST_PTR_SIZE);
    list->capacity = new_capacity;
}

/* Whether @var appears in @list. */
bool var_list_holds(var_list_t *list, var_t *var)
{
    for (int i = 0; i < list->size; i++) {
        if (list->elements[i] == var)
            return true;
    }
    return false;
}

void var_list_add_var(var_list_t *list, var_t *var)
{
    if (var_list_holds(list, var))
        return;

    var_list_ensure_capacity(list, list->size + 1);
    list->elements[list->size++] = var;
}

/* Append without the membership scan var_list_add_var() performs. Callers that
 * use this must establish uniqueness themselves.
 */
void var_list_append(var_list_t *list, var_t *var)
{
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
/* The only thing a step of either traversal changes in the argument block is
 * the block it names, so a step sets that field and puts it back on the way
 * out. Copying the whole structure per edge instead -- which is what these did
 * -- costs a copy on every block of every traversal, and the traversals are
 * how nearly every analysis in the middle end walks a function.
 */
void bb_forward_traversal(bb_traversal_args_t *args)
{
    basic_block_t *bb = args->bb;
    func_t *func = args->func;

    bb->visited++;

    if (args->preorder_cb)
        args->preorder_cb(func, bb);

    if (bb->next) {
        if (bb->next->visited < func->visited) {
            args->bb = bb->next;
            bb_forward_traversal(args);
        }
    }
    if (bb->then_) {
        if (bb->then_->visited < func->visited) {
            args->bb = bb->then_;
            bb_forward_traversal(args);
        }
    }
    if (bb->else_) {
        if (bb->else_->visited < func->visited) {
            args->bb = bb->else_;
            bb_forward_traversal(args);
        }
    }

    args->bb = bb;

    if (args->postorder_cb)
        args->postorder_cb(func, bb);
}

/* cfront does not accept structure as an argument, pass pointer */
void bb_backward_traversal(bb_traversal_args_t *args)
{
    basic_block_t *bb = args->bb;
    func_t *func = args->func;

    bb->visited++;

    if (args->preorder_cb)
        args->preorder_cb(func, bb);

    for (int i = 0; i < bb->prev_idx; i++) {
        basic_block_t *pred = bb->prev[i].bb;

        if (!pred)
            continue;
        if (pred->visited < func->visited) {
            args->bb = pred;
            bb_backward_traversal(args);
        }
    }

    args->bb = bb;

    if (args->postorder_cb)
        args->postorder_cb(func, bb);
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
                basic_block_t *pred = NULL;
                for (int i = 0; i < bb->prev_idx; i++) {
                    if (!bb->prev[i].bb)
                        continue;
                    if (!bb->prev[i].bb->idom)
                        continue;
                    pred = bb->prev[i].bb;
                    break;
                }
                /* Reverse postorder puts a predecessor of every reachable
                 * block ahead of it, so one is normally settled by now. A
                 * block where none is cannot be given an immediate dominator
                 * yet; leaving it for a later round is what keeps the walk off
                 * an uninitialised pointer.
                 */
                if (!pred)
                    continue;

                for (int i = 0; i < bb->prev_idx; i++) {
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

    for (int i = 0; i < pred->dom_next_idx; i++) {
        if (pred->dom_next[i] == succ)
            return false;
    }

    if (pred->dom_next_idx >= pred->dom_next_cap)
        pred->dom_next =
            arena_grow(BB_ARENA, (char *) pred->dom_next, &pred->dom_next_cap,
                       sizeof(basic_block_t *), 4, MAX_BB_DOM_SUCC,
                       "Too many children in dominator tree");

    pred->dom_next[pred->dom_next_idx++] = succ;
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

/* Recompute the dominator tree over the CFG as it now stands.
 *
 * if_convert() only removes edges, and losing one can only strengthen
 * dominance, so a stale tree stays conservative there. thread_const_branch()
 * adds "pred -> target", a path that bypasses the join: a join that dominated
 * the target no longer does. strength_reduce() and mark_loop_depth() both ask
 * is_dominate() which edges close a loop, and against a stale tree a forward
 * edge reads as a back edge -- mark_natural_loop() then walks predecessors out
 * of the region it was meant to stay inside, since it has no stop at the
 * header and relies on the header dominating the latch.
 *
 * The order has to be rebuilt along with the tree. Both passes keep the
 * rpo_next chain consistent -- a block they drop comes out of it -- but
 * dropping blocks and adding edges does not preserve reverse postorder, and
 * build_idom() depends on it: it reaches each block once a predecessor already
 * has an idom, and out of order it can meet one where none does.
 */
void rebuild_dom(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        if (!func->bbs)
            continue;

        /* dom_connect() refuses a block that already has a parent, and
         * bb_build_rpo() splices into the chain rather than replacing it, so
         * both have to be taken down before they can be built again. The next
         * link is read before it is cleared: clearing as the walk goes would
         * cut the chain out from under it.
         */
        basic_block_t *bb = func->bbs;

        while (bb) {
            basic_block_t *next = bb->rpo_next;

            bb->idom = NULL;
            bb->dom_prev = NULL;
            bb->dom_next_idx = 0;
            bb->rpo_next = NULL;
            bb = next;
        }
        func->bb_cnt = 0;
    }

    build_rpo();
    build_idom();
    build_dom();
}

void bb_build_df(func_t *func, basic_block_t *bb)
{
    UNUSED(func);

    int cnt = 0;
    for (int i = 0; i < bb->prev_idx; i++) {
        if (bb->prev[i].bb)
            cnt++;
    }
    if (cnt <= 0)
        return;

    for (int i = 0; i < bb->prev_idx; i++) {
        if (bb->prev[i].bb) {
            /* Walk up from the predecessor to this block's immediate
             * dominator. The walk normally stops there, since a block's
             * immediate dominator dominates all of its predecessors -- but an
             * edge the dominator tree does not account for runs off the top
             * instead, so stop at the root as well. Ending early only widens
             * the frontier, which costs a phi that turns out to be trivial.
             */
            for (basic_block_t *curr = bb->prev[i].bb; curr && curr != bb->idom;
                 curr = curr->idom)
                bb_add_df(curr, bb);
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
    /* A block is attached to the reverse-dominator tree at most once, so the
     * rdom_prev guard above already rules out a duplicate edge.
     */
    if (succ->rdom_prev)
        return false;

    /* rdom_count only ever guarded a MAX_BB_RDOM_SUCC ceiling on an array of
     * reverse-dominator children that no longer exists -- nothing reads the
     * count, and nothing walks those children -- so the field is gone.
     */
    succ->rdom_prev = pred;
    return true;
}

void bb_build_rdom(func_t *func, basic_block_t *bb)
{
    if (!func->bbs)
        return;
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

    /* As in bb_build_df(), the walk up the post-dominator tree stops at this
     * block's immediate post-dominator, and at the root when an edge the tree
     * does not account for takes it past that.
     */
    if (bb->next) {
        for (basic_block_t *curr = bb->next; curr && curr != bb->r_idom;
             curr = curr->r_idom)
            bb_add_rdf(curr, bb);
    }
    if (bb->else_) {
        for (basic_block_t *curr = bb->else_; curr && curr != bb->r_idom;
             curr = curr->r_idom)
            bb_add_rdf(curr, bb);
    }
    if (bb->then_) {
        for (basic_block_t *curr = bb->then_; curr && curr != bb->r_idom;
             curr = curr->r_idom)
            bb_add_rdf(curr, bb);
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
        /* Only the first 'size' entries hold a variable; the rest of the
         * allocation was never written. Reading them compares against whatever
         * the allocator left there, and a stray match puts a variable in scope
         * that is not, which changes where phis are inserted and so what code
         * comes out -- differently from one build to the next.
         */
        for (int i = 0; i < block->locals.size; i++) {
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

/* A new variable holding @val, for use as an instruction operand.
 *
 * It has to be new. rename_var() gives every use reached by one definition the
 * same var_t, and mark_const() stamps init_val onto that shared object, so
 * rewriting an existing constant's value changes what every other use sees.
 * The caller emits the OP_load_constant that defines it.
 */
var_t *new_const_var(block_t *scope, int val)
{
    var_t *var = require_var(scope);

    var->var_name = gen_name();
    var->is_const = true;
    var->init_val = val;
    return var;
}
bool is_dominate(basic_block_t *pred, basic_block_t *succ);

/* The renaming state of @v, created on first use. */
rename_t *var_rename(var_t *v)
{
    if (!v->rename)
        v->rename = arena_calloc(BLOCK_ARENA, 1, sizeof(rename_t));
    return v->rename;
}

/* Push a fresh subscript onto @base's renaming stack, growing it as needed. */
void rename_stack_push(var_t *base, int sub)
{
    rename_t *r = var_rename(base);
    if (r->stack_idx >= r->stack_cap)
        r->stack = arena_grow(BLOCK_ARENA, (char *) r->stack, &r->stack_cap,
                              sizeof(int), 8, MAX_RENAME_STACK,
                              "Too many nested definitions of a variable");
    r->stack[r->stack_idx++] = sub;
}

void new_name(block_t *block, var_t **var)
{
    var_t *v = *var;
    if (!v->base)
        v->base = v;
    if (v->is_global)
        return;

    rename_t *r = var_rename(v->base);
    int i = r->counter++;
    rename_stack_push(v->base, i);
    var_t *vd = require_var(block);
    memcpy(vd, *var, sizeof(var_t));
    var_reset_subscripts(vd); /* the copy shares nothing with its base */
    vd->base = *var;
    vd->subscript = i;
    var_add_subscript(v, vd);
    var[0] = vd;
}

var_t *get_stack_top_subscript_var(var_t *var)
{
    rename_t *r = var_rename(var->base);
    if (r->stack_idx < 1)
        return var; /* fallback: use base when no prior definition */

    int sub = r->stack[r->stack_idx - 1];
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
    /* Pop unconditionally, creating the state if the variable has none: the
     * inline rename_t this replaced was always present, so a pop with nothing
     * pushed drove stack_idx to -1, and the next push then landed one slot
     * below the stack. Preserve that exactly.
     */
    rename_t *r = var_rename(var->base);
    r->stack_idx--;
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

    for (int i = 0; i < bb->dom_next_idx; i++)
        bb_solve_phi_params(bb->dom_next[i]);

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
            var_reset_subscripts(var); /* the copy shares nothing with base */
            var->base = base;
            var->subscript = 0;

            rename_t *r = var_rename(base);
            rename_stack_push(base, r->counter++);
            var_add_subscript(base, var);
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

int loop_scan_gen;

/* Raise the depth of every block in the natural loop that the edge from
 * @latch back to @header closes: the header, and everything that can reach the
 * latch without leaving the loop.
 */
bool mark_natural_loop(basic_block_t *header, basic_block_t *latch)
{
    basic_block_t *stack[MAX_LOOP_WALK];
    int sp = 0;

    loop_scan_gen++;
    header->loop_mark = loop_scan_gen;
    header->loop_depth++;

    if (latch != header) {
        latch->loop_mark = loop_scan_gen;
        latch->loop_depth++;
        stack[sp++] = latch;
    }

    while (sp) {
        basic_block_t *bb = stack[--sp];

        for (int i = 0; i < bb->prev_idx; i++) {
            basic_block_t *p = bb->prev[i].bb;

            if (!p || p->loop_mark == loop_scan_gen)
                continue;
            /* Out of room to widen. Marking this block and then not walking
             * through it would leave the rest of the loop unmarked while
             * looking marked, and a reader of loop_mark would take blocks
             * inside the loop for blocks outside it. Report the whole answer
             * as unusable instead.
             */
            if (sp >= MAX_LOOP_WALK)
                return false;
            p->loop_mark = loop_scan_gen;
            p->loop_depth++;
            stack[sp++] = p;
        }
    }
    return true;
}

/* Count the loops enclosing each block of @func.
 *
 * A loop is closed by an edge to a block that dominates its source. Position
 * along the rpo_next chain cannot stand in for that: the passes that splice
 * blocks out of the chain leave an if's arms sitting after the block they
 * rejoin, and every one of those edges would read as a loop.
 */
void mark_loop_depth(func_t *func)
{
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        bb->loop_depth = 0;
        bb->loop_mark = 0;
    }


    for (basic_block_t *p = func->bbs; p; p = p->rpo_next) {
        basic_block_t *succ[3];

        succ[0] = p->next;
        succ[1] = p->then_;
        succ[2] = p->else_;

        for (int k = 0; k < 3; k++) {
            if (!succ[k])
                continue;
            if (succ[k] != p && !is_dominate(succ[k], p))
                continue;
            /* A walk that ran out of room stopped partway, leaving the depths
             * it had already raised standing over a region it never finished
             * measuring. Those feed the weights pin_registers() compares, so a
             * half-counted loop would outbid a whole one; drop the tally for
             * the function instead and let every block weigh the same.
             */
            if (!mark_natural_loop(succ[k], p)) {
                for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
                    bb->loop_depth = 0;
                    bb->loop_weight = 1;
                }
                return;
            }
        }
    }

    /* What one naming in each block is worth, so that the tally in
     * pin_registers() reads it rather than deriving it per operand.
     */
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        int w = 1;

        for (int d = 0; d < bb->loop_depth && d < MAX_WEIGHTED_LOOP_DEPTH; d++)
            w = w * LOOP_USE_WEIGHT;
        bb->loop_weight = w;
    }
}

/* Put @insn into @bb straight after @after, or at the head when it is NULL. */
void bb_insert_after(basic_block_t *bb, insn_t *after, insn_t *insn)
{
    insn->belong_to = bb;
    insn->prev = after;
    if (after) {
        insn->next = after->next;
        after->next = insn;
    } else {
        insn->next = bb->insn_list.head;
        bb->insn_list.head = insn;
    }
    if (insn->next)
        insn->next->prev = insn;
    else
        bb->insn_list.tail = insn;
}

/* Append @insn to @bb, detaching it from wherever it was. */
void bb_append_insn(basic_block_t *bb, insn_t *insn)
{
    bb_insert_after(bb, bb->insn_list.tail, insn);
}

/* A fresh instruction naming @rd, @rs1 and @rs2, not yet in any block. The
 * allocation is zeroed, so every other field starts empty -- which is what the
 * passes that build instructions want, and one less thing for each of them to
 * remember.
 */
insn_t *new_insn(opcode_t op, var_t *rd, var_t *rs1, var_t *rs2)
{
    insn_t *insn = arena_calloc(INSN_ARENA, 1, sizeof(insn_t));

    insn->opcode = op;
    insn->rd = rd;
    insn->rs1 = rs1;
    insn->rs2 = rs2;
    return insn;
}

/* Whether @insn computes a value with no side effect and no way to fault, so
 * running it on a path that would not have reached it changes nothing.
 */
bool insn_is_speculatable(insn_t *insn)
{
    switch (insn->opcode) {
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_negate:
    case OP_bit_not:
    case OP_log_not:
    case OP_eq:
    case OP_neq:
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
    case OP_assign:
    case OP_load_constant:
        return true;
    default:
        /* Division can trap, a read can fault, a write or a call is a side
         * effect, and anything else is not worth reasoning about here.
         */
        return false;
    }
}

/* Walk the arm starting at @arm, gathering the blocks it runs before control
 * rejoins.
 *
 * An arm is not one block: the copy carrying its value to the join is appended
 * to whichever block immediately precedes the join, which is separate from the
 * one holding the arm's computation. The walk follows single-entry,
 * single-exit blocks and stops at the first block something else can also
 * reach, which is the join.
 */
basic_block_t *if_arm_chain(basic_block_t *arm,
                            basic_block_t **chain,
                            int *len,
                            int *count)
{
    basic_block_t *bb = arm;

    *len = 0;
    *count = 0;

    while (bb) {
        if (bb->then_ || bb->else_ || !bb->next)
            return NULL;
        if (bb_pred_count(bb) != 1)
            return NULL;
        if (*len >= MAX_IF_ARM_BLOCKS)
            return NULL;

        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            if (insn->opcode == OP_unwound_phi)
                continue;
            if (!insn_is_speculatable(insn))
                return NULL;
            /* Writing a global or an address-taken variable is a store, and a
             * store is a side effect however plain the arithmetic producing it
             * looks.
             */
            if (insn->rd && (insn->rd->is_global || insn->rd->address_taken))
                return NULL;
            *count = *count + 1;
        }

        chain[*len] = bb;
        *len = *len + 1;

        if (bb_pred_count(bb->next) != 1)
            return bb->next;
        bb = bb->next;
    }
    return NULL;
}

/* The single copy an arm hands to the join, or NULL when it hands over more or
 * fewer than one.
 */
insn_t *if_chain_phi(basic_block_t **chain, int len)
{
    insn_t *phi = NULL;

    for (int i = 0; i < len; i++) {
        for (insn_t *insn = chain[i]->insn_list.head; insn; insn = insn->next) {
            if (insn->opcode != OP_unwound_phi)
                continue;
            if (phi)
                return NULL;
            phi = insn;
        }
    }
    return phi;
}

/* Whether any instruction in @b reads a value defined in @a. */
bool if_arm_reads_other(basic_block_t **a,
                        int alen,
                        basic_block_t **b,
                        int blen)
{
    for (int i = 0; i < alen; i++) {
        for (insn_t *def = a[i]->insn_list.head; def; def = def->next) {
            if (!def->rd)
                continue;
            for (int j = 0; j < blen; j++) {
                for (insn_t *use = b[j]->insn_list.head; use; use = use->next) {
                    if (use->rs1 == def->rd || use->rs2 == def->rd ||
                        use->rs3 == def->rd)
                        return true;
                }
            }
        }
    }
    return false;
}

/* Flatten "if (c) x = A; else x = B;" into both computations and a select.
 *
 * A branch the hardware cannot predict costs far more than the arm it skips:
 * the CRC loop in the benchmark suite mispredicts 17% of 283 million branches
 * and pays about a thousand million cycles for it, most of its run time.
 * Running both arms and selecting between the results has nothing to
 * mispredict, and neither arm may write memory, call anything or divide, so
 * running the one that would have been skipped costs only itself.
 */
bool if_convert_bb(func_t *func, basic_block_t *bb)
{
    basic_block_t *t = bb->then_;
    basic_block_t *e = bb->else_;
    basic_block_t *t_chain[MAX_IF_ARM_BLOCKS];
    basic_block_t *e_chain[MAX_IF_ARM_BLOCKS];
    int t_len = 0;
    int e_len = 0;
    int tn = 0;
    int en = 0;

    if (!t || !e || t == e)
        return false;

    basic_block_t *join = if_arm_chain(t, t_chain, &t_len, &tn);
    basic_block_t *e_join = if_arm_chain(e, e_chain, &e_len, &en);

    if (!join || join != e_join)
        return false;
    /* Flattening removes both arms, so the join must be reached from them and
     * nothing else.
     */
    if (bb_pred_count(join) != 2)
        return false;

    insn_t *t_phi = if_chain_phi(t_chain, t_len);
    insn_t *e_phi = if_chain_phi(e_chain, e_len);
    if (!t_phi || !e_phi || t_phi->rd != e_phi->rd)
        return false;
    if (t_phi->rd->is_global || t_phi->rd->address_taken)
        return false;
    /* Both arms handing over the same value makes the select pointless. */
    if (t_phi->rs1 == e_phi->rs1)
        return false;
    if (tn + en > MAX_SPECULATED_INSNS)
        return false;

    insn_t *br = bb->insn_list.tail;
    if (!br || br->opcode != OP_branch || !br->rs1)
        return false;

    /* The arms run one after the other now, so whichever computes a value the
     * other reads has to go first -- common subexpression elimination leaves
     * exactly that. If each reads something the other defines, no order works.
     */
    bool e_needs_t = if_arm_reads_other(t_chain, t_len, e_chain, e_len);
    bool t_needs_e = if_arm_reads_other(e_chain, e_len, t_chain, t_len);

    if (e_needs_t && t_needs_e)
        return false;

    var_t *cond = br->rs1;

    bb->insn_list.tail = br->prev;
    if (br->prev)
        br->prev->next = NULL;
    else
        bb->insn_list.head = NULL;

    insn_t *next;

    for (int step = 0; step < 2; step++) {
        bool take_t = e_needs_t ? step == 0 : step == 1;
        basic_block_t **chain = take_t ? t_chain : e_chain;
        insn_t *skip = take_t ? t_phi : e_phi;
        int len = take_t ? t_len : e_len;

        for (int i = 0; i < len; i++) {
            for (insn_t *insn = chain[i]->insn_list.head; insn; insn = next) {
                next = insn->next;
                if (insn == skip)
                    continue;
                /* Whatever the arm computes is a version of the variable the
                 * select writes as often as not, and that variable's pinned
                 * register still has to carry the value the arms read.
                 */
                if (insn->rd)
                    insn->rd->in_select_arm = true;
                bb_append_insn(bb, insn);
            }
            chain[i]->insn_list.head = NULL;
            chain[i]->insn_list.tail = NULL;
        }
    }

    /* One instruction naming all three inputs: split into a copy and a
     * read-modify move, the allocator settles their registers separately and
     * need not agree on one.
     */
    t_phi->opcode = OP_cmov;
    t_phi->rs2 = cond;
    t_phi->rs3 = e_phi->rs1;
    bb_append_insn(bb, t_phi);

    /* Rewire: the test now falls straight into the join. */
    bb_disconnect(t_chain[t_len - 1], join);
    bb_disconnect(e_chain[e_len - 1], join);
    bb_disconnect(bb, t);
    bb_disconnect(bb, e);
    bb_connect(bb, join, NEXT);

    /* Take the arms out of the order the rest of the compiler walks. */
    for (basic_block_t *p = func->bbs; p; p = p->rpo_next) {
        bool drop = true;

        while (drop && p->rpo_next) {
            drop = false;
            for (int i = 0; i < t_len; i++) {
                if (p->rpo_next == t_chain[i])
                    drop = true;
            }
            for (int i = 0; i < e_len; i++) {
                if (p->rpo_next == e_chain[i])
                    drop = true;
            }
            if (drop)
                p->rpo_next = p->rpo_next->rpo_next;
        }
    }
    return true;
}

/* The generation stamping the loop strength_reduce() is looking at, so that
 * var_read_by() can be asked about that loop alone.
 */
int sr_gen;

/* Whether anything in @func reads @var, ignoring the @nskip instructions in
 * @skip.
 *
 * With @loop_only set, only the blocks the stamped loop covers are searched: a
 * reader outside it does not keep a literal alive inside, because the
 * allocator materialises one wherever the value is wanted.
 */
bool var_read_by(func_t *func,
                 var_t *var,
                 insn_t **skip,
                 int nskip,
                 bool loop_only)
{
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        if (loop_only && bb->loop_mark != sr_gen)
            continue;
        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            bool skipped = false;

            for (int i = 0; i < nskip; i++) {
                if (skip[i] == insn)
                    skipped = true;
            }
            if (skipped)
                continue;
            if (insn->rs1 == var || insn->rs2 == var || insn->rs3 == var)
                return true;
        }
    }
    return false;
}

/* Detach @insn from the block holding it. */
void bb_remove_insn(basic_block_t *bb, insn_t *insn)
{
    if (insn->prev)
        insn->prev->next = insn->next;
    else
        bb->insn_list.head = insn->next;
    if (insn->next)
        insn->next->prev = insn->prev;
    else
        bb->insn_list.tail = insn->prev;
    insn->prev = NULL;
    insn->next = NULL;
}

/* Send the arms of "if (a && b)" straight where they are going.
 *
 * A short-circuit condition becomes a block per arm, each writing 0 or 1 into
 * one temporary, and a join that does nothing but branch on it. The value is
 * born a constant and dies at that branch, so the arm already knows which way
 * it goes: writing the answer to a stack slot, reading it back and testing it
 * is five instructions to reach a label the arm could have jumped to. Every
 * loop guarded by "i < n && p[i]" pays them on each iteration.
 */
bool thread_const_branch(func_t *func, basic_block_t *join)
{
    insn_t *br = join->insn_list.head;

    if (!join->then_ || !join->else_)
        return false;
    /* Only a block that does nothing else: anything before the branch would
     * be skipped, and a phi copy left in it belongs to a successor.
     */
    if (!br || br->next || br->opcode != OP_branch || !br->rs1)
        return false;

    var_t *cond = br->rs1;
    bool threaded = false;

    /* Two passes: the first only looks, because the reader scan between them
     * walks the whole function and is not worth paying for a join no
     * predecessor can be threaded to anyway.
     */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < join->prev_idx; i++) {
            basic_block_t *pred = join->prev[i].bb;

            if (!pred || pred == join)
                continue;
            /* A predecessor that also goes somewhere else keeps a branch of
             * its own, and rewiring one of its edges would need that branch
             * rewritten.
             */
            if (pred->then_ || pred->else_ || pred->next != join)
                continue;

            insn_t *def = pred->insn_list.tail;

            if (!def || def->rd != cond)
                continue;
            if (def->opcode != OP_unwound_phi && def->opcode != OP_assign)
                continue;
            if (!def->rs1 || !def->rs1->is_const)
                continue;

            if (!pass) {
                threaded = true;
                break;
            }

            basic_block_t *target =
                def->rs1->init_val ? join->then_ : join->else_;

            bb_remove_insn(pred, def);
            bb_disconnect(pred, join);
            bb_connect(pred, target, NEXT);
        }

        if (!threaded)
            return false;
        if (!pass) {
            /* The condition must die here: threading removes the writes, so
             * anything else reading it would read nothing.
             */
            insn_t *skip[1];

            skip[0] = br;
            if (var_read_by(func, cond, skip, 1, false))
                return false;
        }
    }

    /* Nothing reaches the join any more, so take it out of the order the rest
     * of the compiler walks -- and with it the branch, whose condition no
     * longer has a definition on any path.
     */
    if (!bb_pred_count(join)) {
        bb_disconnect(join, join->then_);
        bb_disconnect(join, join->else_);
        for (basic_block_t *p = func->bbs; p && p->rpo_next; p = p->rpo_next) {
            if (p->rpo_next == join) {
                p->rpo_next = join->rpo_next;
                break;
            }
        }
    }
    return true;
}

void thread_const_branches(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        if (!func->bbs)
            continue;
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            insn_t *br = bb->insn_list.head;

            if (!br || br->next || br->opcode != OP_branch || !br->rs1)
                continue;
            thread_const_branch(func, bb);
        }
    }
}

/* Copy a small function into its callers.
 *
 * A call to a four-line helper costs more than the helper: the arguments go
 * into the argument registers, everything live crosses a call boundary and so
 * goes to the frame, and the callee builds and tears down a frame of its own.
 * The benchmark suite's "calls" case spends three instructions on that for
 * every one it spends computing. Copying the body in removes all of it and
 * lets the optimizer see the caller and the callee together.
 *
 * Only a body with no control flow of its own is copied, which keeps the
 * transformation to splicing one instruction list into another.
 */
var_t *inline_from[MAX_INLINE_VARS];
var_t *inline_to[MAX_INLINE_VARS];
int inline_map_n;

/* The caller's stand-in for the callee's @var: the argument for a parameter,
 * a fresh variable for anything the body computes, and the variable itself
 * for anything shared.
 */
var_t *inline_lookup(var_t *var, block_t *scope)
{
    /* A global is shared, not copied. */
    if (!var || var->is_global)
        return var;

    for (int i = 0; i < inline_map_n; i++) {
        if (inline_from[i] == var)
            return inline_to[i];
        /* Parameters are mapped by the variable they are versions of, so that
         * a body naming a later version still finds the argument.
         */
        if (inline_from[i] && inline_from[i] == var->base)
            return inline_to[i];
    }

    if (inline_map_n >= MAX_INLINE_VARS)
        return NULL;

    var_t *copy = require_var(scope);

    copy->var_name = gen_name();
    copy->type = var->type;
    copy->ptr_level = var->ptr_level;
    copy->is_const = var->is_const;
    copy->init_val = var->init_val;
    inline_from[inline_map_n] = var;
    inline_to[inline_map_n] = copy;
    inline_map_n++;
    return copy;
}

/* Whether @func is small and straight-line enough to copy into its callers.
 *
 * A function is a chain of blocks even when its source has no control flow:
 * the entry block holds the declarations and falls into the body. Any block
 * that branches, or that something else can reach, ends the chain and makes
 * the function too complicated to splice into an instruction list.
 */
int inline_round;

/* Whether cloning @func's body would need more renaming entries than the map
 * holds.
 *
 * inline_lookup() returns NULL once it is full, and inline_call_at() has
 * already removed the push/call/retval sequence by the time cloning starts, so
 * a failure there cannot be declined -- it leaves instructions carrying null
 * operands behind, which the register allocator dereferences. Counting before
 * anything is touched is what keeps that from needing to be undone.
 *
 * The count mirrors inline_lookup()'s matching exactly: a global is shared
 * rather than copied, a parameter is found through the variable it is a
 * version of, and every other version gets an entry of its own.
 */
bool inline_map_fits(func_t *func)
{
    var_t *seen[MAX_INLINE_VARS];
    int n = 0;

    for (int q = 0; q < func->num_params; q++) {
        if (n >= MAX_INLINE_VARS)
            return false;
        seen[n++] = &func->param_defs[q];
    }

    for (basic_block_t *bb = func->bbs; bb; bb = bb->next) {
        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            var_t *ops[4];

            ops[0] = insn->rd;
            ops[1] = insn->rs1;
            ops[2] = insn->rs2;
            ops[3] = insn->rs3;

            for (int k = 0; k < 4; k++) {
                var_t *var = ops[k];

                if (!var || var->is_global)
                    continue;

                bool found = false;

                for (int i = 0; i < n; i++) {
                    if (seen[i] == var || seen[i] == var->base)
                        found = true;
                }
                if (found)
                    continue;
                if (n >= MAX_INLINE_VARS)
                    return false;
                seen[n++] = var;
            }
        }
    }
    return true;
}

bool func_is_inlinable(func_t *func)
{
    int n = 0, blocks = 0;
    insn_t *last = NULL;

    /* The answer only changes when a round changes the body. */
    if (func->inline_gen == inline_round)
        return func->inline_ok;
    func->inline_gen = inline_round;
    func->inline_ok = false;
    func->inline_ret = NULL;

    if (!func->bbs || func->va_args)
        return false;
    if (func->num_params > MAX_ARGS_IN_REG)
        return false;

    for (basic_block_t *bb = func->bbs; bb; bb = bb->next) {
        if (bb->then_ || bb->else_)
            return false;
        if (bb != func->bbs && bb_pred_count(bb) != 1)
            return false;
        /* The single-predecessor test above is skipped for the entry block, so
         * a "next" chain that came back round to it would walk for ever. A
         * chain longer than the instruction budget cannot be inlinable in any
         * case, so stopping there costs nothing and bounds the walk.
         */
        if (++blocks > MAX_INLINE_INSNS)
            return false;

        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            n++;
            if (n > MAX_INLINE_INSNS)
                return false;
            last = insn;
            if (insn->opcode == OP_return)
                continue;
            /* A call of its own would have to be copied as a call, which is
             * what the copying is meant to remove; the pass runs again once
             * that callee has been copied in, and by then this body
             * qualifies.
             */
            if (!insn_is_speculatable(insn))
                return false;
            if (insn->rd && (insn->rd->is_global || insn->rd->address_taken))
                return false;
            /* Writing a parameter would mean the copy assigns to the caller's
             * own variable, since a parameter maps onto the argument.
             */
            for (int q = 0; q < func->num_params; q++) {
                if (insn->rd && insn->rd->base == &func->param_defs[q])
                    return false;
            }
        }
    }

    if (!last || last->opcode != OP_return)
        return false;
    if (!inline_map_fits(func))
        return false;

    func->inline_ok = true;
    func->inline_ret = last;
    return true;
}

/* Copy @src into @bb after @after, renaming what it names. */
insn_t *inline_clone(basic_block_t *bb,
                     insn_t *after,
                     insn_t *src,
                     block_t *scope)
{
    insn_t *copy = new_insn(src->opcode, inline_lookup(src->rd, scope),
                            inline_lookup(src->rs1, scope),
                            inline_lookup(src->rs2, scope));

    copy->rs3 = inline_lookup(src->rs3, scope);
    copy->sz = src->sz;
    copy->str = src->str;
    bb_insert_after(bb, after, copy);
    return copy;
}

/* Replace the push/call/retval sequence ending at @call with the callee's
 * body. Returns the instruction to carry on scanning from, which is NULL when
 * the copy lands at the end of the block; *@done says whether the call was
 * replaced at all. The two have to be reported separately -- a NULL return
 * read as "left alone" stopped the round from being counted as progress and
 * left the rest of the block unscanned.
 *
 * @done is an int and not a bool on purpose. A _Bool is one byte, and writing
 * one byte through a pointer into the caller's slot -- which is pointer-sized
 * -- leaves the bytes above it holding whatever was there before, so the read
 * back can be true when false was written. Compiling shecc with itself for
 * RISC-V is where that shows: the copy of the compiler so built inlines less
 * than the one gcc built, and stage1 and stage2 stop matching. Widening the
 * flag sidesteps it; the underlying narrow-store bug is still there for any
 * other "bool *" to find.
 */
insn_t *inline_call_at(basic_block_t *bb, insn_t *call, int *done)
{
    func_t *callee = find_func(call->str);

    *done = 0;
    if (!callee || !func_is_inlinable(callee))
        return NULL;

    /* The arguments are the pushes immediately before the call. */
    var_t *args[MAX_PARAMS];
    insn_t *first = call;
    int argc = 0;

    for (insn_t *p = call->prev; p && p->opcode == OP_push; p = p->prev) {
        first = p;
        argc++;
    }
    if (argc != callee->num_params || argc > MAX_PARAMS)
        return NULL;

    /* A call nested in another call's argument list -- "f(g(x), y)" -- has
     * the outer call's pushes already standing before it. The register
     * allocator hands those the argument registers as it meets them, so
     * anything spliced in between would overwrite arguments the outer call is
     * still waiting to make.
     */
    for (insn_t *p = first->prev; p; p = p->prev) {
        if (p->opcode == OP_call || p->opcode == OP_indirect)
            break;
        if (p->opcode == OP_push)
            return NULL;
    }

    insn_t *walk = first;
    for (int i = 0; i < argc; i++) {
        args[i] = walk->rs1;
        walk = walk->next;
    }

    insn_t *last = call;
    var_t *dest = NULL;

    if (call->next && call->next->opcode == OP_func_ret) {
        last = call->next;
        dest = last->rd;
    }

    insn_t *ret = callee->inline_ret;

    /* A value the caller wants and the callee does not produce, or the other
     * way round, would need a fabricated definition.
     */
    if (!dest != !ret->rs1)
        return NULL;

    inline_map_n = 0;
    for (int i = 0; i < argc; i++) {
        if (inline_map_n >= MAX_INLINE_VARS)
            return NULL;
        inline_from[inline_map_n] = &callee->param_defs[i];
        inline_to[inline_map_n] = args[i];
        inline_map_n++;
    }

    block_t *scope = bb->scope;
    insn_t *after = first->prev;
    insn_t *next;

    for (insn_t *dead = first; dead; dead = next) {
        next = dead->next;
        bb_remove_insn(bb, dead);
        if (dead == last)
            break;
    }

    for (basic_block_t *src_bb = callee->bbs; src_bb; src_bb = src_bb->next) {
        for (insn_t *src = src_bb->insn_list.head; src; src = src->next) {
            if (src == ret)
                break;
            after = inline_clone(bb, after, src, scope);
        }
    }

    if (dest) {
        insn_t *copy =
            new_insn(OP_assign, dest, inline_lookup(ret->rs1, scope), NULL);

        bb_insert_after(bb, after, copy);
        after = copy;
    }

    *done = 1;
    if (after)
        return after->next;
    return bb->insn_list.head;
}

void inline_calls(void)
{
    for (int round = 0; round < MAX_INLINE_ROUNDS; round++) {
        bool changed = false;

        inline_round++;

        for (func_t *func = FUNC_LIST.head; func; func = func->next) {
            if (!func->bbs)
                continue;
            for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
                insn_t *next;

                for (insn_t *insn = bb->insn_list.head; insn; insn = next) {
                    next = insn->next;
                    if (insn->opcode != OP_call || !insn->str)
                        continue;

                    int done;
                    insn_t *resume = inline_call_at(bb, insn, &done);

                    if (done) {
                        next = resume;
                        changed = true;
                        /* The body just grew, so the verdict cached for it
                         * this round no longer describes it -- and losing a
                         * call may be exactly what makes it copyable.
                         */
                        func->inline_gen = 0;
                    }
                }
            }
        }
        if (!changed)
            break;
    }
}

/* Walk an array with a pointer instead of recomputing its address.
 *
 * "a[i * 64 + k]" inside the k loop spends four instructions turning k into an
 * address, every one of which changes by the same amount from one iteration to
 * the next. Computing the first address before the loop and adding that amount
 * at the bottom leaves the body with the access alone -- matmul's innermost
 * loop spent six of its sixteen instructions on the two subscripts.
 *
 * Unlike hoisting a loop-invariant value, this pays even when the result has
 * to live on the frame: the loop trades a four-instruction recomputation for
 * one addition, where hoisting trades a recomputation for a reload.
 */
/* The variable the loop counts with, where the chain walk stops: its value
 * before the loop is what the first address is computed from.
 */
var_t *sr_iv;

var_t *sr_base(var_t *var)
{
    if (!var)
        return NULL;
    return var->base ? var->base : var;
}

/* Whether @var is written inside the loop being examined. A constant never is,
 * wherever its materialisation sits: the allocator emits one wherever the
 * value is wanted.
 */
bool sr_varies(var_t *var)
{
    var_t *base;

    if (!var || var->is_const)
        return false;
    base = sr_base(var);
    return base && base->loop_stamp == sr_gen;
}

/* How much @var moves per iteration, into *step. Reports false when that is
 * not known -- which for a variable the loop writes means "not yet derived".
 */
bool sr_step_of(var_t *var, int *step)
{
    var_t *base;

    if (!var)
        return false;
    if (var->is_const) {
        *step = 0;
        return true;
    }
    base = sr_base(var);
    if (!base)
        return false;
    if (base->iv_gen == sr_gen) {
        *step = base->iv_step;
        return true;
    }
    if (base->loop_stamp != sr_gen) {
        *step = 0; /* nothing in the loop writes it */
        return true;
    }
    return false;
}

void sr_set_step(var_t *var, int step)
{
    var_t *base = sr_base(var);

    if (!base)
        return;
    base->iv_gen = sr_gen;
    base->iv_step = step;
}

/* Whether @insn is one the pass is willing to lift out of a loop: it computes
 * a value from its operands, touches no memory and faults on nothing. Taking
 * an array's address qualifies -- that address is the same on every
 * iteration -- and a literal does not, because the allocator materialises one
 * wherever it is wanted and moving it would only lengthen a live range.
 */
bool sr_movable(insn_t *insn)
{
    switch (insn->opcode) {
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_assign:
    case OP_address_of:
    case OP_global_address_of:
    case OP_load_data_address:
    case OP_load_rodata_address:
        return true;
    default:
        return false;
    }
}

/* The instruction inside the loop that defines @var, or NULL. */
insn_t *sr_def_of(func_t *func, var_t *var)
{
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        if (bb->loop_mark != sr_gen)
            continue;
        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            if (insn->rd == var)
                return insn;
        }
    }
    return NULL;
}

/* Find the variable the loop counts with, and how far it counts each time.
 *
 * It is the one whose only write in the loop adds a literal to itself, which
 * after phi unwinding is a pair: a temporary holding the sum, and a copy of
 * that temporary back into the variable.
 */
var_t *sr_basic_iv(func_t *func, basic_block_t *latch, int *step)
{
    var_t *found = NULL;

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        if (bb->loop_mark != sr_gen)
            continue;
        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            if (insn->opcode != OP_assign && insn->opcode != OP_unwound_phi)
                continue;
            if (!insn->rd || !insn->rs1)
                continue;

            var_t *k = sr_base(insn->rd);
            insn_t *sum = sr_def_of(func, insn->rs1);

            if (!k || !sum)
                continue;
            if (sum->opcode != OP_add && sum->opcode != OP_sub)
                continue;
            if (!sum->rs1 || !sum->rs2)
                continue;

            var_t *addend = NULL;
            var_t *carried = NULL;
            int sign = 1;

            if (sr_base(sum->rs1) == k && sum->rs2->is_const) {
                addend = sum->rs2;
                carried = sum->rs1;
                if (sum->opcode == OP_sub)
                    sign = -1; /* counting down */
            } else if (sum->opcode == OP_add && sr_base(sum->rs2) == k &&
                       sum->rs1->is_const) {
                addend = sum->rs1;
                carried = sum->rs2;
            }
            if (!addend || !addend->init_val)
                continue;
            /* The literal has to be added to the variable itself, not to
             * something computed from it: a temporary made from a variable
             * carries that variable as its base, so "g = g * 2 + 1" matches
             * the shape as readily as "i = i + 1" and would be taken for a
             * counter stepping by one. The value flowing in is the one the
             * loop either carries round in a phi or never writes.
             */
            insn_t *src = sr_def_of(func, carried);

            if (src) {
                if (src->opcode != OP_assign && src->opcode != OP_unwound_phi)
                    continue;
                if (sr_base(src->rs1) != k)
                    continue;
            }
            /* Two counters would each need their own analysis. */
            if (found && found != k)
                return NULL;
            found = k;
            *step = sign * addend->init_val;
        }
    }
    if (!found)
        return NULL;

    /* One variable the loop writes twice is not a counter: "i++" in one arm
     * and "i--" in another moves it by neither step, and an address derived
     * from it would advance by a fixed amount that matches neither.
     */
    int writes = 0;
    basic_block_t *wblk = NULL;

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        if (bb->loop_mark != sr_gen)
            continue;
        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            /* Only a copy back into the variable counts. The arithmetic that
             * produces the new value writes a temporary, and a temporary made
             * from a variable carries that variable as its base.
             */
            /* Only a write of the variable itself counts. The arithmetic
             * that produces the new value writes a temporary, and a temporary
             * made from a variable carries that variable as its base -- but a
             * select names the variable, and one writing the counter means the
             * counter does not advance on every trip round.
             */
            if (insn->opcode != OP_assign && insn->opcode != OP_unwound_phi &&
                insn->opcode != OP_cmov)
                continue;
            if (!insn->rd || sr_base(insn->rd) != found)
                continue;
            /* A phi copying the variable to itself carries it around the loop
             * rather than giving it a new value.
             */
            if (insn->opcode != OP_cmov && sr_base(insn->rs1) == found)
                continue;
            writes++;
            wblk = bb;
        }
    }
    if (writes != 1)
        return NULL;

    /* The write has to happen on every trip round, because the pointer derived
     * from the counter is advanced on every trip round. A "continue" that
     * jumps over the sole increment leaves the two disagreeing, and the loop
     * reads one element too far from then on.
     */
    if (wblk != latch && !is_dominate(wblk, latch))
        return NULL;
    return found;
}

/* Derive how far every value in the loop moves, from the counter outwards.
 * Several rounds because a chain is only as derivable as its operands.
 */
void sr_derive_steps(func_t *func)
{
    for (int round = 0; round < MAX_IV_ROUNDS; round++) {
        bool changed = false;

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            if (bb->loop_mark != sr_gen)
                continue;
            for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
                int a = 0, b = 0, s = 0;

                if (!insn->rd)
                    continue;

                var_t *base = sr_base(insn->rd);

                if (!base || base->iv_gen == sr_gen)
                    continue;
                if (base->def_cnt != 1)
                    continue;
                if (!sr_step_of(insn->rs1, &a))
                    continue;

                switch (insn->opcode) {
                case OP_assign:
                    s = a;
                    break;
                case OP_add:
                    if (!sr_step_of(insn->rs2, &b))
                        continue;
                    s = a + b;
                    break;
                case OP_sub:
                    if (!sr_step_of(insn->rs2, &b))
                        continue;
                    s = a - b;
                    break;
                case OP_mul:
                    if (!insn->rs2 || !insn->rs2->is_const)
                        continue;
                    s = a * insn->rs2->init_val;
                    break;
                case OP_lshift:
                    if (!insn->rs2 || !insn->rs2->is_const)
                        continue;
                    if (insn->rs2->init_val < 0 || insn->rs2->init_val > 30)
                        continue;
                    s = a << insn->rs2->init_val;
                    break;
                default:
                    continue;
                }
                sr_set_step(insn->rd, s);
                changed = true;
            }
        }
        if (!changed)
            return;
    }
}

/* Gather, into @chain, the instructions of the loop that compute @var, in the
 * order they appear. Reports false when any of them is shared, has a side
 * effect, or the chain is longer than the pass is willing to move.
 */
bool sr_collect_chain(func_t *func, var_t *var, insn_t **chain, int *len)
{
    if (sr_base(var) == sr_iv)
        return true; /* the counter itself: the preheader has its first value */

    insn_t *def = sr_def_of(func, var);

    if (!def)
        return false;
    for (int i = 0; i < *len; i++) {
        if (chain[i] == def)
            return true; /* already collected */
    }
    if (*len >= MAX_IV_CHAIN)
        return false;
    if (!sr_movable(def))
        return false;

    var_t *base = sr_base(def->rd);

    if (!base || base->def_cnt != 1)
        return false;
    if (base->is_global || base->address_taken)
        return false;

    chain[*len] = def;
    *len = *len + 1;

    var_t *ops[3];

    ops[0] = def->rs1;
    ops[1] = def->rs2;
    ops[2] = def->rs3;
    for (int k = 0; k < 3; k++) {
        if (!sr_varies(ops[k]))
            continue; /* a constant or something the loop never writes */
        if (!sr_collect_chain(func, ops[k], chain, len))
            return false;
    }
    return true;
}

/* Add "@var += @step" immediately before @latch's terminator. */
void sr_emit_advance(basic_block_t *latch, var_t *var, int step)
{
    block_t *scope = latch->scope;
    var_t *amount = new_const_var(scope, step);
    var_t *sum = require_var(scope);
    insn_t *after = latch->insn_list.tail;
    insn_t *load;
    insn_t *add;
    insn_t *assign;

    /* A loop latch ends in the jump back to its header.  Appending the
     * advance after that jump leaves it unreachable, so place the whole
     * sequence before the terminator instead. */
    if (after && (after->opcode == OP_branch || after->opcode == OP_jump ||
                  after->opcode == OP_return || after->opcode == OP_func_ret))
        after = after->prev;

    sum->var_name = gen_name();
    sum->type = var->type;
    sum->ptr_level = var->ptr_level;

    load = new_insn(OP_load_constant, amount, NULL, NULL);
    add = new_insn(OP_add, sum, var, amount);
    assign = new_insn(OP_assign, var, sum, NULL);
    bb_insert_after(latch, after, load);
    bb_insert_after(latch, load, add);
    bb_insert_after(latch, add, assign);
}

/* Turn the address @use reads or writes into a pointer the loop advances.
 * Reports whether it did.
 */
bool sr_reduce_access(func_t *func,
                      basic_block_t *pre,
                      basic_block_t *latch,
                      insn_t *use)
{
    /* One slot past the chain holds the access itself, so that the scan below
     * can be told to ignore the whole of what is moving in one list.
     */
    insn_t *chain[MAX_IV_CHAIN + 1];
    int len = 0, step = 0;
    var_t *addr = use->rs1;

    if (!addr || !sr_step_of(addr, &step) || !step)
        return false;
    if (!sr_collect_chain(func, addr, chain, &len) || !len)
        return false;
    if (len < MIN_IV_CHAIN)
        return false;

    /* Every value the chain computes has to belong to it alone: what moves out
     * of the loop stops being recomputed there.
     *
     * The access is skipped along with the chain, so its own operands are
     * checked by hand: it may read the address the chain ends at, and nothing
     * else. A value the chain computes that the access also stores would stay
     * behind in the preheader at its first-iteration value while the address
     * went on advancing.
     */
    chain[len] = use;
    for (int i = 0; i < len; i++) {
        var_t *val = chain[i]->rd;

        if (use->rs2 == val || use->rs3 == val)
            return false;
        if (use->rs1 == val && val != addr)
            return false;
        if (var_read_by(func, val, chain, len + 1, false))
            return false;
    }

    /* Order the chain so that each value lands after the ones it reads.
     *
     * The walk collected every definition before its operands, so reversing it
     * is nearly right -- but only while the chain is a straight line. A value
     * two of them share is collected under the first, and reversing then puts
     * it after the second, which would read a variable nothing had defined
     * yet. Choosing repeatedly instead is correct either way: take an entry
     * once every chain value it reads has been taken. Nothing is moved until
     * the whole order is settled, so a chain that cannot be ordered at all is
     * declined rather than half-moved.
     */
    int order[MAX_IV_CHAIN + 1];
    int placed[MAX_IV_CHAIN + 1];
    int n_placed = 0;

    for (int i = 0; i < len; i++)
        placed[i] = 0;

    while (n_placed < len) {
        int progress = 0;

        for (int i = len - 1; i >= 0; i--) {
            if (placed[i])
                continue;

            var_t *ops[3];
            int ready = 1;

            ops[0] = chain[i]->rs1;
            ops[1] = chain[i]->rs2;
            ops[2] = chain[i]->rs3;
            for (int k = 0; k < 3; k++) {
                if (!ops[k])
                    continue;
                for (int j = 0; j < len; j++) {
                    if (j != i && !placed[j] && chain[j]->rd == ops[k])
                        ready = 0;
                }
            }
            if (!ready)
                continue;

            order[n_placed++] = i;
            placed[i] = 1;
            progress = 1;
        }
        if (!progress)
            return false;
    }

    for (int i = 0; i < n_placed; i++) {
        insn_t *moving = chain[order[i]];

        bb_remove_insn(moving->belong_to, moving);
        bb_append_insn(pre, moving);
    }

    sr_emit_advance(latch, addr, step);
    return true;
}

/* Drop the literals the loop no longer needs.
 *
 * The shift counts and element widths a subscript used are materialised inside
 * the loop, and moving the arithmetic that read them out of it leaves those
 * materialisations behind with nothing to serve -- four of the fourteen
 * instructions in matmul's innermost loop.
 */
void sr_sweep_dead_consts(func_t *func)
{
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        if (bb->loop_mark != sr_gen)
            continue;

        insn_t *next;
        for (insn_t *insn = bb->insn_list.head; insn; insn = next) {
            next = insn->next;
            if (insn->opcode != OP_load_constant || !insn->rd)
                continue;
            if (insn->rd->is_global || insn->rd->address_taken)
                continue;
            if (var_read_by(func, insn->rd, NULL, 0, true))
                continue;
            bb_remove_insn(bb, insn);
        }
    }
}

/* The one block outside the loop that falls into @header, or NULL. */
basic_block_t *sr_preheader(basic_block_t *header)
{
    basic_block_t *pre = NULL;

    for (int i = 0; i < header->prev_idx; i++) {
        basic_block_t *p = header->prev[i].bb;

        if (!p)
            continue;
        if (p->loop_mark == sr_gen)
            continue; /* the latch, or anything else inside */
        if (p->then_ || p->else_ || p->next != header)
            return NULL;
        if (pre)
            return NULL;
        pre = p;
    }
    return pre;
}

/* How many blocks inside the loop jump back to @header. */
int sr_latch_count(basic_block_t *header)
{
    int n = 0;

    for (int i = 0; i < header->prev_idx; i++) {
        basic_block_t *p = header->prev[i].bb;

        if (p && p->loop_mark == sr_gen)
            n++;
    }
    return n;
}

void sr_loop(func_t *func, basic_block_t *header, basic_block_t *latch)
{
    int step = 0, made = 0;

    /* One way back, and one way in that runs everything before the loop: the
     * first address is computed there and advanced there. The test on the
     * latch costs nothing and comes before the walk that marks the loop.
     */
    if (latch->then_ || latch->else_ || latch->next != header)
        return;

    sr_gen = loop_scan_gen + 1;
    if (!mark_natural_loop(header, latch))
        return; /* the walk ran out of room, so loop_mark says nothing */

    if (sr_latch_count(header) != 1)
        return;

    basic_block_t *pre = sr_preheader(header);

    if (!pre)
        return;

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        if (bb->loop_mark != sr_gen)
            continue;
        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            var_t *base = sr_base(insn->rd);

            if (base)
                base->loop_stamp = sr_gen;
        }
    }

    var_t *iv = sr_basic_iv(func, latch, &step);

    if (!iv || !step)
        return;
    sr_iv = iv;
    sr_set_step(iv, step);
    sr_derive_steps(func);

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        if (bb->loop_mark != sr_gen)
            continue;

        insn_t *next;
        for (insn_t *insn = bb->insn_list.head; insn; insn = next) {
            next = insn->next;
            if (insn->opcode != OP_read && insn->opcode != OP_write)
                continue;
            /* A reduction takes instructions out of this block, so the walk
             * starts again rather than following a pointer into the block they
             * moved to.
             */
            if (made >= MAX_IV_PER_LOOP)
                break;
            if (sr_reduce_access(func, pre, latch, insn)) {
                made++;
                next = bb->insn_list.head;
            }
        }
    }

    if (made)
        sr_sweep_dead_consts(func);
}

void strength_reduce(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        if (!func->bbs)
            continue;

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
                var_t *base = sr_base(insn->rd);

                if (base)
                    base->def_cnt = 0;
            }
        }
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
                var_t *base = sr_base(insn->rd);

                if (base)
                    base->def_cnt++;
            }
        }

        for (basic_block_t *latch = func->bbs; latch; latch = latch->rpo_next) {
            basic_block_t *succ[3];

            succ[0] = latch->next;
            succ[1] = latch->then_;
            succ[2] = latch->else_;

            for (int k = 0; k < 3; k++) {
                if (!succ[k] || succ[k] == latch)
                    continue;
                if (!is_dominate(succ[k], latch))
                    continue;
                sr_loop(func, succ[k], latch);
            }
        }
    }
}

void if_convert(void)
{
    /* The guard wraps the body rather than returning early: shecc parses its
     * own source, and a target without conditional moves would be left with a
     * loop after a return, which it rejects as unreachable code.
     */
#ifdef HAVE_COND_MOVE
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        if (!func->bbs)
            continue;
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next)
            if_convert_bb(func, bb);
    }
#endif
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

/* Whether @pred is a proper ancestor of @succ in the dominator tree.
 *
 * The walk goes up from @succ rather than down through everything @pred
 * dominates: the answer lies on the one path to the root, where the downward
 * search visited @pred's whole subtree and did not stop when it found it.
 * dom_prev is the inverse of the dom_next the search followed, so the two
 * agree on every pair.
 */
bool is_dominate(basic_block_t *pred, basic_block_t *succ)
{
    for (basic_block_t *bb = succ; bb; bb = bb->dom_prev) {
        if (bb->dom_prev == pred)
            return true;
    }
    return false;
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
            char str[DUMP_INSN_LEN];
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
                /* Split in two: a single call would pass nine arguments, one
                 * more than MAX_PARAMS, and the last one -- rs2's subscript --
                 * came out as garbage.
                 */
                sprintf(str, "<%s<SUB>%d</SUB> := %s<SUB>%d</SUB> %s ",
                        insn->rd->var_name, insn->rd->subscript,
                        insn->rs1->var_name, insn->rs1->subscript,
                        get_insn_op(insn));
                sprintf(str + strlen(str), "%s<SUB>%d</SUB>>",
                        insn->rs2->var_name, insn->rs2->subscript);
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

    for (int i = 0; i < bb->prev_idx; i++)
        if (bb->prev[i].bb)
            bb_dump_connection(fd, bb->prev[i].bb, bb, bb->prev[i].type);
}

void dump_cfg(char name[])
{
    FILE *fd = fopen(name, "w");

    if (!fd)
        usage_error("Unable to open DOT output");

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

int func_marked_count;

/* Mark @var's function as reachable when @var names one, so a function reached
 * only through a pointer is not mistaken for dead.
 */
void func_mark_addressed(var_t *var)
{
    if (!var || !var->is_func)
        return;

    /* is_func labels function-pointer variables too, and those names match no
     * function; find_func() answering NULL is the ordinary case here.
     */
    func_t *target = find_func(var->var_name);
    if (target && !target->is_used) {
        target->is_used = true;
        func_marked_count++;
    }
}

/* Mark everything the code in @bbs names: the target of every direct call, and
 * every function whose address it takes.
 *
 * A function symbol only ever reaches the IR as an operand -- never as a
 * destination, since nothing assigns to a function -- so the two source slots
 * are the only ones worth looking at.
 */
void func_mark_reached_from(basic_block_t *bbs)
{
    for (basic_block_t *bb = bbs; bb; bb = bb->rpo_next) {
        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            if (insn->opcode == OP_call) {
                func_t *callee = find_func(insn->str);
                if (callee && !callee->is_used) {
                    callee->is_used = true;
                    func_marked_count++;
                }
                continue;
            }
            func_mark_addressed(insn->rs1);
            func_mark_addressed(insn->rs2);
        }
    }
}

/* Drop the functions no call can reach.
 *
 * Every compile prepends the whole of lib/c.c, so a program that prints one
 * line had registers allocated and machine code emitted for every function the
 * library defines. Nothing calls those, and both the work and the bytes they
 * occupy in the output are wasted: on a small input this is most of what the
 * compiler does.
 *
 * Reachability starts at main and at every function whose address is taken
 * anywhere -- an indirect call names no callee, so a function reached that way
 * has to be kept on the strength of the address alone. Functions with no body
 * stay either way: they are declarations, and the dynamic linker path looks
 * them up by name.
 *
 * is_used carries the mark. It belongs to the dynamic linker, which sets it
 * later during register allocation, so this pass leaves it as it found it.
 */
void prune_unused_funcs(void)
{
    func_t *entry = find_func("main");
    if (!entry)
        return;

    int count = 0;
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        func->is_used = false;
        count++;
    }

    /* Expanded once each: a body is scanned no more than once however many
     * times the sweep below passes over the list.
     */
    char *expanded = arena_alloc(GENERAL_ARENA, count);
    for (int i = 0; i < count; i++)
        expanded[i] = 0;

    entry->is_used = true;
    func_marked_count = 1;

    /* Global initializers run before main and can name a function, so they are
     * a root in their own right.
     */
    if (GLOBAL_FUNC)
        func_mark_reached_from(GLOBAL_FUNC->bbs);

    /* Once every function in the list is marked there is nothing left to
     * discover, and the bodies not yet expanded need not be read at all. That
     * is the usual case when the input is a large program rather than a small
     * one carrying the library along: the sweep stops instead of walking the
     * whole IR.
     */
    bool changed = true;
    while (changed && func_marked_count < count) {
        changed = false;

        int idx = 0;
        for (func_t *func = FUNC_LIST.head; func; func = func->next) {
            int at = idx++;

            if (!func->is_used || expanded[at])
                continue;
            expanded[at] = 1;
            changed = true;
            func_mark_reached_from(func->bbs);
            if (func_marked_count >= count)
                break;
        }
    }

    func_t *head = NULL, *tail = NULL;
    for (func_t *func = FUNC_LIST.head; func;) {
        func_t *next = func->next;

        func->next = NULL;
        if (func->bbs && !func->is_used) {
            func = next;
            continue;
        }

        if (!head)
            head = func;
        else
            tail->next = func;
        tail = func;
        func = next;
    }
    FUNC_LIST.head = head;
    FUNC_LIST.tail = tail;

    for (func_t *func = FUNC_LIST.head; func; func = func->next)
        func->is_used = false;
}

/* Builds SSA form and stops there; unwind_phi() is the caller's to call. */
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
    /* A variable whose address escaped can be written through that pointer
     * between the assignment and the next read -- a callee handed "&var" does
     * exactly that -- so its value is not the constant it was given, however
     * plainly it was given one. Marking it anyway makes every later reload
     * materialise the initialiser instead of reading the slot, which is how
     * "int a = 0; f(&a); int b = a;" left b holding zero.
     */
    if (insn->rd && insn->rd->address_taken)
        return false;

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
    /* Copying from such a variable is no better: the value read is whatever
     * the pointer last wrote, not the constant the source was assigned.
     */
    if (insn->rs1->address_taken)
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

bool var_escapes(var_t *var)
{
    /* Reports every variable as escaping, which makes dce_init_mark() treat
     * every OP_write as useful and so disables SSA-level dead-store
     * elimination. The is_global/is_func branches that used to precede this
     * were unreachable for the same reason and are gone rather than left
     * reading as though they decided something.
     */
    return true;
}

/* Append one initial useful instruction, reserving space only when it exists.
 */
void dce_init_push(insn_t *work_list[],
                   int work_list_idx,
                   int *mark_num,
                   insn_t *insn)
{
    if (work_list_idx + *mark_num >= DCE_WORKLIST_SIZE)
        fatal("DCE worklist size exceeded");

    work_list[work_list_idx + *mark_num] = insn;
    *mark_num = *mark_num + 1;
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
        dce_init_push(work_list, work_list_idx, &mark_num, insn);
        break;
    case OP_write:
    case OP_store:
        /* Only mark stores/writes to escaping variables as useful */
        if (!insn->rd || var_escapes(insn->rd)) {
            insn->useful = true;
            insn->belong_to->useful = true;
            dce_init_push(work_list, work_list_idx, &mark_num, insn);
        }
        break;
    case OP_global_store:
        /* Global stores always escape */
        insn->useful = true;
        insn->belong_to->useful = true;
        dce_init_push(work_list, work_list_idx, &mark_num, insn);
        break;
    case OP_address_of:
    case OP_unwound_phi:
    case OP_allocat:
        insn->useful = true;
        insn->belong_to->useful = true;
        dce_init_push(work_list, work_list_idx, &mark_num, insn);
        break;
    case OP_indirect:
    case OP_call:
        insn->useful = true;
        insn->belong_to->useful = true;
        dce_init_push(work_list, work_list_idx, &mark_num, insn);
        /* mark precall and postreturn sequences at calls */
        if (insn->next && insn->next->opcode == OP_func_ret) {
            insn->next->useful = true;
            dce_init_push(work_list, work_list_idx, &mark_num, insn->next);
        }
        while (insn->prev && insn->prev->opcode == OP_push) {
            insn = insn->prev;
            insn->useful = true;
            dce_init_push(work_list, work_list_idx, &mark_num, insn);
        }
        break;
    default:
        if (!insn->rd)
            break;
        /* if the instruction affects a global value, set "useful" */
        if (insn->rd->is_global && !insn->useful) {
            insn->useful = true;
            insn->belong_to->useful = true;
            dce_init_push(work_list, work_list_idx, &mark_num, insn);
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
        work_list_idx += dce_init_mark(insn, work_list, work_list_idx);
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

                /* Strength reduction for power-of-2 operations.
                 *
                 * The replacement operand has to be a variable of its own.
                 * mark_const() hands every use of a folded local the same
                 * var_t, and that var_t is what its defining OP_load_constant
                 * materialises, so rewriting init_val in place changes the
                 * value every other use sees: "int k = 8; return a*k + b*k;"
                 * returned 2 << 3 + 3 * 3.
                 */
                if (insn->rs2 && insn->rs2->is_const && insn->rd) {
                    int val = insn->rs2->init_val;
                    int shift = exact_log2(val);
                    opcode_t reduced = OP_generic;
                    int operand = 0;

                    if (shift >= 0) {
                        /* x * power_of_2 = x << shift */
                        if (insn->opcode == OP_mul) {
                            reduced = OP_lshift;
                            operand = shift;
                        }
                        /* x / power_of_2 = x >> shift (unsigned) */
                        else if (insn->opcode == OP_div) {
                            reduced = OP_rshift;
                            operand = shift;
                        }
                        /* x % power_of_2 = x & (power_of_2 - 1) */
                        else if (insn->opcode == OP_mod) {
                            reduced = OP_bit_and;
                            operand = val - 1;
                        }
                    }

                    if (reduced != OP_generic) {
                        var_t *amount = new_const_var(bb->scope, operand);

                        bb_insert_after(
                            bb, insn->prev,
                            new_insn(OP_load_constant, amount, NULL, NULL));
                        insn->opcode = reduced;
                        insn->rs2 = amount;
                    }
                }

                /* more optimizations */
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

void update_consumed(insn_t *insn, var_t *var);

/* Combined function to reset and solve locals in one pass */
void bb_reset_and_solve_locals(func_t *func, basic_block_t *bb)
{
    UNUSED(func);

    /* Reset live_kill list */
    bb->live_kill.size = 0;

    /* Both sets are asked about once per operand and once per destination, and
     * answering from the lists themselves means a scan of one of them for
     * every one of a block's instructions -- quadratic in the size of the
     * block, which is what made this the most expensive part of the analysis
     * on shecc's own longer functions. Stamping a variable as it enters a set
     * turns each of those questions into one comparison. live_kill was just
     * emptied, so nothing carries a stale stamp; live_gen is not, so what it
     * already holds is stamped first.
     */
    liveness_gen++;
    int gen = liveness_gen;
    for (int k = 0; k < bb->live_gen.size; k++)
        bb->live_gen.elements[k]->in_gen = gen;

    /* Solve locals */
    int i = 0;
    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
        insn->idx = i++;

        /* The three source operands are treated alike; the third is the value
         * a select keeps when its condition does not hold.
         */
        var_t *srcs[3];
        srcs[0] = insn->rs1;
        srcs[1] = insn->rs2;
        srcs[2] = insn->rs3;
        for (int k = 0; k < 3; k++) {
            var_t *src = srcs[k];
            if (!src)
                continue;
            if (src->kill_gen != gen && !src->is_global && src->in_gen != gen) {
                src->in_gen = gen;
                var_list_append(&bb->live_gen, src);
            }
            update_consumed(insn, src);
        }

        var_t *rd = insn->rd;
        if (rd && rd->kill_gen != gen) {
            rd->kill_gen = gen;
            var_list_append(&bb->live_kill, rd);
        }
    }
}

void update_consumed(insn_t *insn, var_t *var)
{
    if (insn->idx > var->consumed)
        var->consumed = insn->idx;
}

void compute_live_in(basic_block_t *bb)
{
    bb->live_in.size = 0;

    /* This runs to a fixed point over every block, so the two membership tests
     * below used to dominate the pass: one linear scan of live_kill per
     * candidate, plus one linear scan of live_in per insertion. Stamping both
     * sets with the current generation makes each test a single comparison.
     */
    liveness_gen++;
    for (int i = 0; i < bb->live_kill.size; i++)
        bb->live_kill.elements[i]->kill_gen = liveness_gen;

    for (int i = 0; i < bb->live_out.size; i++) {
        var_t *var = bb->live_out.elements[i];
        if (var->kill_gen == liveness_gen)
            continue;
        if (var->in_gen == liveness_gen)
            continue;
        var->in_gen = liveness_gen;
        var_list_append(&bb->live_in, var);
    }
    for (int i = 0; i < bb->live_gen.size; i++) {
        var_t *var = bb->live_gen.elements[i];
        if (var->in_gen == liveness_gen)
            continue;
        var->in_gen = liveness_gen;
        var_list_append(&bb->live_in, var);
    }
}

/* Add bb's live_in to the successor union being built in @live_out, skipping
 * variables already there. Membership is a stamp comparison rather than a scan
 * of the union, which this used to do for every candidate.
 */
int merge_live_in(var_t *live_out[], int live_out_idx, basic_block_t *bb)
{
    for (int i = 0; i < bb->live_in.size; i++) {
        var_t *var = bb->live_in.elements[i];
        if (var->merge_gen == live_merge_gen)
            continue;
        if (live_out_idx >= MAX_ANALYSIS_STACK_SIZE)
            break;
        var->merge_gen = live_merge_gen;
        live_out[live_out_idx++] = var;
    }
    return live_out_idx;
}

bool recompute_live_out(basic_block_t *bb)
{
    var_t *live_out[MAX_ANALYSIS_STACK_SIZE];
    int live_out_idx = 0;

    live_merge_gen++;

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
            bb_add_killed_var(func->bbs, var_subscript0(&func->param_defs[i]));
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
