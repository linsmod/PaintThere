/*
 * vmwm_log.h - 极简日志
 */
#ifndef VMWM_LOG_H
#define VMWM_LOG_H

#include <stdio.h>
#include <uv.h>

#define vmwm_log(fmt, ...) \
    do { \
        fprintf(stderr, "[vmwm %5u] " fmt "\n", \
                (unsigned)(uv_hrtime() / 1000000ull) % 100000u, ##__VA_ARGS__); \
        fflush(stderr); \
    } while (0)

#endif /* VMWM_LOG_H */
