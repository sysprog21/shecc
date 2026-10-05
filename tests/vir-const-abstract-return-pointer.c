int x;
int *p = &x;
int **f(void)
{
    return &p;
}
int main(void)
{
    *((int *const *(*) (void) ) f)() = &x;
    return 0;
}
