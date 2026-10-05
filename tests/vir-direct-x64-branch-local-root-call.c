static int set_then_read(int *pointer, int value)
{
    *pointer = value;
    return *pointer;
}

int main(int argc)
{
    int left;
    int right;

    if (argc > 1)
        return set_then_read(&left, 17);
    return set_then_read(&right, 23);
}
