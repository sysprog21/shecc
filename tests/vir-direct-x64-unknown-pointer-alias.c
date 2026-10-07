static int store_then_read(int *store_pointer, int *read_pointer)
{
    *store_pointer = 13;
    return *read_pointer;
}

static int first = 0;
static int second = 0;

int main(int argc)
{
    if (argc > 1)
        return store_then_read(&first, &first);
    return store_then_read(&first, &second);
}
