/*
 * A table indexed in one arm of a conditional, where the index is read only
 * once the whole function has been seen: the table's address is bound where it
 * decays so the deferred add can use it. Each entry holds a pointer, so it is
 * 16 bytes wide on an LP64 target.
 */
typedef struct {
    const char *name;
    int width;
} entry_t;

static const entry_t table[] = {{"a", 10}, {"b", 20}, {"c", 30}, {"d", 40}};

int pick(int t)
{
    return (unsigned int) t < sizeof(table) / sizeof(*table) ? table[t].width
                                                             : 0;
}

int main()
{
    return pick(1) + pick(3) + pick(4) + pick(-1);
}
