unsigned int a[128];
unsigned int sink;
int main(void)
{
    unsigned int s = 0, x = 1;
    for (unsigned int k = 0; k < 20000; k++) {
        for (unsigned int i = 0; i < 128; i++) {
            x = x * 1664525u + 1013904223u;
            a[i] = x;
        }
        for (unsigned int i = 1; i < 128; i++) {
            unsigned int v = a[i], j = i;
            while (j > 0 && a[j - 1] > v) {
                a[j] = a[j - 1];
                j--;
            }
            a[j] = v;
        }
        for (unsigned int i = 0; i < 128; i++)
            s = s * 33u + a[i];
    }
    sink = s;
    return sink != 3540951488u;
}
