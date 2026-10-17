int sink;

int access_mixed(int *address, int value);

int main(int argc)
{
    return access_mixed(&sink, argc + 28);
}
int access_mixed(int *address, int value)
{
    *address = value;
    value = *(volatile int *) address;
    *address = value + 1;
    return *address;
}
