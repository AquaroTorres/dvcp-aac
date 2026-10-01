#include "aac_config.h"
#include <cassert>
#include <cstdio>
#include <vector>
struct Header {
    std::vector<uint8_t> data;unsigned count=0;
    void put(uint32_t value,unsigned n) {
        for(unsigned i=n;i;i--) {if(count%8==0)data.push_back(0);data.back()|=((value>>(i-1))&1)<<(7-count%8);count++;}
    }
    bool parse(AACBridge::AudioConfig& config) {return AACBridge::parseAudioConfig(data.data(),data.size(),config);}
};
int main() {
    AACBridge::AudioConfig c;
    const uint8_t lc48[]={0x11,0x90},lc44[]={0x12,0x10},bad[]={0xff,0xff};
    assert(AACBridge::parseAudioConfig(lc48,2,c)&&c.rate==48000&&c.channels==2&&c.frameSamples==1024);
    assert(AACBridge::parseAudioConfig(lc44,2,c)&&c.rate==44100&&c.channels==2);
    assert(!AACBridge::parseAudioConfig(nullptr,2,c));
    assert(!AACBridge::parseAudioConfig(lc48,1,c));
    assert(!AACBridge::parseAudioConfig(lc48,1024*1024+1,c));
    assert(!AACBridge::parseAudioConfig(bad,2,c));
    for(unsigned channels=0;channels<=15;channels++) {
        Header h;h.put(2,5);h.put(3,4);h.put(channels,4);h.put(0,3);
        bool ok=h.parse(c);assert(ok==(channels>=1&&channels<=7));
        if(ok)assert(c.channels==(channels==7?8:channels));
    }
    for(unsigned index=13;index<=14;index++) {
        Header h;h.put(2,5);h.put(index,4);h.put(2,4);h.put(0,3);assert(!h.parse(c));
    }
    Header explicitRate;explicitRate.put(2,5);explicitRate.put(15,4);explicitRate.put(48000,24);explicitRate.put(1,4);explicitRate.put(1,1);
    assert(explicitRate.parse(c)&&c.rate==48000&&c.channels==1&&c.frameSamples==960);
    for(size_t n=0;n<explicitRate.data.size();n++)assert(!AACBridge::parseAudioConfig(explicitRate.data.data(),n,c));
    for(unsigned aot:{5u,29u}) {
        Header h;h.put(aot,5);h.put(6,4);h.put(1,4);h.put(3,4);h.put(2,5);h.put(0,3);
        assert(h.parse(c)&&c.rate==48000&&c.channels==(aot==29?2u:1u)&&c.frameSamples==2048);
    }
    Header invalidRatio;invalidRatio.put(5,5);invalidRatio.put(6,4);invalidRatio.put(2,4);invalidRatio.put(4,4);invalidRatio.put(2,5);invalidRatio.put(0,3);
    assert(!invalidRatio.parse(c));
    puts("PASS: AAC metadata, explicit rates, SBR/PS headers, malformed and truncated configurations");
}
