#include "peer.h"
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>

// Global state pointer for signal handler (see peer.h for definition of app_state_t struct
// which holds information about the peer that is running)
app_state_t *g_state = NULL;

// Used to catch SIGINT and SIGTERM signals and perform graceful shutdown via g_state used in a loop
void handle_signal(int sig) {
    if (g_state) {
        g_state->running = 0;
    }
    log_message("Received signal %d. Shutting down...", sig);
}

// Initialise application state
void init_app_state(app_state_t *state, const char *username) {
    // Init the struct to hold state with zeros
    memset(state, 0, sizeof(app_state_t));
    // Set initial states
    state->running = 1;
    state->tcp_socket = -1;
    state->udp_socket = -1;
    // Using strncpy to ensure the copy operation doesn't overflow the destination
    // strncopy will fill any remaining bytes in the destination with null char \0
    // and if the username is too big then 3rd param of sizeof state->username - 1 will limit
    // the user name to 1 char less that the destination with a null \0 at the end to denote end of string
    strncpy(state->username, username, sizeof(state->username) - 1);
    // Initialise a mutal exclusion object what will protect the peer list from concurrent access by multiple threads
    // so adding/removing peers is thread safe
    pthread_mutex_init(&state->peers_mutex, NULL);
    
    // Set global pointer for signal handler
    g_state = state;
    
    // Set up signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal; // method defined above
    sigaction(SIGINT, &sa, NULL); // Register a Ctrl+C
    sigaction(SIGTERM, &sa, NULL); // Register a kill command
}

// Called when something goes wrong or the application quits
// makes sure both sockets are closed properly if open
void cleanup_app_state(app_state_t *state) {
    if (state->tcp_socket >= 0) {
        close(state->tcp_socket);
        state->tcp_socket = -1;
    }
    
    if (state->udp_socket >= 0) {
        close(state->udp_socket);
        state->udp_socket = -1;
    }
    
    // Destroy the mutex that was used to protect the peer list from concurrent access.
    // This releases system resources associated with the mutex.
    // IMPORTANT: This should only be called after all threads that might use this mutex
    // have terminated, otherwise undefined behavior may occur if a thread attempts to
    // use the mutex after it's destroyed.
    pthread_mutex_destroy(&state->peers_mutex);
    log_message("Cleanup complete");
}

/*
 * This function creates a timestamped log entry and outputs it to the console.
 * It works similar to printf() but automatically adds a timestamp prefix in the
 * format [HH:MM:SS] and a newline character at the end of each message.
 * The function also ensures immediate output by flushing stdout.
*/
void log_message(const char *format, ...) {
    time_t now = time(NULL);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));
    
    printf("[%s] ", time_str);
    
    va_list args;
    va_start(args, format);
    // print out the args using the format string, like printf
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

/*
 * This function manages the application's peer list by either adding a new peer
 * or updating the "last seen" timestamp of an existing peer. It uses mutex locking
 * to ensure thread safety when multiple threads access the peer list concurrently.
 *
*/
int add_peer(app_state_t *state, const char *ip, const char *username) {
    // lock the peer list because it can be added to from both the listener and 
    // discovery threadsin network.c
    pthread_mutex_lock(&state->peers_mutex);
    
    // Check if peer already exists
    for (int i = 0; i < MAX_PEERS; i++) {
        if (state->peers[i].active && strcmp(state->peers[i].ip, ip) == 0) {
            // Update last seen time
            state->peers[i].last_seen = time(NULL);
            // Update username if provided
            if (username && username[0] != '\0') {
                strncpy(state->peers[i].username, username, sizeof(state->peers[i].username) - 1);
                state->peers[i].username[sizeof(state->peers[i].username) - 1] = '\0';
            }
            pthread_mutex_unlock(&state->peers_mutex);
            return 0; // Peer already exists
        }
    }
    
    // Find an empty slot
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!state->peers[i].active) {
            strncpy(state->peers[i].ip, ip, INET_ADDRSTRLEN);
            state->peers[i].last_seen = time(NULL);
            state->peers[i].active = 1;

            // Set username if provided
            if (username && username[0] != '\0') {
                strncpy(state->peers[i].username, username, sizeof(state->peers[i].username) - 1);
                state->peers[i].username[sizeof(state->peers[i].username) - 1] = '\0';
            }

            pthread_mutex_unlock(&state->peers_mutex);
            return 1; // New peer added
        }
    }
    
    // We'r edone with the peer list so unlock
    pthread_mutex_unlock(&state->peers_mutex);
    return -1; // No space for new peer
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

void *user_input_thread(void *arg) {
    app_state_t *state = (app_state_t *)arg;
    char input[BUFFER_SIZE];
    char message[BUFFER_SIZE];
    
    printf("\nCommands:\n");
    printf("  /list - List all active peers\n");
    printf("  /send <peer_number> <message> - Send message to specific peer\n");
    printf("  /broadcast <message> - Send message to all peers\n");
    printf("  /quit - Exit the application\n\n");
    
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
        
        if (strcmp(input, "/list") == 0) {
            print_peers(state);
        } 
        else if (strncmp(input, "/send ", 6) == 0) {
            int peer_num;
            char *msg_start = strchr(input + 6, ' ');
            
            if (msg_start == NULL) {
                printf("Usage: /send <peer_number> <message>\n");
                continue;
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
            break;
        }
        else {
            log_message("Unknown command. Type /list, /send, /broadcast, or /quit");
        }
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    app_state_t state;
    pthread_t listener_tid, discovery_tid, input_tid;
    char username[32] = "anonymous";
    
    // Get username if provided
    if (argc > 1) {
        strncpy(username, argv[1], sizeof(username) - 1);
    }
    
    // Initialize application state
    init_app_state(&state, username);
    
    log_message("Starting P2P messaging application as '%s'", username);
    
    // Initialize network components
    if (init_listener(&state) < 0 || init_discovery(&state) < 0) {
        log_message("Failed to initialize network components");
        cleanup_app_state(&state);
        return EXIT_FAILURE;
    }
    
    // Start threads
    if (pthread_create(&listener_tid, NULL, listener_thread, &state) != 0 ||
        pthread_create(&discovery_tid, NULL, discovery_thread, &state) != 0 ||
        pthread_create(&input_tid, NULL, user_input_thread, &state) != 0) {
        
        log_message("Failed to create threads");
        state.running = 0;

        // Since some threads may have been started and I'm not tracking return values from pthread_create() 
        // give any open threads a moment to notice the running flag change before the mutext
        // is destroyed by cleanup_app_state()
        usleep(100000);  // 100ms

        cleanup_app_state(&state);
        return EXIT_FAILURE;
    }
    
    // Wait for user input thread to finish (when user quits)
    pthread_join(input_tid, NULL);
    
    // Signal other threads to stop
    state.running = 0;
    
    // Wait for threads to finish
    pthread_join(listener_tid, NULL);
    pthread_join(discovery_tid, NULL);
    
    // Close sockets to unblock threads, must be called after all threads re-join main
    cleanup_app_state(&state);

    log_message("Application terminated");
    return EXIT_SUCCESS;
}