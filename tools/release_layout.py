#!/usr/bin/env python3
"""Packages a built HA Knob firmware into release files and the web flasher site.

This is the only code that derives flash offsets and writes the ESP Web Tools manifest.
Every offset comes from the build: the PlatformIO project metadata (flash_images,
application_offset) and the built partitions.bin. Nothing is hard-coded, and the layout
is checked on every run (see check_layout).

Output (--out, e.g. dist/):
    ha-knob-<ver>-{bootloader,partitions,boot_app0,firmware,littlefs}.bin
    ha-knob-<ver>-merged.bin       one image for 0x0, ends before NVS (settings survive)
    flash_args.txt                 esptool.py --chip esp32s3 write_flash @flash_args.txt
    SHA256SUMS.txt                 sha256sum -c SHA256SUMS.txt
    LICENSE, THIRD_PARTY_NOTICES.md, licenses/
    site/                          GitHub Pages content: the flasher page, manifest.json,
                                   firmware/ (the 5 parts), esp-web-tools/, licences

Stdlib only; run it with `python -I` (isolated mode). It fetches nothing: --ewt-dir is
the already verified esp-web-tools package/dist/web directory.

    python -I tools/release_layout.py --env t-encoder-pro --metadata <pio metadata json> \\
        --build-dir .pio/build/t-encoder-pro --version-header include/version.h \\
        [--tag v0.8.0] --out dist --site-template web-flasher --ewt-dir <package/dist/web> \\
        --esptool <python> <esptool.py>

Exit codes: 0 done, 1 any check or step failed (message on stderr), 2 usage error.
"""

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

EXIT_OK = 0
EXIT_FAIL = 1  # argparse exits with 2 on usage errors

CHIP = "esp32s3"
CHIP_FAMILY = "ESP32-S3"  # ESP Web Tools manifest name of CHIP
PRODUCT = "HA Knob"
FILE_PREFIX = "ha-knob"
SECTOR = 0x1000  # flash erase sector; every part starts on one and is rounded up to it

VERSION_RE = re.compile(r'#define\s+FW_VERSION\s+"([^"]+)"')
VERSION_CHARS_RE = re.compile(r"^[0-9A-Za-z.+-]+$")

# Partition table (ESP-IDF esp_partition binary format).
PART_ENTRY_SIZE = 32
PART_MAGIC = 0x50AA
PART_MD5_MAGIC = 0xEBEB
PART_END_MAGIC = 0xFFFF
PART_TABLE_MAX = 0xC00  # ESP-IDF partition table size

# (type, subtype) of the partitions the layout depends on.
PART_KEYS = {
    "otadata": (0x01, 0x00),
    "app0": (0x00, 0x10),
    "spiffs": (0x01, 0x82),
    "nvs": (0x01, 0x02),
    "coredump": (0x01, 0x03),
}

# Order of the parts in the outputs; the first three come from the metadata flash_images.
PART_NAMES = ("bootloader", "partitions", "boot_app0", "firmware", "littlefs")
FLASH_IMAGE_FILES = {"bootloader.bin": "bootloader", "partitions.bin": "partitions",
                     "boot_app0.bin": "boot_app0"}

# Repository files that go into dist/ and dist/site/.
LICENSE_FILES = ("LICENSE", "THIRD_PARTY_NOTICES.md")
LICENSE_DIR = "licenses"


class LayoutError(Exception):
    """A check or step failed; the message says which and why."""


def fail(message):
    raise LayoutError(message)


def hexs(value):
    return "0x%X" % value


def round_up(value, unit=SECTOR):
    return (value + unit - 1) // unit * unit


def parse_int(value, what):
    """Metadata offsets are strings like "0x8000" (or plain ints)."""
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            pass
    fail("%s is not a number: %r" % (what, value))


def read_version(header):
    try:
        text = header.read_text(encoding="utf-8")
    except OSError as e:
        fail("cannot read %s: %s" % (header, e))
    match = VERSION_RE.search(text)
    if not match:
        fail("no #define FW_VERSION \"...\" in %s" % header)
    version = match.group(1)
    if not VERSION_CHARS_RE.match(version):
        fail("FW_VERSION %r has characters outside [0-9A-Za-z.+-]" % version)
    return version


def check_tag(tag, version):
    if tag is not None and tag != "v" + version:
        fail("tag %s does not match FW_VERSION %s (expected v%s)" % (tag, version, version))


def read_metadata(path, env):
    """Returns ([(offset, Path)] flash_images, application_offset) of env."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        fail("cannot read metadata %s: %s" % (path, e))
    try:
        extra = data[env]["extra"]
        images = extra["flash_images"]
        app_offset = parse_int(extra["application_offset"], "application_offset")
    except (KeyError, TypeError):
        fail("metadata %s has no %s.extra.flash_images / application_offset" % (path, env))
    result = []
    for image in images:
        try:
            result.append((parse_int(image["offset"], "flash_images offset"), Path(image["path"])))
        except (KeyError, TypeError):
            fail("metadata flash_images entry without offset/path: %r" % (image,))
    return result, app_offset


def read_partitions(path):
    """Parses partitions.bin; returns {key: (offset, size)} for PART_KEYS."""
    try:
        blob = path.read_bytes()
    except OSError as e:
        fail("cannot read %s: %s" % (path, e))
    found = {}
    for pos in range(0, min(len(blob), PART_TABLE_MAX), PART_ENTRY_SIZE):
        entry = blob[pos:pos + PART_ENTRY_SIZE]
        if len(entry) < PART_ENTRY_SIZE:
            break
        magic, ptype, subtype, offset, size = struct.unpack_from("<HBBII", entry)
        if magic in (PART_MD5_MAGIC, PART_END_MAGIC):
            break
        if magic != PART_MAGIC:
            fail("%s: bad partition entry magic 0x%04X at byte %d" % (path, magic, pos))
        for key, ident in PART_KEYS.items():
            if (ptype, subtype) == ident and key not in found:
                found[key] = (offset, size)
    missing = [key for key in PART_KEYS if key not in found]
    if missing:
        fail("%s has no %s partition" % (path, ", ".join(missing)))
    return found


def collect_parts(flash_images, app_offset, partitions, build_dir):
    """Returns {name: (offset, Path)} for PART_NAMES."""
    parts = {}
    for offset, image in flash_images:
        name = FLASH_IMAGE_FILES.get(image.name)
        if name is None:
            fail("unexpected flash image %s in the metadata" % image)
        if name in parts:
            fail("flash image %s listed twice in the metadata" % image.name)
        parts[name] = (offset, image)
    missing = [n for n in FLASH_IMAGE_FILES.values() if n not in parts]
    if missing:
        fail("metadata flash_images lack %s" % ", ".join(missing))
    parts["firmware"] = (app_offset, build_dir / "firmware.bin")
    parts["littlefs"] = (partitions["spiffs"][0], build_dir / "littlefs.bin")
    for name, (_, path) in parts.items():
        if not path.is_file():
            fail("%s image %s does not exist (build it first)" % (name, path))
    return parts


def check_layout(parts, partitions, built_table):
    """Asserts the layout is consistent and leaves NVS and the coredump untouched."""
    sizes = {name: path.stat().st_size for name, (_, path) in parts.items()}
    off = {name: offset for name, (offset, _) in parts.items()}

    if parts["partitions"][1].read_bytes() != built_table.read_bytes():
        fail("metadata partitions image %s differs from the built %s"
             % (parts["partitions"][1], built_table))
    otadata, app0 = partitions["otadata"], partitions["app0"]
    if off["boot_app0"] != otadata[0]:
        fail("boot_app0 offset %s != otadata offset %s" % (hexs(off["boot_app0"]), hexs(otadata[0])))
    if off["firmware"] != app0[0]:
        fail("application_offset %s != app0 offset %s" % (hexs(off["firmware"]), hexs(app0[0])))
    bootloader_end = off["bootloader"] + sizes["bootloader"]
    if off["partitions"] < bootloader_end:
        fail("partition table at %s is not above the bootloader end %s"
             % (hexs(off["partitions"]), hexs(bootloader_end)))
    if sizes["firmware"] > app0[1]:
        fail("firmware.bin (%s bytes) does not fit app0 (%s bytes)"
             % (hexs(sizes["firmware"]), hexs(app0[1])))
    if sizes["littlefs"] != partitions["spiffs"][1]:
        fail("littlefs.bin size %s != spiffs partition size %s"
             % (hexs(sizes["littlefs"]), hexs(partitions["spiffs"][1])))

    ranges = []
    for name in PART_NAMES:
        if off[name] % SECTOR:
            fail("%s offset %s is not %s-aligned" % (name, hexs(off[name]), hexs(SECTOR)))
        if sizes[name] == 0:
            fail("%s image is empty" % name)
        ranges.append((name, off[name], off[name] + round_up(sizes[name])))
    for i, (name_a, start_a, end_a) in enumerate(ranges):
        for name_b, start_b, end_b in ranges[i + 1:]:
            if start_a < end_b and start_b < end_a:
                fail("%s %s-%s overlaps %s %s-%s" % (name_a, hexs(start_a), hexs(end_a),
                                                     name_b, hexs(start_b), hexs(end_b)))
        for keep in ("nvs", "coredump"):
            start_k, size_k = partitions[keep]
            if start_a < start_k + size_k and start_k < end_a:
                fail("%s %s-%s overlaps the %s partition %s-%s"
                     % (name_a, hexs(start_a), hexs(end_a), keep, hexs(start_k),
                        hexs(start_k + size_k)))
    return sizes


def prepare_out(out, clean):
    """Creates out; an existing non-empty out is replaced only with --clean, and only if
    it contains nothing but this script's outputs (so a wrong --out never deletes data)."""
    if out.exists() and any(out.iterdir()):
        if not clean:
            fail("%s is not empty (use --clean to replace a previous output)" % out)
        own = re.compile(r"^(%s-.+\.bin|flash_args\.txt|SHA256SUMS\.txt|site|%s|%s)$"
                         % (re.escape(FILE_PREFIX), re.escape(LICENSE_DIR),
                            "|".join(re.escape(f) for f in LICENSE_FILES)))
        foreign = [p.name for p in out.iterdir() if not own.match(p.name)]
        if foreign:
            fail("%s contains files this script did not write (%s); not deleting it"
                 % (out, ", ".join(sorted(foreign))))
        shutil.rmtree(out)
    out.mkdir(parents=True, exist_ok=True)


def copy_licences(repo_root, dest):
    for name in LICENSE_FILES:
        src = repo_root / name
        if not src.is_file():
            fail("%s is missing" % src)
        shutil.copy2(src, dest / name)
    src_dir = repo_root / LICENSE_DIR
    if not src_dir.is_dir():
        fail("%s is missing" % src_dir)
    shutil.copytree(src_dir, dest / LICENSE_DIR)


def merge(esptool, out_file, parts):
    """Runs esptool merge_bin (no shell); no --fill-flash-size, so the image ends at the
    end of the last part and never reaches NVS."""
    cmd = list(esptool) + ["--chip", CHIP, "merge_bin", "-o", str(out_file),
                           "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep"]
    for offset, path in parts:
        cmd += [hexs(offset), str(path)]
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True)
    except OSError as e:
        fail("cannot run esptool %s: %s" % (esptool, e))
    if result.returncode != 0:
        fail("esptool merge_bin failed (exit %d):\n%s" % (result.returncode, result.stdout.strip()))


def check_merged(merged, parts):
    """The merged image must contain every part byte for byte at its offset and end at
    the end of the last part."""
    blob = merged.read_bytes()
    end = max(offset + path.stat().st_size for offset, path in parts)
    if len(blob) != end:
        fail("merged image is %s bytes, expected %s" % (hexs(len(blob)), hexs(end)))
    for offset, path in parts:
        data = path.read_bytes()
        if blob[offset:offset + len(data)] != data:
            fail("merged image differs from %s at %s" % (path.name, hexs(offset)))


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_text(path, text):
    # newline="\n": the same bytes on Windows and on the Linux runner.
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def build_site(site, template, version, ewt_dir, ewt_license, repo_root, part_files):
    """part_files: [(name, offset, Path in out)] in flash order."""
    if not template.is_dir():
        fail("site template %s is not a directory" % template)
    if not (ewt_dir / "install-button.js").is_file():
        fail("%s has no install-button.js (expected esp-web-tools package/dist/web)" % ewt_dir)
    if not ewt_license.is_file():
        fail("ESP Web Tools licence %s is missing" % ewt_license)

    site.mkdir()
    for src in sorted(template.rglob("*")):
        dest = site / src.relative_to(template)
        if src.is_dir():
            dest.mkdir(parents=True, exist_ok=True)
        elif src.suffix == ".html":
            text = src.read_text(encoding="utf-8")
            if "__VERSION__" not in text:
                fail("%s has no __VERSION__ placeholder" % src)
            write_text(dest, text.replace("__VERSION__", version))
        else:
            shutil.copy2(src, dest)

    shutil.copytree(ewt_dir, site / "esp-web-tools")
    shutil.copy2(ewt_license, site / "esp-web-tools" / "LICENSE")
    copy_licences(repo_root, site)

    (site / "firmware").mkdir()
    manifest_parts = []
    for _, offset, path in part_files:
        shutil.copy2(path, site / "firmware" / path.name)
        manifest_parts.append({"path": "firmware/" + path.name, "offset": offset})
    manifest = {
        "name": PRODUCT,
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": CHIP_FAMILY, "parts": manifest_parts}],
    }
    write_text(site / "manifest.json", json.dumps(manifest, indent=2) + "\n")


def run(args):
    repo_root = Path(args.repo_root).resolve()
    build_dir = Path(args.build_dir)
    out = Path(args.out)

    version = read_version(Path(args.version_header))
    check_tag(args.tag, version)
    flash_images, app_offset = read_metadata(Path(args.metadata), args.env)
    built_table = build_dir / "partitions.bin"
    partitions = read_partitions(built_table)
    parts = collect_parts(flash_images, app_offset, partitions, build_dir)
    sizes = check_layout(parts, partitions, built_table)

    ewt_dir = Path(args.ewt_dir)
    ewt_license = Path(args.ewt_license) if args.ewt_license else ewt_dir.parent.parent / "LICENSE"

    prepare_out(out, args.clean)
    part_files = []
    for name in sorted(PART_NAMES, key=lambda n: parts[n][0]):
        offset, src = parts[name]
        dest = out / ("%s-%s-%s.bin" % (FILE_PREFIX, version, name))
        shutil.copy2(src, dest)
        part_files.append((name, offset, dest))

    merged = out / ("%s-%s-merged.bin" % (FILE_PREFIX, version))
    merge(args.esptool, merged, [(o, p) for _, o, p in part_files])
    check_merged(merged, [(o, p) for _, o, p in part_files])

    lines = ["--flash_mode keep --flash_freq keep --flash_size keep"]
    lines += ["%s %s" % (hexs(o), p.name) for _, o, p in part_files]
    write_text(out / "flash_args.txt", "\n".join(lines) + "\n")

    copy_licences(repo_root, out)
    build_site(out / "site", Path(args.site_template), version, ewt_dir, ewt_license,
               repo_root, part_files)

    sums = [p for p in sorted(out.iterdir()) if p.is_file() and p.name != "SHA256SUMS.txt"]
    write_text(out / "SHA256SUMS.txt", "".join("%s  %s\n" % (sha256(p), p.name) for p in sums))

    print("HA Knob %s (%s)" % (version, args.tag or "no tag"))
    for name, offset, path in part_files:
        print("  %-10s %9s %9s bytes  %s" % (name, hexs(offset), hexs(sizes[name]), path.name))
    print("  %-10s %9s %9s bytes  %s" % ("merged", "0x0", hexs(merged.stat().st_size), merged.name))
    print("  nvs %s-%s and coredump %s-%s are not written"
          % (hexs(partitions["nvs"][0]), hexs(sum(partitions["nvs"])),
             hexs(partitions["coredump"][0]), hexs(sum(partitions["coredump"]))))
    print("output: %s" % out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--env", required=True, help="PlatformIO environment, e.g. t-encoder-pro")
    parser.add_argument("--metadata", required=True,
                        help="pio project metadata -e <env> --json-output result")
    parser.add_argument("--build-dir", required=True, help="e.g. .pio/build/t-encoder-pro")
    parser.add_argument("--version-header", required=True, help="include/version.h")
    parser.add_argument("--tag", help="release ref name; must be v<FW_VERSION>")
    parser.add_argument("--out", required=True, help="output directory, e.g. dist")
    parser.add_argument("--clean", action="store_true",
                        help="replace a previous output in --out")
    parser.add_argument("--site-template", required=True, help="web-flasher/")
    parser.add_argument("--ewt-dir", required=True,
                        help="verified esp-web-tools package/dist/web directory")
    parser.add_argument("--ewt-license",
                        help="ESP Web Tools LICENSE (default: <ewt-dir>/../../LICENSE)")
    parser.add_argument("--esptool", required=True, nargs="+", metavar="ARG",
                        help="esptool command as separate arguments, e.g. <python> <esptool.py>")
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parent.parent),
                        help="where LICENSE, THIRD_PARTY_NOTICES.md and licenses/ are")
    args = parser.parse_args()
    try:
        run(args)
    except LayoutError as e:
        print("release_layout: %s" % e, file=sys.stderr)
        return EXIT_FAIL
    except OSError as e:
        print("release_layout: %s" % e, file=sys.stderr)
        return EXIT_FAIL
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
