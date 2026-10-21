#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Hardware-independent ESP32 half of mcx/source/protocol.h.
// Stop-and-wait keeps retries ordered; the MCX acknowledges duplicate frames
// without applying their one-shot effects twice.
class McxLink {
 public:
  static constexpr unsigned MAX_LINE = 64;
  static constexpr uint32_t ACK_TIMEOUT_MS = 200;
  static constexpr uint32_t HEARTBEAT_MS = 400;
  static constexpr unsigned MAX_RETRIES = 3;

  bool setState(unsigned lives, unsigned long score, const char *mode) {
    if (lives > 99 || score > 9999999 ||
        (strcmp(mode, "IDLE") && strcmp(mode, "PLAY") && strcmp(mode, "OVER"))) return false;
    char next[40];
    snprintf(next, sizeof(next), "%u,%lu,%s", lives, score, mode);
    if (strcmp(next, state_)) stateDirty_ = true;
    strcpy(state_, next);
    return true;
  }

  bool queueEvent(const char *event) {
    if (strcmp(event, "HIT") && strcmp(event, "START") &&
        strcmp(event, "OVER") && strcmp(event, "BONUS")) return false;
    if (eventCount_ == EVENT_CAPACITY) { droppedEvents++; return false; }
    strcpy(events_[(eventHead_ + eventCount_) % EVENT_CAPACITY], event);
    ++eventCount_;
    return true;
  }

  // Call only when UART has room for MAX_LINE+1 bytes; never blocks sampling.
  const char *nextFrame(uint32_t now) {
    if (pending_) {
      if (!retryNow_ && uint32_t(now - sentAt_) < ACK_TIMEOUT_MS) return nullptr;
      if (retries_ == MAX_RETRIES) {
        ++failedFrames;
        pending_ = false;
      } else {
        ++retries_;
        retryNow_ = false;
        sentAt_ = now;
        return frame_;
      }
    }
    char body[MAX_LINE];
    if (state_[0] && (stateDirty_ || !sentState_ || uint32_t(now - stateAt_) >= HEARTBEAT_MS)) {
      snprintf(body, sizeof(body), "GS,%u,%s", nextSeq_, state_);
      stateDirty_ = false;
      sentState_ = true;
      stateAt_ = now;
    } else if (eventCount_) {
      snprintf(body, sizeof(body), "EV,%u,%s", nextSeq_, events_[eventHead_]);
      eventHead_ = (eventHead_ + 1) % EVENT_CAPACITY;
      --eventCount_;
    } else return nullptr;
    pendingSeq_ = nextSeq_++;
    buildFrame(frame_, sizeof(frame_), body);
    pending_ = true;
    retryNow_ = false;
    retries_ = 0;
    sentAt_ = now;
    return frame_;
  }

  // Drop old cloud commands when its state expires. Preserve sequence numbers.
  void suspend() {
    pending_ = false;
    eventCount_ = eventHead_ = 0;
    state_[0] = '\0';
    stateDirty_ = false;
    sentState_ = false;
  }

  bool receive(const char *line, uint32_t now) {
    const size_t n = strlen(line);
    if (n < 5 || n > MAX_LINE || line[0] != '$' || line[n - 3] != '*') return false;
    const int hi = hex(line[n - 2]), lo = hex(line[n - 1]);
    if (hi < 0 || lo < 0 || checksum(line + 1, n - 4) != ((hi << 4) | lo)) return false;
    char body[MAX_LINE + 1];
    memcpy(body, line + 1, n - 4);
    body[n - 4] = '\0';
    char *fields[4] = {body};
    unsigned count = 1;
    for (char *p = body; *p; ++p) {
      if (*p == ',') {
        if (count == 4) return false;
        *p = '\0';
        fields[count++] = p + 1;
      }
    }
    unsigned long seq;
    if (count < 2 || !number(fields[1], 65535, seq)) return false;
    if (count == 2 && (!strcmp(fields[0], "ACK") || !strcmp(fields[0], "NAK"))) {
      if (pending_ && seq == pendingSeq_) {
        if (!strcmp(fields[0], "ACK")) pending_ = false;
        else retryNow_ = true;
      }
      return true;
    }
    unsigned long raw, dark;
    if (count != 4 || strcmp(fields[0], "LDR") ||
        !number(fields[2], 4095, raw) || !number(fields[3], 1, dark)) return false;
    ldrRaw = static_cast<unsigned>(raw);
    ldrDark = dark != 0;
    ldrAt = now;
    hasLdr = true;
    return true;
  }

  static bool buildFrame(char *out, size_t capacity, const char *body) {
    const int n = snprintf(out, capacity, "$%s*%02X\n", body,
                           static_cast<unsigned>(checksum(body, strlen(body))));
    return n > 0 && static_cast<size_t>(n) < capacity;
  }

  unsigned ldrRaw = 0;
  bool ldrDark = false, hasLdr = false;
  uint32_t ldrAt = 0, failedFrames = 0, droppedEvents = 0;

 private:
  static uint8_t checksum(const char *s, size_t n) {
    uint8_t result = 0;
    for (size_t i = 0; i < n; ++i) result ^= static_cast<uint8_t>(s[i]);
    return result;
  }
  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }
  static bool number(const char *s, unsigned long max, unsigned long &out) {
    if (!*s) return false;
    out = 0;
    for (; *s; ++s) {
      if (*s < '0' || *s > '9') return false;
      const unsigned digit = *s - '0';
      if (out > max / 10 || (out == max / 10 && digit > max % 10)) return false;
      out = out * 10 + digit;
    }
    return true;
  }
  static constexpr unsigned EVENT_CAPACITY = 16;
  char events_[EVENT_CAPACITY][6] = {};
  unsigned eventHead_ = 0, eventCount_ = 0;
  char state_[40] = {}, frame_[MAX_LINE + 2] = {};
  uint16_t nextSeq_ = 0, pendingSeq_ = 0;
  uint32_t stateAt_ = 0, sentAt_ = 0;
  unsigned retries_ = 0;
  bool pending_ = false, retryNow_ = false, stateDirty_ = false, sentState_ = false;
};
