typedef int *ip;
typedef const ip *(*get_t)(void);
int value;
int *pointer = &value;
const ip *get(void)
{
    return &pointer;
}
int main(void)
{
    get_t callback = get;
    *callback() = 0;
    return 0;
}
