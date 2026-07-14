#include "challenge.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

const char *challenge_type_name(challenge_type_t t) {
    switch (t) {
        case CH_LIFT_UP:     return "quick_lift_up";
        case CH_LOWER_DOWN:  return "quick_lower_down";
        case CH_UPSIDE_DOWN:   return "upside_down";
        case CH_SHAKE_TWICE:   return "shake_twice";
        default: return "unknown";
    }
}

challenge_type_t challenge_type_from_name(const char *name) {
    if (!name) return CH_COUNT;
    if (strcmp(name, "quick_lift_up") == 0) return CH_LIFT_UP;
    if (strcmp(name, "quick_lower_down") == 0) return CH_LOWER_DOWN;
    if (strcmp(name, "upside_down") == 0) return CH_UPSIDE_DOWN;
    if (strcmp(name, "shake_twice") == 0) return CH_SHAKE_TWICE;
    return CH_COUNT; /* invalid */
}

/* Tracks the most recently issued challenge (server-wide) so the same
 * challenge is never handed out twice back-to-back. Plain random selection
 * can otherwise repeat the same challenge several times in a row, which
 * feels broken to users even though it's statistically "fair". */
static challenge_type_t g_last_issued = CH_COUNT; /* CH_COUNT = none yet */
static pthread_mutex_t g_last_issued_lock = PTHREAD_MUTEX_INITIALIZER;

challenge_type_t challenge_pick_random(void) {
    pthread_mutex_lock(&g_last_issued_lock);
    challenge_type_t pick;
    if (g_last_issued == CH_COUNT) {
        pick = (challenge_type_t)(rand() % CH_COUNT);
    } else {
        /* Choose uniformly among the CH_COUNT - 1 challenges that are not
         * the last one issued. */
        int offset = 1 + (rand() % (CH_COUNT - 1));
        pick = (challenge_type_t)((g_last_issued + offset) % CH_COUNT);
    }
    g_last_issued = pick;
    pthread_mutex_unlock(&g_last_issued_lock);
    return pick;
}

const char *challenge_instruction(challenge_type_t t) {
    switch (t) {
        case CH_LIFT_UP:     return "Quickly lift your phone up";
        case CH_LOWER_DOWN:  return "Quickly lower your phone down";
        case CH_UPSIDE_DOWN:   return "Turn your phone upside down";
        case CH_SHAKE_TWICE:   return "Shake your phone twice";
        default: return "Unknown challenge";
    }
}
