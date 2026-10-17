int global_object;

int store_local(int value);
int store_global(int value);


int main(void)
{
    return store_local(17) + store_global(25);
}
int store_local(int value)
{
    int object;
    int *pointer = &object;

    *pointer = value;
    return *pointer;
}
int store_global(int value)
{
    int *pointer = &global_object;

    *pointer = value;
    return *pointer;
}
