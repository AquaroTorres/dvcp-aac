// Exercise the actual plugin decoder through the shipped SDK buffer wrappers.
#include "decoder.cpp"
#include <cassert>
#include <cmath>
#include <fdk-aac/aacenc_lib.h>
struct Prop {PropertyType type; int count; std::vector<uint8_t> bytes;};
struct Obj {int refs=1;std::map<std::string,Prop> props;std::vector<uint8_t> data;};
static uint64_t outputSamples=0,hash=1469598103934665603ULL;
static double energy=0;

static StatusCode host(MessageID message,...) {
    va_list ap;va_start(ap,message);
    StatusCode rc=errNone;
    if(message==msgCreate) {
        (void)va_arg(ap,unsigned char*);*va_arg(ap,ObjectRef*)=new Obj;
    } else {
        Obj* o=static_cast<Obj*>(va_arg(ap,ObjectRef));
        switch(message) {
        case msgRetain: *va_arg(ap,int*)=++o->refs;break;
        case msgRelease: {int n=--o->refs;*va_arg(ap,int*)=n;if(!n)delete o;break;}
        case msgPropSet: {
            const char* key=va_arg(ap,PropertyID);PropertyType t=PropertyType(va_arg(ap,int));
            const uint8_t* v=static_cast<const uint8_t*>(va_arg(ap,const void*));int n=va_arg(ap,int);
            size_t units[]={0,1,1,2,2,4,4,8,8,8,1};
            assert(t>=0 && size_t(t)<sizeof(units)/sizeof(*units) && n>=0);
            o->props[key]={t,n,std::vector<uint8_t>(v,v+units[t]*n)};break;
        }
        case msgPropGet: {
            const char* key=va_arg(ap,PropertyID);auto it=o->props.find(key);
            auto type=va_arg(ap,PropertyType*);auto ptr=va_arg(ap,const void**);auto count=va_arg(ap,int*);
            if(it==o->props.end())rc=errNoParam;
            else {*type=it->second.type;*ptr=it->second.bytes.data();*count=it->second.count;}
            break;
        }
        case msgBufferResize: o->data.resize(va_arg(ap,size_t));break;
        case msgBufferLock: {*va_arg(ap,unsigned char**)=o->data.data();*va_arg(ap,size_t*)=o->data.size();break;}
        case msgBufferUnlock: break;
        case msgCodecProcessData: {
            Obj* b=static_cast<Obj*>(va_arg(ap,ObjectRef));
            assert(b->data.size()%2==0);
            for(size_t i=0;i<b->data.size();i+=2) {
                int16_t value;memcpy(&value,b->data.data()+i,2);
                double v=value/32768.;energy+=v*v;outputSamples++;
            }
            for(uint8_t b:b->data){hash^=b;hash*=1099511628211ULL;}
            break;
        }
        default: rc=errUnsupported;
        }
    }
    va_end(ap);return rc;
}
class TestDecoder : public AACDecoder {
public: using AACDecoder::DoInit;using AACDecoder::DoOpen;using AACDecoder::DoProcess;using AACDecoder::DoFlush;
};

struct Encoded {std::vector<uint8_t> asc;std::vector<std::vector<uint8_t>> packets;};
static Encoded tone() {
    HANDLE_AACENCODER encoder=nullptr;AACENC_InfoStruct info={};
    assert(aacEncOpen(&encoder,0,2)==AACENC_OK);
    assert(aacEncoder_SetParam(encoder,AACENC_AOT,AOT_AAC_LC)==AACENC_OK);
    assert(aacEncoder_SetParam(encoder,AACENC_SAMPLERATE,48000)==AACENC_OK);
    assert(aacEncoder_SetParam(encoder,AACENC_CHANNELMODE,MODE_2)==AACENC_OK);
    assert(aacEncoder_SetParam(encoder,AACENC_BITRATE,192000)==AACENC_OK);
    assert(aacEncoder_SetParam(encoder,AACENC_TRANSMUX,TT_MP4_RAW)==AACENC_OK);
    assert(aacEncEncode(encoder,nullptr,nullptr,nullptr,nullptr)==AACENC_OK);
    assert(aacEncInfo(encoder,&info)==AACENC_OK && info.frameLength==1024);
    Encoded result;result.asc.assign(info.confBuf,info.confBuf+info.confSize);
    for(int frame=0;frame<72;frame++) {
        INT_PCM pcm[2048];uint8_t packet[8192];
        for(int i=0;i<1024;i++)for(int ch=0;ch<2;ch++)
            pcm[2*i+ch]=INT_PCM(8000*std::sin(2*3.141592653589793*(440+440*ch)*(frame*1024+i)/48000));
        void* ib=pcm;void* ob=packet;int iid=IN_AUDIO_DATA,oid=OUT_BITSTREAM_DATA;
        int isize=sizeof(pcm),osize=sizeof(packet),ielem=sizeof(INT_PCM),oelem=1;
        AACENC_BufDesc in={1,&ib,&iid,&isize,&ielem},out={1,&ob,&oid,&osize,&oelem};
        AACENC_InArgs ia={};AACENC_OutArgs oa={};ia.numInSamples=frame<64?2048:-1;
        auto rc=aacEncEncode(encoder,&in,&out,&ia,&oa);
        if(rc==AACENC_ENCODE_EOF)break;
        assert(rc==AACENC_OK);
        if(oa.numOutBytes)result.packets.emplace_back(packet,packet+oa.numOutBytes);
    }
    assert(aacEncClose(&encoder)==AACENC_OK && result.packets.size()>=64);
    return result;
}
static void fill(HostBufferRef& buffer,const std::vector<uint8_t>& bytes) {
    assert(buffer.Resize(bytes.size()));char* dst;size_t size;
    assert(buffer.LockBuffer(&dst,&size));assert(size>=bytes.size());
    memcpy(dst,bytes.data(),bytes.size());buffer.UnlockBuffer();
}
static uint64_t run(const Encoded& media,bool missing,bool probe) {
    Obj callback;TestDecoder decoder;assert(decoder.SetCallback(&callback)==errNone);
    HostPropertyCollectionRef init;assert(decoder.DoInit(&init)==errNone);
    HostBufferRef open;double rate=48000;
    open.SetProperty(pIOPropSamplingRate,propTypeDouble,&rate,1);u32(&open,pIOPropNumChannels,2);
    const uint8_t esds[]={3,128,128,128,27,0,2,0,4,128,128,128,13,64,21,0,0,0,0,2,238,0,0,2,238,0,6,128,128,128,1,2};
    open.SetProperty(pIOPropMagicCookie,propTypeUInt8,missing?esds:media.asc.data(),missing?sizeof(esds):media.asc.size());
    if(probe)fill(open,media.packets.front());
    assert(decoder.DoOpen(&open)==errNone);
    uint32_t delay=0,preroll=0;
    assert(open.GetUINT32(pIOPropDecoderDelay,delay));
    assert(open.GetUINT32(pIOPropDecoderPreroll,preroll));
    assert((probe?delay>0:delay==0) && preroll>=delay+2048);
    uint64_t firstHash=0;
    for(int repeat=0;repeat<2;repeat++) {
        decoder.DoFlush();outputSamples=0;energy=0;hash=1469598103934665603ULL;
        int64_t pts=0;
        for(const auto& packet:media.packets) {
            HostBufferRef input;fill(input,packet);
            input.SetProperty(pIOPropPTS,propTypeInt64,&pts,1);pts+=1024;
            assert(decoder.DoProcess(&input)==errNone);
        }
        double rms=std::sqrt(energy/outputSamples);
        assert(outputSamples==media.packets.size()*1024*2 && rms>0.12 && rms<0.20);
        if(!repeat)firstHash=hash;else assert(hash==firstHash);
        // MKV can request samples beyond the last compressed packet. EOF must
        // make finite progress and then terminate, including after a seek reset.
        HostBufferRef eof;unsigned drained=0;
        while(decoder.DoProcess(&eof)==errNone) {assert(++drained<=4);}
        assert(drained>0 && decoder.DoProcess(&eof)==errFail);
        decoder.DoFlush();assert(decoder.DoProcess(&eof)==errFail);
    }
    printf("PASS: %s ASC, %s probe, %llu PCM samples; flush/replay identical, RMS %.6f\n",
           missing?"missing":"normal",probe?"with":"without",(unsigned long long)outputSamples,std::sqrt(energy/outputSamples));
    return hash;
}
int main() {
    APIContext api={1,host};SetHostAPI(&api);auto media=tone();
    uint64_t expected=run(media,false,false);
    assert(run(media,true,false)==expected);
    assert(run(media,false,true)==expected);
    assert(run(media,true,true)==expected);
}
