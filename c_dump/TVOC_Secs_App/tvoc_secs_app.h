#ifndef TVOC_SECS_APP_H
#define TVOC_SECS_APP_H

#include "../../Secs_App/secs_app.h"

#include "../tvoc_func_app/tvoc_app.h"
#define DEVICE_ID 0x1000
#define SECSC_PROJECT_ID "tvoc_secs"
#define VERSION_ID "25-09-28"
#define CEID 0x5000

int tvoc_secs_init(void);

#endif