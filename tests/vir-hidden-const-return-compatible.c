typedef int *ip;
typedef const ip *(*get_t)(void);
int value = 9;
int *pointer = &value;
const ip *get(void)
{
    return &pointer;
}
get_t global_get = get;
const ip *(*declared_get)(void) = (const ip *(*) (void) ) get;
int main(void)
{
    typedef const ip *(*local_get_t)(void);
    get_t first = get;
    local_get_t second = get;
    get_t conditional = value ? (const ip *(*) (void) ) get : get;
    return **first() != 9 || **second() != 9 || **conditional() != 9 ||
           **global_get() != 9 || **declared_get() != 9;
}
