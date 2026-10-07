int values[2];

int main(int argc)
{
    int index = argc - 1;
    int *p = values + index;

    *p = argc + 5;
    return values[index] + 2;
}
