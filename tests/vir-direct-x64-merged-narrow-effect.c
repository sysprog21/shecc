signed char left;
signed char right;

int main(int argc)
{
    signed char *address;
    int value;

    if (argc == 1) {
        address = &left;
        value = 129;
    } else {
        address = &right;
        value = 130;
    }
    *address = value;
    return *address;
}
