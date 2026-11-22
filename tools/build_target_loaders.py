"""Build Cortex-M Thumb target loaders (F1/F3 and F4) and emit CH592 header."""
from pathlib import Path
import os
import shutil
import struct
import subprocess

BASE = 0x20000000
MAX_CODE = 0x600
STACK = 0x20000C00


def _find_thumb_toolchain():
    env_dir = os.environ.get('CH592_THUMB_TOOLS')
    candidates = []
    if env_dir:
        candidates.append(Path(env_dir))
    riscv_dir = os.environ.get('CH592_RISCV_BIN')
    if riscv_dir:
        sibling = Path(riscv_dir).resolve().parent.parent / 'arm-none-eabi-gcc' / 'bin'
        if sibling.is_dir():
            candidates.append(sibling)

    for d in candidates:
        if (d / 'clang.exe').exists() and (d / 'lld.exe').exists():
            return ('clang', d)
        if (d / 'clang').exists() and (d / 'lld').exists():
            return ('clang', d)
        if (d / 'arm-none-eabi-gcc.exe').exists() and (d / 'arm-none-eabi-ld.exe').exists():
            return ('gcc', d)
        if (d / 'arm-none-eabi-gcc').exists() and (d / 'arm-none-eabi-ld').exists():
            return ('gcc', d)

    clang = shutil.which('clang')
    lld = shutil.which('lld')
    if clang and lld:
        return ('clang', Path(clang).parent)
    arm_gcc = shutil.which('arm-none-eabi-gcc')
    arm_ld = shutil.which('arm-none-eabi-ld')
    if arm_gcc and arm_ld:
        return ('gcc', Path(arm_gcc).parent)

    raise SystemExit(
        'Set CH592_THUMB_TOOLS to a directory containing clang+lld or arm-none-eabi-gcc.'
    )


def _exe(directory, name):
    p = directory / (name + '.exe')
    return p if p.exists() else directory / name


def _compile_one(kind, tools_dir, root, out, name, source_rel, lds_rel):
    source = root / source_rel
    lds = root / lds_rel
    obj, elf, binary = (out / (name + suffix) for suffix in ('.o', '.elf', '.bin'))

    common_flags = [
        '-mcpu=cortex-m3', '-mthumb', '-Os', '-std=c99',
        '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
        '-fstack-usage', '-fno-unwind-tables', '-Wall', '-Wextra', '-Werror',
        '-I', str(root / 'src'),
    ]

    if kind == 'clang':
        subprocess.run(
            [str(_exe(tools_dir, 'clang')), '--target=arm-none-eabi', *common_flags,
             '-c', str(source), '-o', str(obj)],
            check=True,
        )
        link = [str(_exe(tools_dir, 'lld')), '-flavor', 'gnu', '-m', 'armelf',
                '-T', str(lds), str(obj)]
        subprocess.run([*link, '-o', str(elf)], check=True)
        subprocess.run([*link, '--oformat=binary', '-o', str(binary)], check=True)
    else:
        subprocess.run(
            [str(_exe(tools_dir, 'arm-none-eabi-gcc')), *common_flags,
             '-c', str(source), '-o', str(obj)],
            check=True,
        )
        subprocess.run(
            [str(_exe(tools_dir, 'arm-none-eabi-ld')), '-T', str(lds),
             str(obj), '-o', str(elf)],
            check=True,
        )
        subprocess.run(
            [str(_exe(tools_dir, 'arm-none-eabi-objcopy')), '-O', 'binary',
             str(elf), str(binary)],
            check=True,
        )

    image = binary.read_bytes()
    if not 68 < len(image) <= MAX_CODE or len(image) % 4:
        raise ValueError(f'Unexpected {name} loader size: {len(image)} bytes (max {MAX_CODE})')

    words = struct.unpack(f'<{len(image) // 4}I', image)
    msp, entry, exception = words[:3]
    stop = words[16]
    if (msp != STACK or not (entry & 1) or not (exception & 1) or (stop & 1) or
            not all(BASE + 68 <= addr < BASE + len(image) for addr in (entry, exception, stop)) or
            image[stop - BASE:stop - BASE + 2] != b'\x00\xbe'):
        raise ValueError(
            f'Invalid {name} vectors, entry point or completion breakpoint: '
            f'msp={msp:#x} entry={entry:#x} stop={stop:#x}'
        )
    return {
        'name': name,
        'bytes': len(image),
        'entry': entry & ~1,
        'stop': stop,
        'words': words,
    }


def build_target_loaders(root, out):
    root, out = Path(root).resolve(), Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    kind, tools_dir = _find_thumb_toolchain()

    specs = [
        ('f1_f3', 'target_loader/stm32_f1_f3.c', 'target_loader/stm32_common.ld'),
        ('f4', 'target_loader/stm32_f4.c', 'target_loader/stm32_common.ld'),
    ]

    lines = [
        '/* Generated Cortex-M Thumb target loaders (F1/F3 and F4). */',
        '#ifndef TARGET_LOADER_IMAGES_H',
        '#define TARGET_LOADER_IMAGES_H',
        '#include <stdint.h>',
        '',
    ]
    sizes = {}
    for name, src_rel, lds_rel in specs:
        info = _compile_one(kind, tools_dir, root, out, name, src_rel, lds_rel)
        sizes[name] = info['bytes']
        upper = name.upper()
        rows = [
            '    ' + ', '.join(f'0x{w:08x}u' for w in info['words'][i:i + 6]) + ','
            for i in range(0, len(info['words']), 6)
        ]
        lines.append(f'#define {upper}_LOADER_ENTRY 0x{info["entry"]:08x}u')
        lines.append(f'#define {upper}_LOADER_STOP  0x{info["stop"]:08x}u')
        lines.append(f'static const uint32_t {name}_loader_image[] = {{')
        lines.extend(rows)
        lines.append('};')
        lines.append('')

    lines.append('#endif')
    (out / 'target_loader_images.h').write_text('\n'.join(lines) + '\n', encoding='ascii')
    return sizes


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1]
    res = build_target_loaders(root, root / 'obj' / 'ble_build' / 'thumb')
    print('Built target loaders:', res)
