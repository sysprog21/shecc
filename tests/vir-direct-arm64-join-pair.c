int main(int argc)
{
    int left;
    int right;

    if (argc == 2) {
        left = 29;
        right = 30;
    } else {
        left = 30;
        right = 29;
    }
    return left * 2 + right;
}
