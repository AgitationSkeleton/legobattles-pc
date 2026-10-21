#!/usr/bin/env python3
"""Render a LEGO Battles UI sound effect from the player's own ROM to a WAV, to audition notification sounds.

    python tools/lb_sfx.py <rom.nds> <out_dir> [SE_UI_SELECT SE_UI_ACCEPT ...]    (default: a set of UI candidates)
    python tools/lb_sfx.py <rom.nds> --list                                     every SE_ name in the archive

Sound/sound_data.sdat: the effect is a sequence in a sequence archive (SSAR), played by an instrument bank
(SBNK) whose samples are in wave archives (SWAR). This follows the effect's sequence to its first notes and
renders each note's sample at that note's pitch, with a simple release. Not a full sequence player: enough to
hear what an effect is. Nothing from the ROM is written anywhere but out_dir. docs/427.
"""
from __future__ import annotations

import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

DEFAULT = ["SE_UI_SELECT", "SE_UI_ACCEPT", "SE_UI_CLICK1", "SE_UI_ENTER1", "SE_UI_GROUPSET",
           "SE_UI_OBJECTIVECOMPLETE", "SE_UI_SCREENSWAP", "SE_UI_MAGIC1", "SE_FE_SELECT", "SE_FE_ACCEPT"]


def nds_file(rom: bytes, want: str) -> bytes:
    fnt_off, fnt_size, fat_off, _ = struct.unpack_from("<IIII", rom, 0x40)
    fnt = rom[fnt_off:fnt_off + fnt_size]

    def walk(dir_id, prefix):
        sub_off, first_id, _ = struct.unpack_from("<IHH", fnt, (dir_id & 0xFFF) * 8)
        p, fid = sub_off, first_id
        while fnt[p]:
            t = fnt[p]
            name = fnt[p + 1:p + 1 + (t & 0x7F)].decode("latin-1")
            p += 1 + (t & 0x7F)
            if t & 0x80:
                r = walk(struct.unpack_from("<H", fnt, p)[0], prefix + name + "/")
                p += 2
                if r is not None:
                    return r
            else:
                if prefix + name == want:
                    s, e = struct.unpack_from("<II", rom, fat_off + fid * 8)
                    return rom[s:e]
                fid += 1
        return None

    data = walk(0xF000, "")
    if data is None:
        raise SystemExit(f"{want} not in the ROM")
    return data


class Sdat:
    def __init__(self, d: bytes):
        self.d = d
        symb, _, info, _, fat, _ = struct.unpack_from("<6I", d, 0x10)
        self.symb, self.info, self.fat = symb, info, fat

    def record(self, base: int, n: int) -> list[int]:
        off = struct.unpack_from("<I", self.d, base + 8 + 4 * n)[0]
        count = struct.unpack_from("<I", self.d, base + off)[0]
        return [struct.unpack_from("<I", self.d, base + off + 4 + 4 * i)[0] for i in range(count)]

    def file(self, fid: int) -> bytes:
        off, size = struct.unpack_from("<II", self.d, self.fat + 12 + 16 * fid)
        return self.d[off:off + size]

    def cstr(self, off: int) -> str:
        return self.d[off:self.d.index(b"\0", off)].decode("latin-1")

    def effects(self) -> dict[str, tuple[int, int]]:
        """SE name -> (seqarc index, sequence index in it)."""
        out = {}
        arcs_off = struct.unpack_from("<I", self.d, self.symb + 8 + 4)[0]   # record 1: SEQARC
        count = struct.unpack_from("<I", self.d, self.symb + arcs_off)[0]
        for a in range(count):
            _, sub = struct.unpack_from("<II", self.d, self.symb + arcs_off + 4 + 8 * a)
            if not sub:
                continue
            n = struct.unpack_from("<I", self.d, self.symb + sub)[0]
            for s in range(n):
                no = struct.unpack_from("<I", self.d, self.symb + sub + 4 + 4 * s)[0]
                if no:
                    out[self.cstr(self.symb + no)] = (a, s)
        return out


def varlen(b: bytes, p: int) -> tuple[int, int]:
    v = 0
    while True:
        c = b[p]
        p += 1
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, p


def notes(seq: bytes, start: int, limit: int = 8) -> list[tuple[int, int, int, int]]:
    """(program, note, velocity, duration ticks) of the first notes, following track opens too."""
    out, todo, seen = [], [start], set()
    while todo and len(out) < limit:
        p = todo.pop(0)
        if p in seen or p >= len(seq):
            continue
        seen.add(p)
        prog = 0
        for _ in range(400):
            if p >= len(seq):
                break
            c = seq[p]
            p += 1
            if c < 0x80:
                vel = seq[p]
                dur, p = varlen(seq, p + 1)
                out.append((prog, c, vel, dur))
                if len(out) >= limit:
                    break
            elif c in (0x80,):
                _, p = varlen(seq, p)
            elif c == 0x81:
                prog, p = varlen(seq, p)
            elif c == 0x93:
                todo.append(seq[p + 1] | (seq[p + 2] << 8) | (seq[p + 3] << 16))
                p += 4
            elif c in (0x94, 0x95):
                p += 3
            elif 0xA0 <= c <= 0xA2:
                p += 1   # a prefix: the next command is read normally (approximation)
            elif 0xB0 <= c <= 0xBD:
                p += 3
            elif 0xC0 <= c <= 0xD7:
                p += 1
            elif 0xE0 <= c <= 0xE3:
                p += 2
            elif c == 0xFE:
                p += 2
            elif c in (0xFC, 0xFD):
                pass
            elif c == 0xFF:
                break
    return out


def instrument(bank: bytes, prog: int, note: int):
    """(swav, swar slot, base note, release) for program `prog` at `note`, or None."""
    count = struct.unpack_from("<I", bank, 0x38)[0]
    if prog >= count:
        return None
    t, off = bank[0x3C + 4 * prog], struct.unpack_from("<H", bank, 0x3C + 4 * prog + 1)[0]
    if t in (1, 2, 3, 4, 5):
        swav, swar, base = struct.unpack_from("<HHB", bank, off)
        return swav, swar, base, bank[off + 8]
    if t == 0x10:
        lo, hi = bank[off], bank[off + 1]
        if not lo <= note <= hi:
            return None
        e = off + 2 + (note - lo) * 12
        swav, swar, base = struct.unpack_from("<HHB", bank, e + 2)
        return swav, swar, base, bank[e + 10]
    if t == 0x11:
        keys = bank[off:off + 8]
        r = next((i for i, k in enumerate(keys) if k and note <= k), None)
        if r is None:
            return None
        e = off + 8 + r * 12
        swav, swar, base = struct.unpack_from("<HHB", bank, e + 2)
        return swav, swar, base, bank[e + 10]
    return None


IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8] * 2
IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
            88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
            658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
            3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
            16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]


def swav(swar: bytes, idx: int) -> tuple[list[int], int]:
    off = struct.unpack_from("<I", swar, 0x3C + 4 * idx)[0]
    kind, _, rate, _, loop_start, loop_len = struct.unpack_from("<BBHHHI", swar, off)
    data = swar[off + 12: off + 12 + (loop_start + loop_len) * 4]
    if kind == 0:
        return [((b ^ 0x80) - 128) << 8 for b in data], rate
    if kind == 1:
        return list(struct.unpack_from(f"<{len(data) // 2}h", data)), rate
    s, i = struct.unpack_from("<hh", data, 0)
    out = []
    for b in data[4:]:
        for nib in (b & 0xF, b >> 4):
            step = IMA_STEP[i]
            diff = step >> 3
            if nib & 1: diff += step >> 2
            if nib & 2: diff += step >> 1
            if nib & 4: diff += step
            s = max(-32768, min(32767, s - diff if nib & 8 else s + diff))
            i = max(0, min(88, i + IMA_INDEX[nib]))
            out.append(s)
    return out, rate


def render(sd: Sdat, arc: int, seq_i: int) -> list[float]:
    arc_info = sd.record(sd.info, 1)[arc]
    ssar = sd.file(struct.unpack_from("<H", sd.d, sd.info + arc_info)[0])
    data_off, count = struct.unpack_from("<II", ssar, 0x18)
    s_off, bank_id = struct.unpack_from("<IH", ssar, 0x20 + 12 * seq_i)
    bank_info = sd.info + sd.record(sd.info, 2)[bank_id]
    bank = sd.file(struct.unpack_from("<H", sd.d, bank_info)[0])
    wave_arcs = struct.unpack_from("<4H", sd.d, bank_info + 4)
    out: list[float] = []
    at = 0.0
    for prog, note, vel, dur in notes(ssar[data_off:], s_off):
        ins = instrument(bank, prog, note)
        if not ins:
            continue
        wv, slot, base, _ = ins
        wa = wave_arcs[slot] if slot < 4 else 0xFFFF
        if wa == 0xFFFF:
            continue
        swar = sd.file(struct.unpack_from("<H", sd.d, sd.info + sd.record(sd.info, 3)[wa])[0])
        pcm, rate = swav(swar, wv)
        step = rate * 2 ** ((note - base) / 12) / 32000.0
        length = min(int(len(pcm) / step), 32000)   # at most a second per note
        start = int(at)
        if len(out) < start + length:
            out.extend([0.0] * (start + length - len(out)))
        for k in range(length):
            j = int(k * step)
            if j >= len(pcm):
                break
            fade = 1.0 if k < length * 0.8 else (length - k) / (length * 0.2)
            out[start + k] += pcm[j] / 32768.0 * (vel / 127.0) * fade
        at += dur * 32000 / 240.0 * 2   # ticks at 120 bpm, 48 per beat: 1 tick ~ 1/96 s, roughly
    return out


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    rom = open(sys.argv[1], "rb").read()
    sd = Sdat(nds_file(rom, "Sound/sound_data.sdat"))
    fx = sd.effects()
    if sys.argv[2] == "--list":
        for name in sorted(fx):
            print(name)
        return 0
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    for name in sys.argv[3:] or DEFAULT:
        if name not in fx:
            print(f"{name}: not found")
            continue
        pcm = render(sd, *fx[name])
        if not pcm:
            print(f"{name}: no renderable note")
            continue
        peak = max(1e-6, max(abs(v) for v in pcm))
        path = os.path.join(out_dir, name + ".wav")
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(32000)
            w.writeframes(b"".join(struct.pack("<h", int(v / peak * 26000)) for v in pcm))
        print(f"{name}: {len(pcm) / 32000:.2f} s -> {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
