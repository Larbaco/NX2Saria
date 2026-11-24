import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

DESCRIPTION = ("Refuse files that look like game content, keys, generated game code or compiled shaders, and "
               "art that is not listed.")

MAX_BYTES = 1024 * 1024

BLOCKED_SUFFIXES = {
    ".xci", ".xcz", ".nsp", ".nsz", ".nca", ".ncz", ".tik", ".cert",
    ".nso", ".nro", ".nrr", ".npdm", ".keys",
    ".bfres", ".bntx", ".bnsh", ".sarc", ".szs", ".zs", ".pack", ".byml",
    ".bgyml", ".msbt", ".bars", ".bwav", ".bfwav", ".bfstm", ".bfttf",
    ".bfotf", ".sharcb", ".webm",
}

BLOCKED_NAMES = {"main", "sdk", "rtld", "subsdk0", "subsdk1", "main.npdm"}

GENERATED_MARKER = "NX2NAVI_" + "GENERATED_FROM_GAME_CODE"

KEY_LINE = re.compile(
    r"^\s*[A-Za-z0-9_]*(key|kek|seed)[A-Za-z0-9_]*\s*=\s*[0-9A-Fa-f]{32,}\s*$",
    re.MULTILINE,
)


def list_files(staged: bool) -> list[Path]:
    if staged:
        cmd = ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"]
    else:
        cmd = ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"]
    out = subprocess.run(cmd, check=True, capture_output=True).stdout
    return [Path(p) for p in out.decode("utf-8").split("\0") if p]


def staged_bytes(path: Path) -> bytes | None:
    r = subprocess.run(["git", "cat-file", "blob", f":{path.as_posix()}"], capture_output=True)
    return r.stdout if r.returncode == 0 else None


def problems_for(path: Path, allow_large: bool = False, data: bytes | None = None) -> list[str]:
    found = []
    if path.suffix.lower() in BLOCKED_SUFFIXES:
        found.append(f"blocked file type {path.suffix}")
    if path.name.lower() in BLOCKED_NAMES:
        found.append("name of a Switch executable module")
    if data is None:
        if not path.is_file():
            return found
        size = path.stat().st_size
    else:
        size = len(data)
    if size > MAX_BYTES and allow_large:
        if GENERATED_MARKER.encode() in (path.read_bytes() if data is None else data):
            found.append("generated from game code")
        return found
    if size > MAX_BYTES:
        found.append(f"{size // 1024} KiB, over the {MAX_BYTES // 1024} KiB limit")
        return found
    if data is None:
        data = path.read_bytes()
    if GENERATED_MARKER.encode() in data:
        found.append("generated from game code")
    if KEY_LINE.search(data.decode("utf-8", errors="ignore")):
        found.append("contains a key in prod.keys format")
    return found


def compiled_shader(path: Path) -> bool:
    return path.suffix.lower() == ".spv" or path.name.lower().endswith("_spv.h")


RECORD_MAGICS = (b"NX2NSPRG", b"NX2NSANS", b"NX2NPKEY", b"NX2NPLST")


def recorded_in_play(path: Path, data: bytes | None) -> bool:
    if data is None:
        if not path.is_file():
            return False
        with open(path, "rb") as f:
            data = f.read(8)
    return data[:8] in RECORD_MAGICS


def execution_profile(path: Path) -> bool:
    return path.suffix.lower() in (".profdata", ".profraw")


ART_FOLDER = "prompt-art"
ART_RECORDS = ("SOURCE.md", "SHA256SUMS")


def art_sums(staged: bool) -> dict[str, str]:
    path = Path(ART_FOLDER) / "SHA256SUMS"
    data = staged_bytes(path) if staged else (path.read_bytes() if path.is_file() else None)
    sums = {}
    for line in (data or b"").decode("utf-8", errors="replace").splitlines():
        digest, _, name = line.partition("  ")
        if name:
            sums[name] = digest.lower()
    return sums


def art_problem(path: Path, data: bytes | None, sums: dict[str, str]) -> str | None:
    parts = path.as_posix().split("/")
    if parts[0] != ART_FOLDER or len(parts) < 2 or path.as_posix() in (f"{ART_FOLDER}/{n}" for n in ART_RECORDS):
        return None
    rel = "/".join(parts[1:])
    if rel not in sums:
        return (f"art not listed in {ART_FOLDER}/SHA256SUMS: only pictures of a pack with a clear free licence, named "
                f"in {ART_FOLDER}/SOURCE.md and listed there, go in")
    if data is None:
        data = path.read_bytes() if path.is_file() else b""
    if hashlib.sha256(data).hexdigest() != sums[rel]:
        return f"differs from its SHA-256 in {ART_FOLDER}/SHA256SUMS: the pictures go in unmodified"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=DESCRIPTION)
    parser.add_argument("--staged", action="store_true", help="check staged files only")
    args = parser.parse_args()

    failed = False
    sums = art_sums(args.staged)
    for path in list_files(args.staged):
        data = staged_bytes(path) if args.staged else None
        problems = problems_for(path, data=data)
        if compiled_shader(path):
            problems.append("compiled shader: the repository holds the source, tools/build_shaders.py builds this")
        if recorded_in_play(path, data):
            problems.append("recorded from a game folder (shaders, answers, pipeline keys or a pipeline list)")
        if execution_profile(path):
            problems.append("execution profile of the game's code: it stays in build/ (tools/pgo.py)")
        art = art_problem(path, data, sums)
        if art:
            problems.append(art)
        for problem in problems:
            print(f"{path}: {problem}")
            failed = True
    if failed:
        print("\nGame content, keys, generated game code and compiled shaders never go into the repository, nor art")
        print("without a clear free licence.")
        print("See CONTRIBUTING.md.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
