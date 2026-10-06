unsigned int data[1024];
unsigned int sink;
int main(void)
{
    for (unsigned int i = 0; i < 1024; i++)
        data[i] = i * 3 + 1;
    unsigned int s = 0;
    for (unsigned int k = 0; k < 40000; k++) {
        unsigned int *p = data;
        for (unsigned int i = 0; i < 1024; i++)
            s += *p++;
    }
    sink = s;
    return sink != 2764537856u;
}
