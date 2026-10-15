"""Validate the input-only native draw archive; never declares frame replay safe."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect(path):
    archive = path.read_bytes()
    assert len(archive) >= 80 and archive[:8] == b'HZPAY001', 'header'
    version, flags, replay_complete, frame, count, size, first, last, digest = struct.unpack_from('<9Q', archive, 8)
    assert version == 1 and flags == 3 and replay_complete == 0, 'unsupported or misrepresented capture'
    assert 0 < count <= 8192 and 0 < size <= 256 * 1024 * 1024, 'limits'
    assert last - first + 1 == count, 'draw coverage'
    start = 80 + count * 72
    assert len(archive) == start + size, 'truncated or trailing data'
    payload = memoryview(archive)[start:]
    totals = [0, 0, 0, 0]
    present = [0, 0, 0, 0]
    identities = [{}, {}, {}, {}]
    cursor = 0
    for index in range(count):
        record = struct.unpack_from('<9Q', archive, 80 + index * 72)
        assert record[0] == first + index, 'ordinal gap'
        assert record[6] == 8224, 'VS input length differs from current renderer'
        for kind in range(4):
            offset, length = record[1 + kind * 2:3 + kind * 2]
            if not length:
                assert offset == 0 and kind != 2, 'empty span encoding'
                continue
            assert offset % 256 == 0 and offset >= cursor, 'alignment or overlap'
            assert offset <= size and length <= size - offset, 'out of range'
            assert not any(payload[cursor:offset]), 'uninitialized padding'
            cursor = offset + length
            totals[kind] += length
            present[kind] += 1
            identity = (length, hashlib.sha256(payload[offset:offset + length]).digest())
            identities[kind][identity] = identities[kind].get(identity, 0) + 1
    assert cursor == size, 'unreferenced trailing payload'
    actual = 1469598103934665603
    for byte in payload:
        actual = ((actual ^ byte) * 1099511628211) & 0xffffffffffffffff
    assert actual == digest, 'payload hash'
    names = ('vertex', 'index', 'vs_constants', 'ps_constants')
    reuse = {}
    for kind, name in enumerate(names):
        unique = sum(identity[0] for identity in identities[kind])
        reuse[name] = dict(distinct_payloads=len(identities[kind]), unique_bytes=unique,
                           duplicate_bytes=totals[kind]-unique,
                           duplicate_fraction=(totals[kind]-unique)/totals[kind] if totals[kind] else 0)
    return dict(path=str(path.resolve()), sha256=hashlib.sha256(archive).hexdigest(),
                capture_frame=frame, draws=count, first_draw=first, last_draw=last,
                arena_bytes=size, payload_bytes=dict(zip(('vertex', 'index', 'vs_constants', 'ps_constants'), totals)),
                present_draws=dict(zip(('vertex', 'index', 'vs_constants', 'ps_constants'), present)),
                alignment_padding=size-sum(totals), input_archive_valid=True,
                same_frame_byte_reuse=reuse,
                stable_across_following_present_interval=True, replay_complete=False,
                limitation='Owned draw inputs only; no draw states, resource versions, descriptors, clears, copies or presentation. Byte identity does not establish cross-frame cache validity or a speedup. Not a performance measurement.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.archive)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
