#include "../include/IntervalActor.h"


IntervalActor::IntervalActor(uint32_t interval, uint32_t offset, std::function<void()> callback) : interval(interval), offset(offset), lastRun(millis() - interval + (offset % interval)), callback(callback) {}
// the freaking modulo and subtracting the interval first is what we were missing and basically the sign being flipped and us sending left hand of >= to negative was what was cooking us
// what was making the horrible weird behavior: they were all going immediately because of wraparound
// vicious little bug

void IntervalActor::act() {
  uint32_t currTime = millis();

  if (currTime - lastRun >= interval) {
    lastRun += interval; // clever tech for preventing drift
    callback();
  } else {
    // do nothing
  }
}

void IntervalActor::act(boolean force) {
  if (force) {
    callback();
  }
}

void IntervalActor::reset(uint32_t syncedTime) { lastRun = syncedTime - interval + (offset % interval); }

void IntervalActor::reset() { lastRun = millis() - interval + (offset % interval); }

void IntervalActor::setInterval(uint32_t newInterval) {
    interval = newInterval;
    lastRun = millis();
}