#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------- allocation helpers ---------- */

static json_value_t *json_alloc(json_type_t t) {
    json_value_t *v = calloc(1, sizeof(json_value_t));
    v->type = t;
    return v;
}

json_value_t *json_new_object(void) { return json_alloc(JSON_OBJECT); }
json_value_t *json_new_array(void)  { return json_alloc(JSON_ARRAY); }

json_value_t *json_new_string(const char *s) {
    json_value_t *v = json_alloc(JSON_STRING);
    v->u.string = strdup(s ? s : "");
    return v;
}

json_value_t *json_new_number(double n) {
    json_value_t *v = json_alloc(JSON_NUMBER);
    v->u.number = n;
    return v;
}

json_value_t *json_new_bool(int b) {
    json_value_t *v = json_alloc(JSON_BOOL);
    v->u.boolean = b ? 1 : 0;
    return v;
}

void json_free(json_value_t *v) {
    if (!v) return;
    json_value_t *c = v->child;
    while (c) {
        json_value_t *n = c->next;
        json_free(c);
        c = n;
    }
    free(v->key);
    if (v->type == JSON_STRING) free(v->u.string);
    free(v);
}

void json_object_set(json_value_t *obj, const char *key, json_value_t *val) {
    val->key = strdup(key);
    if (!obj->child) {
        obj->child = val;
    } else {
        json_value_t *c = obj->child;
        while (c->next) c = c->next;
        c->next = val;
    }
}

void json_array_add(json_value_t *arr, json_value_t *val) {
    if (!arr->child) {
        arr->child = val;
    } else {
        json_value_t *c = arr->child;
        while (c->next) c = c->next;
        c->next = val;
    }
}

json_value_t *json_object_get(const json_value_t *obj, const char *key) {
    if (!obj) return NULL;
    for (json_value_t *c = obj->child; c; c = c->next) {
        if (c->key && strcmp(c->key, key) == 0) return c;
    }
    return NULL;
}

double json_get_number(const json_value_t *v, double fallback) {
    if (!v || v->type != JSON_NUMBER) return fallback;
    return v->u.number;
}

const char *json_get_string(const json_value_t *v) {
    if (!v || v->type != JSON_STRING) return NULL;
    return v->u.string;
}

/* ---------- parsing ---------- */

typedef struct {
    const char *s;
    size_t pos;
    size_t len;
} parser_t;

static void skip_ws(parser_t *p) {
    while (p->pos < p->len && isspace((unsigned char)p->s[p->pos])) p->pos++;
}

static json_value_t *parse_value(parser_t *p);

static char *parse_raw_string(parser_t *p) {
    /* assumes current char is '"' */
    p->pos++; /* skip opening quote */
    size_t start = p->pos;
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    while (p->pos < p->len && p->s[p->pos] != '"') {
        char c = p->s[p->pos];
        if (c == '\\' && p->pos + 1 < p->len) {
            p->pos++;
            char esc = p->s[p->pos];
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                default: c = esc; break;
            }
        }
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = c;
        p->pos++;
    }
    buf[len] = '\0';
    if (p->pos < p->len) p->pos++; /* skip closing quote */
    (void)start;
    return buf;
}

static json_value_t *parse_object(parser_t *p) {
    json_value_t *obj = json_new_object();
    p->pos++; /* skip { */
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return obj; }
    while (p->pos < p->len) {
        skip_ws(p);
        if (p->s[p->pos] != '"') break;
        char *key = parse_raw_string(p);
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ':') p->pos++;
        skip_ws(p);
        json_value_t *val = parse_value(p);
        if (val) {
            val->key = key;
            if (!obj->child) obj->child = val;
            else {
                json_value_t *c = obj->child;
                while (c->next) c = c->next;
                c->next = val;
            }
        } else {
            free(key);
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; break; }
        break;
    }
    return obj;
}

static json_value_t *parse_array(parser_t *p) {
    json_value_t *arr = json_new_array();
    p->pos++; /* skip [ */
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return arr; }
    while (p->pos < p->len) {
        skip_ws(p);
        json_value_t *val = parse_value(p);
        if (val) json_array_add(arr, val);
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; break; }
        break;
    }
    return arr;
}

static json_value_t *parse_value(parser_t *p) {
    skip_ws(p);
    if (p->pos >= p->len) return NULL;
    char c = p->s[p->pos];
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '"') {
        char *s = parse_raw_string(p);
        json_value_t *v = json_alloc(JSON_STRING);
        v->u.string = s;
        return v;
    }
    if (c == 't' && strncmp(p->s + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return json_new_bool(1);
    }
    if (c == 'f' && strncmp(p->s + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return json_new_bool(0);
    }
    if (c == 'n' && strncmp(p->s + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return json_alloc(JSON_NULL);
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *end;
        double n = strtod(p->s + p->pos, &end);
        p->pos = end - p->s;
        return json_new_number(n);
    }
    return NULL;
}

json_value_t *json_parse(const char *text) {
    if (!text) return NULL;
    parser_t p = { text, 0, strlen(text) };
    return parse_value(&p);
}

/* ---------- serialization ---------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} sbuf_t;

static void sbuf_ensure(sbuf_t *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) b->cap *= 2;
        b->buf = realloc(b->buf, b->cap);
    }
}

static void sbuf_append(sbuf_t *b, const char *s) {
    size_t l = strlen(s);
    sbuf_ensure(b, l);
    memcpy(b->buf + b->len, s, l);
    b->len += l;
    b->buf[b->len] = '\0';
}

static void sbuf_append_escaped(sbuf_t *b, const char *s) {
    sbuf_append(b, "\"");
    for (const char *c = s; *c; c++) {
        char tmp[8];
        if (*c == '"' || *c == '\\') {
            tmp[0] = '\\'; tmp[1] = *c; tmp[2] = '\0';
            sbuf_append(b, tmp);
        } else if (*c == '\n') {
            sbuf_append(b, "\\n");
        } else {
            tmp[0] = *c; tmp[1] = '\0';
            sbuf_append(b, tmp);
        }
    }
    sbuf_append(b, "\"");
}

static void serialize_value(const json_value_t *v, sbuf_t *b) {
    if (!v) { sbuf_append(b, "null"); return; }
    switch (v->type) {
        case JSON_NULL:
            sbuf_append(b, "null");
            break;
        case JSON_BOOL:
            sbuf_append(b, v->u.boolean ? "true" : "false");
            break;
        case JSON_NUMBER: {
            char tmp[64];
            if (v->u.number == (long long)v->u.number)
                snprintf(tmp, sizeof(tmp), "%lld", (long long)v->u.number);
            else
                snprintf(tmp, sizeof(tmp), "%.6f", v->u.number);
            sbuf_append(b, tmp);
            break;
        }
        case JSON_STRING:
            sbuf_append_escaped(b, v->u.string);
            break;
        case JSON_ARRAY: {
            sbuf_append(b, "[");
            json_value_t *c = v->child;
            int first = 1;
            while (c) {
                if (!first) sbuf_append(b, ",");
                serialize_value(c, b);
                first = 0;
                c = c->next;
            }
            sbuf_append(b, "]");
            break;
        }
        case JSON_OBJECT: {
            sbuf_append(b, "{");
            json_value_t *c = v->child;
            int first = 1;
            while (c) {
                if (!first) sbuf_append(b, ",");
                sbuf_append_escaped(b, c->key ? c->key : "");
                sbuf_append(b, ":");
                serialize_value(c, b);
                first = 0;
                c = c->next;
            }
            sbuf_append(b, "}");
            break;
        }
    }
}

char *json_serialize(const json_value_t *v) {
    sbuf_t b;
    b.cap = 256;
    b.len = 0;
    b.buf = malloc(b.cap);
    b.buf[0] = '\0';
    serialize_value(v, &b);
    return b.buf;
}
