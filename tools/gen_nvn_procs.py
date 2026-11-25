# The NVN proc names: Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NVN_DIR = next((ROOT / "refs/MacNativeGames/games").glob("*/third_party/d2r_gfx/gfx/nvn"),
               ROOT / "refs/MacNativeGames/games/third_party/d2r_gfx/gfx/nvn")
OUT = ROOT / "runtime/nvn/nvn_procs.inc"

NOT_LOADED = {"nvnDeviceApplyDeferredFinalizes", "nvnQueueAcquireTexture"}
SUBSDK0 = [
    "nvnDeviceGetProcAddress",
    "nvnSyncFinalize",
    "nvnSyncGetNvRmSyncNVX",
    "nvnSyncInitializeFromNvRmSyncNVX",
    "nvnTextureGetNvRmSurfaceNVX",
]
MAIN_COUNT = 534
NVN2 = [
    "nvnCommandBufferDrawMeshTasks",
    "nvnCommandBufferDrawMeshTasksIndirect",
    "nvnCommandBufferPushComputeConstants",
    "nvnCommandBufferSetGeometryProcessingMode",
    "nvnProgramBuilderSetDefaults",
    "nvnProgramBuilderSetDevice",
    "nvnProgramBuilderSetShaders",
    "nvnProgramInitializeFromBuilder",
]


def main() -> int:
    names = [n.strip() for n in (NVN_DIR / "names.txt").read_text(encoding="utf-8").splitlines() if n.strip()]
    extras = re.findall(r"^NVN_EXTRA\((\w+)", (NVN_DIR / "nx2navi_nvn_ext.c").read_text(encoding="utf-8"), re.M)
    if len(extras) != 13:
        print(f"expected 13 NVN_EXTRA names, found {len(extras)}", file=sys.stderr)
        return 1
    main_names = [n for n in names if n not in NOT_LOADED] + extras
    if len(main_names) != len(set(main_names)) or len(main_names) != MAIN_COUNT:
        print(f"expected {MAIN_COUNT} distinct main names, found {len(main_names)} "
              f"({len(set(main_names))} distinct)", file=sys.stderr)
        return 1
    extra_sub = [n for n in SUBSDK0 if n not in main_names]
    rows = sorted(main_names) + sorted(extra_sub) + sorted(n for n in NVN2 if n not in main_names)
    text = "".join(f"NVN_PROC({n})\n" for n in rows)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUT.relative_to(ROOT)}: {len(main_names)} main names + {len(extra_sub)} subsdk0-only names "
          f"({len(SUBSDK0)} asked by subsdk0), {len(rows)} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
