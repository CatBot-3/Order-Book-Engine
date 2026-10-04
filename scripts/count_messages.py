#!/usr/bin/env python3
"""Independent ITCH message counter (success criterion 1).

Walks the two-byte big-endian length prefixes of an ITCH file and counts the
type byte of each record. It knows nothing else about the protocol and shares
no code with the C++ parser, which is the point: if `itch_stats --counts` and
this script print the same thing, the parser's framing and dispatch agree with
a second, much simpler reading of the file.

    scripts/count_messages.py data/01302019.NASDAQ_ITCH50
    scripts/count_messages.py data/01302019.NASDAQ_ITCH50.gz   # reads gzip too

Output: one "<type> <count>" line per type, in byte order, then "total <n>".
Exit status: 0 ok, 1 usage or I/O error, 2 the file ends inside a record.

A full day is a few hundred million records, so expect several minutes.
"""

import gzip
import sys

CHUNK = 64 * 1024 * 1024


def count(path):
    """Return (counts by type byte, total, error message or None)."""
    counts = [0] * 256
    total = 0
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rb") as stream:
        buf = b""
        consumed = 0  # bytes of the file fully processed before `buf`
        while True:
            chunk = stream.read(CHUNK)
            if chunk:
                buf = buf + chunk
            view = memoryview(buf)
            size = len(buf)
            pos = 0
            while size - pos >= 2:
                length = (view[pos] << 8) | view[pos + 1]
                if length == 0:
                    return counts, total, "zero-length record at offset %d" % (consumed + pos)
                end = pos + 2 + length
                if end > size:
                    break  # the record continues in the next chunk
                counts[view[pos + 2]] += 1
                total += 1
                pos = end
            view.release()
            consumed += pos
            buf = buf[pos:]
            if not chunk:
                break
        if buf:
            return counts, total, "file ends inside a record at offset %d" % consumed
    return counts, total, None


def main(argv):
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        sys.stderr.write(__doc__)
        return 1
    try:
        counts, total, error = count(argv[1])
    except OSError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1
    for byte, n in enumerate(counts):
        if n:
            print("%s %d" % (chr(byte), n))
    print("total %d" % total)
    if error:
        sys.stderr.write("error: %s\n" % error)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
