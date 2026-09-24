/* installer.c - orbisRPC Setup: Inject -> token -> network test -> tweaks.
 * Skeleton: proves the app boots on-console with a native dialog. */
#include <stdio.h>
#include "ui.h"

#define SETUP_VERSION "0.4.0"

int main(void){
    char msg[256];
    snprintf(msg, sizeof msg,
             "orbisRPC Setup %s\n\nInstaller skeleton boots.\nWizard lands next.", SETUP_VERSION);
    ui_ok(msg);
    return 0;
}
