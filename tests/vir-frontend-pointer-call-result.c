static int *identity_pointer(int *pointer)
{
    if (pointer == 0)
        return pointer;

    return pointer;
}

static int read_relay(int *pointer)
{
    return *identity_pointer(pointer);
}

static int value = 31;

int main(void)
{
    return read_relay(&value);
}
