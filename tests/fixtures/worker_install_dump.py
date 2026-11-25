import hashlib
import struct
import sys
from pathlib import Path

from launcher.gamefiles import TITLE_ID
from tools import nup


def write_dump(path, title=TITLE_ID):
    toc_size = 0x30 + nup.PACKAGE.size
    toc = struct.pack('<IIQQQQQ', 0x01434F54, 1, 0, 0, toc_size, 0, 0)
    toc += nup.PACKAGE.pack(title, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, b'')
    header = nup.SUPER.pack(b'NUP\x01', 0, 0, nup.SUPER.size, len(toc), hashlib.sha256(toc).digest(),
                            0, 0, 1, 0, b'', b'')
    Path(path).write_bytes(header + toc)


if __name__ == '__main__':
    write_dump(sys.argv[1])
