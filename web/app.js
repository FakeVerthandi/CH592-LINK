/* Device selection stays in the click gesture. No automatic reconnect/replay. */
'use strict';
(() => {
  const B=window.LinkBLE,S=window.LinkBLESession,$=id=>document.getElementById(id);
  let device=null,transport=null,programmer=null,probeInfo=null,image=null,busy=false,connecting=false,activeSession=null,closingAttempt=null;
  let features={},lastMessage='',lastState='idle',actionId=0,fileGeneration=0;
  const lines=[];
  function log(message){lastMessage=message;const stamp=new Date().toLocaleString('zh-CN',{hour12:false});lines.push(`[${stamp}] ${message}`);$('log').textContent=lines.join('\n');$('log').scrollTop=$('log').scrollHeight;if(message.startsWith('连接阶段：'))status(message.slice(5)+'…');}
  function status(message,error=false){$('status').textContent=message;$('status').classList.toggle('error-text',error);}
  const controller=new S.Controller({uuids:B.UUID,locks:navigator.locks,timers:{setTimeout:(...args)=>setTimeout(...args),clearTimeout:id=>clearTimeout(id)},log,
    onState:state=>{sync();if(state.state==='draining'){status('本页已停止蓝牙操作，等待系统释放旧请求；请勿在其他页面反复重连。',true);if(lastState!=='draining')log('旧蓝牙请求尚未结束；暂不允许新连接，收到迟到结果后将自动清理。');}
      else if(state.state==='idle'&&lastState==='draining'){status('旧蓝牙请求已结束，可重新选择设备。');log('旧请求已结束，网页会话清理完成。');}lastState=state.state;},
    onNotification:(s,value)=>{if(s===activeSession&&transport)transport.receive(value);},
    onDeviceDisconnect:s=>{if(closingAttempt?.session===s)closingAttempt.confirm();},
    onClosed:(s,error)=>{if(activeSession&&activeSession!==s)return;if(programmer)programmer.attached=false;activeSession=null;
      if(transport&&!transport.closed)transport.fail(error);status(error.message,!['NORMAL','LOCAL_CLOSE','PAGE_CLOSED'].includes(error.code));if(lastMessage!==error.message)log(error.message);sync();}
  });
  window.getLinkBleDiagnostics=()=>({version:probeInfo?.version||null,connected:connected(),session:controller.snapshot(),capabilities:activeSession?{...activeSession.capabilities}:null,transfer:transport?.lastTransfer?{...transport.lastTransfer}:null});
  function connected(){return !!(activeSession&&controller.isCurrent(activeSession)&&device?.gatt?.connected&&transport&&!transport.closed);}
  function sync(){
    const online=connected(),canOpen=controller.snapshot().canOpen,environment=!!(window.isSecureContext&&navigator.bluetooth?.requestDevice);
    $('connect').disabled=busy||online||!canOpen||!environment;$('reconnect').disabled=busy||online||!device||!canOpen||!environment;
    $('disconnect').disabled=busy||!online;$('cancel-connect').disabled=!connecting;
    $('reset-connection').disabled=busy||online||controller.state!=='draining';
    $('link-status').disabled=busy||!online||!features.linkStatus;
    $('retry-attach').disabled=busy||!online||!programmer||programmer.attached;$('diagnose').disabled=busy||!online||!features.diagnostic;
    const needsProbe=online&&(transport.fastData||twoWritesSelected())&&!transport.fastQualified;
    $('flash').disabled=busy||!online||!programmer?.attached||!image;
    $('flash').textContent=needsProbe?'检测快传、烧录并校验':'擦除、写入并校验';
    $('file').disabled=busy;$('stop').disabled=!busy||!programmer?.busy;
    $('write-mode').disabled=busy||(!canOpen&&!online);
    $('two-writes').disabled=!online||!features.twoWrites;
    $('strict-verify').disabled=busy;
    $('swd-clock').disabled=busy||!online||!features.writeProfile;
    $('transfer-test').disabled=busy||!online||!programmer?.attached;
    $('run').disabled=busy||!online||!programmer?.attached;$('release').disabled=busy||!online||!programmer?.attached;
    $('save-readback').disabled=!programmer?.readback||!image;
    $('connection').textContent=online?'BLE 已连接':controller.state==='draining'?'等待系统释放蓝牙请求':'未连接';$('connection').classList.toggle('connected',online);
  }
  function reportError(error){const e=error instanceof B.ProtocolError?error:S.classify(error);status(e.message,true);if(lastMessage!==e.message)log(e.message);}
  function drop(reason,s=activeSession||controller.current){return controller.close(s,reason instanceof Error?reason:new S.SessionError('CANCELLED',reason));}
  async function action(fn){if(busy)return;const id=++actionId;busy=true;sync();try{await fn();}catch(error){if(id===actionId)reportError(error);}finally{if(id===actionId){busy=false;sync();}}}
  function renderImageInfo() {
    if (!image) { $('image').textContent = '尚未选择文件'; return {mismatch: false, reasons: []}; }
    const target = programmer?.target || null;
    if (target) {
      const plan = B.planImageForTarget(image, target);
      const check = B.detectTargetMismatch(image, target);
      const isSector = target.eraseModel === B.ERASE_MODEL.STM32F4_SINGLE;
      const warn = check.mismatch ? `\n警告：${check.reasons.join('；')}` : '';
      $('image').textContent = `${image.name}\n${image.count} 有效字节 · 擦除 ${plan.eraseUnits.length} ${isSector ? '扇区' : '页'} / ${plan.eraseBytes / 1024}KiB · ${plan.verifyBlocks.length} 个校验块\n${B.hex(B.BASE + image.min)} ～ ${B.hex(B.BASE + image.max - 1)}\n初始 SP ${B.hex(image.sp)} · Reset ${B.hex(image.reset)}${warn}`;
      return check;
    } else {
      $('image').textContent = `${image.name}\n${image.count} 有效字节 · ${image.pages.length} 个 1KiB 块（连接目标后按芯片模型计算擦除单元）\n${B.hex(B.BASE + image.min)} ～ ${B.hex(B.BASE + image.max - 1)}\n初始 SP ${B.hex(image.sp)} · Reset ${B.hex(image.reset)}`;
      return {mismatch: false, reasons: []};
    }
  }
  async function attachTarget(){
    const s=activeSession,p=programmer;controller.check(s);status('正在通过 SWD 识别 STM32 目标…');
    const target=await controller.stage(s,'通过 SWD 识别目标',()=>p.attach(),15000);controller.check(s);
    const devHex='0x'+target.devId.toString(16).toUpperCase().padStart(3,'0');
    $('device').textContent=`${device.name||'CH592 LINK'} · BLE ${probeInfo.version}\nDP ${B.hex(target.dpid)} / DBGMCU ${B.hex(target.deviceid)}\n${target.familyName} (DEV ${devHex}) · ${target.flashKB}KiB Flash / ${target.sramKB}KiB SRAM · ${target.eraseSummary} · 已停机`;
    if(image){
      try{
        const check=renderImageInfo();
        if(check.mismatch){ status('检测到固件与当前芯片系列不匹配，请核对后再烧录。',true); log('防烧错警告：'+check.reasons.join('；')); }
        else status('目标识别与已选固件校验通过，可开始烧录。');
      }
      catch(err){ $('image').textContent=err.message; status(err.message,true); log('已选固件与当前目标不匹配：'+err.message); image=null; }
    } else {
      status('目标识别通过，可选择固件并烧录。');
    }
  }
  async function connectDevice(selected){
    connecting=true;let s;sync();
    try{
      device=selected;probeInfo=null;features={};
      s=await controller.open(selected);activeSession=s;
      const t=new B.Transport(s.rx,{fastData:false,onFatal:error=>{
        if(closingAttempt?.session===s)return;
        const f=t.lastTransfer;
        if(t.active && f?.cmd===B.CMD.WRITE)log(`${t.probeActive?'传输自检':'WRITE'}失败：序号 ${f.seq}，已提交 ${f.submitted}/${f.total} 片，偏移 ${f.offset}，${f.mode}，${f.phase}；${error.name}：${error.message}`);
        controller.close(s,error);
      }});transport=t;
      const info=B.decodeInfo(await controller.stage(s,'读取固件版本',()=>t.request(B.CMD.INFO)));controller.check(s);probeInfo=info;features=S.protocolFeatures(info);
      features.writeProfile=info.protocol===2||(info.protocol===1&&S.versionAtLeast(info.version,[0,3,16]));
      features.twoWrites=features.writeProfile&&(info.protocol===2||S.versionAtLeast(info.version,[0,3,17]))&&s.capabilities.writeAPI==='writeValueWithResponse';
      if(twoWritesSelected()&&!features.twoWrites)$('write-mode').value='safe';
      updateTransfer();
      $('device').textContent=`${device.name||'CH592 LINK'} · BLE ${info.version} · 协议 ${info.protocol}\n蓝牙已连接，正在识别 SWD 目标`;
      log(`设备 ${device.name||'CH592 LINK'}，固件 ${info.version}，协议 ${info.protocol}`);
      log(`连接能力：服务${s.capabilities.serviceDiscovery==='enumerate'?'枚举匹配':'兼容查询'}，${s.capabilities.writeAPI}，${s.capabilities.notifyMode}；${s.capabilities.pageLock?'已取得本浏览器页面互斥锁':'请保持仅一个工具页面连接设备'}`);
      programmer=new B.Programmer(t,{features,log:message=>{if(controller.isCurrent(s))log(message);},progress:(stageName,done,total,address)=>{if(!controller.isCurrent(s))return;
        $('stage').textContent=stageName;$('progress').value=total?done/total:0;$('percent').textContent=Math.round((total?done/total:0)*100)+'%';status(`${stageName} · ${done} / ${total} · ${B.hex(address)}`);}});
      if(features.linkStatus){try{await controller.stage(s,'读取蓝牙状态',()=>programmer.linkStatus());}catch(error){if(error instanceof B.ProtocolError&&error.status===1){features.linkStatus=false;log('固件未提供蓝牙状态扩展，继续基本连接。');}else throw error;}}
      await attachTarget();
    }catch(error){if(!(error instanceof B.ProtocolError)||transport?.closed)controller.close(s||controller.current,S.classify(error));throw error;}
    finally{connecting=false;sync();}
  }
  async function disconnectDevice(){
    const s=activeSession,p=programmer;controller.check(s);
    if(!features.deviceDisconnect){drop(new S.SessionError('LOCAL_CLOSE','当前固件不支持设备主动断链，已请求释放网页连接'),s);return;}
    if(p.busy||transport.active)throw Error('请先停止当前烧录并等待当前命令完成');
    let timer,resolveEnded;const ended=new Promise(resolve=>{resolveEnded=()=>resolve({ended:true});});
    const attempt={session:s,confirm:resolveEnded};closingAttempt=attempt;s.expectedDisconnect=true;
    const deadline=new Promise(resolve=>{timer=setTimeout(()=>resolve({timeout:true}),8000);});
    status('请求 CH592 主动断链，等待设备终止事件…');log('请求 CH592 主动断链；收到终止事件前保持网页 GATT 连接');
    try{
      const ack=p.requestLinkDisconnect().then(()=>({accepted:true}),error=>({error}));const first=await Promise.race([ack,ended,deadline]);
      if(first.error instanceof B.ProtocolError&&s.device.gatt.connected)throw first.error;
      if(first.error)log('断链请求应答：'+first.error.message);
      const result=first.ended||first.timeout?first:await Promise.race([ended,deadline]);
      if(!result.ended)throw Error('8 秒内未确认设备断链，请保留 USB 诊断现场');
      log('已收到 GATT 断开事件；可重新连接并核对芯片断连计数');drop(new S.SessionError('NORMAL','已收到 GATT 断开事件，网页会话已释放'),s);
    }catch(error){if(error instanceof B.ProtocolError&&error.status===1){features.deviceDisconnect=false;error.message+='；当前连接保留，再次点击断开将仅释放网页连接';}
      else if(!(error instanceof B.ProtocolError))drop(new S.SessionError('DISCONNECT_UNCONFIRMED','设备端断链未确认，仅释放网页会话：'+error.message),s);throw error;}
    finally{clearTimeout(timer);s.expectedDisconnect=false;if(closingAttempt===attempt)closingAttempt=null;}
  }
  $('connect').addEventListener('click',async()=>{
    if(!window.isSecureContext||!navigator.bluetooth?.requestDevice){status('请在支持Web Bluetooth的浏览器中打开本地localhost工具。',true);return;}
    if(busy||!controller.snapshot().canOpen)return;
    let selected;try{selected=navigator.bluetooth.requestDevice({filters:[{services:[B.UUID.service]},{namePrefix:'CH592-LINK'}],optionalServices:[B.UUID.service]});}catch(error){reportError(error);return;}
    await action(async()=>connectDevice(await selected));
  });
  $('reconnect').addEventListener('click',()=>action(async()=>{if(!device)throw Error('请先选择蓝牙设备');log('正在重连上次设备；重新发现服务并订阅，不自动擦写');await connectDevice(device);}));
  $('cancel-connect').addEventListener('click',()=>drop('已取消连接等待'));
  $('reset-connection').addEventListener('click',()=>{if(busy||connected()||controller.state!=='draining')return;
    log('用户选择保存日志并重新加载工具；当前操作不会重放，之后需重新选择固件。');download('CH592_LINK_BLE_recovery_log.txt',lines.join('\n'),'text/plain;charset=utf-8');controller.dispose();setTimeout(()=>window.location.reload(),250);});
  $('disconnect').addEventListener('click',()=>action(disconnectDevice));
  $('retry-attach').addEventListener('click',()=>action(attachTarget));
  $('diagnose').addEventListener('click',()=>action(async()=>status(await programmer.diagnose())));
  $('link-status').addEventListener('click',()=>action(async()=>status(await programmer.linkStatus())));
  function twoWritesSelected(){return $('write-mode').value==='two';}
  function updateTransfer(){
    try{
      const interval=Number($('write-mode').value);
      const mixed=[4,8,16].includes(interval);
      transport?.setFastData(mixed,mixed?interval:16);
      if(mixed&&transport&&!transport.supportsFastData)$('write-mode').value='safe';
      log(`写入模式：${twoWritesSelected()?'满块两次确认（需要 MTU ≥142）':mixed&&(!transport||transport.fastData)?`20B 混合，每${interval}片确认`:'20B 保守发送'}`);
    }catch(error){reportError(error);}sync();
  }
  $('write-mode').addEventListener('change',updateTransfer);
  async function applyWriteProfile(p,s){
    if(features.writeProfile){
      await p.configureWriteProfile(Number($('swd-clock').value)||0,twoWritesSelected());
      controller.check(s);
    }
  }
  $('transfer-test').addEventListener('click',()=>action(async()=>{const p=programmer;await applyWriteProfile(p,activeSession);await p.testTransfer();status('传输自检通过；STM32 未擦写。');}));
  $('file').addEventListener('change', async event => {
    const generation=++fileGeneration,file=event.target.files[0];image=null;if(programmer)programmer.readback=null;sync();
    if (!file) { $('image').textContent = '尚未选择文件'; sync(); return; }
    try {
      if (file.size > 4 * 1024 * 1024) throw Error('固件文件超过 4MiB 解析上限');
      const target = programmer?.target || null;
      const maxBin = target ? target.flashBytes : B.MAX_FLASH;
      if (/\.bin$/i.test(file.name) && file.size > maxBin) throw Error(`BIN 超过 ${maxBin / 1024}KiB Flash 范围`);
      let parsed;
      if (/\.(hex|ihex)$/i.test(file.name)) parsed = B.parseHex(await file.text(), file.name, target);
      else if (/\.bin$/i.test(file.name)) parsed = B.parseBin(await file.arrayBuffer(), file.name, target);
      else throw Error('只接受 .bin、.hex 或 .ihex 固件文件');
      if(generation!==fileGeneration)return;image=parsed;
      const check=renderImageInfo();
      if(check.mismatch){
        log(`防烧错警告（${file.name}）：${check.reasons.join('；')}`);
        status('检测到固件与当前芯片系列不匹配，请核对后再烧录。',true);
      } else {
        log(`文件解析与向量检查通过：${file.name}，${image.count} 字节`); status('固件检查通过，可开始烧录。');
      }
    } catch (error) { if(generation!==fileGeneration)return;$('image').textContent = error.message; status(error.message, true); log(error.message); }
    sync();
  });
  $('flash').addEventListener('click', () => action(async () => {
    const s=activeSession,p=programmer,selectedImage=image;controller.check(s);
    if(!selectedImage)throw Error('请先选择并完成固件解析');
    const check=B.detectTargetMismatch(selectedImage,p.target);
    if(check.mismatch){
      const msg=`固件与当前目标芯片疑似不匹配：\n\n- ${check.reasons.join('\n- ')}\n\n确认继续烧录吗？`;
      log('防烧错拦截：'+check.reasons.join('；'));
      if(!window.confirm(msg)){ status('已取消烧录：固件与目标芯片不匹配。',true); return; }
      log('已手动确认忽略不匹配警告，继续烧录');
    }
    await applyWriteProfile(p,s);
    if((transport.fastData||twoWritesSelected())&&!transport.fastQualified){status('正在检查传输；通过后自动开始擦写…');$('stage').textContent='检查传输';await p.testTransfer();controller.check(s);}
    const task = p.flash(selectedImage, {verify:$('strict-verify').checked?'full':'auto'}); sync(); await task;
    const mode = p.metrics.verifyMode === 'crc32' ? '页 CRC32 校验' : '完整读回校验';
    status(`${mode}通过。目标保持停机，可点击“软件复位运行”。`);
  }));
  $('stop').addEventListener('click', () => { programmer?.requestStop(); $('stop').disabled = true; status('已请求停止，等待当前命令完成后停止后续擦写…'); log('用户请求停止后续擦写'); });
  $('run').addEventListener('click', () => action(async () => { await programmer.run(); status('软件复位命令成功，SWD 会话已释放。'); }));
  $('release').addEventListener('click', () => action(async () => { await programmer.release(); status('会话已释放；STM32 保持停机。'); }));
  function download(name, value, type) {
    const url = URL.createObjectURL(new Blob([value], {type})), a = document.createElement('a'); a.href = url; a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  $('save-log').addEventListener('click', () => download('CH592_LINK_BLE_log.txt', lines.join('\n'), 'text/plain;charset=utf-8'));
  $('save-readback').addEventListener('click', () => {
    if (!programmer?.readback || !image) return;
    // File starts at 0x08000000. Pages outside this session are FF placeholders,
    // explicitly identified here and in the README; never claim they were read.
    download(image.name.replace(/\.[^.]+$/, '') + '_verified_pages_08000000.bin', programmer.readback.slice(0, Math.ceil(image.max / B.PAGE) * B.PAGE), 'application/octet-stream');
    log('已导出校验页 BIN：文件从 0x08000000 起，未参与本次操作的页使用 FF 占位（未读取）');
  });
  async function checkEnvironment() {
    if (!window.isSecureContext || typeof navigator.bluetooth?.requestDevice !== 'function') {
      $('environment').textContent = 'Web Bluetooth 不可用。请在 Windows Chrome / Edge 中访问启动器提供的 http://localhost 地址。'; $('environment').classList.add('error'); $('connect').disabled = true; return;
    }
    try {
      const available = typeof navigator.bluetooth.getAvailability === 'function' ? await navigator.bluetooth.getAvailability() : null;
      $('environment').textContent = available === false ? '浏览器未发现可用蓝牙适配器。请检查蓝牙和浏览器权限后再选择设备。' : 'Web Bluetooth 可用。请保持蓝牙开启，选择运行 CH592 LINK 协议固件的设备。';
      $('environment').classList.toggle('error', available === false);
    } catch (error) { $('environment').textContent = '蓝牙能力检查失败：' + error.message; $('environment').classList.add('error'); }
    log('页面就绪（WEB 0.5.0 / 协议 v2）。默认保守发送；支持时使用目标端 1KiB 块 CRC32；完整读回可手动选择；擦写不自动重试。'); sync();
  }
  window.addEventListener?.('pagehide', () => controller.dispose());
  sync(); checkEnvironment();
})();
