/* Defined after main, so the call stays: it can reach every slot through ppp,
 * which keeps the stores to them and the loads that follow.
 */
void sink(int ***p);
int main(int argc)
{
    int value = 29;
    int *p = &value;
    int **pp = &p;
    int ***ppp = &pp;
    int ***alias;

    sink(ppp);
    if (argc > 1)
        alias = ppp;
    else
        alias = ppp;

    ***alias = argc + 51;
    return ***alias;
}
void sink(int ***p)
{
    (void) p;
}
