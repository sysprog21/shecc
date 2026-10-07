int main(void)
{
    typedef int *P;
    typedef volatile int *VP;
    typedef int *volatile PV;
    int x = 1;
    volatile int y = 2;
    int *volatile own = &x;
    volatile P aliasown = &x;
    PV aliasstar = &x;
    volatile int *pointee = &y;
    VP aliaspointee = &y;
    own;
    aliasown;
    aliasstar;
    *pointee;
    *aliaspointee;
    return 0;
}
