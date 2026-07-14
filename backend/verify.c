#include "verify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------- tunable thresholds ----------------
 * The goal here is robust HUMAN verification, not precise motion
 * tracking: "did the user nudge the phone in roughly the right way?",
 * not "did they hit an exact angle?". Note that `gamma` is physically
 * capped at +/-90 deg by the DeviceOrientation spec, and real people
 * rarely twist a phone anywhere near that far for a quick gesture - so
 * thresholds here deliberately require only a small, clearly-intentional
 * movement rather than a large target angle. Every criterion below
 * contributes partial credit; nothing is an exact-angle pass/fail.
 */
#define MIN_SAMPLES              5
#define HARD_MIN_DURATION_MS   150.0   /* sanity floor: filters near-instant/replayed data */
#define HARD_MAX_DURATION_MS 15000.0   /* sanity ceiling */

#define TIME_GOOD_MIN_MS       1000.0  /* comfortable human timing window */
#define TIME_GOOD_MAX_MS       5000.0
#define TIME_SOFT_MARGIN_MS    1000.0  /* partial credit fades out over this margin outside the window */

#define SMOOTH_WINDOW               3  /* moving-average window (samples) for orientation deltas */
#define NOISE_FLOOR_DEG            0.3 /* per-step deltas below this are treated as tremor, not movement */

/* Quick Lift Up / Quick Lower Down - acceleration-based, using the
 * device's `az` channel (the axis the frontend already treats as the
 * gravity/vertical axis - see its `{x:0, y:0, z:9.81}` fallback for a
 * phone resting flat). A deliberate quick lift or lower produces the
 * classic two-part "elevator" signature: an initial push away from
 * rest (accelerometer reads above gravity when lifting, below gravity
 * when lowering) followed shortly by the opposite excursion as the
 * motion is stopped. The order of that pair - which excursion comes
 * first - is what distinguishes "up" from "down"; this looks at that
 * shape directly rather than tracking any full motion path. */
#define LIFT_DYNAMIC_THRESHOLD    4.0    /* m/s^2 deviation from gravity to count as an excursion */
#define LIFT_STRENGTH_TARGET      6.0    /* m/s^2 deviation for full "strength" credit */
#define LIFT_MAX_GAP_MS         800.0    /* max time between the two excursions to count as one quick gesture */

/* Turn Upside Down - beta-based, relative to the calibrated start.
 * Also lenient: a clear partial flip is enough, not a precise 180. */
#define UPSIDE_FULL_CREDIT_DEG   90.0
#define UPSIDE_MIN_REGISTER_DEG  25.0

/* Shake Twice */
#define GRAVITY                  9.81
#define SHAKE_DYNAMIC_THRESHOLD  6.0    /* m/s^2 deviation from gravity to count as a peak */
#define SHAKE_DEBOUNCE_MS      200.0    /* minimum time between distinct shake events */
#define SHAKE_MIN_PEAKS            2

#define PASS_SCORE_THRESHOLD    70.0

/* ---------------- small helpers ---------------- */

static double angle_diff(double a, double b) {
    /* shortest signed distance from a to b, in degrees, range (-180,180] */
    double d = fmod(b - a + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return d - 180.0;
}

static double clampd(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Simple moving-average smoothing over a series of per-step angle
 * deltas, so an isolated hand-tremor spike doesn't distort the
 * smoothness score or the net rotation. Averaging deltas (rather than
 * raw angles) also sidesteps any wraparound issues near +/-180 deg. */
static void smooth_series(const double *in, double *out, int n, int window) {
    int half = window / 2;
    for (int i = 0; i < n; i++) {
        int lo = i - half; if (lo < 0) lo = 0;
        int hi = i + half; if (hi >= n) hi = n - 1;
        double sum = 0.0;
        int cnt = 0;
        for (int j = lo; j <= hi; j++) { sum += in[j]; cnt++; }
        out[i] = cnt > 0 ? sum / cnt : in[i];
    }
}

/* Ramps from 0 credit at 0 degrees up to full credit at `full`
 * degrees, then stays at full credit forever after (no penalty for
 * "too much" movement - only "not enough" matters). */
static double ramp_score(double value, double full) {
    return clampd(value / full, 0.0, 1.0);
}

/* Piecewise-linear "how close is this value to the comfortable window"
 * score from 0.0 to 1.0, used for timing: full credit inside [lo, hi],
 * fading linearly to 0 over `margin` outside it. */
static double range_score(double value, double lo, double hi, double margin) {
    if (value >= lo && value <= hi) return 1.0;
    if (value < lo) return clampd(1.0 - (lo - value) / margin, 0.0, 1.0);
    return clampd(1.0 - (value - hi) / margin, 0.0, 1.0);
}

/* ---------------- rotation analysis (shared by rotate + upside-down) ---------------- */

typedef struct {
    double net_rotation;   /* smoothed net rotation, degrees (signed) */
    double consistency;    /* fraction of meaningful steps agreeing with the net direction, 0..1 */
} rotation_stats_t;

/* axis[] holds one orientation value (gamma or beta) per sample, with the
 * calibrated starting position (if available) prepended as axis[0] so
 * everything is measured relative to it, not to an assumed "upright"
 * start. */
static rotation_stats_t analyze_rotation(const double *axis, int n) {
    rotation_stats_t st = { 0.0, 1.0 };
    if (n < 2) return st;

    int steps = n - 1;
    double *raw = malloc(sizeof(double) * steps);
    double *smoothed = malloc(sizeof(double) * steps);
    for (int i = 0; i < steps; i++) raw[i] = angle_diff(axis[i], axis[i + 1]);
    smooth_series(raw, smoothed, steps, SMOOTH_WINDOW);

    double net = 0.0;
    for (int i = 0; i < steps; i++) net += smoothed[i];

    int overall_dir = net >= 0 ? 1 : -1;
    int agree = 0, total = 0;
    for (int i = 0; i < steps; i++) {
        if (fabs(smoothed[i]) < NOISE_FLOOR_DEG) continue; /* ignore tremor-level steps */
        total++;
        int step_dir = smoothed[i] >= 0 ? 1 : -1;
        if (step_dir == overall_dir) agree++;
    }

    st.net_rotation = net;
    st.consistency = total > 0 ? (double)agree / (double)total : 1.0; /* no meaningful steps -> don't punish */

    free(raw);
    free(smoothed);
    return st;
}

/* Builds the axis series for gamma or beta, anchored to the calibrated
 * starting position when the client provided one (spec: "always compare
 * movements relative to the calibrated starting position"). Falls back
 * to using the first recorded sample as the baseline otherwise. */
static rotation_stats_t analyze_axis(const motion_sample_t *s, int n, int use_beta,
                                      const calibration_t *cal) {
    int has_baseline = cal && cal->valid;
    int total_n = has_baseline ? n + 1 : n;
    double *axis = malloc(sizeof(double) * total_n);
    int offset = 0;
    if (has_baseline) {
        axis[0] = use_beta ? cal->beta : cal->gamma;
        offset = 1;
    }
    for (int i = 0; i < n; i++) axis[offset + i] = use_beta ? s[i].beta : s[i].gamma;
    rotation_stats_t st = analyze_rotation(axis, total_n);
    free(axis);
    return st;
}

/* ---------------- vertical jerk analysis (Quick Lift Up / Quick Lower Down) ---------------- */

typedef struct {
    double peak_high;     /* strongest positive az deviation from gravity, m/s^2 (0 if none found) */
    double peak_low;      /* strongest negative az deviation from gravity, m/s^2 (0 if none found) */
    double t_high;        /* timestamp of peak_high */
    double t_low;         /* timestamp of peak_low */
    int has_high;
    int has_low;
} vertical_jerk_stats_t;

/* Scans the whole recording for the single strongest upward excursion
 * (az above gravity) and the single strongest downward excursion (az
 * below gravity), along with when each occurred. Whichever excursion
 * happens first, and how quickly the other follows it, is what tells
 * "lift up" (push up, then stop) apart from "lower down" (drop down,
 * then stop) - the classic two-part accelerometer signature of a
 * deliberate quick vertical motion, sometimes called the "elevator
 * effect". No attempt is made to reconstruct the full motion path. */
static vertical_jerk_stats_t analyze_vertical_jerk(const motion_sample_t *s, int n) {
    vertical_jerk_stats_t st;
    memset(&st, 0, sizeof(st));

    for (int i = 0; i < n; i++) {
        double dyn = s[i].az - GRAVITY;
        if (dyn > 0 && dyn > st.peak_high) {
            st.peak_high = dyn;
            st.t_high = s[i].t_ms;
            st.has_high = 1;
        }
        if (dyn < 0 && dyn < st.peak_low) {
            st.peak_low = dyn;
            st.t_low = s[i].t_ms;
            st.has_low = 1;
        }
    }

    return st;
}

/* ---------------- per-challenge weighted scoring ----------------
 * Each challenge answers one question only: "did the user perform the
 * requested movement with reasonable accuracy?" Every criterion below
 * contributes partial credit; nothing is a hard reject except the
 * sanity checks in verify_run(). Pass threshold is a total score >= 70.
 */

/* Quick Lift Up / Quick Lower Down: az-based "elevator effect" check.
 *   correct excursion order, close enough together to be one quick
 *     gesture (up: high-then-low, down: low-then-high):         50 pts
 *   strength of the weaker of the two excursions:                30 pts
 *   completed within a comfortable time window:                  20 pts
 */
static void check_lift(const motion_sample_t *s, int n, int want_up,
                        double duration_ms, const char *label, verify_result_t *out) {
    vertical_jerk_stats_t st = analyze_vertical_jerk(s, n);

    int both_present = st.has_high && st.has_low;
    /* "up" needs the push (high) before the stop (low); "down" needs
     * the drop (low) before the catch (high). */
    int order_ok = both_present && (want_up ? (st.t_high < st.t_low) : (st.t_low < st.t_high));
    double gap_ms = order_ok ? fabs(st.t_low - st.t_high) : 0.0;
    int gap_ok = order_ok && (gap_ms <= LIFT_MAX_GAP_MS);

    double order_score = gap_ok ? 50.0 : 0.0;

    double weaker_mag = both_present ? fmin(st.peak_high, fabs(st.peak_low)) : 0.0;
    double strength_score = 30.0 * clampd(weaker_mag / LIFT_STRENGTH_TARGET, 0.0, 1.0);

    double time_pts = 20.0 * range_score(duration_ms, TIME_GOOD_MIN_MS, TIME_GOOD_MAX_MS, TIME_SOFT_MARGIN_MS);

    out->confidence = clampd(order_score + strength_score + time_pts, 0.0, 100.0);
    /* The defining requirement: both excursions must be present, in
     * the right order, close enough together to be one deliberate
     * quick motion - not a soft range like the other criteria. */
    out->success = (out->confidence >= PASS_SCORE_THRESHOLD) && gap_ok &&
                   (weaker_mag >= LIFT_DYNAMIC_THRESHOLD);
    snprintf(out->reason, sizeof(out->reason),
             "%s: %.0f/100 (sequence %s %.0f/50, strength %.1f m/s^2 %.0f/30, timing %.0f/20)",
             label, out->confidence, gap_ok ? "ok" : "wrong/missing", order_score,
             weaker_mag, strength_score, time_pts);
}

/* Turn Upside Down: beta-based. No direction requirement since flipping
 * either way ends in the same place.
 *   rotation amount (ramps to full credit at a partial flip): 50 pts
 *   smooth, continuous movement:                              30 pts
 *   completed within a comfortable time window:                20 pts
 */
static void check_upside_down(const motion_sample_t *s, int n, const calibration_t *cal,
                               double duration_ms, verify_result_t *out) {
    rotation_stats_t st = analyze_axis(s, n, 1 /* beta */, cal);
    double mag = fabs(st.net_rotation);

    double amount_score = 50.0 * ramp_score(mag, UPSIDE_FULL_CREDIT_DEG);
    double smooth_score = 30.0 * clampd(st.consistency, 0.0, 1.0);
    double time_pts = 20.0 * range_score(duration_ms, TIME_GOOD_MIN_MS, TIME_GOOD_MAX_MS, TIME_SOFT_MARGIN_MS);

    out->confidence = clampd(amount_score + smooth_score + time_pts, 0.0, 100.0);
    out->success = (out->confidence >= PASS_SCORE_THRESHOLD) && (mag >= UPSIDE_MIN_REGISTER_DEG);
    snprintf(out->reason, sizeof(out->reason),
             "Upside down: %.0f/100 (rotation %.0f deg %.0f/50, smoothness %.0f/30, timing %.0f/20)",
             out->confidence, mag, amount_score, smooth_score, time_pts);
}

/* Shake Twice: acceleration-magnitude peak detection.
 *   at least two distinct peaks, separated by >= 200ms: 50 pts
 *   peaks clearly above the noise threshold (strength):  30 pts
 *   completed within a comfortable time window:          20 pts
 */
static void check_shake(const motion_sample_t *s, int n, double duration_ms, verify_result_t *out) {
    int peaks = 0;
    double last_peak_t = -1e18;
    double peak_mag_sum = 0.0;
    double max_dynamic = 0.0;

    for (int i = 0; i < n; i++) {
        double mag = sqrt(s[i].ax * s[i].ax + s[i].ay * s[i].ay + s[i].az * s[i].az);
        double dynamic = fabs(mag - GRAVITY);
        if (dynamic > max_dynamic) max_dynamic = dynamic;
        /* Only count a peak once per shake event: require both a strong
         * enough deviation from gravity AND enough time since the last
         * counted peak, so a single shake's multiple noisy samples don't
         * get counted as several distinct events. */
        if (dynamic >= SHAKE_DYNAMIC_THRESHOLD && (s[i].t_ms - last_peak_t) >= SHAKE_DEBOUNCE_MS) {
            peaks++;
            peak_mag_sum += dynamic;
            last_peak_t = s[i].t_ms;
        }
    }

    double peak_count_score;
    if (peaks >= SHAKE_MIN_PEAKS) peak_count_score = 50.0;
    else if (peaks == 1) peak_count_score = 25.0;
    else peak_count_score = 0.0;

    double avg_peak_mag = peaks > 0 ? peak_mag_sum / peaks : 0.0;
    double strength_score = 30.0 * clampd(avg_peak_mag / (SHAKE_DYNAMIC_THRESHOLD * 1.5), 0.0, 1.0);
    double time_pts = 20.0 * range_score(duration_ms, TIME_GOOD_MIN_MS, TIME_GOOD_MAX_MS, TIME_SOFT_MARGIN_MS);

    out->confidence = clampd(peak_count_score + strength_score + time_pts, 0.0, 100.0);
    /* "At least two distinct peaks" is a defining requirement of this
     * challenge, not a soft range - a single strong shake can otherwise
     * score high enough on strength+timing alone to pass. */
    out->success = (out->confidence >= PASS_SCORE_THRESHOLD) && (peaks >= SHAKE_MIN_PEAKS);
    snprintf(out->reason, sizeof(out->reason),
             "Shake twice: %.0f/100 (%d distinct peak(s) %.0f/50, avg strength %.1f m/s^2 %.0f/30, timing %.0f/20)",
             out->confidence, peaks, peak_count_score, avg_peak_mag, strength_score, time_pts);
}

/* ---------------- entry point ---------------- */

void verify_run(challenge_type_t type, const motion_sample_t *samples, int n,
                 const calibration_t *calibration, verify_result_t *out) {
    memset(out, 0, sizeof(*out));

    if (n < MIN_SAMPLES) {
        out->success = 0;
        out->confidence = 0.0;
        snprintf(out->reason, sizeof(out->reason),
                 "Too few samples collected (%d, need >= %d)", n, MIN_SAMPLES);
        return;
    }

    double duration = samples[n - 1].t_ms - samples[0].t_ms;
    out->duration_ms = duration;

    /* Sanity checks only - not the "ideal timing" scoring window. These
     * exist purely to reject obviously degenerate input (near-instant or
     * absurdly long submissions), not to police normal human pacing. */
    if (duration < HARD_MIN_DURATION_MS || duration > HARD_MAX_DURATION_MS) {
        out->success = 0;
        out->confidence = 0.0;
        snprintf(out->reason, sizeof(out->reason),
                 "Duration %.0fms outside sane bounds [%.0f, %.0f]ms",
                 duration, HARD_MIN_DURATION_MS, HARD_MAX_DURATION_MS);
        return;
    }

    switch (type) {
        case CH_LIFT_UP:
            check_lift(samples, n, 1 /* push (high) then stop (low) */, duration, "Quick lift up", out);
            break;
        case CH_LOWER_DOWN:
            check_lift(samples, n, 0 /* drop (low) then stop (high) */, duration, "Quick lower down", out);
            break;
        case CH_UPSIDE_DOWN:
            check_upside_down(samples, n, calibration, duration, out);
            break;
        case CH_SHAKE_TWICE:
            check_shake(samples, n, duration, out);
            break;
        default:
            out->success = 0;
            out->confidence = 0.0;
            snprintf(out->reason, sizeof(out->reason), "Unknown challenge type");
            return;
    }

    out->duration_ms = duration;
}
