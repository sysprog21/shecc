int left;
int right;

int main(int argc)
{
    int *address;

    if (argc)
        address = &left;
    else
        address = &right;
    *address = argc + 28;
    return *address;
}
