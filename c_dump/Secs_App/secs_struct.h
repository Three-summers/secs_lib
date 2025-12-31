#ifndef SECS_STRUCT_H_
#define SECS_STRUCT_H_

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include "../BSP_App/Linux_BSP/Timer_bsp/timer_bsp.h"
#include "../BSP_App/Linux_BSP/serial_bsp/serial_bsp.h"
#else
#include "tim.h"
#include "usart.h"
#endif

typedef struct {
    uint8_t Stream_ID;   // upper Message_ID S*
    uint8_t Function_ID; // lower Message_ID F*
    uint8_t SFType_NO;   // 相同的类型下，可能要发送多种
} SecsType;

typedef struct {
    uint8_t
        SendData_Ctrl; // secs
                       // 报文控制，若为1，则表明已经转化成串口可发送的形式，若为0，则需要进一步转换
    uint8_t *
        SendData_Buf; // secs
                      // 发送的数据，已指针的形式，可以根据发送数据包打大小而调整
    size_t SendData_Size; // secs 发送数据包的整个长度
    uint16_t SendData_Ptr; // secs 发送数据包的指针，拆包发送时可用
} SendDataInfo;

typedef struct {
    uint8_t
        SystemByte_Ctrl; // 0:生成串口发送报文时，system byte需要递增；1：system
                         // byte不变，并使用SystemByte_Maintain
    uint32_t SystemByte_Maintain; // secs 回复报文时system
                                  // byte需相同，以及发送大包拆成多包时，system
                                  // byte也需相同
    uint32_t SystemByte_Increase; // secs 发送的system byte，一般为递增模式
} SystemByteInfo;

typedef struct {
    SecsType SecsSend_Type;
    SendDataInfo Send_DataInfo;
    SystemByteInfo Systembyte_Info;
} SecsSendInfo;

typedef struct {
    uint8_t SecsRecv_Flag; // secs recv flag
    uint8_t *SecsRecv_Buf; // secs recv buffer

    uint8_t SecsTimeout_Flag; // secs transation timeout flag
} SecsRecvInfo;

typedef struct {
    uint8_t Secs_State;    // secs 状态机寄存器
    uint8_t Secs_WRFlag;   // secs 读写标志：0：写标志；1：读标志
    uint8_t Secs_TryTimes; // secs 尝试握手次数
} SecsStatus;

typedef struct {
    uint8_t Reverse_Bit; // 0:Host to Equipment; 1:Equipment to Host
    uint16_t Device_ID;  // 设备ID
    uint8_t Reply_Bit;   // 0:no need to reply; 1:need reply (Wait_bit)
    SecsType Secs_Type;  // secs 类型 S*F*
    uint32_t SystemByte; // secs system byte控制
    uint8_t End_Bit;     // 0:not the last blobk 1:the last block
    uint16_t Block_num;
} SecsHead;

typedef struct {
#ifdef __linux__
    serial_listener_t *Secs_Serial;
    timer_thread_t *Secs_Timer;

#else
    UART_HandleTypeDef *Secs_Serial;
    TIM_HandleTypeDef *Secs_Timer;
#endif
    uint16_t Device_ID; // 设备ID
} SecsBSPInfo;

typedef struct {
    uint8_t Secs_IsBusy; // secs is busy

    uint8_t SecsSend_Flag;     // secs send flag
    SecsType SecsSend_Type;    // secs send type
    uint8_t *SecsSend_Message; // secs send message

    uint16_t Message_ID;
} SecsTransInfo;

typedef struct {
    SecsTransInfo *Secs_TransInfo;
    SecsSendInfo Secs_SendInfo;
    SecsRecvInfo *Secs_RecvInfo;
    SecsBSPInfo *Secs_BSPInfo; // secs bsp function
    SecsStatus Secs_Status;
    void (*callback)(SecsHead *Secs_Head, uint8_t *data);
} SecsInfo;

#endif /* SRC_STRUCT_FILE_H_ */
