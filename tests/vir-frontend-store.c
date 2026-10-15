int store_then_load(int *pointer, int value)
{
    *pointer = value;
    return *pointer;
}

int main(void)
{
    int value = 0;

    return store_then_load(&value, 42);
}
