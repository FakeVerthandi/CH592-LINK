"""Build CH592-LINK firmware for the dual USB/BLE probe."""
from pathlib import Path
import subprocess, os, json, datetime, re, shutil, sys
from build_f103_loader import build_f103_loader
sys.stdout.reconfigure(encoding='utf-8')

ROOT=Path(__file__).resolve().parents[1]
compiler = os.environ.get('CH592_RISCV_BIN')
if not compiler and not shutil.which('riscv-none-embed-gcc'):
    raise SystemExit('Set CH592_RISCV_BIN to the WCH RISC-V toolchain bin directory.')
BIN = Path(compiler) if compiler else Path(shutil.which('riscv-none-embed-gcc')).parent
OUT=ROOT/'obj'/'ble_build'
OUT.mkdir(parents=True,exist_ok=True)
loader_bytes=build_f103_loader(ROOT,OUT/'thumb')
sources=['src/Main.c','src/link_usb.c','src/link_usb_cherry.c','src/cmsis_dap.c',
         'src/target_arm_swd.c','src/link_board.c','src/link_protocol.c','src/link_target.c',
         'src/link_ble.c','src/link_ble_programmer.c','src/link_session.c','src/link_f103_loader.c',
         'third_party/cherryusb/core/usbd_core.c',
         'third_party/cherryusb/port/ch32/ch58x/usb_ch58x_dc_usbfs.c',
         'ble_sdk/HAL/MCU.c','ble_sdk/HAL/RTC.c','ble_sdk/LIB/ble_task_scheduler.S',
         'ble_sdk/Startup/startup_CH592.S','ble_sdk/RVMSIS/core_riscv.c']
sources += ['ble_sdk/StdPeriphDriver/'+p.name for p in (ROOT/'ble_sdk/StdPeriphDriver').glob('*.c')]
includes=['src','ble_sdk/StdPeriphDriver/inc','ble_sdk/RVMSIS','ble_sdk/LIB','ble_sdk/HAL/include',
          'third_party/cherryusb','third_party/cherryusb/common','third_party/cherryusb/core',
          'third_party/cherryusb/port/ch32/ch58x']
flags=['-march=rv32imac','-mabi=ilp32','-mcmodel=medany','-msmall-data-limit=8','-mno-save-restore',
       '-Os','-g','-fsigned-char','-ffunction-sections','-fdata-sections','-fno-common','-std=gnu99',
       '-DDEBUG=1','-DLINK_USB_USE_CHERRYUSB=1','-DLINK_BLE_ENABLE=1','-DLINK_BLE_SRAM_WRITE=1',
       '-DLINK_BLE_SWD_WORD_IO=1','-DLINK_USB_DRW_WORD_IO=1','-DLINK_S9_FAST_CLOCK=1',
       '-DLINK_BLE_TWO_WRITES=1','-DBLE_BUFF_MAX_LEN=146',
       '-DLINK_USB_PROFILE_TIMING=0','-DHAL_KEY=0','-DHAL_LED=0','-DHAL_SLEEP=0',
       '-DBLE_MAC=0','-DBLE_SNV=0','-DBLE_TX_NUM_EVENT=4','-DCENTRAL_MAX_CONNECTION=0','-DPERIPHERAL_MAX_CONNECTION=1']
flags += ['-I'+str(ROOT/p) for p in includes]
flags += ['-I'+str(OUT/'thumb')]
objects=[]
log=[]
env=os.environ.copy(); env['PATH']=str(BIN)+os.pathsep+env.get('PATH','')
def run(args):
    result=subprocess.run([str(x) for x in args],cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    output=result.stdout.decode('utf-8',errors='replace')
    log.append(output)
    (OUT/'build.log').write_text(''.join(log),encoding='utf-8')
    if result.returncode:
        print(output); raise SystemExit(result.returncode)
    return output
for source in sources:
    obj=OUT/(source.replace('/','_').replace('.c','.o').replace('.S','.o'))
    run([BIN/'riscv-none-embed-gcc.exe',*flags,'-fstack-usage','-c',ROOT/source,'-o',obj])
    objects.append(obj)
name='CH592_LINK_V1_0.3.17-s10-ble'
elf=OUT/(name+'.elf'); hexfile=OUT/(name+'.hex'); mapfile=OUT/(name+'.map')
(OUT/'objects.rsp').write_text('\n'.join('"'+p.relative_to(ROOT).as_posix()+'"' for p in objects),encoding='ascii')
run([BIN/'riscv-none-embed-gcc.exe',*flags,'-T',ROOT/'Ld/Link.ld','-nostartfiles',
     '-Wl,--gc-sections','-Wl,--print-memory-usage','-Wl,--defsym,_heap_end=_ebss+512',
     '-Wl,-Map,'+str(mapfile),'--specs=nano.specs','--specs=nosys.specs','-o',elf,
     '@'+str(OUT/'objects.rsp'),'-L'+str(ROOT/'ble_sdk/StdPeriphDriver'),'-L'+str(ROOT/'ble_sdk/LIB'),
     '-lISP592','-lCH59xBLE','-lm'])
run([BIN/'riscv-none-embed-objcopy.exe','-O','ihex',elf,hexfile])
sections=run([BIN/'riscv-none-embed-size.exe','-A',elf])
symbols=run([BIN/'riscv-none-embed-nm.exe','-n',elf])
(OUT/'symbols.txt').write_text(symbols,encoding='utf-8')
match=re.search(r'^([0-9a-f]+)\s+\w\s+_ebss\s*$',symbols,re.M)
ebss=int(match[1],16) if match else None
report={'built':datetime.datetime.now().astimezone().isoformat(),
        'sections':sections,'ebss':ebss,'stack_headroom_bytes':0x20006800-ebss if ebss else None,
        'libc_heap_limit_bytes':512,'BLE_pool_bytes':6144,'physical_tests':'pending',
        'BLE_TX_NUM_EVENT':4,'f103_thumb_bytes':loader_bytes,'ble_swd_word_io':True,
        'f103_page_crc':True,'sram_burst_upload':True,'usb_drw_word_io':True,
        'swd_requested_fast_hz':[2000000,5000000],'ble_max_rx_frame':139}
assert report['stack_headroom_bytes']-512>=4096,'Less than 4KiB reserved for stack.'
report['warning_count']=''.join(log).count('warning:')
report['stack_after_heap_reservation_bytes']=report['stack_headroom_bytes']-512
report['compiler']=run([BIN/'riscv-none-embed-gcc.exe','--version']).splitlines()[0]

(OUT/'build_report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(sections)
print('Firmware:', hexfile)
