const fs=require('fs'),vm=require('vm'),assert=require('assert');
const {TextEncoder}=require('util');const crypto=require('crypto').webcrypto;
const enc=new TextEncoder();let c,requests=[],session=null,committed='',playCount=0,lostChunk=false,lostCommit=false;
function crc32(bytes){let crc=0xffffffff;for(const x of bytes){crc^=x;for(let b=0;b<8;b++)crc=(crc>>>1)^((crc&1)?0xedb88320:0);}return (crc^0xffffffff)>>>0;}
const instance={dialogRef:{close(){},afterClosed(){return {subscribe(){}}}}};
const context={settings:{controlDeviceId:'test-device'},$scope:{$injector:{get(){return {customDialog(html,fn){c=instance;fn(instance);return {subscribe(){}};}}}}},servicesMap:new Map([['customDialog','dialog']]),detectChanges(){},http:{post(url,body){return {subscribe(cb){requests.push(body);setTimeout(()=>{
 const p=body.params;
 switch(body.method){
 case 'getStatus':return cb.next({firmware:'school-node-text-mp3-v2',text_max_bytes:131072,mp3_max_bytes:2097152,psram_bytes:8388608,upload_chunk_bytes:1536,volume_permille:1000,audio_ok:true,fire:false,upload_active:false,media_active:false});
 case 'beginUpload':session={...p,bytesReceived:Buffer.alloc(0)};return cb.next({accepted:true,offset:0});
 case 'uploadChunk':{
  const chunk=Buffer.from(p.data,'base64');assert(chunk.length<=1536);assert.strictEqual(p.id,session.id);
  if(p.offset===session.bytesReceived.length)session.bytesReceived=Buffer.concat([session.bytesReceived,chunk]);
  else assert(session.bytesReceived.subarray(p.offset,p.offset+chunk.length).equals(chunk));
  if(!lostChunk){lostChunk=true;return cb.error(new Error('Lost chunk ACK'));}
  return cb.next({accepted:true,offset:session.bytesReceived.length});}
 case 'commitUpload':
  assert.strictEqual(session.bytesReceived.length,session.bytes);assert.strictEqual(crc32(session.bytesReceived),session.crc32);
  if(committed!==p.id){committed=p.id;playCount++;}
  if(!lostCommit){lostCommit=true;return cb.error(new Error('Lost commit ACK'));}
  return cb.next({accepted:true,status:'queued'});
 case 'cancelUpload':return cb.next({accepted:true});
 case 'setVolume':return cb.next(p);
 case 'stop':return cb.next({stopped:true});
 default:return cb.error(new Error('Unexpected RPC '+body.method));
 }
 },0);}};}}};
vm.runInNewContext(fs.readFileSync(__dirname+'/../thingsboard/widgets/control_action.js','utf8'),{widgetContext:context,TextEncoder,crypto,Date,Uint8Array,Map,btoa:s=>Buffer.from(s,'binary').toString('base64'),setTimeout,clearTimeout,console});
(async()=>{
 await new Promise(r=>setTimeout(r,20));
 c.setText(('Thông báo: tiếng Việt 🔊\n').repeat(3000));assert(c.textBytes<=131072);await c.speak();
 assert(session.bytesReceived.equals(Buffer.from(c.text.trim(),'utf8')));assert.strictEqual(playCount,1);assert(c.status.includes('CRC32'));
 const before=requests.filter(x=>x.method==='beginUpload').length;c.setText('a'.repeat(131073));await c.speak();assert.strictEqual(requests.filter(x=>x.method==='beginUpload').length,before);
 const file=Buffer.alloc(65537);for(let i=0;i<file.length;i++)file[i]=i%256;
 c.chooseFile({target:{files:[{name:'voice.mp3',size:file.length,arrayBuffer:async()=>file.buffer.slice(file.byteOffset,file.byteOffset+file.length)}]}});
 await c.uploadMP3();assert(session.bytesReceived.equals(file));assert.strictEqual(playCount,2);
 c.volumeChange(12);c.volumeChange(65);c.volumeChange(25);await new Promise(r=>setTimeout(r,400));assert.deepStrictEqual(requests.filter(x=>x.method==='setVolume').map(x=>x.params),[25]);
 await c.stop();assert(c.status.includes('xác nhận dừng'));
 console.log('PASS: Vietnamese/emoji long text, negotiated size limit, raw MP3 bytes, CRC, lost packet+commit ACK, no duplicate play, volume coalescing, stop');
})().catch(e=>{console.error(e);process.exitCode=1;});
