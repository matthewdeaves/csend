#include "ui_terminal.h"
#include "peer.h"
#include <stdio.h>
#include <string.h>

void print_help_message(void) {
    printf("\nCommands:\n");
    printf("  /list - List all active peers\n");
    printf("  /send <peer_number> <message> - Send message to specific peer\n");
    printf("  /broadcast <message> - Send message to all peers\n");
    printf("  /quit - Exit the application\n");
    printf("  /help - Show this help message\n\n");
}

void print_peers(app_state_t *state) {
    // Lock the peers list whilst we use it
    pthread_mutex_lock(&state->peers_mutex);
    
    time_t now = time(NULL);
    int active_count = 0;
    
    printf("\n--- Active Peers ---\n");
    for (int i = 0; i < MAX_PEERS; i++) {
        if (state->peers[i].active) {
            // Check if peer has timed out
            if (now - state->peers[i].last_seen > PEER_TIMEOUT) {
                state->peers[i].active = 0;
                continue;
            }
            
            printf("%d. %s@%s (last seen %ld seconds ago)\n", 
                   ++active_count,
                   state->peers[i].username,
                   state->peers[i].ip, 
                   now - state->peers[i].last_seen);
        }
    }
    
    if (active_count == 0) {
        printf("No active peers found\n");
    }
    printf("------------------\n");
    
    pthread_mutex_unlock(&state->peers_mutex);
}

int handle_command(app_state_t *state, const char *input) {
    if (strcmp(input, "/list") == 0) {
        print_peers(state);
        return 0;
    } 
    else if (strcmp(input, "/help") == 0) {
        print_help_message();
        return 0;
    }
    else if (strncmp(input, "/send ", 6) == 0) {
        int peer_num;
        char *msg_start = strchr(input + 6, ' ');
        
        if (msg_start == NULL) {
            printf("Usage: /send <peer_number> <message>\n");
            return 0;
        }
        
        *msg_start = '\0';
        peer_num = atoi(input + 6);
        msg_start++;
        
        pthread_mutex_lock(&state->peers_mutex);
        
        int count = 0;
        int found = 0;
        char target_ip[INET_ADDRSTRLEN];
        
        for (int i = 0; i < MAX_PEERS; i++) {
            if (state->peers[i].active) {
                count++;
                if (count == peer_num) {
                    strcpy(target_ip, state->peers[i].ip);
                    found = 1;
                    break;
                }
            }
        }
        
        pthread_mutex_unlock(&state->peers_mutex);
        
        if (found) {
            if (send_message(target_ip, msg_start, MSG_TEXT) < 0) {
                log_message("Failed to send message to %s", target_ip);
            } else {
                log_message("Message sent to %s", target_ip);
            }
        } else {
            log_message("Invalid peer number");
        }
        return 0;
    }
    else if (strncmp(input, "/broadcast ", 11) == 0) {
        pthread_mutex_lock(&state->peers_mutex);
        
        for (int i = 0; i < MAX_PEERS; i++) {
            if (state->peers[i].active) {
                if (send_message(state->peers[i].ip, input + 11, MSG_TEXT) < 0) {
                    log_message("Failed to send message to %s", state->peers[i].ip);
                }
            }
        }
        
        pthread_mutex_unlock(&state->peers_mutex);
        log_message("Broadcast message sent");
        return 0;
    }
    else if (strcmp(input, "/quit") == 0) {
        // Send quit message to all peers before exiting
        pthread_mutex_lock(&state->peers_mutex);
        
        for (int i = 0; i < MAX_PEERS; i++) {
            if (state->peers[i].active) {
                // Send quit notification to each peer
                if (send_message(state->peers[i].ip, "", MSG_QUIT) < 0) {
                    log_message("Failed to send quit notification to %s", state->peers[i].ip);
                }
            }
        }
        
        pthread_mutex_unlock(&state->peers_mutex);
        log_message("Quit notifications sent to all peers");
        
        // Set running flag to false to exit the application
        state->running = 0;
        return 1; // Signal to exit
    }
    else {
        log_message("Unknown command. Type /help for available commands");
        return 0;
    }
}

void *user_input_thread(void *arg) {
    app_state_t *state = (app_state_t *)arg;
    char input[BUFFER_SIZE];
    
    print_help_message();
    
    while (state->running) {
        printf("> ");
        fflush(stdout);
        
        if (fgets(input, BUFFER_SIZE, stdin) == NULL) {
            if (state->running) {
                log_message("Error reading input");
            }
            break;
        }
        
        // Remove newline
        input[strcspn(input, "\n")] = 0;
        
        if (strlen(input) == 0) {
            continue;
        }
        
        if (handle_command(state, input)) {
            break; // Exit command was processed
        }
    }
    
    return NULL;
}