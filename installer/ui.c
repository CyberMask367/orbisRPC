/* ui.c - native PS4 dialogs: MsgDialog (ok / yes-no / progress) + ImeDialog.
 * Host-testable pure parts live in ui_text.c; everything here needs the
 * console (dialog system calls). */
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/ImeDialog.h>
#include <orbis/UserService.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

static void base_init(OrbisMsgDialogParam *param){
    memset(param, 0, sizeof(*param));
    param->baseParam.size = (uint32_t)sizeof(param->baseParam);
    param->baseParam.magic =
        (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)&param->baseParam);
    param->size = sizeof(OrbisMsgDialogParam);
    param->mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
}

static int ui_ready = 0;

int ui_init(void){
    if(ui_ready) return 0;
    if(sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG) < 0) return -1;
    if(sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG) < 0) return -1;
    if(sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_BACKEND) < 0) return -1;
    if(sceCommonDialogInitialize() < 0) return -1;
    ui_ready = 1;
    return 0;
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

static int progress_open = 0;

int ui_progress_open(const char *msg){
    OrbisMsgDialogParam param;
    OrbisMsgDialogProgressBarParam bar;
    if(progress_open) return 0;
    sceMsgDialogInitialize();
    base_init(&param);
    param.mode = ORBIS_MSG_DIALOG_MODE_PROGRESS_BAR;
    memset(&bar, 0, sizeof bar);
    bar.barType = ORBIS_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
    bar.msg = msg;
    param.progBarParam = &bar;
    if(sceMsgDialogOpen(&param) < 0){ sceMsgDialogTerminate(); return -1; }
    progress_open = 1;
    return 0;
}

void ui_progress_msg(const char *msg){
    if(progress_open && msg) sceMsgDialogProgressBarSetMsg(0, msg);
}

void ui_progress_set(unsigned pct){
    if(progress_open) sceMsgDialogProgressBarSetValue(0, pct > 100 ? 100 : pct);
}

void ui_progress_close(void){
    if(!progress_open) return;
    progress_open = 0;
    sceMsgDialogClose();
    sceMsgDialogTerminate();
}

int ui_input(const char *title, const char *placeholder, char *out, size_t cap){
    static wchar_t wbuf[256];
    static wchar_t wtitle[64];
    static wchar_t wplace[64];
    OrbisImeDialogSetting st;
    OrbisDialogResult res;
    int32_t uid = 0;
    size_t i;
    if(!out || cap == 0 || cap > 200) return -1;
    /* Prefill with current value (ASCII round-trip). */
    for(i = 0; i < cap - 1 && out[i]; i++) wbuf[i] = (wchar_t)(unsigned char)out[i];
    wbuf[i] = 0;
    for(i = 0; i < 63 && title && title[i]; i++) wtitle[i] = (wchar_t)(unsigned char)title[i];
    wtitle[i] = 0;
    for(i = 0; i < 63 && placeholder && placeholder[i]; i++) wplace[i] = (wchar_t)(unsigned char)placeholder[i];
    wplace[i] = 0;
    if(sceUserServiceGetInitialUser(&uid) < 0) uid = 0;
    memset(&st, 0, sizeof st);
    st.userId = (uint32_t)uid;
    st.type = 0;
    st.supportedLanguages = 0;
    st.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    st.inputMethod = 0;
    st.filter = 0;
    st.option = 0;
    st.maxTextLength = (uint32_t)(cap - 1);
    st.inputTextBuffer = wbuf;
    st.posx = 0;
    st.posy = 0;
    st.horizontalAlignment = ORBIS_H_CENTER;
    st.verticalAlignment = ORBIS_V_CENTER;
    st.placeholder = wplace;
    st.title = wtitle;
    sceImeDialogInit(&st, NULL);
    /* Pump until the OSK stops (IME uses its own status enum). */
    while(sceImeDialogGetStatus() != ORBIS_DIALOG_STATUS_STOPPED){
        /* The dialog system needs no explicit pump; yield briefly. */
        sceKernelUsleep(20000);
    }
    memset(&res, 0, sizeof res);
    if(sceImeDialogGetResult(&res) < 0){ sceImeDialogTerm(); return -1; }
    sceImeDialogTerm();
    if(res.endstatus != ORBIS_DIALOG_OK) return 0;
    for(i = 0; i < cap - 1 && wbuf[i]; i++)
        out[i] = (wbuf[i] < 128 && wbuf[i] >= 32) ? (char)wbuf[i] : '?';
    out[i] = 0;
    return 1;
}
