int values[2];

int main(int argc)
{
    int *p = values + 1;

    *p = argc + 5;
    return values[1] + 2;
}
