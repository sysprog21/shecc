int main(int argc)
{
    int value = 29;
    int *p = &value;
    int **pp = &p;
    int ***ppp = &pp;
    int ****pppp = &ppp;

    if (argc)
        pppp = &ppp;
    else
        pppp = &ppp;
    ****pppp = argc + 51;
    return ****pppp;
}
