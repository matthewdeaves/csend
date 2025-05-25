#ifndef UI_FACTORY_H
#define UI_FACTORY_H

#include "ui_interface.h"

typedef enum {
    UI_MODE_INTERACTIVE,
    UI_MODE_MACHINE
} ui_mode_t;

/* Factory function to create appropriate UI implementation */
ui_context_t *ui_factory_create(ui_mode_t mode);

/* Cleanup function */
void ui_factory_destroy(ui_context_t *ui);

/* Get operations for specific implementations */
ui_operations_t *ui_terminal_interactive_ops(void);
ui_operations_t *ui_terminal_machine_ops(void);

#endif /* UI_FACTORY_H */