#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include "../kernel/calc.h"

static void expect(const char *expr, calc_status_t status, int64_t expected) {
    int64_t actual = 123;
    assert(calc_evaluate(expr, &actual) == status);
    if (status == CALC_OK) assert(actual == expected);
}

int main(void) {
    expect("12*4", CALC_OK, 48);
    expect(" +2 * -4 \t", CALC_OK, -8);
    expect("-9223372036854775808 + 0", CALC_OK, INT64_MIN);
    expect("9223372036854775807 + 0", CALC_OK, INT64_MAX);
    expect("-9223372036854775808 / 1", CALC_OK, INT64_MIN);
    expect("-0-0", CALC_OK, 0);

    expect("9223372036854775808+0", CALC_OVERFLOW, 0);
    expect("-9223372036854775809+0", CALC_OVERFLOW, 0);
    expect("9223372036854775807+1", CALC_OVERFLOW, 0);
    expect("-9223372036854775808-1", CALC_OVERFLOW, 0);
    expect("3037000500*3037000500", CALC_OVERFLOW, 0);
    expect("-9223372036854775808/-1", CALC_OVERFLOW, 0);
    expect("1/0", CALC_DIVIDE_BY_ZERO, 0);

    expect("", CALC_INVALID_EXPRESSION, 0);
    expect("12+", CALC_INVALID_EXPRESSION, 0);
    expect("12+3junk", CALC_INVALID_EXPRESSION, 0);
    expect("12x3", CALC_INVALID_EXPRESSION, 0);
    assert(calc_evaluate(0, &(int64_t){0}) == CALC_INVALID_EXPRESSION);
    assert(calc_evaluate("1+1", 0) == CALC_INVALID_EXPRESSION);
    return 0;
}
