int main(void)
{
    int values[8] = {0};
    int *p = values;
    int *end = values + 8;

    while (p != end) {
        *p = p - values;
        p++;
    }

    return values[3] + values[7];
}
