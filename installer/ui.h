/* ui.h - native dialog helpers for the installer wizard. */
#ifndef INSTALLER_UI_H
#define INSTALLER_UI_H

/* Info dialog with OK. 0 shown, -1 failed to open. */
int ui_ok(const char *msg);
/* Yes/No dialog, focus on No (safe default). 1 yes, 0 no/closed, -1 error. */
int ui_confirm(const char *msg);

#endif
