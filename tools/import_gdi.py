#!/usr/bin/env python3
"""Import a Dreamcast .gdi dump into a recomp game directory.

A .gdi file is a text manifest describing the raw track files of a GD-ROM
dump (Redump format). The game filesystem lives on the high-density data
track (track 3 on a typical dump). This tool:

  1. parses the .gdi manifest (read straight out of a .zip when the dump
     is still archived - only the needed data track is unpacked, to a
     temporary file),
  2. cooks the chosen data track from raw 2352-byte sectors down to
     2048-byte ISO 9660 sectors (mode 1, or mode 2 form 1 as a fallback),
  3. walks the ISO 9660 directory tree with a small built-in reader and
     extracts every file into the destination directory.

No third-party modules are needed - Python 3.8+ standard library only.

Usage:
  import_gdi.py <image.gdi|dump.zip> <dest-dir> [--track N] [--list]

Exit status: 0 ok, 1 usage error, 2 bad dump, 3 extraction problem.
"""
import argparse
import os
import struct
import sys
import tempfile
import zipfile

RAW_SECTOR = 2352
COOKED_SECTOR = 2048
MODE1_DATA_OFFSET = 16      # sync(12) + header(4) then user data
MODE2_FORM1_DATA_OFFSET = 24  # + subheader(8)


class GdiError(Exception):
    """The dump itself is malformed or unsupported."""


class Track:
    def __init__(self, number, lba, kind, sector_size, filename, offset):
        self.number = number
        self.lba = lba
        self.kind = kind
        self.sector_size = sector_size
        self.filename = filename
        self.offset = offset
        self.path = ""  # filled in by parse_gdi


def parse_gdi(path):
    """Return (tracks, zipfile_or_None) for a .gdi file or a zip holding one.

    Track .path values are only final for a plain .gdi; for a zipped dump
    they are resolved (and the needed track unpacked) later in extract().
    """
    zf = None
    if zipfile.is_zipfile(path):
        zf = zipfile.ZipFile(path)
        manifests = [m for m in zf.namelist()
                     if m.lower().endswith(".gdi") and not m.endswith("/")]
        if not manifests:
            zf.close()
            raise GdiError("no .gdi manifest inside " + path)
        if len(manifests) > 1:
            zf.close()
            raise GdiError("several .gdi files inside the zip; "
                           "unzip it and pick one")
        text = zf.read(manifests[0]).decode("ascii", errors="replace")
        base = None
    else:
        with open(path, "r", encoding="ascii", errors="replace") as f:
            text = f.read()
        base = os.path.dirname(os.path.abspath(path))
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if not lines:
        raise GdiError("empty .gdi manifest")
    try:
        count = int(lines[0])
    except ValueError:
        raise GdiError("first line of the .gdi must be the track count")
    tracks = []
    for ln in lines[1:]:
        parts = ln.split(None, 5)
        if len(parts) != 6:
            raise GdiError("bad track line: %r" % ln)
        number, lba, kind, sector_size, filename, offset = parts
        try:
            t = Track(int(number), int(lba), int(kind), int(sector_size),
                      filename.strip('"'), int(offset))
        except ValueError:
            raise GdiError("bad numbers in track line: %r" % ln)
        t.path = os.path.join(base, t.filename) if base else ""
        tracks.append(t)
    if len(tracks) != count:
        # Real-world dumps occasionally disagree with their own header;
        # the lines we have are more trustworthy than the count.
        sys.stderr.write("warning: manifest says %d tracks, lists %d\n"
                         % (count, len(tracks)))
    if not tracks:
        if zf:
            zf.close()
        raise GdiError("no tracks in manifest")
    return tracks, zf


def zip_member_for(zf, filename):
    """Find the zip member holding a track file, by name, any case/depth."""
    want = filename.replace("\\", "/").rstrip("/").split("/")[-1].lower()
    for m in zf.namelist():
        if m.rstrip("/").split("/")[-1].lower() == want:
            return m
    raise GdiError("track file %s is not inside the zip" % filename)


def unpack_track(zf, member, near_dir):
    """Stream one track out of the zip into a temp file; return its path.

    The temp file sits next to the destination: a GD-ROM data track is
    up to ~1.2 GB, far more than a small /tmp tmpfs can hold.
    """
    fd, tmp = tempfile.mkstemp(prefix=".import-gdi-", suffix=".track",
                               dir=near_dir)
    with os.fdopen(fd, "wb") as out, zf.open(member) as src:
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            out.write(chunk)
    return tmp


def pick_data_track(tracks, wanted=None):
    """Choose the track that holds the ISO 9660 filesystem.

    kind 4 marks a data track. The game filesystem is on the first
    high-density (LBA >= 45000) data track; single-session dumps only
    have one data track, which is then the answer by default.
    """
    data = [t for t in tracks if t.kind == 4]
    if wanted is not None:
        for t in tracks:
            if t.number == wanted:
                if t.kind != 4:
                    sys.stderr.write("warning: track %d is not marked data "
                                     "(type %d); trying it anyway\n"
                                     % (wanted, t.kind))
                return t
        raise GdiError("no track number %d in manifest" % wanted)
    if not data:
        # Some tools write the wrong type nibble; fall back to the
        # highest-LBA track, which is where GD-ROM data sessions live.
        return max(tracks, key=lambda t: t.lba)
    high = [t for t in data if t.lba >= 45000]
    return min(high, key=lambda t: t.lba) if high else max(data, key=lambda t: t.lba)


def cooked_stream(track):
    """Return (data_offset, raw_size, file, pvd_sector) for reading the ISO.

    Raw GD-ROM track files are 2352 bytes per sector with the user data
    at offset 16 (mode 1) or 24 (mode 2 form 1). A track dumped already
    cooked (2048 bytes/sector) is read straight through. pvd_sector is
    the track-relative sector where the CD001 primary descriptor sits
    (16 on a normal dump, later if the track carries a pregap).
    """
    if not os.path.isfile(track.path):
        raise GdiError("track file not found: %s" % track.path)
    size = os.path.getsize(track.path)
    if track.sector_size == COOKED_SECTOR:
        f = open(track.path, "rb")
        if track.offset:
            f.seek(track.offset)
        probe = f.read(COOKED_SECTOR * 512)
        pvd = next((s for s in range(len(probe) // COOKED_SECTOR)
                    if probe[s * COOKED_SECTOR + 1:s * COOKED_SECTOR + 6] == b"CD001"), 16)
        f.seek(track.offset)
        return 0, COOKED_SECTOR, f, pvd
    if track.sector_size != RAW_SECTOR:
        raise GdiError("unsupported sector size %d for %s "
                       "(want 2352 raw or 2048 cooked)"
                       % (track.sector_size, track.filename))
    if size < track.offset or (size - track.offset) < RAW_SECTOR * 17:
        raise GdiError("track file too small: %s" % track.filename)
    f = open(track.path, "rb")
    if track.offset:
        f.seek(track.offset)
    # Scan the lead-in for the primary volume descriptor (CD001), at both
    # candidate layouts; mode 1 is by far the common one for GD-ROMs.
    probe = f.read(RAW_SECTOR * 512)
    found = None
    for s in range(len(probe) // RAW_SECTOR):
        sec = probe[s * RAW_SECTOR:(s + 1) * RAW_SECTOR]
        if sec[MODE1_DATA_OFFSET + 1:MODE1_DATA_OFFSET + 6] == b"CD001" \
                and sec[MODE1_DATA_OFFSET] == 1:
            found = (s, MODE1_DATA_OFFSET)
            break
        if sec[MODE2_FORM1_DATA_OFFSET + 1:MODE2_FORM1_DATA_OFFSET + 6] == b"CD001" \
                and sec[MODE2_FORM1_DATA_OFFSET] == 1:
            found = (s, MODE2_FORM1_DATA_OFFSET)
            break
    if found is None:
        # Trust mode 1 at sector 16; the ISO reader will raise if wrong.
        sys.stderr.write("warning: no CD001 magic in the lead-in; "
                         "assuming mode 1 layout at sector 16\n")
        found = (16, MODE1_DATA_OFFSET)
    f.seek(track.offset)
    return found[1], RAW_SECTOR, f, found[0]


class IsoReader:
    """Minimal ISO 9660 (primary volume descriptor) reader.

    Dreamcast mastering quirk: GD-ROM filesystems record directory
    extents as ABSOLUTE disc LBAs (the high-density session begins at
    LBA 45000, so the root directory sits around 45020) rather than
    volume-relative ones. Detected by checking the root extent against
    the volume window [track_lba, track_lba + volume_size); extents are
    then translated back to track-relative sectors before reading.
    """

    def __init__(self, stream, raw_size, data_offset, pvd_sector, track_lba):
        self.f = stream
        self.raw_size = raw_size
        self.data_offset = data_offset
        self.base_lba = 0
        pvd = self._file_sector(pvd_sector)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise GdiError("no ISO 9660 primary volume descriptor "
                           "(sector %d is not CD001) - wrong track?" % pvd_sector)
        self.block_size = struct.unpack_from("<H", pvd, 128)[0]
        if self.block_size != COOKED_SECTOR:
            raise GdiError("unusual ISO block size %d" % self.block_size)
        volume_size = struct.unpack_from("<I", pvd, 80)[0]
        root = pvd[156:156 + 34]
        extent, size = self._record(root)
        if track_lba > 0 and track_lba <= extent < track_lba + max(volume_size, 1):
            self.base_lba = track_lba
            sys.stderr.write("note: GD-ROM absolute LBA addressing "
                             "(session base %d)\n" % self.base_lba)
        self.root_extent, self.root_size = extent, size
        self.pvd = pvd

    def _file_sector(self, n):
        """Read a cooked sector by its position inside the track file."""
        if n < 0:
            raise GdiError("negative sector lookup (bad extent table?)")
        self.f.seek(n * self.raw_size + self.data_offset)
        data = self.f.read(COOKED_SECTOR)
        if len(data) < COOKED_SECTOR:
            raise GdiError("short read at file sector %d" % n)
        return data

    def sector(self, n):
        """Read a cooked sector by its ISO (possibly absolute) LBA."""
        return self._file_sector(n - self.base_lba)

    def read_extent(self, lba, size):
        out = bytearray()
        sectors = (size + COOKED_SECTOR - 1) // COOKED_SECTOR
        for i in range(sectors):
            out += self.sector(lba + i)
        return bytes(out[:size])

    @staticmethod
    def _record(rec):
        extent = struct.unpack_from("<I", rec, 2)[0]
        size = struct.unpack_from("<I", rec, 10)[0]
        return extent, size

    def walk(self, lba=None, size=None, prefix=""):
        """Yield (path, extent, size, is_dir) for every file below root."""
        if lba is None:
            lba, size = self.root_extent, self.root_size
        data = self.read_extent(lba, size)
        pos = 0
        while pos < len(data):
            rec_len = data[pos]
            if rec_len == 0:
                # Skip to the next sector boundary.
                pos = (pos // COOKED_SECTOR + 1) * COOKED_SECTOR
                continue
            rec = data[pos:pos + rec_len]
            pos += rec_len
            name_len = rec[32]
            raw_name = rec[33:33 + name_len]
            if name_len == 1 and raw_name in (b"\x00", b"\x01"):
                continue  # . and ..
            name = raw_name.decode("ascii", errors="replace")
            if name.endswith(";1"):
                name = name[:-2]
            extent, fsize = self._record(rec)
            is_dir = bool(rec[25] & 2)
            path = prefix + name
            yield path, extent, fsize, is_dir
            if is_dir:
                yield from self.walk(extent, fsize, path + "/")


def safe_join(dest, rel):
    """Join, refusing anything that would escape dest."""
    rel = rel.replace("\\", "/").lstrip("/")
    parts = [p for p in rel.split("/") if p not in ("", ".", "..")]
    return os.path.join(dest, *parts) if parts else None


def extract(gdi_path, dest, track_number=None, list_only=False, progress=True):
    tracks, zf = parse_gdi(gdi_path)
    tmp = None
    try:
        track = pick_data_track(tracks, track_number)
        if zf is not None:
            member = zip_member_for(zf, track.filename)
            if progress:
                print("unpacking %s from the zip..." % member)
            near = os.path.abspath(dest)
            if not os.path.isdir(near):
                near = os.path.dirname(near) or "."
            tmp = unpack_track(zf, member, near)
            track.path = tmp
        if progress:
            print("data track: %d (%s, %d-byte sectors, lba %d)"
                  % (track.number, track.filename, track.sector_size, track.lba))
        data_offset, raw_size, stream, pvd_sector = cooked_stream(track)
        with stream:
            iso = IsoReader(stream, raw_size, data_offset, pvd_sector, track.lba)
            # gdmap.txt + ISO_META.BIN: the sector map the recomp hosts'
            # GD-ROM layer serves reads from (see runtime/gdrom.c). Map
            # LBAs are the ISO-recorded (for GD-ROMs: absolute) values;
            # ISO_META.BIN holds every non-file sector the guest can ask
            # for: the session system area, the volume descriptor set,
            # the path tables and all directory records.
            meta = bytearray()
            entries = []  # (lba, count, path, offset)

            def add_meta(lba, count):
                entries.append((lba, count, "ISO_META.BIN", len(meta)))
                meta.extend(iso.read_extent(lba, count * COOKED_SECTOR))

            if not list_only:
                add_meta(iso.base_lba + pvd_sector - 16, 16)  # system area
                lba = iso.base_lba + pvd_sector
                while True:  # descriptor set, terminator inclusive
                    d = iso.read_extent(lba, COOKED_SECTOR)
                    add_meta(lba, 1)
                    lba += 1
                    if d[0] == 255:
                        break
                ptsize = struct.unpack_from("<I", iso.pvd, 132)[0]
                ptsec = (ptsize + COOKED_SECTOR - 1) // COOKED_SECTOR
                ptl = struct.unpack_from("<I", iso.pvd, 140)[0]
                ptm = struct.unpack_from(">I", iso.pvd, 148)[0]
                for l in {ptl, ptm}:
                    if l:
                        add_meta(l, ptsec)
                # the root directory itself (walk only covers its children)
                add_meta(iso.root_extent,
                         (iso.root_size + COOKED_SECTOR - 1) // COOKED_SECTOR)
            files = 0
            bytes_done = 0
            for path, extent, size, is_dir in iso.walk():
                count = (size + COOKED_SECTOR - 1) // COOKED_SECTOR
                if is_dir:
                    if not list_only:
                        add_meta(extent, count)
                        target = safe_join(dest, path)
                        if target is not None:
                            os.makedirs(target, exist_ok=True)
                    continue
                if not list_only:
                    entries.append((extent, count, path, 0))
                files += 1
                bytes_done += size
                if list_only:
                    print("%12d  %s" % (size, path))
                    continue
                target = safe_join(dest, path)
                if target is None:
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as out:
                    out.write(iso.read_extent(extent, size))
                if progress:
                    print("%12d  %s" % (size, path))
            if files == 0:
                raise GdiError("the filesystem on track %d is empty" % track.number)
            if not list_only:
                entries.sort(key=lambda e: e[0])
                with open(os.path.join(dest, "gdmap.txt"), "w",
                          encoding="ascii") as m:
                    for l, c, n, o in entries:
                        m.write("%d %d %s %d\n" % (l, c, n, o))
                with open(os.path.join(dest, "ISO_META.BIN"), "wb") as mf:
                    mf.write(meta)
            if progress or list_only:
                print("%d file(s), %.1f MiB%s" % (
                    files, bytes_done / 1048576.0,
                    "" if list_only else " -> " + os.path.abspath(dest)))
            return files
    finally:
        if zf is not None:
            zf.close()
        if tmp is not None:
            try:
                os.unlink(tmp)
            except OSError:
                pass


def main(argv=None):
    ap = argparse.ArgumentParser(description="Extract the game filesystem "
                                     "from a Dreamcast .gdi dump.")
    ap.add_argument("gdi", help="path to the .gdi manifest")
    ap.add_argument("dest", help="directory to extract into (the game's "
                    "disc/ folder)")
    ap.add_argument("--track", type=int, default=None,
                    help="data track number (default: first high-density "
                    "data track)")
    ap.add_argument("--list", action="store_true",
                    help="list the filesystem without extracting")
    args = ap.parse_args(argv)
    try:
        if not args.list:
            if os.path.islink(args.dest):
                raise GdiError("destination is a symbolic link: " + args.dest)
            os.makedirs(args.dest, exist_ok=True)
        extract(args.gdi, args.dest, args.track, args.list)
    except GdiError as e:
        print("import-gdi: %s" % e, file=sys.stderr)
        return 2
    except OSError as e:
        print("import-gdi: %s" % e, file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
