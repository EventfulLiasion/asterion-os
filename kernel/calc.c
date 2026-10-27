#include <limits.h>
#include <stdint.h>
#include "calc.h"

static const char *skip_spaces(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

static calc_status_t parse_integer(const char **cursor, int64_t *value) {
    const char *p = skip_spaces(*cursor);
    int negative = 0;
    if (*p == '+' || *p == '-') { negative = (*p == '-'); ++p; }
    if (*p < '0' || *p > '9') return CALC_INVALID_EXPRESSION;

    const uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t magnitude = 0;
    do {
        uint32_t digit = (uint32_t)(*p - '0');
        if (magnitude > (limit - digit) / 10u) return CALC_OVERFLOW;
        magnitude = magnitude * 10u + digit;
        ++p;
    } while (*p >= '0' && *p <= '9');

    if (negative) {
        *value = magnitude == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)magnitude;
    } else {
        *value = (int64_t)magnitude;
    }
    *cursor = p;
    return CALC_OK;
}

static calc_status_t checked_add(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return CALC_OVERFLOW;
    *out = a + b;
    return CALC_OK;
}

static calc_status_t checked_subtract(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return CALC_OVERFLOW;
    *out = a - b;
    return CALC_OK;
}

static calc_status_t checked_multiply(int64_t a, int64_t b, int64_t *out) {
    if (a > 0) {
        if ((b > 0 && a > INT64_MAX / b) || (b < 0 && b < INT64_MIN / a)) return CALC_OVERFLOW;
    } else if (a < 0) {
        if ((b > 0 && a < INT64_MIN / b) || (b < 0 && b < INT64_MAX / a)) return CALC_OVERFLOW;
    }
    *out = a * b;
    return CALC_OK;
}

calc_status_t calc_evaluate(const char *expression, int64_t *result) {
    if (!expression || !result) return CALC_INVALID_EXPRESSION;
    const char *p = expression;
    int64_t left, right, value;
    calc_status_t status = parse_integer(&p, &left);
    if (status != CALC_OK) return status;
    p = skip_spaces(p);
    char op = *p++;
    if (op != '+' && op != '-' && op != '*' && op != '/') return CALC_INVALID_EXPRESSION;
    status = parse_integer(&p, &right);
    if (status != CALC_OK) return status;
    if (*skip_spaces(p) != '\0') return CALC_INVALID_EXPRESSION;

    switch (op) {
    case '+': status = checked_add(left, right, &value); break;
    case '-': status = checked_subtract(left, right, &value); break;
    case '*': status = checked_multiply(left, right, &value); break;
    case '/':
        if (!right) return CALC_DIVIDE_BY_ZERO;
        if (left == INT64_MIN && right == -1) return CALC_OVERFLOW;
        value = left / right;
        status = CALC_OK;
        break;
    default: return CALC_INVALID_EXPRESSION;
    }
    if (status == CALC_OK) *result = value;
    return status;
}
