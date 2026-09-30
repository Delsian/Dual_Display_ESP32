"""Patch the SDK's precompiled app descriptor before PlatformIO creates firmware.bin."""
import re
import struct
from pathlib import Path


def embed(elf_path, version):
    parts = version.split('.')
    if len(parts) != 3 or any(not re.fullmatch(r'0|[1-9][0-9]*', p) or int(p) > 0xffffffff for p in parts):
        raise ValueError('FIRMWARE_VERSION must be canonical major.minor.patch')
    if len(version) >= 32:
        raise ValueError('FIRMWARE_VERSION must fit the 32-byte ESP app descriptor')
    image = bytearray(elf_path.read_bytes())
    if image[:6] != b'\x7fELF\x01\x01':
        raise ValueError('Expected little-endian ELF32')
    section_offset = struct.unpack_from('<I', image, 32)[0]
    entry_size, count, names_index = struct.unpack_from('<HHH', image, 46)
    sections = [struct.unpack_from('<10I', image, section_offset + i * entry_size) for i in range(count)]
    names = sections[names_index]
    strings = image[names[4]:names[4] + names[5]]
    descriptors = [s for s in sections if strings[s[0]:].split(b'\0', 1)[0] == b'.flash.appdesc']
    if len(descriptors) != 1 or descriptors[0][5] != 256:
        raise ValueError('Expected one 256-byte .flash.appdesc section')
    start = descriptors[0][4]
    if struct.unpack_from('<I', image, start)[0] != 0xABCD5432:
        raise ValueError('Invalid ESP descriptor magic')
    image[start + 16:start + 48] = version.encode('ascii').ljust(32, b'\0')
    image[start + 48:start + 80] = b'Parrot'.ljust(32, b'\0')
    elf_path.write_bytes(image)


def register(env):
    header = Path(env.subst('$PROJECT_DIR')) / 'include/firmware_version.h'
    def patch_descriptor(source, target, env):
        match = re.search(r'^#define FIRMWARE_VERSION "([^"]+)"', header.read_text(), re.M)
        if not match:
            raise ValueError('Missing FIRMWARE_VERSION')
        embed(Path(str(target[0])), match[1])
        print('Embedded Parrot firmware version ' + match[1])
    env.Depends('$BUILD_DIR/${PROGNAME}.elf', str(header))
    env.AddPostAction('$BUILD_DIR/${PROGNAME}.elf', patch_descriptor)


if 'Import' in globals():
    Import('env')
    register(env)
