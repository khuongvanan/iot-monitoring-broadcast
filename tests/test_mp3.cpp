#include <cstdio>
#include <vector>
#include <cassert>
#include "../firmware/School_Node/audio_resampler.h"
#include "../firmware/School_Node/upload_buffer.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "../firmware/School_Node/minimp3.h"
int main(int argc,char**argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 std::vector<unsigned char>b(n);assert(fread(b.data(),1,n,f)==(size_t)n);fclose(f);
 UploadBuffer u;uint32_t crc=UploadBuffer::updateCRC(0xffffffff,b.data(),b.size())^0xffffffff;
 assert(u.begin((uint8_t*)malloc(b.size()+1),"mp3",b.size(),crc,true,1,0));
 for(size_t off=0;off<b.size();){size_t chunk=std::min(size_t(1536),b.size()-off);assert(u.append("mp3",off,b.data()+off,chunk,1,0));off+=chunk;}
 assert(u.complete());assert(memcmp(u.data,b.data(),b.size())==0);
 mp3dec_t d;mp3dec_init(&d);short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];size_t off=0;assert(mp3Offset(b.data(),b.size(),off));int frames=0,hz=0,channels=0;long samples=0;
 AudioResampler48k resampler; size_t outputFrames=0;
 auto sink=[&](int16_t l,int16_t r){assert(l==r);++outputFrames;return true;};
 while(off<b.size()){
  mp3dec_frame_info_t info={};int count=mp3dec_decode_frame(&d,b.data()+off,b.size()-off,pcm,&info);
  if(info.frame_bytes<=0)break;off+=info.frame_bytes;if(!count)continue;
  assert(info.channels==1||info.channels==2);assert(info.hz>=8000&&info.hz<=48000);
  if(hz)assert(info.hz==hz);else assert(resampler.begin(info.hz));
  assert(resampler.push(pcm,count,info.channels,true,sink));
  hz=info.hz;channels=info.channels;samples+=count;frames++;
 }
 assert(frames>0);assert(resampler.finish(sink));
 assert(outputFrames==(samples*48000LL+hz-1)/hz);
 printf("PASS decoded alert -> resampler: %zu output stereo frames at 48000 Hz\n",outputFrames);printf("PASS embedded alert: %d frames, %d Hz, %d channel(s), %.3f s decoded, %zu/%ld bytes consumed\n",frames,hz,channels,(double)samples/hz,off,n);
}
