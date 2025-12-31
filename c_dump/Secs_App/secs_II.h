#ifndef SECS_II_H
#define SECS_II_H

#include <errno.h>
#ifdef __linux__
#include "secs_struct.h"
#else
#include "secs_app.h"
#endif

#define SECS_LIST     0x01
#define SECS_BINARY   0x21
#define SECS_BOOLEAN  0x25
#define SECS_ASCII    0x41
#define SECS_JIS8     0x45
#define SECS_CHAR2    0x49
#define SECS_UINT1    0xA5
#define SECS_UINT2    0xA9
#define SECS_UINT4    0xB1
#define SECS_UINT8    0xA1
#define SECS_INT1     0x65
#define SECS_INT2     0x69
#define SECS_INT4     0x71
#define SECS_INT8     0x61
#define SECS_FT4      0x91
#define SECS_FT8      0x81

int Secs_MessageArrange(char *Secs_string, SendDataInfo *SecsArray);
void SecsII_PrintArray(SendDataInfo *SecsArray);

#endif
