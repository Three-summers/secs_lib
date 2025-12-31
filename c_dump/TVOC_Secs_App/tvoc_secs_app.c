#include "tvoc_secs_app.h"
#include "../TVOC_hmi_app/tvoc_hmi_handler.h"

int Secs_MessageCtrl(
    SecsTransInfo *Secs_TransInfo) { // Validate input parameters
    if (Secs_TransInfo == NULL) {
        printf("[error ] Invalid input parameters\n");
        return -1;
    }

    // 释放之前分配的SecsSend_Message，防止内存泄漏
    if (Secs_TransInfo->SecsSend_Message) {
        free(Secs_TransInfo->SecsSend_Message);
        Secs_TransInfo->SecsSend_Message = NULL;
    }

    switch (Secs_TransInfo->SecsSend_Type.Stream_ID) {
    case SECS_S1:
        switch (Secs_TransInfo->SecsSend_Type.Function_ID) {
        case SECS_F2: {
            // S1F2
            char *Secs_S1F2 = "<L[2]"
                              "<A \"project: %s\">"
                              "<A \"version: %s\">"
                              ">";

            int needed_size =
                snprintf(NULL, 0, Secs_S1F2, SECSC_PROJECT_ID, VERSION_ID);

            if (needed_size < 0) {
                printf("[error] Formatting error for S1F2\n");
                return -1;
            }

            Secs_TransInfo->SecsSend_Message =
                (uint8_t *)malloc(needed_size + 1);

            if (!Secs_TransInfo->SecsSend_Message) {
                printf("[error] Memory allocation failed for S1F2\n");
                return -1;
            }

            snprintf((char *)Secs_TransInfo->SecsSend_Message,
                     needed_size + 1,
                     Secs_S1F2,
                     SECSC_PROJECT_ID,
                     VERSION_ID);

            break;
        }
        case SECS_F4: {
            // S1F4
            char *Secs_S1F4 = "<L[2]"
                              "<A \"01H1\">"
                              "<L[5]"
                              "<A \"%.2f\">"
                              "<A \"%.2f\">"
                              "<A \"%.2f\">"
                              "<A \"%.2f\">"
                              "<A \"%.2f\">"
                              ">"
                              ">";

            if (Secs_TransInfo->SecsSend_Type.SFType_NO == 0x00) {
                int needed_size = snprintf(NULL,
                                           0,
                                           Secs_S1F4,
                                           valve_voc_max[0],
                                           valve_voc_max[1],
                                           valve_voc_max[2],
                                           valve_voc_max[3],
                                           valve_voc_max[4]);

                if (needed_size < 0) {
                    printf("[error] Formatting error for S1F4\n");
                    return -1;
                }

                Secs_TransInfo->SecsSend_Message =
                    (uint8_t *)malloc(needed_size + 1);
                if (!Secs_TransInfo->SecsSend_Message) {
                    printf("[error] Memory allocation failed for S1F4\n");
                    return -1;
                }

                snprintf((char *)Secs_TransInfo->SecsSend_Message,
                         needed_size + 1,
                         Secs_S1F4,
                         valve_voc_max[0],
                         valve_voc_max[1],
                         valve_voc_max[2],
                         valve_voc_max[3],
                         valve_voc_max[4]);
            } else if (Secs_TransInfo->SecsSend_Type.SFType_NO == 0x01) {
                int needed_size = snprintf(NULL,
                                           0,
                                           Secs_S1F4,
                                           valve_voc_max[5],
                                           valve_voc_max[6],
                                           valve_voc_max[7],
                                           valve_voc_max[8],
                                           valve_voc_max[9]);

                if (needed_size < 0) {
                    printf("[error] Formatting error for S1F4\n");
                    return -1;
                }

                Secs_TransInfo->SecsSend_Message =
                    (uint8_t *)malloc(needed_size + 1);
                if (!Secs_TransInfo->SecsSend_Message) {
                    printf("[error] Memory allocation failed for S1F4\n");
                    return -1;
                }

                snprintf((char *)Secs_TransInfo->SecsSend_Message,
                         needed_size + 1,
                         Secs_S1F4,
                         valve_voc_max[5],
                         valve_voc_max[6],
                         valve_voc_max[7],
                         valve_voc_max[8],
                         valve_voc_max[9]);
            } else {
                printf("[error ] Invalid SFType_NO for S1F4\n");
                return -2;
            }

            break;
        }
        default:
            printf("[error] Unknown SECS type S%dF%d\n",
                   Secs_TransInfo->SecsSend_Type.Stream_ID,
                   Secs_TransInfo->SecsSend_Type.Function_ID);
            return -1;
        }
        break;

    case SECS_S5:
        switch (Secs_TransInfo->SecsSend_Type.Function_ID) {
        case SECS_F1: {
            // S5F1
            char *Secs_S5F1 = "<L[3]"
                              "<U2 %d>"
                              "<U2 %d>"
                              "<A \"%s\">"
                              ">";

            char *Secs_S5F1_Info = (char *)malloc(128 * sizeof(char));

            switch (Secs_TransInfo->SecsSend_Type.SFType_NO & 0xf0) {
            case 0x10:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING PID SENSOR OUT OF LIMITS");
                break;
            case 0x20:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING PM TOO HIGH");
                break;
            case 0x30:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING PM TOO LOW");
                break;
            case 0x40:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING PE TOO HIGH");
                break;
            case 0x50:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING PE TOO LOW");
                break;
            case 0x60:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING PD TOO HIGH");
                break;
            case 0x70:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING PD TOO LOW");
                break;
            case 0x80:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING FM TOO HIGH");
                break;
            case 0x90:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING FM TOO LOW");
                break;
            case 0xa0:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING FD TOO HIGH");
                break;
            case 0xb0:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "valve",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " RUNNING FD TOO LOW");
                break;
            case 0xc0:
                switch (Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f) {
                case 0x00:
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON PM2 TOO HIGH");
                    break;
                case 0x01:
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON PM2 TOO LOW");
                    break;
                case 0x02:
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON PM3 TOO LOW");
                    break;
                case 0x03:
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON FM2 TOO LOW");
                    break;
                case 0x04:
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON PD2 TOO HIGH");
                    break;
                case 0x05:
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON PD2 TOO LOW");
                    break;
                case 0x06:
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON PD3 TOO LOW");
                    break;
                case 0x07:
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON PD2 TOO LOW");
                    break;
                case 0x08:
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON VALVE ERROR");
                    break;
                case 0x09: // Added missing case for 0x09
                    sprintf(Secs_S5F1_Info, "%s", "POWER ON FM2 TOO HIGH");
                    break;
                default:
                    printf("[error] Unknown SECS type S%dF%d:%d\n",
                           Secs_TransInfo->SecsSend_Type.Stream_ID,
                           Secs_TransInfo->SecsSend_Type.Function_ID,
                           Secs_TransInfo->SecsSend_Type.SFType_NO);
                    return -1;
                    break;
                }
                break;
            case 0xd0:
                sprintf(Secs_S5F1_Info,
                        "%s%d%s",
                        "POWER ON VALVE",
                        Secs_TransInfo->SecsSend_Type.SFType_NO & 0x0f,
                        " CAN NOT OFF");
                break;
            default:
                printf("[error] Unknown SECS type S%dF%d:%d\n",
                       Secs_TransInfo->SecsSend_Type.Stream_ID,
                       Secs_TransInfo->SecsSend_Type.Function_ID,
                       Secs_TransInfo->SecsSend_Type.SFType_NO);
                return -1;
                break;
            }

            int needed_size = snprintf(NULL,
                                       0,
                                       Secs_S5F1,
                                       Secs_TransInfo->Message_ID,
                                       (uint16_t)CEID,
                                       Secs_S5F1_Info);

            if (needed_size < 0) {
                printf("[error] Formatting error for S5F1\n");
                free(Secs_S5F1_Info);
                return -1;
            }

            Secs_TransInfo->SecsSend_Message =
                (uint8_t *)malloc(needed_size + 1);
            if (!Secs_TransInfo->SecsSend_Message) {
                printf("[error] Memory allocation failed for S5F1\n");
                free(Secs_S5F1_Info);
                return -1;
            }

            snprintf((char *)Secs_TransInfo->SecsSend_Message,
                     needed_size + 1,
                     Secs_S5F1,
                     Secs_TransInfo->Message_ID++,
                     (uint16_t)CEID,
                     Secs_S5F1_Info);
            free(Secs_S5F1_Info);
            break;
        }
        default:
            printf("[error] Unknown SECS type S%dF%d\n",
                   Secs_TransInfo->SecsSend_Type.Stream_ID,
                   Secs_TransInfo->SecsSend_Type.Function_ID);
            return -1;
        }
        break;

    case SECS_S6:
        switch (Secs_TransInfo->SecsSend_Type.Function_ID) {
        case SECS_F11: {
            // S6F11 - Fixed the format string (was missing a >)
            char *Secs_S6F11 = "<L[3]"
                               "<U2 %d>"
                               "<U2 %d>"
                               "<L[2]"
                               "<A \"valve%d: PID|FM|PM|PE MAX|AVG\">"
                               "<L[8]"
                               "<A \"%.2f\">"
                               "<A \"%.2f\">"
                               "<A \"%.2f\">"
                               "<A \"%.2f\">"
                               "<A \"%.2f\">"
                               "<A \"%.2f\">"
                               "<A \"%.2f\">"
                               "<A \"%.2f\">"
                               ">"
                               ">"
                               ">";

            int needed_size = snprintf(NULL,
                                       0,
                                       Secs_S6F11,
                                       Secs_TransInfo->Message_ID,
                                       (uint16_t)CEID,
                                       Pipeline_Value.chan_num,
                                       Pipeline_Value.PIDMax,
                                       Pipeline_Value.PIDMean,
                                       Pipeline_Value.FlowMax,
                                       Pipeline_Value.FlowMean,
                                       Pipeline_Value.PressFMax,
                                       Pipeline_Value.PressFMean,
                                       Pipeline_Value.PressBMax,
                                       Pipeline_Value.PressBMean);

            if (needed_size < 0) {
                printf("[error] Formatting error for S6F11\n");
                return -1;
            }

            Secs_TransInfo->SecsSend_Message =
                (uint8_t *)malloc(needed_size + 1);
            if (!Secs_TransInfo->SecsSend_Message) {
                printf("[error] Memory allocation failed for S6F11\n");
                return -1;
            }

            snprintf((char *)Secs_TransInfo->SecsSend_Message,
                     needed_size + 1,
                     Secs_S6F11,
                     Secs_TransInfo->Message_ID++,
                     (uint16_t)CEID,
                     Pipeline_Value.chan_num,
                     Pipeline_Value.PIDMax,
                     Pipeline_Value.PIDMean,
                     Pipeline_Value.FlowMax,
                     Pipeline_Value.FlowMean,
                     Pipeline_Value.PressFMax,
                     Pipeline_Value.PressFMean,
                     Pipeline_Value.PressBMax,
                     Pipeline_Value.PressBMean);

            break;
        }
        default:
            printf("[error] Unknown SECS type S%dF%d\n",
                   Secs_TransInfo->SecsSend_Type.Stream_ID,
                   Secs_TransInfo->SecsSend_Type.Function_ID);
            return -1;
        }
        break;

    default:
        printf("[error] Unknown SECS type S%dF%d\n",
               Secs_TransInfo->SecsSend_Type.Stream_ID,
               Secs_TransInfo->SecsSend_Type.Function_ID);
        return -1;
    }

#if SECS_LOG
    if (Secs_TransInfo->SecsSend_Message) {
        printf("Generated SECS message: %s\n",
               Secs_TransInfo->SecsSend_Message);
    }
#endif
    return 0;
}

SecsTransInfo tvoc_secs1 = {0};
SecsTransInfo tvoc_secs2 = {0};

void SecsData_DealCallback1(SecsHead *SecsRecv_Head, uint8_t *SecsRecv_Buf) {
    if (SecsRecv_Head) {
        printf("recv secs S%dF%d\r\n",
               SecsRecv_Head->Secs_Type.Stream_ID,
               SecsRecv_Head->Secs_Type.Function_ID);
        if (SecsRecv_Head->Reply_Bit) {
            tvoc_secs1.SecsSend_Type.Stream_ID =
                SecsRecv_Head->Secs_Type.Stream_ID;
            tvoc_secs1.SecsSend_Type.Function_ID =
                SecsRecv_Head->Secs_Type.Function_ID + 1;
            tvoc_secs1.SecsSend_Type.SFType_NO =
                0x00; // Set a default SFType_NO
            if (Secs_MessageCtrl(&tvoc_secs1) == 0) {
                tvoc_secs1.SecsSend_Flag = 1;
            }
        }
        // Handle specific SECS messages
    }
}

void SecsData_DealCallback2(SecsHead *SecsRecv_Head, uint8_t *SecsRecv_Buf) {
    if (SecsRecv_Head) {
        printf("recv secs S%dF%d\r\n",
               SecsRecv_Head->Secs_Type.Stream_ID,
               SecsRecv_Head->Secs_Type.Function_ID);
        if (SecsRecv_Head->Reply_Bit) {
            tvoc_secs2.SecsSend_Type.Stream_ID =
                SecsRecv_Head->Secs_Type.Stream_ID;
            tvoc_secs2.SecsSend_Type.Function_ID =
                SecsRecv_Head->Secs_Type.Function_ID + 1;
            tvoc_secs2.SecsSend_Type.SFType_NO =
                0x01; // Set a default SFType_NO
            if (Secs_MessageCtrl(&tvoc_secs2) == 0) {
                tvoc_secs2.SecsSend_Flag = 1;
            }
        }
        // Handle specific SECS messages
    }
}

void *tvoc_Secs_Thread1(void *arg) {
    // secs_status.
    while (1) {
#if 0
        //错误信息发送
        for (uint8_t i = 0; i < TVOC_ERRORNUMBER / 2; i++) {
            if (tvoc_ErrorType[i] != 0x00 && tvoc_secs1.SecsSend_Flag == 0) {
                tvoc_secs1.SecsSend_Type.Stream_ID = SECS_S5;
                tvoc_secs1.SecsSend_Type.Function_ID = SECS_F1;
                tvoc_secs1.SecsSend_Type.SFType_NO = tvoc_ErrorType[i];
                Secs_MessageCtrl(&tvoc_secs1);
                tvoc_secs1.SecsSend_Flag = 1;
                usleep(10 * 1000);
            }
            if (tvoc_ErrorType[i] != 0x00 && tvoc_secs1.SecsSend_Flag == 0 && post_s1f1_flag) {
                post_s1f1_flag = false;
                tvoc_secs1.SecsSend_Type.Stream_ID = SECS_S1;
                tvoc_secs1.SecsSend_Type.Function_ID = SECS_F1;
                tvoc_secs1.SecsSend_Type.SFType_NO = tvoc_ErrorType[i];
                Secs_MessageCtrl(&tvoc_secs1);
                tvoc_secs1.SecsSend_Flag = 1;
                usleep(10 * 1000);
            }
        }
#endif

        usleep(1000);
    }
    return NULL;
}

void *tvoc_Secs_Thread2(void *arg) {
    // secs_status.
    while (1) {
#if 0
        //错误信息发送
        for (uint8_t i = TVOC_ERRORNUMBER / 2; i < TVOC_ERRORNUMBER; i++) {
            if (tvoc_ErrorType[i] != 0x00 && tvoc_secs2.SecsSend_Flag == 0) {
                tvoc_secs2.SecsSend_Type.Stream_ID = SECS_S5;
                tvoc_secs2.SecsSend_Type.Function_ID = SECS_F1;
                tvoc_secs2.SecsSend_Type.SFType_NO = tvoc_ErrorType[i];
                Secs_MessageCtrl(&tvoc_secs2);
                tvoc_secs2.SecsSend_Flag = 1;
                usleep(10 * 1000);
            }
        }
#endif

        usleep(1000);
    }
    return NULL;
}
// secs初始化
int tvoc_secs_init(void) {
    int secs_init_result;

    memset(&tvoc_secs1, 0, sizeof(SecsTransInfo));

    // secs报文内容设定
    secs_init_result = init_secs_app(&tvoc_secs1,
                                     &tvoc_secs2,
                                     DEVICE_ID,
                                     SecsData_DealCallback1,
                                     SecsData_DealCallback2);

    // Check for initialization errors
    if (secs_init_result != 0) {
        printf("[error ] initial tvoc_secs \r\n");
        return -1;
    }

    //创建secs处理线程
    pthread_t tvoc_Secs_Handler;
    secs_init_result =
        pthread_create(&tvoc_Secs_Handler, NULL, tvoc_Secs_Thread1, NULL);
    if (secs_init_result != 0) {
        perror("[error ] 创建 tvoc secs 1处理线程失败");
        return -1;
    }
    printf("[  OK  ] create tvoc secs 1 thread!\n");
    pthread_detach(tvoc_Secs_Handler);

    secs_init_result =
        pthread_create(&tvoc_Secs_Handler, NULL, tvoc_Secs_Thread2, NULL);
    if (secs_init_result != 0) {
        perror("[error ] 创建 tvoc secs 2处理线程失败");
        return -1;
    }
    printf("[  OK  ] create tvoc secs 2 thread!\n");
    pthread_detach(tvoc_Secs_Handler);

    return 0;
}
