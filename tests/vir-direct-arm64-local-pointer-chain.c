int main(int argc)
{
    int value = 29;
    int *p = &value;
    int **pp = &p;

    **pp = argc + 40;
    return **pp;
}
