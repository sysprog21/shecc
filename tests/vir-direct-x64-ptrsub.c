int main(int argc, char **argv)
{
    int values[2];
    int *last = values + 1;
    int *first = last - argc;

    *first = 29;
    return values[0];
}
