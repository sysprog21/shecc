int main(int argc)
{
    int value = 0;

    if (argc)
        return value;
    *(&value) = 29;
    return *(&value);
}
