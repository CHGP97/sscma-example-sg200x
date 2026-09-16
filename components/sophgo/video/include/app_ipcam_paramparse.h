#ifndef __SOPHGO_VIDEO_PARAM_PARSE_H__
#define __SOPHGO_VIDEO_PARAM_PARSE_H__

#include <linux/cvi_comm_sys.h>

#include "app_ipcam_comm.h"
#include "app_ipcam_sys.h"
#include "app_ipcam_venc.h"
#include "app_ipcam_vi.h"
#include "app_ipcam_vpss.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_RES_MODE_DEFAULT = 0,
    APP_RES_MODE_MAX,
} APP_RES_MODE_E;

int app_ipcam_Param_setVencChnType(int ch, PAYLOAD_TYPE_E enType);
int app_ipcam_Param_Load(void);

/* select sensor resolution mode; must be called before initVideo().
 * the mode is applied on every app_ipcam_Param_Load() and survives
 * venc template re-copies (app_ipcam_Param_setVencChnType). */
int app_ipcam_Param_SetResMode(APP_RES_MODE_E mode);
APP_RES_MODE_E app_ipcam_Param_GetResMode(void);

/* max capability of the attached sensor: {w, h, fps}. returns 0 on success. */
int app_ipcam_Get_SnsMaxRes(uint16_t* w, uint16_t* h, uint8_t* fps);

#ifdef __cplusplus
}
#endif

#endif // __SOPHGO_VIDEO_PARAM_PARSE_H__
