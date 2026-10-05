#define _POSIX_C_SOURCE 200809L

/* Host-only profiling keeps graph construction and verification outside the
 * timed region; each sample batches fresh functions to reduce clock noise.
 */
#include "vir.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef bool (*vir_profile_builder_t)(vir_function_t *, int, int);

typedef struct {
    const char *name;
    vir_profile_builder_t build;
    int size;
    int depth;
    int expected_hoisted;
    int expected_loops;
    bool expect_no_value_worklist;
    int expected_readiness_admissions;
    int expected_readiness_sample_values;
} vir_profile_case_t;

static uint64_t vir_profile_now_ns(void)
{
    struct timespec time;

    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t) time.tv_sec * 1000000000ULL + (uint64_t) time.tv_nsec;
}

static bool vir_profile_build_sparse_chain(vir_function_t *func,
                                           int size,
                                           int depth)
{
    vir_block_t **body = NULL;
    vir_block_t *entry;
    vir_block_t *header;
    vir_block_t *exit;
    vir_value_t *condition;
    vir_value_t *left;
    vir_value_t *right;
    vir_value_t *previous = NULL;
    vir_edge_args_t body_edge;
    vir_edge_args_t exit_edge;
    bool ok = false;

    (void) depth;
    body = malloc((size_t) size * sizeof(*body));
    if (!body)
        goto done;
    vir_function_init(func, 256);
    entry = vir_block_create(func);
    header = vir_block_create(func);
    exit = vir_block_create(func);
    condition = entry ? vir_block_add_param(func, entry, VIR_TYPE_I1) : NULL;
    left = entry ? vir_block_add_param(func, entry, VIR_TYPE_I32) : NULL;
    right = entry ? vir_block_add_param(func, entry, VIR_TYPE_I32) : NULL;
    if (!entry || !header || !exit || !condition || !left || !right)
        goto done;
    for (int i = 0; i < size; i++) {
        body[i] = vir_block_create(func);
        if (!body[i])
            goto done;
    }
    for (int i = size - 1; i >= 0; i--) {
        vir_value_t *value =
            vir_binary(func, body[i], i == size - 1 ? VIR_OP_MUL : VIR_OP_ADD,
                       VIR_TYPE_I32, i == size - 1 ? left : previous, right);

        if (!value)
            goto done;
        previous = value;
        if (i < size - 1 &&
            !vir_edge_create(func, body[i + 1], body[i], NULL, 0))
            goto done;
    }
    body_edge.to = body[size - 1];
    body_edge.args = NULL;
    body_edge.arg_count = 0;
    exit_edge.to = exit;
    exit_edge.args = NULL;
    exit_edge.arg_count = 0;
    if (!vir_edge_create(func, entry, header, NULL, 0) ||
        !vir_block_set_branch(func, header, condition, &body_edge,
                              &exit_edge) ||
        !vir_edge_create(func, body[0], header, NULL, 0) ||
        !vir_block_set_return(func, exit, left))
        goto done;
    ok = true;

done:
    free(body);
    return ok;
}

static bool vir_profile_build_joins(vir_function_t *func, int size, int depth)
{
    vir_block_t **blocks = NULL;
    vir_block_t *entry;
    vir_block_t *header;
    vir_block_t *exit;
    vir_value_t *condition;
    vir_value_t *left;
    vir_value_t *right;
    vir_edge_args_t body_edge;
    vir_edge_args_t exit_edge;
    bool ok = false;

    (void) depth;
    blocks = calloc((size_t) size * 4, sizeof(*blocks));
    if (!blocks)
        goto done;
    vir_function_init(func, 256);
    entry = vir_block_create(func);
    header = vir_block_create(func);
    exit = vir_block_create(func);
    condition = entry ? vir_block_add_param(func, entry, VIR_TYPE_I1) : NULL;
    left = entry ? vir_block_add_param(func, entry, VIR_TYPE_I32) : NULL;
    right = entry ? vir_block_add_param(func, entry, VIR_TYPE_I32) : NULL;
    if (!entry || !header || !exit || !condition || !left || !right)
        goto done;
    for (int i = 0; i < size; i++) {
        vir_block_t **stage = &blocks[4 * i];

        for (int j = 0; j < 4; j++) {
            stage[j] = vir_block_create(func);
            if (!stage[j])
                goto done;
        }
        if (!vir_block_add_param(func, stage[3], VIR_TYPE_I32))
            goto done;
    }
    body_edge.to = blocks[0];
    body_edge.args = NULL;
    body_edge.arg_count = 0;
    exit_edge.to = exit;
    exit_edge.args = NULL;
    exit_edge.arg_count = 0;
    if (!vir_edge_create(func, entry, header, NULL, 0) ||
        !vir_block_set_branch(func, header, condition, &body_edge, &exit_edge))
        goto done;
    for (int i = 0; i < size; i++) {
        vir_block_t **stage = &blocks[4 * i];
        vir_block_t *next = i + 1 == size ? header : blocks[4 * (i + 1)];
        vir_edge_args_t left_edge = {stage[1], NULL, 0};
        vir_edge_args_t right_edge = {stage[2], NULL, 0};
        vir_value_t *left_value = vir_add(func, stage[1], left, right);
        vir_value_t *right_value = vir_add(func, stage[2], left, right);
        vir_value_t *left_args[] = {left_value};
        vir_value_t *right_args[] = {right_value};

        if (!left_value || !right_value ||
            !vir_block_set_branch(func, stage[0], condition, &left_edge,
                                  &right_edge) ||
            !vir_edge_create(func, stage[1], stage[3], left_args, 1) ||
            !vir_edge_create(func, stage[2], stage[3], right_args, 1))
            goto done;
        if (!vir_edge_create(func, stage[3], next, NULL, 0))
            goto done;
    }
    if (!vir_block_set_return(func, exit, left))
        goto done;
    ok = true;

done:
    free(blocks);
    return ok;
}

static bool vir_profile_build_nested(vir_function_t *func, int size, int depth)
{
    vir_block_t **headers = NULL;
    vir_block_t **latches = NULL;
    vir_value_t **conditions = NULL;
    vir_block_t *entry;
    vir_block_t *body;
    vir_block_t *exit;
    vir_value_t *left;
    vir_value_t *right;
    vir_value_t *outer_value;
    vir_value_t *exit_value;
    vir_value_t *previous;
    vir_value_t *outer_next;
    vir_value_t *entry_args[1];
    vir_value_t *outer_args[1];
    bool ok = false;

    headers = calloc((size_t) depth, sizeof(*headers));
    latches = depth > 1 ? calloc((size_t) depth - 1, sizeof(*latches)) : NULL;
    conditions = malloc((size_t) depth * sizeof(*conditions));
    if (!headers || (depth > 1 && !latches) || !conditions)
        goto done;
    vir_function_init(func, 256);
    entry = vir_block_create(func);
    for (int i = 0; i < depth; i++)
        headers[i] = vir_block_create(func);
    for (int i = 0; i < depth - 1; i++)
        latches[i] = vir_block_create(func);
    body = vir_block_create(func);
    exit = vir_block_create(func);
    if (!entry || !body || !exit)
        goto done;
    for (int i = 0; i < depth; i++) {
        conditions[i] = vir_block_add_param(func, entry, VIR_TYPE_I1);
        if (!headers[i] || !conditions[i])
            goto done;
    }
    for (int i = 0; i < depth - 1; i++)
        if (!latches[i])
            goto done;
    left = vir_block_add_param(func, entry, VIR_TYPE_I32);
    right = vir_block_add_param(func, entry, VIR_TYPE_I32);
    outer_value = vir_block_add_param(func, headers[0], VIR_TYPE_I32);
    exit_value = vir_block_add_param(func, exit, VIR_TYPE_I32);
    if (!left || !right || !outer_value || !exit_value)
        goto done;

    previous = outer_value;
    for (int i = 0; i < size; i++) {
        previous = vir_add(func, body, previous, right);
        if (!previous)
            goto done;
    }
    outer_next =
        vir_add(func, depth == 1 ? body : latches[0], outer_value, right);
    if (!outer_next)
        goto done;
    entry_args[0] = left;
    outer_args[0] = outer_next;
    if (!vir_edge_create(func, entry, headers[0], entry_args, 1))
        goto done;

    for (int i = 0; i < depth; i++) {
        vir_edge_args_t body_edge;
        vir_edge_args_t loop_exit;

        body_edge.to = i + 1 < depth ? headers[i + 1] : body;
        body_edge.args = NULL;
        body_edge.arg_count = 0;
        loop_exit.to = i == 0 ? exit : latches[i - 1];
        loop_exit.args = i == 0 ? &outer_value : NULL;
        loop_exit.arg_count = i == 0 ? 1 : 0;
        if (!vir_block_set_branch(func, headers[i], conditions[i], &body_edge,
                                  &loop_exit))
            goto done;
    }
    if (depth == 1) {
        if (!vir_edge_create(func, body, headers[0], outer_args, 1))
            goto done;
    } else {
        for (int i = 0; i < depth - 1; i++) {
            vir_value_t **args = i == 0 ? outer_args : NULL;
            int arg_count = i == 0 ? 1 : 0;

            if (!vir_edge_create(func, latches[i], headers[i], args, arg_count))
                goto done;
        }
        if (!vir_edge_create(func, body, headers[depth - 1], NULL, 0))
            goto done;
    }
    if (!vir_block_set_return(func, exit, exit_value))
        goto done;
    ok = true;

done:
    free(headers);
    free(latches);
    free(conditions);
    return ok;
}

static bool vir_profile_build_partial_fallback(vir_function_t *func,
                                               int size,
                                               int depth)
{
    vir_block_t *entry;
    vir_block_t *header;
    vir_block_t *user;
    vir_block_t *definition;
    vir_block_t *exit;
    vir_value_t *condition;
    vir_value_t *left;
    vir_value_t *right;
    vir_value_t *scale;
    vir_edge_args_t body_edge;
    vir_edge_args_t exit_edge;

    (void) depth;
    vir_function_init(func, 32);
    entry = vir_block_create(func);
    header = vir_block_create(func);

    /* The user block comes first, so the eager source-order scan must move the
     * roots first and then use the queue to move their dependents.
     */
    user = vir_block_create(func);
    definition = vir_block_create(func);
    exit = vir_block_create(func);
    if (!entry || !header || !user || !definition || !exit)
        return false;
    condition = vir_block_add_param(func, entry, VIR_TYPE_I1);
    left = vir_block_add_param(func, entry, VIR_TYPE_I32);
    right = vir_block_add_param(func, entry, VIR_TYPE_I32);
    scale = vir_block_add_param(func, entry, VIR_TYPE_I32);
    if (!condition || !left || !right || !scale)
        return false;
    for (int i = 0; i < size; i++) {
        vir_value_t *constant = vir_const_i32(func, entry, i + 1);
        vir_value_t *root =
            constant ? vir_add(func, definition, left, constant) : NULL;

        if (!root || !vir_mul(func, user, root, scale))
            return false;
    }
    body_edge.to = definition;
    body_edge.args = NULL;
    body_edge.arg_count = 0;
    exit_edge.to = exit;
    exit_edge.args = NULL;
    exit_edge.arg_count = 0;
    if (!vir_edge_create(func, entry, header, NULL, 0) ||
        !vir_block_set_branch(func, header, condition, &body_edge,
                              &exit_edge) ||
        !vir_edge_create(func, definition, user, NULL, 0) ||
        !vir_edge_create(func, user, header, NULL, 0) ||
        !vir_block_set_return(func, exit, left))
        return false;
    return true;
}

static bool vir_profile_build_spread_chain(vir_function_t *func,
                                           int values_per_block,
                                           int value_blocks,
                                           int dependent_values_per_root)
{
    vir_block_t **body = NULL;
    vir_block_t *entry;
    vir_block_t *header;
    vir_block_t *exit;
    vir_value_t *condition;
    vir_value_t *left;
    vir_value_t *right;
    vir_edge_args_t body_edge;
    vir_edge_args_t exit_edge;
    bool ok = false;

    body = calloc((size_t) value_blocks, sizeof(*body));
    if (!body)
        goto done;
    vir_function_init(func, 64);
    entry = vir_block_create(func);
    header = vir_block_create(func);
    exit = vir_block_create(func);
    if (!entry || !header || !exit)
        goto done;
    condition = vir_block_add_param(func, entry, VIR_TYPE_I1);
    left = vir_block_add_param(func, entry, VIR_TYPE_I32);
    right = vir_block_add_param(func, entry, VIR_TYPE_I32);
    if (!condition || !left || !right)
        goto done;
    for (int i = 0; i < value_blocks; i++) {
        body[i] = vir_block_create(func);
        if (!body[i])
            goto done;
        for (int j = 0; j < values_per_block; j++) {
            int constant_value = i * values_per_block + j + 1;
            vir_value_t *constant = vir_const_i32(func, entry, constant_value);
            vir_value_t *root =
                constant ? vir_add(func, body[i], left, constant) : NULL;
            vir_value_t *value = root;

            if (!root)
                goto done;
            for (int k = 0; k < dependent_values_per_root; k++) {
                value = vir_mul(func, body[i], value, right);
                if (!value)
                    goto done;
            }
        }
    }
    body_edge.to = body[0];
    body_edge.args = NULL;
    body_edge.arg_count = 0;
    exit_edge.to = exit;
    exit_edge.args = NULL;
    exit_edge.arg_count = 0;
    if (!vir_edge_create(func, entry, header, NULL, 0) ||
        !vir_block_set_branch(func, header, condition, &body_edge, &exit_edge))
        goto done;
    for (int i = 0; i < value_blocks; i++) {
        vir_block_t *next = i + 1 < value_blocks ? body[i + 1] : header;

        if (!vir_edge_create(func, body[i], next, NULL, 0))
            goto done;
    }
    if (!vir_block_set_return(func, exit, left))
        goto done;
    if (vir_function_block_count(func) != value_blocks + 3)
        goto done;
    ok = true;

done:
    free(body);
    return ok;
}

static bool vir_profile_build_spread(vir_function_t *func,
                                     int values_per_block,
                                     int value_blocks)
{
    return vir_profile_build_spread_chain(func, values_per_block, value_blocks,
                                          1);
}

static bool vir_profile_build_sparse_spread(vir_function_t *func,
                                            int values_per_block,
                                            int value_blocks)
{
    return vir_profile_build_spread_chain(func, values_per_block, value_blocks,
                                          2);
}

/* Put one ready root at each sampled block, then bury it under a long chain.
 * The root is deliberately distributed rather than concentrated at the first
 * sampled location, while each 64-value sample remains mostly dependent.
 */
static bool vir_profile_build_distributed_roots(vir_function_t *func,
                                                int value_blocks,
                                                int chain_depth)
{
    vir_block_t **body = NULL;
    vir_block_t *entry;
    vir_block_t *header;
    vir_block_t *exit;
    vir_value_t *condition;
    vir_value_t *left;
    vir_value_t *right;
    vir_edge_args_t body_edge;
    vir_edge_args_t exit_edge;
    bool ok = false;

    body = calloc((size_t) value_blocks, sizeof(*body));
    if (!body)
        goto done;
    vir_function_init(func, 64);
    entry = vir_block_create(func);
    header = vir_block_create(func);
    exit = vir_block_create(func);
    if (!entry || !header || !exit)
        goto done;
    condition = vir_block_add_param(func, entry, VIR_TYPE_I1);
    left = vir_block_add_param(func, entry, VIR_TYPE_I32);
    right = vir_block_add_param(func, entry, VIR_TYPE_I32);
    if (!condition || !left || !right)
        goto done;
    for (int i = 0; i < value_blocks; i++) {
        vir_value_t *value;
        vir_value_t *constant;

        body[i] = vir_block_create(func);
        constant = vir_const_i32(func, entry, i + 1);
        value =
            body[i] && constant ? vir_add(func, body[i], left, constant) : NULL;
        if (!body[i] || !value)
            goto done;
        for (int j = 0; j < chain_depth; j++) {
            value = vir_mul(func, body[i], value, right);
            if (!value)
                goto done;
        }
    }
    body_edge.to = body[0];
    body_edge.args = NULL;
    body_edge.arg_count = 0;
    exit_edge.to = exit;
    exit_edge.args = NULL;
    exit_edge.arg_count = 0;
    if (!vir_edge_create(func, entry, header, NULL, 0) ||
        !vir_block_set_branch(func, header, condition, &body_edge, &exit_edge))
        goto done;
    for (int i = 0; i < value_blocks; i++) {
        vir_block_t *next = i + 1 < value_blocks ? body[i + 1] : header;

        if (!vir_edge_create(func, body[i], next, NULL, 0))
            goto done;
    }
    if (!vir_block_set_return(func, exit, left))
        goto done;
    ok = true;

done:
    free(body);
    return ok;
}

/* The reverse layout keeps the CFG and value graph unchanged but visits
 * dependent blocks before their loop-local roots during the eager scan.
 */
static bool vir_profile_build_dependency_order(vir_function_t *func,
                                               int value_blocks,
                                               int reverse_layout)
{
    vir_block_t **body = NULL;
    vir_value_t **roots = NULL;
    vir_block_t *entry;
    vir_block_t *header;
    vir_block_t *exit;
    vir_value_t *condition;
    vir_value_t *left;
    vir_value_t *right;
    vir_edge_args_t body_edge;
    vir_edge_args_t exit_edge;
    bool ok = false;

    body = calloc((size_t) value_blocks, sizeof(*body));
    roots = calloc((size_t) value_blocks, sizeof(*roots));
    if (!body || !roots)
        goto done;
    vir_function_init(func, 64);
    entry = vir_block_create(func);
    header = vir_block_create(func);
    exit = vir_block_create(func);
    if (!entry || !header || !exit)
        goto done;
    condition = vir_block_add_param(func, entry, VIR_TYPE_I1);
    left = vir_block_add_param(func, entry, VIR_TYPE_I32);
    right = vir_block_add_param(func, entry, VIR_TYPE_I32);
    if (!condition || !left || !right)
        goto done;
    for (int i = 0; i < value_blocks; i++) {
        int index = reverse_layout ? value_blocks - i - 1 : i;

        body[index] = vir_block_create(func);
        if (!body[index])
            goto done;
    }
    for (int i = 0; i < value_blocks; i++) {
        vir_value_t *constant = vir_const_i32(func, entry, i + 1);

        roots[i] = constant ? vir_add(func, body[i], left, constant) : NULL;
        if (!roots[i])
            goto done;
    }
    for (int i = 1; i < value_blocks; i++)
        if (!vir_mul(func, body[i], roots[i - 1], right))
            goto done;

    body_edge.to = body[0];
    body_edge.args = NULL;
    body_edge.arg_count = 0;
    exit_edge.to = exit;
    exit_edge.args = NULL;
    exit_edge.arg_count = 0;
    if (!vir_edge_create(func, entry, header, NULL, 0) ||
        !vir_block_set_branch(func, header, condition, &body_edge, &exit_edge))
        goto done;
    for (int i = 0; i < value_blocks; i++) {
        vir_block_t *next = i + 1 < value_blocks ? body[i + 1] : header;

        if (!vir_edge_create(func, body[i], next, NULL, 0))
            goto done;
    }
    if (!vir_block_set_return(func, exit, left))
        goto done;
    ok = true;

done:
    free(body);
    free(roots);
    return ok;
}

static bool vir_profile_integer_arg(const char *text, int *result, int minimum)
{
    char *end;
    long value = strtol(text, &end, 10);

    if (!text[0] || *end || value < minimum || value > 1000)
        return false;
    *result = (int) value;
    return true;
}

static void vir_profile_measure(const vir_profile_case_t *profile_case,
                                int samples,
                                int repeats)
{
    uint64_t *elapsed = malloc((size_t) samples * sizeof(*elapsed));
    vir_function_t *functions = malloc((size_t) repeats * sizeof(*functions));
    int *pass_stats =
        malloc((size_t) repeats * VIR_LICM_STAT_COUNT * sizeof(*pass_stats));
    vir_stats_t graph_stats;
    int expected_hoisted;
    int expected_loops;

    if (!elapsed || !functions || !pass_stats) {
        fprintf(stderr, "profile allocation failed\n");
        exit(EXIT_FAILURE);
    }
    expected_hoisted = profile_case->expected_hoisted;
    expected_loops = profile_case->expected_loops;
    for (int sample = 0; sample < samples; sample++) {
        uint64_t start;
        uint64_t finish;

        for (int repeat = 0; repeat < repeats; repeat++) {
            if (!profile_case->build(&functions[repeat], profile_case->size,
                                     profile_case->depth) ||
                !vir_verify(&functions[repeat], NULL)) {
                fprintf(stderr, "invalid %s graph at size %d\n",
                        profile_case->name, profile_case->size);
                exit(EXIT_FAILURE);
            }
        }
        if (sample == 0)
            vir_collect_stats(&functions[0], &graph_stats);
        start = vir_profile_now_ns();
        for (int repeat = 0; repeat < repeats; repeat++) {
            int *stats = &pass_stats[repeat * VIR_LICM_STAT_COUNT];
            int hoisted =
                vir_licm_with_stats(&functions[repeat], VIR_OPT_O2, stats);

            if (hoisted != expected_hoisted) {
                fprintf(stderr, "%s size %d: expected %d hoists, got %d\n",
                        profile_case->name, profile_case->size,
                        expected_hoisted, hoisted);
                exit(EXIT_FAILURE);
            }
        }
        finish = vir_profile_now_ns();
        elapsed[sample] = (finish - start) / (uint64_t) repeats;
        for (int repeat = 0; repeat < repeats; repeat++) {
            int *stats = &pass_stats[repeat * VIR_LICM_STAT_COUNT];

            if (stats[VIR_LICM_STAT_LOOPS] != expected_loops ||
                stats[VIR_LICM_STAT_VALUES_HOISTED] != expected_hoisted ||
                stats[VIR_LICM_STAT_READINESS_SAMPLE_VALUES] >
                    VIR_LICM_READINESS_SAMPLE_MAX_VALUES * expected_loops ||
                stats[VIR_LICM_STAT_READINESS_SAMPLE_VALUES] !=
                    profile_case->expected_readiness_sample_values ||
                stats[VIR_LICM_STAT_READINESS_EAGER_ADMISSIONS] !=
                    profile_case->expected_readiness_admissions ||
                (profile_case->expect_no_value_worklist &&
                 stats[VIR_LICM_STAT_VALUE_WORKLIST_ALLOCS] != 0) ||
                (!profile_case->expect_no_value_worklist &&
                 stats[VIR_LICM_STAT_VALUE_WORKLIST_ALLOCS] == 0) ||
                !vir_verify(&functions[repeat], NULL)) {
                fprintf(stderr, "%s size %d: invalid post-LICM graph\n",
                        profile_case->name, profile_case->size);
                exit(EXIT_FAILURE);
            }
            vir_function_release(&functions[repeat]);
        }
    }
    for (int i = 1; i < samples; i++) {
        uint64_t value = elapsed[i];
        int j = i;

        while (j > 0 && elapsed[j - 1] > value) {
            elapsed[j] = elapsed[j - 1];
            j--;
        }
        elapsed[j] = value;
    }
    double median_ns = (double) elapsed[samples / 2];

    if (!(samples % 2))
        median_ns = ((double) elapsed[samples / 2 - 1] + median_ns) / 2.0;
    printf("licm-profile case=%s size=%d depth=%d samples=%d repeats=%d ",
           profile_case->name, profile_case->size, profile_case->depth, samples,
           repeats);
    printf("blocks=%d values=%d loops=%d hoisted=%d temp_bytes=%d ",
           graph_stats.blocks, graph_stats.values,
           pass_stats[VIR_LICM_STAT_LOOPS],
           pass_stats[VIR_LICM_STAT_VALUES_HOISTED],
           pass_stats[VIR_LICM_STAT_TEMPORARY_BYTES]);
    printf("value_worklists=%d ",
           pass_stats[VIR_LICM_STAT_VALUE_WORKLIST_ALLOCS]);
    printf("readiness_sample_values=%d ",
           pass_stats[VIR_LICM_STAT_READINESS_SAMPLE_VALUES]);
    printf("readiness_eager_admissions=%d ",
           pass_stats[VIR_LICM_STAT_READINESS_EAGER_ADMISSIONS]);
    printf("median_wall_us_per_pass=%.3f\n", median_ns / 1000.0);
    free(pass_stats);
    free(functions);
    free(elapsed);
}

int main(int argc, char **argv)
{
    const vir_profile_case_t cases[] = {
        {"sparse_chain", vir_profile_build_sparse_chain, 3, 0, 3, 1, false, 0,
         0},
        {"sparse_chain", vir_profile_build_sparse_chain, 4, 0, 4, 1, false, 0,
         0},
        {"sparse_chain", vir_profile_build_sparse_chain, 8, 0, 8, 1, false, 0,
         0},
        {"sparse_chain", vir_profile_build_sparse_chain, 16, 0, 16, 1, false, 0,
         0},
        {"sparse_chain", vir_profile_build_sparse_chain, 32, 0, 32, 1, false, 0,
         0},
        {"sparse_chain", vir_profile_build_sparse_chain, 64, 0, 64, 1, false, 0,
         0},
        {"sparse_chain", vir_profile_build_sparse_chain, 128, 0, 128, 1, false,
         0, 0},
        {"sparse_chain", vir_profile_build_sparse_chain, 129, 0, 129, 1, false,
         0, 8},
        {"sparse_chain", vir_profile_build_sparse_chain, 256, 0, 256, 1, false,
         0, 8},
        {"sparse_chain", vir_profile_build_sparse_chain, 512, 0, 512, 1, false,
         0, 8},
        {"joins", vir_profile_build_joins, 4, 0, 8, 1, true, 0, 0},
        {"joins", vir_profile_build_joins, 8, 0, 16, 1, true, 0, 0},
        {"joins", vir_profile_build_joins, 16, 0, 32, 1, true, 0, 0},
        {"joins", vir_profile_build_joins, 32, 0, 64, 1, true, 0, 0},
        {"joins", vir_profile_build_joins, 64, 0, 128, 1, true, 0, 0},
        {"nested", vir_profile_build_nested, 64, 2, 64, 2, true, 0, 0},
        {"nested", vir_profile_build_nested, 64, 4, 64, 4, true, 0, 0},
        {"nested", vir_profile_build_nested, 64, 8, 64, 8, true, 0, 0},
        {"nested", vir_profile_build_nested, 128, 4, 128, 4, true, 0, 0},
        {"nested", vir_profile_build_nested, 256, 4, 256, 4, true, 0, 0},
        {"nested", vir_profile_build_nested, 512, 4, 512, 4, true, 0, 0},
        {"partial_fallback", vir_profile_build_partial_fallback, 32, 0, 64, 1,
         false, 0, 0},
        {"spread", vir_profile_build_spread, 1, 3, 6, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 8, 3, 48, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 32, 3, 192, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 8, 4, 64, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 32, 4, 256, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 8, 8, 128, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 32, 8, 512, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 8, 16, 256, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 32, 16, 1024, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 8, 32, 512, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 32, 32, 2048, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 8, 64, 1024, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 32, 64, 4096, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 8, 128, 2048, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 32, 128, 8192, 1, true, 0, 0},
        {"spread", vir_profile_build_spread, 1, 129, 258, 1, true, 1, 16},
        {"spread_below_half", vir_profile_build_sparse_spread, 1, 129, 387, 1,
         false, 0, 24},
        {"spread", vir_profile_build_spread, 8, 256, 4096, 1, true, 1, 128},
        {"spread", vir_profile_build_spread, 32, 256, 16384, 1, true, 1, 512},
        {"distributed_roots", vir_profile_build_distributed_roots, 256, 63,
         16384, 1, false, 0, 512},
        {"dependency_forward", vir_profile_build_dependency_order, 256, 0, 511,
         1, true, 1, 15},
        {"dependency_reverse", vir_profile_build_dependency_order, 256, 1, 511,
         1, false, 1, 15},
    };
    const char *case_name = NULL;
    int samples = 7;
    int repeats = 5;
    int case_size = -1;
    int case_depth = -1;

    if (argc > 6 || (argc > 3 && argc < 6) ||
        (argc > 1 && !vir_profile_integer_arg(argv[1], &samples, 1)) ||
        (argc > 2 && !vir_profile_integer_arg(argv[2], &repeats, 1)) ||
        (argc > 3 && (case_name = argv[3])[0] == '\0') ||
        (argc > 4 && !vir_profile_integer_arg(argv[4], &case_size, 1)) ||
        (argc > 5 && !vir_profile_integer_arg(argv[5], &case_depth, 0))) {
        fprintf(stderr,
                "usage: %s [samples [repeats [case-name size depth]]]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if (case_name) {
        const vir_profile_case_t *selected = NULL;

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            if (strcmp(cases[i].name, case_name) != 0 ||
                cases[i].size != case_size || cases[i].depth != case_depth)
                continue;
            if (selected) {
                fprintf(stderr, "profile case selector is not unique\n");
                return EXIT_FAILURE;
            }
            selected = &cases[i];
        }
        if (!selected) {
            fprintf(stderr, "no profile case matches the requested selector\n");
            return EXIT_FAILURE;
        }
        vir_profile_measure(selected, samples, repeats);
    } else {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            vir_profile_measure(&cases[i], samples, repeats);
    }
    return EXIT_SUCCESS;
}
