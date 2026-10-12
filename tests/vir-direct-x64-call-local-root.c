static int local_value(void)
{
    int value = 29;
    int *address = &value;

    return *address;
}

int main(void)
{
    return local_value();
}
