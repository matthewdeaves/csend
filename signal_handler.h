// Include guard: Prevents the header file from being included multiple times
// within a single compilation unit (.c file), avoiding potential errors caused
// by duplicate declarations.
#ifndef SIGNAL_HANDLER_H // If SIGNAL_HANDLER_H is not defined...
#define SIGNAL_HANDLER_H // ...define SIGNAL_HANDLER_H.

// --- Includes ---
// No standard or project-specific headers are included *in this header file*.
// This is intentional and good practice for header files when possible.
// Why?
// 1. Minimizes Dependencies: Files that only need to *know about* the `handle_signal`
//    function (e.g., peer.c where it's registered with `sigaction`) don't need to
//    pull in all the headers required by the *implementation* of `handle_signal`
//    (like peer.h or utils.h, which are needed in signal_handler.c). This speeds
//    up compilation and reduces coupling between modules.
// 2. Clarity: It clearly shows that only the function's signature (its name,
//    return type, and parameter types) is needed by users of this header.

// The implementation file (signal_handler.c) is responsible for including the
// necessary headers (like <signal.h>, "peer.h", "utils.h") to define and use
// types like `sig_atomic_t`, `app_state_t`, `g_state`, and `log_message`.


// --- Function Declaration ---

/**
 * @brief Declares the signal handler function responsible for initiating graceful shutdown.
 * @details This function is intended to be registered with the operating system (using `sigaction`
 *          in `init_app_state`) to handle specific signals, typically `SIGINT` (Ctrl+C) and
 *          `SIGTERM` (termination request). When invoked by the OS upon receiving one of these
 *          signals, its primary action (implemented in signal_handler.c) is to set a global
 *          flag (`g_state->running`) to 0. This flag signals the application's main worker
 *          threads to exit their loops and terminate gracefully.
 *
 * @param sig The integer representing the signal number that caused the handler to be invoked
 *            (e.g., the value of SIGINT or SIGTERM). This parameter is part of the standard
 *            signature for signal handling functions of this type.
 *
 * @note The function itself returns `void` as required for signal handlers registered
 *       with `sa_handler` in `struct sigaction`.
 */
void handle_signal(int sig);

#endif // End of the include guard SIGNAL_HANDLER_H