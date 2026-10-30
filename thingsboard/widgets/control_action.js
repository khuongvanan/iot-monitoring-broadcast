const HTML_TEMPLATE="<div style=\"width:min(720px,92vw);max-height:85vh;overflow:auto;padding:24px;box-sizing:border-box;font:14px/1.6 Arial,sans-serif;color:#19334b\">\n<h2 style=\"margin:0\">Văn bản &amp; MP3</h2>\n<p>Gửi trực tiếp qua ThingsBoard → ESP32. Nhận đủ và kiểm tra dữ liệu trước khi phát.</p>\n<div style=\"padding:12px;background:#edf5f7;border-radius:8px\">{{limitsLabel}}</div>\n<h3>1. Nhập văn bản</h3>\n<textarea style=\"width:100%;box-sizing:border-box;padding:12px\" rows=\"7\" [disabled]=\"busy\" [value]=\"text\" (input)=\"setText($event.target.value)\" placeholder=\"Nhập nội dung tiếng Việt…\"></textarea>\n<p>{{textBytes}} byte UTF-8 / {{textLimit}} byte hiện khả dụng</p>\n<button mat-raised-button color=\"primary\" [disabled]=\"busy || !ready\" (click)=\"speak()\">Gửi và đọc văn bản</button>\n<h3>2. Chọn file MP3</h3>\n<input type=\"file\" accept=\".mp3,audio/mpeg\" [disabled]=\"busy\" (change)=\"chooseFile($event)\">\n<p>{{fileLabel}}</p>\n<button mat-raised-button color=\"primary\" [disabled]=\"busy || !ready || !file\" (click)=\"uploadMP3()\">Gửi và phát MP3</button>\n<p style=\"color:#65788a\">Giữ cửa sổ này mở đến khi gửi xong. File lớn có thể mất nhiều phút; thời gian phụ thuộc mạng và hạn mức ThingsBoard.</p>\n<progress style=\"width:100%\" max=\"100\" [value]=\"progress\"></progress>\n<p style=\"white-space:pre-wrap;overflow-wrap:anywhere;background:#edf5f7;padding:12px;border-radius:8px\">{{status}}</p>\n<button mat-stroked-button [disabled]=\"busy\" (click)=\"refresh()\">Đọc trạng thái / giới hạn</button>\n<button mat-raised-button color=\"warn\" (click)=\"stop()\">Dừng phát / hủy gửi</button>\n<hr style=\"margin:20px 0\">\n<label>Âm lượng: <b>{{volume}}%</b></label>\n<input style=\"width:100%\" type=\"range\" min=\"0\" max=\"100\" step=\"1\" [value]=\"volume\" (input)=\"volumeChange($event.target.value)\">\n<p>{{volumeState}}</p>\n<p style=\"font-size:12px\">100% là mức tín hiệu số; chưa hiệu chỉnh theo ngưỡng méo ampli. Báo lửa vẫn ưu tiên.</p>\n<button mat-button [disabled]=\"busy\" (click)=\"close()\">Đóng</button>\n</div>\n";
const injector=widgetContext.$scope.$injector;
const customDialog=injector.get(widgetContext.servicesMap.get('customDialog'));
const device=widgetContext.settings.controlDeviceId;
if(!device)throw new Error('Thiếu settings.controlDeviceId.');
customDialog.customDialog(HTML_TEMPLATE,function(vm){
  vm.text='';vm.textBytes=0;vm.textLimit=0;vm.fileLimit=0;vm.file=null;vm.fileLabel='Chưa chọn file';
  vm.busy=false;vm.ready=false;vm.progress=0;vm.status='Đang đọc cấu hình ESP32…';
  vm.limitsLabel='Chưa nhận được giới hạn từ thiết bị';vm.volume=100;vm.volumeState='Chưa xác nhận';
  const sessions=new Map();
  const encoder=new TextEncoder();let closed=false,operation=0,activeID=null,debounce=null,volumeBusy=false,latestVolume=null;
  const redraw=()=>widgetContext.detectChanges();
  const show=s=>{if(!closed){vm.status=s;redraw();}};
  const errorText=e=>e.message||e.error?.message||('HTTP '+(e.status||'?')+' — kiểm tra kết nối và quyền ThingsBoard.');
  const wait=ms=>new Promise(r=>setTimeout(r,ms));
  function rpc(method,params){return new Promise((resolve,reject)=>{
    widgetContext.http.post('/api/rpc/twoway/'+device,{method,params,timeout:60000,persistent:false})
      .subscribe({next:resolve,error:reject});
  });}
  function crc32(bytes){let crc=0xffffffff;for(const x of bytes){crc^=x;for(let b=0;b<8;b++)crc=(crc>>>1)^((crc&1)?0xedb88320:0);}return (crc^0xffffffff)>>>0;}
  function b64(bytes){let s='';for(const x of bytes)s+=String.fromCharCode(x);return btoa(s);}
  function check(token){if(closed||token!==operation)throw new Error('Đã hủy gửi.');}
  async function accepted(method,params,token){
    for(let attempt=0;attempt<3;attempt++){
      check(token);let r;
      try{r=await rpc(method,params);}catch(e){
        if(attempt===2)throw e;show('Chưa nhận phản hồi; thử lại đúng gói đang gửi…');await wait(1000*(attempt+1));continue;
      }
      check(token);
      if(r?.accepted)return r;
      if(r?.status?.includes('queue_busy')&&attempt<2){await wait(1000);continue;}
      throw new Error('Thiết bị từ chối: '+(r?.status||'kiểm tra báo lửa, bộ nhớ hoặc phiên truyền khác'));
    }
  }
  function applyStatus(s){
    if(s.firmware!=='school-node-text-mp3-v2')throw new Error('Cần nạp firmware text-mp3-v2 cùng bộ file này.');
    vm.textLimit=Number(s.text_max_bytes)||0;vm.fileLimit=Number(s.mp3_max_bytes)||0;
    vm.chunkSize=Math.min(1536,Number(s.upload_chunk_bytes)||1536);
    vm.ready=!s.fire&&!s.upload_active&&!s.media_active;
    vm.limitsLabel='Văn bản: '+vm.textLimit.toLocaleString()+' byte | MP3: '+(vm.fileLimit/1024).toFixed(0)+' KiB | PSRAM: '+(Number(s.psram_bytes||0)/1048576).toFixed(1)+' MiB';
    if(!volumeBusy&&latestVolume===null){vm.volume=Math.round(s.volume_permille/10);vm.volumeState='Thiết bị xác nhận: '+vm.volume+'%';}
    return s;
  }
  vm.refresh=async()=>{try{const s=applyStatus(await rpc('getStatus',{}));if(closed)return;
    const state=['nghỉ','đã xếp hàng','đang phát','phát xong','lỗi phát'][s.playback_state]||'không rõ';
    show('MP3: '+state+' | Báo lửa: '+s.fire+' | I2S: '+(s.audio_ok?'OK':'lỗi')+
      (s.upload_active?' | Đang nhận '+s.upload_received+'/'+s.upload_total+' byte':'')+
      (s.tts_last_http?' | HTTP TTS gần nhất: '+s.tts_last_http:''));
  }catch(e){vm.ready=false;show(errorText(e));}};
  vm.setText=t=>{vm.text=t;vm.textBytes=encoder.encode(t.trim()).length;};
  vm.chooseFile=e=>{vm.file=e.target.files[0]||null;vm.fileLabel=vm.file?(vm.file.name+' — '+vm.file.size.toLocaleString()+' byte'):'Chưa chọn file';};
  async function sendBytes(kind,bytes,token){
    check(token);const s=applyStatus(await rpc('getStatus',{}));check(token);
    const limit=kind==='text'?vm.textLimit:vm.fileLimit;
    if(!vm.ready)throw new Error('Thiết bị đang phát MP3, nhận file khác hoặc báo lửa. Dừng rồi thử lại.');
    if(!bytes.length||bytes.length>limit)throw new Error('Dung lượng '+bytes.length+' byte vượt mức hiện khả dụng '+limit+' byte, hoặc nội dung rỗng.');
    const id=crypto.randomUUID();activeID=id;sessions.set(token,id);const crc=crc32(bytes);const started=Date.now();
    await accepted('beginUpload',{id,kind,bytes:bytes.length,crc32:crc},token);
    let offset=0;
    while(offset<bytes.length){
      check(token);const chunk=bytes.subarray(offset,Math.min(bytes.length,offset+vm.chunkSize));
      const r=await accepted('uploadChunk',{id,offset,data:b64(chunk)},token);
      offset+=chunk.length;if(r.offset!==offset)throw new Error('Offset phản hồi không khớp; đã dừng để tránh dữ liệu sai.');
      vm.progress=Math.floor(offset*100/bytes.length);
      const elapsed=Math.max(1,(Date.now()-started)/1000),eta=Math.ceil((bytes.length-offset)/(offset/elapsed));
      show('Thiết bị đã nhận '+offset.toLocaleString()+'/'+bytes.length.toLocaleString()+' byte ('+vm.progress+'%). Còn khoảng '+eta+' giây theo tốc độ hiện tại.');
    }
    await accepted('commitUpload',{id},token);sessions.delete(token);if(activeID===id)activeID=null;
    show('ESP32 đã nhận đủ, CRC32 khớp và đã xếp hàng '+(kind==='mp3'?'phát MP3. Có thể đóng cửa sổ.':'đọc văn bản. Việc tạo giọng nói vẫn cần Internet.'));
  }
  async function start(kind){
    if(vm.busy)return;vm.busy=true;vm.progress=0;const token=++operation;
    try{
      let bytes;
      if(kind==='text'){
        if(vm.text.includes('\u0000'))throw new Error('Văn bản chứa ký tự NUL không hợp lệ.');
        bytes=encoder.encode(vm.text.trim());
      }else{
        const file=vm.file;if(!file||!file.name.toLowerCase().endsWith('.mp3'))throw new Error('Chọn file có định dạng MP3.');

        if(file.size>2097152)throw new Error('File vượt trần 2 MiB của bản firmware này.');
        bytes=new Uint8Array(await file.arrayBuffer());check(token);
      }
      await sendBytes(kind,bytes,token);
    }catch(e){
      const id=sessions.get(token);
      if(id){try{await rpc('cancelUpload',{id});}catch(_){}if(activeID===id)activeID=null;}
      if(token===operation)show(errorText(e));
    }finally{sessions.delete(token);if(token===operation){vm.busy=false;vm.ready=true;}redraw();}
  }
  vm.speak=()=>start('text');vm.uploadMP3=()=>start('mp3');
  vm.stop=async()=>{operation++;vm.busy=true;show('Đang gửi lệnh dừng…');
    try{const r=await rpc('stop',{});activeID=null;show(r.stopped?'ESP32 xác nhận dừng và hủy dữ liệu đang gửi. Báo lửa vẫn ưu tiên.':'Chưa xác nhận dừng.');}
    catch(e){show(errorText(e));}finally{vm.busy=false;vm.ready=true;redraw();}};
  async function sendVolume(){
    if(closed||volumeBusy||latestVolume===null)return;
    volumeBusy=true;const wanted=latestVolume;latestVolume=null;vm.volumeState='Đang gửi '+wanted+'%…';redraw();
    try{const r=await rpc('setVolume',wanted);if(r!==wanted)throw new Error('Chưa xác nhận mức âm lượng.');vm.volumeState='Thiết bị xác nhận: '+r+'%';}
    catch(e){vm.volumeState=errorText(e);}finally{volumeBusy=false;redraw();if(latestVolume!==null)sendVolume();}
  }
  vm.volumeChange=v=>{vm.volume=Math.max(0,Math.min(100,Math.round(Number(v))));latestVolume=vm.volume;clearTimeout(debounce);debounce=setTimeout(sendVolume,350);};
  vm.close=()=>{if(!vm.busy)vm.dialogRef.close(null);};
  vm.dialogRef.afterClosed().subscribe(()=>{closed=true;operation++;clearTimeout(debounce);if(activeID)rpc('cancelUpload',{id:activeID}).catch(()=>{});});
  vm.refresh();
}).subscribe();
