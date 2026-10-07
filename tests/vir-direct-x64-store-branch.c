int main(int argc)
{
    int value = 29;

    if (argc)
        return *(&value);
    return 7;
}
