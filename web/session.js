/* Web Bluetooth lifecycle, independent of MCU and programming protocol.
 * Cancellation invalidates this page immediately; native promises remain
 * tracked until they settle. A new session must not overlap that drain.
 * onClosed means local invalidation, not confirmation of radio termination.
 * Each new connection discovers fresh attributes; closed sessions discard
 * native service/characteristic references even when readback data is kept.
 */
(function(root,factory){const api=factory();if(typeof module==='object'&&module.exports)module.exports=api;else root.LinkBLESession=api;})(typeof globalThis!=='undefined'?globalThis:this,function(){
  'use strict';
  class SessionError extends Error {
    constructor(code,message,cause){super(message);this.name='SessionError';this.code=code;this.cause=cause;}
  }
  const deferred=()=>{let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});promise.catch(()=>{});return {promise,resolve,reject};};
  const uuid=x=>String(x||'').toLowerCase();
  function classify(error){
    if(error instanceof SessionError)return error;
    const message=error?.message||String(error),name=error?.name||'';
    if(/access.?denied|权限|拒绝访问|sharing.?violation/i.test(message)||name==='SecurityError')return new SessionError('ACCESS','蓝牙服务访问被拒绝；请先断开其他占用设备的工具，再重新连接。原始错误：'+message,error);
    if(name==='NotAllowedError')return new SessionError('PERMISSION','未获得蓝牙访问权限，请重新选择设备并允许访问。'+message,error);
    if(name==='NotSupportedError' && /GATT operation failed for unknown reason/i.test(message))return new SessionError('GATT',message,error);
    if(name==='NotSupportedError')return new SessionError('UNSUPPORTED','当前浏览器或设备缺少所需蓝牙能力：'+message,error);
    return new SessionError('GATT',message,error);
  }
  function versionAtLeast(version,minimum){const parts=String(version||'').split('.').map(Number);if(parts.length!==3||parts.some(n=>!Number.isInteger(n)))return false;for(let i=0;i<3;i++){if(parts[i]!==minimum[i])return parts[i]>minimum[i];}return true;}
  function protocolFeatures(info){return {diagnostic:info?.protocol===1&&versionAtLeast(info.version,[0,3,1]),linkStatus:info?.protocol===1&&versionAtLeast(info.version,[0,3,4]),deviceDisconnect:info?.protocol===1&&versionAtLeast(info.version,[0,3,6])};}
  class Controller {
    constructor({uuids,locks=null,onState=()=>{},onClosed=()=>{},onNotification=()=>{},onDeviceDisconnect=()=>{},log=()=>{},timers={setTimeout:(...args)=>setTimeout(...args),clearTimeout:id=>clearTimeout(id)},timeouts={}}={}){
      this.uuids=uuids;this.locks=locks;this.onState=onState;this.onClosed=onClosed;this.onNotification=onNotification;this.onDeviceDisconnect=onDeviceDisconnect;this.log=log;this.timers=timers;
      this.timeouts={connect:20000,service:20000,characteristic:15000,subscribe:15000,cleanup:2000,disconnectTail:500,...timeouts};
      this.current=null;this.retired=new Set();this.nextId=0;this.state='idle';
    }
    snapshot(){return {state:this.state,sessionId:this.current?.id||null,pending:[...this.retired].flatMap(s=>[...s.pending].map(p=>p.label)),canOpen:!this.current&&!this.retired.size};}
    emit(state=this.state){this.state=state;this.onState(this.snapshot());}
    isCurrent(s){return !!s?.valid&&this.current===s;}
    check(s){if(!this.isCurrent(s))throw s?.reason||new SessionError('CANCELLED','蓝牙会话已结束');}
    _disconnect(s){
      if(!s.touchedGatt)return;
      const gatt=s.device?.gatt;if(!gatt)return;
      if(gatt.connected&&!s.tailPending){
        s.tailPending=true;
        s.tailHandler=()=>{if(!gatt.connected)this._endTail(s);};
        s.device.addEventListener('gattserverdisconnected',s.tailHandler);
        s.tailTimer=this.timers.setTimeout(()=>this._endTail(s),this.timeouts.disconnectTail);
      }
      try{gatt.disconnect();}catch(_){this._endTail(s);}
    }
    _endTail(s){if(!s.tailPending)return;s.tailPending=false;this.timers.clearTimeout(s.tailTimer);s.device.removeEventListener('gattserverdisconnected',s.tailHandler);s.tailHandler=null;this._finish(s);}
    _finish(s){
      if(s.valid||s.pending.size||s.cleanupPending||s.tailPending)return;
      s.server=s.service=s.rawRx=s.tx=null;s.rx=null;s.notifyHandler=null;s.disconnectHandler=null;
      s.releaseLock?.();s.releaseLock=null;this.retired.delete(s);s.done.resolve();
      if(!this.current)this.emit(this.retired.size?'draining':'idle');
    }
    _late(s,label,value){
      if(label==='订阅设备应答'&&s.device.gatt.connected&&typeof value?.stopNotifications==='function'){
        const work=Promise.resolve().then(()=>value.stopNotifications()).finally(()=>this._disconnect(s));
        this._track(s,'清理迟到的通知订阅',work,true).catch(()=>{});
      }else this._disconnect(s);
    }
    _track(s,label,nativePromise,cleanup=false){
      const item={label};s.pending.add(item);
      if(!s.valid)this.emit('draining');
      const observed=Promise.resolve(nativePromise).then(value=>{
        if(!cleanup&&!this.isCurrent(s)){this._late(s,label,value);throw s.reason||new SessionError('CANCELLED','旧会话结果已丢弃');}return value;
      },error=>{if(!cleanup&&!s.valid)this._disconnect(s);throw error;}).finally(()=>{s.pending.delete(item);this._finish(s);if(!s.valid&&s.pending.size)this.emit('draining');});
      observed.catch(()=>{});return observed;
    }
    async _race(s,label,promise,milliseconds){
      let timer;const choices=[promise,s.cancel.promise];
      if(milliseconds)choices.push(new Promise((_,reject)=>{timer=this.timers.setTimeout(()=>reject(new SessionError('TIMEOUT',label+'超时；已停止本页操作，正在等待系统释放旧蓝牙请求')),milliseconds);}));
      try{const value=await Promise.race(choices);this.check(s);return value;}
      catch(error){if(error instanceof SessionError&&error.code==='TIMEOUT')this.close(s,error);throw error;}
      finally{this.timers.clearTimeout(timer);}
    }
    native(s,label,operation,milliseconds=0){this.check(s);const work=Promise.resolve().then(()=>{this.check(s);return operation();});return this._race(s,label,this._track(s,label,work),milliseconds);}
    stage(s,label,operation,milliseconds=25000){this.check(s);this.log('连接阶段：'+label);let work;try{work=operation();}catch(e){work=Promise.reject(e);}return this._race(s,label,Promise.resolve(work),milliseconds);}
    async _lease(s){
      if(!this.locks?.request||!s.device.id){s.capabilities.pageLock=false;return;}
      const acquired=deferred(),released=deferred();let claimed=false,task;
      const failed=error=>{if(!claimed&&this.isCurrent(s)&&['NotSupportedError','SecurityError'].includes(error?.name)){
        s.capabilities.pageLock=false;this.log('浏览器未提供可用的页面互斥锁，请只保留一个工具页面连接设备。');acquired.resolve();
      }else acquired.reject(classify(error));};
      try{task=this.locks.request('ch592-link-ble:'+s.device.id,{mode:'exclusive',ifAvailable:true},async lock=>{
        if(!lock){acquired.reject(new SessionError('IN_USE','该设备正在被另一个工具页面使用，请先在该页面断开连接'));return;}
        if(!s.valid){released.resolve();acquired.reject(s.reason);return;}
        claimed=true;s.releaseLock=()=>released.resolve();s.capabilities.pageLock=true;acquired.resolve();await released.promise;
      });}catch(error){failed(error);}
      Promise.resolve(task).catch(failed);
      await this._race(s,'获取页面会话锁',acquired.promise,5000);
    }
    _canFallback(s,error){return this.isCurrent(s)&&s.device.gatt.connected&&!s.pending.size&&!['ACCESS','PERMISSION','TIMEOUT','CANCELLED'].includes(classify(error).code);}
    async _service(s){
      const server=s.server;let first;
      if(typeof server.getPrimaryServices==='function'){
        try{
          const list=await this.native(s,'发现烧录服务',()=>server.getPrimaryServices(),this.timeouts.service);
          const found=list.filter(x=>uuid(x.uuid)===uuid(this.uuids.service));
          if(found.length>1)throw new SessionError('AMBIGUOUS','发现多个同UUID烧录服务，无法安全选择');
          if(found.length===1){s.capabilities.serviceDiscovery='enumerate';return found[0];}
          first=new SessionError('MISSING','未在服务列表中找到烧录服务');
        }catch(e){first=e;if(!this._canFallback(s,e)||e.code==='AMBIGUOUS')throw e;}
      }
      if(typeof server.getPrimaryService!=='function')throw first||new SessionError('UNSUPPORTED','浏览器不支持GATT服务发现');
      this.log('服务发现使用兼容查询路径');
      const service=await this.native(s,'兼容查询烧录服务',()=>server.getPrimaryService(this.uuids.service),this.timeouts.service);
      s.capabilities.serviceDiscovery='specific';return service;
    }
    async _characteristics(s){
      let pair,first;
      if(typeof s.service.getCharacteristics==='function'){
        try{
          const list=await this.native(s,'获取烧录特征',()=>s.service.getCharacteristics(),this.timeouts.characteristic);
          const rx=list.filter(x=>uuid(x.uuid)===uuid(this.uuids.rx)),tx=list.filter(x=>uuid(x.uuid)===uuid(this.uuids.tx));
          if(rx.length>1||tx.length>1)throw new SessionError('AMBIGUOUS','烧录特征重复，无法安全选择');
          if(rx.length===1&&tx.length===1){pair=[rx[0],tx[0]];s.capabilities.characteristicDiscovery='enumerate';}
          else first=new SessionError('MISSING','缺少接收或通知特征');
        }catch(e){first=e;if(!this._canFallback(s,e)||e.code==='AMBIGUOUS')throw e;}
      }
      if(!pair){
        if(typeof s.service.getCharacteristic!=='function')throw first||new SessionError('UNSUPPORTED','浏览器不支持GATT特征发现');
        pair=[await this.native(s,'获取接收特征',()=>s.service.getCharacteristic(this.uuids.rx),this.timeouts.characteristic),
              await this.native(s,'获取通知特征',()=>s.service.getCharacteristic(this.uuids.tx),this.timeouts.characteristic)];
        s.capabilities.characteristicDiscovery='specific';
      }
      [s.rawRx,s.tx]=pair;const props=s.rawRx.properties||{},notify=s.tx.properties||{};
      let writeName;
      if(typeof s.rawRx.writeValueWithResponse==='function'&&props.write!==false)writeName='writeValueWithResponse';
      else if(typeof s.rawRx.writeValue==='function'&&props.write===true)writeName='writeValue';
      else throw new SessionError('UNSUPPORTED','接收特征不支持写确认，无法安全发送控制命令');
      if(typeof s.tx.startNotifications!=='function'||(notify.notify===false&&notify.indicate!==true))throw new SessionError('UNSUPPORTED','设备没有可用的通知或指示通道');
      s.capabilities.writeAPI=writeName;s.capabilities.fastWrite=!!(props.writeWithoutResponse&&typeof s.rawRx.writeValueWithoutResponse==='function');
      s.capabilities.notifyMode=notify.notify?'notify':notify.indicate?'indicate':'unspecified';
      const copy=value=>new Uint8Array(ArrayBuffer.isView(value)?new Uint8Array(value.buffer,value.byteOffset,value.byteLength):value);
      s.rx={properties:{write:true,writeWithoutResponse:s.capabilities.fastWrite},writeMode:writeName==='writeValue'?'legacy':'request',
        writeValueWithResponse:value=>{const data=copy(value);return this.native(s,'GATT 写入确认',()=>s.rawRx[writeName](data));},
        writeValueWithoutResponse:value=>{if(!s.capabilities.fastWrite)throw new SessionError('UNSUPPORTED','无响应写不可用');const data=copy(value);return this.native(s,'GATT 无响应写',()=>s.rawRx.writeValueWithoutResponse(data));}};
    }
    async open(device){
      if(this.current||this.retired.size)throw new SessionError('RECOVERY','旧蓝牙会话尚未释放，请等待系统完成清理后再连接');
      if(!device?.gatt?.connect)throw new SessionError('UNSUPPORTED','所选设备不支持Web Bluetooth GATT');
      const s={id:++this.nextId,device,valid:true,pending:new Set(),cancel:deferred(),done:deferred(),capabilities:{},expectedDisconnect:false};
      this.current=s;this.emit('connecting');
      s.disconnectHandler=()=>{if(!this.isCurrent(s)||device.gatt.connected)return;if(!s.established)return;
        if(s.expectedDisconnect)this.onDeviceDisconnect(s);else this.close(s,new SessionError('DISCONNECTED','蓝牙连接已断开，未重放擦写命令'));};
      device.addEventListener('gattserverdisconnected',s.disconnectHandler);
      try{
        await this._lease(s);
        this.log('连接阶段：建立 GATT 连接');s.touchedGatt=true;s.server=await this.native(s,'建立 GATT 连接',()=>device.gatt.connect(),this.timeouts.connect);
        if(!device.gatt.connected)throw new SessionError('DISCONNECTED','GATT连接未保持在线');s.established=true;
        this.emit('discovering');this.log('连接阶段：发现烧录服务');s.service=await this._service(s);
        await this._characteristics(s);
        s.notifyHandler=event=>{if(this.isCurrent(s))this.onNotification(s,event.target.value);};
        s.tx.addEventListener('characteristicvaluechanged',s.notifyHandler);
        this.log('连接阶段：订阅设备应答');await this.native(s,'订阅设备应答',()=>s.tx.startNotifications(),this.timeouts.subscribe);s.subscribed=true;
        this.emit('ready');return s;
      }catch(error){this.close(s,classify(error));throw classify(error);}
    }
    close(s=this.current,error=new SessionError('CANCELLED','已释放网页蓝牙会话'),{immediate=false}={}){
      if(!s)return Promise.resolve();if(!s.valid)return s.done.promise;
      s.valid=false;s.reason=error;s.cancel.reject(error);if(this.current===s)this.current=null;this.retired.add(s);
      s.device.removeEventListener('gattserverdisconnected',s.disconnectHandler);
      if(s.tx&&s.notifyHandler)s.tx.removeEventListener('characteristicvaluechanged',s.notifyHandler);
      this.emit(s.pending.size?'draining':'closing');this.onClosed(s,error);
      if(!immediate&&!s.pending.size&&s.subscribed&&s.device.gatt.connected&&typeof s.tx?.stopNotifications==='function'){
        s.cleanupPending=true;const native=this._track(s,'取消通知订阅',Promise.resolve().then(()=>s.tx.stopNotifications()),true);
        const timer=this.timers.setTimeout(()=>this._disconnect(s),this.timeouts.cleanup);
        native.catch(()=>{}).finally(()=>{this.timers.clearTimeout(timer);s.cleanupPending=false;this._disconnect(s);this._finish(s);});
      }else this._disconnect(s);
      this._finish(s);return s.done.promise;
    }
    dispose(){if(this.current)this.close(this.current,new SessionError('PAGE_CLOSED','工具页面已关闭'),{immediate:true});for(const s of this.retired)this._disconnect(s);}
  }
  return {Controller,SessionError,classify,protocolFeatures,versionAtLeast};
});
