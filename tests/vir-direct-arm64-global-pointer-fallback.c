int value = 7;
int *p = &value;

int main(int argc)
{
    int initial = *p;

    *p = argc + 5;
    return initial + *p;
}
