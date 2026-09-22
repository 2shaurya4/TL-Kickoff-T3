#ifndef INTERVALACTOR_H
#define INTERVALACTOR_H

#include <functional>
#include "Particle.h"

class IntervalActor {
   public:
    uint32_t interval;
    uint32_t offset;
    uint32_t lastRun;
    std::function<void()> callback;

    IntervalActor(uint32_t interval, uint32_t offset, std::function<void()> callback);
    void act();
    void act(boolean force);
    void reset(uint32_t syncedTime);
    void reset();
    void setInterval(uint32_t newInterval);
};

#endif
