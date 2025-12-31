#include "secs_bsp.h"

/*
串口发送函数，可根据不同平台进行条件编译
*/
int Secs_SerialSend(SecsBSPInfo *Secs_BSPInfo, uint8_t *send_buf, uint16_t buf_size) {
#ifdef __linux__
	serial_listener_write(Secs_BSPInfo->Secs_Serial, (char *)send_buf, (size_t)buf_size);
#else
	HAL_UART_Transmit(Secs_BSPInfo->Secs_Serial, send_buf, buf_size, 1000);
#endif
	return 0;
}

/*
定时器控制函数，可根据不同的平台进行条件编译
*/
int Secs_TimerCtrl(SecsBSPInfo *Secs_BSPInfo, uint8_t SecsTimer_en, uint16_t Secs_IntrTime) {
#ifdef __linux__
	if (SecsTimer_en == 0)
		timer_thread_start(Secs_BSPInfo->Secs_Timer, Secs_IntrTime*1000);
	else
		timer_thread_stop(Secs_BSPInfo->Secs_Timer);
#else
	if (SecsTimer_en == 1) {
		__HAL_TIM_SET_AUTORELOAD(Secs_BSPInfo->Secs_Timer, Secs_IntrTime*1000-1);
		HAL_TIM_Base_Start_IT(Secs_BSPInfo->Secs_Timer);
	}
	else {
		HAL_TIM_Base_Stop_IT(Secs_BSPInfo->Secs_Timer);
		__HAL_TIM_SET_AUTORELOAD(Secs_BSPInfo->Secs_Timer, Secs_IntrTime*1000-1);
	}
#endif
    return 0;
}


SecsRecvInfo Secs_RecvInfo1 = {0};
SecsBSPInfo  Secs_BSPInfo1  = {0};

void SecsSerial_RecvCallback1(const char *Serial_RecvBuf, size_t RecvBuf_Size) {
    if (Secs_RecvInfo1.SecsRecv_Flag) {
        printf("[error ] Secs Process the previous package of data");
    }
    else {
        if (Secs_RecvInfo1.SecsRecv_Buf != NULL) {
            free(Secs_RecvInfo1.SecsRecv_Buf);
            Secs_RecvInfo1.SecsRecv_Buf = NULL;
        }
        Secs_RecvInfo1.SecsRecv_Buf = (uint8_t *)malloc(RecvBuf_Size + 1);
        if (Secs_RecvInfo1.SecsRecv_Buf) {
            memcpy(Secs_RecvInfo1.SecsRecv_Buf, Serial_RecvBuf, RecvBuf_Size);
            Secs_RecvInfo1.SecsRecv_Flag = 1;
        }
    }
}

void SecsTimer_IntrCallback1(void) {
    Secs_RecvInfo1.SecsTimeout_Flag = 1;
}


SecsRecvInfo  Secs_RecvInfo2 = {0};
SecsRecvInfo  Secs_RecvInfo3  = {0};
SecsBSPInfo   Secs_BSPInfo2  = {0};
SecsBSPInfo   Secs_BSPInfo3  = {0};

// 当前帧已累积的字节数
size_t SecsRecv_BufSize = 0;

void SecsSerial_RecvCallback2(const char *Serial_RecvBuf, size_t RecvBuf_Size) {
	// 每次UARTLite只收1字节，RecvBuf_Size理论上=1
    if (RecvBuf_Size == 0 || Serial_RecvBuf == NULL) {
		printf("[error ] Secs Process no data to recv");
    }

	// 初始化接收缓存
	if (Secs_RecvInfo3.SecsRecv_Buf == NULL) {
		Secs_RecvInfo3.SecsRecv_Buf = (uint8_t *)malloc(256);  // 最长帧长度
		SecsRecv_BufSize = 0;
	}
	// 缓冲区追加数据
	if (SecsRecv_BufSize + RecvBuf_Size <= 256) {
		memcpy(Secs_RecvInfo3.SecsRecv_Buf + SecsRecv_BufSize, Serial_RecvBuf, RecvBuf_Size);
		SecsRecv_BufSize += RecvBuf_Size;
	}else {
        printf("[warn ] Buffer overflow, dropping byte\n");
    }	

    // 重启定时器（防止帧超时）
	timer_thread_start(Secs_BSPInfo3.Secs_Timer, 50);		
}

void SecsTimer_IntrCallback2(void) {
    Secs_RecvInfo2.SecsTimeout_Flag = 1;
}

void SecsTimer_IntrCallback3(void) {
	if (Secs_RecvInfo2.SecsRecv_Flag) {
        printf("[error ] Secs Process the previous package of data");
    }
    else {
        if (Secs_RecvInfo2.SecsRecv_Buf != NULL) {
            free(Secs_RecvInfo2.SecsRecv_Buf);
            Secs_RecvInfo2.SecsRecv_Buf = NULL;
        }
        Secs_RecvInfo2.SecsRecv_Buf = (uint8_t *)malloc(SecsRecv_BufSize + 1);
        if (Secs_RecvInfo2.SecsRecv_Buf) {
            memcpy(Secs_RecvInfo2.SecsRecv_Buf, Secs_RecvInfo3.SecsRecv_Buf, SecsRecv_BufSize);
            Secs_RecvInfo2.SecsRecv_Flag = 1;
			free(Secs_RecvInfo3.SecsRecv_Buf);
			Secs_RecvInfo3.SecsRecv_Buf = NULL;
			SecsRecv_BufSize = 0;
        }			
    }
	timer_thread_stop(Secs_BSPInfo3.Secs_Timer);
}

#ifdef __linux__

int init_secs_bsp(
	char *Secs_Serial_Device1,
	char *Secs_Serial_Device2
) {

	int secs_init_result = 0;

	//create serial
	serial_listener_t *Secs_Serial1 = (serial_listener_t *)malloc(sizeof(serial_listener_t));
	if (Secs_Serial1 == NULL) {
		printf("[error ] failed to allocate memory for Secs_Serial1\n");
		return -1;
	}

	secs_init_result = serial_listener_init(Secs_Serial1, "Secs_Serial1", Secs_Serial_Device1, SECE_UART_BAUDRATE, SecsSerial_RecvCallback1);
	if (secs_init_result < 0) {
		perror("[error ] create SECS serial port data receiving thread");
		return -1;
	}
	serial_listener_start(Secs_Serial1);
	printf("[  OK  ] create Secs Serial1 receive!\n");

	//create timer
	timer_thread_t *Secs_Timer1 = (timer_thread_t *)malloc(sizeof(timer_thread_t));
	if (Secs_Timer1 == NULL) {
		printf("[error ] failed to allocate memory for Secs_Timer1\n");
		free(Secs_Serial1);
		return -1;
	}
	secs_init_result = timer_thread_create(Secs_Timer1, "Secs_Timer1", SecsTimer_IntrCallback1);
	if (secs_init_result < 0) {
        perror("[error ] create SECS Timer thread!");
        return -1;
    }
	printf("[  OK  ] create secs timer1!\n");

	Secs_BSPInfo1.Secs_Serial = Secs_Serial1;
	Secs_BSPInfo1.Secs_Timer = Secs_Timer1;

		//create serial
	serial_listener_t *Secs_Serial2 = (serial_listener_t *)malloc(sizeof(serial_listener_t));
	if (Secs_Serial2 == NULL) {
		printf("[error ] failed to allocate memory for Secs_Serial2\n");
		return -1;
	}

	secs_init_result = serial_listener_init(Secs_Serial2, "Secs_Serial2", Secs_Serial_Device2, SECE_UART_BAUDRATE, SecsSerial_RecvCallback2);
	if (secs_init_result < 0) {
		perror("[error ] create SECS serial port data receiving thread");
		return -1;
	}
	serial_listener_start(Secs_Serial2);
	printf("[  OK  ] create Secs Serial receive!\n");

	//create timer
	timer_thread_t *Secs_Timer2 = (timer_thread_t *)malloc(sizeof(timer_thread_t));
	if (Secs_Timer2 == NULL) {
		printf("[error ] failed to allocate memory for Secs_Timer2\n");
		free(Secs_Serial2);
		return -1;
	}
	secs_init_result = timer_thread_create(Secs_Timer2, "Secs_Timer2", SecsTimer_IntrCallback2);
	if (secs_init_result < 0) {
        perror("[error ] create SECS2 Timer1 thread!");
        return -1;
    }
	printf("[  OK  ] create secs timer2!\n");

	//create timer for secs2 receive timeout
	timer_thread_t *Secs_Timer3 = (timer_thread_t *)malloc(sizeof(timer_thread_t));
	if (Secs_Timer3 == NULL) {
		printf("[error ] failed to allocate memory for Secs_Timer3\n");
		free(Secs_Serial2);
		return -1;
	}
	secs_init_result = timer_thread_create(Secs_Timer3, "Secs_Timer3", SecsTimer_IntrCallback3);
	if (secs_init_result < 0) {
        perror("[error ] create SECS2 Timer2 thread!");
        return -1;
    }
	printf("[  OK  ] create secs2 timer3!\n");

	Secs_BSPInfo2.Secs_Serial = Secs_Serial2;
	Secs_BSPInfo2.Secs_Timer = Secs_Timer2;
	Secs_BSPInfo3.Secs_Timer = Secs_Timer3;

	return 0;
}

#else

int init_secs_bsp(
	UART_HandleTypeDef *HalUart,
    TIM_HandleTypeDef *HalTimer
) {
	Secs_BSPInfo1.Secs_Serial = HalUart;
	Secs_BSPInfo1.Secs_Timer = HalTimer;
	return 0;
}


#endif
