#ifndef SECS_APP_H
#define SECS_APP_H
/*
SECS version: SECS-2025-09-17
*/

#include "secs_I.h"
#include "secs_II.h"
#include "secs_bsp.h"
#include "secs_struct.h"

// SECS log 控制，设为 1，表明打印更多调试信息
#define SECS_LOG 1

#define SECS_S1 0x01
#define SECS_S5 0x05
#define SECS_S6 0x06

#define SECS_F1 0x01
#define SECS_F2 0x02
#define SECS_F3 0x03
#define SECS_F4 0x04
#define SECS_F11 0x0b
#define SECS_F12 0x0c
#define SECS_F15 0x0f
#define SECS_F16 0x10

#ifdef __linux__
int init_secs_app(SecsTransInfo *Secs_TransInfo1,
                  SecsTransInfo *Secs_TransInfo2,
                  uint16_t Secs_DeviceID,
                  void (*SecsData_DealCallback1)(SecsHead *Secs_Head,
                                                 uint8_t *data),
                  void (*SecsData_DealCallback2)(SecsHead *Secs_Head,
                                                 uint8_t *data));
#else
void Secs_Ctrl_Func(void);

int init_secs_app(SecsTransInfo *Secs_TransInfo,
                  uint16_t Secs_DeviceID,
                  UART_HandleTypeDef *HalUart,
                  TIM_HandleTypeDef *HalTimer,
                  void (*SecsData_DealCallback)(SecsHead *Secs_Head,
                                                uint8_t *data));
#endif

#endif
