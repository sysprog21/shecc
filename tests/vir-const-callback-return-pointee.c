typedef const int *pointer_t;
typedef pointer_t (*getter_t)(void);
getter_t getter;
int main(void)
{
    *getter() = 1;
    return 0;
}
