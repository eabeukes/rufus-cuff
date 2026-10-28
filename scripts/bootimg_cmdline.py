"""Append to the cmdline of an Android header-v0 boot/recovery image, changing nothing else.

usage:
  python -I bootimg_cmdline.py show  <image>
  python -I bootimg_cmdline.py patch <image> <out.img> "<args to append>"

The kernel, ramdisk and second blobs (MTK 512-byte headers included) and every other
header field are copied verbatim. Only `cmdline` and the `id` hash change. The input may
be a raw partition dump; trailing padding is dropped and the output covers the image only.
"""
import hashlib
import struct
import sys

HDR = struct.Struct("<8s10I16s512s32s1024s")  # v0 layout, 1632 bytes


def pages(n, page):
    return (n + page - 1) // page * page


def load(path):
    data = open(path, "rb").read()
    f = list(HDR.unpack_from(data))
    magic, ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, hver, osver, name, cmd, img_id, extra = f
    if magic != b"ANDROID!" or hver != 0:
        sys.exit(f"{path}: not a header-v0 boot image (magic={magic!r}, header_version={hver})")
    off = page
    kernel = data[off:off + ksz]; off += pages(ksz, page)
    ramdisk = data[off:off + rsz]; off += pages(rsz, page)
    second = data[off:off + ssz]; off += pages(ssz, page)
    return data, f, kernel, ramdisk, second, off


def image_id(kernel, ramdisk, second):
    h = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        h.update(blob)
        h.update(struct.pack("<I", len(blob)))
    return h.digest().ljust(32, b"\0")


def show(path):
    data, f, kernel, ramdisk, second, end = load(path)
    cmd = f[12].rstrip(b"\0").decode()
    ok = image_id(kernel, ramdisk, second) == f[13]
    print(f"page={f[8]} kernel={len(kernel)}@{f[2]:#x} ramdisk={len(ramdisk)}@{f[4]:#x} "
          f"tags@{f[7]:#x} os_version={f[10]:#x} name={f[11].rstrip(bytes(1)).decode()!r}")
    print(f"cmdline={cmd!r}")
    print(f"id hash recomputes: {'yes' if ok else 'NO'}; image ends at {end} of {len(data)} bytes")
    return ok


def patch(path, out, extra):
    data, f, kernel, ramdisk, second, end = load(path)
    cmd = f[12].rstrip(b"\0").decode()
    new = (cmd + " " + extra).strip().encode()
    if len(new) >= 512:
        sys.exit(f"cmdline too long ({len(new)} bytes, max 511)")
    f[12] = new.ljust(512, b"\0")
    f[13] = image_id(kernel, ramdisk, second)
    header = HDR.pack(*f)
    image = bytearray(data[:end])
    image[:len(header)] = header
    open(out, "wb").write(image)
    changed = [i for i in range(end) if image[i] != data[i]]
    print(f"wrote {out}: {end} bytes, {len(changed)} bytes differ, all in "
          f"[{min(changed)}, {max(changed)}] (cmdline field is 64..575)" if changed else "no change")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "show" and len(sys.argv) == 3:
        sys.exit(0 if show(sys.argv[2]) else 1)
    if cmd == "patch" and len(sys.argv) == 5:
        patch(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)
