typedef struct {
    int v1;
    int v2;
} foo_t;

foo_t spts[10];

int main()
{
    foo_t *s1 = spts + 5;
    foo_t *s2 = &spts[5];
    foo_t *s3 = spts + 3;
    foo_t *s4 = spts + 1;

    s3++;
    s3++;
    s4 += 4;

    printf("%d %d %d %d %d\n", spts, s1, s2, s3, s4);

    s1->v2 = 7;
    printf("%d %d %d %d\n", s1->v2, s2->v2, s3->v2, s4->v2);
    return 0;
}
