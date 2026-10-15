struct holder {
    volatile int *p;
    int *volatile q;
};
static int x = 3;
static volatile int y = 4;
static struct holder h;
int touch(void)
{
    *h.p;
    *h.q;
    return 0;
}
int main(void)
{
    h.p = &y;
    h.q = &x;
    return touch();
}
