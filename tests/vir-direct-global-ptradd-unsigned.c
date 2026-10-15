int values[2];

int main(int argc)
{
    unsigned int index = (unsigned int) (argc - 1);
    int *p = values + index;

    *p = argc + 5;
    return values[index] + 2;
}
