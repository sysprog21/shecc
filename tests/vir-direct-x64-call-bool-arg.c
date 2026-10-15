static _Bool helper(_Bool value)
{
    return value == 1;
}

int main(int argc)
{
    return helper(argc == 1) ? 0 : 1;
}
