int main(int argc)
{
    int local = 0;
    int *p = &local;

    *p = argc + 4;
    return *p + 3;
}
