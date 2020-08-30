typedef enum { enum1 = 5, enum2 } enum_t;

typedef struct {
    int v1;
    char txt[10];
    char *ptr;
    enum_t v2;
} struct_t;

void setchar(char txt[], int i)
{
    txt[i] = '_';
}

void dump(int v)
{
    printf("%d\n", v);
}

int main(int argc, char *argv[])
{
    dump(2 * 3 + 4);
    dump((2 * 3) + 4);
    dump(2 * (3 + 4));
    dump(2 + 3 * 4);
    dump(2 + (3 * 4));
    dump((2 + 3) * 4);

    if (5 == 6 || 6 == 7) {
        printf("False\n");
    }
    if (5 != 6 && 6 != 7) {
        printf("True\n");
    }

    struct_t s1;
    s1.v1 = 5;
    s1.v1++;
    dump(s1.v1);

    struct_t *s2 = &s1;
    s2->v1++;
    dump(s2->v1);

    s1.txt[0] = 'X';
    s1.txt[1] = 'Y';
    s1.txt[2] = 'Z';
    s1.txt[3] = '\n';
    s1.txt[4] = 0;

    struct_t *s2 = &s1;
    s2->txt[2] = '!';

    setchar(s1.txt, 0);
    setchar(s2->txt, 1);

    printf(s1.txt);

    return 0;
}
