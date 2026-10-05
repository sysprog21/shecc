int value = 7;
int *p = &value;
int **pp = &p;

int main(void)
{
    return **pp;
}
