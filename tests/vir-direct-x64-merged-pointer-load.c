int first;
int second;
int *left = &first;
int *right = &second;

int main(int argc)
{
    int **address;

    if (argc)
        address = &left;
    else
        address = &right;
    **address = argc + 28;
    return **address;
}
