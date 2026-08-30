/*
 * utils.c - Utility functions
 *
 * Common helper functions used across the codebase.
 */

#include "unbridle.h"
#include <time.h>
#include <sys/time.h>

/*
 * get_time_ms - Get current time in milliseconds
 *
 * Returns: Current time as milliseconds since epoch (1970-01-01)
 *
 * Used for socket timeout tracking in socket_manager.c.
 * Resolution: microseconds (but returned as milliseconds)
 */
uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    /* Convert seconds to milliseconds and add microseconds converted to milliseconds */
    return (uint64_t)(tv.tv_sec) * 1000 + (uint64_t)(tv.tv_usec) / 1000;
}
