int load_indirect(int **address);

int main(void)
{
    int value = 29;
    int *pointer = &value;

    return load_indirect(&pointer);
}
int load_indirect(int **address)
{
    return **address;
}
