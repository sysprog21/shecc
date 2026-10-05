int main(int argc)
{
    int value = 29;
    int *p = &value;
    int **pp = &p;
    int ***ppp = &pp;
    int ****pppp = &ppp;
    int *****p5 = &pppp;

    *****p5 = argc + 51;
    return *****p5;
}
