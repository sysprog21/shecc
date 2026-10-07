int main(int argc)
{
    int a = (signed char) (argc + 254);
    int b = (unsigned char) (argc + 254);
    int c = (short) (argc + 65534);
    int d = (unsigned short) (argc + 65534);

    return (a == -1) + (b == 255) * 2 + (c == -1) * 4 + (d == 65535) * 8;
}
