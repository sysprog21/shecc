volatile int left;
volatile int right;

int main(int argc)
{
    volatile int *address;

    if (argc)
        address = &left;
    else
        address = &right;
    *address = argc + 28;
    return *address;
}
