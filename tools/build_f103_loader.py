"""Build the F103 Thumb payload and emit the CH592 include from its exact bytes."""
from pathlib import Path
import os
import shutil
import struct
import subprocess

compiler = os.environ.get('CH592_THUMB_TOOLS')
if not compiler and not shutil.which('clang'):
    raise SystemExit('Set CH592_THUMB_TOOLS to a directory containing clang.exe and lld.exe.')
THUMB_TOOLS = Path(compiler) if compiler else Path(shutil.which('clang')).parent
BASE = 0x20000000
STACK = 0x20000800


def build_f103_loader(root, out):
    root, out = Path(root).resolve(), Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = root / 'target_loader/stm32f103.c'
    lds = root / 'target_loader/stm32f103.ld'
    obj, elf, binary = (out / ('f103_loader' + suffix) for suffix in ('.o', '.elf', '.bin'))
    subprocess.run([THUMB_TOOLS/'clang.exe', '--target=arm-none-eabi', '-mcpu=cortex-m3',
                    '-mthumb', '-Os', '-std=c99', '-ffreestanding', '-fno-builtin',
                    '-fno-stack-protector', '-fstack-usage', '-fno-unwind-tables', '-Wall', '-Wextra', '-Werror',
                    '-I', root/'src', '-c', source, '-o', obj], check=True)
    link = [THUMB_TOOLS/'lld.exe', '-flavor', 'gnu', '-m', 'armelf', '-T', lds, obj]
    subprocess.run([*link, '-o', elf], check=True)
    subprocess.run([*link, '--oformat=binary', '-o', binary], check=True)
    image = binary.read_bytes()
    if not 68 < len(image) <= 0x400 or len(image) % 4:
        raise ValueError(f'Unexpected F103 loader size: {len(image)} bytes')
    words = struct.unpack(f'<{len(image) // 4}I', image)
    msp, entry, exception = words[:3]
    stop = words[16]
    if (msp != STACK or not entry & 1 or not exception & 1 or stop & 1 or
            not all(BASE + 68 <= addr < BASE + len(image) for addr in (entry, exception, stop)) or
            image[stop-BASE:stop-BASE+2] != b'\x00\xbe'):
        raise ValueError('Invalid F103 vectors, entry point or completion breakpoint')
    rows = ['    ' + ', '.join(f'0x{w:08x}u' for w in words[i:i+6]) + ','
            for i in range(0, len(words), 6)]
    (out / 'f103_loader_image.h').write_text(
        f'/* Generated from stm32f103.c for Cortex-M3 Thumb. */\n'
        f'#define F103_LOADER_ENTRY 0x{entry & ~1:08x}u\n'
        f'#define F103_LOADER_STOP 0x{stop:08x}u\n'
        'static const uint32_t f103_loader_image[] = {\n' + '\n'.join(rows) + '\n};\n',
        encoding='ascii')
    return len(image)
