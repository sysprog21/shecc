int main(int argc)
{
    int local = 0;
    int *pointer = &local;
    long long value = 0;

    if (argc > 1)
        value = (long long) pointer;
    return value != 0;
}
