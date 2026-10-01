// Resolve AAC decoder, with a guarded runtime bridge for MP4/MOV and MKV.
#include "wrapper/plugin_api.h"
#include "diagnostics.h"
#include <fdk-aac/aacdecoder_lib.h>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <unistd.h>
#ifdef AAC_RUNTIME_BRIDGE
#include "bridge.h"
#endif

static const uint8_t pluginUUID[16] = {0xb1,0x34,0x21,0x19,0xda,0x60,0x49,0x36,0x80,0x8f,0x12,0x47,0xb5,0xaa,0x0c,0x21};
static const uint8_t codecUUID[16] = {0xf3,0x31,0x68,0x39,0x4a,0x32,0x43,0x01,0xab,0x3d,0x87,0x10,0x22,0x5a,0xbc,0x90};
static void trace(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); aacLogV(fmt,ap); va_end(ap);
}
static void u32(IPropertyProvider* p, PropertyID key, uint32_t v) {
    p->SetProperty(key, propTypeUInt32, &v, 1);
}
static void u8(IPropertyProvider* p, PropertyID key, uint8_t v) {
    p->SetProperty(key, propTypeUInt8, &v, 1);
}
static void dump(IPropertyProvider* p, const char* phase) {
    trace("%s", phase);
    for (const char* key : {"4cc", "codecDirection", "mediaType", "samplingRate", "numChannels", "bitDepth", "sampleBits", "isFloat", "magicCookieType", "magicCookie", "pts", "duration", "timeBase", "contExt", "decoderDelay", "decoderPreroll"}) {
        PropertyType t; const void* v=nullptr; int n=0;
        if (p->GetProperty(key,&t,&v,&n)!=errNone || !v || n<=0) continue;
        size_t unit=t==propTypeUInt32||t==propTypeInt32?4:t==propTypeInt64||t==propTypeUInt64||t==propTypeDouble?8:1;
        char hex[257]; size_t count=std::min(size_t(n)*unit,size_t(128));
        for (size_t i=0;i<count;i++) snprintf(hex+2*i,3,"%02x",static_cast<const uint8_t*>(v)[i]);
        trace("  %s type=%d count=%d bytes=%s",key,int(t),n,hex);
    }
}
// Recursively unwrap ISO 14496 ES/DecoderConfig descriptors with bounds checks.
static bool ascIn(const uint8_t* p,size_t n,std::vector<uint8_t>& asc,int depth=0) {
    if (depth>4) return false;
    size_t at=0;
    while(at<n) {
        uint8_t tag=p[at++]; size_t len=0; bool ended=false;
        for(int i=0;i<4 && at<n;i++) {uint8_t b=p[at++];len=(len<<7)|(b&127);if(!(b&128)){ended=true;break;}}
        if(!ended || len>n-at) return false;
        size_t skip=0;
        if(tag==5 && len>0) {asc.assign(p+at,p+at+len);return true;}
        if(tag==3) {
            if(len<3) return false;
            uint8_t flags=p[at+2]; skip=3;
            if(flags&128) skip+=2;
            if(flags&64) {if(skip>=len)return false;skip+=1+p[at+skip];}
            if(flags&32) skip+=2;
        } else if(tag==4) skip=13;
        else {at+=len;continue;}
        if(skip<=len && ascIn(p+at+skip,len-skip,asc,depth+1)) return true;
        at+=len;
    }
    return false;
}

class AACDecoder : public IPluginCodecRef {
    HANDLE_AACDECODER dec=nullptr;
    uint32_t rate=0,channels=0;
    uint32_t decoderDelay=0,frameSamples=1024;
    uint32_t drainSamples=0;
    std::vector<uint8_t> config;
    uint64_t packets=0,samples=0;
    int64_t nextPTS=0;
    void properties(IPropertyProvider* p) {
        u32(p,pIOPropBitDepth,16); u32(p,pIOPropBitsPerSample,16);
        u32(p,pIOPropSamplingRate,rate); u32(p,pIOPropNumChannels,channels);
        u8(p,pIOPropIsFloat,0); u8(p,pIOPropByteOrder,0);
        u32(p,pIOPropDecoderDelay,decoderDelay);
        // For audio, the host interprets "frames" as PCM sample frames, not
        // compressed AAC access units. Include the algorithmic delay and two
        // AAC frames so a seek can rebuild overlap/limiter state.
        u32(p,pIOPropDecoderPreroll,decoderDelay+2*frameSamples);
    }
    void load(IPropertyProvider* p) {
        double sampleRate=0;
        if(!p->GetUINT32(pIOPropSamplingRate,rate) && p->GetDouble(pIOPropSamplingRate,sampleRate) && sampleRate>0 && sampleRate<384001) rate=uint32_t(sampleRate);
        p->GetUINT32(pIOPropNumChannels,channels);
        PropertyType t; const void* ptr=nullptr;int n=0;
        if(p->GetProperty(pIOPropMagicCookie,&t,&ptr,&n)==errNone && t==propTypeUInt8 && n>0 && ptr) {
            const uint8_t* b=static_cast<const uint8_t*>(ptr);
            if(b[0]==3) ascIn(b,n,config);
            else if(n>4 && b[0]==0 && b[4]==3) ascIn(b+4,n-4,config);
            else config.assign(b,b+n);
        }
    }
    bool open() {
        if(dec) return true;
        if(config.empty()) {
            static const uint32_t rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
            auto it=std::find(std::begin(rates),std::end(rates),rate);
            if(it==std::end(rates) || channels<1 || channels>6) return false;
            unsigned bits=(2<<11)|(unsigned(it-rates)<<7)|(channels<<3);
            config={uint8_t(bits>>8),uint8_t(bits)};
            trace("missing ASC: synthesized AAC-LC rate=%u channels=%u",rate,channels);
        }
        dec=aacDecoder_Open(TT_MP4_RAW,1);
        if(!dec) return false;
        UCHAR* p=config.data();UINT n=config.size();
        auto err=aacDecoder_ConfigRaw(dec,&p,&n);
        trace("ConfigRaw status=%x bytes=%zu",int(err),config.size());
        if(err!=AAC_DEC_OK) {aacDecoder_Close(dec);dec=nullptr;return false;}
        return true;
    }
public:
    AACDecoder() {trace("CREATE decoder=%p",this);}
    ~AACDecoder() {trace("DESTROY decoder=%p packets=%llu samples=%llu",this,(unsigned long long)packets,(unsigned long long)samples);if(dec)aacDecoder_Close(dec);}
protected:
    void DoFlush() override {trace("FLUSH decoder=%p",this);if(dec)aacDecoder_Close(dec);dec=nullptr;nextPTS=0;drainSamples=0;}
    StatusCode DoInit(HostPropertyCollectionRef* p) override {
        dump(p,"INIT");load(p);return errNone;
    }
    StatusCode DoOpen(HostBufferRef* p) override {
        dump(p,"OPEN");load(p);
        if(!open()) return errFail;
        // Resolve supplies the first compressed packet for probing. FDK only
        // reports its actual output delay after decoding one frame. Discard
        // this probe and reset: the host will submit the packet again.
        char* raw=nullptr;size_t bytes=0;
        if(!p->LockBuffer(&raw,&bytes))return errFail;
        std::vector<UCHAR> first;
        if(bytes>0 && bytes<=1024*1024)first.assign(raw,raw+bytes);
        p->UnlockBuffer();
        if(bytes>1024*1024)return errInvalidParam;
        if(!first.empty()) {
            UCHAR* in=first.data();UINT size=first.size(),valid=size;
            std::vector<INT_PCM> scratch(2048*8);
            auto rc=aacDecoder_Fill(dec,&in,&size,&valid);
            if(rc!=AAC_DEC_OK || valid)return errFail;
            rc=aacDecoder_DecodeFrame(dec,scratch.data(),scratch.size(),0);
            if(rc!=AAC_DEC_OK)return errFail;
            const CStreamInfo* info=aacDecoder_GetStreamInfo(dec);
            if(!info || info->numChannels<1 || info->numChannels>8 || info->frameSize<=0)return errFail;
            rate=info->sampleRate;channels=info->numChannels;
            decoderDelay=info->outputDelay;frameSamples=info->frameSize;
            aacDecoder_Close(dec);dec=nullptr;
            if(!open())return errFail;
        }
        trace("TIMING delay=%u preroll=%u probeBytes=%zu",decoderDelay,decoderDelay+2*frameSamples,bytes);
#ifdef AAC_RUNTIME_BRIDGE
        AACBridge::setAudioChannels(channels);
#endif
        properties(p);return errNone;
    }
    StatusCode DoProcess(HostBufferRef* p) override {
        if(packets<3) dump(p,"PROCESS");
        if(!open()) return errFail;
        char* raw=nullptr;size_t n=0;
        if(!p->LockBuffer(&raw,&n)) return errFail;
        if(n>1024*1024) {p->UnlockBuffer();return errInvalidParam;}
        bool draining=n==0;
        std::vector<UCHAR> input;
        if(n)input.assign(raw,raw+n);
        p->UnlockBuffer();
        // An empty packet is EOF. Resolve retries a successful empty decode
        // until its request is filled; returning success with no output spins
        // forever on MKV files whose duration includes a partial final frame.
        // Drain FDK's finite delay, then stop the host's read with errFail (the
        // SDK has no separate EOF status). Never generate unlimited padding.
        if(draining && !drainSamples)return errFail;
        AAC_DECODER_ERROR err=AAC_DEC_OK;
        if(!draining) {
            UCHAR* in=input.data();UINT size=input.size(),valid=size;
            err=aacDecoder_Fill(dec,&in,&size,&valid);
            if(err!=AAC_DEC_OK || valid) {trace("Fill error=%x left=%u",int(err),valid);return errFail;}
        }
        std::vector<INT_PCM> pcm(2048*8);
        err=aacDecoder_DecodeFrame(dec,pcm.data(),pcm.size(),draining?AACDEC_FLUSH:0);
        if(err==AAC_DEC_NOT_ENOUGH_BITS) return errMoreData;
        if(err!=AAC_DEC_OK) {trace("Decode error=%x",int(err));return errFail;}
        CStreamInfo* info=aacDecoder_GetStreamInfo(dec);
        if(!info || info->frameSize<=0 || info->numChannels<=0 || info->numChannels>8) return errFail;
        if(draining)drainSamples=drainSamples>uint32_t(info->frameSize)?drainSamples-info->frameSize:0;
        else drainSamples=info->outputDelay;
        if(packets==0)trace("STREAM frameSize=%d outputDelay=%u",info->frameSize,info->outputDelay);
        rate=info->sampleRate;channels=info->numChannels;
        HostBufferRef out;
        size_t bytes=size_t(info->frameSize)*channels*sizeof(INT_PCM);
        if(!out.IsValid()||!out.Resize(bytes)) return errAlloc;
        char* dst=nullptr;size_t cap=0;
        if(!out.LockBuffer(&dst,&cap))return errFail;
        if(cap<bytes) {out.UnlockBuffer();return errFail;}
        memcpy(dst,pcm.data(),bytes);out.UnlockBuffer();properties(&out);
        int64_t pts=nextPTS;if(!draining)p->GetINT64(pIOPropPTS,pts);
        int64_t duration=info->frameSize;uint32_t tb[2]={1,rate};
        out.SetProperty(pIOPropPTS,propTypeInt64,&pts,1);
        out.SetProperty(pIOPropDuration,propTypeInt64,&duration,1);
        out.SetProperty(pIOPropTimeBase,propTypeUInt32,tb,2);
        nextPTS=pts+duration;packets++;samples+=duration;
        StatusCode rc=m_pCallback?m_pCallback->SendOutput(&out):errInvalidOperation;
        if(packets<=3 || rc!=errNone || packets%500==0) trace("OUTPUT packet=%llu samples=%d channels=%u rate=%u callback=%d",(unsigned long long)packets,info->frameSize,channels,rate,int(rc));
        return rc;
    }
};

StatusCode g_HandleGetInfo(HostPropertyCollectionRef* p) {
    trace("GETINFO host version=%u",GetHostAPI()->version);
    p->SetProperty(pIOPropUUID,propTypeUInt8,pluginUUID,16);
    const char* name="Resolve AAC decoder";
    return p->SetProperty(pIOPropName,propTypeString,name,strlen(name));
}
StatusCode g_HandleCreateObj(unsigned char* uuid,ObjectRef* out) {
    trace("CREATE request uuid=%02x%02x%02x%02x",uuid[0],uuid[1],uuid[2],uuid[3]);
    if(memcmp(uuid,codecUUID,15)!=0 || uuid[15]<0x90 || uuid[15]>0x91) return errUnsupported;
    *out=new AACDecoder;return errNone;
}
StatusCode g_HandlePluginStart() {
    trace("START");
#ifdef AAC_RUNTIME_BRIDGE
    if(AACBridge::install())g_Log(logLevelInfo,"AAC bridge active (memory only); application files unchanged");
    else g_Log(logLevelWarn,"AAC bridge inactive: this Resolve layout is not recognized");
#endif
    return errNone;
}
StatusCode g_HandlePluginTerminate() {trace("TERMINATE");return errNone;}
StatusCode g_ListContainers(HostListRef*) {return errNone;}
StatusCode g_GetEncoderSettings(unsigned char*,HostPropertyCollectionRef*,HostListRef*) {return errUnsupported;}
StatusCode g_ListCodecs(HostListRef* list) {
    trace("LIST CODECS");
    for(unsigned i=0;i<2;i++) {
        HostPropertyCollectionRef p;if(!p.IsValid())return errAlloc;
        uint8_t uuid[16];memcpy(uuid,codecUUID,16);uuid[15]+=i;
        p.SetProperty(pIOPropUUID,propTypeUInt8,uuid,16);
        const char* name=i?"AAC mp4a decoder":"AAC decoder";
        p.SetProperty(pIOPropName,propTypeString,name,strlen(name));
        u32(&p,pIOPropFourCC,i?0x6d703461:0x61616320);
        u32(&p,pIOPropMediaType,mediaAudio);u32(&p,pIOPropCodecDirection,dirDecode);
        u32(&p,pIOPropBitDepth,16);u32(&p,pIOPropBitsPerSample,16);
        u32(&p,pIOPropInitFrameBytes,0);u8(&p,pIOPropIsFloat,0);
        bool ok=list->Append(&p);trace("REGISTER 4cc=%08x decode-only appended=%d",i?0x6d703461:0x61616320,int(ok));
        if(!ok)return errFail;
    }
    return errNone;
}
