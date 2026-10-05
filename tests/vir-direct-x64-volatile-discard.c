int sink;

int main(int argc)
{
    volatile int *address = (volatile int *) &sink;

    *address = argc;
    *address;
    *address = argc + 28;
    return *address;
}
