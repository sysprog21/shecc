typedef int *P;
typedef int *volatile PV;
int main(void)
{
    int x = 1;
    volatile P p = &x, q = &x;
    PV a = &x, b = &x;
    p;
    q;
    a;
    b;
    return 0;
}
