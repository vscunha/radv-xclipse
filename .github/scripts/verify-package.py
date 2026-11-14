#!/usr/bin/env python3
"""Static packaging/ABI verification, not a GPU or Android runtime test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import zipfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def normalized_notice(data):
    # GitHub's official Windows-built ZIP stores the same NOTICE text with CRLF;
    # compare the license text, not the checkout's line endings.
    return data.replace(b"\r\n", b"\n")


def inspect(package, directory, readelf):
    with zipfile.ZipFile(package) as z:
        assert sorted(z.namelist()) == ["NOTICE.txt", "meta.json", "vulkan.radeon.so"], z.namelist()
        assert z.testzip() is None
        meta = json.loads(z.read("meta.json"))
        assert meta["libraryName"] == "vulkan.radeon.so"
        assert meta["schemaVersion"] == 1 and meta["minApi"] == 34
        blob = z.read("vulkan.radeon.so")
        notice = z.read("NOTICE.txt")
    assert blob[:6] == b"\x7fELF\x02\x01", "not little-endian ELF64"
    assert struct.unpack_from("<HH", blob, 16) == (3, 183), "not AArch64 ET_DYN"
    phoff, shoff = struct.unpack_from("<QQ", blob, 32)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", blob, 54)
    loads = []
    for i in range(phnum):
        fields = struct.unpack_from("<IIQQQQQQ", blob, phoff + i * phentsize)
        if fields[0] == 1:
            assert fields[7] >= 16384, "LOAD alignment below Android 16 KiB"
            assert fields[2] % 16384 == fields[3] % 16384
            loads.append({"offset": fields[2], "vaddr": fields[3], "align": fields[7]})
    sections = [struct.unpack_from("<IIQQQQIIQQ", blob, shoff + i * shentsize) for i in range(shnum)]
    strings_section = sections[shstrndx]
    strings = blob[strings_section[4]:strings_section[4] + strings_section[5]]
    android = None
    for section in sections:
        name = strings[section[0]:].split(b"\0", 1)[0].decode()
        assert name != ".symtab" and not name.startswith(".debug"), "not stripped"
        if name == ".note.android.ident":
            data = blob[section[4]:section[4] + section[5]]
            namesz, descsz, _ = struct.unpack_from("<III", data)
            desc = data[12 + ((namesz + 3) & ~3):][:descsz]
            android = {"api": struct.unpack_from("<I", desc)[0],
                       "ndk": desc[4:68].split(b"\0", 1)[0].decode(),
                       "ndk_build": desc[68:132].split(b"\0", 1)[0].decode()}
    assert android and android["api"] == 34, android
    directory.mkdir(parents=True, exist_ok=True)
    lib = directory / "vulkan.radeon.so"
    lib.write_bytes(blob)
    def elf(*args):
        return subprocess.check_output([readelf, *args, str(lib)], text=True)
    dynamic = elf("-d")
    exports = elf("--dyn-syms", "--wide")
    versions = elf("--version-info")
    assert "GLIBC_" not in versions, "host glibc dependency"
    needed = sorted(re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic))
    for symbol in ("vk_icdGetInstanceProcAddr", "vk_icdNegotiateLoaderICDInterfaceVersion"):
        assert re.search(r"\b" + symbol + r"\b", exports), f"missing loader export {symbol}"
    (directory / "readelf.txt").write_text(elf("-h", "-l", "-d", "-n") + versions + exports)
    result = {"package_sha256": sha(Path(package).read_bytes()), "library_sha256": sha(blob),
              "library_bytes": len(blob), "metadata": meta, "needed": needed,
              "android": android, "load_segments": loads, "notice_sha256": sha(notice)}
    return result, blob, notice


def main():
    p = argparse.ArgumentParser()
    for name in ("package", "reference", "out", "variant", "source", "readelf"):
        p.add_argument("--" + name, required=True)
    a = p.parse_args()
    out = Path(a.out)
    built, blob, notice = inspect(a.package, out / "verification/built", a.readelf)
    reference, _, reference_notice = inspect(a.reference, out / "verification/official", a.readelf)
    assert built["needed"] == reference["needed"], (built["needed"], reference["needed"])
    assert normalized_notice(notice) == normalized_notice(reference_notice), "license notices differ"
    for key in ("schemaVersion", "author", "vendor", "driverVersion", "minApi", "libraryName"):
        assert built["metadata"][key] == reference["metadata"][key], key
    assert a.variant == "diag19", f"unsupported variant {a.variant}"
    code = subprocess.check_output(["git", "-C", a.source, "rev-parse", "HEAD"], text=True).strip()
    build_id = f"{a.variant}-{os.environ['GITHUB_RUN_ID']}-{os.environ['GITHUB_RUN_ATTEMPT']}"
    label = '1.9 DIAGNOSTIC'
    meta = dict(built["metadata"])
    meta.update(
        name=f"RADV Xclipse {label} {build_id} ({code[:7]})",
        description=(
            f"v1.9.0 (cc87fdf) plus opt-in task/mesh emulation diagnostics; source {code}; {build_id}. "
            "NOT device-validated. Based on JimVulkan RADV Xclipse."
        ),
        codeCommit=code,
        upstreamCommit='cc87fdf71267dd0acc3b470920a6566a1a46f0be',
        workflowCommit=os.environ["GITHUB_SHA"],
        buildId=build_id,
    )
    target = out / "packages" / f"radv-xclipse-{build_id}-{code[:7]}.zip"
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("meta.json", json.dumps(meta, indent=2) + "\n")
        z.writestr("vulkan.radeon.so", blob)
        z.writestr("NOTICE.txt", notice)
    comparison = {"built": built, "official_1_9": reference,
                  "binary_identical": built["library_sha256"] == reference["library_sha256"],
                  "static_checks": "passed; no device execution or correctness implied",
                  "installable_zip": target.name, "installable_sha256": sha(target.read_bytes())}
    (out / "verification/comparison.json").write_text(json.dumps(comparison, indent=2) + "\n")
    print(json.dumps(comparison, indent=2))


if __name__ == "__main__":
    main()
