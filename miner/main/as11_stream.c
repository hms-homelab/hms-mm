#include "as11_stream.h"

#include <math.h>
#include <string.h>

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void wr64(uint8_t *p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(u & 0xFF); u >>= 8; }
}

static int16_t clampi(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32767.0f) return -32767;
    return (int16_t)lroundf(v);
}

/* ── the ring's reading ──
 * Written by the scanner task, read by the host task. A seqlock keeps the
 * three fields consistent without an OS primitive (this file also builds on
 * the host). */
static volatile uint32_t s_ring_seq;
static volatile bool     s_ring_valid;
static volatile uint8_t  s_ring_spo2, s_ring_hr;
static volatile int64_t  s_ring_at_ms;

void as11_stream_set_ring(bool valid, uint8_t spo2, uint8_t hr, int64_t at_ms)
{
    s_ring_seq++;                 /* odd: being written */
    s_ring_valid = valid;
    s_ring_spo2  = spo2;
    s_ring_hr    = hr;
    s_ring_at_ms = at_ms;
    s_ring_seq++;                 /* even: settled */
}

void as11_stream_clear_ring(void) { as11_stream_set_ring(false, 0, 0, 0); }

void as11_second_apply_ring(as11_second_t *s, int64_t now_ms)
{
    bool valid; uint8_t spo2, hr; int64_t at;
    for (int tries = 0; tries < 8; tries++) {
        const uint32_t a = s_ring_seq;
        if (a & 1u) continue;
        valid = s_ring_valid; spo2 = s_ring_spo2; hr = s_ring_hr; at = s_ring_at_ms;
        if (s_ring_seq == a) goto settled;
    }
    return;                       /* a write in flight every time: this second goes without */
settled:
    if (!valid || at == 0 || now_ms - at > AS11_RING_STALE_MS) return;
    s->slow[AS11_CH_SPO2]           = (int16_t)spo2 * 10;
    s->slow_set[AS11_CH_SPO2]       = true;
    s->slow[AS11_CH_HEART_RATE]     = (int16_t)hr;
    s->slow_set[AS11_CH_HEART_RATE] = true;
}

int16_t as11_stream_quantise(as11_channel_t ch, float v)
{
    if (isnan(v)) return AS11_STREAM_NULL;
    switch (ch) {
    case AS11_CH_FLOW:            return clampi(v * 1000.0f);   /* L/s -> mL/s */
    case AS11_CH_MASK_PRESSURE:
    case AS11_CH_MASK_PRESS_2S:
    case AS11_CH_INSP_PRESS_2S:
    case AS11_CH_EXP_PRESS_2S:    return clampi(v * 100.0f);
    case AS11_CH_LEAK:            return clampi(v * 1000.0f);   /* L/s -> mL/s */
    case AS11_CH_RESP_RATE:       return clampi(v * 10.0f);
    case AS11_CH_TIDAL_VOLUME:    return clampi(v * 1000.0f);   /* L -> mL */
    case AS11_CH_MINUTE_VENT:     return clampi(v * 100.0f);
    case AS11_CH_SNORE:           return clampi(v * 100.0f);
    case AS11_CH_FLOW_LIMITATION: return clampi(v * 1000.0f);
    case AS11_CH_SPO2:            return clampi(v * 10.0f);
    case AS11_CH_HEART_RATE:      return clampi(v);
    }
    return AS11_STREAM_NULL;
}

void as11_second_reset(as11_second_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 14; i++) s->slow[i] = AS11_STREAM_NULL;
}

bool as11_second_feed(as11_second_t *s, int64_t start_ms, int interval_ms, int n,
                      const float *flow_lps, const float *press_cmh2o,
                      const float slow_by_id[14], bool *closed)
{
    *closed = false;
    if (n <= 0 || interval_ms <= 0) return false;
    if (s->start_ms == 0) {
        s->start_ms = start_ms;
    } else {
        /* Contiguous means this report begins where the second's samples end
         * (a report's own 200 ms of jitter is allowed, a lost report is not). */
        const int64_t expect = s->start_ms + (int64_t)s->fast_n * interval_ms;
        if (start_ms - expect > interval_ms * 2 || expect - start_ms > interval_ms * 2) {
            *closed = true;
            return false;
        }
    }
    for (int i = 0; i < n && s->fast_n < AS11_STREAM_FAST_PER_SEC; i++, s->fast_n++) {
        s->flow[s->fast_n]  = as11_stream_quantise(AS11_CH_FLOW, flow_lps ? flow_lps[i] : NAN);
        s->press[s->fast_n] = as11_stream_quantise(AS11_CH_MASK_PRESSURE, press_cmh2o ? press_cmh2o[i] : NAN);
    }
    if (slow_by_id) {
        for (int id = 3; id <= 13; id++) {
            if (!isnan(slow_by_id[id])) {
                s->slow[id]     = as11_stream_quantise((as11_channel_t)id, slow_by_id[id]);
                s->slow_set[id] = true;
            }
        }
    }
    return s->fast_n >= AS11_STREAM_FAST_PER_SEC;
}

size_t as11_second_encode(const as11_second_t *s, const char *serial, uint8_t *out, size_t cap)
{
    if (s->start_ms == 0 || s->fast_n == 0) return 0;
    const size_t slen = serial ? strnlen(serial, 32) : 0;
    size_t need = 17 + slen + 2 * (4 + 2 * (size_t)s->fast_n);
    int nslow = 0;
    for (int id = 3; id <= 13; id++) if (s->slow_set[id]) { nslow++; need += 6; }
    if (need > cap) return 0;

    uint8_t *p = out;
    memcpy(p, "CDS1", 4); p += 4;
    *p++ = 1;
    *p++ = (uint8_t)(2 + nslow);
    wr16(p, AS11_STREAM_INTERVAL_MS); p += 2;
    wr64(p, s->start_ms); p += 8;
    *p++ = (uint8_t)slen;
    if (slen) { memcpy(p, serial, slen); p += slen; }

    *p++ = AS11_CH_FLOW; *p++ = 0; wr16(p, (uint16_t)s->fast_n); p += 2;
    for (int i = 0; i < s->fast_n; i++) { wr16(p, (uint16_t)s->flow[i]); p += 2; }
    *p++ = AS11_CH_MASK_PRESSURE; *p++ = 0; wr16(p, (uint16_t)s->fast_n); p += 2;
    for (int i = 0; i < s->fast_n; i++) { wr16(p, (uint16_t)s->press[i]); p += 2; }
    for (int id = 3; id <= 13; id++) {
        if (!s->slow_set[id]) continue;
        *p++ = (uint8_t)id; *p++ = 1; wr16(p, 1); p += 2;
        wr16(p, (uint16_t)s->slow[id]); p += 2;
    }
    return (size_t)(p - out);
}
