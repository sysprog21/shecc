int main(void)
{
    int value = 0;

    *(&value) = 29;
    return value + 1;
}
