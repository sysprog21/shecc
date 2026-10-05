int initialized_local(void)
{
    int object = 42;
    int *pointer = &object;

    return *pointer;
}

int main(void)
{
    return initialized_local();
}
