unsigned short value = 0xff00;
unsigned short *p = &value;

int main(int argc)
{
    if (*p != 0xff00)
        return 0;
    *p = argc + 5;
    return *p;
}
