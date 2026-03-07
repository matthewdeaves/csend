#ifndef TEST_H
#define TEST_H

#include "common_defs.h"
#include <stddef.h>

#ifndef __CLASSIC_MAC__
#include <time.h>
#endif

typedef struct {
    int broadcast_count;
    int direct_per_peer;
    int test_rounds;
    int delay_ms;
} test_config_t;

/* Test callbacks - platform provides these.
 * send_direct uses connected peer index (0-based), not IP. */
typedef struct {
    int (*send_broadcast)(const char *message, void *context);
    int (*send_direct)(int peer_index, const char *message, void *context);
    int (*get_peer_count)(void *context);
    int (*get_peer_info)(int index, char *name_buf, size_t name_size,
                         char *addr_buf, size_t addr_size, void *context);
    void *context;
} test_callbacks_t;

test_config_t get_default_test_config(void);

int start_automated_test(const test_config_t *config, const test_callbacks_t *callbacks);
void process_automated_test(void);
void stop_automated_test(void);
int is_automated_test_running(void);

#endif
