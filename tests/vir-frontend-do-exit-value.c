int last_before(int limit)
{
    int value;
    int index = 0;

    do {
        value = index;
        index = index + 1;
    } while (index < limit);
    return value;
}

int main(void)
{
    return last_before(4);
}
