int count_to(int limit)
{
    int value = 0;

    for (; value < limit; value = value + 1)
        ;
    return value;
}

int main(void)
{
    return count_to(9);
}
