/**
 * @file as11_stream.h
 * @brief One second of the AirSense 11's stream as a CDS1 block (SDD-001).
 *
 * The machine reports every 200 ms: five samples of each fast channel
 * (PatientFlow, MaskPressure at 40 ms) and five repeats of each slow one. This
 * module gathers them into ONE-SECOND blocks in the wire format hms-cpap
 * takes, "CDS1" (hms-cpap's docs/MULE_LOCAL_STREAM.md):
 *
 *   magic 4 "CDS1", version 1, nchan 1, interval_ms 2 LE, start_ms 8 LE (the
 *   machine's UTC clock), serial_len 1 + serial, then per channel: id 1,
 *   cadence 1 (0 = per interval, 1 = per 2 s), count 2 LE, int16 LE values.
 *   INT16_MIN is "the machine sent none".
 *
 * The mule merges these into blocks of up to 10 s and streams them to
 * hms-cpap. Units are integers hms-cpap scales back: flow mL/s, pressures
 * 0.01 cmH2O, leak mL/s, respiratory rate 0.1/min, tidal volume mL, minute
 * ventilation 0.01 L/min, snore 0.01, flow limitation 0.001, SpO2 0.1 %,
 * heart rate bpm.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AS11_STREAM_INTERVAL_MS   40
#define AS11_STREAM_FAST_PER_SEC  25
#define AS11_STREAM_NULL          INT16_MIN
/* A one-second block: header 17 + serial 16 + 2 fast (4 + 50) + 11 slow (4 + 2). */
#define AS11_STREAM_BLOCK_MAX     220

typedef enum {
    AS11_CH_FLOW = 1, AS11_CH_MASK_PRESSURE = 2, AS11_CH_MASK_PRESS_2S = 3,
    AS11_CH_INSP_PRESS_2S = 4, AS11_CH_EXP_PRESS_2S = 5, AS11_CH_LEAK = 6,
    AS11_CH_RESP_RATE = 7, AS11_CH_TIDAL_VOLUME = 8, AS11_CH_MINUTE_VENT = 9,
    AS11_CH_SNORE = 10, AS11_CH_FLOW_LIMITATION = 11, AS11_CH_SPO2 = 12,
    AS11_CH_HEART_RATE = 13,
} as11_channel_t;

/* The second being filled. */
typedef struct {
    int64_t start_ms;                            /* the second's start on the machine's clock; 0 = empty */
    int     fast_n;                              /* samples of flow/pressure so far (0..25) */
    int16_t flow[AS11_STREAM_FAST_PER_SEC];
    int16_t press[AS11_STREAM_FAST_PER_SEC];
    int16_t slow[14];                            /* by channel id, the second's value (one per 2 s is enough) */
    bool    slow_set[14];
} as11_second_t;

void as11_second_reset(as11_second_t *s);

/* One StreamData report, already parsed: `values` by channel id, NaN for a
 * null; fast channels carry `n` samples at `interval_ms` from `start_ms`.
 * Appends into `s`; returns true when the second is full (25 fast samples),
 * which is the caller's cue to encode and reset. A report whose start is not
 * contiguous with the second being filled closes that second first (the
 * caller must encode what `s` holds when `*closed` is set). */
bool as11_second_feed(as11_second_t *s, int64_t start_ms, int interval_ms, int n,
                      const float *flow_lps, const float *press_cmh2o,
                      const float slow_by_id[14], bool *closed);

/* Encode the second as a CDS1 block. Returns the length, 0 for an empty
 * second. `serial` may be "". */
size_t as11_second_encode(const as11_second_t *s, const char *serial, uint8_t *out, size_t cap);

/* hms-cpap's integer for a channel's float, or AS11_STREAM_NULL for NaN. */
int16_t as11_stream_quantise(as11_channel_t ch, float v);

/* The ring rides the stream. The scanner task sets the ring's latest reading
 * (SpO2 in %, heart rate in bpm, `valid` the ring's own flag) with the time it
 * was read; the host task applies it to each second as it closes. A reading
 * older than AS11_RING_STALE_MS, or an invalid one, leaves channels 12 and 13
 * absent (an absent channel, not a zero). */
#define AS11_RING_STALE_MS 5000
void as11_stream_set_ring(bool valid, uint8_t spo2, uint8_t hr, int64_t at_ms);
void as11_stream_clear_ring(void);
void as11_second_apply_ring(as11_second_t *s, int64_t now_ms);
