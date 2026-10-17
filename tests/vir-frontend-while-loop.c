int count_to(int limit);

int main(void)
{
    return count_to(9);
}
int count_to(int limit)
{
    int value = 0;

    while (value < limit)
        value = value + 1;
    return value;
}
