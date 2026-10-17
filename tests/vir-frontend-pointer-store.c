int store_pointer(int **address, int *value);

int main(void)
{
    int value = 29;
    int *pointer = 0;

    return store_pointer(&pointer, &value);
}
int store_pointer(int **address, int *value)
{
    *address = value;
    return **address;
}
