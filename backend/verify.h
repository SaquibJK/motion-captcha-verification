#ifndef VERIFY_H
#define VERIFY_H

#include "challenge.h"

/* One motion sample sent from the browser.
 * t_ms       : milliseconds since recording started
 * alpha/beta/gamma : deviceorientation angles in degrees
 * ax, ay, az : devicemotion acceleration (including gravity), m/s^2
 */
typedef struct {
    double t_ms;
    double alpha, beta, gamma;
    double ax, ay, az;
} motion_sample_t;

/* Orientation captured during the ~1s calibration phase, before the
 * challenge was even shown. All rotation-based challenges are measured
 * relative to this starting position (not assumed to be "upright"),
 * since people naturally hold their phones differently. `valid` is 0
 * when a client didn't send calibration data, in which case verification
 * falls back to using the first recorded sample as the baseline. */
typedef struct {
    double beta;
    double gamma;
    int valid;
} calibration_t;

typedef struct {
    int success;            /* 1 = pass, 0 = fail */
    double confidence;       /* 0.0 - 100.0, weighted score across criteria */
    double duration_ms;       /* time span of recorded samples */
    char reason[256];        /* human readable explanation */
} verify_result_t;

/*
 * Runs verification of `samples` (array of length n) against the
 * expected movement for `type`, relative to `calibration` (may be NULL).
 * Fills in `out`.
 */
void verify_run(challenge_type_t type, const motion_sample_t *samples, int n,
                 const calibration_t *calibration, verify_result_t *out);

#endif
