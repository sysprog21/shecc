int main(int argc)
{
    int value = 29;
    int *volatile p = &value;
    int *volatile *pp = &p;
    int *volatile **ppp = &pp;

    ***ppp = argc + 51;
    return ***ppp;
}
