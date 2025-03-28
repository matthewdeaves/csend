//====================================
// FILE: ./ui_terminal.h
//====================================

#ifndef UI_TERMINAL_H
#define UI_TERMINAL_H

// Include peer.h for the definition of app_state_t
#include "peer.h"

/**
 * @brief Displays the help message with available commands to stdout.
 */
void print_help_message(void);

/**
 * @brief The main function for the user input thread.
 * Reads commands from stdin and processes them using handle_command.
 * @param arg Pointer to the application state (app_state_t*).
 * @return NULL.
 */
void *user_input_thread(void *arg);

/**
 * @brief Prints the list of currently active peers to stdout.
 * Checks for peer timeouts and marks inactive peers. Thread-safe.
 * @param state Pointer to the application state.
 */
void print_peers(app_state_t *state);

/**
 * @brief Parses and executes user commands (e.g., /list, /send, /quit).
 * @param state Pointer to the application state.
 * @param input The command string entered by the user.
 * @return 0 to continue processing input, 1 if the /quit command was issued.
 */
int handle_command(app_state_t *state, const char *input);

#endif // UI_TERMINAL_H