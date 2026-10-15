int main(void)
{
    int values[2];
    unsigned int index = 1U;
    int *last = values + 1;
    int *first = last - index;

    *first = 29;
    return values[0];
}
