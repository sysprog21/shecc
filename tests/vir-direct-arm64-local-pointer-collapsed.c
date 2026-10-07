int main(int argc)
{
    int value = 29;
    int *p = &value;
    int **pp = &p;
    int ***ppp = &pp;
    int ***alias;

    if (argc > 1)
        alias = ppp;
    else
        alias = ppp;

    ***alias = argc + 51;
    return ***alias;
}
