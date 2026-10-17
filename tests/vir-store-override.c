/*
 * An inlined lookup leaves map[node->id] in a register whose reload the x64
 * backend elides through src0_override. The store behind it must record the
 * register it really wrote: recording the IR source made the NULL test after
 * the join read a register that never held the value, so an operand with id 0
 * looked missing.
 */
typedef struct node {
    int id;
    int kind;
    int type;
    int arity;
    struct node *first;
    struct node *second;
} node_t;
static void *lookup(void **map, const node_t *node)
{
    return node ? map[node->id] : 0;
}
void *build(void *a, void *b);
void *clone(const node_t *node, void **map)
{
    switch (node->kind) {
    case 0:
        return map[0];
    case 26:
        return map[1];
    default:
        if (node->arity < 1 || node->arity > 2 || !lookup(map, node->first) ||
            (node->arity == 2 && !lookup(map, node->second)))
            return 0;
        return build(map[node->first->id],
                     node->arity == 2 ? map[node->second->id] : 0);
    }
}
int built;
void *build(void *a, void *b)
{
    built++;
    return a;
}
int main(void)
{
    int x0, x1;
    void *map[4] = {&x0, &x1, 0, 0};
    node_t param = {0, 99, 6, 0, 0, 0}, index = {1, 99, 5, 0, 0, 0};
    node_t sum = {2, 17, 6, 2, &param, &index};
    node_t neg = {3, 18, 5, 1, &index, 0};
    if (clone(&sum, map) != &x0 || clone(&neg, map) != &x1 || built != 2)
        return 1;
    return 0;
}
