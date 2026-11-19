#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
#include "ported/features/pixel_screen/core.h"

namespace bridge::board::ring {
using Frame = std::array<uint32_t,24>;
constexpr unsigned clockHour(unsigned hour,unsigned minute) { return (hour%12*2+minute/30)%24; }
constexpr unsigned clockMinute(unsigned minute) { return minute%60*24/60; }
constexpr unsigned clockSecond(unsigned second) { return second%60*24/60; }
constexpr uint32_t rainbow(unsigned hue) {
    hue%=1536;
    const unsigned rising=(hue%256)*32/256,falling=32-rising;
    switch(hue/256) {
    case 0:return (32u<<16)|(rising<<8);
    case 1:return (falling<<16)|(32u<<8);
    case 2:return (32u<<8)|rising;
    case 3:return (falling<<8)|32u;
    case 4:return (rising<<16)|32u;
    default:return (32u<<16)|falling;
    }
}
constexpr pixel_screen::State visibleState(const pixel_screen::Snapshot &s) {
    return s.state==pixel_screen::State::Complete && s.stateAgeMs>=10000
        ?pixel_screen::State::Idle:s.state;
}
constexpr Frame render(const pixel_screen::Snapshot &s,unsigned hour,unsigned minute,
                    unsigned second,bool timeValid,uint32_t now) {
    Frame out{};
    using pixel_screen::State;
    const auto state=visibleState(s);
    if(state==State::Printing||state==State::Paused) {
        const unsigned filled=std::min(100u,unsigned(s.percent))*24/100;
        const uint32_t color=state==State::Paused?0x201000:0x002000;
        for(auto &pixel:out)pixel=0x020202;
        for(unsigned i=0;i<filled;++i)out[i]=color;
        if(filled<24 && now/400%2)out[filled]=color;
    } else if(state==State::Preparing) {
        for(auto &pixel:out)pixel=0x020100;
        out[now/120%24]=0x201000;
    } else if(state==State::Error) {
        for(auto &pixel:out)pixel=now/400%2?0x200000:0x020000;
    } else if(state==State::Cancelled) {
        for(auto &pixel:out)pixel=0x100000;
    } else if(state==State::Complete) {
        const unsigned phase=s.stateAgeMs/40*32%1536;
        for(unsigned i=0;i<24;++i)out[i]=rainbow((i*64+1536-phase)%1536);
    } else if(timeValid) {
        // Additive channel mixing preserves coincident hands.
        out[clockHour(hour,minute)]|=0x200000;
        out[clockMinute(minute)]|=0x002000;
        out[clockSecond(second)]|=0x000020;
    } else {
        out[now/180%24]=0x100800;
    }
    return out;
}
static_assert(clockHour(0,0)==0 && clockHour(12,0)==0 && clockHour(11,59)==23);
static_assert(clockMinute(59)==23 && clockSecond(59)==23);
} // namespace bridge::board::ring
