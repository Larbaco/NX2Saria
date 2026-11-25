import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "xlate" / ("nx2navi_xlate.exe" if os.name == "nt" else "nx2navi_xlate")
SPIRV_VAL = (Path(r"C:\msys64\ucrt64\bin\spirv-val.exe") if os.name == "nt"
             else Path(shutil.which("spirv-val") or "/usr/bin/spirv-val"))

PT = 7 << 16


def mov32i(dest: int, value: float) -> int:
    imm = struct.unpack("<I", struct.pack("<f", value))[0]
    return 0x0100000000000000 | (imm << 20) | (0xF << 12) | PT | dest


EXIT = 0xE30000000000000F | PT
SCHED = 0x001F8000FC0007E0
RZ = 0xFF
IMAGE0 = 0x120 // 4


def fragment_code() -> bytes:
    header = [0] * 20
    header[0] = 2 | (3 << 5) | (5 << 10)
    header[18] = 0xF
    words = [SCHED, mov32i(0, 1.0), mov32i(1, 0.5), mov32i(2, 0.25),
             SCHED, mov32i(3, 1.0), EXIT, 0]
    return struct.pack("<20I", *header) + struct.pack(f"<{len(words)}Q", *words)


SULD_P, SUST_P = 0x1D61, 0x1D65
TXQ = 0x1BE9


def surface_p(opcode: int, data: int, coord: int, handle: int) -> int:
    no_pred_out = 7 << 30 if opcode == SULD_P else 0
    return (opcode << 51) | (handle << 36) | no_pred_out | (0xF << 20) | PT | (coord << 8) | data


def image_read_code() -> bytes:
    words = [SCHED, surface_p(SULD_P, 0, RZ, IMAGE0), surface_p(SUST_P, 0, RZ, IMAGE0), EXIT]
    return struct.pack(f"<{len(words)}Q", *words)


def layer_count_code() -> bytes:
    txq = (TXQ << 51) | (0x10 << 36) | (0x4 << 31) | (1 << 22) | PT | (RZ << 8) | 0
    sust = (SUST_P << 51) | (IMAGE0 << 36) | (0x1 << 20) | PT | (RZ << 8) | 0
    words = [SCHED, txq, sust, EXIT]
    return struct.pack(f"<{len(words)}Q", *words)


IPA = 0xE0 << 56
TXQ_B = 0x1BEA


def pool_query_code() -> bytes:
    header = [0] * 20
    header[0] = 2 | (3 << 5) | (5 << 10)
    header[18] = 0xF
    ipa = IPA | (7 << 47) | (RZ << 39) | (0x80 << 28) | (RZ << 20) | PT | (RZ << 8) | 4
    txq = (TXQ_B << 51) | (0x1 << 31) | (1 << 22) | PT | (4 << 8) | 0
    words = [SCHED, ipa, txq, mov32i(1, 0.0), SCHED, mov32i(2, 0.0), mov32i(3, 1.0), EXIT, 0]
    return struct.pack("<20I", *header) + struct.pack(f"<{len(words)}Q", *words)


def spirv_clamps(spv: bytes):
    w = struct.unpack(f"<{len(spv) // 4}I", spv[:len(spv) // 4 * 4])
    consts, lengths, umin, glsl = {}, [], [], None
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if not n:
            break
        ins = w[i:i + n]
        if op == 11 and struct.pack(f"<{n - 2}I", *ins[2:]).split(b"\0")[0] == b"GLSL.std.450":
            glsl = ins[1]
        elif op == 43 and n == 4:
            consts[ins[2]] = ins[3]
        elif op == 28:
            lengths.append(ins[3])
        elif op == 12 and n > 5 and ins[3] == glsl and ins[4] == 38:
            umin += ins[5:]
        i += n
    return [consts.get(x) for x in lengths], [consts[x] for x in umin if x in consts]


def pool_check(exe: Path):
    m = re.search(r"TexturePoolLength = (\d+)", (ROOT / "tools" / "xlate" / "Program.cs").read_text(encoding="utf-8"))
    if not m:
        return "tools/xlate/Program.cs names no TexturePoolLength"
    length = int(m.group(1))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        code = tmp / "pool_ps.bin"
        code.write_bytes(pool_query_code())
        out = tmp / "out"
        job = {"key": "pool_ps", "stage": "ps", "code": str(code), "debug": False, "fout": {"0": "float"}}
        try:
            r = subprocess.run([str(exe), "--serve"], input=json.dumps({"job": job, "out": str(out)}) + "\n",
                               capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.SubprocessError) as e:
            return f"the translator does not run: {e}"
        if r.stdout.strip() != "ok pool_ps":
            return f"the translator answered {r.stdout.strip()[:120]!r} to the texture pool shader"
        try:
            textures = json.loads((out / "pool_ps.res.json").read_text(encoding="utf-8")).get("textures") or []
            lengths, clamps = spirv_clamps((out / "pool_ps.spv").read_bytes())
        except (OSError, ValueError, struct.error) as e:
            return f"the translator's texture pool shader cannot be read: {e}"
    pools = [t.get("array") for t in textures if t.get("pool") == 1]
    if pools != [length] or length not in lengths:
        return (f"the translator declares the texture pool with {pools or 'no'} entries, where its sources say {length} "
                f"(tools/xlate/Program.cs TexturePoolLength): it is older than its sources")
    if length - 1 not in clamps:
        return f"the translator clamps a texture pool handle to {clamps}, not to {length - 1}"
    return None


def main() -> int:
    exe = Path(sys.argv[sys.argv.index("--exe") + 1]) if "--exe" in sys.argv[1:-1] else EXE
    if not exe.is_file():
        print(f"missing {exe}; run tools/build_xlate.py", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        code = tmp / "ps.bin"
        code.write_bytes(fragment_code())
        image_code = tmp / "cs.bin"
        image_code.write_bytes(image_read_code())
        layer_code = tmp / "layers.bin"
        layer_code.write_bytes(layer_count_code())
        out = tmp / "out"
        job = {"key": "const_ps", "stage": "ps", "code": str(code), "debug": False, "fout": {"0": "float"}}
        image_job = {"key": "image_read_cs", "stage": "cs", "code": str(image_code), "debug": False, "local": [1, 1, 1]}
        layer_job = {"key": "layer_count_cs", "stage": "cs", "code": str(layer_code), "debug": False, "local": [1, 1, 1]}
        lines = [
            json.dumps({"job": job, "out": str(out)}),
            "this is not json",
            json.dumps({"job": dict(job, key="again"), "out": str(out)}),
            json.dumps({"job": image_job, "out": str(out)}),
            json.dumps({"job": layer_job, "out": str(out)}),
        ]
        r = subprocess.run([str(exe), "--serve"], input="\n".join(lines) + "\n", capture_output=True, text=True, timeout=60)
        replies = r.stdout.splitlines()
        print("replies:", replies)
        if (len(replies) != 5 or replies[0] != "ok const_ps" or not replies[1].startswith("fail ") or replies[2] != "ok again"
                or replies[3] != "ok image_read_cs" or replies[4] != "ok layer_count_cs"):
            print("unexpected replies; stderr:", r.stderr, file=sys.stderr)
            return 1
        failed = 0
        layer_types = [t["type"] for t in json.loads((out / "layer_count_cs.res.json").read_text())["textures"]]
        if layer_types != [0x103]:
            print(f"layer_count_cs: texture types {layer_types}, expected a 2D array [259]", file=sys.stderr)
            failed = 1
        for key in ("const_ps", "image_read_cs", "layer_count_cs"):
            spv = out / f"{key}.spv"
            data = spv.read_bytes()
            if len(data) < 20 or struct.unpack("<I", data[:4])[0] != 0x07230203:
                print(f"{key}: reply is not SPIR-V", file=sys.stderr)
                return 1
            v = subprocess.run([str(SPIRV_VAL), "--target-env", "vulkan1.3", str(spv)], capture_output=True, text=True)
            print(f"{key}: spirv-val exit {v.returncode} ({len(data)} bytes) {v.stdout}{v.stderr}")
            failed |= v.returncode != 0
    why = pool_check(exe)
    print(f"pool_ps: {why or 'the texture pool array and its clamp have the length tools/xlate/Program.cs names'}")
    return int(failed or why is not None)


if __name__ == "__main__":
    sys.exit(main())
