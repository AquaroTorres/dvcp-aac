#pragma once
#include <cstddef>
#include <cstdint>

namespace AACBridge {
struct AudioConfig {uint32_t rate=0,channels=0,frameSamples=0;};
// Bounded AudioSpecificConfig header reader. Metadata only; FDK validates the
// full configuration when the decoder opens. Unsupported headers are refused.
inline bool parseAudioConfig(const uint8_t* data,size_t bytes,AudioConfig& output) {
    if(!data || bytes<2 || bytes>1024*1024)return false;
    size_t bit=0;bool valid=true;
    auto bits=[&](unsigned count)->uint32_t {
        if(count>32 || bit>bytes*8 || count>bytes*8-bit){valid=false;return 0;}
        uint32_t value=0;
        for(unsigned i=0;i<count;i++,bit++)value=(value<<1)|((data[bit/8]>>(7-bit%8))&1);
        return value;
    };
    auto object=[&]()->uint32_t {uint32_t n=bits(5);return n==31?32+bits(6):n;};
    auto frequency=[&]()->uint32_t {
        static const uint32_t table[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
        uint32_t n=bits(4);if(n==15)return bits(24);
        if(n>=13){valid=false;return 0;}return table[n];
    };
    uint32_t aot=object(),rate=frequency(),channels=bits(4),scale=1;
    bool ps=aot==29;
    if(aot==5 || ps) {
        uint32_t extension=frequency();aot=object();
        if(!rate || (extension!=rate && extension!=rate*2))return false;
        scale=extension/rate;rate=extension;
    }
    if(!valid || (aot!=1 && aot!=2 && aot!=3 && aot!=4) || channels<1 || channels>7 || rate<7350 || rate>192000)return false;
    uint32_t frame=bits(1)?960:1024;
    if(!valid)return false;
    AudioConfig result;result.rate=rate;result.channels=channels==7?8:channels;
    if(ps && result.channels==1)result.channels=2;
    result.frameSamples=frame*scale;output=result;return true;
}
}
