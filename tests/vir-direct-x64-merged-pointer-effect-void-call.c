int left;
int right;
int result;

static void set_result(int *address)
{
    result = *address;
}

int main(int argc)
{
    int *address;

    if (argc)
        address = &left;
    else
        address = &right;
    *address = argc + 28;
    set_result(address);
    return result;
}
