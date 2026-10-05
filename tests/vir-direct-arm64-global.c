int first;
int value = 7;

int main(int argc)
{
    int *p = &value;
    int *q = &first;
    int initial = *p;

    *p = argc + 5;
    *q = argc + 1;
    return initial + *p + *q;
}
