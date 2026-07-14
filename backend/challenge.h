#ifndef CHALLENGE_H
#define CHALLENGE_H

typedef enum {
    CH_LIFT_UP = 0,
    CH_LOWER_DOWN,
    CH_UPSIDE_DOWN,
    CH_SHAKE_TWICE,
    CH_COUNT
} challenge_type_t;

const char *challenge_type_name(challenge_type_t t);
challenge_type_t challenge_type_from_name(const char *name);

/* Picks a random challenge type. Call srand() once at program start. */
challenge_type_t challenge_pick_random(void);

/* Human readable instruction text sent to the client */
const char *challenge_instruction(challenge_type_t t);

#endif
