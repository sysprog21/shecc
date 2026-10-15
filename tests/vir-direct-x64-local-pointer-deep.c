int main(int argc)
{
    int value = 29;
    int *p = &value;
    int **pp = &p;
    int ***ppp = &pp;

    ***ppp = argc + 50;
    return ***ppp;
}
