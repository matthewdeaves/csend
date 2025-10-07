#include "test.h"
#include "logging.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

test_config_t get_default_test_config(void)
{
    test_config_t config;
    config.broadcast_count = 3;
    config.direct_per_peer = 3;
    config.test_rounds = 4;
    config.delay_ms = 2000;  /* 2 seconds between messages */
    return config;
}

int run_automated_test(const test_config_t *config, const test_callbacks_t *callbacks)
{
    int round, i, j;
    int peer_count;
    peer_t peer;
    char message[BUFFER_SIZE];
    int total_messages = 0;
    int failed_messages = 0;

    if (!config || !callbacks) {
        log_app_event("Test: Invalid config or callbacks");
        return -1;
    }

    /* Log test start with clear marker */
    log_app_event("========================================");
    log_app_event("AUTOMATED TEST START");
    log_app_event("Configuration: rounds=%d, broadcasts_per_round=%d, direct_per_peer=%d, delay=%dms",
                  config->test_rounds, config->broadcast_count, config->direct_per_peer, config->delay_ms);

    peer_count = callbacks->get_peer_count(callbacks->context);
    log_app_event("Test: Found %d active peer(s)", peer_count);

    if (peer_count == 0) {
        log_app_event("Test: No peers available - test aborted");
        log_app_event("AUTOMATED TEST END (ABORTED - NO PEERS)");
        log_app_event("========================================");
        return 0;
    }

    /* Run test rounds: each round does broadcasts then directs */
    for (round = 0; round < config->test_rounds; round++) {
        log_app_event("----------------------------------------");
        log_app_event("Test Round %d/%d START", round + 1, config->test_rounds);
        log_app_event("----------------------------------------");

        /* Phase 1 of this round: Broadcast messages */
        log_app_event("Test Round %d - Phase 1: Sending %d broadcast message(s)",
                      round + 1, config->broadcast_count);

        for (i = 0; i < config->broadcast_count; i++) {
            snprintf(message, sizeof(message), "TEST_R%d_BROADCAST_%d", round + 1, i + 1);

            log_app_event("Test Round %d: Broadcasting message %d/%d: '%s'",
                          round + 1, i + 1, config->broadcast_count, message);

            if (callbacks->send_broadcast(message, callbacks->context) != 0) {
                log_app_event("Test Round %d: Broadcast %d FAILED", round + 1, i + 1);
                failed_messages++;
            } else {
                log_app_event("Test Round %d: Broadcast %d sent successfully", round + 1, i + 1);
            }

            total_messages++;

            /* Delay between messages */
            if (i < config->broadcast_count - 1) {
                callbacks->delay_func(config->delay_ms, callbacks->context);
            }
        }

        /* Delay before direct messages */
        callbacks->delay_func(config->delay_ms, callbacks->context);

        /* Phase 2 of this round: Direct messages to each peer */
        log_app_event("Test Round %d - Phase 2: Sending %d direct message(s) to each peer",
                      round + 1, config->direct_per_peer);

        for (i = 0; i < peer_count; i++) {
            if (callbacks->get_peer_by_index(i, &peer, callbacks->context) != 0) {
                log_app_event("Test Round %d: Failed to get peer %d", round + 1, i);
                continue;
            }

            log_app_event("Test Round %d: Sending to peer %d: %s@%s",
                          round + 1, i + 1, peer.username, peer.ip);

            for (j = 0; j < config->direct_per_peer; j++) {
                snprintf(message, sizeof(message), "TEST_R%d_DIRECT_%d_TO_%s_MSG_%d",
                         round + 1, i + 1, peer.username, j + 1);

                log_app_event("Test Round %d: Direct message %d/%d to %s: '%s'",
                              round + 1, j + 1, config->direct_per_peer, peer.username, message);

                if (callbacks->send_direct(peer.ip, message, callbacks->context) != 0) {
                    log_app_event("Test Round %d: Direct message to %s FAILED",
                                  round + 1, peer.username);
                    failed_messages++;
                } else {
                    log_app_event("Test Round %d: Direct message to %s sent successfully",
                                  round + 1, peer.username);
                }

                total_messages++;

                /* Delay between messages */
                if (j < config->direct_per_peer - 1) {
                    callbacks->delay_func(config->delay_ms, callbacks->context);
                }
            }

            /* Delay before next peer */
            if (i < peer_count - 1) {
                callbacks->delay_func(config->delay_ms, callbacks->context);
            }
        }

        log_app_event("----------------------------------------");
        log_app_event("Test Round %d/%d COMPLETE", round + 1, config->test_rounds);
        log_app_event("----------------------------------------");

        /* Delay before next round */
        if (round < config->test_rounds - 1) {
            callbacks->delay_func(config->delay_ms * 2, callbacks->context);
        }
    }

    /* Log test completion */
    log_app_event("========================================");
    log_app_event("AUTOMATED TEST END");
    log_app_event("Test Summary: %d total messages, %d failed, %d succeeded",
                  total_messages, failed_messages, total_messages - failed_messages);
    log_app_event("========================================");

    return failed_messages == 0 ? 0 : -1;
}
