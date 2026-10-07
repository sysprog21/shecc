static int values[265] = {11};
extern int *wmemchr(const int *values, int value, unsigned long count);

#define STEP(p) ((p) + 1)
#define STEP_2(p) STEP(STEP(p))
#define STEP_4(p) STEP_2(STEP_2(p))
#define STEP_8(p) STEP_4(STEP_4(p))

int main(void)
{
    int *p0 = wmemchr(values, 11, 265);
    int *p8 = STEP_8(p0);
    int *p16 = STEP_8(p8);
    int *p24 = STEP_8(p16);
    int *p32 = STEP_8(p24);
    int *p40 = STEP_8(p32);
    int *p48 = STEP_8(p40);
    int *p56 = STEP_8(p48);
    int *p64 = STEP_8(p56);
    int *p72 = STEP_8(p64);
    int *p80 = STEP_8(p72);
    int *p88 = STEP_8(p80);
    int *p96 = STEP_8(p88);
    int *p104 = STEP_8(p96);
    int *p112 = STEP_8(p104);
    int *p120 = STEP_8(p112);
    int *p128 = STEP_8(p120);
    int *p136 = STEP_8(p128);
    int *p144 = STEP_8(p136);
    int *p152 = STEP_8(p144);
    int *p160 = STEP_8(p152);
    int *p168 = STEP_8(p160);
    int *p176 = STEP_8(p168);
    int *p184 = STEP_8(p176);
    int *p192 = STEP_8(p184);
    int *p200 = STEP_8(p192);
    int *p208 = STEP_8(p200);
    int *p216 = STEP_8(p208);
    int *p224 = STEP_8(p216);
    int *p232 = STEP_8(p224);
    int *p240 = STEP_8(p232);
    int *p248 = STEP_8(p240);
    int *p256 = STEP_8(p248);
    int *p264 = STEP_8(p256);

    return p264 != values + 264;
}
