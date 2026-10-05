int main(int argc)
{
    int value;

    if (argc > 1) {
        value = 17;
        value = 29;
    } else {
        value = 23;
        value = 31;
    }
    return *(&value);
}
