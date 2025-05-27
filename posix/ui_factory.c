#include "ui_factory.h"
#include <stdlib.h>
#include <string.h>

ui_context_t *ui_factory_create(ui_mode_t mode)
{
    ui_context_t *ui = (ui_context_t *)calloc(1, sizeof(ui_context_t));
    if (!ui) return NULL;

    switch (mode) {
    case UI_MODE_INTERACTIVE:
        ui->ops = ui_terminal_interactive_ops();
        break;
    case UI_MODE_MACHINE:
        ui->ops = ui_terminal_machine_ops();
        break;
    default:
        free(ui);
        return NULL;
    }

    /* Initialize the implementation */
    if (ui->ops && ui->ops->init) {
        ui->ops->init(ui);
    }

    return ui;
}

void ui_factory_destroy(ui_context_t *ui)
{
    if (!ui) return;

    /* Cleanup implementation */
    if (ui->ops && ui->ops->cleanup) {
        ui->ops->cleanup(ui);
    }

    /* Free implementation data if allocated */
    if (ui->impl_data) {
        free(ui->impl_data);
    }

    free(ui);
}