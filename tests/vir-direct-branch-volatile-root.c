int main(int argc)
{
    volatile int value;

    if (argc > 1)
        value = 29;
    else
        value = 31;
    return *(&value);
}
