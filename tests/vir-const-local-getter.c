typedef int (*callback_t)(int);
int increment(int value)
{
    return value + 1;
}
callback_t get(void)
{
    return increment;
}
int main(void)
{
    typedef callback_t (*const getter_t)(void);
    getter_t getter = get;
    getter = get;
    return 0;
}
