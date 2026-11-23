"""Refuse files that look like game content, keys, generated game code or compiled shaders, and art that is not listed.

Runs in CI on every push and pull request, and locally as the pre-commit hook
(.githooks/pre-commit). With no arguments it checks every tracked file; with
--staged it checks the files staged for the next commit.

Standard library only, so it runs before any environment is set up.
"""

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

MAX_BYTES = 1024 * 1024

# Switch containers, executables and game data formats.
BLOCKED_SUFFIXES = {
    ".xci", ".xcz", ".nsp", ".nsz", ".nca", ".ncz", ".tik", ".cert",
    ".nso", ".nro", ".nrr", ".npdm", ".keys",
    ".bfres", ".bntx", ".bnsh", ".sarc", ".szs", ".zs", ".pack", ".byml",
    ".bgyml", ".msbt", ".bars", ".bwav", ".bfwav", ".bfstm", ".bfttf",
    ".bfotf", ".sharcb", ".webm",
}

# ExeFS modules carry no extension.
BLOCKED_NAMES = {"main", "sdk", "rtld", "subsdk0", "subsdk1", "main.npdm"}

# Written by the code generator into every file it emits.
GENERATED_MARKER = "NX2NAVI_" + "GENERATED_FROM_GAME_CODE"

# A prod.keys / title.keys line: name containing key, kek or seed, then hex.
KEY_LINE = re.compile(
    r"^\s*[A-Za-z0-9_]*(key|kek|seed)[A-Za-z0-9_]*\s*=\s*[0-9A-Fa-f]{32,}\s*$",
    re.MULTILINE,
)


def list_files(staged: bool) -> list[Path]:
    if staged:
        cmd = ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"]
    else:
        # Tracked files plus untracked ones that are not ignored: everything a commit could pick up.
        cmd = ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"]
    out = subprocess.run(cmd, check=True, capture_output=True).stdout
    return [Path(p) for p in out.decode("utf-8").split("\0") if p]


def staged_bytes(path: Path) -> bytes | None:
    """The content of path as staged for the next commit (the index), not the working copy, which may differ."""
    r = subprocess.run(["git", "cat-file", "blob", f":{path.as_posix()}"], capture_output=True)
    return r.stdout if r.returncode == 0 else None


def problems_for(path: Path, allow_large: bool = False, data: bytes | None = None) -> list[str]:
    """allow_large: files over MAX_BYTES (toolchain binaries in a release folder) are checked for the generated marker
    only, not refused. data: the content to check in place of the file on disk (the staged one, for --staged)."""
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
    """A SPIR-V binary or a header of SPIR-V words. The repository holds its shaders as source (runtime/shaders/, the
    tools/make_*.py assemblers) and tools/build_shaders.py builds runtime/<name>_spv.h from them when the game is
    built. A build kit carries the headers, so make_kit.py, which calls problems_for, is not held to this."""
    return path.suffix.lower() == ".spv" or path.name.lower().endswith("_spv.h")


# What a game folder records in play and what tools/pipeline_list.py folds from it: the game's shader code, the
# answers it was translated for, the pipelines it drew. All of it is derived from game data.
RECORD_MAGICS = (b"NX2NSPRG", b"NX2NSANS", b"NX2NPKEY", b"NX2NPLST")


def recorded_in_play(path: Path, data: bytes | None) -> bool:
    """A shader-programs.bin, shader-answers.bin, pipeline-keys.bin or a pipeline list, by its first bytes. A build kit
    may carry a pipeline list (data/pipeline_list.bin), so make_kit.py, which calls problems_for, is not held to this."""
    if data is None:
        if not path.is_file():
            return False
        with open(path, "rb") as f:
            data = f.read(8)
    return data[:8] in RECORD_MAGICS


def execution_profile(path: Path) -> bool:
    """The counts an instrumented game wrote (.profraw) or their merged profile (.profdata, tools/pgo.py): they name
    the lifted functions and count their branches, so they are derived from the game's code. A launcher release may
    carry a profile (data/pgo-<version>.profdata), so make_launcher.py, which calls problems_for, is not held to this."""
    return path.suffix.lower() in (".profdata", ".profraw")


# The button art in the repository: pictures from a pack with a clear free licence (CC0), which SOURCE.md names. Every
# other file of the folder is a picture listed in SHA256SUMS with its SHA-256, so a picture of unknown origin, or one
# changed by hand, is refused.
ART_FOLDER = "prompt-art"
ART_RECORDS = ("SOURCE.md", "SHA256SUMS")


def art_sums(staged: bool) -> dict[str, str]:
    """ART_FOLDER's SHA256SUMS (the staged one for --staged): picture path inside the folder -> SHA-256."""
    path = Path(ART_FOLDER) / "SHA256SUMS"
    data = staged_bytes(path) if staged else (path.read_bytes() if path.is_file() else None)
    sums = {}
    for line in (data or b"").decode("utf-8", errors="replace").splitlines():
        digest, _, name = line.partition("  ")
        if name:
            sums[name] = digest.lower()
    return sums


def art_problem(path: Path, data: bytes | None, sums: dict[str, str]) -> str | None:
    """Why a file of ART_FOLDER may not be committed, or None: it is SOURCE.md or SHA256SUMS, or a picture listed in
    SHA256SUMS with its SHA-256."""
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
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--staged", action="store_true", help="check staged files only")
    args = parser.parse_args()

    failed = False
    sums = art_sums(args.staged)
    for path in list_files(args.staged):
        # --staged checks what the commit takes: a marker staged and then removed from the working copy alone
        # passed when the file on disk was read
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
