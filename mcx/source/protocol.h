/*
 * CG2271 A4 — ESP32 <-> MCXC444 UART frame protocol
 *
 * Frame format (ASCII, one per line):
 *     $<TYPE>,<seq>,<fields...>*<CK>\n
 * CK = two upper-case hex digits, XOR of every byte between '$' and '*'.
 *
 * ESP32 -> MCXC444
 *     $GS,<seq>,<lives>,<score>,<IDLE|PLAY|OVER>*CK    game state (>= every 500 ms)
 *     $EV,<seq>,<HIT|START|OVER|BONUS>*CK             one-shot game event
 *
 * MCXC444 -> ESP32
 *     $ACK,<seq>*CK          valid frame <seq> received
 *     $NAK,<seq>*CK          invalid frame (bad checksum / format), seq 0 if unreadable
 *     $LDR,<seq>,<raw>,<dark>*CK   light sensor: raw 0..4095, dark 0/1
 *
 * Pure C, no hardware or RTOS dependencies, so it can be unit-tested on a PC.
 */

#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#define PROTO_MAX_LINE      64U     /* longest frame, excluding '\n'   */
#define PROTO_MAX_SEQ       65535L
#define PROTO_MAX_LIVES     99L
#define PROTO_MAX_SCORE     9999999L

typedef enum {
    PROTO_OK = 0,
    PROTO_ERR_FORMAT,       /* framing / field count / non-numeric      */
    PROTO_ERR_CHECKSUM,     /* framing fine but checksum mismatch       */
    PROTO_ERR_TYPE,         /* unknown frame type                       */
    PROTO_ERR_RANGE         /* numeric field or keyword out of range    */
} proto_status_t;

typedef enum { MSG_GAME_STATE, MSG_EVENT } proto_msg_type_t;

typedef enum { MODE_IDLE = 0, MODE_PLAY, MODE_OVER } game_mode_t;

typedef enum { EV_HIT = 0, EV_START, EV_OVER, EV_BONUS } game_event_t;

typedef struct {
    proto_msg_type_t type;
    uint16_t         seq;
    union {
        struct {
            int32_t     lives;
            int32_t     score;
            game_mode_t mode;
        } gs;
        game_event_t ev;
    } u;
} proto_msg_t;

/* XOR checksum over len bytes. */
uint8_t proto_checksum(const char *s, size_t len);

/*
 * Parse one received line (without the trailing '\n'; a trailing '\r' is
 * tolerated). On return *seq_out holds the frame's sequence number if it
 * could be read (even when the frame is otherwise invalid), else -1, so the
 * caller can NAK the right frame.
 */
proto_status_t proto_parse(const char *line, proto_msg_t *out, int32_t *seq_out);

/*
 * Build a complete frame from a printf-style body, e.g.
 *     proto_build(buf, sizeof buf, "ACK,%u", seq)  ->  "$ACK,12*5B\n"
 * Returns the frame length, or -1 if it does not fit.
 */
int proto_build(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* PROTOCOL_H_ */
