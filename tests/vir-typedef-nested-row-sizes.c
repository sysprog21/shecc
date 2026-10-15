typedef int int4_t[4];
typedef int4_t row_t[2];
typedef row_t *rows_t[3];
typedef const row_t *const_rows_t[3];
row_t global_row;
rows_t global_rows;
const row_t const_row;
const_rows_t qualified_rows;

int main(void)
{
    row_t local_row;
    rows_t local_rows;
    const_rows_t local_qualified;
    return sizeof(row_t) != 8 * sizeof(int) ||
           sizeof(rows_t) != 3 * sizeof(void *) ||
           sizeof(global_row) != sizeof(local_row) ||
           sizeof(*global_rows[0]) != sizeof(row_t) ||
           sizeof(*local_rows[0]) != sizeof(row_t) ||
           sizeof(*qualified_rows[0]) != sizeof(row_t) ||
           sizeof(*local_qualified[0]) != sizeof(row_t) ||
           sizeof((*global_rows[0])[0]) != sizeof(int4_t) ||
           sizeof((*local_rows[0])[0]) != sizeof(int4_t);
}
