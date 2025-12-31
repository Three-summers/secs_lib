#include "secs_II.h"

void SecsII_ResetArray(SendDataInfo *SecsArray) {
    SecsArray->SendData_Ctrl = 1;
    SecsArray->SendData_Buf = NULL;
    SecsArray->SendData_Size = 0;
    SecsArray->SendData_Ptr = 0;
}

void SecsII_AddByte(SendDataInfo *SecsArray, uint8_t value) {
    SecsArray->SendData_Buf =
        realloc(SecsArray->SendData_Buf, SecsArray->SendData_Size + 1);
    SecsArray->SendData_Buf[SecsArray->SendData_Size++] = value;
}

int SecsII_AddUint(SendDataInfo *SecsArray,
                   uint8_t ByteNum,
                   uint32_t UintValue) {
    switch (ByteNum) {
    case 0x01:
        SecsII_AddByte(SecsArray, SECS_UINT1);
        SecsII_AddByte(SecsArray, ByteNum);
        SecsII_AddByte(SecsArray, (uint8_t)UintValue);
        break;
    case 0x02:
        SecsII_AddByte(SecsArray, SECS_UINT2);
        SecsII_AddByte(SecsArray, ByteNum);
        uint16_t UintValue_trans = (uint16_t)UintValue;
        for (int i = 0; i < ByteNum; i++) {
            SecsII_AddByte(SecsArray,
                           (UintValue_trans >> (8 * (1 - i))) & 0xFF);
        }
        break;
    case 0x04:
        SecsII_AddByte(SecsArray, SECS_UINT4);
        SecsII_AddByte(SecsArray, ByteNum);
        for (int i = 0; i < ByteNum; i++) {
            SecsII_AddByte(SecsArray, (UintValue >> (8 * (3 - i))) & 0xFF);
        }
        break;
    // case 0x08:
    //     SecsII_AddByte(SecsArray, SECS_UINT8);
    //     break;
    default:
        printf("[error ] Invalid Secs unsigned data byte number\r\n");
        return -1;
        break;
    }
    return 0;
}

int SecsII_AddInt(SendDataInfo *SecsArray, uint8_t ByteNum, int32_t IntValue) {
    switch (ByteNum) {
    case 0x01:
        SecsII_AddByte(SecsArray, SECS_INT1);
        SecsII_AddByte(SecsArray, ByteNum);
        SecsII_AddByte(SecsArray, (int8_t)IntValue);
        break;
    case 0x02:
        SecsII_AddByte(SecsArray, SECS_INT2);
        SecsII_AddByte(SecsArray, ByteNum);
        int16_t IntValue_trans = (int16_t)IntValue;
        for (int i = 0; i < ByteNum; i++) {
            SecsII_AddByte(SecsArray, (IntValue_trans >> (8 * (1 - i))) & 0xFF);
        }
        break;
    case 0x04:
        SecsII_AddByte(SecsArray, SECS_INT4);
        SecsII_AddByte(SecsArray, ByteNum);
        for (int i = 0; i < ByteNum; i++) {
            SecsII_AddByte(SecsArray, (IntValue >> (8 * (3 - i))) & 0xFF);
        }
        break;
    // case 0x08:
    //     SecsII_AddByte(SecsArray, SECS_INT8);
    //     break;
    default:
        printf("[error ] Invalid Secs signed data byte number\r\n");
        return -1;
        break;
    }
    return 0;
}

int SecsII_AddFloat(SendDataInfo *SecsArray,
                    uint8_t ByteNum,
                    float FloatValue) {
    switch (ByteNum) {
    case 0x04:;
        union {
            float FloatData;
            uint8_t Bytes[4];
        } FloatConver;
        FloatConver.FloatData = FloatValue;
        SecsII_AddByte(SecsArray, SECS_FT4);
        SecsII_AddByte(SecsArray, ByteNum);
        for (int i = 0; i < ByteNum; i++) {
            SecsII_AddByte(SecsArray, FloatConver.Bytes[3 - i]);
        }
        break;
    case 0x08:;
        union {
            double DoubleData;
            uint8_t Bytes[8];
        } DoubleConver;
        DoubleConver.DoubleData = FloatValue;
        SecsII_AddByte(SecsArray, SECS_FT8);
        SecsII_AddByte(SecsArray, ByteNum);
        for (int i = 0; i < ByteNum; i++) {
            SecsII_AddByte(SecsArray, DoubleConver.Bytes[7 - i]);
        }
        break;
    default:
        printf("[error ] Invalid Secs float data byte number\r\n");
        return -1;
        break;
    }
    return 0;
}

// 提取 <L[ 之后 以及数据类型(U* I* F*)之后的数字，
int parse_number(char **ptr, uint8_t *num) {
    char *str = *ptr;
    char *end;
    errno = 0; // 重置错误标志
    long val = strtol(str, &end, 10);

    // 错误检查：无数字/溢出/超出范围
    if (end == str)
        return -1; // 无有效数字
    if (errno == ERANGE)
        return -1; // 数值溢出
    if (val < 0 || val > 0xFF)
        return -1; // 超出uint8_t范围

    *num = (uint8_t)val;
    *ptr = end; // 更新指针位置
    return 0;
}

int parse_signed(char **ptr, int32_t *num) {
    char *str = *ptr;
    char *end;
    errno = 0; // 重置错误标志
    long val = strtol(str, &end, 10);

    // 错误检查：无数字/溢出/超出范围
    if (end == str)
        return -1; // 无有效数字
    if (errno == ERANGE)
        return -1; // 数值溢出

    // 检查是否在int32_t范围内
    if (val < INT32_MIN || val > INT32_MAX)
        return -1;

    *num = (int32_t)val;
    *ptr = end; // 更新指针位置
    return 0;
}

int parse_unsigned(char **ptr, uint32_t *num) {
    char *str = *ptr;
    char *end;
    errno = 0; // 重置错误标志
    unsigned long val = strtoul(str, &end, 10);

    // 错误检查
    if (end == str)
        return -1; // 无有效数字
    if (errno == ERANGE)
        return -1; // 数值溢出
    if (val > UINT32_MAX)
        return -1; // 超出uint32_t范围

    *num = (uint32_t)val;
    *ptr = end; // 更新指针位置
    return 0;
}

int parse_float(char **ptr, float *num) {
    char *str = *ptr;
    char *end;
    errno = 0;                     // 重置错误标志
    float val = strtof(str, &end); // 修正参数传递

    // 错误检查
    if (end == str)
        return -1; // 无有效数字
    if (errno == ERANGE)
        return -1; // 数值溢出

    *num = val;
    *ptr = end; // 更新指针位置
    return 0;
}

int Secs_MessageArrange(char *Secs_string, SendDataInfo *SecsArray) {
    // 重置secs发送信息
    SecsII_ResetArray(SecsArray);
    uint8_t List_cnt = 0;

    while (*Secs_string) {
        /*
        检测<L[*]，以及提取*代表的数值
        */
        if (strncmp(Secs_string, "<L[", 3) == 0) {
            List_cnt++;
            Secs_string += 3;
            // 解析列表元素个数（1字节）
            uint8_t list_num;
            if (parse_number(&Secs_string, &list_num)) {
                printf("[error ] Invalid Secs list number\r\n");
                return -1;
            }
            if (*Secs_string != ']') {
                printf("[error ] Expected ']' after Secs list number\r\n");
                return -1;
            }
            Secs_string++;

            SecsII_AddByte(SecsArray, SECS_LIST);
            SecsII_AddByte(SecsArray, list_num); // 只添加1个字节
        }
        /*
        提取字符串
        检测<A，以及提取之后所跟的字符串
        */

        else if (strncmp(Secs_string, "<A ", 3) == 0) {
            Secs_string += 3;
            // 跳过空白字符
            while (isspace(*Secs_string))
                Secs_string++;

            if (*Secs_string != '"') {
                printf("[error ] Expected '\"' after <A\r\n");
                return -1;
            }
            Secs_string++;
            const char *str_start = Secs_string;

            // 先计算字符串长度
            size_t str_len = 0;
            while (*Secs_string && *Secs_string != '"') {
                str_len++;
                Secs_string++;
            }

            if (*Secs_string != '"') {
                printf("[error ] Unterminated string\n");
                return -1;
            }
            Secs_string++;

            if (*Secs_string != '>') {
                printf("[error ] Expected '>' after string\n");
                return -1;
            }
            Secs_string++;

            SecsII_AddByte(SecsArray, SECS_ASCII);
            SecsII_AddByte(SecsArray, str_len);
            for (size_t i = 0; i < str_len; i++) {
                SecsII_AddByte(SecsArray, str_start[i]);
            }
        }
        /*
        提取无符号数
        检测<U或<u，提取无符号数字节数，以及无符号数值
        */
        else if ((strncmp(Secs_string, "<U", 2) == 0) ||
                 (strncmp(Secs_string, "<u", 2) == 0)) {
            Secs_string += 2;
            uint8_t UintNum;
            if (parse_number(&Secs_string, &UintNum)) {
                printf("[error ] Invalid Secs unsigned data byte number\r\n");
                return -1;
            }
            Secs_string++;

            // 跳过空白字符
            while (isspace(*Secs_string))
                Secs_string++;

            // 读取无符号整数
            uint32_t UintValue;
            if (parse_unsigned(&Secs_string, &UintValue)) {
                printf("[error ] Invalid unsigned value\r\n");
                return -1;
            }

            if (*Secs_string != '>') {
                printf("[error] Expected '>' after unsigned\r\n");
                return -1;
            }
            Secs_string++;

            SecsII_AddUint(SecsArray, UintNum, UintValue);
        }
        /*
        提取有符号数
        检测<I或<i，提取有符号数字节数，以及有符号数值
        */
        else if ((strncmp(Secs_string, "<I", 2) == 0) ||
                 (strncmp(Secs_string, "<i", 2) == 0)) {
            Secs_string += 2;
            uint8_t IntNum;
            if (parse_number(&Secs_string, &IntNum)) {
                printf("[error ] Invalid Secs signed data byte number\r\n");
                return -1;
            }
            Secs_string++;

            // 跳过空白字符
            while (isspace(*Secs_string))
                Secs_string++;

            // 读取有符号整数
            int32_t IntValue;
            if (parse_signed(&Secs_string, &IntValue)) {
                printf("[error ] Invalid integer value\n");
                return -1;
            }

            if (*Secs_string != '>') {
                printf("[error ] Expected '>' after integer\n");
                return -1;
            }
            Secs_string++;

            SecsII_AddInt(SecsArray, IntNum, IntValue);
        }
        /*
        提取浮点数
        检测<F或<f，提取有符号数字节数，以及有符号数数值
        */
        else if (strncmp(Secs_string, "<F", 2) == 0) {
            Secs_string += 2;
            uint8_t FloatNum;
            if (parse_number(&Secs_string, &FloatNum)) {
                printf("[error ] Invalid Secs float data byte number\r\n");
                return -1;
            }
            Secs_string++;

            // 跳过空白字符
            while (isspace(*Secs_string))
                Secs_string++;

            // 读取浮点数
            float FloatValue;
            if (parse_float(&Secs_string, &FloatValue)) {
                printf("[error] Invalid float value\n");
                return -1;
            }

            if (*Secs_string != '>') {
                printf("[error ] Expected '>' after float\n");
                return -1;
            }
            Secs_string++;

            SecsII_AddFloat(SecsArray, FloatNum, FloatValue);
        } else if (strncmp(Secs_string, ">", 1) == 0) {
            if (List_cnt > 0) {
                List_cnt--;
                Secs_string++;
            } else {
                printf("[error ] more '>' detect\r\n");
                return -1;
            }
        } else if (isspace(*Secs_string)) {
            Secs_string++;
        } else {
            printf("[error ] Unknown token: %c\n", *Secs_string);
            return -1;
        }
    }
    return 0;
}

void SecsII_PrintArray(SendDataInfo *SecsArray) {
    printf("Generated SecsArray (%zu bytes):\n", SecsArray->SendData_Size);
    printf("{");
    for (size_t i = 0; i < SecsArray->SendData_Size; i++) {
        printf("0x%02X", SecsArray->SendData_Buf[i]);
        if (i != SecsArray->SendData_Size - 1)
            printf(", ");
        if ((i + 1) % 8 == 0)
            printf("\n ");
    }
    printf("};\n");
}

/*
int main() {
    // 示例输入字符串
    const char* input = "<L[4]\n<A \"Hello\">\n<I4 -12345>\n<U4
123456>\n<F4 3.14159>";

    SendDataInfo SecsArray;
    SecsII_ResetArray(&SecsArray);

    Secs_MessageArrange(input, &SecsArray);
    SecsII_PrintArray(&SecsArray);

    if (SecsArray.SendData_Buf) {
        free(SecsArray.SendData_Buf);
        SecsArray.SendData_Buf = NULL;
    }
    return EXIT_SUCCESS;
}
    0x01, 0x02, 0x41, 0x70, 0x72, 0x6F, 0x6A, 0x65,
 0x63, 0x74, 0x3A, 0x20, 0x25, 0x73, 0x41, 0x76,
 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x3A, 0x20,
 0x25, 0x73}
    */
