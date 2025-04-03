/**
 * @file ui_terminal.c
 * @brief Terminal user interface implementation for the P2P messaging application.
 * @details This file contains the functions responsible for interacting with the user
 *          via the command line (terminal). It handles reading user input, parsing commands,
 *          displaying information like the peer list and help messages, and triggering
 *          actions like sending messages based on user commands. It runs in its own dedicated thread.
 */

// Include the header file for this module, which declares the functions defined here.
// It also includes peer.h, providing access to app_state_t and related definitions.
#include "ui_terminal.h"

// Include necessary headers from other modules.
#include "network.h"     // Needed for send_message() to send messages over TCP.
#include "utils.h"       // Needed for log_message() for displaying status/error messages.
#include "../shared/protocol.h"   // Needed for message type constants like MSG_TEXT and MSG_QUIT.
#include "messaging.h"   // Needed for send_message()

// --- Standard Library Includes ---
#include <stdio.h>       // For standard input/output functions: fgets (read input), printf (display output), stdout, fflush (ensure output is displayed promptly).
#include <string.h>      // For string manipulation functions: strlen, strncmp (compare commands), strcmp, strcspn (remove newline), strcpy, strchr (find space in /send).
#include <time.h>        // For time() to get the current time, used in print_peers for checking timeouts.
#include <pthread.h>     // For thread synchronization primitives: pthread_mutex_lock() and pthread_mutex_unlock() to safely access the shared peer list.
#include <stdlib.h>      // For atoi() to convert the peer number string to an integer in the /send command.
#include <sys/select.h> // For select() system call and related macros (fd_set, FD_ZERO, FD_SET)
#include <unistd.h>     // For STDIN_FILENO constant (standard input file descriptor, usually 0)
#include <errno.h>      // For errno variable and error constants like EINTR

/**
 * @brief Displays a help message listing available user commands to standard output.
 * @details Prints a clear, formatted list of commands the user can type, along with brief descriptions
 *          of what each command does. This function is typically called when the application starts
 *          or when the user explicitly types `/help`.
 */
 void print_help_message(void) {
     // Print a header for the commands section.
     printf("\nCommands:\n");
     // Print each command and its description on a new line, indented for readability.
     printf("  /list                       - List all active peers\n");
     printf("  /send <peer_number> <msg> - Send <msg> to a specific peer from the list\n");
     printf("  /broadcast <message>        - Send <message> to all active peers\n");
     printf("  /quit                       - Send quit notification and exit the application\n");
     printf("  /help                       - Show this help message\n\n");
     // Ensure the output buffer is flushed so the user sees the message immediately.
     fflush(stdout);
 }

 /**
  * @brief Prints the list of currently known and active peers to standard output.
  * @details This function iterates through the `peers` array in the application state.
  *          It acquires a mutex lock (`peers_mutex`) before accessing the list to ensure
  *          thread safety, as other threads (discovery, listener) might modify the list concurrently.
  *          For each peer marked as `active`, it checks if the peer has timed out based on
  *          `last_seen` and `PEER_TIMEOUT`. If timed out, the peer is marked as inactive.
  *          If still active, the peer's number (in the current list display), username, IP address,
  *          and time since last seen are printed. The mutex is released after iterating through the list.
  * @param state Pointer to the application state (`app_state_t`) containing the `peers` array and `peers_mutex`.
  */
 void print_peers(app_state_t *state) {
     // Acquire the mutex lock to safely access the shared peers array.
     // This blocks if another thread currently holds the lock.
     pthread_mutex_lock(&state->peers_mutex);

     // Get the current time to compare against peer last_seen times.
     time_t now = time(NULL);
     // Counter for numbering the active peers displayed in the list.
     int active_count = 0;

     // Print a header for the peer list.
     printf("\n--- Active Peers ---\n");
     // Iterate through the entire peers array.
     for (int i = 0; i < MAX_PEERS; i++) {
         // Check if the peer slot is currently marked as active.
         if (state->peers[i].active) {
             // Check if the time elapsed since the peer was last seen exceeds the timeout threshold.
             // difftime(now, state->peers[i].last_seen) is arguably safer than direct subtraction for time_t.
             if (difftime(now, state->peers[i].last_seen) > PEER_TIMEOUT) {
                 // Peer has timed out. Mark it as inactive.
                 // Note: This modification happens *while holding the lock*, ensuring safety.
                 log_message("Peer %s@%s timed out.", state->peers[i].username, state->peers[i].ip);
                 state->peers[i].active = 0;
                 // Skip printing this peer as it's no longer considered active.
                 continue;
             }

             // If the peer is active and not timed out, print its details.
             // Increment the active_count *before* using it to get 1-based numbering.
             printf("%d. %s@%s (last seen %ld seconds ago)\n",
                    ++active_count,                     // Peer number in this list display
                    state->peers[i].username,           // Peer's username
                    state->peers[i].ip,                 // Peer's IP address
                    (long)(now - state->peers[i].last_seen)); // Time elapsed since last seen
         }
     }

     // If no active peers were found after checking the list.
     if (active_count == 0) {
         printf("No active peers found.\n");
     }
     // Print a footer for the list.
     printf("--------------------\n\n");

     // Release the mutex lock, allowing other threads to access the peers array again.
     pthread_mutex_unlock(&state->peers_mutex);
     // Ensure the output buffer is flushed.
     fflush(stdout);
 }

 /**
  * @brief Parses and executes a command entered by the user.
  * @details Compares the user's input string against known commands (e.g., "/list", "/send").
  *          - For `/list`, calls `print_peers`.
  *          - For `/help`, calls `print_help_message`.
  *          - For `/send`, parses the peer number and message, finds the corresponding peer's IP
  *            (thread-safely), and calls `send_message` to send a `MSG_TEXT` message.
  *          - For `/broadcast`, iterates through all active peers (thread-safely) and calls
  *            `send_message` for each one.
  *          - For `/quit`, iterates through active peers (thread-safely) sending a `MSG_QUIT`
  *            notification to each, then sets the application's `running` flag to 0, and returns 1
  *            to signal the input loop to terminate.
  *          - For unknown commands, prints an error message.
  * @param state Pointer to the application state (`app_state_t`).
  * @param input The null-terminated string containing the command entered by the user.
  * @return 0 if the command was processed and the application should continue running.
  * @return 1 if the `/quit` command was processed, signaling the application to shut down.
  */
 int handle_command(app_state_t *state, const char *input) {
     // --- Handle /list command ---
     if (strcmp(input, "/list") == 0) {
         print_peers(state); // Call the function to display the peer list.
         return 0; // Continue running.
     }
     // --- Handle /help command ---
     else if (strcmp(input, "/help") == 0) {
         print_help_message(); // Call the function to display help.
         return 0; // Continue running.
     }
     // --- Handle /send command ---
     // Use strncmp to check if the input *starts with* "/send ".
     else if (strncmp(input, "/send ", 6) == 0) {
         int peer_num_input; // Store the peer number entered by the user.
         char *msg_start;    // Pointer to the beginning of the message part.
         // Temporary buffer to hold a mutable copy of the input for parsing.
         // Needed because we modify the string to extract the peer number.
         char input_copy[BUFFER_SIZE];
         strncpy(input_copy, input, BUFFER_SIZE -1);
         input_copy[BUFFER_SIZE -1] = '\0';


         // Find the first space *after* "/send ". This space separates the peer number from the message.
         msg_start = strchr(input_copy + 6, ' '); // Search starting after "/send "

         if (msg_start == NULL) {
             // No space found after the potential peer number, meaning the message part is missing.
             printf("Usage: /send <peer_number> <message>\n");
             return 0; // Invalid format, continue running.
         }

         // Temporarily terminate the string at the space found.
         // This isolates the peer number part (input_copy + 6) so atoi can parse it.
         *msg_start = '\0';
         // Convert the peer number string (now null-terminated) to an integer.
         peer_num_input = atoi(input_copy + 6);
         // Advance msg_start to point to the actual beginning of the message content (after the space).
         msg_start++;

         // Check if a valid positive peer number was entered.
         if (peer_num_input <= 0) {
              printf("Invalid peer number. Use /list to see active peers.\n");
              return 0;
         }


         // Now, find the target IP address corresponding to the peer number.
         // This requires iterating through the *active* peers list safely.
         pthread_mutex_lock(&state->peers_mutex); // Lock the peer list.

         int current_peer_index = 0; // Counter for active peers found so far.
         int found = 0;              // Flag to indicate if the target peer number was found.
         char target_ip[INET_ADDRSTRLEN]; // Buffer to store the target IP if found.

         for (int i = 0; i < MAX_PEERS; i++) {
             // Consider only active peers that haven't timed out (check timeout again for safety).
             if (state->peers[i].active && (difftime(time(NULL), state->peers[i].last_seen) <= PEER_TIMEOUT)) {
                 current_peer_index++; // Increment the count of active peers seen.
                 // Check if this active peer is the one the user requested.
                 if (current_peer_index == peer_num_input) {
                     // Found the target peer. Copy its IP address.
                     strncpy(target_ip, state->peers[i].ip, INET_ADDRSTRLEN -1);
                     target_ip[INET_ADDRSTRLEN -1] = '\0'; // Ensure null termination
                     found = 1; // Set the found flag.
                     break; // Exit the loop, no need to check further.
                 }
             }
         }

         pthread_mutex_unlock(&state->peers_mutex); // Unlock the peer list.

         // Check if the requested peer number was valid and found.
         if (found) {
             // Attempt to send the message using the network function.
             // Pass the found IP, the message content, type TEXT, and our username.
             if (send_message(target_ip, msg_start, MSG_TEXT, state->username) < 0) {
                 // Log if sending failed (e.g., connection refused, timeout).
                 log_message("Failed to send message to %s", target_ip);
             } else {
                 // Log successful send attempt.
                 log_message("Message sent to peer %d (%s)", peer_num_input, target_ip);
             }
         } else {
             // The provided peer number was out of range or invalid.
             log_message("Invalid peer number '%d'. Use /list to see active peers.", peer_num_input);
         }
         return 0; // Continue running.
     }
     // --- Handle /broadcast command ---
     // Check if the input starts with "/broadcast ".
     else if (strncmp(input, "/broadcast ", 11) == 0) {
         const char *message_content = input + 11; // The message starts after "/broadcast ".

         log_message("Broadcasting message: %s", message_content);
         // Lock the peer list to safely iterate through active peers.
         pthread_mutex_lock(&state->peers_mutex);

         int sent_count = 0;
         // Iterate through all peer slots.
         for (int i = 0; i < MAX_PEERS; i++) {
             // Check if the peer is active and not timed out.
             if (state->peers[i].active && (difftime(time(NULL), state->peers[i].last_seen) <= PEER_TIMEOUT)) {
                 // Send the message to this active peer.
                 if (send_message(state->peers[i].ip, message_content, MSG_TEXT, state->username) < 0) {
                     // Log failure for individual peers.
                     log_message("Failed to send broadcast message to %s", state->peers[i].ip);
                 } else {
                     sent_count++;
                 }
             }
         }

         pthread_mutex_unlock(&state->peers_mutex); // Unlock the peer list.
         log_message("Broadcast message sent to %d active peer(s).", sent_count);
         return 0; // Continue running.
     }
     // --- Handle /quit command ---
     else if (strcmp(input, "/quit") == 0) {
         log_message("Initiating quit sequence...");
         // Send a quit notification message to all known active peers.
         pthread_mutex_lock(&state->peers_mutex); // Lock peer list.

         log_message("Sending QUIT notifications to peers...");
         int notify_count = 0;
         for (int i = 0; i < MAX_PEERS; i++) {
             // Check if peer is active (no need to check timeout here, just notify known active ones).
             if (state->peers[i].active) {
                 // Send a MSG_QUIT message (content is typically empty).
                 if (send_message(state->peers[i].ip, "", MSG_QUIT, state->username) < 0) {
                     log_message("Failed to send quit notification to %s", state->peers[i].ip);
                 } else {
                     notify_count++;
                 }
             }
         }
         pthread_mutex_unlock(&state->peers_mutex); // Unlock peer list.
         log_message("Quit notifications sent to %d peer(s).", notify_count);

         // Set the global running flag to 0. This signals all threads (including this one's caller)
         // to terminate their loops.
         // Accessing g_state directly is okay here as it's the designated way for shutdown.
         if (g_state) { // Check if g_state is initialized
             g_state->running = 0;
         } else {
             // Fallback if g_state somehow isn't set - less graceful shutdown.
             state->running = 0;
         }


         log_message("Exiting application...");
         return 1; // Return 1 to signal the user_input_thread loop to break.
     }
     // --- Handle Unknown Command ---
     else {
         log_message("Unknown command: '%s'. Type /help for available commands.", input);
         return 0; // Continue running.
     }
 }

  /**
  * @brief Main function for the user input thread.
  * @details This function runs in a dedicated thread and continuously handles user interaction.
  *          It performs the following in a loop:
  *          1. Uses `select` to wait for input on standard input (`stdin`) or for a timeout (1 second).
  *             This prevents the thread from blocking indefinitely and allows graceful shutdown.
  *          2. If `select` indicates input is ready, reads a line using `fgets`.
  *          3. Removes the trailing newline character.
  *          4. If the input is not empty, calls `handle_command` to process it.
  *          5. Prints the command prompt ("> ") only *after* processing input or handling empty input,
  *             right before waiting again with `select`. It does *not* print the prompt on timeout.
  *          6. The loop breaks on `/quit`, EOF, read error, or when the application's `running` flag becomes 0.
  * @param arg A void pointer, expected to be cast to `app_state_t*`, providing access to the shared application state.
  * @return NULL. Standard practice for Pthread functions; the return value is not used here.
  */
 void *user_input_thread(void *arg) {
    // Cast the void pointer argument back to the expected app_state_t pointer.
    app_state_t *state = (app_state_t *)arg;
    // Buffer to store the user's input line.
    char input[BUFFER_SIZE];
    // File descriptor set used with select() to monitor standard input.
    fd_set readfds;
    // Timeout structure for select() to prevent indefinite blocking.
    struct timeval timeout;

    // Display the help message once when the thread starts.
    print_help_message();

    // Print the initial command prompt before entering the loop.
    printf("> ");
    fflush(stdout);

    // Main loop: continues as long as the application is running.
    while (state->running) {
        // --- Prepare for select() ---
        // Clear the file descriptor set.
        FD_ZERO(&readfds);
        // Add the standard input file descriptor (STDIN_FILENO, usually 0) to the set.
        FD_SET(STDIN_FILENO, &readfds);

        // Set the timeout for select().
        timeout.tv_sec = 1;  // Wait for up to 1 second.
        timeout.tv_usec = 0; // 0 microseconds.

        // --- Wait for input or timeout using select() ---
        int activity = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);

        // --- Handle select() results ---
        if (activity < 0) {
            // An error occurred during select().
            if (errno == EINTR) {
                // Interrupted by signal, simply continue the loop to check state->running.
                continue;
            } else {
                // A non-interrupt error occurred. Log it and exit the thread.
                perror("select error in user input thread");
                break;
            }
        }

        // Check if the application is still supposed to be running after select() returns.
        if (!state->running) {
            break; // Exit the loop if the running flag is now false.
        }

        // If activity is 0, select() timed out. No input was ready.
        // DO NOT print the prompt here. Just loop again to wait.
        if (activity == 0) {
            continue;
        }

        // If we reach here, activity > 0, meaning input is ready on STDIN_FILENO.

        // --- Read the available input ---
        if (fgets(input, BUFFER_SIZE, stdin) == NULL) {
            // Check if fgets failed because we are shutting down or due to a real error/EOF.
            if (state->running) {
                if (feof(stdin)) {
                    log_message("EOF detected on stdin. Exiting input loop.");
                } else {
                    perror("Error reading input from stdin");
                }
            }
            // Break the loop if input fails (EOF or error).
            break;
        }

        // --- Process the received input ---
        input[strcspn(input, "\n")] = '\0'; // Remove trailing newline.

        // Check if the input line is empty (user just pressed Enter).
        if (strlen(input) == 0) {
            // Input was empty. Print the prompt again and wait for the next input.
            printf("> ");
            fflush(stdout);
            continue; // Go back to select()
        }

        // Process the non-empty command entered by the user.
        if (handle_command(state, input) == 1) {
            // If handle_command signals to quit (/quit command), break out of the input loop.
            break;
        }

        // --- Command processed (or was unknown/invalid) ---
        // Print the prompt again, ready for the next command.
        printf("> ");
        fflush(stdout);

        // Loop continues to wait for next input via select().

    } // End of while(state->running) loop

    log_message("User input thread stopped.");
    // Return NULL as the thread exit value.
    return NULL;
}