typedef int *(*volatile callback_t)(int);
static int value;
int *get(int n)
{
    value = n;
    return &value;
}
static callback_t callback;
int typedef_object(void)
{
    callback = get;
    callback;
    callback = get;
    return *callback(7);
}
int typedef_slot(void)
{
    callback_t *slot = &callback;
    *slot;
    return *(*slot)(8);
}
int typedef_outer_slot(void)
{
    callback_t *inner = &callback;
    callback_t **outer = &inner;
    *outer;
    return *(*(*outer))(10);
}
int direct_slot(void)
{
    int *(*volatile local)(int) = get;
    int *(*volatile * slot)(int) = &local;
    *slot;
    return *(*slot)(9);
}
int main(void)
{
    return typedef_object() != 7 || typedef_slot() != 8 ||
           typedef_outer_slot() != 10 || direct_slot() != 9;
}
