/*
 * A55 -> R8 PWM 控制協定(2026-08-13)
 *
 * ⚠️ 本檔(repo 的 assets 版)是**唯一來源**,但兩側各持一份**拷貝**:
 *   CR8  : <e2studio 專案>/src/r8_pwm_proto.h
 *   CA55 : ~/r8_bench/r8_pwm_proto.h
 * **沒有任何機制保證那兩份拷貝同步** —— 這正是 `abi_ver` 存在的理由。
 * 改動時:改 repo 這份 → 複製到兩側 → **兩側都重編**,並用 md5 對一次:
 *   md5sum <專案>/src/r8_pwm_proto.h ~/r8_bench/r8_pwm_proto.h
 * 只重編一邊的話,`abi_ver` 會在兩個方向各攔一次(CR8 檢查 cmd、CA55 檢查 rsp),
 * 立刻報錯而不是靜默錯位(doc 13 §7.5 把這列為裸共用記憶體最致命的失效模式)。
 *
 * 全部欄位都是 uint32_t/int32_t,所以 ARM32(CR8)與 aarch64(CA55)的
 * sizeof 與 offset 完全一致,不需要 packed、也沒有 padding 差異。
 */
#ifndef R8_PWM_PROTO_H
#define R8_PWM_PROTO_H

#include <stdint.h>

#define PWM_CMD_MAGIC   0x434D5750u   /* 'P''W''M''C' little-endian */
#define PWM_RSP_MAGIC   0x525D5750u   /* 'P''W''M''R' 之變體,與 CMD 不同即可 */
#define JIT_CMD_MAGIC   0x5154494Au   /* 'J''I''T''Q' —— tick 抖動查詢 */
#define JIT_RSP_MAGIC   0x5352494Au   /* 'J''I''R''S' */

/* v1 -> v2:新增 jit_cmd_t / jit_rsp_t(tick 抖動量測)。
 * 結構本身沒動,但版本仍要推 —— 舊韌體收到 JITQ 只會當一般回聲送回去,
 * CA55 那邊看到的是回聲而不是統計,還不如讓 abi_ver 直接擋掉。 */
/* v2 -> v3:新增 sns_cmd_t / sns_rsp_t(R8 直讀 GPS 與羅盤)。
 * 結構本身沒動,但 A55 端不重編就會拿舊 sizeof 去比 —— 讓 abi_ver 擋掉。 */
/* v3 -> v4:sns_rsp_t 新增三個 I2C 診斷欄位。
 * 起因:第一版分不出「Open 失敗」「傳輸 NACK」「callback 沒來」,
 * 只看到 WHO_AM_I=0,查了很久。診斷欄位不是裝飾。 */
#define PWM_ABI_VER     4u

/* CA55 -> CR8。24 bytes。 */
typedef struct {
    uint32_t magic;          /* PWM_CMD_MAGIC */
    uint32_t abi_ver;        /* PWM_ABI_VER */
    uint32_t gpt;            /* GPT 編號:0,4,5,6,7,8,9(doc 15 §7.2 表) */
    uint32_t use_b;          /* 0 = A 輸出(GTIOCA), 1 = B 輸出(GTIOCB) */
    uint32_t freq_hz;        /* 目標頻率 */
    uint32_t duty_permille;  /* 佔空比,千分比 0..1000(用千分比而非百分比,
                              * 因為伺服訊號常見的 5.0%/7.5%/10.0% 需要小數點) */
} pwm_cmd_t;

/* CR8 -> CA55。24 bytes。 */
typedef struct {
    uint32_t magic;          /* PWM_RSP_MAGIC */
    uint32_t abi_ver;        /* PWM_ABI_VER */
    int32_t  status;         /* 0 = OK,負數見下 */
    uint32_t period_counts;  /* 實際寫進 GTPR 的值 */
    uint32_t duty_counts;    /* 實際寫進 GTCCRA/B 的值 */
    uint32_t pclk_hz;        /* CR8 用的 PCLK,讓 CA55 自己換算實際頻率/佔空比,
                              * 不必在兩邊各寫死一份常數 */
} pwm_rsp_t;

/* ===== tick 抖動量測(v2 新增)=========================================
 * R8 在 FreeRTOS tick hook(1 kHz)裡讀 GPT6 GTCNT(5 ns/count)算相鄰 tick
 * 的間隔,累積 min/max/總和與三個離群計數。比示波器好的地方:5 ns 解析度、
 * 可累積數百萬個 tick、離群直接數出來,不受示波器擷取深度限制。
 * 量的是 **ISR 層級**的抖動 —— 這是地板值,週期性 task 還要再加排程器那份。 */

/* CA55 -> CR8。12 bytes。 */
typedef struct {
    uint32_t magic;          /* JIT_CMD_MAGIC */
    uint32_t abi_ver;
    uint32_t reset;          /* 1 = 回報後把統計歸零,重新開始累積 */
} jit_cmd_t;

/* CR8 -> CA55。48 bytes。 */
typedef struct {
    uint32_t magic;          /* JIT_RSP_MAGIC */
    uint32_t abi_ver;
    uint32_t count;          /* 已納入統計的 tick 間隔數 */
    uint32_t nominal;        /* 標稱間隔(counts)= pclk_hz / tick_rate_hz */
    uint32_t min_d;          /* 最短間隔 */
    uint32_t max_d;          /* 最長間隔 */
    uint32_t sum_lo;         /* 間隔總和,64-bit 拆成兩個 32(算平均用) */
    uint32_t sum_hi;
    uint32_t n_1us;          /* |間隔 - 標稱| > 1 µs 的次數 */
    uint32_t n_10us;         /* > 10 µs */
    uint32_t n_100us;        /* > 100 µs */
    uint32_t pclk_hz;
} jit_rsp_t;


/* ===== 感測器讀取(v3 新增)==========================================
 * R8 直接讀 GPS(RSCI5 UART,J1 pin 8/10)與羅盤(RSCI7 I2C,J1 pin 3/5),
 * A55 用 SNSQ 取回最近一次快照。
 *
 * 設計上 R8 只回**原始事實**,不做詮釋:
 *   - 磁力計送原始 LSB,不換算成 µT(係數 0.3 µT/LSB 由 A55 套用)
 *   - 經緯度送 u-blox 原生的 1e-7 度整數,不轉浮點
 * 理由和 pwm_rsp_t 送 pclk_hz 而不送實際頻率一樣:換算規則只該有一份,
 * 而且 R8 上做浮點會拖慢即時迴圈。
 *
 * ⚠️ 一致性:各欄位單獨讀寫是原子的,但**跨欄位沒有快照一致性**
 *    (可能 lat 是新的、lon 是上一輪的)。5 Hz 遙測可接受;若日後要拿去
 *    做導航解算,要改成 seqlock。 */

#define SNS_CMD_MAGIC   0x51534E53u   /* 'S''N''S''Q' little-endian */
#define SNS_RSP_MAGIC   0x52534E53u   /* 'S''N''S''R' */

/* CA55 -> CR8。12 bytes。 */
typedef struct {
    uint32_t magic;          /* SNS_CMD_MAGIC */
    uint32_t abi_ver;
    uint32_t reset;          /* 1 = 回報後把計數器歸零 */
} sns_cmd_t;

/* CR8 -> CA55。92 bytes。 */
typedef struct {
    uint32_t magic;          /* SNS_RSP_MAGIC */
    uint32_t abi_ver;
    int32_t  status;         /* 0 = OK */

    /* --- GPS(u-blox M8N,UBX NAV-PVT)--- */
    uint32_t gps_frames;     /* 校驗通過的 UBX 幀累計 */
    uint32_t gps_ck_err;     /* 校驗失敗累計。與 frames 的比值就是鏈路品質 */
    uint32_t gps_drop;       /* UART 環形緩衝滿而丟棄的位元組 —— 非 0 代表 task 掏不贏 */
    uint32_t gps_age_ms;     /* 最後一筆 NAV-PVT 距今毫秒;0xFFFFFFFF = 從未收到 */
    uint32_t gps_fix;        /* fixType: 0=無, 2=2D, 3=3D, 4=GNSS+DR, 5=僅授時 */
    uint32_t gps_nsv;        /* 使用中的衛星數 */
    int32_t  gps_lat;        /* 1e-7 度 */
    int32_t  gps_lon;        /* 1e-7 度 */
    int32_t  gps_alt_mm;     /* 海拔(hMSL),mm */
    uint32_t gps_hacc_mm;    /* 水平精度估計,mm */
    uint32_t gps_pdop;       /* pDOP × 100 */
    uint32_t gps_itow;       /* GPS 週內秒,ms */

    /* --- 羅盤(IST8310)--- */
    uint32_t mag_ok;         /* 1 = WHO_AM_I 正確且初始化完成 */
    uint32_t mag_whoami;     /* 實讀的 WHO_AM_I。0x10 = IST8310 */
    uint32_t mag_samples;    /* 成功取樣累計 */
    uint32_t mag_err;        /* 失敗累計(逾時 / NACK / DRDY 沒來)*/
    uint32_t mag_age_ms;
    int32_t  mag_x;          /* 原始 LSB,×0.3 得 µT */
    int32_t  mag_y;
    int32_t  mag_z;
    uint32_t mag_open_err;   /* R_SCI_B_I2C_Open 的 fsp_err_t;0 = FSP_SUCCESS */
    uint32_t mag_last_evt;   /* 最後一次 I2C callback 的 event 值;0xFFFFFFFF = 從未進來 */
    uint32_t mag_timeouts;   /* i2c_wait() 逾時次數 —— 大於 0 代表 callback 沒來 */
} sns_rsp_t;

#define SNS_ERR_ABI  (-10)   /* 與 PWM_ERR_ABI 同值,語意相同 */

/* status 錯誤碼 */
#define PWM_ERR_BAD_GPT      (-1)   /* 不在 0/4/5/6/7/8/9 之列 */
#define PWM_ERR_ZERO_FREQ    (-2)
#define PWM_ERR_BAD_DUTY     (-3)   /* > 1000 */
#define PWM_ERR_NO_SUCH_PIN  (-4)   /* 該 GPT 沒有配置這個 A/B 輸出 */
#define PWM_ERR_RANGE        (-5)   /* 週期算出來 < 2 或超過 32-bit */
#define PWM_ERR_STOP         (-6)
#define PWM_ERR_PERIOD       (-7)
#define PWM_ERR_DUTY         (-8)
#define PWM_ERR_START        (-9)
#define PWM_ERR_ABI          (-10)  /* abi_ver 不合 —— 一邊重編了另一邊沒有 */

#endif /* R8_PWM_PROTO_H */
