#include <stdio.h>
typedef struct Use Use;
typedef struct Value Value;
typedef struct Block Block;
struct Use {
    Use *next;
    Block *owner;
};
struct Value {
    Value *next, *param_next;
    Use *uses;
    int opcode;
};
struct Block {
    Value *params, *head;
};
int guard(Block *block)
{
    for (int list = 0; list < 2; list++)
        for (Value *value = list ? block->head : block->params; value;
             value = list ? value->next : value->param_next) {
            if (list && value->opcode != 1 && value->opcode != 2)
                return 0;
            for (Use *use = value->uses; use; use = use->next)
                if (use->owner != block)
                    return 0;
        }
    return 1;
}
int main(void)
{
    unsigned hash = 0;
    for (int n = 0; n < 256; n++) {
        Block block, other;
        Value params[3], head[3];
        Use uses[6][2];
        block.params = params;
        block.head = head;
        for (int i = 0; i < 6; i++) {
            Value *v = i < 3 ? params + i : head + i - 3;
            v->next = i >= 3 && i < 5 ? head + i - 2 : 0;
            v->param_next = i < 2 ? params + i + 1 : 0;
            v->opcode = 1 + (i & 1);
            v->uses = uses[i];
            uses[i][0].owner = &block;
            uses[i][0].next = uses[i] + 1;
            uses[i][1].next = 0;
            uses[i][1].owner = (n & (1 << i)) ? &other : &block;
        }
        if (n & 64)
            head[2].opcode = 3;
        int got = guard(&block), expected = (n & 127) == 0;
        if (got != expected) {
            printf("bad %d %d %d\n", n, got, expected);
            return 1;
        }
        hash = hash * 1664525u + (unsigned) got + 1013904223u;
    }
    if (hash != 2305299594u)
        return 2;
    int i = 0, effects = 0, body = 0;
    for (; i < 6; (i & 1) ? (effects += 3, i++) : (effects += 2, i++)) {
        if (i & 1)
            continue;
        body += i;
    }
    if (i != 6 || effects != 15 || body != 6)
        return 3;
    i = 0;
    effects = 0;
    body = 0;
    for (; i < 6; ((++i < 4) && (++effects)), effects += 2) {
        if (i & 1)
            continue;
        body += i;
    }
    if (i != 6 || effects != 15 || body != 6)
        return 4;
    i = 0;
    effects = 0;
    for (; i < 3; (++i == 2) || (++effects))
        continue;
    if (i != 3 || effects != 2)
        return 5;
    i = 0;
    effects = 0;
    for (;; (i ? ++effects : (effects += 2)), ++i)
        break;
    if (i || effects)
        return 6;
    return 0;
}
