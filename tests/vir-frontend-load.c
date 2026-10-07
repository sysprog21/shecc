int load_value(int *pointer)
{
    return *pointer;
}

int main(void)
{
    int value = 42;

    return load_value(&value);
}
