/*
 * Host test for the RDS encoder:
 *   - checks the checkword against an independent bit-serial CRC,
 *   - feeds a synthetic FM30 composite (pilot with frequency offset + audio
 *     + L-R subcarrier + noise) through the real dsp.c process(),
 *   - demodulates the produced RDS signal and decodes PS / RT from it.
 *
 * Build & run:  cc -O2 -DDSP_HOST_TEST -I../include -I. test_dsp.c ../src/dsp.c ../src/rds.c -lm -o test_dsp && ./test_dsp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "host_stub.h"
#include "dsp.h"
#include "rds.h"

void process(const uint16_t *in, uint16_t *out);

static uint16_t ref_crc(uint16_t data)          /* PiFmRds-style serial CRC */
{
    uint16_t crc = 0;
    for (int j = 0; j < 16; j++) {
        int bit = (data >> (15 - j)) & 1;
        int msb = (crc >> 9) & 1;
        crc = (crc << 1) & 0x3FF;
        if (msb ^ bit) crc ^= 0x1B9;
    }
    return crc;
}

#define SECONDS 6.0
int main(int argc, char **argv)
{
    /* ---- 1. checkword ---- */
    int bad = 0;
    for (uint32_t d = 0; d < 65536; d += 7)
        if (rds_checkword((uint16_t)d, 0) != ref_crc((uint16_t)d)) bad++;
    printf("checkword vs reference CRC: %s\n", bad ? "FAIL" : "ok");

    /* ---- 2. modulator ---- */
    double fs = 170e6 / 746.0;
    double fp = 19000.0 + (argc > 1 ? atof(argv[1]) : 3.0);   /* pilot offset Hz */
    double th = 1.234;                                          /* pilot start phase */
    memset(&rds, 0, sizeof rds);
    rds.pi = 0xC0DE; rds.ms = 1; rds.stereo = 1; rds.pty = 10;
    rds_set_ps("FM30TEST");
    rds_set_rt("Hello from the FM30 RDS encoder");
    dsp.rds_enable = true; dsp.level = 400; dsp.phase_deg = 0;
    dsp_core_init((float)fs);

    int nblk = (int)(SECONDS * fs / DSP_BLOCK);
    int ns = nblk * DSP_BLOCK;
    float *y = calloc(ns + 2 * DSP_BLOCK, sizeof(float));
    uint16_t in[DSP_BLOCK], out[DSP_BLOCK];
    int lock_at = -1;
    srand(1);
    for (int b = 0; b < nblk; b++) {
        for (int i = 0; i < DSP_BLOCK; i++) {
            double t = (b * DSP_BLOCK + i) / fs;
            double pp = 2 * M_PI * fp * t + th;
            double x = 2048 + 90 * cos(pp)                       /* pilot */
                     + 500 * sin(2 * M_PI * 1000 * t)            /* L+R audio */
                     + 300 * sin(2 * M_PI * 2500 * t) * cos(2 * pp) /* L-R DSB */
                     + 20.0 * ((rand() / (double)RAND_MAX) - 0.5);
            in[i] = (uint16_t)lrint(x);
        }
        process(in, out);
        if (lock_at < 0 && dsp.pilot_lock) lock_at = b;
        /* output of block b is played two blocks later */
        for (int i = 0; i < DSP_BLOCK; i++) {
            int k = (b + 2) * DSP_BLOCK + i;
            if (k < ns) y[k] = out[i] - 2048.0f;
        }
    }
    printf("pilot lock after %.0f ms, NCO %.3f Hz (true %.3f), ampl %.1f, err %.3f rad\n",
           lock_at * DSP_BLOCK / fs * 1000, dsp.pilot_hz, fp, dsp.pilot_ampl, dsp.pilot_err);

    /* ---- 3. coherent demod against 3x the TRUE pilot phase ---- */
    int start = (int)(2.0 * fs);                        /* skip acquisition */
    double ci = 0, cq = 0;
    float *bb = calloc(ns, sizeof(float));
    for (int k = start; k < ns; k++) {
        double p57 = 3 * (2 * M_PI * fp * k / fs + th);
        double y2 = (double)y[k] * y[k];                 /* DSB-SC: square -> 114 kHz line */
        ci += y2 * -cos(2 * p57); cq += y2 * sin(2 * p57);
    }
    double ang = 0.5 * atan2(cq, ci);                    /* carrier phase vs 3x pilot (mod 180) */
    printf("57k carrier phase vs 3x pilot: %.1f deg (mod 180)\n", ang * 180 / M_PI);
    for (int k = start; k < ns; k++) {
        double p57 = 3 * (2 * M_PI * fp * k / fs + th) + ang;
        bb[k] = (float)(y[k] * sin(p57));
    }

    /* ---- 4. biphase decode: bit rate = 3*fp/48, search bit phase ---- */
    double rb = 3 * fp / 48.0, spb = fs / rb;
    int nbits = (int)((ns - start) / spb) - 2;
    double best = -1; int bestoff = 0;
    for (int off = 0; off < 64; off++) {
        double o = off * spb / 64, m = 0;
        for (int n = 0; n < nbits; n++) {
            double a = 0, s0 = start + o + n * spb;
            for (int j = 0; j < (int)spb; j++) a += (j < spb / 2 ? 1 : -1) * bb[(int)(s0 + j)];
            m += fabs(a);
        }
        if (m > best) { best = m; bestoff = off; }
    }
    uint8_t *d = malloc(nbits), *raw = malloc(nbits);
    for (int n = 0; n < nbits; n++) {
        double a = 0, s0 = start + bestoff * spb / 64 + n * spb;
        for (int j = 0; j < (int)spb; j++) a += (j < spb / 2 ? 1 : -1) * bb[(int)(s0 + j)];
        d[n] = a > 0;
    }
    for (int n = 1; n < nbits; n++) raw[n] = d[n] ^ d[n - 1];

    /* ---- 5. block sync + decode (both polarities) ---- */
    static const uint16_t ofs[4] = { 0x0FC, 0x198, 0x168, 0x1B4 };
    for (int pol = 0; pol < 2; pol++) {
        char ps[9] = "        ", rt[65]; memset(rt, ' ', 64); rt[64] = 0;
        int good = 0, total = 0;
        unsigned pi = 0;
        for (int n = 1; n + 104 < nbits; n++) {
            uint16_t blk[4]; int ok = 1;
            for (int b = 0; b < 4 && ok; b++) {
                uint32_t w = 0;
                for (int j = 0; j < 26; j++) w = (w << 1) | (raw[n + b * 26 + j] ^ pol ^ 0);
                blk[b] = w >> 10;
                ok = ((w & 0x3FF) == rds_checkword(blk[b], ofs[b]));
            }
            if (!ok) continue;
            good++;
            pi = blk[0];
            int type = blk[1] >> 12, seg;
            if (type == 0) { seg = blk[1] & 3; ps[seg * 2] = blk[3] >> 8; ps[seg * 2 + 1] = blk[3] & 0xFF; }
            if (type == 2) { seg = blk[1] & 15;
                rt[seg*4] = blk[2] >> 8; rt[seg*4+1] = blk[2] & 0xFF; rt[seg*4+2] = blk[3] >> 8; rt[seg*4+3] = blk[3] & 0xFF; }
            n += 103;
        }
        total = (int)(nbits / 104);
        char *cr = strchr(rt, 0x0D); if (cr) *cr = 0;
        if (good) {
            printf("polarity %d: %d/%d groups CRC-ok  PI=%04X  PS=\"%s\"  RT=\"%s\"\n", pol, good, total, pi, ps, rt);
            int pass = good >= total - 2 && !strcmp(ps, "FM30TEST") && pi == 0xC0DE &&
                       !strncmp(rt, "Hello from the FM30 RDS encoder", 31);
            printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
            return pass && !bad ? 0 : 1;
        }
    }
    printf("RESULT: FAIL (no groups decoded)\n");
    return 1;
}
