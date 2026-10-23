/*
 * Host unit tests for protocol.c — no hardware needed.
 *
 *   gcc -std=c11 -Wall -Wextra -I../source test_protocol.c ../source/protocol.c -o test_protocol
 *   ./test_protocol
 */

#include <stdio.h>
#include <string.h>

#include "protocol.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* Wrap a body in "$...*HH" with a correct checksum (no '\n'). */
static const char *frame(const char *body)
{
    static char buf[128];
    int n = proto_build(buf, sizeof buf, "%s", body);
    if (n > 0) buf[n - 1] = '\0';                   /* drop '\n' like the ISR does */
    return buf;
}

static void test_checksum(void)
{
    /* Known NMEA-style values computed by hand. */
    CHECK(proto_checksum("", 0) == 0x00);
    CHECK(proto_checksum("A", 1) == 0x41);
    CHECK(proto_checksum("AB", 2) == (0x41 ^ 0x42));
    CHECK(proto_checksum("ACK,12", 6) == 0x66);
}

static void test_build(void)
{
    char buf[32];

    CHECK(proto_build(buf, sizeof buf, "ACK,%u", 12U) == 11);
    CHECK(strcmp(buf, "$ACK,12*66\n") == 0);

    CHECK(proto_build(buf, sizeof buf, "ENC,%u,%u", 65535U, 100U) > 0);
    CHECK(strncmp(buf, "$ENC,65535,100*", 15) == 0);
    CHECK(buf[0] == '$' && buf[strlen(buf) - 1] == '\n');

    /* Exact fit: "$ACK,12*66\n" is 11 chars + NUL = 12 bytes. */
    CHECK(proto_build(buf, 12, "ACK,%u", 12U) == 11);
    CHECK(proto_build(buf, 11, "ACK,%u", 12U) == -1);
    CHECK(proto_build(buf, 4, "A") == -1);
}

static void test_parse_game_state(void)
{
    proto_msg_t m;
    int32_t seq;

    CHECK(proto_parse(frame("GS,5,3,1200,PLAY"), &m, &seq) == PROTO_OK);
    CHECK(seq == 5);
    CHECK(m.type == MSG_GAME_STATE);
    CHECK(m.seq == 5);
    CHECK(m.u.gs.lives == 3);
    CHECK(m.u.gs.score == 1200);
    CHECK(m.u.gs.mode == MODE_PLAY);

    CHECK(proto_parse(frame("GS,0,0,0,IDLE"), &m, &seq) == PROTO_OK);
    CHECK(m.u.gs.mode == MODE_IDLE);
    CHECK(proto_parse(frame("GS,65535,99,9999999,OVER"), &m, &seq) == PROTO_OK);
    CHECK(m.seq == 65535 && m.u.gs.mode == MODE_OVER);

    /* Trailing '\r' tolerated. */
    char crlf[64];
    snprintf(crlf, sizeof crlf, "%s\r", frame("GS,9,2,10,PLAY"));
    CHECK(proto_parse(crlf, &m, &seq) == PROTO_OK);

    /* Lower-case hex checksum accepted. */
    char lower[64];
    strcpy(lower, frame("GS,1,1,1,PLAY"));
    for (char *p = strchr(lower, '*'); *p; p++)
        if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
    CHECK(proto_parse(lower, &m, &seq) == PROTO_OK);
}

static void test_parse_event(void)
{
    proto_msg_t m;
    int32_t seq;

    CHECK(proto_parse(frame("EV,8,HIT"), &m, &seq) == PROTO_OK);
    CHECK(m.type == MSG_EVENT && m.u.ev == EV_HIT && m.seq == 8);
    CHECK(proto_parse(frame("EV,9,START"), &m, &seq) == PROTO_OK && m.u.ev == EV_START);
    CHECK(proto_parse(frame("EV,10,OVER"), &m, &seq) == PROTO_OK && m.u.ev == EV_OVER);
    CHECK(proto_parse(frame("EV,11,BONUS"), &m, &seq) == PROTO_OK && m.u.ev == EV_BONUS);
}

static void test_bad_checksum(void)
{
    proto_msg_t m;
    int32_t seq;
    char bad[64];

    strcpy(bad, frame("EV,42,HIT"));
    char *star = strchr(bad, '*');
    star[2] = (star[2] == '0') ? '1' : '0';         /* flip the low nibble */
    CHECK(proto_parse(bad, &m, &seq) == PROTO_ERR_CHECKSUM);
    CHECK(seq == 42);                                /* still NAK-able */

    /* Corrupted payload byte, original checksum. */
    strcpy(bad, frame("GS,7,3,100,PLAY"));
    bad[6] = '4';                                    /* lives 3 -> 4 */
    CHECK(proto_parse(bad, &m, &seq) == PROTO_ERR_CHECKSUM);
    CHECK(seq == 7);
}

static void test_bad_format(void)
{
    proto_msg_t m;
    int32_t seq;

    CHECK(proto_parse("", &m, &seq) == PROTO_ERR_FORMAT);
    CHECK(seq == -1);
    CHECK(proto_parse("GS,1,3,0,PLAY*00", &m, &seq) == PROTO_ERR_FORMAT);   /* no '$' */
    CHECK(proto_parse("$GS,1,3,0,PLAY", &m, &seq) == PROTO_ERR_FORMAT);     /* no '*' */
    CHECK(proto_parse("$GS,1,3,0,PLAY*G1", &m, &seq) == PROTO_ERR_FORMAT);  /* bad hex */
    CHECK(proto_parse("$GS,1,3,0,PLAY*1", &m, &seq) == PROTO_ERR_FORMAT);   /* 1 hex digit */
    CHECK(proto_parse("$*00", &m, &seq) == PROTO_ERR_FORMAT);               /* empty body */
    CHECK(proto_parse("$GS*1*00", &m, &seq) == PROTO_ERR_FORMAT);           /* two '*' */
    CHECK(proto_parse(frame("GS"), &m, &seq) == PROTO_ERR_FORMAT);          /* no seq */
    CHECK(proto_parse(frame("GS,x,3,0,PLAY"), &m, &seq) == PROTO_ERR_FORMAT);
    CHECK(seq == -1);
    CHECK(proto_parse(frame("GS,1,3,0"), &m, &seq) == PROTO_ERR_FORMAT);    /* too few */
    CHECK(seq == 1);
    CHECK(proto_parse(frame("GS,1,3,0,PLAY,9"), &m, &seq) == PROTO_ERR_FORMAT); /* too many */
    CHECK(proto_parse(frame("EV,1"), &m, &seq) == PROTO_ERR_FORMAT);
    CHECK(proto_parse(frame("EV,1,HIT,2"), &m, &seq) == PROTO_ERR_FORMAT);
    CHECK(proto_parse(frame("A,1,2,3,4,5,6,7"), &m, &seq) == PROTO_ERR_FORMAT); /* > MAX_FIELDS */

    /* Overlong line rejected without overflowing the parser's buffer. */
    char longLine[200];
    memset(longLine, 'A', sizeof longLine - 1);
    longLine[0] = '$';
    longLine[sizeof longLine - 4] = '*';
    longLine[sizeof longLine - 1] = '\0';
    CHECK(proto_parse(longLine, &m, &seq) == PROTO_ERR_FORMAT);
}

static void test_bad_type_and_range(void)
{
    proto_msg_t m;
    int32_t seq;

    CHECK(proto_parse(frame("XY,3,1"), &m, &seq) == PROTO_ERR_TYPE);
    CHECK(seq == 3);
    CHECK(proto_parse(frame("ACK,3"), &m, &seq) == PROTO_ERR_TYPE);   /* MCX never receives ACK */

    CHECK(proto_parse(frame("GS,1,-1,0,PLAY"), &m, &seq) == PROTO_ERR_RANGE);
    CHECK(proto_parse(frame("GS,1,100,0,PLAY"), &m, &seq) == PROTO_ERR_RANGE);
    CHECK(proto_parse(frame("GS,1,3,-5,PLAY"), &m, &seq) == PROTO_ERR_RANGE);
    CHECK(proto_parse(frame("GS,1,3,10000000,PLAY"), &m, &seq) == PROTO_ERR_RANGE);
    CHECK(proto_parse(frame("GS,1,3,0,play"), &m, &seq) == PROTO_ERR_RANGE);
    CHECK(proto_parse(frame("GS,1,3,0,"), &m, &seq) == PROTO_ERR_RANGE);
    CHECK(proto_parse(frame("GS,1,3,12a,PLAY"), &m, &seq) == PROTO_ERR_RANGE);
    CHECK(proto_parse(frame("GS,1,3,99999999999999999999,PLAY"), &m, &seq) == PROTO_ERR_RANGE);
    CHECK(proto_parse(frame("EV,1,JUMP"), &m, &seq) == PROTO_ERR_RANGE);
    CHECK(proto_parse(frame("GS,65536,3,0,PLAY"), &m, &seq) == PROTO_ERR_FORMAT); /* seq range */
    CHECK(proto_parse(frame("GS,-1,3,0,PLAY"), &m, &seq) == PROTO_ERR_FORMAT);
}

int main(void)
{
    test_checksum();
    test_build();
    test_parse_game_state();
    test_parse_event();
    test_bad_checksum();
    test_bad_format();
    test_bad_type_and_range();

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
