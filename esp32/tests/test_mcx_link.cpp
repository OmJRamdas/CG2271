#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "McxLink.h"
extern "C" {
#include "protocol.h"
}

static void receive(McxLink &link, const char *body, uint32_t now = 0) {
  char frame[80];
  assert(McxLink::buildFrame(frame, sizeof(frame), body));
  frame[strlen(frame) - 1] = '\0';
  assert(link.receive(frame, now));
}
static proto_msg_t parse(const char *frame) {
  assert(frame);
  char line[80];
  strcpy(line, frame);
  line[strlen(line) - 1] = '\0';
  proto_msg_t message;
  int32_t seq;
  assert(proto_parse(line, &message, &seq) == PROTO_OK);
  return message;
}

int main() {
  McxLink link;
  assert(!link.setState(100, 0, "PLAY"));
  assert(!link.setState(3, 10000000, "PLAY"));
  assert(!link.setState(3, 0, "INVALID"));
  assert(link.setState(3, 10, "PLAY"));
  const char *frame = link.nextFrame(0);
  const proto_msg_t state = parse(frame);
  assert(state.type == MSG_GAME_STATE && state.u.gs.lives == 3 && state.u.gs.score == 10);
  char original[80];
  strcpy(original, frame);
  assert(!link.nextFrame(199));
  assert(!strcmp(link.nextFrame(200), original));  // Lost ACK: identical sequence/body.
  receive(link, "ACK,99");  // An unrelated ACK cannot release this frame.
  assert(!link.nextFrame(201));
  receive(link, "NAK,0");
  assert(!strcmp(link.nextFrame(201), original));
  receive(link, "ACK,0");
  assert(!link.nextFrame(202));
  assert(link.queueEvent("HIT"));
  assert(!link.queueEvent("ROLL"));
  proto_msg_t event = parse(link.nextFrame(202));
  assert(event.type == MSG_EVENT && event.u.ev == EV_HIT && event.seq == 1);

  // Exercise the MCX's actual retry history with a lost event ACK.
  proto_history_t history = {};
  char line[80];
  const char *retry = link.nextFrame(402);
  strcpy(line, retry);
  line[strlen(line) - 1] = '\0';
  assert(proto_remember(&history, line));
  assert(!proto_remember(&history, line));
  strcat(line, "\r");
  assert(!proto_remember(&history, line));
  receive(link, "ACK,1");
  assert(parse(link.nextFrame(403)).type == MSG_GAME_STATE);  // Heartbeat.
  receive(link, "ACK,2");
  receive(link, "ENC,10,65", 500);
  assert(link.hasEnc && link.encLevel == 65 && link.encAt == 500);
  assert(!link.receive("$ENC,10,65*00", 501));  // Bad checksum.
  char bad[80];
  McxLink::buildFrame(bad, sizeof(bad), "ENC,1,999999999999999999999");
  bad[strlen(bad) - 1] = '\0';
  assert(!link.receive(bad, 502));
  McxLink::buildFrame(bad, sizeof(bad), "ENC,1,101");  // Out of range.
  bad[strlen(bad) - 1] = '\0';
  assert(!link.receive(bad, 502));
  McxLink::buildFrame(bad, sizeof(bad), "ENC,1,50,1");  // Old/extra field.
  bad[strlen(bad) - 1] = '\0';
  assert(!link.receive(bad, 502));
  McxLink::buildFrame(bad, sizeof(bad), "LDR,1,1100,1");  // LDR no longer accepted.
  bad[strlen(bad) - 1] = '\0';
  assert(!link.receive(bad, 502));
  assert(link.encLevel == 65 && link.encAt == 500);

  // The real MCX builder's $ENC frame is accepted.
  char mcxFrame[32];
  assert(proto_build(mcxFrame, sizeof(mcxFrame), "ENC,%u,%u", 11U, 0U) > 0);
  mcxFrame[strlen(mcxFrame) - 1] = '\0';
  assert(link.receive(mcxFrame, 600) && link.encLevel == 0 && link.encAt == 600);
  for (unsigned i = 0; i < 16; ++i) assert(link.queueEvent("BONUS"));
  assert(!link.queueEvent("HIT") && link.droppedEvents == 1);
  link.suspend();
  assert(!link.nextFrame(1000));  // No stale events after cloud disconnect.
  assert(link.setState(0, 50, "OVER"));
  assert(parse(link.nextFrame(1001)).u.gs.mode == MODE_OVER);

  McxLink dead;
  dead.setState(3, 0, "IDLE");
  dead.nextFrame(0);
  dead.nextFrame(200);
  dead.nextFrame(400);
  dead.nextFrame(600);
  dead.nextFrame(800);
  assert(dead.failedFrames == 1);  // Bounded retries, then a new heartbeat.

  // Sender wraps sequence 65535 -> 0, and the real MCX parser accepts both.
  McxLink wrap;
  wrap.setState(3, 0, "IDLE");
  for (unsigned i = 0; i <= 65536; ++i) {
    auto msg = parse(wrap.nextFrame(i * 400U));
    assert(msg.seq == static_cast<uint16_t>(i));
    char ack[32];
    snprintf(ack, sizeof(ack), "ACK,%u", msg.seq);
    receive(wrap, ack);
  }
  puts("ESP32/MCX interoperability, retry, duplicate, ENC and sequence wrap tests passed");
}
