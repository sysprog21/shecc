typedef int (*fn_t)(int);
typedef fn_t row_t[2];
typedef row_t *rows_t[3];
typedef rows_t board_t[2];
typedef row_t matrix_t[2];
typedef matrix_t *matrices_t[3];
typedef row_t **chain_t[3];
typedef const fn_t const_row_t[2];
typedef const_row_t *const_rows_t[3];
typedef int scalar_row_t[2];
typedef scalar_row_t *scalar_rows_t[3];
int inc(int x)
{
    return x + 1;
}
int twice(int x)
{
    return x * 2;
}
row_t first = {inc, twice};
row_t second = {twice, inc};
rows_t global_rows = {&first, &second, &first};

int main(void)
{
    typedef fn_t local_row_t[2];
    typedef local_row_t *local_rows_t[3];
    typedef local_rows_t local_board_t[2];
    local_row_t a = {inc, twice};
    local_row_t b = {twice, inc};
    local_rows_t local_rows = {&a, &b, &a};
    fn_t *global = (fn_t *) global_rows[1];
    fn_t *local = (fn_t *) local_rows[0];
    return sizeof(*global_rows[0]) != 2 * sizeof(void *) ||
           sizeof(*local_rows[0]) != 2 * sizeof(void *) ||
           sizeof(matrices_t) != 3 * sizeof(void *) ||
           sizeof(matrix_t) != 4 * sizeof(void *) ||
           sizeof(global_rows) != 3 * sizeof(void *) ||
           sizeof(local_rows) != 3 * sizeof(void *) ||
           sizeof(board_t) != 6 * sizeof(void *) ||
           sizeof(local_board_t) != 6 * sizeof(void *) ||
           sizeof(chain_t) != 3 * sizeof(void *) ||
           sizeof(const_rows_t) != 3 * sizeof(void *) ||
           sizeof(scalar_rows_t) != 3 * sizeof(void *) || global[0](7) != 14 ||
           global[1](8) != 9 || local[0](4) != 5 || local[1](6) != 12;
}
