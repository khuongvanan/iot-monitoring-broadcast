#include <cassert>
#include <cstdio>
#include <vector>
#include "../firmware/School_Node/upload_buffer.h"
int main(){
 const uint8_t text[]="123456789";
 assert((UploadBuffer::updateCRC(0xffffffff,text,9)^0xffffffff)==0xcbf43926U);
 UploadBuffer u;assert(u.begin((uint8_t*)malloc(10),"session",9,0xcbf43926U,false,3,1));
 assert(!u.append("other",0,text,4,3,2));assert(!u.append("session",0,text,4,2,2));
 assert(!u.append("session",1,text,4,3,2));assert(!u.complete());
 assert(u.append("session",0,text,4,3,2));assert(u.used==4);
 assert(u.append("session",0,text,4,3,3));assert(u.used==4);
 const uint8_t bad[]="9999";assert(!u.append("session",0,bad,4,3,4));
 assert(!u.append("session",4,text,6,3,4));
 assert(u.append("session",4,text+4,5,3,5));assert(u.complete());assert(u.data[9]==0);
 uint8_t *p=u.release();assert(!u.data);assert(memcmp(p,text,9)==0);free(p);
 assert(u.begin((uint8_t*)malloc(10),"badcrc",9,0,true,3,1));assert(u.append("badcrc",0,text,9,3,1));assert(!u.complete());u.clear();

 std::vector<uint8_t> mp3(2*1024*1024);for(size_t i=0;i<mp3.size();i++)mp3[i]=uint8_t(i*29);
 uint32_t crc=UploadBuffer::updateCRC(0xffffffff,mp3.data(),mp3.size())^0xffffffff;
 assert(u.begin((uint8_t*)malloc(mp3.size()+1),"large",mp3.size(),crc,true,8,1));
 for(size_t off=0;off<mp3.size();){size_t n=mp3.size()-off;if(n>1536)n=1536;
  assert(u.append("large",off,mp3.data()+off,n,8,2));assert(u.append("large",off,mp3.data()+off,n,8,2));off+=n;}
 assert(u.complete());assert(memcmp(u.data,mp3.data(),mp3.size())==0);u.clear();
 size_t offset=0;uint8_t tag[16]={'I','D','3',4,0,0,0,0,0,4,0,0,0,0,255,251};
 assert(mp3Offset(tag,sizeof(tag),offset)&&offset==14);tag[9]=127;assert(!mp3Offset(tag,sizeof(tag),offset));
 puts("PASS: CRC32 known vector, lost ACK, duplicate mismatch, wrong session/epoch, bounds, ownership, 2 MiB exact transfer, ID3 bounds");
}
