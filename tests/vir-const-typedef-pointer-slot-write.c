typedef int *pointer_t;
pointer_t *const slots[1] = {0};
int main(void)
{
    *slots = 0;
    return 0;
}
