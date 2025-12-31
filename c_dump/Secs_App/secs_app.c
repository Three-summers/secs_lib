#include "secs_app.h"

/*
secs 发送处理，
当 Secs_Info->secs_status.Secs_SendInfo.SendData_Ctrl
为0时，表明报文未转化成串口可发送的形式，需进一步转化， 若为1，则表示已经转换
*/
int Secs_SendHandle(SecsInfo *Secs_Info) {
    /*
    当 Secs_Info->Secs_SendInfo.Send_DataInfo.SendData_Ctrl
    为0时，表明报文未转化成串口可发送的形式，需进一步转化，
    若为1，则表示已经转换
    */
    if (Secs_Info->Secs_SendInfo.Send_DataInfo.SendData_Ctrl == 0) {
        if (Secs_Info->Secs_TransInfo->SecsSend_Message == NULL) {
            printf("1\r\n");
        }
        if (Secs_MessageArrange(
                (char *)Secs_Info->Secs_TransInfo->SecsSend_Message,
                &Secs_Info->Secs_SendInfo.Send_DataInfo) != 0) {
            Secs_Info->Secs_TransInfo->Secs_IsBusy = 0;
            printf("[error ] secs message arrange\r\n");
            return -1;
        } else {
#if SECS_LOG
            printf("secs message \r\n%s\r\n",
                   Secs_Info->Secs_TransInfo->SecsSend_Message);
            printf("successed trans to\r\n");
            SecsII_PrintArray(&Secs_Info->Secs_SendInfo.Send_DataInfo);
#endif
        }

        if (Secs_Info->Secs_TransInfo->SecsSend_Message) {
            free(Secs_Info->Secs_TransInfo->SecsSend_Message);
            Secs_Info->Secs_TransInfo->SecsSend_Message = NULL;
        }
    }

    Secs_Info->Secs_Status.Secs_WRFlag = SECS_WRITE;

    if (Secs_Info->Secs_SendInfo.Send_DataInfo.SendData_Buf == NULL ||
        Secs_Info->Secs_SendInfo.Send_DataInfo.SendData_Size == 0) {
        Secs_Info->Secs_TransInfo->Secs_IsBusy = 0;
        printf("[error ] secs send data size is 0\r\n");
        return -1;
    }

    if (SecsI_StateMachine(&Secs_Info->Secs_Status,
                           Secs_Info->Secs_BSPInfo,
                           &Secs_Info->Secs_SendInfo,
                           Secs_Info->Secs_RecvInfo->SecsRecv_Buf) != 0) {
        Secs_Info->Secs_TransInfo->Secs_IsBusy = 0;
        printf("[error ] secs send handle failed\r\n");
        return -1;
    }
    return 0;
}

/*
secs 读取数据处理函数
当需要回复时，会自动调用发送函数
当将测到发送的secs指令为S5F15时，表明在配置参数，这里提取配置参数的函数需要根据需求编写
*/
int Secs_RecvHandle(SecsInfo *Secs_Info) {
    Secs_Info->Secs_Status.Secs_WRFlag = SECS_READ;
    uint8_t Secs_OldState = Secs_Info->Secs_Status.Secs_State;

    if (SecsI_StateMachine(&Secs_Info->Secs_Status,
                           Secs_Info->Secs_BSPInfo,
                           &Secs_Info->Secs_SendInfo,
                           Secs_Info->Secs_RecvInfo->SecsRecv_Buf) == 0) {
        /*
        判断是否读取到数据包，如果成功读取到数据包，提取数据包头部信息
        */
        if (Secs_OldState == WAIT_BLOCK) {
            SecsHead Secs_RecvHead;
            SecsI_ExtractHead(Secs_Info->Secs_RecvInfo->SecsRecv_Buf,
                              &Secs_RecvHead);

            if (Secs_RecvHead.Reply_Bit && Secs_RecvHead.End_Bit) {
                Secs_Info->Secs_SendInfo.Systembyte_Info.SystemByte_Ctrl = 1;
                Secs_Info->Secs_SendInfo.Systembyte_Info.SystemByte_Maintain =
                    Secs_RecvHead.SystemByte;
            }

            Secs_Info->Secs_TransInfo->Secs_IsBusy = 0;

            Secs_Info->callback(&Secs_RecvHead,
                                Secs_Info->Secs_RecvInfo->SecsRecv_Buf);
        }
        /*
        判断是否需要多包发送，如需多包发送，需调用Secs_SendHandle函数
        */
        else if (Secs_OldState == WAIT_CHECK) {
            if (Secs_Info->Secs_SendInfo.Send_DataInfo.SendData_Size != 0) {
                Secs_SendHandle(Secs_Info);
            } else {
                Secs_Info->Secs_TransInfo->Secs_IsBusy = 0;
            }
        }
    } else {
        Secs_Info->Secs_TransInfo->Secs_IsBusy = 0;
        printf("[error ] secs recv handle failed\r\n");
        return -1;
    }

    return 0;
}

/*
secs定时器处理函数、
定时器到达设定时间后，调用此函数即可
*/
int Secs_TimerHandle(SecsInfo *Secs_Info) {
    Secs_Info->Secs_Status.Secs_WRFlag = SECS_TIMEOUT;
    if (SecsI_StateMachine(&Secs_Info->Secs_Status,
                           Secs_Info->Secs_BSPInfo,
                           &Secs_Info->Secs_SendInfo,
                           Secs_Info->Secs_RecvInfo->SecsRecv_Buf) == 0)
        return 0;
    else {
        Secs_Info->Secs_TransInfo->Secs_IsBusy = 0;
        printf("[error ] secs timer handle failed\r\n");
        return -1;
    }
}

#ifdef __linux__
void *Secs_Ctrl_Thread1(void *arg) {
    SecsInfo *Secs_Info = (SecsInfo *)arg;
    // secs_status.
    while (1) {
        if (Secs_Info->Secs_TransInfo->SecsSend_Flag &&
            !Secs_Info->Secs_TransInfo->Secs_IsBusy) {
            printf("secs send S%dF%d\r\n",
                   Secs_Info->Secs_TransInfo->SecsSend_Type.Stream_ID,
                   Secs_Info->Secs_TransInfo->SecsSend_Type.Function_ID);
            Secs_Info->Secs_SendInfo.SecsSend_Type =
                Secs_Info->Secs_TransInfo->SecsSend_Type;
            Secs_Info->Secs_SendInfo.Send_DataInfo.SendData_Ctrl = 0;
            Secs_Info->Secs_TransInfo->Secs_IsBusy = 1;
            Secs_SendHandle(Secs_Info);
            Secs_Info->Secs_TransInfo->SecsSend_Flag = 0;
        }

        if (Secs_Info->Secs_RecvInfo->SecsRecv_Flag) {
            Secs_Info->Secs_RecvInfo->SecsRecv_Flag = 0;
            if (Secs_Info->Secs_RecvInfo->SecsRecv_Buf == NULL) {
                printf("[error ] secs recv empty buf\r\n");
                continue;
            }
            Secs_Info->Secs_TransInfo->Secs_IsBusy = 1;
            Secs_RecvHandle(Secs_Info);
            free(Secs_Info->Secs_RecvInfo->SecsRecv_Buf);
            Secs_Info->Secs_RecvInfo->SecsRecv_Buf = NULL;
#if SECS_LOG
            printf("secs recv buf clear\r\n");
#endif
        }

        if (Secs_Info->Secs_RecvInfo->SecsTimeout_Flag) {
            Secs_Info->Secs_RecvInfo->SecsTimeout_Flag = 0;
            Secs_TimerHandle(Secs_Info);
        }

        usleep(1000);
    }
    return NULL;
}

void *Secs_Ctrl_Thread2(void *arg) {
    SecsInfo *Secs_Info = (SecsInfo *)arg;
    // secs_status.
    while (1) {
        if (Secs_Info->Secs_TransInfo->SecsSend_Flag &&
            !Secs_Info->Secs_TransInfo->Secs_IsBusy) {
            printf("secs send S%dF%d\r\n",
                   Secs_Info->Secs_TransInfo->SecsSend_Type.Stream_ID,
                   Secs_Info->Secs_TransInfo->SecsSend_Type.Function_ID);
            Secs_Info->Secs_SendInfo.SecsSend_Type =
                Secs_Info->Secs_TransInfo->SecsSend_Type;
            Secs_Info->Secs_SendInfo.Send_DataInfo.SendData_Ctrl = 0;
            Secs_Info->Secs_TransInfo->Secs_IsBusy = 1;
            Secs_SendHandle(Secs_Info);
            Secs_Info->Secs_TransInfo->SecsSend_Flag = 0;
        }

        if (Secs_Info->Secs_RecvInfo->SecsRecv_Flag) {
            Secs_Info->Secs_RecvInfo->SecsRecv_Flag = 0;
            if (Secs_Info->Secs_RecvInfo->SecsRecv_Buf == NULL) {
                printf("[error ] secs recv empty buf\r\n");
                continue;
            }
            Secs_Info->Secs_TransInfo->Secs_IsBusy = 1;
            Secs_RecvHandle(Secs_Info);
            free(Secs_Info->Secs_RecvInfo->SecsRecv_Buf);
            Secs_Info->Secs_RecvInfo->SecsRecv_Buf = NULL;
#if SECS_LOG
            printf("secs recv buf clear\r\n");
#endif
        }

        if (Secs_Info->Secs_RecvInfo->SecsTimeout_Flag) {
            Secs_Info->Secs_RecvInfo->SecsTimeout_Flag = 0;
            Secs_TimerHandle(Secs_Info);
        }

        usleep(1000);
    }
    return NULL;
}

int init_secs_app(SecsTransInfo *Secs_TransInfo1,
                  SecsTransInfo *Secs_TransInfo2,
                  uint16_t Secs_DeviceID,
                  void (*SecsData_DealCallback1)(SecsHead *Secs_Head,
                                                 uint8_t *data),
                  void (*SecsData_DealCallback2)(SecsHead *Secs_Head,
                                                 uint8_t *data)) {

    int secs_init_result = 0;

    SecsInfo *Secs_Info1 = (SecsInfo *)malloc(sizeof(SecsInfo));
    if (Secs_Info1 == NULL) {
        printf("[error ] failed to allocate memory for Secs_Info\n");
        return -1;
    }
    Secs_Info1->Secs_TransInfo = Secs_TransInfo1;
    Secs_Info1->Secs_RecvInfo = &Secs_RecvInfo1;
    Secs_Info1->Secs_BSPInfo = &Secs_BSPInfo1;
    Secs_Info1->Secs_BSPInfo->Device_ID = Secs_DeviceID;
    Secs_Info1->Secs_Status.Secs_State = IDLE_STA;
    Secs_Info1->Secs_Status.Secs_TryTimes = 0;
    Secs_Info1->callback = SecsData_DealCallback1;

    pthread_t Secs_Ctrl_Handler;
    secs_init_result =
        pthread_create(&Secs_Ctrl_Handler, NULL, Secs_Ctrl_Thread1, Secs_Info1);
    if (secs_init_result != 0) {
        perror("[error ] 创建Secs1处理线程失败");
        return -1;
    }
    printf("[  OK  ] create secs1 ctrl thread1!\n");
    pthread_detach(Secs_Ctrl_Handler);

    SecsInfo *Secs_Info2 = (SecsInfo *)malloc(sizeof(SecsInfo));
    if (Secs_Info2 == NULL) {
        printf("[error ] failed to allocate memory for Secs_Info\n");
        return -1;
    }
    Secs_Info2->Secs_TransInfo = Secs_TransInfo2;
    Secs_Info2->Secs_RecvInfo = &Secs_RecvInfo2;
    Secs_Info2->Secs_BSPInfo = &Secs_BSPInfo2;
    Secs_Info2->Secs_BSPInfo->Device_ID = Secs_DeviceID;
    Secs_Info2->Secs_Status.Secs_State = IDLE_STA;
    Secs_Info2->Secs_Status.Secs_TryTimes = 0;
    Secs_Info2->callback = SecsData_DealCallback2;

    pthread_t Secs_Ctrl_Handler2;
    secs_init_result = pthread_create(
        &Secs_Ctrl_Handler2, NULL, Secs_Ctrl_Thread2, Secs_Info2);
    if (secs_init_result != 0) {
        perror("[error ] 创建Secs2处理线程失败");
        return -1;
    }
    printf("[  OK  ] create secs2 ctrl thread2!\n");
    pthread_detach(Secs_Ctrl_Handler2);

    return 0;
}

#else

SecsInfo Secs_Info;

void Secs_Ctrl_Func(void) {
    // secs_status.
    if (Secs_Info.Secs_TransInfo->SecsSend_Flag &&
        !Secs_Info.Secs_TransInfo->Secs_IsBusy) {
        printf("secs send S%dF%d\r\n",
               Secs_Info.Secs_TransInfo->SecsSend_Type.Stream_ID,
               Secs_Info.Secs_TransInfo->SecsSend_Type.Function_ID);
        Secs_Info.Secs_SendInfo.SecsSend_Type =
            Secs_Info.Secs_TransInfo->SecsSend_Type;
        Secs_Info.Secs_SendInfo.Send_DataInfo.SendData_Ctrl = 0;
        Secs_Info.Secs_TransInfo->Secs_IsBusy = 1;
        Secs_SendHandle(&Secs_Info);
        Secs_Info.Secs_TransInfo->SecsSend_Flag = 0;
    }

    if (Secs_Info.Secs_RecvInfo->SecsRecv_Flag) {
        Secs_Info.Secs_RecvInfo->SecsRecv_Flag = 0;
        if (Secs_Info.Secs_RecvInfo->SecsRecv_Buf == NULL) {
            printf("[error ] secs recv empty buf\r\n");
            return;
        }
        Secs_Info.Secs_TransInfo->Secs_IsBusy = 1;
        Secs_RecvHandle(&Secs_Info);
        free(Secs_Info.Secs_RecvInfo->SecsRecv_Buf);
        Secs_Info.Secs_RecvInfo->SecsRecv_Buf = NULL;
#if SECS_LOG
        printf("secs recv buf clear\r\n");
#endif
    }

    if (Secs_Info.Secs_RecvInfo->SecsTimeout_Flag) {
        Secs_Info.Secs_RecvInfo->SecsTimeout_Flag = 0;
        Secs_TimerHandle(&Secs_Info);
    }

    return;
}

int init_secs_app(SecsTransInfo *Secs_TransInfo,
                  uint16_t Secs_DeviceID,
                  UART_HandleTypeDef *HalUart,
                  TIM_HandleTypeDef *HalTimer,
                  void (*SecsData_DealCallback)(SecsHead *Secs_Head,
                                                uint8_t *data)) {
    memset(&Secs_Info, 0, sizeof(SecsInfo));
    init_secs_bsp(HalUart, HalTimer);

    Secs_Info.Secs_TransInfo = Secs_TransInfo;
    Secs_Info.Secs_RecvInfo = &Secs_RecvInfo1;
    Secs_Info.Secs_BSPInfo = &Secs_BSPInfo1;
    Secs_Info.Secs_BSPInfo->Device_ID = Secs_DeviceID;
    Secs_Info.Secs_Status.Secs_State = IDLE_STA;
    Secs_Info.Secs_Status.Secs_TryTimes = 0;
    Secs_Info.callback = SecsData_DealCallback;
    return 0;
}
#endif
