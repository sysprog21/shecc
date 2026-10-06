int *added_left = 1 + L"ab";
int *selected_true = 1 ? L"a" : L"b";
int *selected_false = 0 ? L"a" : L"b";
int *table[] = {L"ab" + 1};
int *added_right = L"ab" + 1;

int main(void)
{
    return added_left[0] != 'b' || selected_true[0] != 'a' ||
           selected_false[0] != 'b' || table[0][0] != 'b' ||
           added_right[0] != 'b';
}
