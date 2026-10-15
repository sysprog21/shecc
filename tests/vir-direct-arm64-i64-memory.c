int main(int argc)
{
    volatile long long value = 0;
    volatile long long *pointer = &value;

    *pointer = (long long) argc;
    return *pointer != (long long) argc ? 1 : 0;
}
