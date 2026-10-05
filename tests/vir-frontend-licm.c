int hoist(int limit, int a, int b)
{
    int value = 0;

    while (value < limit)
        value = value + a * b;
    return value;
}

int main(void)
{
    return hoist(3, 4, 5);
}
