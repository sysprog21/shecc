volatile int sink;

int access_volatile(volatile int *address, int value)
{
    *address = value;
    return *address;
}

int main(int argc)
{
    return access_volatile(&sink, argc + 28);
}
