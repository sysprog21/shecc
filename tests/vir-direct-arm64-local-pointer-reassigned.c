int main(int argc)
{
    int value = 29;
    int *p = &value;
    int **pp = &p;
    int ***ppp = &pp;
    int other = 31;
    int *q = &other;
    int **qq = &q;

    ppp = &qq;
    ***ppp = argc + 51;
    return ***ppp - value + 29;
}
