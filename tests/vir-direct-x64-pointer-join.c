int main(int argc)
{
    int first = 0;
    int second = 0;
    int *p = &first;
    int *q = &second;

    if (argc > 1)
        q = p;
    *q = 7;
    return first + 2 * second;
}
