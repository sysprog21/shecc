/*
 * Defined after main, so the call stays; the escaped array keeps its store and
 * its reload.
 */
void sink(int *p);
int main(int argc)
{
    int a[2];
    int *p = a + 1;

    *p = argc + 40;
    sink(a);
    return a[1] + 2;
}
void sink(int *p)
{
    (void) p;
}
