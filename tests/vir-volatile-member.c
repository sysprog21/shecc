struct item {
    int value;
};
struct owner {
    struct item *item;
    int *values;
};
static struct item item = {7};
static int values[1] = {11};
static volatile struct owner owner;

/* The pointer members are volatile objects. Their pointees are ordinary. */
int discard_pointees(void)
{
    owner.item->value;
    owner.values[0];
    return 0;
}
int main(void)
{
    owner.item = &item;
    owner.values = values;
    if (discard_pointees())
        return 1;
    return owner.item->value == 7 && owner.values[0] == 11 ? 0 : 2;
}
