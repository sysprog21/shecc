static int backing[3] = {1, 2, 3};
static volatile int *p = backing;
int touch(void)
{
    *(p + 1);
    *(1 + p);
    *(p - (-1));
    return 0;
}
int main(void)
{
    return touch();
}
