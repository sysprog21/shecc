int main(void)
{
    int values[2];
    int *last = values + 1;
    int *first = last - 1;

    *first = 29;
    return values[0];
}
