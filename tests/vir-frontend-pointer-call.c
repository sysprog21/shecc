static int read_nonnull(int *pointer)
{
    if (pointer == 0)
        return 0;

    return *pointer;
}

int relay_pointer(int *pointer)
{
    return read_nonnull(pointer);
}

static int value = 29;

int main(void)
{
    return relay_pointer(&value);
}
