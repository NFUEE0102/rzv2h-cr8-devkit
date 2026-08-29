/* A55 <-> CR8 core1 UART 展示通道協定(rpmsg,與 PWM 協定同一 endpoint 靠 magic 分派)。
 * 兩側各持一份拷貝,動結構就推 UART_ABI_VER 並兩側重編。
 * 尺寸紀律:rpmsg buffer 512B(payload 496B),cmd=400B / rsp=412B 皆留餘裕。 */
#ifndef R8_UART_PROTO_H
#define R8_UART_PROTO_H

#include <stdint.h>

#define UART_ABI_VER    (1u)
#define UART_CMD_MAGIC  (0x43524155u)   /* "UARC" little-endian */
#define UART_RSP_MAGIC  (0x53524155u)   /* "UARS" */

#define UART_OP_SEND    (0u)            /* data[len] -> UART TX */
#define UART_OP_QUERY   (1u)            /* 取 RX ring 內容 + 統計 */

#define UART_DATA_MAX   (384u)

/* status 錯誤碼 */
#define UART_ERR_ABI    (-1)
#define UART_ERR_BADOP  (-2)
#define UART_ERR_TXBUSY (-3)            /* 上一筆還在送,A55 稍後重試 */
#define UART_ERR_WRITE  (-4)

typedef struct
{
    uint32_t magic;
    uint32_t abi_ver;
    uint32_t op;
    uint32_t len;                       /* SEND:有效 data 長度 */
    uint8_t  data[UART_DATA_MAX];
} uart_cmd_t;

typedef struct
{
    uint32_t magic;
    uint32_t abi_ver;
    int32_t  status;
    uint32_t tx_total;                  /* 開機以來送出 bytes */
    uint32_t rx_total;                  /* 開機以來收到 bytes */
    uint32_t rx_dropped;                /* ring 滿丟棄 */
    uint32_t rx_len;                    /* 本回覆攜帶的 RX bytes */
    uint8_t  data[UART_DATA_MAX];
} uart_rsp_t;

#endif
