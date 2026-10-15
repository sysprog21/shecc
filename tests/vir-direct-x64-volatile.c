int sink;

int main(int argc)
{
    volatile int *address = (volatile int *) &sink;

    *address = argc + 28;
    return *address;
}
