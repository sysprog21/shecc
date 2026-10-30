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

    sink(ppp);
    ***ppp = argc + 50;
    return ***ppp;
}
void sink(int ***p)
{
    (void) p;
}
