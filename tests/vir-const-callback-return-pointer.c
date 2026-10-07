typedef int *const *pointer_t;
typedef pointer_t (*getter_t)(void);
getter_t getter;
int main(void)
{
    *getter() = 0;
    return 0;
}
