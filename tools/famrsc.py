"""Road Rash 95 RSC container + PKWARE DCL 'explode' (method 1) decompressor.
Port of zlib/contrib/blast/blast.c (Mark Adler). Usage: py -I famrsc.py FILE.RSC [id]"""
import struct, sys, zlib

# --- blast (PKWARE DCL explode) ---
LITLEN = [11,124,8,7,28,7,188,13,76,4,10,8,12,10,12,10,8,23,8,9,7,6,7,8,7,6,55,8,23,24,12,11,7,9,11,12,6,7,22,5,7,24,6,11,9,6,7,22,7,11,38,7,9,8,25,11,8,11,9,12,8,12,5,38,5,38,5,11,7,5,6,21,6,10,53,8,7,24,10,27,44,253,253,253,252,252,252,13,12,45,12,45,12,61,12,45,44,173]
LENLEN = [2,35,36,53,38,23]
DISTLEN = [2,20,53,230,247,151,248]
BASE = [3,2,4,5,6,7,8,9,10,12,16,24,40,72,136,264]
EXTRA = [0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8]

def construct(rep):
    lengths = []
    for b in rep:
        lengths += [(b & 15)] * ((b >> 4) + 1)
    n = len(lengths)
    count = [0]*14
    for l in lengths: count[l] += 1
    offs = [0]*14
    for i in range(1, 13): offs[i+1] = offs[i] + count[i]
    sym = [0]*n
    for s, l in enumerate(lengths):
        if l: sym[offs[l]] = s; offs[l] += 1
    return count, sym

class Bits:
    def __init__(s, d): s.d=d; s.p=0; s.buf=0; s.cnt=0
    def bits(s, need):
        v = s.buf
        while s.cnt < need:
            if s.p >= len(s.d): raise EOFError
            v |= s.d[s.p] << s.cnt; s.p += 1; s.cnt += 8
        s.buf = v >> need; s.cnt -= need
        return v & ((1 << need) - 1)
    def decode(s, h):
        count, sym = h
        code = first = index = 0
        bitbuf, left = s.buf, s.cnt
        length = 1
        while True:
            while left:
                code |= (bitbuf & 1) ^ 1   # codes are inverted
                bitbuf >>= 1
                c = count[length]
                if code < first + c:
                    s.buf, s.cnt = bitbuf, (s.cnt - length) & 7
                    return sym[index + (code - first)]
                index += c; first += c; first <<= 1; code <<= 1
                length += 1; left -= 1
            left = 13 - length   # MAXBITS+1-len
            if left <= 0: raise ValueError("bad code")
            if s.p >= len(s.d): raise EOFError
            bitbuf = s.d[s.p]; s.p += 1
            if left > 8: left = 8

H_LIT, H_LEN, H_DIST = construct(LITLEN), construct(LENLEN), construct(DISTLEN)

def explode(data):
    b = Bits(data); out = bytearray()
    lit = b.bits(8); dictb = b.bits(8)
    if lit > 1 or not 4 <= dictb <= 6: raise ValueError("bad header")
    while True:
        if b.bits(1):
            sym = b.decode(H_LEN)
            ln = BASE[sym] + b.bits(EXTRA[sym])
            if ln == 519: break
            sym = 2 if ln == 2 else dictb
            dist = (b.decode(H_DIST) << sym) + b.bits(sym) + 1
            if dist > len(out): raise ValueError("distance too far")
            for _ in range(ln): out.append(out[-dist])
        else:
            out.append(b.decode(H_LIT) if lit else b.bits(8))
    return bytes(out)

# --- RSC container ---
def read_rsc(path):
    d = open(path, 'rb').read()
    tag, ln, cnt = struct.unpack_from('<4sII', d, 0x38)
    ents = []
    for i in range(cnt):
        fcc, rid, off, length = struct.unpack_from('<4sIII', d, 0x48 + i*0x20)
        ents.append((fcc[::-1].decode('latin1'), rid, off, length))
    return d, ents

def get(d, ent):
    _, rid, off, length = ent
    raw = d[off:off+length]
    usize, csize, crc, method = struct.unpack_from('<IIII', raw, 0)
    # Stored (uncompressed) chunks start directly with a chunk tag, so require a consistent header.
    if method != 1 or csize != length - 16 or usize == 0: return raw, None
    out = explode(raw[16:16+csize])
    return out, (usize, len(out), crc, zlib.crc32(out))

if __name__ == '__main__':
    d, ents = read_rsc(sys.argv[1])
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    for e in ents:
        if e[1] == want:
            out, info = get(d, e)
            print(e, info)
            if info: print('size ok', info[0]==info[1], 'crc ok', info[2]==info[3])
            if len(sys.argv) > 3: open(sys.argv[3], 'wb').write(out)
