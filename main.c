/* PSP ME Benchmark — SC vs ME × Clock comparison */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspusb.h>
#include <pspusbstor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pspsdk.h>
#include "me-core.h"

/* PSP newlib stub */
int ftruncate(int fd, off_t length) { (void)fd; (void)length; return -1; }

PSP_MODULE_INFO("ME_BENCH", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define SCREEN_W 480
#define SCREEN_H 272
#define BUF_WIDTH 512
#define PI_F 3.14159265358979323846f

static volatile int running = 1;

/* ============================================================
 *  ME Protocol
 * ============================================================ */
#define ME_CMD_NONE     0
#define ME_CMD_PI       1
#define ME_CMD_PRIME    2
#define ME_CMD_MANDEL   3
#define ME_CMD_MEMBW    4
#define ME_CMD_STOP     0xFF
#define ME_STAT_IDLE    0
#define ME_STAT_DONE    1

static volatile unsigned int _me_shared[64] __attribute__((aligned(64), section(".uncached"))) = {0};
/* [0]=cmd [1]=status [2]=param_start [3]=param_end [4]=result_us [5]=result_check
 * [6]=result_float_bits [7]=sub_cmd(membw target) [8]=mandel_buf_addr */
static volatile unsigned int *me_shared_uncached;
static volatile int me_active = 0;
static int me_init_ret = -99;
static int usb_active = 0;

/* ============================================================
 *  Benchmark Parameters
 * ============================================================ */
#define PI_TERMS      1000000
#define PRIME_LIMIT   100000
#define MANDEL_W      160
#define MANDEL_H      120
#define MANDEL_ITER   64
#define MEMBW_SIZE    (16*1024)   /* 16KB (scratchpad size match) */
#define MEMBW_LOOPS   1000

#define NUM_BENCH     3  /* PI, PRIME, MANDEL */
#define NUM_PATTERNS  4

static const char *bench_names[4] = {"PI","PRIME","MANDEL","MEMBW"};
static const char *pattern_names[NUM_PATTERNS] = {
    "SC 222","DUAL 222","SC 333","DUAL 333"
};

typedef struct {
    unsigned int time_us;
    unsigned int check;
    float metric;  /* MFLOPS / primes_per_sec / Mpixels_per_sec / MB_per_sec */
    int valid;
    int cpu_mhz;
    int bus_mhz;
    int set_ret;
} BenchResult;

static BenchResult results[NUM_PATTERNS][NUM_BENCH];
static int current_bench = -1;
static int current_pattern = -1;
static int bench_running = 0;

/* Mandelbrot shared buffer */
static unsigned char mandel_buf[MANDEL_W * MANDEL_H] __attribute__((aligned(64)));

/* Memory bandwidth test buffer */
static unsigned int membw_buf[MEMBW_SIZE / 4] __attribute__((aligned(64)));

/* ============================================================
 *  SC-side Benchmark Implementations
 * ============================================================ */

/* Pi — Leibniz series: pi/4 = sum (-1)^k / (2k+1) */
static float bench_pi_compute(int start, int end) {
    float sum = 0.0f;
    for (int k = start; k < end; k++) {
        float term = 1.0f / (2.0f * k + 1.0f);
        if (k & 1) sum -= term;
        else       sum += term;
    }
    return sum * 4.0f;
}

static unsigned int bench_pi_sc(int start, int end, float *out_pi) {
    unsigned int t0 = sceKernelGetSystemTimeLow();
    *out_pi = bench_pi_compute(start, end);
    unsigned int t1 = sceKernelGetSystemTimeLow();
    return t1 - t0;
}

/* Prime — trial division count */
static int is_prime(int n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;
    for (int i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i+2) == 0) return 0;
    }
    return 1;
}

static unsigned int bench_prime_sc(int start, int end, int *out_count) {
    unsigned int t0 = sceKernelGetSystemTimeLow();
    int count = 0;
    for (int n = start; n <= end; n++) {
        if (is_prime(n)) count++;
    }
    *out_count = count;
    unsigned int t1 = sceKernelGetSystemTimeLow();
    return t1 - t0;
}

/* Mandelbrot */
static unsigned int bench_mandel_sc(int row_start, int row_end, unsigned char *buf) {
    unsigned int t0 = sceKernelGetSystemTimeLow();
    for (int py = row_start; py < row_end; py++) {
        for (int px = 0; px < MANDEL_W; px++) {
            float x0 = (px - MANDEL_W * 0.5f) * 4.0f / MANDEL_W;
            float y0 = (py - MANDEL_H * 0.5f) * 4.0f / MANDEL_H;
            float x = 0, y = 0;
            int iter = 0;
            while (x*x + y*y <= 4.0f && iter < MANDEL_ITER) {
                float xt = x*x - y*y + x0;
                y = 2.0f*x*y + y0;
                x = xt;
                iter++;
            }
            buf[py * MANDEL_W + px] = (unsigned char)iter;
        }
    }
    unsigned int t1 = sceKernelGetSystemTimeLow();
    return t1 - t0;
}

/* Memory bandwidth — sequential read */
static unsigned int bench_membw_sc(volatile unsigned int *target, int size_words, float *out_mbps) {
    unsigned int t0 = sceKernelGetSystemTimeLow();
    volatile unsigned int sink = 0;
    for (int loop = 0; loop < MEMBW_LOOPS; loop++) {
        for (int i = 0; i < size_words; i += 8) {
            sink += target[i]; sink += target[i+1];
            sink += target[i+2]; sink += target[i+3];
            sink += target[i+4]; sink += target[i+5];
            sink += target[i+6]; sink += target[i+7];
        }
    }
    (void)sink;
    unsigned int t1 = sceKernelGetSystemTimeLow();
    unsigned int elapsed = t1 - t0;
    float total_bytes = (float)size_words * 4.0f * MEMBW_LOOPS;
    *out_mbps = total_bytes / (float)elapsed; /* bytes/us = MB/s */
    return elapsed;
}

/* ============================================================
 *  ME-side Benchmarks (runs on Media Engine)
 * ============================================================ */
void meLibOnProcess(void) {
    volatile unsigned int *sh = (volatile unsigned int *)(0x40000000 | (unsigned int)_me_shared);

    meLibDcacheWritebackInvalidateAll();
    meLibIcacheInvalidateAll();

    for (;;) {
        while (sh[0] == ME_CMD_NONE) {
            asm volatile("nop; nop; nop; nop;");
        }
        if (sh[0] == ME_CMD_STOP) {
            sh[1] = ME_STAT_DONE;
            meLibHalt();
            return;
        }

        unsigned int cmd = sh[0];
        unsigned int p_start = sh[2];
        unsigned int p_end = sh[3];

        /* timing via cop0 Count register (ticks at CPU_CLOCK/2) */
        unsigned int cyc0, cyc1;
        asm volatile("mfc0 %0, $9" : "=r"(cyc0));

        if (cmd == ME_CMD_PI) {
            float sum = 0.0f;
            for (unsigned int k = p_start; k < p_end; k++) {
                float term = 1.0f / (2.0f * k + 1.0f);
                if (k & 1) sum -= term;
                else       sum += term;
            }
            sum *= 4.0f;
            asm volatile("mfc0 %0, $9" : "=r"(cyc1));
            sh[4] = cyc1 - cyc0;
            union { float f; unsigned int u; } conv;
            conv.f = sum;
            sh[6] = conv.u;
        }
        else if (cmd == ME_CMD_PRIME) {
            int count = 0;
            for (int n = (int)p_start; n <= (int)p_end; n++) {
                if (n < 2) continue;
                if (n < 4) { count++; continue; }
                if (n % 2 == 0 || n % 3 == 0) continue;
                int pr = 1;
                for (int i = 5; i * i <= n; i += 6) {
                    if (n % i == 0 || n % (i+2) == 0) { pr = 0; break; }
                }
                if (pr) count++;
            }
            asm volatile("mfc0 %0, $9" : "=r"(cyc1));
            sh[4] = cyc1 - cyc0;
            sh[5] = (unsigned int)count;
        }
        else if (cmd == ME_CMD_MANDEL) {
            unsigned char *buf = (unsigned char *)(0x40000000 | sh[8]); /* uncached main RAM */
            for (int py = (int)p_start; py < (int)p_end; py++) {
                for (int px = 0; px < MANDEL_W; px++) {
                    float x0 = (px - MANDEL_W * 0.5f) * 4.0f / MANDEL_W;
                    float y0 = (py - MANDEL_H * 0.5f) * 4.0f / MANDEL_H;
                    float x = 0, y = 0;
                    int iter = 0;
                    while (x*x + y*y <= 4.0f && iter < MANDEL_ITER) {
                        float xt = x*x - y*y + x0;
                        y = 2.0f*x*y + y0;
                        x = xt;
                        iter++;
                    }
                    buf[py * MANDEL_W + px] = (unsigned char)iter;
                }
            }
            asm volatile("mfc0 %0, $9" : "=r"(cyc1));
            sh[4] = cyc1 - cyc0;
        }
        else if (cmd == ME_CMD_MEMBW) {
            /* ME: ダミー — コマンド応答テスト */
            volatile unsigned int dummy = 0;
            for (int i = 0; i < 1000; i++) dummy += i;
            (void)dummy;
            asm volatile("mfc0 %0, $9" : "=r"(cyc1));
            sh[4] = cyc1 - cyc0;
        }

        sh[1] = ME_STAT_DONE;
        sh[0] = ME_CMD_NONE;
    }
}

/* ME helpers */
static void me_send_cmd(unsigned int cmd, unsigned int p0, unsigned int p1) {
    me_shared_uncached[2] = p0;
    me_shared_uncached[3] = p1;
    me_shared_uncached[1] = ME_STAT_IDLE;
    asm volatile("sync");
    me_shared_uncached[0] = cmd;
}

static void me_wait(void) {
    while (me_shared_uncached[1] != ME_STAT_DONE) {
        asm volatile("nop; nop; nop; nop;");
    }
}

/* Convert ME cop0 Count cycles to microseconds.
 * Count ticks at CPU_CLOCK/2 = 166.5MHz @ 333MHz.
 * us = cycles / 166.5 = cycles * 2 / 333 */
static unsigned int me_cycles_to_us(unsigned int cycles) {
    return (unsigned int)((unsigned long long)cycles * 2 / 333);
}

/* ============================================================
 *  Run Single Benchmark
 * ============================================================ */
static void run_benchmark(int bench_idx, int pattern_idx) {
    int dual = (pattern_idx % 2 == 1);
    BenchResult *r = &results[pattern_idx][bench_idx];
    r->valid = 0;

    /* Set clock */
    int is_hi = (pattern_idx >= 2);
    int ret;
    if (is_hi)
        ret = scePowerSetClockFrequency(333, 333, 166);
    else
        ret = scePowerSetClockFrequency(222, 222, 111);
    sceKernelDelayThread(100000); /* 100ms stabilize */
    r->set_ret = ret;
    r->cpu_mhz = scePowerGetCpuClockFrequency();
    r->bus_mhz = scePowerGetBusClockFrequency();

    if (bench_idx == 0) {
        /* PI */
        if (!dual) {
            float pi_val;
            r->time_us = bench_pi_sc(0, PI_TERMS, &pi_val);
            r->check = (unsigned int)(pi_val * 1000000.0f);
            r->metric = (float)PI_TERMS * 2.0f / (float)r->time_us; /* MFLOPS */
        } else {
            int half = PI_TERMS / 2;
            me_send_cmd(ME_CMD_PI, half, PI_TERMS);
            float pi_sc;
            unsigned int sc_us = bench_pi_sc(0, half, &pi_sc);
            me_wait();
            unsigned int me_us = me_cycles_to_us(me_shared_uncached[4]);
            union { unsigned int u; float f; } conv;
            conv.u = me_shared_uncached[6];
            float pi_total = pi_sc + conv.f;
            r->time_us = (sc_us > me_us) ? sc_us : me_us;
            r->check = (unsigned int)(pi_total * 1000000.0f);
            r->metric = (float)PI_TERMS * 2.0f / (float)r->time_us;
        }
    }
    else if (bench_idx == 1) {
        /* PRIME */
        if (!dual) {
            int count;
            r->time_us = bench_prime_sc(2, PRIME_LIMIT, &count);
            r->check = (unsigned int)count;
            r->metric = (float)PRIME_LIMIT / ((float)r->time_us / 1000000.0f) / 1000000.0f; /* M nums/sec */
        } else {
            int half = PRIME_LIMIT / 2;
            me_send_cmd(ME_CMD_PRIME, half + 1, PRIME_LIMIT);
            int sc_count;
            unsigned int sc_us = bench_prime_sc(2, half, &sc_count);
            me_wait();
            unsigned int me_us = me_cycles_to_us(me_shared_uncached[4]);
            int me_count = (int)me_shared_uncached[5];
            r->time_us = (sc_us > me_us) ? sc_us : me_us;
            r->check = (unsigned int)(sc_count + me_count);
            r->metric = (float)PRIME_LIMIT / ((float)r->time_us / 1000000.0f) / 1000000.0f;
        }
    }
    else if (bench_idx == 2) {
        /* MANDELBROT */
        memset(mandel_buf, 0, sizeof(mandel_buf));
        sceKernelDcacheWritebackInvalidateAll();
        if (!dual) {
            r->time_us = bench_mandel_sc(0, MANDEL_H, mandel_buf);
            unsigned int total_iter = 0;
            for (int i = 0; i < MANDEL_W * MANDEL_H; i++) total_iter += mandel_buf[i];
            r->check = total_iter;
            r->metric = (float)(MANDEL_W * MANDEL_H) / (float)r->time_us; /* Mpixels/sec */
        } else {
            int half_h = MANDEL_H / 2;
            me_shared_uncached[8] = (unsigned int)mandel_buf;
            me_send_cmd(ME_CMD_MANDEL, half_h, MANDEL_H);
            unsigned int sc_us = bench_mandel_sc(0, half_h, mandel_buf);
            /* SC側の書き込みをRAMに反映 */
            sceKernelDcacheWritebackInvalidateAll();
            me_wait();
            /* ME側はuncachedで書いてるので即反映済み — SC dcache無効化で読める */
            sceKernelDcacheInvalidateRange(mandel_buf, sizeof(mandel_buf));
            unsigned int me_us = me_cycles_to_us(me_shared_uncached[4]);
            unsigned int total_iter = 0;
            for (int i = 0; i < MANDEL_W * MANDEL_H; i++) total_iter += mandel_buf[i];
            r->time_us = (sc_us > me_us) ? sc_us : me_us;
            r->check = total_iter;
            r->metric = (float)(MANDEL_W * MANDEL_H) / (float)r->time_us;
        }
    }
    else if (bench_idx == 3) {
        /* MEMORY BANDWIDTH */
        /* SC: scratchpad read */
        volatile unsigned int *sp = (volatile unsigned int *)0x00010000;
        /* init scratchpad with dummy data */
        for (int i = 0; i < 4096; i++) ((volatile unsigned int *)0x00010000)[i] = i;
        /* init main RAM test buffer */
        for (int i = 0; i < (int)(MEMBW_SIZE/4); i++) membw_buf[i] = i;
        sceKernelDcacheWritebackInvalidateAll();

        if (!dual) {
            /* SC: test scratchpad (16KB) */
            float sp_mbps;
            unsigned int sp_us = bench_membw_sc(sp, 4096, &sp_mbps); /* 16KB */
            /* SC: test main RAM */
            float ram_mbps;
            unsigned int ram_us = bench_membw_sc((volatile unsigned int *)membw_buf,
                                                  MEMBW_SIZE/4, &ram_mbps);
            r->time_us = sp_us + ram_us;
            r->metric = sp_mbps; /* report scratchpad speed */
            union { float f; unsigned int u; } conv;
            conv.f = ram_mbps;
            r->check = conv.u; /* store RAM speed in check field */
        } else {
            /* Dual: SC reads scratchpad, ME reads main RAM */
            int me_words = MEMBW_SIZE / 4;
            int me_loops = MEMBW_LOOPS;
            sceKernelDcacheWritebackInvalidateAll();
            me_shared_uncached[8] = (unsigned int)membw_buf;
            me_send_cmd(ME_CMD_MEMBW, me_words, me_loops);
            float sp_mbps;
            unsigned int sp_us = bench_membw_sc(sp, 4096, &sp_mbps);
            me_wait();
            unsigned int me_cyc = me_shared_uncached[4];
            unsigned int me_us = me_cycles_to_us(me_cyc);
            float me_bytes = (float)me_words * 4.0f * me_loops;
            float me_mbps = me_bytes / (float)me_us;
            r->time_us = (sp_us > me_us) ? sp_us : me_us;
            r->metric = sp_mbps;
            union { float f; unsigned int u; } conv;
            conv.f = me_mbps;
            r->check = conv.u;
        }
    }
    r->valid = 1;
}

/* ============================================================
 *  GU Drawing
 * ============================================================ */
static unsigned int __attribute__((aligned(16))) gu_list[131072];

typedef struct {
    unsigned int color;
    float x, y, z;
} Vertex;
#define VERTEX_FORMAT (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

#define MAX_VERTS 8192
static Vertex verts[MAX_VERTS] __attribute__((aligned(16)));
static int vtx_count = 0;

static unsigned int make_rgba(int r, int g, int b, int a) {
    return ((unsigned int)a << 24) | ((unsigned int)b << 16) |
           ((unsigned int)g << 8) | (unsigned int)r;
}

static void flush_draw(void) {
    if (vtx_count == 0) return;
    sceKernelDcacheWritebackRange(verts, vtx_count * sizeof(Vertex));
    sceGuDrawArray(GU_TRIANGLES, VERTEX_FORMAT, vtx_count, NULL, verts);
    vtx_count = 0;
}

static void emit_quad(float x0, float y0, float x1, float y1,
                      float x2, float y2, float x3, float y3,
                      unsigned int c0, unsigned int c1, unsigned int c2, unsigned int c3) {
    if (vtx_count + 6 > MAX_VERTS) flush_draw();
    Vertex *v = &verts[vtx_count];
    v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
    v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0;
    v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0;
    v[3].color = c0; v[3].x = x0; v[3].y = y0; v[3].z = 0;
    v[4].color = c2; v[4].x = x2; v[4].y = y2; v[4].z = 0;
    v[5].color = c3; v[5].x = x3; v[5].y = y3; v[5].z = 0;
    vtx_count += 6;
}

static void emit_rect(float x, float y, float w, float h, unsigned int color) {
    emit_quad(x, y, x+w, y, x+w, y+h, x, y+h, color, color, color, color);
}

static void gu_init(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, gu_list);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, BUF_WIDTH);
    sceGuDispBuffer(SCREEN_W, SCREEN_H, (void*)(BUF_WIDTH * SCREEN_H * 4), BUF_WIDTH);
    sceGuDepthBuffer((void*)(BUF_WIDTH * SCREEN_H * 4 * 2), BUF_WIDTH);
    sceGuOffset(2048 - SCREEN_W/2, 2048 - SCREEN_H/2);
    sceGuViewport(2048, 2048, SCREEN_W, SCREEN_H);
    sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/* ============================================================
 *  Screenshot (BMP)
 * ============================================================ */
static int screenshot_count = 0;

/* BMP buffer: 54 header + 480*272*3 pixels = ~391KB */
static unsigned char ss_buf[54 + SCREEN_W * SCREEN_H * 3] __attribute__((aligned(16)));

static void save_screenshot(void) {
    char path[64];
    snprintf(path, sizeof(path), "ms0:/mebench_%d.bmp", screenshot_count++);

    /* Get current display framebuffer */
    void *topaddr;
    int bufferwidth, pixelformat;
    sceDisplayGetFrameBuf(&topaddr, &bufferwidth, &pixelformat, 0);
    if (!topaddr) return;
    /* Use uncached address for VRAM read */
    unsigned int *vram = (unsigned int *)(0x44000000 | (unsigned int)topaddr);

    int w = SCREEN_W, h = SCREEN_H;
    int row_bytes = w * 3;
    int file_size = 54 + row_bytes * h;

    /* BMP header */
    memset(ss_buf, 0, 54);
    ss_buf[0] = 'B'; ss_buf[1] = 'M';
    ss_buf[2] = file_size; ss_buf[3] = file_size >> 8;
    ss_buf[4] = file_size >> 16; ss_buf[5] = file_size >> 24;
    ss_buf[10] = 54;
    ss_buf[14] = 40;
    ss_buf[18] = w; ss_buf[19] = w >> 8;
    ss_buf[22] = h; ss_buf[23] = h >> 8;
    ss_buf[26] = 1;
    ss_buf[28] = 24;
    int img_size = row_bytes * h;
    ss_buf[34] = img_size; ss_buf[35] = img_size >> 8;
    ss_buf[36] = img_size >> 16; ss_buf[37] = img_size >> 24;

    /* Convert pixels bottom-up */
    for (int y = 0; y < h; y++) {
        unsigned int *src = vram + (h - 1 - y) * bufferwidth;
        unsigned char *dst = ss_buf + 54 + y * row_bytes;
        for (int x = 0; x < w; x++) {
            unsigned int px = src[x];
            /* VRAM uint32 (LE): A<<24|B<<16|G<<8|R */
            /* BMP pixel order: B, G, R */
            dst[x * 3 + 0] = (px >> 16) & 0xFF;  /* B */
            dst[x * 3 + 1] = (px >> 8) & 0xFF;   /* G */
            dst[x * 3 + 2] = px & 0xFF;           /* R */
        }
    }

    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, ss_buf, file_size);
        sceIoClose(fd);
    }
}

/* ============================================================
 *  Segment Font (from PMD Visualizer)
 * ============================================================ */
typedef struct { signed char x0,y0,x1,y1; } FontSeg;
#define FS_END {-1,-1,-1,-1}
static const FontSeg seg_font[][7] = {
    /* A-Z */
    {{0,4,0,1},{0,1,2,0},{2,0,4,1},{4,1,4,4},{0,2,4,2},FS_END,FS_END},
    {{0,0,0,4},{0,0,3,0},{3,0,4,1},{4,1,3,2},{0,2,3,2},{3,2,4,3},{4,3,0,4}},
    {{4,0,0,0},{0,0,0,4},{0,4,4,4},FS_END,FS_END,FS_END,FS_END},
    {{0,0,0,4},{0,0,3,0},{3,0,4,2},{4,2,3,4},{3,4,0,4},FS_END,FS_END},
    {{4,0,0,0},{0,0,0,4},{0,4,4,4},{0,2,3,2},FS_END,FS_END,FS_END},
    {{4,0,0,0},{0,0,0,4},{0,2,3,2},FS_END,FS_END,FS_END,FS_END},
    {{4,0,0,0},{0,0,0,4},{0,4,4,4},{4,4,4,2},{4,2,2,2},FS_END,FS_END},
    {{0,0,0,4},{4,0,4,4},{0,2,4,2},FS_END,FS_END,FS_END,FS_END},
    {{1,0,3,0},{2,0,2,4},{1,4,3,4},FS_END,FS_END,FS_END,FS_END},
    {{1,0,4,0},{3,0,3,4},{3,4,0,4},{0,4,0,3},FS_END,FS_END,FS_END},
    {{0,0,0,4},{4,0,0,2},{0,2,4,4},FS_END,FS_END,FS_END,FS_END},
    {{0,0,0,4},{0,4,4,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,4,0,0},{0,0,2,2},{2,2,4,0},{4,0,4,4},FS_END,FS_END,FS_END},
    {{0,4,0,0},{0,0,4,4},{4,4,4,0},FS_END,FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,4,0,0},FS_END,FS_END,FS_END},
    {{0,4,0,0},{0,0,4,0},{4,0,4,2},{4,2,0,2},FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,4,0,0},{3,3,4,4},FS_END,FS_END},
    {{0,4,0,0},{0,0,4,0},{4,0,4,2},{4,2,0,2},{0,2,4,4},FS_END,FS_END},
    {{4,0,0,0},{0,0,0,2},{0,2,4,2},{4,2,4,4},{4,4,0,4},FS_END,FS_END},
    {{0,0,4,0},{2,0,2,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,0,0,4},{0,4,4,4},{4,4,4,0},FS_END,FS_END,FS_END,FS_END},
    {{0,0,2,4},{2,4,4,0},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,0,0,4},{0,4,2,2},{2,2,4,4},{4,4,4,0},FS_END,FS_END,FS_END},
    {{0,0,4,4},{4,0,0,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,0,2,2},{4,0,2,2},{2,2,2,4},FS_END,FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,0,4},{0,4,4,4},FS_END,FS_END,FS_END,FS_END},
    /* 0-9 */
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,4,0,0},{0,4,4,0},FS_END,FS_END},
    {{1,1,2,0},{2,0,2,4},{1,4,3,4},FS_END,FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,4,2},{4,2,0,2},{0,2,0,4},{0,4,4,4},FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,2,4,2},FS_END,FS_END,FS_END},
    {{0,0,0,2},{0,2,4,2},{4,0,4,4},FS_END,FS_END,FS_END,FS_END},
    {{4,0,0,0},{0,0,0,2},{0,2,4,2},{4,2,4,4},{4,4,0,4},FS_END,FS_END},
    {{4,0,0,0},{0,0,0,4},{0,4,4,4},{4,4,4,2},{4,2,0,2},FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,4,0,0},{0,2,4,2},FS_END,FS_END},
    {{0,2,4,2},{4,2,4,4},{0,0,4,0},{4,0,0,0},{0,0,0,2},FS_END,FS_END},
    /* space : + - . / < > ( ) x = */
    {FS_END,FS_END,FS_END,FS_END,FS_END,FS_END,FS_END},
    {{2,1,2,1},{2,3,2,3},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{2,1,2,3},{1,2,3,2},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{1,2,3,2},FS_END,FS_END,FS_END,FS_END,FS_END,FS_END},
    {{2,4,2,4},FS_END,FS_END,FS_END,FS_END,FS_END,FS_END},
    {{1,1,3,3},FS_END,FS_END,FS_END,FS_END,FS_END,FS_END},
    {{3,0,1,2},{1,2,3,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{1,0,3,2},{3,2,1,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{3,0,1,2},{1,2,3,4},FS_END,FS_END,FS_END,FS_END,FS_END}, /* ( same as < */
    {{1,0,3,2},{3,2,1,4},FS_END,FS_END,FS_END,FS_END,FS_END}, /* ) same as > */
    {{0,0,4,4},{4,0,0,4},FS_END,FS_END,FS_END,FS_END,FS_END}, /* x */
    {{0,1,4,1},{0,3,4,3},FS_END,FS_END,FS_END,FS_END,FS_END}, /* = */
};
static int seg_char_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == ' ') return 36;
    if (c == ':') return 37;
    if (c == '+') return 38;
    if (c == '-') return 39;
    if (c == '.') return 40;
    if (c == '/') return 41;
    if (c == '<') return 42;
    if (c == '>') return 43;
    if (c == '(') return 44;
    if (c == ')') return 45;
    if (c == 'x') return 46;
    if (c == '=') return 47;
    return 36;
}
static void draw_seg_string(float x, float y, float char_w, float char_h, const char *str, unsigned int color) {
    float th = 0.8f;
    while (*str) {
        int idx = seg_char_index(*str);
        if (idx >= 0 && idx < (int)(sizeof(seg_font)/sizeof(seg_font[0]))) {
            const FontSeg *segs = seg_font[idx];
            for (int s = 0; s < 7; s++) {
                if (segs[s].x0 < 0) break;
                float x0 = x + segs[s].x0 * char_w / 4.0f;
                float y0 = y + segs[s].y0 * char_h / 4.0f;
                float x1 = x + segs[s].x1 * char_w / 4.0f;
                float y1 = y + segs[s].y1 * char_h / 4.0f;
                float dx = x1 - x0, dy = y1 - y0;
                float len = sqrtf(dx*dx + dy*dy);
                if (len < 0.001f) {
                    emit_quad(x0-th, y0-th, x0+th, y0-th, x0+th, y0+th, x0-th, y0+th,
                              color, color, color, color);
                } else {
                    float nx = -dy/len*th, ny = dx/len*th;
                    emit_quad(x0+nx, y0+ny, x0-nx, y0-ny, x1-nx, y1-ny, x1+nx, y1+ny,
                              color, color, color, color);
                }
            }
        }
        x += char_w + 1.0f;
        str++;
    }
}

/* ============================================================
 *  HUD Rendering
 * ============================================================ */
static void format_us(char *buf, int size, unsigned int us) {
    if (us >= 1000000)
        snprintf(buf, size, "%.2fs", (float)us / 1000000.0f);
    else if (us >= 1000)
        snprintf(buf, size, "%.1fms", (float)us / 1000.0f);
    else
        snprintf(buf, size, "%uus", us);
}

static void draw_mandelbrot(float ox, float oy, float scale) {
    /* 8x8ブロック単位で平均色を描画 (頂点数を抑える) */
    int bx_step = 8, by_step = 8;
    for (int py = 0; py < MANDEL_H; py += by_step) {
        for (int px = 0; px < MANDEL_W; px += bx_step) {
            int sum = 0, cnt = 0;
            for (int dy = 0; dy < by_step && py+dy < MANDEL_H; dy++) {
                for (int dx = 0; dx < bx_step && px+dx < MANDEL_W; dx++) {
                    sum += mandel_buf[(py+dy) * MANDEL_W + (px+dx)];
                    cnt++;
                }
            }
            int avg = sum / cnt;
            if (avg >= MANDEL_ITER) continue;
            float t = (float)avg / (float)MANDEL_ITER;
            int r = (int)(t * 255) & 255;
            int g = (int)(t * 128) & 255;
            int b = (int)((1.0f - t) * 255) & 255;
            unsigned int c = make_rgba(r, g, b, 255);
            float x = ox + px * scale;
            float y = oy + py * scale;
            emit_rect(x, y, bx_step * scale, by_step * scale, c);
        }
    }
}

static void draw_hud(const char *status_msg) {
    unsigned int white = make_rgba(255, 255, 255, 255);
    unsigned int cyan = make_rgba(0, 255, 255, 255);
    unsigned int green = make_rgba(0, 255, 128, 255);
    unsigned int yellow = make_rgba(255, 255, 0, 255);
    unsigned int gray = make_rgba(128, 128, 128, 255);
    unsigned int dark = make_rgba(20, 20, 40, 255);

    /* background */
    emit_rect(0, 0, SCREEN_W, SCREEN_H, dark);

    /* title */
    draw_seg_string(10, 4, 12, 16, "PSP ME BENCHMARK", cyan);

    /* column headers */
    float col_x[4] = {120, 210, 310, 410};
    for (int p = 0; p < NUM_PATTERNS; p++) {
        draw_seg_string(col_x[p], 26, 7, 10, pattern_names[p],
                        (p == current_pattern && bench_running) ? yellow : white);
    }

    /* results grid */
    for (int b = 0; b < NUM_BENCH; b++) {
        float y = 50 + b * 65;
        unsigned int name_c = (b == current_bench && bench_running) ? yellow : white;
        draw_seg_string(10, y, 9, 13, bench_names[b], name_c);

        for (int p = 0; p < NUM_PATTERNS; p++) {
            BenchResult *r = &results[p][b];
            if (!r->valid) {
                draw_seg_string(col_x[p], y, 6, 9, "---", gray);
                continue;
            }

            char buf[32];
            format_us(buf, sizeof(buf), r->time_us);
            draw_seg_string(col_x[p], y, 6, 9, buf, green);

            /* speedup vs pattern 0 (SC 222) */
            if (p > 0 && results[0][b].valid && results[0][b].time_us > 0) {
                float speedup = (float)results[0][b].time_us / (float)r->time_us;
                snprintf(buf, sizeof(buf), "x%.2f", (double)speedup);
                draw_seg_string(col_x[p], y + 16, 5, 7, buf, yellow);
            }
        }
    }

    /* mandelbrot preview */
    if (!bench_running) {
        int has_mandel = 0;
        for (int p = 0; p < NUM_PATTERNS; p++) {
            if (results[p][2].valid) { has_mandel = 1; break; }
        }
        if (has_mandel) {
            draw_mandelbrot(350, 200, 0.4f);
        }
    }

    /* status bar */
    draw_seg_string(10, SCREEN_H - 24, 7, 10, status_msg, white);

    /* controls */
    draw_seg_string(10, SCREEN_H - 12, 4, 6, "O:RUN X:STOP T:AUTO SEL:USB R:SS", gray);

    /* USB indicator */
    if (usb_active) {
        draw_seg_string(420, SCREEN_H - 12, 4, 6, "USB", green);
    }

    flush_draw();
}

/* ============================================================
 *  CSV Logger
 * ============================================================ */
static void save_csv(void) {
    SceUID fd = sceIoOpen("ms0:/me_bench.csv",
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;

    /* Write ME init info first */
    {
        char info[128];
        snprintf(info, sizeof(info), "# me_init_ret=%d me_active=%d\n",
                 me_init_ret, me_active);
        sceIoWrite(fd, info, strlen(info));
    }
    const char *hdr = "bench,pattern,time_us,check,metric,cpu_mhz,bus_mhz,set_ret\n";
    sceIoWrite(fd, hdr, strlen(hdr));

    for (int b = 0; b < NUM_BENCH; b++) {
        for (int p = 0; p < NUM_PATTERNS; p++) {
            BenchResult *r = &results[p][b];
            if (!r->valid) continue;
            char line[128];
            snprintf(line, sizeof(line), "%s,%s,%u,%u,%.2f,%d,%d,%d\n",
                     bench_names[b], pattern_names[p],
                     r->time_us, r->check, (double)r->metric,
                     r->cpu_mhz, r->bus_mhz, r->set_ret);
            sceIoWrite(fd, line, strlen(line));
        }
    }

    sceIoClose(fd);
}

/* ============================================================
 *  USB Control
 * ============================================================ */
static void usb_init(void) {
    /* Load USB kernel modules */
    pspSdkLoadStartModule("flash0:/kd/semawm.prx", PSP_MEMORY_PARTITION_KERNEL);
    pspSdkLoadStartModule("flash0:/kd/usbstor.prx", PSP_MEMORY_PARTITION_KERNEL);
    pspSdkLoadStartModule("flash0:/kd/usbstormgr.prx", PSP_MEMORY_PARTITION_KERNEL);
    pspSdkLoadStartModule("flash0:/kd/usbstorms.prx", PSP_MEMORY_PARTITION_KERNEL);
    pspSdkLoadStartModule("flash0:/kd/usbstorboot.prx", PSP_MEMORY_PARTITION_KERNEL);
    sceUsbStart("USBBusDriver", 0, 0);
    sceUsbStart("USBStor_Driver", 0, 0);
    sceUsbstorBootSetCapacity(0x800000);
}

static void usb_toggle(void) {
    if (!usb_active) {
        sceUsbActivate(0x1c8);
        usb_active = 1;
    } else {
        sceUsbDeactivate(0x1c8);
        usb_active = 0;
    }
}

static void usb_stop(void) {
    if (usb_active) {
        sceUsbDeactivate(0x1c8);
        usb_active = 0;
    }
    sceUsbStop("USBStor_Driver", 0, 0);
    sceUsbStop("USBBusDriver", 0, 0);
}

/* ============================================================
 *  Exit Callbacks
 * ============================================================ */
static int exit_callback(int arg1, int arg2, void *common) {
    (void)arg1; (void)arg2; (void)common;
    running = 0;
    return 0;
}
static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("cb_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
}

/* ============================================================
 *  Main
 * ============================================================ */
int main(void) {
    setup_callbacks();
    scePowerSetClockFrequency(333, 333, 166);

    gu_init();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    usb_init();

    /* ME init */
    me_shared_uncached = (volatile unsigned int *)(0x40000000 | (unsigned int)_me_shared);
    memset((void *)_me_shared, 0, sizeof(_me_shared));
    sceKernelDcacheWritebackInvalidateAll();
    {
        me_init_ret = meLibDefaultInit();
        if (me_init_ret >= 0) {
            me_active = 1;
        }
    }
    if (me_active) {
        sceKernelDelayThread(50000);
    }

    memset(results, 0, sizeof(results));
    char status[80];
    snprintf(status, sizeof(status), "ME:%d READY - PRESS T", me_init_ret);
    int auto_mode = 0;
    int auto_bench = 0;
    int auto_pattern = 0;
    SceCtrlData pad, pad_prev;
    memset(&pad_prev, 0, sizeof(pad_prev));

    while (running) {
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~pad_prev.Buttons;
        pad_prev = pad;

        if (!bench_running) {
            /* Triangle: auto run all */
            if (pressed & PSP_CTRL_TRIANGLE) {
                auto_mode = 1;
                auto_bench = 0;
                auto_pattern = 0;
                bench_running = 1;
            }
            /* Circle: run single (next unfinished) */
            if (pressed & PSP_CTRL_CIRCLE) {
                /* find next unfinished */
                int found = 0;
                for (int p = 0; p < NUM_PATTERNS && !found; p++) {
                    for (int b = 0; b < NUM_BENCH && !found; b++) {
                        if (!results[p][b].valid) {
                            current_pattern = p;
                            current_bench = b;
                            found = 1;
                        }
                    }
                }
                if (found) {
                    auto_mode = 0;
                    bench_running = 1;
                }
            }
            /* Start: save CSV */
            if (pressed & PSP_CTRL_START) {
                save_csv();
                snprintf(status, sizeof(status), "CSV SAVED");
            }
        }

        /* R trigger: screenshot */
        if (pressed & PSP_CTRL_RTRIGGER) {
            save_screenshot();
            snprintf(status, sizeof(status), "SCREENSHOT SAVED %d", screenshot_count);
        }

        /* Select: USB toggle */
        if (pressed & PSP_CTRL_SELECT) {
            usb_toggle();
            snprintf(status, sizeof(status), "USB %s", usb_active ? "ON" : "OFF");
        }

        /* Cross: abort */
        if ((pressed & PSP_CTRL_CROSS) && bench_running) {
            bench_running = 0;
            auto_mode = 0;
            snprintf(status, sizeof(status), "ABORTED");
        }

        /* Run benchmark step */
        if (bench_running) {
            if (auto_mode) {
                current_pattern = auto_pattern;
                current_bench = auto_bench;
            }

            snprintf(status, sizeof(status), "%s %s...",
                     bench_names[current_bench], pattern_names[current_pattern]);

            /* Draw progress before benchmark */
            sceGuStart(GU_DIRECT, gu_list);
            sceGuClearColor(make_rgba(10, 10, 30, 255));
            sceGuClear(GU_COLOR_BUFFER_BIT);
            draw_hud(status);
            sceGuFinish();
            sceGuSync(0, 0);
            sceDisplayWaitVblankStart();
            sceGuSwapBuffers();

            /* Execute */
            int dual = (current_pattern == 1 || current_pattern == 3);
            if (dual && !me_active) {
                /* skip dual if ME not available */
                results[current_pattern][current_bench].valid = 0;
            } else {
                run_benchmark(current_bench, current_pattern);
            }

            /* Restore clock to 333 */
            scePowerSetClockFrequency(333, 333, 166);

            if (auto_mode) {
                auto_bench++;
                if (auto_bench >= NUM_BENCH) {
                    auto_bench = 0;
                    auto_pattern++;
                    if (auto_pattern >= NUM_PATTERNS) {
                        bench_running = 0;
                        auto_mode = 0;
                        save_csv();
                        snprintf(status, sizeof(status), "ALL DONE - CSV SAVED");
                    }
                }
            } else {
                bench_running = 0;
                snprintf(status, sizeof(status), "DONE");
            }
        }

        /* Render */
        sceGuStart(GU_DIRECT, gu_list);
        sceGuClearColor(make_rgba(10, 10, 30, 255));
        sceGuClear(GU_COLOR_BUFFER_BIT);
        draw_hud(status);
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    /* Cleanup */
    if (me_active) {
        me_shared_uncached[0] = ME_CMD_STOP;
        while (me_shared_uncached[1] != ME_STAT_DONE) {
            asm volatile("nop");
        }
    }
    usb_stop();
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
