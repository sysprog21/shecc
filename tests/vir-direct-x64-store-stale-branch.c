/* Volatile, or the slot is never read after forwarding and the stores go. */
int main(int argc)
{
    volatile int value = 0;

    if (argc)
        return value;
    *(&value) = 29;
    return *(&value);
}
