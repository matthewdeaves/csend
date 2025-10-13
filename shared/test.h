#ifndef TEST_H
#define TEST_H

#include "peer.h"

/* For clock_t on non-Mac platforms */
#ifndef __CLASSIC_MAC__
#include <time.h>
#endif

/* Test configuration */
typedef struct {
    int broadcast_count;
    int direct_per_peer;
    int test_rounds;
    int delay_ms;
} test_config_t;

/* Test callbacks - platform provides these */
typedef struct {
    int (*send_broadcast)(const char *message, void *context);
    int (*send_direct)(const char *peer_ip, const char *message, void *context);
    int (*get_peer_count)(void *context);
    int (*get_peer_by_index)(int index, peer_t *out_peer, void *context);
    void *context;
} test_callbacks_t;

/* Get default test configuration */
test_config_t get_default_test_config(void);

/* --- Asynchronous Test API --- */

int start_automated_test(const test_config_t *config, const test_callbacks_t *callbacks);
void process_automated_test(void);
void stop_automated_test(void);

/**
 * @brief Checks if the automated test is currently running.
 *
 * @return 1 if the test is running, 0 otherwise.
 */
int is_automated_test_running(void);

#endif /* TEST_H */