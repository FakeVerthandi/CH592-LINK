/* CH592 LINK BLE protocol v1. Browser and Node, no external dependencies. */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.LinkBLE = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  const UUID = Object.freeze({service: '7e2a0001-6e7d-4b6b-9e54-2b3c59210000', rx: '7e2a0002-6e7d-4b6b-9e54-2b3c59210000', tx: '7e2a0003-6e7d-4b6b-9e54-2b3c59210000'});
  const CMD = Object.freeze({INFO: 1, ATTACH: 2, ERASE: 3, WRITE: 4, READ: 5, RUN: 6, RELEASE: 7, ABORT: 8, DIAG: 9, LINK_STATUS: 10, DISCONNECT: 11, CAPS: 12, PAGE_CRC: 13, WRITE_PROFILE: 14});
  const CAP_PAGE_CRC = 1;
  const BASE = 0x08000000, SIZE = 65536, PAGE = 1024, CHUNK = 256;
  const STATUS = ['成功', '命令或参数错误', '烧录会话被占用（请先断开 USB 调试会话）', '目标尚未连接', '目标型号不受支持', 'SWD 访问失败', 'Flash 擦写失败', '目标操作超时', '操作已中止'];
  const hex = n => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(8, '0');
  const u16 = (a, o = 0) => a[o] | (a[o + 1] << 8);
  const u32 = (a, o = 0) => (a[o] | (a[o + 1] << 8) | (a[o + 2] << 16) | (a[o + 3] << 24)) >>> 0;
  function put16(a, o, n) { a[o] = n; a[o + 1] = n >>> 8; }
  function put32(a, o, n) { a[o] = n; a[o + 1] = n >>> 8; a[o + 2] = n >>> 16; a[o + 3] = n >>> 24; }
  function bytes(v) {
    if (v instanceof Uint8Array) return v;
    if (ArrayBuffer.isView(v)) return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
    return new Uint8Array(v);
  }
  function crc32(input) {
    let c = 0xFFFFFFFF;
    for (const b of input) {
      c ^= b;
      for (let k = 0; k < 8; k++) c = (c >>> 1) ^ ((c & 1) ? 0xEDB88320 : 0);
    }
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  function pack(body) {
    body = bytes(body);
    const out = new Uint8Array(body.length + 4);
    out.set(body); put32(out, body.length, crc32(body));
    if (out.length < 5 || out.length > 274) throw Error('消息长度超出协议范围');
    return out;
  }
  function unpack(message) {
    if (message.length < 5 || message.length > 274) throw Error('消息长度错误');
    const body = message.slice(0, -4);
    if (crc32(body) !== u32(message, message.length - 4)) throw Error('消息 CRC32 校验失败');
    return body;
  }
  function fragments(seq, message, frameBytes = 20) {
    if (!seq || seq > 65535) throw Error('序号错误');
    if (![20, 139].includes(frameBytes)) throw Error('分片长度超出范围');
    const capacity = frameBytes - 6;
    const out = [];
    for (let off = 0; off < message.length;) {
      const remaining = message.length - off;
      const count = Math.min(remaining, capacity);
      const f = new Uint8Array(6 + count);
      put16(f, 0, seq); put16(f, 2, off); put16(f, 4, message.length);
      f.set(message.subarray(off, off + count), 6); out.push(f);
      off += count;
    }
    return out;
  }
  class ProtocolError extends Error {
    constructor(message, status, detail) { super(message); this.name = 'ProtocolError'; this.status = status; this.detail = detail; }
  }
  const nowMs = () => typeof performance !== 'undefined' ? performance.now() : Date.now();
  class Transport {
    constructor(rx, {timeoutMs = 20000, onFatal = () => {}, fastData = true} = {}) {
      this.rx = rx; this.timeoutMs = timeoutMs; this.onFatal = onFatal;
      this.seq = 0; this.pending = null; this.closed = false; this.active = false;
      this.supportsFastData = !!(rx.properties?.writeWithoutResponse && typeof rx.writeValueWithoutResponse === 'function');
      this.fastData = fastData && this.supportsFastData;
      this.fastQualified = false;
      this.fenceInterval = 16;
      this.writeFrameBytes = 20;
    }
    setFastData(enabled, interval = this.fenceInterval) {
      if (this.active) throw Error('请等待当前命令结束后切换传输模式');
      if (![4, 8, 16].includes(interval)) throw Error('不支持的分片确认间距');
      this.fenceInterval = interval;
      this.fastData = !!enabled && this.supportsFastData;
      this.writeFrameBytes = 20;
      this.fastQualified = false;
    }
    fail(reason) {
      if (this.closed) return;
      this.closed = true;
      if (this.pending) {
        const p = this.pending; this.pending = null; clearTimeout(p.timer); p.reject(reason instanceof Error ? reason : Error(reason));
      }
      this.onFatal(reason);
    }
    receive(value) {
      if (this.closed) return;
      try {
        const a = bytes(value), p = this.pending;
        if (!p) throw Error('收到无对应请求的通知');
        if (a.length < 7 || a.length > 20) throw Error('通知分片长度错误');
        const seq = u16(a), offset = u16(a, 2), total = u16(a, 4);
        if (seq !== p.seq || !seq || total < 10 || total > 274 || offset !== p.offset || offset + a.length - 6 > total) throw Error('通知分片序号、偏移或长度错误');
        if (!p.buffer) p.buffer = new Uint8Array(total);
        if (p.buffer.length !== total) throw Error('通知总长度发生变化');
        p.buffer.set(a.subarray(6), offset); p.offset += a.length - 6;
        if (p.offset === total) {
          const body = unpack(p.buffer);
          if (body.length < 6 || body[0] !== (p.cmd | 0x80)) throw Error('应答命令不匹配');
          this.pending = null;
          if (p.cmd === CMD.WRITE) p.receivedAt = nowMs();
          if (body[1]) p.resolve({error: new ProtocolError(`${STATUS[body[1]] || '未知设备错误'}；详情 ${hex(u32(body, 2))}`, body[1], u32(body, 2))});
          else p.resolve({value: body.slice(6)});
        }
      } catch (error) { this.fail(error); }
    }
    async request(cmd, payload = new Uint8Array()) {
      if (this.closed) throw Error('连接已失效，请重新连接并开始新会话');
      if (this.active || this.pending) throw Error('已有命令正在执行');
      if (this.seq === 65535) { this.fail(Error('会话序号已用完，请重新连接')); throw Error('会话序号已用完'); }
      const seq = ++this.seq, body = new Uint8Array(1 + payload.length); body[0] = cmd; body.set(payload, 1);
      const message = pack(body);
      const frames = fragments(seq, message, cmd === CMD.WRITE ? this.writeFrameBytes : 20);
      const transfer = {cmd, seq, total: frames.length, submitted: 0, offset: 0, mode: '', phase: '准备发送'};
      this.lastTransfer = transfer;
      this.active = true;
      let pending, timer;
      const response = new Promise((resolve, reject) => {
        pending = {seq, cmd, resolve, reject, offset: 0, buffer: null};
        this.pending = pending;
      });
      const deadline = new Promise((resolve, reject) => {
        timer = setTimeout(() => {
          const context = this.probeActive ? '传输自检超时；此请求不擦写 STM32，请重新连接' : '等待设备应答或 GATT 写入确认超时；执行结果未知，请重新连接后重新烧录';
          const error = Error(`${context}；命令 ${hex(cmd)}，序号 ${seq}，已提交 ${transfer.submitted}/${transfer.total} 片，当前偏移 ${transfer.offset}，${transfer.mode}，${transfer.phase}`);
          this.fail(error); reject(error);
        }, this.timeoutMs);
        pending.timer = timer;
      });
      const send = async () => {
        const timing = cmd === CMD.WRITE ? {commands:0, requests:0, commandMs:0, requestMs:0, tailMs:0} : null;
        if (timing) transfer.timing = timing;
        for (const [index, frame] of frames.entries()) {
          if (this.closed) throw Error('连接已失效，停止发送');
          // Bound the number of queued commands on the legacy ATT channel.
          // A periodic Write Request confirms the
          // fragment stream has reached the receiver; the final fragment also
          // uses a Request. Flash completion still needs the protocol response.
          const fence = this.writeFrameBytes > 20 || (index + 1) % this.fenceInterval === 0 || index === frames.length - 1;
          transfer.offset = u16(frame, 2); transfer.phase = '等待 GATT 返回';
          transfer.mode = cmd === CMD.WRITE && this.fastData && !fence ? 'Write Command' : this.rx.writeMode === 'legacy' ? 'writeValue（兼容）' : 'Write Request';
          const began = timing && nowMs();
          if (cmd === CMD.WRITE && this.fastData && !fence) {
            await this.rx.writeValueWithoutResponse(frame);
            timing.commandMs += nowMs() - began;
            timing.commands++;
          } else {
            await this.rx.writeValueWithResponse(frame);
            if (timing) {
              timing.requestMs += nowMs() - began;
              timing.requests++;
            }
          }
          transfer.submitted++;
        }
        transfer.phase = '等待设备完整应答';
        if (timing) transfer.sentAt = nowMs();
      };
      try {
        const [, result] = await Promise.race([Promise.all([send(), response]), deadline]);
        if (transfer.timing) transfer.timing.tailMs = Math.max(0, pending.receivedAt - transfer.sentAt);
        if (result.error) throw result.error;
        return result.value;
      }
      catch (error) {
        // An explicit MCU error has a known result. Transport errors have an
        // ambiguous result and permanently invalidate this connection.
        if (!(error instanceof ProtocolError)) this.fail(error);
        throw error;
      } finally { clearTimeout(timer); this.active = false; }
    }
  }
  function validateVectors(image) {
    for (let i = 0; i < 8; i++) if (!image.mask[i]) throw Error('镜像缺少 0x08000000 起始向量表的前 8 字节');
    const sp = u32(image.data), reset = u32(image.data, 4), pc = (reset & 0xFFFFFFFE) >>> 0;
    if ((sp & 3) || sp <= 0x20000000 || sp > 0x20005000) throw Error(`初始栈指针 ${hex(sp)} 不在 STM32F103C8T6 的 20KiB SRAM 范围`);
    if (!(reset & 1) || pc < BASE || pc >= BASE + SIZE || !image.mask[pc - BASE]) throw Error(`复位向量 ${hex(reset)} 必须为镜像内的 Thumb 地址`);
    image.sp = sp; image.reset = reset;
  }
  function makeImage(name) { return {name, data: new Uint8Array(SIZE).fill(0xFF), mask: new Uint8Array(SIZE), count: 0, pages: [], min: SIZE, max: 0}; }
  function addByte(image, address, b) {
    if (!Number.isSafeInteger(address) || address < BASE || address >= BASE + SIZE) throw Error(`文件地址 ${hex(address)} 超出 64KiB Flash 范围`);
    const off = address - BASE;
    if (image.mask[off] && image.data[off] !== b) throw Error(`HEX 重叠数据冲突：${hex(address)}`);
    if (!image.mask[off]) image.count++;
    image.mask[off] = 1; image.data[off] = b;
    image.min = Math.min(image.min, off); image.max = Math.max(image.max, off + 1);
  }
  function finishImage(image) {
    if (!image.count) throw Error('固件文件没有数据');
    validateVectors(image);
    for (let p = 0; p < SIZE; p += PAGE) if (image.mask.subarray(p, p + PAGE).some(Boolean)) image.pages.push(BASE + p);
    return image;
  }
  function parseBin(value, name = 'firmware.bin') {
    const a = bytes(value), image = makeImage(name);
    if (!a.length || a.length > SIZE) throw Error('BIN 大小必须为 1～65536 字节，基址固定 0x08000000');
    for (let i = 0; i < a.length; i++) addByte(image, BASE + i, a[i]);
    return finishImage(image);
  }
  function parseHex(text, name = 'firmware.hex') {
    if (text.length > 2 * 1024 * 1024) throw Error('HEX 文件超过 2MiB 解析上限');
    const image = makeImage(name); let base = 0, eof = false;
    const lines = text.replace(/^\uFEFF/, '').split(/\r?\n/);
    for (let lineNo = 0; lineNo < lines.length; lineNo++) {
      const line = lines[lineNo].trim(); if (!line) continue;
      if (eof) throw Error(`HEX 第 ${lineNo + 1} 行：EOF 之后仍有记录`);
      if (!/^:[0-9a-f]+$/i.test(line) || (line.length & 1) !== 1 || line.length < 11) throw Error(`HEX 第 ${lineNo + 1} 行格式错误`);
      const a = Uint8Array.from(line.slice(1).match(/../g), x => parseInt(x, 16));
      if (a.length !== a[0] + 5 || a.reduce((s, b) => (s + b) & 255, 0)) throw Error(`HEX 第 ${lineNo + 1} 行长度或校验和错误`);
      const len = a[0], addr = (a[1] << 8) | a[2], type = a[3], d = a.subarray(4, 4 + len);
      if (type === 0) {
        if (addr + len > 65536) throw Error(`HEX 第 ${lineNo + 1} 行跨越 64KiB 记录边界`);
        for (let j = 0; j < len; j++) addByte(image, base + addr + j, d[j]);
      } else if (type === 1) {
        if (len || addr) throw Error('HEX EOF 记录必须为空且地址为零');
        eof = true;
      } else if (type === 2 || type === 4) {
        if (len !== 2 || addr) throw Error('HEX 扩展地址记录格式错误');
        base = ((d[0] << 8) | d[1]) * (type === 2 ? 16 : 65536);
      } else if (type === 3 || type === 5) {
        if (len !== 4 || addr) throw Error('HEX 启动地址记录格式错误');
        // Entry metadata never overrides the actual Cortex-M reset vector.
      } else throw Error(`不支持的 HEX 记录类型 ${type}`);
    }
    if (!eof) throw Error('HEX 缺少 EOF 记录');
    return finishImage(image);
  }
  function addressPayload(address, len) {
    const p = new Uint8Array(len === undefined ? 4 : 6); put32(p, 0, address);
    if (len !== undefined) put16(p, 4, len); return p;
  }
  function decodeInfo(p) {
    if (p.length !== 16 || p[3] !== 1 || u16(p, 4) !== CHUNK || u16(p, 6) !== PAGE || u32(p, 8) !== BASE || u32(p, 12) !== SIZE) throw Error('设备协议或目标范围不匹配');
    return {version: `${p[0]}.${p[1]}.${p[2]}`, protocol: p[3]};
  }
  function decodeAttach(p) {
    if (p.length !== 10) throw Error('目标识别应答长度错误');
    const result = {dpid: u32(p), deviceid: u32(p, 4), flashKB: u16(p, 8)};
    if ((result.deviceid & 0xFFF) !== 0x410 || result.flashKB !== 64) throw Error('仅支持 Flash 容量标识为 64KiB 的 STM32F103C8T6');
    return result;
  }
  function decodeDiag(p) {
    if (p.length !== 36) throw Error('SWD 诊断长度错误');
    if (p[0] === 0) return null;
    if (p[0] !== 1) throw Error('SWD 诊断版本不支持');
    return {state: p[1], step: p[2], status: p[3], ack: p[4], port: p[5], read: p[6], register: p[7],
      turnaround: p[8], writePhase: p[9], memStage: p[10], detail: u32(p, 12), dpid: u32(p, 16),
      ctrl: u32(p, 20), csw: u32(p, 24), data: u32(p, 28), memDetail: u32(p, 32)};
  }
  function formatDiag(d) {
    if (!d) return '设备尚未保存 SWD 失败快照';
    const steps = ['未开始', '同步后稳定等待', '读取 DPID', 'ABORT 清除', '清除后状态检查', '选择 AP0',
      '请求调试电源', '等待调试电源 ACK', '写入 CSW32', '读取 CSW（posted）', '读取 CSW（RDBUFF）', '最终状态检查'];
    const statuses = ['OK', '未连接', '未实现', 'IO_ERROR', 'WAIT', 'FAULT', 'PARITY', '参数错误'];
    const acks = {1: 'OK', 2: 'WAIT', 4: 'FAULT', 7: '无有效应答'};
    const operationNames = {2: '写入 DHCSR 暂停内核', 3: '等待内核停机', 4: '读取 DBGMCU ID', 5: '读取 Flash 容量', 31: '断连后等待已接受事务完成'};
    const stage = d.state === 1 ? `连接步骤 ${d.step} / ${steps[d.step] || '未知'}` : `目标操作阶段 ${d.state} / ${operationNames[d.state] || '目标存储器访问'}`;
    return `${stage}；SWD ${statuses[d.status] || d.status}；ACK=${d.ack}(${acks[d.ack] || '协议错误'})；` +
      `${d.port ? 'AP' : 'DP'} ${d.read ? '读' : '写'}寄存器 ${hex(d.register)}；` +
      `DPID=${hex(d.dpid)} CTRL/STAT=${hex(d.ctrl)} CSW=${hex(d.csw)} DATA=${hex(d.data)}；` +
      `读 turnaround=${d.turnaround} 写 phase=${d.writePhase}；详情=${hex(d.detail)}`;
  }
  function decodeLinkStatus(p) {
    if (p.length !== 32 || p[0] !== 1) throw Error('蓝牙状态格式或版本不匹配');
    return {flags: p[1], reason: p[2], localCause: p[3], disconnects: u32(p, 4), disconnectedAt: u32(p, 8),
      advertisingStatus: p[12], terminationStatus: p[13], role: p[14], advertisingPhase: p[15], advertisingAttempts: u32(p, 16),
      inactivityMs: u32(p, 20), handle: u16(p, 24), interval: u16(p, 26), latency: u16(p, 28), timeout: u16(p, 30)};
  }
  function formatLinkStatus(d) {
    const reason = {8: '链路监督超时', 19: '对端主动断开', 22: '本机主动断开'};
    const local = ['无本地协议终止', '分片或CRC错误', '未收完命令超时', '通知发送超时', '命令执行超时', '用户请求设备主动断链'];
    const previous = d.disconnects ? `上次原因 ${hex(d.reason)}(${reason[d.reason] || 'HCI原因码'})，${local[d.localCause] || d.localCause}` : '本次上电尚无断连';
    return `连接间隔 ${(d.interval * 1.25).toFixed(2)}ms，延迟 ${d.latency}，监督超时 ${d.timeout * 10}ms；` +
      `累计断连 ${d.disconnects} 次；${previous}；广播恢复请求 ${d.advertisingAttempts} 次，最近状态 ${hex(d.advertisingStatus)}`;
  }
  class Programmer {
    constructor(transport, {log = () => {}, progress = () => {}, features = {diagnostic: true}} = {}) {
      this.transport = transport; this.log = log; this.progress = progress;
      this.features = features;
      this.attached = false; this.busy = false; this.stopRequested = false; this.readback = null;
    }
    async attach() {
      this.attached = false;
      try {
        const result = decodeAttach(await this.transport.request(CMD.ATTACH)); this.attached = true;
        this.log(`目标已停机：DP ${hex(result.dpid)}，DBGMCU ${hex(result.deviceid)}，Flash ${result.flashKB}KiB`); return result;
      } catch (error) {
        if (this.features.diagnostic && error instanceof ProtocolError && [4, 5, 7].includes(error.status) && !this.transport.closed) {
          try { error.message += '；' + await this.diagnose(); }
          catch (diagError) { this.log('详细诊断不可用：' + diagError.message); }
        }
        throw error;
      }
    }
    async diagnose() {
      try {
        const result = formatDiag(decodeDiag(await this.transport.request(CMD.DIAG)));
        this.log('SWD 诊断：' + result); return result;
      } catch (error) { if (error instanceof ProtocolError && error.status === 1) this.features.diagnostic = false; throw error; }
    }
    async linkStatus() {
      const state = decodeLinkStatus(await this.transport.request(CMD.LINK_STATUS));
      const result = formatLinkStatus(state);
      this.log('蓝牙状态：' + result); return result;
    }
    async configureWriteProfile(clockMHz, twoWrites) {
      if (![0, 2, 5].includes(clockMHz)) throw Error('不支持的 SWCLK 档位');
      const payload = twoWrites ? Uint8Array.of(clockMHz, 139) : Uint8Array.of(clockMHz);
      const reply = await this.transport.request(CMD.WRITE_PROFILE, payload);
      if (reply.length !== 1 || reply[0] < 20 || reply[0] > (twoWrites ? 139 : 64)) throw Error('设备报告的写帧长度无效');
      if (twoWrites && reply[0] < 139) {
        this.transport.fastQualified = false;
        this.transport.writeFrameBytes = 20;
        throw Error(`连接允许 ${reply[0]}B 写帧，两次确认需要 139B（MTU ≥142）。请选择 20B 模式；尚未擦写。`);
      }
      const frameBytes = twoWrites ? 139 : 20;
      if (frameBytes !== this.transport.writeFrameBytes) this.transport.fastQualified = false;
      this.transport.writeFrameBytes = frameBytes;
      this.log(`SWCLK 请求 ${clockMHz ? clockMHz + 'MHz' : '250kHz'}，目标停机寄存器读回通过；本次配置报告上限 ${reply[0]}B，本次使用 ${frameBytes}B`);
    }
    async testTransfer() {
      if (this.busy || this.transport.active) throw Error('请等待当前操作完成');
      if (!this.attached) throw Error('请先连接并识别目标');
      this.busy = true;
      this.transport.fastQualified = false;
      this.transport.probeActive = true;
      const originalTimeout = this.transport.timeoutMs;
      if (Number.isFinite(originalTimeout)) this.transport.timeoutMs = Math.min(originalTimeout, 6000);
      try {
        // In the supported firmware, address validation precedes unlock or
        // any SWD operation. Zero is outside Flash. A full-size block exercises
        // exactly the WRITE framing path and must return RES_PARAM.
        const payload = new Uint8Array(4 + CHUNK);
        for (let i = 4; i < payload.length; i++) payload[i] = (i * 29 + 0x5a) & 255;
        this.log(`开始传输自检：256B 数据，${this.writeModeDescription()}；不擦写 STM32`);
        try {
          await this.transport.request(CMD.WRITE, payload);
        } catch (error) {
          if (error instanceof ProtocolError && error.status === 1) {
            this.transport.fastQualified = !!this.transport.fastData || this.transport.writeFrameBytes > 20;
            const timing = this.transport.lastTransfer?.timing;
            if (timing) this.log(`自检计时：无应答写 ${timing.commands} 片 / ${timing.commandMs.toFixed(1)}ms，写确认 ${timing.requests} 片 / ${timing.requestMs.toFixed(1)}ms，末片提交完成后等通知 ${timing.tailMs.toFixed(1)}ms`);
            this.log('传输自检通过：完整数据块送达，设备正确拒绝越界地址；未执行擦写');
            return;
          }
          throw error;
        }
        this.transport.fail(Error('传输自检异常：设备未拒绝越界地址，请停止操作'));
        throw Error('传输自检异常：设备未拒绝越界地址');
      } finally { this.busy = false; this.transport.probeActive = false; this.transport.timeoutMs = originalTimeout; }
    }
    writeModeDescription() {
      if (this.transport.writeFrameBytes > 20) return '139B 有应答写，256B 满块两次确认';
      if (!this.transport.fastData) return '20B 保守发送，逐片写确认';
      return `混合发送，每${this.transport.fenceInterval}片及末片写确认`;
    }
    async requestLinkDisconnect() {
      if (this.busy || this.transport.active) throw Error('请先停止当前烧录并等待当前命令完成');
      await this.transport.request(CMD.DISCONNECT);
      this.attached = false;
      this.log('设备已接受断链请求，等待设备终止事件');
    }
    requestStop() { this.stopRequested = true; }
    async checkpoint() {
      if (this.stopRequested) {
        await this.transport.request(CMD.ABORT); this.attached = false;
        throw Error('已停止后续擦写；目标可能包含不完整镜像，请重新开始烧录');
      }
    }
    async supportsPageCrc() {
      if (this.pageCrcSupported !== undefined) return this.pageCrcSupported;
      try {
        const capabilities = await this.transport.request(CMD.CAPS);
        if (capabilities.length !== 1) throw Error('固件能力应答长度错误');
        this.pageCrcSupported = !!(capabilities[0] & CAP_PAGE_CRC);
      } catch (error) {
        if (!(error instanceof ProtocolError) || error.status !== 1) throw error;
        this.pageCrcSupported = false;
      }
      return this.pageCrcSupported;
    }
    async readVerifiedPage(image, page, readback, onChunk = () => {}) {
      for (let off = 0; off < PAGE; off += CHUNK) {
        await this.checkpoint();
        const address = page + off;
        const p = await this.transport.request(CMD.READ, addressPayload(address, CHUNK));
        if (p.length !== CHUNK) throw Error(`读回应答长度错误：${hex(address)}`);
        readback?.set(p, address - BASE);
        for (let j = 0; j < p.length; j++) {
          const expected = image.data[address - BASE + j];
          if (p[j] !== expected) throw Error(`读回不一致：${hex(address + j)}，期望 ${expected.toString(16).padStart(2, '0').toUpperCase()}，实际 ${p[j].toString(16).padStart(2, '0').toUpperCase()}`);
        }
        onChunk(p.length, address);
      }
    }
    async verifyPageCrc(image, page) {
      await this.checkpoint();
      const reply = await this.transport.request(CMD.PAGE_CRC, addressPayload(page));
      if (reply.length !== 4) throw Error(`页 CRC32 应答长度错误：${hex(page)}`);
      const expected = crc32(image.data.subarray(page - BASE, page - BASE + PAGE));
      const actual = u32(reply);
      if (actual === expected) return;
      this.log(`页 CRC32 不匹配：${hex(page)}，期望 ${hex(expected)}，实际 ${hex(actual)}。开始逐字节读回此页。`);
      await this.readVerifiedPage(image, page);
      throw Error(`页 CRC32 与逐字节结果不一致：${hex(page)}，校验未通过`);
    }
    async flash(image, {verify = 'full'} = {}) {
      if (this.busy) throw Error('烧录正在执行');
      if (!this.attached) throw Error('请先连接并识别目标');
      if ((this.transport.fastData || this.transport.writeFrameBytes > 20) && !this.transport.fastQualified) throw Error('本次快传尚未通过自检；请先测试传输或选择保守发送模式');
      this.busy = true; this.stopRequested = false; this.readback = null; this.metrics = null;
      let written = 0;
      const writeRanges = image.pages.map(page => {
        const pageOff = page - BASE; let first = PAGE, last = 0;
        for (let j = 0; j < PAGE; j++) if (image.mask[pageOff + j]) { first = Math.min(first, j); last = j + 1; }
        return {page, pageOff, first: first & ~1, last: (last + 1) & ~1};
      });
      const writeTotal = writeRanges.reduce((sum, r) => sum + r.last - r.first, 0);
      const writeTiming = {blocks:0, commands:0, requests:0, commandMs:0, requestMs:0, tailMs:0};
      let transmitted = 0;
      try {
        const pageCrc = verify === 'auto' && await this.supportsPageCrc();
        if (verify === 'auto' && !pageCrc) this.log('当前固件使用完整逐字节读回校验。');
        const started = nowMs();
        this.log(`开始：${image.name}，${image.count} 有效字节，擦除 ${image.pages.length} 页；页内文件空洞按 FF 处理`);
        for (let i = 0; i < image.pages.length; i++) {
          await this.checkpoint(); const address = image.pages[i];
          this.progress('擦除', i, image.pages.length, address);
          await this.transport.request(CMD.ERASE, addressPayload(address));
        }
        const afterErase = nowMs();
        for (const {page, pageOff, first, last} of writeRanges) {
          for (let off = first; off < last; off += CHUNK) {
            await this.checkpoint();
            const count = Math.min(CHUNK, last - off), address = page + off;
            const data = image.data.subarray(pageOff + off, pageOff + off + count);
            if (data.some(b => b !== 0xFF)) {
              const p = new Uint8Array(4 + count); put32(p, 0, address); p.set(data, 4);
              await this.transport.request(CMD.WRITE, p);
              const timing = this.transport.lastTransfer?.timing;
              if (timing) {
                writeTiming.blocks++;
                for (const key of ['commands','requests','commandMs','requestMs','tailMs']) writeTiming[key] += timing[key];
              }
              transmitted += count;
            }
            written += count; this.progress('写入', written, writeTotal, address);
          }
        }
        const afterWrite = nowMs();
        if (writeTiming.blocks) {
          const t = writeTiming, seconds = ms => (ms / 1000).toFixed(3);
          this.log(`WRITE 浏览器计时：${t.blocks} 块，${transmitted}B，无应答写 ${t.commands} 片 / ${seconds(t.commandMs)}s，写确认 ${t.requests} 片 / ${seconds(t.requestMs)}s，末片提交完成后等通知 ${seconds(t.tailMs)}s，写阶段其他 ${seconds(afterWrite - afterErase - t.commandMs - t.requestMs - t.tailMs)}s`);
        }
        const verifiedBytes = image.pages.length * PAGE;
        const readback = pageCrc ? null : new Uint8Array(SIZE).fill(0xFF);
        let done = 0;
        this.log(pageCrc ? '写入完成，开始由 STM32 计算全部受影响 Flash 页的 CRC32'
                         : '写入完成，开始独立读回全部受影响 Flash 页');
        for (const page of image.pages) {
          if (pageCrc) {
            await this.verifyPageCrc(image, page);
            done += PAGE; this.progress('页 CRC32 校验', done, verifiedBytes, page);
          } else {
            await this.readVerifiedPage(image, page, readback, (count, address) => {
              done += count; this.progress('读回校验', done, verifiedBytes, address);
            });
          }
        }
        await this.checkpoint();
        this.readback = readback;
        const finished = nowMs();
        this.metrics = {eraseMs: afterErase - started, writeMs: afterWrite - afterErase,
          verifyMs: finished - afterWrite, totalMs: finished - started, fileBytes: image.count,
          transmittedBytes: transmitted, verifiedBytes, verifyMode: pageCrc ? 'crc32' : 'full',
          verifyPayloadBytes: pageCrc ? image.pages.length * 4 : verifiedBytes, fastData: !!this.transport.fastData};
        this.progress('校验通过', 1, 1, BASE + image.max - 1);
        this.log(pageCrc
          ? `页 CRC32 校验通过：${image.pages.length} 页，${verifiedBytes} 个页字节；回传 ${this.metrics.verifyPayloadBytes}B CRC 数据；目标保持停机，可点击复位运行`
          : `完整读回校验通过：${image.count} 个文件字节，${verifiedBytes} 个页字节；目标保持停机，可点击复位运行`);
        const m = this.metrics, rate = (n, ms) => (n * 1000 / Math.max(ms, 0.001)).toFixed(1);
        const verification = pageCrc ? '页 CRC32 校验' : '读回';
        this.log(`阶段耗时：擦除 ${(m.eraseMs / 1000).toFixed(3)}s，写入 ${(m.writeMs / 1000).toFixed(3)}s，${verification} ${(m.verifyMs / 1000).toFixed(3)}s，总计 ${(m.totalMs / 1000).toFixed(3)}s`);
        this.log(`有效速度：写入 ${rate(transmitted, m.writeMs)}B/s，${verification} ${rate(verifiedBytes, m.verifyMs)}B/s，全流程按文件 ${rate(image.count, m.totalMs)}B/s；数据分片 ${this.writeModeDescription()}`);
        return readback;
      } catch (error) {
        this.log(`失败：${error.message}`);
        // Do not schedule additional Flash operations after any failure.
        this.attached = false;
        if (!this.transport.closed && !(error.message.startsWith('已停止'))) {
          try { await this.transport.request(CMD.ABORT); } catch (cleanup) { this.log(`清理应答：${cleanup.message}`); }
        }
        throw error;
      } finally { this.busy = false; }
    }
    async run() { if (this.busy || !this.attached) throw Error('请先完成目标连接且等待当前操作结束'); await this.transport.request(CMD.RUN); this.attached = false; this.log('已发出 Cortex-M SYSRESETREQ 软件复位；NRST 未接，不使用硬件复位'); }
    async release() { if (this.busy) throw Error('请先等待当前操作停止'); await this.transport.request(CMD.RELEASE); this.attached = false; this.log('已释放 SWD 会话，目标保持停机'); }
  }
  return {UUID, CMD, BASE, SIZE, PAGE, CHUNK, hex, u16, u32, put16, put32, crc32, pack, unpack, fragments, ProtocolError, Transport, parseBin, parseHex, decodeInfo, decodeAttach, decodeDiag, formatDiag, decodeLinkStatus, formatLinkStatus, addressPayload, Programmer};
});
