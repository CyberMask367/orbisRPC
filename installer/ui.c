/* ui.c - native PS4 dialogs: MsgDialog (ok / yes-no / progress) + ImeDialog.
 * Host-testable pure parts live in ui_text.c; everything here needs the
 * console (dialog system calls). */
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Sysmodule.h>

static void base_init(OrbisMsgDialogParam *param){
    memset(param, 0, sizeof(*param));
    param->baseParam.size = (uint32_t)sizeof(param->baseParam);
    param->baseParam.magic =
        (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)&param->baseParam);
    param->size = sizeof(OrbisMsgDialogParam);
}

int ui_ok(const char *msg){
    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam um;
    OrbisMsgDialogResult res;
    memset(&res, 0, sizeof res);
    sceMsgDialogInitialize();
    base_init(&param);
    memset(&um, 0, sizeof um);
    um.msg = msg;
    um.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    param.userMsgParam = &um;
    if(sceMsgDialogOpen(&param) < 0){ sceMsgDialogTerminate(); return -1; }
    do { } while(sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED);
    sceMsgDialogClose();
    sceMsgDialogGetResult(&res);
    sceMsgDialogTerminate();
    return 0;
}

int ui_confirm(const char *msg){
    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam um;
    OrbisMsgDialogResult res;
    memset(&res, 0, sizeof res);
    sceMsgDialogInitialize();
    base_init(&param);
    memset(&um, 0, sizeof um);
    um.msg = msg;
    um.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_YESNO_FOCUS_NO;
    param.userMsgParam = &um;
    if(sceMsgDialogOpen(&param) < 0){ sceMsgDialogTerminate(); return -1; }
    do { } while(sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED);
    sceMsgDialogClose();
    sceMsgDialogGetResult(&res);
    sceMsgDialogTerminate();
    return res.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_YES ? 1 : 0;
}
