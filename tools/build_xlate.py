import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

DESCRIPTION = ("Build the shader translator: build/xlate/nx2navi_xlate.exe (vendored translator 1.3.2 + "
               "patches, NativeAOT, SPIR-V for Vulkan).")

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "tools" / "vendor" / "ryujinx"
MNG_VENDOR = ROOT / "refs" / "MacNativeGames" / "vendor" / "ryujinx"
XLATE = ROOT / "tools" / "xlate"
BUILD = ROOT / "build" / "xlate"
WIN = os.name == "nt"
EXE = "nx2navi_xlate.exe" if WIN else "nx2navi_xlate"
VSWHERE_DIR = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer"


def tree_hashes(base: Path) -> dict[str, str]:
    def build_output(rel: Path) -> bool:
        return len(rel.parts) > 2 and rel.parts[0] == "src" and rel.parts[2] in ("obj", "bin")

    return {
        p.relative_to(base).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(base.rglob("*"))
        if p.is_file() and not build_output(p.relative_to(base))
    }


def mng_pin() -> str:
    for line in (ROOT / "tools" / "fetch_refs.sh").read_text(encoding="utf-8").splitlines():
        f = line.split()
        if len(f) >= 4 and f[:2] == ["fetch", "MacNativeGames"]:
            return f[3]
    sys.exit("tools/fetch_refs.sh pins no MacNativeGames commit")


def pinned_blobs() -> dict[str, str]:
    out = subprocess.run(["git", "-C", str(MNG_VENDOR.parent.parent), "ls-tree", "-r", mng_pin(), "vendor/ryujinx"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"git ls-tree of the MacNativeGames pin failed: {out.stderr.strip()}")
    blobs = {}
    for line in out.stdout.splitlines():
        meta, path = line.split("\t", 1)
        blobs[path.removeprefix("vendor/ryujinx/")] = meta.split()[2]
    return blobs


def blob_ids(base: Path) -> dict[str, str]:
    ids = {}
    for rel in tree_hashes(base):
        data = (base / rel).read_bytes()
        ids[rel] = hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()
    return ids


def verify_vendor() -> None:
    if not MNG_VENDOR.is_dir():
        print("refs/MacNativeGames missing: vendor comparison skipped")
        return
    ours, theirs = blob_ids(VENDOR), pinned_blobs()
    if ours != theirs:
        diff = sorted(k for k in ours.keys() | theirs.keys() if ours.get(k) != theirs.get(k))
        sys.exit(f"tools/vendor/ryujinx differs from MNG in {len(diff)} files, first: {diff[:5]}")
    print(f"vendor identical to MNG: {len(ours)} files")


def prepare_sources() -> Path:
    src = BUILD / "ryujinx"
    if BUILD.exists():
        shutil.rmtree(BUILD / "ryujinx", ignore_errors=True)
    shutil.copytree(VENDOR, src)
    for junk in list(src.glob("src/*/obj")) + list(src.glob("src/*/bin")):
        shutil.rmtree(junk)
    patches = sorted((XLATE / "patches").glob("*.patch"))
    if len(patches) != 40:
        sys.exit(f"expected 40 patches, found {len(patches)}")
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(BUILD.parent), LC_ALL="C")
    for patch in patches:
        for extra in (["--check"], []):
            r = subprocess.run(
                ["git", "apply", *extra, "--verbose", "--whitespace=nowarn",
                 "--directory", str(src.relative_to(BUILD)), str(patch)],
                cwd=BUILD, capture_output=True, text=True, env=env,
            )
            skipped = "Skipped patch" in r.stdout or "Skipped patch" in r.stderr
            if r.returncode != 0 or skipped:
                sys.exit(f"patch does not apply: {patch.name}\n{r.stdout}{r.stderr}")
        print(f"applied {patch.name}")
    return src


BUILD_SETTINGS = "PublishAot=true InvariantGlobalization=true Release"


def strip_code(text: str, xml: bool = False) -> str:
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if xml:
            if text.startswith("<!--", i):
                j = text.find("-->", i + 4)
                i = n if j < 0 else j + 3
                continue
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        elif c in "\"'" or text.startswith('@"', i):
            verbatim = text.startswith('@"', i)
            q = '"' if verbatim else c
            j = i + (2 if verbatim else 1)
            while j < n:
                if verbatim and text.startswith('""', j):
                    j += 2
                elif not verbatim and text[j] == "\\":
                    j += 2
                elif text[j] == q:
                    j += 1
                    break
                else:
                    j += 1
            out.append(text[i:j])
            i = j
            continue
        out.append(c)
        i += 1
    lines = (line.rstrip() for line in "".join(out).splitlines())
    return "\n".join(line for line in lines if line)


def code_digest(path: Path) -> str:
    data = path.read_bytes()
    if path.suffix in (".cs", ".csproj", ".props", ".targets"):
        data = strip_code(data.decode("utf-8-sig", errors="replace"), xml=path.suffix != ".cs").encode()
    return hashlib.sha256(data).hexdigest()


def identity() -> str:
    h = hashlib.sha256()
    h.update(f"settings\0{BUILD_SETTINGS}\n".encode())
    for name in ("Program.cs", "AmpereLowering.cs", "MeshSpirv.cs", "nx2navi_xlate.csproj"):
        h.update(f"{name}\0{code_digest(XLATE / name)}\n".encode())
    tree = BUILD / "ryujinx"
    if (tree / "src").is_dir():
        for f in sorted(tree.rglob("*")):
            rel = f.relative_to(tree)
            if f.is_file() and not (len(rel.parts) > 2 and rel.parts[0] == "src" and rel.parts[2] in ("obj", "bin")):
                h.update(f"tree/{rel.as_posix()}\0{code_digest(f)}\n".encode())
    else:
        for f in sorted((XLATE / "patches").glob("*.patch")):
            h.update(f"{f.name}\0{hashlib.sha256(f.read_bytes()).hexdigest()}\n".encode())
        for rel, digest in tree_hashes(VENDOR).items():
            h.update(f"vendor/{rel}\0{digest}\n".encode())
    return h.hexdigest()


def write_identity(exe: Path) -> None:
    ident = identity()
    exe.with_suffix(".id").write_text(ident + "\n", encoding="ascii")
    print(f"translator identity {ident[:16]} -> {exe.with_suffix('.id')}")


def source_identity() -> str:
    import tempfile
    global BUILD
    old = BUILD
    with tempfile.TemporaryDirectory(prefix="xlate-id-") as d:
        BUILD = Path(d) / "xlate"
        try:
            prepare_sources()
            return identity()
        finally:
            BUILD = old


def check_built(folder: Path, exe: str = EXE) -> str | None:
    folder = Path(folder)
    for name in (exe, "nx2navi_xlate.id"):
        if not (folder / name).is_file():
            return f"{folder / name} is missing: build the shader translator with tools/build_xlate.py --out {folder}"
    have = (folder / "nx2navi_xlate.id").read_text(encoding="ascii", errors="replace").strip()
    want = source_identity()
    if have != want:
        return (f"{folder}: its translator was built from other sources (identity {have[:16]}) than this checkout's "
                f"({want[:16]}): build it again with tools/build_xlate.py --out {folder}")
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description=DESCRIPTION)
    parser.add_argument("--out", type=Path, help="build directory instead of build/xlate")
    parser.add_argument("--llvm", type=Path, help="Linux: an LLVM directory whose bin/clang and lld link the exe")
    parser.add_argument("--sysroot", type=Path, help="Linux: link against this sysroot's glibc instead of the host's")
    a = parser.parse_args()
    if a.out:
        global BUILD
        BUILD = a.out.resolve()
    started = time.time()
    dotnet = shutil.which("dotnet")
    if not dotnet:
        sys.exit("missing: dotnet (.NET 10 SDK)")
    env = dict(os.environ)
    if WIN:
        if not (VSWHERE_DIR / "vswhere.exe").is_file() and not shutil.which("vswhere"):
            sys.exit(f"missing: vswhere.exe (expected in {VSWHERE_DIR}); NativeAOT cannot link without it")
        env["PATH"] = str(VSWHERE_DIR) + os.pathsep + env["PATH"]

    verify_vendor()
    src = prepare_sources()
    work = BUILD / "xlate"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    for name in ("Program.cs", "AmpereLowering.cs", "MeshSpirv.cs", "nx2navi_xlate.csproj"):
        shutil.copy2(XLATE / name, work / name)
    out = BUILD / "publish"
    shutil.rmtree(out, ignore_errors=True)
    cmd = [
        dotnet, "publish", str(work / "nx2navi_xlate.csproj"), "-c", "Release", "-r", "win-x64" if WIN else "linux-x64",
        "-p:PublishAot=true", "-p:InvariantGlobalization=true", f"-p:RyuSrc={src}", "-o", str(out),
    ]
    if a.llvm:
        env["PATH"] = str(a.llvm.resolve() / "bin") + os.pathsep + env["PATH"]
        cmd += ["-p:CppCompilerAndLinker=clang", "-p:LinkerFlavor=lld"]
    if a.sysroot:
        cmd.append(f"-p:SysRoot={a.sysroot.resolve()}")
    if subprocess.run(cmd, env=env).returncode != 0:
        sys.exit("dotnet publish failed")
    exe = BUILD / EXE
    shutil.copy2(out / EXE, exe)
    write_identity(exe)
    print(f"built {exe} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - started:.0f} s")


if __name__ == "__main__":
    main()
