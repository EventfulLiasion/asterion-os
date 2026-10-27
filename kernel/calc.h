#ifndef ASTERION_CALC_H
#define ASTERION_CALC_H

#include <stdint.h>

typedef enum {
    CALC_OK,
    CALC_INVALID_EXPRESSION,
    CALC_DIVIDE_BY_ZERO,
    CALC_OVERFLOW
} calc_status_t;

calc_status_t calc_evaluate(const char *expression, int64_t *result);

#endif
