int load_indirect(int **address)
{
    return **address;
}

int main(void)
{
    int value = 29;
    int *pointer = &value;

    return load_indirect(&pointer);
}
