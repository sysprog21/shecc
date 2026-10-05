int main(int argc)
{
    int value = 0;

    *(&value) = 29;
    if (argc - 1)
        return value;
    return value + 1;
}
