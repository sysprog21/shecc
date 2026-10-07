int main(void)
{
    int value;
    int *address = &value;

    value = 3;
    return *address;
}
