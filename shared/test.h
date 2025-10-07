#ifndef TEST_H
#define TEST_H

#include "peer.h"

/* Test configuration */
typedef struct {
    int broadcast_count;      /* Number of broadcasts per round (default: 2) */
    int direct_per_peer;      /* Messages per peer per round (default: 2) */
    int test_rounds;          /* Number of test rounds (default: 2) */
    int delay_ms;             /* Delay between messages in ms (default: 4000) */
} test_config_t;

/* Test callbacks - platform provides these */
typedef struct {
    /* Send a broadcast message */
    int (*send_broadcast)(const char *message, void *context);

    /* Send direct message to specific peer IP */
    int (*send_direct)(const char *peer_ip, const char *message, void *context);

    /* Get active peer count */
    int (*get_peer_count)(void *context);

    /* Get peer by index (0-based) */
    int (*get_peer_by_index)(int index, peer_t *out_peer, void *context);

    /* Delay function (platform-specific) */
    void (*delay_func)(int milliseconds, void *context);

    /* Platform-specific context */
    void *context;
} test_callbacks_t;

/* Run automated test sequence */
int run_automated_test(const test_config_t *config, const test_callbacks_t *callbacks);

/* Get default test configuration */
test_config_t get_default_test_config(void);

#endif /* TEST_H */
