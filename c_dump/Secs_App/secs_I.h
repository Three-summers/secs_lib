#ifndef SECS_I_H
#define SECS_I_H

#ifdef __linux__
#include "secs_struct.h"
#else
#include "secs_app.h"
#endif

#include "secs_bsp.h"

enum SECS_STATE { IDLE_STA, WAIT_EOT, WAIT_BLOCK, WAIT_CHECK };

#ifdef __linux__
#define _NULL 0x00
#endif
#define _ENQ 0x05
#define _EOT 0x04
#define _ACK 0x06
#define _NAK 0x15

#define TIMEOUT_INTE_RBYTE 1
#define TIMEOUT_PROTOCOL 3
#define TIMEOUT_REPLY 45
#define TIMEOUT_INTER_BLOCK 45
#define SECS_RETRY_LIMIT 3

#define SECS_WRITE 0
#define SECS_READ 1
#define SECS_TIMEOUT 2

// secs收发状态机控制
int SecsI_StateMachine(SecsStatus *Secs_Status,
                       SecsBSPInfo *Secs_BSPInfo,
                       SecsSendInfo *Secs_SendInfo,
                       uint8_t *Secs_RecvBuf);

// secs 提取头部信息
int SecsI_ExtractHead(uint8_t *BlockArray,    // 待提取的block
                      SecsHead *ExtractHead); // 提取后存放的数组指针

#endif
