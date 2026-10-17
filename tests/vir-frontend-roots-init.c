int initialized_local(void);

int main(void)
{
    return initialized_local();
}
int initialized_local(void)
{
    int object = 42;
    int *pointer = &object;

    return *pointer;
}
