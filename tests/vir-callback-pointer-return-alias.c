typedef int *pointer_t;
typedef pointer_t (*volatile callback_t)(int);
typedef pointer_t (*const constant_callback_t)(int);
static int values[2] = {17, 23};
int *get(int n)
{
    return values + n;
}
static callback_t callback;
static constant_callback_t constant_callback = get;
int touch(void)
{
    callback = get;
    callback;
    callback = get;
    return *(callback(0) + 1) != 23 || *(constant_callback(1)) != 23;
}
int main(void)
{
    return touch();
}
