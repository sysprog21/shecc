int main(int argc)
{
    int less_equal = argc <= 1;
    int greater_equal = argc >= 2;
    int unsigned_less_equal = (unsigned int) argc <= 1;
    int negative = argc - 3;
    int signed_negative = negative <= 1;
    int unsigned_negative = (unsigned int) negative <= 1;

    return less_equal + greater_equal * 2 + unsigned_less_equal * 4 +
           signed_negative * 8 + unsigned_negative * 16;
}
