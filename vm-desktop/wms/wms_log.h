/*
 * vmwm_log.h - 极简日志
 */
#ifndef VMWM_LOG_H
#define VMWM_LOG_H

#include <stdio.h>

#define vmwm_log(fmt, ...) \
    do { \
        fprintf(stderr, "[vmwm] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
    } while (0)

#endif /* VMWM_LOG_H */
