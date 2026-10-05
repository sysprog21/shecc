int value = 7;
int *p = &value;
int **pp = &p;
int ***ppp = &pp;

int main(void)
{
    return ***ppp;
}
