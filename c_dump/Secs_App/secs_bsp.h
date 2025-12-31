#ifndef SECS_BSP_H
#define SECS_BSP_H

#ifdef __linux__
#include "secs_struct.h"
#else
#include "secs_app.h"
#endif


#define BLOCK_LENGTH 256

//sece baud
#define SECE_UART_BAUDRATE B9600

extern SecsRecvInfo Secs_RecvInfo1;
extern SecsBSPInfo  Secs_BSPInfo1;

extern SecsRecvInfo Secs_RecvInfo2;
extern SecsBSPInfo  Secs_BSPInfo2;

int Secs_SerialSend(SecsBSPInfo *Secs_BSPInfo, uint8_t *send_buf, uint16_t buf_size);

int Secs_TimerCtrl(SecsBSPInfo *Secs_BSPInfo, uint8_t SecsTimer_en, uint16_t Secs_IntrTime);


#ifdef __linux__
int init_secs_bsp(
	char *Secs_Serial_Device1,
	char *Secs_Serial_Device2
);
#else
void SecsSerial_RecvCallback1(const char *Serial_RecvBuf, size_t RecvBuf_Size);
void SecsTimer_IntrCallback1(void);

int init_secs_bsp(
	UART_HandleTypeDef *HalUart,
	TIM_HandleTypeDef *HalTimer
);
#endif


#endif
