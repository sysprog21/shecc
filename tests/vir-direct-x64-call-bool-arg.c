static _Bool helper(_Bool value);

int main(int argc)
{
    return helper(argc == 1) ? 0 : 1;
}
static _Bool helper(_Bool value)
{
    return value == 1;
}
