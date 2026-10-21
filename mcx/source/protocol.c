/*
 * CG2271 A4 — ESP32 <-> MCXC444 UART frame protocol (see protocol.h)
 */

#include "protocol.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_FIELDS  6U

uint8_t proto_checksum(const char *s, size_t len)
{
    uint8_t ck = 0;
    for (size_t i = 0; i < len; i++) ck ^= (uint8_t)s[i];
    return ck;
}

bool proto_remember(proto_history_t *history, const char *line)
{
    size_t len = strlen(line);
    if (len && line[len - 1] == '\r') len--;
    if (len > PROTO_MAX_LINE) return false;
    if (strlen(history->last_line) == len && !memcmp(history->last_line, line, len))
        return false;
    memcpy(history->last_line, line, len);
    history->last_line[len] = '\0';
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Strict decimal parse: optional '-', digits only, within [min, max]. */
static bool parseInt(const char *s, long min, long max, int32_t *out)
{
    bool neg = false;
    long v = 0;

    if (*s == '-') { neg = true; s++; }
    if (*s == '\0') return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (*s - '0');
        if (v > 100000000L) return false;           /* stop overflow early */
        s++;
    }
    if (neg) v = -v;
    if (v < min || v > max) return false;
    *out = (int32_t)v;
    return true;
}

/* Index of keyword in table, or -1. */
static int lookup(const char *s, const char *const *table, int n)
{
    for (int i = 0; i < n; i++)
        if (strcmp(s, table[i]) == 0) return i;
    return -1;
}

static const char *const MODE_NAMES[]  = { "IDLE", "PLAY", "OVER" };
static const char *const EVENT_NAMES[] = { "HIT", "START", "OVER", "BONUS" };

proto_status_t proto_parse(const char *line, proto_msg_t *out, int32_t *seq_out)
{
    char body[PROTO_MAX_LINE];
    char *field[MAX_FIELDS];
    size_t nfields = 0;
    int32_t seq;

    *seq_out = -1;

    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\r') len--;
    if (len < 4 || len > PROTO_MAX_LINE || line[0] != '$') return PROTO_ERR_FORMAT;

    /* Expect "...*HH" at the very end. */
    if (line[len - 3] != '*') return PROTO_ERR_FORMAT;
    int hi = hexval(line[len - 2]);
    int lo = hexval(line[len - 1]);
    if (hi < 0 || lo < 0) return PROTO_ERR_FORMAT;

    size_t bodyLen = len - 4;                       /* strip '$' and "*HH" */
    if (bodyLen == 0 || memchr(line + 1, '*', bodyLen) != NULL) return PROTO_ERR_FORMAT;
    memcpy(body, line + 1, bodyLen);
    body[bodyLen] = '\0';

    /* Split on ',' in place. */
    char *p = body;
    field[nfields++] = p;
    while (*p) {
        if (*p == ',') {
            if (nfields == MAX_FIELDS) return PROTO_ERR_FORMAT;
            *p = '\0';
            field[nfields++] = p + 1;
        }
        p++;
    }

    /* Read seq before the checksum check so a corrupted frame can be NAKed. */
    if (nfields < 2 || !parseInt(field[1], 0, PROTO_MAX_SEQ, &seq)) return PROTO_ERR_FORMAT;
    *seq_out = seq;

    if (proto_checksum(line + 1, bodyLen) != (uint8_t)((hi << 4) | lo)) return PROTO_ERR_CHECKSUM;

    out->seq = (uint16_t)seq;

    if (strcmp(field[0], "GS") == 0) {
        if (nfields != 5) return PROTO_ERR_FORMAT;
        if (!parseInt(field[2], 0, PROTO_MAX_LIVES, &out->u.gs.lives)) return PROTO_ERR_RANGE;
        if (!parseInt(field[3], 0, PROTO_MAX_SCORE, &out->u.gs.score)) return PROTO_ERR_RANGE;
        int mode = lookup(field[4], MODE_NAMES, 3);
        if (mode < 0) return PROTO_ERR_RANGE;
        out->u.gs.mode = (game_mode_t)mode;
        out->type = MSG_GAME_STATE;
        return PROTO_OK;
    }

    if (strcmp(field[0], "EV") == 0) {
        if (nfields != 3) return PROTO_ERR_FORMAT;
        int ev = lookup(field[2], EVENT_NAMES, 4);
        if (ev < 0) return PROTO_ERR_RANGE;
        out->u.ev = (game_event_t)ev;
        out->type = MSG_EVENT;
        return PROTO_OK;
    }

    return PROTO_ERR_TYPE;
}

int proto_build(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;

    if (size < 6) return -1;                        /* "$" + "*HH\n" + NUL */
    buf[0] = '$';

    va_start(ap, fmt);
    int n = vsnprintf(buf + 1, size - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n + 6 > size) return -1;  /* '$' + body + "*HH\n" + NUL */

    uint8_t ck = proto_checksum(buf + 1, (size_t)n);
    static const char HEX[] = "0123456789ABCDEF";
    char *t = buf + 1 + n;
    t[0] = '*';
    t[1] = HEX[ck >> 4];
    t[2] = HEX[ck & 0x0F];
    t[3] = '\n';
    t[4] = '\0';
    return n + 5;
}
