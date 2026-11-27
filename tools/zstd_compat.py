try:
    from compression.zstd import ZstdDict, decompress, get_frame_info  # noqa: F401
except ImportError:
    from collections import namedtuple

    import zstandard

    FrameInfo = namedtuple('FrameInfo', 'decompressed_size dictionary_id')

    class ZstdDict:
        def __init__(self, raw):
            self._d = zstandard.ZstdCompressionDict(raw, dict_type=zstandard.DICT_TYPE_AUTO)
            self.dict_id = self._d.dict_id()

    def get_frame_info(data):
        p = zstandard.get_frame_parameters(data)
        size = p.content_size if p.content_size >= 0 and p.content_size != zstandard.CONTENTSIZE_UNKNOWN else None
        return FrameInfo(size, p.dict_id)

    def decompress(data, zstd_dict=None):
        d = zstandard.ZstdDecompressor(dict_data=zstd_dict._d if zstd_dict else None)
        p = zstandard.get_frame_parameters(data)
        if 0 <= p.content_size != zstandard.CONTENTSIZE_UNKNOWN:
            frame = zstandard.frame_content_size(data)
            if frame == p.content_size:
                try:
                    return d.decompress(data)
                except zstandard.ZstdError:
                    pass
        out, pos = [], 0
        while pos < len(data):
            obj = d.decompressobj()
            out.append(obj.decompress(data[pos:]))
            rest = obj.unused_data
            pos = len(data) - len(rest)
            if not rest:
                break
        return b''.join(out)
