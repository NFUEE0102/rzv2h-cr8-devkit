/*
 * r8_bench: max-message-length + throughput/latency probe for the
 * CR8 core0 ch1 (rpmsg-service-1) echo channel on RZ/V2H.
 *
 * Reuses the platform_info.c / rz_rproc.c transport glue vendored from
 * the renesas-rdk Micro-XRCE-DDS-Agent (rzv2h-rpmsg-transport), which
 * targets the same UIO devices / vring physical layout as the FSP
 * rpmsg-echo demo (cr8_demo_patched.elf) currently loaded on cr8_0.
 * Only this file (main) is new; platform_info.c/rz_rproc.c/helper.c/
 * global_vars.c are unmodified copies of already-proven code.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <poll.h>      /* pwmd:stdin 輪詢,讓 force_stop 有機會被看到 */

#include <openamp/open_amp.h>
#include <openamp/rpmsg_virtio.h>
#include <metal/utilities.h>   /* metal_container_of(), used in cleanup */
#include "platform_info.h"
#include "helper.h"      /* copy_data(): device-memory-safe byte copy, see msg_cb() */
#include "rsc_table.h"   /* struct remote_resource_table, for release_channel() */
#include "r8_pwm_proto.h"
#include "r8_uart_proto.h"

/* ---- A55 -> R8 PWM 控制模式(2026-08-13,doc 16 §5.6)---- */
static int      g_sns_mode     = 0;   /* R8 感測器快照查詢 */
static int         g_sns_json  = 0;                  /* 常駐輪詢 + 寫 JSON */
static int         g_sns_ms    = 200;                /* 輪詢間隔,預設 5 Hz */
static const char *g_sns_path  = "/run/r8sns.json";
static int      g_sns_n        = 1;   /* 連續讀幾次 */
static int      g_pwm_mode     = 0;
static int         g_pwmd_mode = 0;                  /* 常駐 PWM daemon(網頁後端) */
static const char *g_pwmd_path = "/run/r8pwm.json";  /* 每筆指令的 R8 回報落地處 */
static int         g_uartd_mode = 0;                 /* UART 展示 daemon(core1)*/
static const char *g_uartd_path = "/run/r8uart.json";
static int      g_jit_mode     = 0;   /* tick 抖動量測 */
static int      g_jit_secs     = 10;
static int      g_jit_load     = 0;
static uint32_t g_pwm_gpt      = 9;   /* 預設 GPT9.B = J1 pin 40 = P97(doc 15 §7.2) */
static uint32_t g_pwm_useb     = 1;
static uint32_t g_pwm_freq     = 0;
static uint32_t g_pwm_permille = 0;

static const char *pwm_err_str(int32_t st)
{
    switch (st) {
        case PWM_ERR_BAD_GPT:     return "GPT 編號不在 0/4/5/6/7/8/9 之列";
        case PWM_ERR_ZERO_FREQ:   return "頻率不能是 0";
        case PWM_ERR_BAD_DUTY:    return "佔空比 > 100%";
        case PWM_ERR_NO_SUCH_PIN: return "該 GPT 沒有配置這個 A/B 輸出";
        case PWM_ERR_RANGE:       return "週期算出來超出 GTPR 可表示範圍(頻率太高或太低)";
        case PWM_ERR_STOP:        return "R_GPT_Stop 失敗";
        case PWM_ERR_PERIOD:      return "R_GPT_PeriodSet 失敗";
        case PWM_ERR_DUTY:        return "R_GPT_DutyCycleSet 失敗";
        case PWM_ERR_START:       return "R_GPT_Start 失敗";
        case PWM_ERR_ABI:         return "abi_ver 不合 —— 有一邊改了協定卻沒重編";
        default:                  return "(未知)";
    }
}

#include <fcntl.h>

/* ==== One-way decomposition via a time base BOTH cores can read ============
 * GPT6 GTCNT (0x13010648) free-runs at PCLK 200 MHz -> 5 ns/count, wrapping at
 * GTPR+1 (measured on board: GTPR=0x003D08FF -> 4,000,000 counts = 20 ms).
 * The CR8 firmware stamps T1..T4 into bytes 16..31 of the echoed buffer using
 * the SAME register at the SAME address (hw manual 1.8: one address map for
 * CA55 and CR8), so no clock-domain conversion and no oscilloscope is needed.
 * Round trips are ~100 us, three orders below the wrap, so aliasing is not a
 * concern; ipcts_us() still handles it so a stalled round cannot silently
 * produce a small bogus delta. */
#define GPT_PAGE     0x13010000UL
#define GTCNT_OFF    0x648
#define GTPR_OFF     0x664
static volatile unsigned char *g_gpt = NULL;
static unsigned long g_gpt_wrap = 4000000UL;
static unsigned int g_ts_a55_cb = 0;      /* T5: A55 rpmsg callback entry */
static double g_ts_readcost_us = 0.0;     /* cost of one ipcts_now(), calibrated */

static inline unsigned int ipcts_now(void)
{
    return g_gpt ? *(volatile unsigned int *)(g_gpt + GTCNT_OFF) : 0U;
}
static double ipcts_us(unsigned int a, unsigned int b)
{
    unsigned long d = (b >= a) ? (unsigned long)(b - a)
                               : (g_gpt_wrap - (unsigned long)a + (unsigned long)b);
    return (double)d * 0.005;   /* 200 MHz */
}
static int ipcts_init(void)
{
    /* GPT6 的時脈是由 R8 韌體開啟的。R8 還沒開之前,讀這個暫存器頁
     * **不是**讀到 0,而是直接 SIGBUS —— 在 RZ/V2H 上,讀一個時脈關閉的
     * 週邊是匯流排中止。下面那個 gtpr == 0 的檢查假設讀得到值,擋不住。
     * 剛重開機、或韌體卡在 GPT 初始化之前時就會踩到,而且會在印出任何
     * 訊息之前就死掉,看起來像整支程式壞了。
     * R8_BENCH_NOTS=1 可跳過時間戳探針,單純做 rpmsg 通訊。 */
    if (getenv("R8_BENCH_NOTS")) return -1;

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { printf("  ipcts: open /dev/mem failed: %s\n", strerror(errno)); return -1; }
    void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)GPT_PAGE);
    close(fd);
    if (p == MAP_FAILED) { printf("  ipcts: mmap failed: %s\n", strerror(errno)); return -1; }
    g_gpt = (volatile unsigned char *)p;
    unsigned int gtpr = *(volatile unsigned int *)(g_gpt + GTPR_OFF);
    unsigned int c0 = ipcts_now(), c1 = ipcts_now();
    if (gtpr == 0 || c0 == c1) {
        printf("  ipcts: GPT6 not counting (GTPR=0x%08x, GTCNT static) -- "
               "load a firmware that keeps GPT6 running\n", gtpr);
        g_gpt = NULL;
        return -1;
    }
    g_gpt_wrap = (unsigned long)gtpr + 1UL;
    /* Calibrate the read itself: it sits inside every measured interval.
     * Device-nGnRnE, so it is a real bus transaction, not a cached load. */
    unsigned int a = ipcts_now();
    for (int i = 0; i < 1000; i++) (void)ipcts_now();
    unsigned int b = ipcts_now();
    g_ts_readcost_us = ipcts_us(a, b) / 1001.0;
    printf("  ipcts: GPT6 time base OK (wrap=%lu counts = %.1f ms, "
           "GTCNT read cost = %.3f us)\n",
           g_gpt_wrap, g_gpt_wrap * 0.005 / 1000.0, g_ts_readcost_us);
    return 0;
}

/* defined further down; needed by d_report() above their definitions */
static int cmp_double(const void *a, const void *b);
static double pct(const double *sorted, int n, double p);

/* per-round decomposition */
static double *d_a2r, *d_isr2tk, *d_tk2cb, *d_cb2tx, *d_r2a, *d_cb2app, *d_rt;
static int d_n = 0;
static void d_report(const char *label)
{
    if (d_n <= 0) { printf("  (no decomposition samples)\n"); return; }
    struct { const char *name; double *v; } rows[] = {
        { "T0->T1  A55 send + MHU + R8 IRQ deliver", d_a2r    },
        { "T1->T2  R8  ISR -> task (1 hop)        ", d_isr2tk },
        { "T2->T3  R8  virtqueue -> callback      ", d_tk2cb  },
        { "T3->T4  R8  inside callback            ", d_cb2tx  },
        { "T4->T5  R8 send + MHU + Linux wake chn ", d_r2a    },
        { "T5->T6  A55 callback -> app thread     ", d_cb2app },
        { "T0->T6  round trip (cross-check)       ", d_rt     },
    };
    printf("\n  --- ONE-WAY DECOMPOSITION %s (n=%d, microseconds) ---\n", label, d_n);
    printf("  %-40s %9s %9s %9s %9s\n", "segment", "p50", "avg", "p99", "p99.9");
    double *tmp = malloc(sizeof(double) * d_n);
    for (unsigned r = 0; r < sizeof(rows)/sizeof(rows[0]); r++) {
        memcpy(tmp, rows[r].v, sizeof(double) * d_n);
        double sum = 0; for (int i = 0; i < d_n; i++) sum += tmp[i];
        qsort(tmp, d_n, sizeof(double), cmp_double);
        printf("  %-40s %9.2f %9.2f %9.2f %9.2f\n", rows[r].name,
               pct(tmp, d_n, 50.0), sum / d_n, pct(tmp, d_n, 99.0), pct(tmp, d_n, 99.9));
    }
    /* Self-check: the six segments must add up to the round trip. */
    double s6 = 0, srt = 0;
    for (int i = 0; i < d_n; i++) {
        s6  += d_a2r[i] + d_isr2tk[i] + d_tk2cb[i] + d_cb2tx[i] + d_r2a[i] + d_cb2app[i];
        srt += d_rt[i];
    }
    printf("  SUM CHECK: six segments avg=%.2f us  vs  round trip avg=%.2f us  (delta %.3f us)\n",
           s6 / d_n, srt / d_n, (s6 - srt) / d_n);
    free(tmp);
}


/* Declared in global_vars.c but not exposed via platform_info.h. */
extern bool valid_thread[MBX_CH_NUM];
extern pthread_key_t thkey;
extern int init_system(void);
extern void cleanup_system(void);
extern void init_global_vars(void);
extern void cleanup_global_vars(void);

/* ---- doc 13 §5 tuning knobs ----
 * Adopted by default (item 1 only -- it is a pure win, costs nothing):
 *   R8_BENCH_BYTECOPY=1  revert item 1, copy replies a byte at a time
 *
 * Opt-in, kept as measurement tools rather than production settings:
 *   R8_BENCH_POLL=1      item 2, busy-poll the vring instead of waiting on
 *                        the libmetal condvar handoff. Cuts 16B round trips
 *                        0.102 -> 0.053 ms but pins ~140% CPU while active;
 *                        on a 4-core A55 already running DRP-AI + vision +
 *                        ROS2 that is a bad trade for a 26 KB/s link. Still
 *                        useful as the empirical upper bound on how much
 *                        removing the wakeup chain can ever buy (doc 13 §5.3).
 *   R8_BENCH_RT=1        item 3, SCHED_FIFO + pin + mlockall. Measured
 *                        HARMFUL on this kernel -- see doc 13 §5.4. Left in
 *                        only so the finding stays reproducible.
 */
static int g_bytecopy, g_poll, g_rt;

#define SHUTDOWN_MSG 0xEF56A55AUL

/* Matches struct _payload in the FSP demo's rpmsg_demo.c exactly:
 * two `unsigned long` fields (16 bytes on LP64/aarch64) + data[]. */
struct _payload {
    unsigned long num;
    unsigned long size;
    unsigned char data[];
};

static const char *SVC_NAME = CFG_RPMSG_SVC_NAME1; /* "rpmsg-service-1" = CR8_0 ch1 */
static const unsigned int MBX_ID = UIO_RECEIVER2;  /* CR8 core0(預設;core1 改 UIO_RECEIVER3)*/  /* CR8 core0 */
/* Each per-channel resource_table slice (selected via proc_id/rsc_id in
 * platform_init) carries exactly one fw_rsc_vdev entry, so the vdev index
 * *within that slice* is always 0 -- the channel itself is already picked
 * by proc_id/rsc_id=1 above, not by this index (confirmed against
 * custom_agent.cpp, which also always passes 0 here regardless of channel). */
static const unsigned int VDEV_INDEX = 0;

static struct remoteproc *g_platform = NULL;
static struct rpmsg_endpoint g_ept = {0};
static volatile sig_atomic_t g_bound = 0;
static volatile sig_atomic_t g_reply_ready = 0;
static size_t g_reply_len = 0;
/* 8-byte aligned so copy_from_shm() can take the 64-bit path. */
static unsigned char g_reply_buf[4096] __attribute__((aligned(8)));

extern int force_stop; /* defined in global_vars.c */

static void unbind_cb(struct rpmsg_endpoint *ept)
{
    rpmsg_destroy_ept(ept);
    memset(ept, 0, sizeof(*ept));
    g_bound = 0;
}

/*
 * doc 13 §5 item 1 -- copy width.
 *
 * @src points straight into the vring RX buffer: a UIO mapping, i.e. ARM64
 * Device-nGnRnE. That means (a) unaligned and multi-register accesses fault
 * with SIGBUS(BUS_ADRALN) -- glibc's aarch64 memcpy() uses LDP/STP, which is
 * why plain memcpy() here killed the process on the very first reply -- and
 * (b) non-Gathering: every access is one separate, un-coalescable bus
 * transaction whose cost is dominated by round-trip latency, not bandwidth.
 *
 * So a byte-at-a-time loop (the vendored copy_data()) is correct but pays
 * one bus transaction per byte: 480 bytes = 480 transactions. Copying in
 * naturally-aligned 64-bit units is equally legal on Device memory and cuts
 * that to 60. `volatile` is mandatory either way -- without it the compiler
 * re-synthesises a memcpy() out of the loop and we are back to SIGBUS.
 *
 * TX needs no equivalent: OpenAMP's rpmsg_send() already writes into the
 * vring via metal_io_block_write().
 */
static void copy_from_shm(void *dst, const volatile void *src, size_t len)
{
    uintptr_t s = (uintptr_t)src, d = (uintptr_t)dst;

    /* Fall back to bytes if either side is misaligned (must never fault). */
    if (g_bytecopy || ((s | d) & 7u)) {
        copy_data((uint8_t *)dst, (const volatile uint8_t *)src, len);
        return;
    }

    size_t n8 = len >> 3;
    volatile const uint64_t *s64 = (volatile const uint64_t *)src;
    uint64_t *d64 = (uint64_t *)dst;
    for (size_t i = 0; i < n8; i++)
        d64[i] = s64[i];

    size_t done = n8 << 3;
    if (done < len)
        copy_data((uint8_t *)dst + done,
                  (const volatile uint8_t *)src + done, len - done);
}

static int msg_cb(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv)
{
    (void)ept; (void)src; (void)priv;
    g_ts_a55_cb = ipcts_now();   /* T5 */
    if (len > sizeof(g_reply_buf))
        len = sizeof(g_reply_buf);
    copy_from_shm(g_reply_buf, data, len);
    g_reply_len = len;
    g_reply_ready = 1;
    return RPMSG_SUCCESS;
}

static void ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
    if (!name || strcmp(name, SVC_NAME)) {
        LPERROR("unexpected NS name '%s' (want '%s')", name ? name : "(null)", SVC_NAME);
        return;
    }
    if (g_bound) {
        g_ept.dest_addr = dest;
        return;
    }
    int ret = rpmsg_create_ept(&g_ept, rdev, SVC_NAME, APP_EPT_ADDR, dest, msg_cb, unbind_cb);
    if (ret) {
        LPERROR("rpmsg_create_ept failed: %d", ret);
        return;
    }
    g_bound = 1;
    LPRINTF("endpoint bound (dest=0x%x)", dest);
}

/*
 * Hand this channel back so the CR8 firmware re-arms and re-advertises its
 * endpoint for the next run.
 *
 * Do NOT use the vendored platform_clear_driver_ok() for this: it clears
 * g_rsc_table, which is always resource-table *slice 0*, never the slice of
 * the channel actually in use. That is harmless in drone-px4 (its RPC channel
 * IS channel 0) but wrong for us on ch1 -- the firmware kept seeing DRIVER_OK
 * set, never noticed the disconnect, and every subsequent run timed out
 * waiting for the name-service announcement. platform_release_rpmsg_vdev()
 * gets this right (it uses rproc->rsc_table); we clear the same slice here.
 */
static void release_channel(void)
{
    if (g_platform && g_platform->rsc_table) {
        struct remote_resource_table *rt =
            (struct remote_resource_table *)g_platform->rsc_table;
        rt->rpmsg_vdev.status = 0x0;
    }
}

static void on_signal(int signo)
{
    (void)signo;
    force_stop = 1;
    release_channel();
}

static long ms_between(struct timespec *a, struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

static double ms_between_d(struct timespec *a, struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

/*
 * doc 13 §5 item 2 -- drop the wakeup chain.
 *
 * platform_poll() waits on a condvar that the libmetal IRQ thread signals.
 * The full receive path is therefore three scheduler wakeups per direction:
 *   MHU IRQ -> [kernel IRQ thread, uio.c uses request_threaded_irq]
 *           -> [libmetal IRQ pthread blocked in ppoll on the uio fd]
 *           -> [this thread, woken via pthread_cond_signal]
 * Six per round trip. Measurement (doc 12 §2.2) says that chain, not the
 * virtqueue bookkeeping, dominates the ~0.09 ms fixed overhead.
 *
 * remoteproc_get_notification() reads the vring used index directly, so
 * spinning on it collects the reply without waiting for any of those
 * wakeups -- the IRQ still fires and libmetal still services it, we just
 * stop being downstream of it. Bounded by the same timeout, so a lost
 * notification still ends the wait instead of hanging.
 */
static int wait_reply(int timeout_ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    g_reply_ready = 0;
    while (!g_reply_ready && !force_stop) {
        if (!g_poll)
            platform_poll(g_platform);                       /* condvar handoff */
        else
            remoteproc_get_notification(g_platform, RSC_NOTIFY_ID_ANY);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ms_between(&start, &now) > timeout_ms)
            return -1;
    }
    return g_reply_ready ? 0 : -1;
}

/*
 * doc 13 §5 item 3 -- take the scheduler out of the measurement.
 * SCHED_FIFO stops CFS preempting us mid-round-trip, the pin keeps us off a
 * core that migrates, and mlockall removes page-fault outliers. Each failure
 * is reported but non-fatal: without CAP_SYS_NICE the run still produces
 * valid (just noisier) numbers, and saying so beats silently degrading.
 */
static void apply_rt_tuning(void)
{
    struct sched_param sp;
    cpu_set_t set;

    if (!g_rt) {
        printf("RT tuning: off (measured harmful on this kernel, doc 13 §5.4;"
               " set R8_BENCH_RT=1 to reproduce)\n");
        return;
    }

    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 80;
    /* EPERM here even as root means CONFIG_RT_GROUP_SCHED=y: under cgroup v2
     * only the root cgroup may hold RT tasks, and we are in a session scope.
     * Report it plainly -- silently degrading to SCHED_OTHER while claiming
     * RT would poison every number below it. */
    printf("RT tuning: SCHED_FIFO 80 -> %s\n",
           sched_setscheduler(0, SCHED_FIFO, &sp) == 0 ? "OK" : strerror(errno));

    CPU_ZERO(&set);
    CPU_SET(3, &set);
    printf("RT tuning: pin CPU3   -> %s\n",
           sched_setaffinity(0, sizeof(set), &set) == 0 ? "OK" : strerror(errno));

    printf("RT tuning: mlockall   -> %s\n",
           mlockall(MCL_CURRENT | MCL_FUTURE) == 0 ? "OK" : strerror(errno));
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Percentiles, not just avg: doc 13 §6 T1 is a p99.9 threshold, and a mean
 * hides exactly the outliers a flight controller cares about. */
static double pct(const double *sorted, int n, double p)
{
    if (n <= 0) return 0.0;
    long idx = (long)(p * (n - 1) / 100.0 + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

static int send_and_wait(struct _payload *pl, size_t total, int timeout_ms)
{
    int ret = rpmsg_send(&g_ept, pl, total);
    if (ret < 0)
        return ret;
    return wait_reply(timeout_ms);
}


/* 把快照寫成 JSON。先寫暫存檔再 rename —— rename 在同一個檔案系統內是原子的,
 * 所以網頁那側永遠讀到完整內容,不會讀到寫了一半的檔。
 *
 * 換算(0.3 uT/LSB)在這裡做,和 sns 模式的印表一致:R8 只送原始事實,
 * 詮釋放 A55。頁面拿到的是已經換算好的值,但原始 LSB 也一併附上 ——
 * 展示時「這是真的暫存器讀數」比「這是漂亮的數字」有說服力。 */
static void sns_write_json(const sns_rsp_t *r, const char *path,
                           unsigned long long seq, double age_s)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;

    double mx = r->mag_x * 0.3, my = r->mag_y * 0.3, mz = r->mag_z * 0.3;
    double mag = sqrt(mx * mx + my * my + mz * mz);
    double hdg = atan2(my, mx) * 180.0 / M_PI;
    if (hdg < 0) hdg += 360.0;

    fprintf(f,
        "{\n"
        "  \"seq\": %llu,\n"
        "  \"host_uptime_s\": %.3f,\n"
        "  \"abi\": %u,\n"
        "  \"gps\": {\n"
        "    \"frames\": %u, \"ck_err\": %u, \"drop\": %u,\n"
        "    \"age_ms\": %u, \"fix\": %u, \"nsv\": %u,\n"
        "    \"lat\": %.7f, \"lon\": %.7f, \"alt_m\": %.2f,\n"
        "    \"hacc_m\": %.2f, \"pdop\": %.2f, \"itow\": %u\n"
        "  },\n"
        "  \"mag\": {\n"
        "    \"ok\": %u, \"whoami\": %u, \"samples\": %u, \"err\": %u,\n"
        "    \"age_ms\": %u, \"open_err\": %u, \"last_evt\": %u, \"timeouts\": %u,\n"
        "    \"raw\": [%d, %d, %d],\n"
        "    \"ut\": [%.2f, %.2f, %.2f],\n"
        "    \"field_ut\": %.2f, \"heading_deg\": %.1f\n"
        "  }\n"
        "}\n",
        seq, age_s, r->abi_ver,
        r->gps_frames, r->gps_ck_err, r->gps_drop,
        r->gps_age_ms, r->gps_fix, r->gps_nsv,
        r->gps_lat * 1e-7, r->gps_lon * 1e-7, r->gps_alt_mm / 1000.0,
        r->gps_hacc_mm / 1000.0, r->gps_pdop / 100.0, r->gps_itow,
        r->mag_ok, r->mag_whoami, r->mag_samples, r->mag_err,
        r->mag_age_ms, r->mag_open_err, r->mag_last_evt, r->mag_timeouts,
        r->mag_x, r->mag_y, r->mag_z,
        mx, my, mz, mag, hdg);

    fclose(f);
    (void) rename(tmp, path);   /* 原子替換 */
}

/* uartd:JSON 字串逃脫(可印 ASCII 直出;控制/非 ASCII -> \u00XX)*/
static void uartd_json_escape(FILE *f, const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')      fprintf(f, "\\%c", c);
        else if (c == '\n')             fputs("\\n", f);
        else if (c == '\r')             fputs("\\r", f);
        else if (c < 0x20 || c > 0x7E)  fprintf(f, "\\u%04x", c);
        else                            fputc(c, f);
    }
}

/* uartd:送一筆 uart_cmd、等回覆。回 0 = rsp 有效。 */
static int uartd_xfer(const uart_cmd_t *cmd, uart_rsp_t *rsp)
{
    int sret = RPMSG_ERR_NO_BUFF;
    for (int a = 0; a < 200 && !force_stop; a++) {
        sret = rpmsg_trysend(&g_ept, cmd, sizeof(*cmd));
        if (sret != RPMSG_ERR_NO_BUFF) break;
        struct timespec d = { .tv_sec = 0, .tv_nsec = 1000 * 1000L };
        nanosleep(&d, NULL);
        platform_poll(g_platform);
    }
    if (sret < 0 || force_stop) return -1;
    if (wait_reply(1000) != 0) return -1;
    if (g_reply_len < sizeof(uart_rsp_t)) return -1;
    memcpy(rsp, g_reply_buf, sizeof(*rsp));
    if (UART_RSP_MAGIC != rsp->magic || UART_ABI_VER != rsp->abi_ver) return -1;
    return 0;
}

/* pwmd:把一筆指令與 R8 的回覆(或錯誤)寫成單行 JSON,tmp+rename 原子替換。
 * 與 sns_write_json 相同的落地手法,讀方(serve_pwm.py)靠 seq 判斷新舊。 */
static void pwmd_write_json(const char *path, unsigned long long seq,
                            const pwm_cmd_t *c, const pwm_rsp_t *r, const char *err)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    if (r && 0 == r->status) {
        double af = r->period_counts ? (double)r->pclk_hz / (double)r->period_counts : 0.0;
        double ad = r->period_counts ? 100.0 * (double)r->duty_counts / (double)r->period_counts : 0.0;
        fprintf(f,
            "{\"seq\":%llu,\"ok\":1,\"gpt\":%u,\"ch\":\"%c\",\"req_freq_hz\":%u,"
            "\"req_duty_permille\":%u,\"period_counts\":%u,\"duty_counts\":%u,"
            "\"pclk_hz\":%u,\"actual_freq_hz\":%.4f,\"actual_duty_pct\":%.4f}\n",
            seq, c->gpt, c->use_b ? 'b' : 'a', c->freq_hz, c->duty_permille,
            r->period_counts, r->duty_counts, r->pclk_hz, af, ad);
    } else {
        fprintf(f,
            "{\"seq\":%llu,\"ok\":0,\"gpt\":%u,\"ch\":\"%c\",\"req_freq_hz\":%u,"
            "\"req_duty_permille\":%u,\"err\":\"%s\",\"status\":%d}\n",
            seq, c->gpt, c->use_b ? 'b' : 'a', c->freq_hz, c->duty_permille,
            err ? err : (r ? pwm_err_str(r->status) : "?"), r ? r->status : 0);
    }
    fclose(f);
    rename(tmp, path);
}

int main(int argc, char **argv)
{
    int ret;
    int n_rounds = 500;
    setbuf(stdout, NULL);
    if (argc > 1 && 0 == strcmp(argv[1], "jitter")) {
        g_jit_mode = 1;
        if (argc > 2) g_jit_secs = atoi(argv[2]);
        if (g_jit_secs <= 0) g_jit_secs = 10;
        if (argc > 3 && 0 == strcmp(argv[3], "load")) g_jit_load = 1;
    }
    if (argc > 1 && 0 == strcmp(argv[1], "snsjson")) {
        g_sns_mode = 1; g_sns_json = 1;
        if (argc > 2) g_sns_ms = atoi(argv[2]);
        if (g_sns_ms < 50) g_sns_ms = 50;      /* 別把 rpmsg 打爆 */
        if (argc > 3) g_sns_path = argv[3];
    }
    if (argc > 1 && 0 == strcmp(argv[1], "pwmd")) {
        g_pwmd_mode = 1;
        if (argc > 2) g_pwmd_path = argv[2];
    }
    if (argc > 1 && 0 == strcmp(argv[1], "uartd")) {
        g_uartd_mode = 1;
        if (argc > 2) g_uartd_path = argv[2];
    }
    if (argc > 1 && 0 == strcmp(argv[1], "sns")) {
        g_sns_mode = 1;
        if (argc > 2) g_sns_n = atoi(argv[2]);
        if (g_sns_n <= 0) g_sns_n = 1;
    }
    if (argc > 1 && 0 == strcmp(argv[1], "pwm")) {
        if (argc < 6) {
            fprintf(stderr,
                "用法: %s pwm <gpt> <a|b> <freq_hz> <duty_%%>\n"
                "  例: %s pwm 9 b 1000 25     # J1 pin 40 (P97 = GPT9.B),1 kHz,25%%\n"
                "  例: %s pwm 9 b 50 7.5      # 伺服中立 1.5 ms @ 50 Hz\n"
                "  GPT -> J1 腳位對照見 doc 15 §7.2\n",
                argv[0], argv[0], argv[0]);
            return 2;
        }
        g_pwm_mode = 1;
        g_pwm_gpt  = (uint32_t)strtoul(argv[2], NULL, 0);
        g_pwm_useb = ('b' == argv[3][0] || 'B' == argv[3][0]) ? 1u : 0u;
        g_pwm_freq = (uint32_t)strtoul(argv[4], NULL, 0);
        {   /* 用千分比,伺服常見的 7.5% 需要小數點 */
            double pc = atof(argv[5]);
            if (pc < 0.0)   pc = 0.0;
            if (pc > 100.0) pc = 100.0;
            g_pwm_permille = (uint32_t)(pc * 10.0 + 0.5);
        }
    }
    ipcts_init(); /* so output survives if we hit the known abort() in teardown */

    g_bytecopy = getenv("R8_BENCH_BYTECOPY") != NULL;
    g_poll     = getenv("R8_BENCH_POLL")     != NULL;
    g_rt       = getenv("R8_BENCH_RT")       != NULL;
    printf("config: copy=%s  wait=%s  rt=%s\n",
           g_bytecopy ? "byte"    : "64-bit",
           g_poll     ? "busy-poll" : "condvar",
           g_rt       ? "on"        : "off");
    apply_rt_tuning();
    if (!g_pwm_mode && !g_pwmd_mode && !g_uartd_mode && !g_jit_mode && !g_sns_mode && argc > 1)
        n_rounds = atoi(argv[1]);
    if (n_rounds <= 0)
        n_rounds = 500;

    ret = init_system();
    if (ret) {
        fprintf(stderr, "init_system failed: %d\n", ret);
        return 1;
    }
    init_global_vars();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Register this (single, main) thread as the CR8_0 poller so
     * platform_poll()/LPRINTF can find their thread-local IPI slot. */
    int *idx = malloc(sizeof(int));
    *idx = (int)MBX_ID;
    valid_thread[MBX_ID] = true;
    pthread_setspecific(thkey, idx);
    g_tid_cr8_0 = (pid_t)syscall(SYS_gettid);

    /* platform_create_proc() mmaps the WHOLE resource-table region (all
     * MBX_CH_NUM slices) only on the very first platform_init() call in
     * this process, and on that first call it ALWAYS hands back slice 0
     * regardless of the rsc_index you asked for -- subsequent calls then
     * correctly index g_rsc_table[rsc_index] against the cached mapping.
     * So priming with channel 0 first is required before we can reach
     * channel 1's (ch1's) actual resource-table slice. Confirmed by
     * dumping the raw table: without this priming call, rsc_id=1 silently
     * returned channel 0's vring addresses (0x43000000/0x43050000)
     * instead of channel 1's (0x43100000/0x43150000). */
    struct remoteproc *prime = NULL;
    ret = platform_init(0, 0, MBX_ID, &prime);
    if (ret) {
        fprintf(stderr, "platform_init (priming, channel 0) failed: %d\n", ret);
        return 1;
    }

    ret = platform_init(1 /*proc_id*/, 1 /*rsc_id*/, MBX_ID, &g_platform);
    if (ret) {
        fprintf(stderr, "platform_init failed: %d\n", ret);
        return 1;
    }

    if (getenv("R8_BENCH_DUMP_RSC")) {
        unsigned char *raw = (unsigned char *)g_platform->rsc_table;
        printf("DEBUG raw resource table @ %p, rsc_len=%zu:\n", (void*)raw, g_platform->rsc_len);
        size_t dump_len = g_platform->rsc_len < 128 ? g_platform->rsc_len : 128;
        for (size_t i = 0; i < dump_len; i++) {
            printf("%02x ", raw[i]);
            if (i % 16 == 15) printf("\n");
        }
        printf("\n");
        unsigned int *w = (unsigned int *)raw;
        printf("as u32: version=%u num=%u reserved=[%u,%u] offset[0]=%u offset[1]=%u\n",
               w[0], w[1], w[2], w[3], w[4], w[5]);
    }

    struct rpmsg_device *rdev = platform_create_rpmsg_vdev(
        g_platform, VDEV_INDEX, VIRTIO_DEV_MASTER, NULL, ns_bind_cb);
    if (!rdev) {
        fprintf(stderr, "platform_create_rpmsg_vdev failed\n");
        platform_cleanup(g_platform);
        return 1;
    }

    printf("Waiting for CR8_0 ch1 ('%s') to advertise (cr8_demo_patched.elf must be running)...\n", SVC_NAME);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!g_bound && !force_stop) {
        platform_poll(g_platform);
        usleep(1000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (ms_between(&t0, &t1) > 15000) {
            fprintf(stderr, "Timed out waiting for NS bind. Is cr8_0 running the rpmsg demo firmware?\n");
            goto cleanup;
        }
    }
    if (force_stop)
        goto cleanup;
    while (!is_rpmsg_ept_ready(&g_ept) && !force_stop)
        platform_poll(g_platform);
    printf("Endpoint ready.\n\n");

    if (g_jit_mode) {
        jit_cmd_t  jc;
        jit_rsp_t  jr;
        struct timespec w0, w1;

        jc.magic = JIT_CMD_MAGIC; jc.abi_ver = PWM_ABI_VER; jc.reset = 1u;

        /* 第一發:歸零並確認韌體支援 */
        if (rpmsg_trysend(&g_ept, &jc, sizeof(jc)) < 0 || wait_reply(1000) != 0) {
            fprintf(stderr, "抖動查詢送不出去/沒回覆\n"); goto shutdown;
        }
        if (g_reply_len < sizeof(jit_rsp_t)) {
            fprintf(stderr, "回覆太短(%zu < %zu)—— 韌體沒有抖動探針,載入 cr8_jitter.elf\n",
                    (size_t)g_reply_len, sizeof(jit_rsp_t));
            goto shutdown;
        }
        memcpy(&jr, g_reply_buf, sizeof(jr));
        if (JIT_RSP_MAGIC != jr.magic) {
            fprintf(stderr, "回覆 magic 0x%08x —— 韌體沒有抖動探針,載入 cr8_jitter.elf\n", jr.magic);
            goto shutdown;
        }
        if (PWM_ABI_VER != jr.abi_ver) {
            fprintf(stderr, "abi_ver 不合(韌體 %u,本程式 %u)—— 兩側要用同一份 proto 重編\n",
                    jr.abi_ver, PWM_ABI_VER);
            goto shutdown;
        }

        printf("統計已歸零,量測 %d 秒%s ...\n",
               g_jit_secs, g_jit_load ? "(同時打 rpmsg 負載)" : "(閒置)");

        /* ⚠️ 負載模式一定要走 busy-poll,不能用 condvar。
         * wait_reply() 的逾時檢查排在 platform_poll() **之後**,而 condvar 模式下
         * platform_poll() 會無限阻塞 —— 只要漏掉一次通知,逾時永遠跑不到,行程整個卡住,
         * 接著被外部 timeout 硬砍,然後 MHU ch3 卡在中斷 set(doc 16 §2.2.2)。
         * 實測踩過一次。remoteproc_get_notification() 不阻塞,逾時才真的有效。 */
        int g_jit_saved_poll = g_poll;
        if (g_jit_load) g_poll = 1;

        clock_gettime(CLOCK_MONOTONIC, &w0);
        long long load_msgs = 0;
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &w1);
            if (ms_between(&w0, &w1) >= (long)g_jit_secs * 1000L) break;
            if (force_stop) break;
            if (g_jit_load && load_msgs < 3000) {
                /* 打回聲流量:每一發都會在 R8 觸發一次 MHU ISR,
                 * 也就是 tick ISR 的直接競爭者。
                 * ⚠️ 預算上限 3000 —— shm pool 是只進不出的 bump allocator
                 * (doc 13 §6.2),無上限地打會把 TX buffer 用光,連最後那發
                 * 統計查詢與 SHUTDOWN_MSG 都送不出去。3000 是 r8_bench 已驗證
                 * 安全的量級;打完就停止施壓、讓窗口剩下的時間繼續累積 tick。 */
                unsigned char lb[64];
                memset(lb, 0x5A, sizeof(lb));
                if (rpmsg_trysend(&g_ept, lb, sizeof(lb)) >= 0) {
                    if (wait_reply(200) == 0) load_msgs++;
                } else {
                    load_msgs = 3000;   /* 拿不到 buffer 就別再試 */
                }
            } else {
                struct timespec d = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000L };
                nanosleep(&d, NULL);
            }
        }

        g_poll = g_jit_saved_poll;

        /* 第二發:取統計,不歸零。
         * ⚠️ 要驗 magic —— 負載模式下可能有遲到的回聲先把 g_reply_ready 設起來,
         * 那時讀到的是 0x5A 填充而不是統計。驗不過就再要一次。 */
        jc.reset = 0u;
        if (rpmsg_trysend(&g_ept, &jc, sizeof(jc)) < 0 || wait_reply(1000) != 0) {
            fprintf(stderr, "取統計失敗\n"); goto shutdown;
        }
        memcpy(&jr, g_reply_buf, sizeof(jr));
        for (int retry = 0; JIT_RSP_MAGIC != jr.magic && retry < 5; retry++) {
            fprintf(stderr, "  (收到非統計回覆 magic=0x%08x,重要一次)\n", jr.magic);
            if (rpmsg_trysend(&g_ept, &jc, sizeof(jc)) < 0 || wait_reply(1000) != 0) break;
            memcpy(&jr, g_reply_buf, sizeof(jr));
        }
        if (JIT_RSP_MAGIC != jr.magic) {
            fprintf(stderr, "取統計失敗:一直收到非統計回覆\n"); goto shutdown;
        }

        {
            double us_per_cnt = 1e6 / (double)jr.pclk_hz;      /* 200MHz -> 0.005 us */
            double nom_us = jr.nominal * us_per_cnt;
            double min_us = jr.min_d   * us_per_cnt;
            double max_us = jr.max_d   * us_per_cnt;
            double sum    = (double)jr.sum_hi * 4294967296.0 + (double)jr.sum_lo;
            double avg_us = jr.count ? (sum / jr.count) * us_per_cnt : 0.0;

            printf("\n  === R8 FreeRTOS tick ISR 抖動 (n = %u) ===\n", jr.count);
            if (g_jit_load) printf("  量測窗內完成 rpmsg 往返 %lld 次\n", load_msgs);
            printf("  標稱週期  %.3f us (%u counts @ %u Hz)\n", nom_us, jr.nominal, jr.pclk_hz);
            printf("  實際平均  %.3f us   (偏差 %+.3f us)\n", avg_us, avg_us - nom_us);
            printf("  最短      %.3f us   (%+.3f us)\n", min_us, min_us - nom_us);
            printf("  最長      %.3f us   (%+.3f us)\n", max_us, max_us - nom_us);
            printf("  峰對峰    %.3f us\n", max_us - min_us);
            printf("  離群      >1us: %u (%.4f%%)   >10us: %u (%.4f%%)   >100us: %u (%.4f%%)\n",
                   jr.n_1us,   jr.count ? 100.0*jr.n_1us/jr.count   : 0.0,
                   jr.n_10us,  jr.count ? 100.0*jr.n_10us/jr.count  : 0.0,
                   jr.n_100us, jr.count ? 100.0*jr.n_100us/jr.count : 0.0);
            printf("\n  註:這是 **ISR 層級**的抖動,是地板值 ——\n");
            printf("      週期性 task 還要再加 FreeRTOS 排程器那一份。\n");
        }
        goto shutdown;
    }


    if (g_sns_json) {
        struct timespec b0, b1;
        unsigned long long seq = 0;
        int consecutive_fail = 0;

        /* 這裡**刻意不強制 busy-poll**。
         *
         * main.c 上方那段警告(condvar 下 platform_poll() 會無限阻塞,
         * 逾時永遠跑不到)是針對抖動量測滿載寫的。套到常駐輪詢反而更糟,
         * 實測:
         *     condvar     存活 2055 次取樣(411 秒)
         *     busy-poll   存活  686 次取樣(137 秒)
         *     busy-poll + 回聲基準  第一輪就 communication abort(0/20000)
         * 而且用 busy-poll 跑回聲時,新舊兩顆韌體都是 0/20000 ——
         * 也就是 busy-poll 直接讓 rz_proc_notify() 的握手失敗。
         *
         * ⇒ 用 condvar 換效率與穩定,靜默卡死交給外部看門狗(r8web/watchdog.sh):
         *   它監看 JSON 的新鮮度,停滯就把整條管線重新乾淨啟動。
         *   要手動試 busy-poll 仍可用 R8_BENCH_POLL=1。 */
        clock_gettime(CLOCK_MONOTONIC, &b0);
        printf("常駐取樣中:每 %d ms 寫一次 %s\n", g_sns_ms, g_sns_path);
        printf("Ctrl-C 結束(會 release_channel(),讓下一次還能連上)\n\n");
        while (!force_stop) {
            sns_cmd_t sc;
            sns_rsp_t sr;
            sc.magic = SNS_CMD_MAGIC; sc.abi_ver = PWM_ABI_VER; sc.reset = 0u;

            /* 一律 trysend(鐵律②:rpmsg_send() 拿不到 buffer 會無限阻塞) */
            int sret = RPMSG_ERR_NO_BUFF;
            for (int a = 0; a < 200 && !force_stop; a++) {
                sret = rpmsg_trysend(&g_ept, &sc, sizeof(sc));
                if (sret != RPMSG_ERR_NO_BUFF) break;
                struct timespec d = { .tv_sec = 0, .tv_nsec = 1000 * 1000L };
                nanosleep(&d, NULL);
                platform_poll(g_platform);
            }
            if (force_stop) break;
            if (sret < 0) { fprintf(stderr, "trysend 失敗 %d\n", sret); break; }
            if (wait_reply(1000) != 0) {
                /* 單次逾時不放棄 —— 展示跑到一半整個斷掉最難看。
                 * 連續失敗才是真的出事。 */
                if (++consecutive_fail >= 10) {
                    fprintf(stderr, "R8 連續 %d 次未回覆,放棄\n", consecutive_fail);
                    break;
                }
                fprintf(stderr, "逾時 %d/10,重試\n", consecutive_fail);
                continue;
            }
            consecutive_fail = 0;
            if (g_reply_len < sizeof(sns_rsp_t)) {
                fprintf(stderr, "回覆太短(%zu < %zu)—— 韌體沒有感測器功能\n",
                        (size_t)g_reply_len, sizeof(sns_rsp_t));
                break;
            }
            memcpy(&sr, g_reply_buf, sizeof(sr));
            if (SNS_RSP_MAGIC != sr.magic) {
                fprintf(stderr, "回覆 magic 0x%08x —— 韌體沒有感測器功能\n", sr.magic);
                break;
            }
            if (PWM_ABI_VER != sr.abi_ver) {
                fprintf(stderr, "abi_ver %u != %u —— 兩側要用同一份 proto 重編\n",
                        sr.abi_ver, PWM_ABI_VER);
                break;
            }
            clock_gettime(CLOCK_MONOTONIC, &b1);
            sns_write_json(&sr, g_sns_path, ++seq, ms_between(&b0, &b1) / 1000.0);

            /* 進度只印在整秒邊界,免得 journal 爆掉 */
{   /* 每 10 秒一行。原本用 \r 就地更新,寫進檔案會擠成一條超長行。 */
                unsigned long long every =
                    (unsigned long long)(10000 / (g_sns_ms ? g_sns_ms : 1));
                if (every && 0 == (seq % every))
                    printf("  seq=%llu  GPS %u 幀/%u 錯  羅盤 %u 取樣/%u 錯\n",
                           seq, sr.gps_frames, sr.gps_ck_err, sr.mag_samples, sr.mag_err);
            }
            fflush(stdout);

            struct timespec d = { .tv_sec = g_sns_ms / 1000,
                                  .tv_nsec = (long)(g_sns_ms % 1000) * 1000000L };
            nanosleep(&d, NULL);
        }
        printf("\n收工,seq=%llu\n", seq);
        goto shutdown;
    }

    if (g_sns_mode) {
        static const char *FIXNAME[] = {
            "無定位", "航位推算", "2D fix", "3D fix", "GNSS+DR", "僅授時"
        };
        for (int k = 0; k < g_sns_n; k++) {
            sns_cmd_t sc;
            sns_rsp_t sr;
            sc.magic = SNS_CMD_MAGIC; sc.abi_ver = PWM_ABI_VER; sc.reset = 0u;

            /* 一律 trysend(鐵律②:rpmsg_send() 拿不到 buffer 會無限阻塞) */
            int sret = RPMSG_ERR_NO_BUFF;
            for (int a = 0; a < 200; a++) {
                sret = rpmsg_trysend(&g_ept, &sc, sizeof(sc));
                if (sret != RPMSG_ERR_NO_BUFF) break;
                struct timespec d = { .tv_sec = 0, .tv_nsec = 1000 * 1000L };
                nanosleep(&d, NULL);
                platform_poll(g_platform);
            }
            if (sret < 0) { fprintf(stderr, "rpmsg_trysend 失敗: %d\n", sret); goto shutdown; }
            if (wait_reply(1000) != 0) { fprintf(stderr, "R8 沒有回覆(1 s 逾時)\n"); goto shutdown; }

            if (g_reply_len < sizeof(sns_rsp_t)) {
                fprintf(stderr,
                    "回覆太短(%zu < %zu)—— 板上韌體沒有感測器功能。\n"
                    "  載入 cr8_sensors.elf(ABI %u)。\n",
                    (size_t)g_reply_len, sizeof(sns_rsp_t), PWM_ABI_VER);
                goto shutdown;
            }
            memcpy(&sr, g_reply_buf, sizeof(sr));
            if (SNS_RSP_MAGIC != sr.magic) {
                fprintf(stderr,
                    "回覆 magic = 0x%08x(期望 0x%08x)—— R8 把它當一般回聲送回來了,\n"
                    "  代表板上韌體沒有感測器功能。載入 cr8_sensors.elf。\n",
                    sr.magic, SNS_RSP_MAGIC);
                goto shutdown;
            }
            /* 兩個方向都檢查 abi_ver,否則「一邊重編了另一邊沒有」只會抓到一半 */
            if (PWM_ABI_VER != sr.abi_ver) {
                fprintf(stderr,
                    "回覆 abi_ver = %u(本程式是 %u)—— 兩側必須用同一份 r8_pwm_proto.h 重編。\n"
                    "  拒絕採信這次結果。\n", sr.abi_ver, PWM_ABI_VER);
                goto shutdown;
            }
            if (0 != sr.status) {
                fprintf(stderr, "R8 回報 status = %d\n", sr.status);
                goto shutdown;
            }

            if (g_sns_n > 1) printf("\n---------- 第 %d/%d 次 ----------\n", k + 1, g_sns_n);

            /* ---- GPS ---- */
            printf("\nGPS   u-blox M8N,UBX @ 230400,RSCI5 = J1 pin 8/10\n");
            printf("  幀      %u 收 / %u 校驗錯 / %u 位元組丟棄\n",
                   sr.gps_frames, sr.gps_ck_err, sr.gps_drop);
            if (0xFFFFFFFFu == sr.gps_age_ms)
                printf("  年齡    從未收到 NAV-PVT\n");
            else
                printf("  年齡    %u ms\n", sr.gps_age_ms);
            printf("  定位    %s,%u 顆衛星,pDOP %.2f\n",
                   (sr.gps_fix < 6u) ? FIXNAME[sr.gps_fix] : "?",
                   sr.gps_nsv, sr.gps_pdop / 100.0);
            if (sr.gps_fix >= 2u) {
                printf("  位置    %.7f, %.7f    海拔 %.1f m\n",
                       sr.gps_lat * 1e-7, sr.gps_lon * 1e-7, sr.gps_alt_mm / 1000.0);
                printf("  精度    水平 +/-%.1f m\n", sr.gps_hacc_mm / 1000.0);
            } else {
                printf("  位置    尚未定位(室內正常 —— 天線要看得到天空)\n");
            }
            if (0u == sr.gps_frames)
                printf("  ⚠ 一幀都沒收到 —— 檢查接線,或 DT 的 sci5 是否還被 Linux 佔著\n");
            if (0u != sr.gps_drop)
                printf("  ⚠ 有位元組被丟棄 —— R8 的 sensor task 掏得不夠快\n");

            /* ---- 羅盤 ---- */
            printf("\n羅盤  IST8310 @ 0x0E,RSCI7 I2C 100 kHz = J1 pin 3/5\n");
            printf("  WHO_AM_I 0x%02X %s\n", sr.mag_whoami,
                   (0x10u == sr.mag_whoami) ? "(IST8310,正確)"
                                            : "(不是 0x10 —— I2C 沒通或不是這顆晶片)");
            printf("  取樣    %u 成功 / %u 失敗", sr.mag_samples, sr.mag_err);
            if (0xFFFFFFFFu == sr.mag_age_ms) printf("    年齡 從未取樣\n");
            else                              printf("    年齡 %u ms\n", sr.mag_age_ms);
            if (sr.mag_samples > 0u) {
                /* IST8310 靈敏度 0.3 µT/LSB。換算放這一側,R8 只送原始值。 */
                double x = sr.mag_x * 0.3, y = sr.mag_y * 0.3, z = sr.mag_z * 0.3;
                double m = sqrt(x * x + y * y + z * z);
                double h = atan2(y, x) * 180.0 / M_PI; if (h < 0) h += 360.0;
                printf("  原始    X %6d  Y %6d  Z %6d   (LSB)\n", sr.mag_x, sr.mag_y, sr.mag_z);
                printf("  磁場    X %6.1f  Y %6.1f  Z %6.1f   |B| %.1f uT\n", x, y, z, m);
                printf("  航向    %.1f deg(未校正,僅供比對)\n", h);
                /* 台灣地磁總場約 45 µT。偏差大多來自硬鐵干擾,轉一圈才分得出來。 */
                if (m < 25.0 || m > 70.0)
                    printf("  ⚠ |B| 偏離台灣地磁(約 45 uT)—— 附近有磁性物體,或需要硬鐵校正\n");
            }
            printf("\n");

            if (k + 1 < g_sns_n) {
                struct timespec d = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000L };
                nanosleep(&d, NULL);
            }
        }
        goto shutdown;
    }

    if (g_uartd_mode) {
        /* UART 展示 daemon(core1)。stdin:`send <text>` -> text+'\n' 出 UART;
         * 閒時每 ~200ms QUERY 收 RX。JSON:totals + 累積 RX(4KB 滾動)。
         * 停止同 pwmd:stdin EOF 或 SIGINT,完整 release_channel。 */
        static char rx_acc[4096];
        int  rx_len = 0;
        char line[512];
        unsigned long long seq = 0;
        int idle = 0, fails = 0;
        setvbuf(stdin, NULL, _IONBF, 0);   /* poll+fgets 陷阱,同 pwmd */
        printf("uartd 常駐:stdin 收 \"send <text>\",回報寫 %s\n", g_uartd_path);
        while (!force_stop) {
            uart_cmd_t cmd; uart_rsp_t rsp;
            int have_send = 0;
            struct pollfd pfd = { .fd = 0, .events = POLLIN };
            int pr = poll(&pfd, 1, 20);
            if (pr < 0) { if (EINTR == errno) continue; break; }
            if (pr > 0) {
                if (!fgets(line, sizeof(line), stdin)) break;   /* EOF */
                size_t L = strlen(line);
                while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
                if (0 == strncmp(line, "send ", 5)) {
                    const char *txt = line + 5;
                    size_t n = strlen(txt);
                    if (n > UART_DATA_MAX - 1) n = UART_DATA_MAX - 1;
                    memset(&cmd, 0, sizeof(cmd));
                    cmd.magic = UART_CMD_MAGIC; cmd.abi_ver = UART_ABI_VER;
                    cmd.op = UART_OP_SEND;
                    memcpy(cmd.data, txt, n);
                    cmd.data[n] = '\n';
                    cmd.len = (uint32_t)(n + 1);
                    have_send = 1;
                } else if (L) {
                    fprintf(stderr, "uartd: 看不懂:%s\n", line);
                    continue;
                }
            } else if (++idle < 10) {
                continue;                       /* 每 10 輪(~200ms)才 QUERY */
            }
            idle = 0;
            if (!have_send) {
                memset(&cmd, 0, sizeof(cmd));
                cmd.magic = UART_CMD_MAGIC; cmd.abi_ver = UART_ABI_VER;
                cmd.op = UART_OP_QUERY;
            }
            if (uartd_xfer(&cmd, &rsp) != 0) {
                if (++fails >= 10) { fprintf(stderr, "uartd: 連續 %d 次失敗,放棄\n", fails); break; }
                continue;
            }
            fails = 0;
            if (have_send && UART_ERR_TXBUSY == rsp.status) {
                struct timespec d = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
                nanosleep(&d, NULL);
                if (uartd_xfer(&cmd, &rsp) != 0) continue;      /* 一次重試 */
            }
            if (rsp.rx_len) {                    /* 累積 RX(滿了丟最舊)*/
                int n = (int)rsp.rx_len;
                if (n > (int)sizeof(rx_acc)) n = sizeof(rx_acc);
                if (rx_len + n > (int)sizeof(rx_acc)) {
                    int drop = rx_len + n - (int)sizeof(rx_acc);
                    memmove(rx_acc, rx_acc + drop, (size_t)(rx_len - drop));
                    rx_len -= drop;
                }
                memcpy(rx_acc + rx_len, rsp.data, (size_t)n);
                rx_len += n;
            }
            {
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "%s.tmp", g_uartd_path);
                FILE *f = fopen(tmp, "w");
                if (f) {
                    fprintf(f, "{\"seq\":%llu,\"status\":%d,\"tx_total\":%u,"
                               "\"rx_total\":%u,\"rx_dropped\":%u,\"rx_text\":\"",
                            ++seq, rsp.status, rsp.tx_total, rsp.rx_total, rsp.rx_dropped);
                    uartd_json_escape(f, rx_acc, rx_len);
                    fputs("\"}\n", f);
                    fclose(f);
                    rename(tmp, g_uartd_path);
                }
            }
        }
        printf("uartd 收攤,seq 到 %llu\n", seq);
        goto shutdown;
    }

    if (g_pwmd_mode) {
        /* 常駐 PWM 控制 daemon(網頁後端,serve_pwm.py 以子行程持有)。
         * stdin 每行一筆:  <gpt> <a|b> <freq_hz> <duty_permille>
         * 每筆送 PWMC、等回覆、把 R8 回報寫成 JSON(tmp+rename 原子替換)。
         * stdin EOF 或 SIGINT 收攤,走與 snsjson 相同的 shutdown 路徑,
         * 通道由本行程獨佔直到收攤(doc 16:反覆建拆會把 MHU 停在半個握手)。
         * fgets 前先 poll(200ms):glibc signal() 帶 SA_RESTART,
         * 不這樣做 Ctrl-C 之後 read 會自動重啟,force_stop 永遠沒機會被看到。 */
        char line[128];
        unsigned long long seq = 0;
        int consecutive_fail = 0;
        /* poll+fgets 混用的陷阱:stdio 緩衝會一口氣吸走多行,第二行之後
         * 躺在緩衝裡,poll 看 fd 是空的 → 卡到「下一筆」到達才被處理。
         * 關掉 stdin 的 stdio 緩衝,fgets 逐字 read 到 \n(指令行很短,無所謂)。 */
        setvbuf(stdin, NULL, _IONBF, 0);
        printf("pwmd 常駐:stdin 收 \"<gpt> <a|b> <freq_hz> <duty_permille>\",回報寫 %s\n",
               g_pwmd_path);
        while (!force_stop) {
            struct pollfd pfd = { .fd = 0, .events = POLLIN };
            int pr = poll(&pfd, 1, 20);   /* 20ms:旋鈕拖曳的即時感;CPU 影響可忽略 */
            if (pr < 0) { if (EINTR == errno) continue; break; }
            if (0 == pr) continue;
            if (!fgets(line, sizeof(line), stdin)) break;   /* EOF = 收攤 */

            unsigned gpt, freq, permille;
            char ch;
            if (4 != sscanf(line, "%u %c %u %u", &gpt, &ch, &freq, &permille)) {
                fprintf(stderr, "pwmd: 看不懂:%s", line);
                continue;
            }

            pwm_cmd_t cmd;
            pwm_rsp_t rsp;
            cmd.magic         = PWM_CMD_MAGIC;
            cmd.abi_ver       = PWM_ABI_VER;
            cmd.gpt           = gpt;
            cmd.use_b         = ('b' == ch || 'B' == ch) ? 1u : 0u;
            cmd.freq_hz       = freq;
            cmd.duty_permille = permille;

            /* 一律 trysend(doc 16 鐵律②) */
            int sret = RPMSG_ERR_NO_BUFF;
            for (int a = 0; a < 200 && !force_stop; a++) {
                sret = rpmsg_trysend(&g_ept, &cmd, sizeof(cmd));
                if (sret != RPMSG_ERR_NO_BUFF) break;
                struct timespec d = { .tv_sec = 0, .tv_nsec = 1000 * 1000L };
                nanosleep(&d, NULL);
                platform_poll(g_platform);
            }
            if (force_stop) break;

            const char *err  = NULL;
            int         have = 0;
            if (sret < 0)                                err = "trysend";
            else if (wait_reply(1000) != 0)              err = "timeout";
            else if (g_reply_len < sizeof(pwm_rsp_t))    err = "short";
            else {
                memcpy(&rsp, g_reply_buf, sizeof(rsp));
                have = 1;
                if (PWM_RSP_MAGIC != rsp.magic)      { err = "magic"; have = 0; }
                else if (PWM_ABI_VER != rsp.abi_ver) { err = "abi";   have = 0; }
            }
            if (err && !have) {
                if (++consecutive_fail >= 10) {
                    fprintf(stderr, "pwmd: 連續 %d 次失敗,放棄\n", consecutive_fail);
                    pwmd_write_json(g_pwmd_path, ++seq, &cmd, NULL, err);
                    break;
                }
            } else {
                consecutive_fail = 0;
            }
            pwmd_write_json(g_pwmd_path, ++seq, &cmd, have ? &rsp : NULL, err);
        }
        printf("pwmd 收攤,共處理 %llu 筆\n", seq);
        goto shutdown;
    }

    if (g_pwm_mode) {
        pwm_cmd_t cmd;
        cmd.magic         = PWM_CMD_MAGIC;
        cmd.abi_ver       = PWM_ABI_VER;
        cmd.gpt           = g_pwm_gpt;
        cmd.use_b         = g_pwm_useb;
        cmd.freq_hz       = g_pwm_freq;
        cmd.duty_permille = g_pwm_permille;

        printf("送出 -> GPT%u.%c   freq = %u Hz   duty = %.1f %%\n",
               g_pwm_gpt, g_pwm_useb ? 'B' : 'A', g_pwm_freq, g_pwm_permille / 10.0);

        /* 一律 trysend(doc 16 鐵律②:rpmsg_send() 拿不到 buffer 會無限阻塞) */
        int sret = RPMSG_ERR_NO_BUFF;
        for (int a = 0; a < 200; a++) {
            sret = rpmsg_trysend(&g_ept, &cmd, sizeof(cmd));
            if (sret != RPMSG_ERR_NO_BUFF) break;
            struct timespec d = { .tv_sec = 0, .tv_nsec = 1000 * 1000L };
            nanosleep(&d, NULL);
            platform_poll(g_platform);
        }
        if (sret < 0) {
            fprintf(stderr, "rpmsg_trysend 失敗: %d\n", sret);
            goto shutdown;
        }
        if (wait_reply(1000) != 0) {
            fprintf(stderr, "R8 沒有回覆(1 s 逾時)\n");
            goto shutdown;
        }
        if (g_reply_len < sizeof(pwm_rsp_t)) {
            fprintf(stderr, "回覆長度不對:%zu < %zu —— R8 韌體可能沒有 PWM 控制功能\n",
                    (size_t)g_reply_len, sizeof(pwm_rsp_t));
            goto shutdown;
        }
        {
            pwm_rsp_t rsp;
            memcpy(&rsp, g_reply_buf, sizeof(rsp));
            if (PWM_RSP_MAGIC != rsp.magic) {
                fprintf(stderr,
                        "回覆 magic = 0x%08x(期望 0x%08x)—— R8 把它當一般回聲了,\n"
                        "  代表板上韌體沒有 PWM 控制功能。載入 cr8_rpmsg_pwm_ctl.elf。\n",
                        rsp.magic, PWM_RSP_MAGIC);
                goto shutdown;
            }
            /* R8 檢查命令的 abi_ver,這裡檢查回覆的 —— 兩個方向都要,
             * 否則「一邊改了協定另一邊沒重編」只有一半會被抓到。
             * 放在 status 判讀之前:版本不合時 status 的意義本身就不可信。 */
            if (PWM_ABI_VER != rsp.abi_ver) {
                fprintf(stderr,
                        "回覆 abi_ver = %u(本程式是 %u)—— 韌體與這支程式的協定版本不同。\n"
                        "  兩側必須用同一份 r8_pwm_proto.h 重編。拒絕採信這次結果。\n",
                        rsp.abi_ver, PWM_ABI_VER);
                goto shutdown;
            }
            if (0 != rsp.status) {
                fprintf(stderr, "R8 拒絕:status = %d(%s)\n",
                        rsp.status, pwm_err_str(rsp.status));
                goto shutdown;
            }
            {
                double act_f = (double)rsp.pclk_hz / (double)rsp.period_counts;
                double act_d = 100.0 * (double)rsp.duty_counts / (double)rsp.period_counts;
                printf("R8 已套用:\n");
                /* R_GPT_PeriodSet 收的是「週期 = N counts」,而 GTPR 暫存器實際存 N-1
                 * (計數器數 0..N-1)。兩個都印,免得跟 devmem 讀到的值對不上。 */
                printf("  週期   = %u counts (GTPR 存 N-1 = %u)  -> 實際頻率   %.4f Hz   (誤差 %+.4f %%)\n",
                       rsp.period_counts,
                       rsp.period_counts ? rsp.period_counts - 1u : 0u, act_f,
                       g_pwm_freq ? 100.0 * (act_f - (double)g_pwm_freq) / (double)g_pwm_freq : 0.0);
                printf("  GTCCR%c = %u counts  -> 實際佔空比 %.4f %%  (脈寬 %.3f ms)\n",
                       g_pwm_useb ? 'B' : 'A', rsp.duty_counts, act_d,
                       1000.0 * (double)rsp.duty_counts / (double)rsp.pclk_hz);
                printf("  PCLK   = %u Hz(由 R8 回報,不是 A55 寫死的)\n", rsp.pclk_hz);
            }
        }
        goto shutdown;
    }

    {
        int buf_sz = rpmsg_virtio_get_buffer_size(rdev);
        printf("rpmsg_virtio_get_buffer_size() = %d bytes (raw per-message vring buffer, includes 16B rpmsg header)\n", buf_sz);

        /* shm pool 耗盡預測 —— 見 bench 迴圈的註解:pool 是 bump allocator,
         * 從不歸還。RX 在 init 時就先吃掉 vq_nentries 個,剩下的才是 TX 可挖的。 */
        {
            size_t rpmsg_buf = (size_t)buf_sz + 16;                 /* RPMSG_BUFFER_SIZE */
            size_t shm_bytes = CFG_VRING_SHM_SIZE1;                 /* 本通道的 shm 區 */
            size_t total_bufs = shm_bytes / rpmsg_buf;
            size_t rx_prealloc = CFG_RPMSG_NUM_BUFS1;               /* master 在 init 預配 */
            printf("shm pool: %zu KB / %zu B = %zu buffers,扣掉 RX 預配 %zu"
                   "  ->  TX 預算約 %zu 次\n",
                   shm_bytes / 1024, rpmsg_buf, total_bufs, rx_prealloc,
                   total_bufs > rx_prealloc ? total_bufs - rx_prealloc : 0);
        }

        const size_t hdr = 2 * sizeof(unsigned long); /* 16 bytes, matches struct _payload */
        /* IMPORTANT: rpmsg_send() in this library does NOT bounds-check
         * against the real vring buffer capacity -- probing past it
         * previously took down the whole probe process with SIGBUS
         * instead of returning an error code (recovered fine via the
         * proven-safe stop->start cycle, no board damage, but don't
         * repeat it). Cap the probe at the size the library itself
         * reports as the buffer size minus our 16-byte payload header
         * -- do not add any margin past that. */
        size_t upper_probe = (buf_sz > (int)hdr) ? (size_t)buf_sz - hdr : 480;
        unsigned char *sbuf = malloc(upper_probe + hdr + 16);
        if (!sbuf) { fprintf(stderr, "OOM\n"); goto cleanup; }
        struct _payload *pl = (struct _payload *)sbuf;

        printf("\n=== [1/2] Max message length probe (data[] size 1..%zu bytes) ===\n", upper_probe);
        size_t last_ok = 0;
        int stopped_reason = 0; /* 0=none yet, 1=send rejected, 2=echo timeout, 3=mismatch */
        for (size_t sz = 1; sz <= upper_probe && !force_stop; sz++) {
            pl->num = 900000 + sz;
            pl->size = sz;
            memset(pl->data, 0xA5, sz);
            size_t total = hdr + sz;

            if (getenv("R8_BENCH_VERBOSE_PROBE")) {
                printf("  trying data[]=%4zu B (total %4zu B)...\n", sz, total);
                fflush(stdout);
            }
            int sret = rpmsg_send(&g_ept, pl, total);
            if (sret < 0) {
                printf("  data[]=%4zu B (total %4zu B): rpmsg_send() rejected, ret=%d -> hard transport cap\n",
                       sz, total, sret);
                stopped_reason = 1;
                break;
            }
            if (wait_reply(200) != 0) {
                printf("  data[]=%4zu B (total %4zu B): sent OK, no echo within 200ms -> effective cap\n",
                       sz, total);
                stopped_reason = 2;
                break;
            }
            /* Bytes 16..31 of the echo are the CR8 timestamp window (see the
             * ipcts block above): the firmware deliberately overwrites them, so
             * they must be excluded from the 0xA5 integrity check.  Without this
             * the probe reports "max working data[] size = 15 bytes" -- the
             * instrumentation looking exactly like a transport failure. */
            int ts_mism = (g_reply_len != total);
            if (!ts_mism) {
                size_t lo = total < 16 ? total : 16;
                ts_mism = memcmp(g_reply_buf, pl, lo) != 0;
                if (!ts_mism && total > 32)
                    ts_mism = memcmp((const char *)g_reply_buf + 32,
                                     (const char *)pl + 32, total - 32) != 0;
            }
            if (ts_mism) {
                printf("  data[]=%4zu B: echo MISMATCH (got %zu bytes back)\n", sz, g_reply_len);
                stopped_reason = 3;
                break;
            }
            last_ok = sz;
        }
        if (stopped_reason) {
            printf("\nResult: max working data[] size = %zu bytes (rpmsg payload %zu bytes incl. 16B demo header)\n",
                   last_ok, last_ok + hdr);
        } else if (!force_stop) {
            printf("\nResult: reached probe ceiling %zu bytes without hitting a cap (raise upper_probe to search further)\n",
                   upper_probe);
            last_ok = upper_probe;
        }

        if (force_stop)
            goto shutdown;

        /* === [2/2] Round-trip latency / throughput at two sizes: the
         * discovered max, and a small fixed size for a baseline. === */
        size_t sizes_to_bench[2];
        int n_sizes = 0;
        if (last_ok > 0) sizes_to_bench[n_sizes++] = last_ok;
        if (last_ok != 16) sizes_to_bench[n_sizes++] = 16 <= last_ok ? 16 : (last_ok ? last_ok : 16);

        for (int s = 0; s < n_sizes; s++) {
            size_t bench_sz = sizes_to_bench[s];
            size_t bench_total = hdr + bench_sz;
            pl->size = bench_sz;
            memset(pl->data, 0xA5, bench_sz);

            printf("\n=== [2/2] Round-trip speed test @ data[]=%zu B (total %zu B), %d round trips ===\n",
                   bench_sz, bench_total, n_rounds);

            double *lat = malloc(sizeof(double) * n_rounds);
            d_a2r    = malloc(sizeof(double) * n_rounds);
            d_isr2tk = malloc(sizeof(double) * n_rounds);
            d_tk2cb  = malloc(sizeof(double) * n_rounds);
            d_cb2tx  = malloc(sizeof(double) * n_rounds);
            d_r2a    = malloc(sizeof(double) * n_rounds);
            d_cb2app = malloc(sizeof(double) * n_rounds);
            d_rt     = malloc(sizeof(double) * n_rounds);
            d_n = 0;
            int errors = 0, ok = 0;
            struct timespec run0, run1;
            clock_gettime(CLOCK_MONOTONIC, &run0);
            /* 進度/失速偵測。
             *
             * 為什麼需要:量到的 per-message latency 只涵蓋「send → 收到回覆」,
             * **不包含「取得下一個 TX buffer」**。OpenAMP 的 shm pool 是只進不出的
             * bump allocator(rpmsg_virtio_shm_pool_get_buffer 只有 avail -= size,
             * 全域沒有任何歸還),回收跟不上時會一路挖新的,挖光後每次送出都要
             * 等 used ring 回流 —— 吞吐崩潰但延遲數字依舊漂亮。短測完全看不出來。
             *
             * 所以這裡量的是**迭代間隔**(含取 buffer 的等待),並在失速時
             * 主動乾淨結束:被 timeout 硬砍會讓 MHU 卡在中斷 set 狀態,
             * 那是 stop→start 救不回、必須重開機的(doc 12 §5 要補這條)。 */
            const int CHUNK = 1000;
            struct timespec c0; clock_gettime(CLOCK_MONOTONIC, &c0);
            int stalled_at = -1, starved_at = -1;
            long nobuf = 0;
            for (int i = 0; i < n_rounds && !force_stop; i++) {
                pl->num = (unsigned long)i;
                unsigned int ts_t0 = 0;
                struct timespec m0, m1;
                clock_gettime(CLOCK_MONOTONIC, &m0);
                /* ⚠️ 不要用 rpmsg_send():拿不到 TX buffer 時它會**無限阻塞**,
                 * 呼叫端完全失去控制權——連逾時偵測都跑不到(實測就是這樣被
                 * 外部 timeout 硬砍,進而卡死 MHU)。上游 custom_agent.cpp 的
                 * rpmsg_send_retry() 為同一原因改用 trysend,其註解記載
                 * rpmsg_send() 可持鎖阻塞達 3 秒並造成優先權反轉。 */
                int sret = RPMSG_ERR_NO_BUFF;
                for (int a = 0; a < 200; a++) {          /* 最多退讓 ~200 ms */
                    ts_t0 = ipcts_now();   /* T0 */
                    sret = rpmsg_trysend(&g_ept, pl, bench_total);
                    if (sret != RPMSG_ERR_NO_BUFF) break;
                    nobuf++;
                    struct timespec d = { .tv_sec = 0, .tv_nsec = 1000 * 1000L };
                    nanosleep(&d, NULL);
                    platform_poll(g_platform);           /* 推動 used ring 回收 */
                }
                if (sret < 0) {
                    errors++;
                    if (sret == RPMSG_ERR_NO_BUFF && starved_at < 0) {
                        starved_at = i + 1;
                        printf("    ⚠️ 第 %d 次:TX buffer 耗盡(重試 200ms 仍拿不到)\n",
                               starved_at);
                    }
                    continue;
                }
                if (wait_reply(200) != 0) { errors++; continue; }
                unsigned int ts_t6 = ipcts_now();   /* T6 */
                clock_gettime(CLOCK_MONOTONIC, &m1);
                if (g_gpt && bench_total >= 32 && g_reply_len >= 32) {
                    const unsigned int *rt = (const unsigned int *)(g_reply_buf + 16);
                    d_a2r[d_n]    = ipcts_us(ts_t0, rt[0]);
                    d_isr2tk[d_n] = ipcts_us(rt[0], rt[1]);
                    d_tk2cb[d_n]  = ipcts_us(rt[1], rt[2]);
                    d_cb2tx[d_n]  = ipcts_us(rt[2], rt[3]);
                    d_r2a[d_n]    = ipcts_us(rt[3], g_ts_a55_cb);
                    d_cb2app[d_n] = ipcts_us(g_ts_a55_cb, ts_t6);
                    d_rt[d_n]     = ipcts_us(ts_t0, ts_t6);
                    d_n++;
                }
                lat[ok++] = ms_between_d(&m0, &m1);

                /* 硬停偵測:必須逐次檢查,不能只在 CHUNK 邊界。
                 * 實測的失效型態是「跑到某個次數突然完全停住」而非漸慢,
                 * 只看邊界的話會卡在兩個邊界之間、等到被 timeout 硬砍 ——
                 * 而硬砍會讓 MHU 卡在中斷 set 狀態,得重開機才救得回來。 */
                if (ms_between_d(&c0, &m1) > 5000.0) {
                    stalled_at = i + 1;
                    printf("    第 %d 次之後停滯 >5s,乾淨結束\n", stalled_at);
                    break;
                }

                if ((i + 1) % CHUNK == 0) {
                    struct timespec c1; clock_gettime(CLOCK_MONOTONIC, &c1);
                    double chunk_s = ms_between_d(&c0, &c1) / 1000.0;
                    double rate = CHUNK / chunk_s;
                    printf("    [%6d] %7.1f 次/秒  (本區間 %.3f s)%s\n",
                           i + 1, rate, chunk_s, rate < 500 ? "  ← 失速" : "");
                    c0 = c1;
                    /* 掉到 1/10 以下就收工,不必等 timeout 來砍 */
                    if (rate < 500.0) {
                        stalled_at = i + 1;
                        printf("    偵測到失速,於第 %d 次乾淨結束(避免硬砍卡死 MHU)\n",
                               stalled_at);
                        break;
                    }
                }
            }
            if (stalled_at > 0)
                printf("  ⚠️ 吞吐在第 %d 次左右崩潰 —— 對照 shm pool 容量預測值\n", stalled_at);
            clock_gettime(CLOCK_MONOTONIC, &run1);
            double total_s = (run1.tv_sec - run0.tv_sec) + (run1.tv_nsec - run0.tv_nsec) / 1e9;

            double sum = 0, lmin = 1e18, lmax = 0;
            for (int i = 0; i < ok; i++) {
                sum += lat[i];
                if (lat[i] < lmin) lmin = lat[i];
                if (lat[i] > lmax) lmax = lat[i];
            }
            printf("  %d/%d ok, %d errors, wall time %.3f s  (TX no-buf 重試 %ld 次%s)\n",
                   ok, n_rounds, errors, total_s, nobuf,
                   starved_at > 0 ? ",曾耗盡" : "");
            if (ok > 0) {
                printf("  Round-trip latency: min=%.3f ms  avg=%.3f ms  max=%.3f ms\n",
                       lmin, sum / ok, lmax);
                { char lb[40]; snprintf(lb, sizeof lb, "@ %zu B total", bench_total); d_report(lb); }
                qsort(lat, ok, sizeof(double), cmp_double);
                double p999 = pct(lat, ok, 99.9);
                printf("  Percentiles (ms): p50=%.3f  p90=%.3f  p99=%.3f  p99.9=%.3f  p99.99=%.3f\n",
                       pct(lat, ok, 50.0), pct(lat, ok, 90.0),
                       pct(lat, ok, 99.0), p999, pct(lat, ok, 99.99));

                /* doc 13 §6 T1 判準:滿載下 p99.9 > 500 µs,或出現 >2 ms 離群。
                 * 平均值對飛控沒有意義,尾端才有 —— 所以把離群單獨數出來。 */
                int n05 = 0, n2 = 0;
                for (int i = 0; i < ok; i++) {
                    if (lat[i] > 0.5) n05++;
                    if (lat[i] > 2.0) n2++;
                }
                printf("  Outliers: >0.5ms = %d (%.4f%%)   >2ms = %d (%.4f%%)\n",
                       n05, 100.0 * n05 / ok, n2, 100.0 * n2 / ok);
                printf("  T1 verdict: p99.9 %s 0.5ms%s  ->  %s\n",
                       p999 > 0.5 ? ">" : "<=", n2 ? ", has >2ms outliers" : ", no >2ms outlier",
                       (p999 > 0.5 || n2) ? "TRIGGERED (see doc 13 §6)" : "not triggered");
                printf("  Throughput: %.1f round-trips/sec, %.1f KB/s payload, %.1f KB/s incl. rpmsg header\n",
                       ok / total_s,
                       (ok * bench_sz) / total_s / 1024.0,
                       (ok * bench_total) / total_s / 1024.0);
            }
            free(lat);
        }
        free(sbuf);
    }

shutdown:
    {
        /* ⚠️ 一定要 trysend。原本這裡是 rpmsg_send() —— 阻塞版。
         * TX buffer 耗盡時(OpenAMP 的 shm pool 是只進不出的 bump allocator,
         * doc 13 §6.2)它會無限阻塞,行程被外部 timeout 硬砍,然後 MHU ch3
         * 卡在中斷 set(doc 16 §2.2.2)—— 一個會自我強化的失效鏈。
         * 這正是 doc 16 鐵律② 說的事,而收尾路徑自己違反了它。實測踩過兩次。 */
        unsigned long msg = SHUTDOWN_MSG;
        for (int a = 0; a < 50; a++) {
            if (rpmsg_trysend(&g_ept, &msg, sizeof(msg)) != RPMSG_ERR_NO_BUFF) break;
            struct timespec d = { .tv_sec = 0, .tv_nsec = 1000 * 1000L };
            nanosleep(&d, NULL);
            platform_poll(g_platform);
        }
        usleep(50000);
    }

cleanup:
    /* Teardown, minus the one line that crashes. platform_release_rpmsg_vdev()
     * in the vendored platform_info.c does:
     *     rpmsg_deinit_vdev(rpmsg_vdev);
     *     remoteproc_remove_virtio(rproc, rpmsg_vdev->vdev);
     * but rpmsg_deinit_vdev() has already cleared ->vdev by then, so the
     * second line passes NULL and remoteproc_remove_virtio() asserts (100%
     * reproducible). Latch the vdev pointer first, then run the same steps.
     * Skipping teardown entirely is not an option: without it the firmware
     * never re-advertises and only the first run of the process works. */
    release_channel();

    /* ⚠️ CR8 每 200 ms 才輪詢一次狀態位元組。清掉 DRIVER_OK 之後若立刻結束,
     * 下一個行程會在 CR8 的輪詢窗內把 DRIVER_OK 重新設起來 —— CR8 從頭到尾
     * 沒察覺斷線,於是抱著上一輪的過期 endpoint 不放,下次連線就 NS bind 逾時。
     * 上游 custom_agent.cpp 的 stop_handler 為此刻意自旋等 600 ms,註解寫得很明白。
     * 這裡沿用同一個數字。 */
    {
        struct timespec s, n;
        clock_gettime(CLOCK_MONOTONIC, &s);
        do {
            struct timespec d = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000L };
            nanosleep(&d, NULL);
            clock_gettime(CLOCK_MONOTONIC, &n);
        } while (ms_between(&s, &n) < 600);
    }

    if (rdev) {
        struct rpmsg_virtio_device *rvdev =
            metal_container_of(rdev, struct rpmsg_virtio_device, rdev);
        struct virtio_device *vdev = rvdev->vdev; /* latch before deinit nulls it */
        rpmsg_deinit_vdev(rvdev);
        if (vdev)
            remoteproc_remove_virtio(g_platform, vdev);
    }
    fflush(stdout);
    /* _exit() rather than return: remaining OpenAMP/UIO destructors are not
     * needed here and have their own teardown-order hazards. */
    _exit(0);
}
