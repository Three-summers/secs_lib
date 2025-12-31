#include "secs_I.h"

void SecsI_ResetStatus(SecsStatus *Secs_Status, SecsBSPInfo *Secs_BSPInfo);
int SecsI_BlockCheck(uint8_t *BlockArray);
int SecsI_SendBlocks(SecsSendInfo *Secs_SendInfo, SecsBSPInfo *Secs_BSPInfo);

/*
SECS 收发控制，由于secs采用半双工通讯，所以secs串口收发在同一函数里实现
int SecsI_StateMachine(SecsStatus *Secs_Status,
    SecsBSPInfo *secs_bsp_info,
    uint8_t *Secs_RecvBuf);
    ·SecsStatus
*Secs_Status：当前secs传输状态结构体指针，方便secs口扩展，结构体内容通过secs_app.h里查看
    secs_bsp_info: 当前secs串口发送的结构体指针，以及定时器结构体指针
    ·uint8_t *Secs_RecvBuf：secs串口接收数组指针，发送数据时，设为_NULL即可
    可通过将SECS_LOG定义为0，打印更多握手信息，便于调试
    内部逻辑：状态机各个状态采用枚举的方式，在secs_I.h里的SECS_STATE里可见
*/
int SecsI_StateMachine(SecsStatus *Secs_Status,
                       SecsBSPInfo *Secs_BSPInfo,
                       SecsSendInfo *Secs_SendInfo,
                       uint8_t *Secs_RecvBuf) {
    /*
    当传入的 Secs_Status 的读写标志为写标志时，表明为发送数据请求
    当传入的 Secs_Status 的读写标志为读标志时，表明为接收到数据，
    当传入的 Secs_Status 的读写标志为超时标志时，表明在指定时间内没有收到回复
    判断重复次数，若在规定时间内发送握手请求，对方未回复超过SECS_RETRY_LIMIT(默认3次)，返回-1
    */
    Secs_Status->Secs_TryTimes++;
    if (Secs_Status->Secs_WRFlag == SECS_WRITE) {
#if SECS_LOG
        printf("Secs send data request\r\n");
#endif
    } else if (Secs_Status->Secs_WRFlag == SECS_READ) {
        if (Secs_RecvBuf == NULL) {
            SecsI_ResetStatus(Secs_Status, Secs_BSPInfo);
            printf("[error ] secs receive data is empty\r\n");
        } else {
            Secs_Status->Secs_TryTimes = 0;
#if SECS_LOG
            printf("[  OK  ] secs receive data\r\n");
#endif
        }
    } else if (Secs_Status->Secs_WRFlag == SECS_TIMEOUT) {
        if (Secs_Status->Secs_TryTimes > SECS_RETRY_LIMIT) {
            SecsI_ResetStatus(Secs_Status, Secs_BSPInfo);
            printf("[error ] secs send data without respond at %d times\r\n",
                   Secs_Status->Secs_TryTimes - 1);
            return -1;
        } else
            printf("[warning] secs send data without respond at %d times\r\n",
                   Secs_Status->Secs_TryTimes - 1);
    } else {
        SecsI_ResetStatus(Secs_Status, Secs_BSPInfo);
        printf("[error ] secs detect unkonwn write read flag: %d\r\n",
               Secs_Status->Secs_WRFlag);
    }

    uint8_t Secs_RepyByte;

    if (Secs_Status->Secs_State > WAIT_CHECK) {
        printf("[error ] secs state error %d \n", Secs_Status->Secs_State);
        SecsI_ResetStatus(Secs_Status, Secs_BSPInfo);
        return -1;
    } else {
#if SECS_LOG
        printf("secs status is %d \n", Secs_Status->Secs_State);
#endif
        switch (Secs_Status->Secs_State) {

        //若当前状态机状态为IDLE状态，则表明为发送数据请求或接收数据请求
        //判断读写状态寄存器，若为写请求，发送ENQ，并开启定时器等待回复EOT
        //若为读请求，回复EOT，同样开启定时器，等待对方发送数据包
        case IDLE_STA:
            if (Secs_Status->Secs_WRFlag == SECS_WRITE) {
                Secs_RepyByte = _ENQ;
                Secs_SerialSend(Secs_BSPInfo, &Secs_RepyByte, 1);
                Secs_TimerCtrl(Secs_BSPInfo, 1, TIMEOUT_PROTOCOL);
                Secs_Status->Secs_State = WAIT_EOT;
#if SECS_LOG
                printf("[S] secs send ENQ at IDLE_STA\r\n");
                printf("[  OK  ] secs state turn to WAIT_EOT\r\n");
#endif
            } else if (Secs_Status->Secs_WRFlag == SECS_READ) {
                if (Secs_RecvBuf[0] == _ENQ) {
                    Secs_RepyByte = _EOT;
                    Secs_SerialSend(Secs_BSPInfo, &Secs_RepyByte, 1);
                    Secs_TimerCtrl(Secs_BSPInfo, 1, TIMEOUT_PROTOCOL);
                    Secs_Status->Secs_State = WAIT_BLOCK;
#if SECS_LOG
                    printf("[R] secs recv ENQ at IDLE_STA\r\n");
                    printf("[S] secs send EOT at IDLE_STA\r\n");
                    printf("[  OK  ] secs state turn to WAIT_BLOCK\r\n");
#endif
                } else {
                    printf("[error ] secs recv bad char at IDLE_STA\r\n");
                    return -1;
                }
            } else {
                SecsI_ResetStatus(Secs_Status, Secs_BSPInfo);
                printf("[error ] secs recv error Secs_WRFlag at IDLE_STA\r\n");
                return -1;
            }
            break;
        /*
        等待对方回复EOT状态，本机发送ENQ后，需要上位机发送EOT回复作为握手成功信号，此时可发送信息包，
        若判断数据包为空，表明为在指定时间内没有回复，重新发送ENQ
        进入该状态时若数据包不为空,且判断确实为EOT，关闭定时器，待发送完全后，再开启定时器，等待对方校验结果
        否则表明对方回复错误，关闭定时器，状态机重设为IDLE,并汇报错误
        */
        case WAIT_EOT:
            if (Secs_Status->Secs_WRFlag == SECS_TIMEOUT) {
                Secs_RepyByte = _ENQ;
                Secs_SerialSend(Secs_BSPInfo, &Secs_RepyByte, 1);
                printf("[failed] secs send ENQ without respond EOT\r\n");
                printf("[S] secs send ENQ at WAIT_EOT\r\n");
            } else if (Secs_Status->Secs_WRFlag == SECS_READ &&
                       Secs_RecvBuf[0] == _EOT) {
                Secs_TimerCtrl(Secs_BSPInfo, 0, 0);
                SecsI_SendBlocks(Secs_SendInfo, Secs_BSPInfo);
                Secs_TimerCtrl(Secs_BSPInfo, 1, TIMEOUT_PROTOCOL);
                Secs_Status->Secs_State = WAIT_CHECK;
#if SECS_LOG
                printf("[R] secs receive EOT at WAIT_EOT\r\n");
                printf("[S] secs send blocks\r\n");
                printf("[  OK  ] secs state turn to WAIT_CHECK\r\n");
#endif
            } else {
                printf("[R] secs recv unkown byte %2x at WAIT_EOT\r\n",
                       Secs_RecvBuf[0]);
                return -1;
            }
            break;
        /*
        等待数据包，若对方有数据发送请求，并且本机与其握手完毕，对方会发送数据长度大于等于10的数据包,程序运行至此表明接收到非空数据包，关闭定时器，状态机设为IDLE
        若数据包长度小于10，则表明数据包错误，返回-1
        若数据包长度不小于10，则对包数据进行校验，校验正确回复ACK，并对数据包进行处理
        校验错误回复NAK
        */
        case WAIT_BLOCK:
            SecsI_ResetStatus(Secs_Status, Secs_BSPInfo);
            if (Secs_Status->Secs_WRFlag == SECS_TIMEOUT) {
                printf("[failed] secs send EOT without respond block at "
                       "WAIT_BLOCK\r\n");
                return -1;
            } else if (Secs_Status->Secs_WRFlag == SECS_READ) {
                if (Secs_RecvBuf[0] < 10) {
                    printf(
                        "[R] secs recv error Tuple number %d at WAIT_BLOCK\r\n",
                        Secs_RecvBuf[0]);
#if SECS_LOG
                    printf("[error ] secs state turn to IDLE_STA\r\n");
#endif
                    return -1;
                } else if (SecsI_BlockCheck(Secs_RecvBuf) == 0) {
                    Secs_RepyByte = _ACK;
                    Secs_SerialSend(Secs_BSPInfo, &Secs_RepyByte, 1);
#if SECS_LOG
                    printf("[S] secs send ACK at WAIT_BLOCK\r\n");
                    printf("[  OK  ] secs state turn to IDLE_STA\r\n");
#endif
                } else {
                    Secs_RepyByte = _NAK;
                    Secs_SerialSend(Secs_BSPInfo, &Secs_RepyByte, 1);
                    printf("[error ] secs check receive block\r\n");
#if SECS_LOG
                    printf("[S] secs send NAK at WAIT_BLOCK\r\n");
                    printf("[error ] secs state turn to IDLE_STA\r\n");
#endif
                    return -1;
                }
            }
            break;
        /*
        发送数据包后，需等待对方回复校验结果，程序运行至此表明接收到非空数据包，关闭定时器，状态机设为IDLE
        判断回复结果，若回复ACK表明发送正确，
        若返回NCK表明发送错误，需重新发送（这里默认不重新发送），返回-1
        若返回其他值，表明发送数据失败，返回-1
        */
        case WAIT_CHECK:
            SecsI_ResetStatus(Secs_Status, Secs_BSPInfo);
            if (Secs_Status->Secs_WRFlag == SECS_TIMEOUT) {
                printf("[failed] secs send block without respond ack at "
                       "WAIT_CHECK\r\n");
                return -1;
            } else if (Secs_Status->Secs_WRFlag == SECS_READ) {
                if (Secs_RecvBuf[0] == _ACK) {
#if SECS_LOG
                    printf("[R] secs recv ACK at WAIT_CHECK\r\n");
                    printf("[  OK  ] secs state turn to IDLE_STA\r\n");
#endif
                } else if (Secs_RecvBuf[0] == _NAK) {
                    printf("[error ] secs send error, need re-send\r\n");
#if SECS_LOG
                    printf("[R] secs recv NAK at WAIT_CHECK\r\n");
                    printf("[error ] secs state turn to IDLE_STA\r\n");
#endif
                    return -1;
                } else {
                    printf("[R] secs recv unkown byte %x at WAIT_CHECK\r\n",
                           Secs_RecvBuf[0]);
                    return -1;
                }
            }
            break;

        default:
            SecsI_ResetStatus(Secs_Status, Secs_BSPInfo);
            printf("[error ] secs state not in enum SECS_STATE\r\n");
            return -1;
            break;
        }
        return 0;
    }
}

void SecsI_ResetStatus(SecsStatus *Secs_Status, SecsBSPInfo *Secs_BSPInfo) {
    Secs_TimerCtrl(Secs_BSPInfo, 0, 0);
    Secs_Status->Secs_State = IDLE_STA;
    Secs_Status->Secs_TryTimes = 0;
}

/*
校验secs读取的block是否正确
*/
int SecsI_BlockCheck(uint8_t *BlockArray) {
    uint8_t Block_Length = BlockArray[0];
    uint16_t Block_CheckSum;
    uint16_t Calcu_CheckSum = 0;

    for (int array_cnt = 0; array_cnt < Block_Length; array_cnt++)
        Calcu_CheckSum += BlockArray[array_cnt + 1];

    Block_CheckSum =
        (BlockArray[Block_Length + 1] << 8) + BlockArray[Block_Length + 2];

    if (Calcu_CheckSum == Block_CheckSum)
        return 0;
    else {
        printf("[C] secs block check error, recv checksum : %d, calcu checksum "
               ": %d\r\n",
               Block_CheckSum,
               Calcu_CheckSum);
        for (uint8_t array_cnt = 0; array_cnt < Block_Length; array_cnt++)
            printf("%02x ", BlockArray[array_cnt + 1]);
        printf("\r\n");
        return -1;
    }
}

int SecsI_ExtractHead(uint8_t *BlockArray, SecsHead *ExtractHead) {
    ExtractHead->Reverse_Bit = (BlockArray[1] >> 7);
    ExtractHead->Device_ID = ((BlockArray[1] & 0x7f) << 8) + BlockArray[2];
    ExtractHead->Reply_Bit = (BlockArray[3] >> 7);
    ExtractHead->Secs_Type.Stream_ID = (BlockArray[3] & 0x7f);
    ExtractHead->Secs_Type.Function_ID = BlockArray[4];
    ExtractHead->End_Bit = (BlockArray[5] >> 7);
    ExtractHead->Block_num = BlockArray[6];
    ExtractHead->SystemByte = ((BlockArray[7] << 24) + (BlockArray[8] << 16) +
                               (BlockArray[9] << 8) + BlockArray[10]);
    return 0;
}

int SecsI_ArrangeHead(SecsHead *ArrangeHead, uint8_t *Secs_SendBuf) {
    Secs_SendBuf[1] = (ArrangeHead->Reverse_Bit << 7) +
                      (uint8_t)(ArrangeHead->Device_ID >> 8);
    Secs_SendBuf[2] = (uint8_t)((ArrangeHead->Device_ID) & 0xff);
    Secs_SendBuf[3] =
        (ArrangeHead->Reply_Bit) + (ArrangeHead->Secs_Type.Stream_ID);
    Secs_SendBuf[4] = ArrangeHead->Secs_Type.Function_ID;
    Secs_SendBuf[5] = (ArrangeHead->End_Bit << 7);
    Secs_SendBuf[6] = ArrangeHead->Block_num;
    Secs_SendBuf[7] = (uint8_t)((ArrangeHead->SystemByte >> 24) & 0xff);
    Secs_SendBuf[8] = (uint8_t)((ArrangeHead->SystemByte >> 16) & 0xff);
    Secs_SendBuf[9] = (uint8_t)((ArrangeHead->SystemByte >> 8) & 0xff);
    Secs_SendBuf[10] = (uint8_t)((ArrangeHead->SystemByte) & 0xff);
    return 0;
}

void SecsI_ArrangeChecksum(uint8_t *Secs_SendBuf) {
    uint8_t Block_Length = Secs_SendBuf[0];
    uint16_t Calcu_CheckSum = 0;

    for (int array_cnt = 0; array_cnt < Block_Length; array_cnt++)
        Calcu_CheckSum += Secs_SendBuf[array_cnt + 1];

    Secs_SendBuf[Block_Length + 1] = (uint8_t)((Calcu_CheckSum >> 8) & 0xff);
    Secs_SendBuf[Block_Length + 2] = (uint8_t)(Calcu_CheckSum & 0xff);
}

int SecsI_SendBlocks(SecsSendInfo *Secs_SendInfo, SecsBSPInfo *Secs_BSPInfo) {
    uint8_t Secs_SendBuf[BLOCK_LENGTH];
    // 该包总长，头的长度固定为10位，所以给一个初始值10
    Secs_SendBuf[0] = 10;

    SecsHead Secs_SendHead;
    Secs_SendHead.Reverse_Bit = 1;
    Secs_SendHead.Device_ID = Secs_BSPInfo->Device_ID;
    Secs_SendHead.Reply_Bit = 0;
    Secs_SendHead.Secs_Type.Stream_ID = Secs_SendInfo->SecsSend_Type.Stream_ID;
    Secs_SendHead.Secs_Type.Function_ID =
        Secs_SendInfo->SecsSend_Type.Function_ID;

    /*
    判断Secs_Status->Secs_Type里的System_Byte是否为0，
    若为0，则表明不是回复和多包发，使用Secs_Status下的Secs_SystemByte
    若不为0.则使用该值作为systembyte
    */
    if (Secs_SendInfo->Systembyte_Info.SystemByte_Ctrl == 0)
        Secs_SendHead.SystemByte =
            Secs_SendInfo->Systembyte_Info.SystemByte_Increase++;
    else {
        Secs_SendHead.SystemByte =
            Secs_SendInfo->Systembyte_Info.SystemByte_Maintain;
    }
    /*
    如果待发送讯息包大小不大于243，表明可以一次发完，或最后一笔数据,
    将讯息包复制到发送数组的第11位之后，
    讯息包指针指向讯息包最后一位数据，
    发送数组长度增加讯息包长度
    讯息包长度清零
    释放讯息包内存空间
    */
    if (Secs_SendInfo->Send_DataInfo.SendData_Size <= 243) {
        Secs_SendHead.End_Bit = 1;
        memcpy(&Secs_SendBuf[11],
               &Secs_SendInfo->Send_DataInfo
                    .SendData_Buf[Secs_SendInfo->Send_DataInfo.SendData_Ptr],
               Secs_SendInfo->Send_DataInfo.SendData_Size);
        Secs_SendInfo->Send_DataInfo.SendData_Ptr +=
            Secs_SendInfo->Send_DataInfo.SendData_Size - 1;
        Secs_SendBuf[0] += Secs_SendInfo->Send_DataInfo.SendData_Size;
        Secs_SendInfo->Send_DataInfo.SendData_Size = 0;
        free(Secs_SendInfo->Send_DataInfo.SendData_Buf);
        Secs_SendInfo->Systembyte_Info.SystemByte_Ctrl = 0;
    }
    /*
    如果待发送讯息包大小大于243，表明不可以一次发完，不是最后一包数据
    将讯息包的243个长度复制到发送数组的第11位之后，
    讯息包指针指向增加243，
    发送数组长度增加243
    讯息包长度减去243
    */
    else {
        Secs_SendHead.End_Bit = 0;
        memcpy(&Secs_SendBuf[11],
               &Secs_SendInfo->Send_DataInfo
                    .SendData_Buf[Secs_SendInfo->Send_DataInfo.SendData_Ptr],
               243);
        Secs_SendInfo->Send_DataInfo.SendData_Ptr += 243;
        Secs_SendBuf[0] += 243;
        Secs_SendInfo->Send_DataInfo.SendData_Size -= 243;
        Secs_SendInfo->Systembyte_Info.SystemByte_Ctrl = 1;
        Secs_SendInfo->Systembyte_Info.SystemByte_Maintain =
            Secs_SendHead.SystemByte;
    }

    Secs_SendHead.Block_num = (Secs_SendInfo->Send_DataInfo.SendData_Ptr / 243);
    if (Secs_SendInfo->Send_DataInfo.SendData_Size == 0)
        Secs_SendHead.Block_num++;

    SecsI_ArrangeHead(&Secs_SendHead, Secs_SendBuf);
    SecsI_ArrangeChecksum(Secs_SendBuf);
    return (Secs_SerialSend(Secs_BSPInfo, Secs_SendBuf, Secs_SendBuf[0] + 3));
}
