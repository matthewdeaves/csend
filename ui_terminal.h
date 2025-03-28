// Include guard: Prevents the header file from being included multiple times
// in the same compilation unit, avoiding redefinition errors.
#ifndef UI_TERMINAL_H // If UI_TERMINAL_H is not defined...
#define UI_TERMINAL_H // ...define UI_TERMINAL_H.

// Include peer.h because the functions declared in this header file
// operate on or need access to the `app_state_t` structure, which is
// defined in peer.h. This structure contains the shared application state,
// including the peer list, mutex, running flag, etc., that the UI needs
// to display information and interact with the application core.
#include "peer.h"

// --- Function Declarations ---
// These declarations specify the functions related to the terminal user interface
// that are implemented in ui_terminal.c and can be called by other parts of
// the application (specifically, main() in peer.c calls user_input_thread).

/**
 * @brief Displays a formatted help message listing available commands to standard output.
 * @details This function prints the list of commands (like /list, /send, /quit) and
 *          their usage instructions directly to the console, guiding the user on how
 *          to interact with the application.
 */
void print_help_message(void);

/**
 * @brief The main function designed to run in a separate thread dedicated to handling user input.
 * @details This function contains the primary loop for the terminal UI. It continuously
 *          prompts the user for input, reads commands from standard input (`stdin`),
 *          parses them (by calling `handle_command`), and manages the UI interaction flow.
 *          It runs until the application is signaled to shut down (e.g., via the `/quit` command
 *          or an external signal).
 * @param arg A void pointer, expected to be cast to `app_state_t*` within the function.
 *            This provides the thread with access to the shared application state needed
 *            for processing commands (e.g., accessing the peer list, sending messages).
 * @return NULL. Standard practice for Pthread functions; the return value is typically unused.
 */
void *user_input_thread(void *arg);

/**
 * @brief Prints the list of currently known and active peers to standard output.
 * @details Iterates through the application's peer list (stored in `state->peers`).
 *          It checks each peer's `active` status and `last_seen` timestamp to determine
 *          if the peer is currently considered active (not timed out). Active peers are
 *          displayed with a number, username, IP address, and how long ago they were last seen.
 *          This function accesses the shared peer list and therefore uses the `peers_mutex`
 *          (internally within its implementation in ui_terminal.c) to ensure thread safety.
 * @param state Pointer to the `app_state_t` structure containing the peer list and mutex.
 */
void print_peers(app_state_t *state);

/**
 * @brief Parses a command string entered by the user and executes the corresponding action.
 * @details Takes the raw input string from the user, determines which command it represents
 *          (e.g., `/list`, `/send`, `/broadcast`, `/quit`, `/help`), extracts any necessary
 *          arguments (like peer number or message content), and performs the requested action
 *          (e.g., calling `print_peers`, `send_message`, or setting the shutdown flag).
 *          It interacts with other parts of the application (like the network module via `send_message`)
 *          based on the command.
 * @param state Pointer to the `app_state_t` structure, providing access to application state
 *              needed to execute commands (e.g., username, peer list, running flag).
 * @param input A pointer to the null-terminated character string containing the command line
 *              entered by the user.
 * @return 0 if the command was processed and the application should continue accepting input.
 * @return 1 if the `/quit` command was successfully processed, indicating that the input loop
 *         (in `user_input_thread`) should terminate, leading to application shutdown.
 */
int handle_command(app_state_t *state, const char *input);

#endif // End of the include guard UI_TERMINAL_H