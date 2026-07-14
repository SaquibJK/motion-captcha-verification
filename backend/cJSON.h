/*
  Minimal cJSON-style JSON library (MIT-style, written for this project).
  Provides just enough JSON parsing/building for the motion-captcha backend:
  parsing simple objects/arrays/numbers/strings/booleans, and building
  response objects. Not a full JSON spec implementation, but sufficient
  and well-tested for this project's message formats.
*/
#ifndef CJSON_MINI_H
#define CJSON_MINI_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type_t;

typedef struct json_value {
    json_type_t type;
    union {
        int boolean;
        double number;
        char *string;
    } u;
    /* for arrays and objects: linked list of children */
    struct json_value *child;   /* first child */
    struct json_value *next;    /* next sibling */
    char *key;                  /* set if this node is a member of an object */
} json_value_t;

/* Parse a NUL-terminated JSON string. Returns NULL on failure. */
json_value_t *json_parse(const char *text);

/* Free a parsed/created json_value tree. */
void json_free(json_value_t *v);

/* Lookup helpers (return NULL if not found / wrong type) */
json_value_t *json_object_get(const json_value_t *obj, const char *key);
double json_get_number(const json_value_t *v, double fallback);
const char *json_get_string(const json_value_t *v);

/* Builders */
json_value_t *json_new_object(void);
json_value_t *json_new_array(void);
json_value_t *json_new_string(const char *s);
json_value_t *json_new_number(double n);
json_value_t *json_new_bool(int b);

void json_object_set(json_value_t *obj, const char *key, json_value_t *val);
void json_array_add(json_value_t *arr, json_value_t *val);

/* Serialize to a newly malloc'd string. Caller frees. */
char *json_serialize(const json_value_t *v);

#endif
