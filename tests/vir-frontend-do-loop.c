int count_to(int limit);

int main(void)
{
    return count_to(9);
}
int count_to(int limit)
{
    int value = 0;

    do {
        value = value + 1;
    } while (value < limit);
    return value;
}
