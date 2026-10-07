int main(void)
{
    int values[2];
    signed char index = -1;
    int *last = values + 1;

    last[index] = 29;
    return values[0];
}
