int main(int argc)
{
    int a[2];
    int index = argc - 1;
    unsigned int unsigned_index = (unsigned int) index;
    int *p = a + index;
    int *q = a + unsigned_index;

    *p = 7;
    *q = 9;
    return *p - *q + 2;
}
