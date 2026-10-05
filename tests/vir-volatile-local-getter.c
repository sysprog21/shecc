typedef int (*callback_t)(int);
int increment(int value)
{
    return value + 1;
}
callback_t get(void)
{
    return increment;
}
int touch(void)
{
    typedef callback_t (*volatile getter_t)(void);
    getter_t getter = get;
    getter;
    return 0;
}
int main(void)
{
    return touch();
}
