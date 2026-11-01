#pragma once
#include "config.h"
#include "../shared/FirebaseWorker.h"

// Keep the standalone test's existing begin() interface and private config.
namespace FirebaseWorker {
static bool begin() {
  return begin(Config{FIREBASE_API_KEY, FIREBASE_EMAIL, FIREBASE_PASSWORD,
                      DATABASE_URL, DEVICE_UID});
}
}
