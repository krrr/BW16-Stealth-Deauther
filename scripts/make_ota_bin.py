#!/usr/bin/env python3
import os
import sys
import struct

def calculate_checksum(data):
    """Calculate the byte-sum checksum of the binary data."""
    return sum(data) & 0xFFFFFFFF

def make_ota_bin(input_path, output_path):
    if not os.path.exists(input_path):
        print(f"Error: Input file '{input_path}' not found.")
        sys.exit(1)

    print(f"Reading input: {input_path}")
    with open(input_path, "rb") as f:
        payload = f.read()

    payload_len = len(payload)
    checksum = calculate_checksum(payload)
    
    print(f"Payload Size: {payload_len} bytes")
    print(f"Payload Checksum: 0x{checksum:08X}")

    # OTA Header Layout (32 bytes total):
    # 1. update_file_hdr (8 bytes):
    #    - FwVer (4 bytes): 0xFFFFFFFF
    #    - HdrNum (4 bytes): 1
    # 2. update_file_img_hdr (24 bytes) for target image:
    #    - ImgId (4 bytes): b'OTA\x00'
    #    - ImgHdrLen (4 bytes): 24
    #    - Checksum (4 bytes): checksum
    #    - ImgLen (4 bytes): payload_len
    #    - Offset (4 bytes): 32 (start of payload data)
    #    - FlashAddr (4 bytes): 0x08006000 (ignored, set to default)

    header = bytearray(32)

    # 1. update_file_hdr
    struct.pack_into("<I", header, 0, 0xFFFFFFFF)  # FwVer
    struct.pack_into("<I", header, 4, 1)           # HdrNum = 1

    # 2. update_file_img_hdr
    header[8:12] = b'OTA\x00'                      # ImgId
    struct.pack_into("<I", header, 12, 24)         # ImgHdrLen
    struct.pack_into("<I", header, 16, checksum)   # Checksum
    struct.pack_into("<I", header, 20, payload_len)# ImgLen
    struct.pack_into("<I", header, 24, 32)          # Offset
    struct.pack_into("<I", header, 28, 0x08006000) # FlashAddr

    print("Generating OTA file header (32 bytes)...")
    
    with open(output_path, "wb") as f:
        f.write(header)
        f.write(payload)

    print(f"Success! Generated: {output_path} ({len(header) + payload_len} bytes)")
    return True

# ---------------------------------------------------------------------------
# PlatformIO SCons post-action hook
# ---------------------------------------------------------------------------
try:
    Import("env")

    def _postaction_make_ota(target, source, env):
        build_dir = env.subst("$BUILD_DIR")
        input_file = os.path.join(build_dir, "km0_km4_image2.bin")
        output_file = os.path.join(build_dir, "ota_all.bin")
        make_ota_bin(input_file, output_file)

    progname = env.subst("$PROGNAME")
    progsuffix = env.subst("$PROGSUFFIX")
    target_axf = os.path.join(env.subst("$BUILD_DIR"), f"{progname}{progsuffix}")
    env.AddPostAction(target_axf, _postaction_make_ota)

except NameError:
    # Not running inside PlatformIO / SCons
    pass

if __name__ == "__main__":
    # Default paths matching PlatformIO structure
    default_input = os.path.join(".pio", "build", "bw16", "km0_km4_image2.bin")
    default_output = os.path.join(".pio", "build", "bw16", "ota_all.bin")

    input_file = sys.argv[1] if len(sys.argv) > 1 else default_input
    output_file = sys.argv[2] if len(sys.argv) > 2 else default_output

    if not make_ota_bin(input_file, output_file):
        sys.exit(1)

