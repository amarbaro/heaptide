# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# heaptide report DIR [--json] [--top N] [--supp FILE] [--fail-on any|leaks|growth|none] [--growth-limit MIB]
# Reads DIR/heaptide.<pid>.{bin,maps} (+ py.<pid>.txt), symbolizes the reported stacks in one
# llvm-symbolizer batch, prints LEAKS / GROWTH / PEAK / HOTSPOTS / TEMPORARIES / PYTHON.
# Exit 0 clean, 1 leak or growth (per --fail-on), 2 tool error.
from std.sys import argv, exit, stderr
from std.os import listdir, getenv
from std.ffi import external_call

comptime ROW = 56
# $HEAPTIDE_SYMBOLIZER, else llvm-symbolizer on PATH, else ROCm's copy; exit 3 when none runs
comptime FIND_SYMBOLIZER = "S=${HEAPTIDE_SYMBOLIZER:-$(command -v llvm-symbolizer || echo /opt/rocm/llvm/bin/llvm-symbolizer)}; [ -x \"$S\" ] || exit 3; \"$S\""
# Stripped modules: swap in the debug file from elfutils' cache (shared with gdb), fetched by
# build-id from $DEBUGINFOD_URLS when missing. Reads the request file $I in place.
comptime DEBUGINFOD = """c=${XDG_CACHE_HOME:-$HOME/.cache}/debuginfod_client
cut -d'"' -f2 "$I" | sort -u | while IFS= read -r m; do
  readelf -S "$m" 2>/dev/null | grep -q debug_info && continue
  id=$(readelf -n "$m" 2>/dev/null | awk '/Build ID/{print $3}'); [ -n "$id" ] || continue
  d=$c/$id/debuginfo
  [ -s "$d" ] || [ -z "${DEBUGINFOD_URLS:-}" ] || { mkdir -p "$c/$id" && curl -sfL --max-time 120 "${DEBUGINFOD_URLS%% *}/buildid/$id/debuginfo" -o "$d.tmp" && mv "$d.tmp" "$d" || echo "note: debuginfod has no debug file for $m" >&2; }
  [ -s "$d" ] && sed -i "s|^\\"$m\\" |\\"$d\\" |" "$I"
done
"""


def kind_name(k: Int) -> String:
    return "gpu" if k == 1 else ("host" if k == 2 else ("mmap" if k == 3 else "cpu"))


def u64(b: List[UInt8], off: Int) -> UInt64:
    var v: UInt64 = 0
    for i in range(8):
        v |= UInt64(b[off + i]) << UInt64(8 * i)
    return v


def u32(b: List[UInt8], off: Int) -> UInt64:
    var v: UInt64 = 0
    for i in range(4):
        v |= UInt64(b[off + i]) << UInt64(8 * i)
    return v


@fieldwise_init
struct Stack(Copyable, Movable):
    var allocs: UInt64
    var bytes: UInt64
    var temps: UInt64
    var live_n: UInt64
    var live_b: UInt64
    var peak_b: UInt64
    var direct: UInt64
    var indirect: UInt64
    var leaked_n: UInt64  # leaked blocks (files from before v0.1.2: all live blocks)
    var kind: Int
    var ips: List[UInt64]


@fieldwise_init
struct Mapping(Copyable, Movable):
    var start: UInt64
    var end: UInt64
    var path: String


def hexval(s: StringSlice) raises -> UInt64:
    var v: UInt64 = 0
    for c in s.codepoints():
        var d = Int(c.to_u32())
        v = v * 16 + UInt64(d - 48 if d < 58 else d - 87)
    return v


def load_maps(path: String) raises -> List[Mapping]:
    # one Mapping per file: [lowest start, highest end), base = lowest start
    var out = List[Mapping]()
    var text: String
    with open(path, "r") as f:
        text = f.read()
    for line in text.split("\n"):
        var parts = line.split()
        var slash = line.find(" /")  # the path starts at the first " /" (fields 1-5 hold no slash)
        if len(parts) < 6 or slash < 0 or line[byte=slash + 1 :].startswith("/dev/"):
            continue
        var r = parts[0].split("-")
        var s = hexval(r[0])
        var e = hexval(r[1])
        var p = String(line[byte=slash + 1 :])
        var found = False
        for i in range(len(out)):
            if out[i].path == p:
                out[i].start = min(out[i].start, s)
                out[i].end = max(out[i].end, e)
                found = True
        if not found:
            out.append(Mapping(s, e, p))
    return out^


def top(stacks: List[Stack], key: Int, n: Int, among: List[Int]) -> List[Int]:
    # indices (from `among`) of the n largest non-zero values of field `key`, largest first
    var picked = List[Int]()
    var used = List[Bool](length=len(stacks), fill=False)
    for _ in range(n):
        var best = -1
        var bv: UInt64 = 0
        for i in among:
            var v = field(stacks[i], key)
            if not used[i] and v > bv:
                best = i
                bv = v
        if best < 0:
            break
        used[best] = True
        picked.append(best)
    return picked^


def field(s: Stack, key: Int) -> UInt64:
    if key == 0:
        return s.direct + s.indirect
    if key == 1:
        return s.peak_b
    if key == 2:
        return s.allocs
    return s.temps


def short(x: Float64) -> String:
    var s = String(x)
    return String(s[byte=: min(6, s.byte_length())])


def human(b: UInt64) -> String:
    if b >= 1 << 30:
        return short(Float64(b) / Float64(1 << 30)) + " GiB"
    if b >= 1 << 20:
        return short(Float64(b) / Float64(1 << 20)) + " MiB"
    if b >= 1 << 10:
        return short(Float64(b) / Float64(1 << 10)) + " KiB"
    return String(b) + " B"


def jstr(s: String) -> String:  # a JSON string literal: quotes, backslashes and control bytes escaped
    var out = String('"')
    for c in s.codepoints():
        var u = Int(c.to_u32())
        if u == 34 or u == 92:
            out += "\\" + chr(u)
        elif u < 32:
            out += "\\u00" + ("0" if u < 16 else "1") + hex(u % 16)[byte=2:]
        else:
            out += chr(u)
    return out + '"'


def sq(s: String) -> String:  # one shell word, whatever the path holds
    return "'" + s.replace("'", "'\\''") + "'"


def is_exec(path: String) -> Bool:  # ET_EXEC (non-PIE): the symbolizer wants the absolute address
    try:
        with open(path, "r") as f:
            var h = f.read_bytes(18)
            return len(h) == 18 and h[16] == 2
    except:
        return False


def user_file(path: String) -> Bool:
    return path.startswith("/") and not path.startswith("/usr/") and not path.startswith("/opt/") and not ("/.venv/" in path)


def symbolize(stacks: List[Stack], wanted: List[Int], maps: List[Mapping], dir: String, pid: String) raises -> Dict[Int, String]:
    # site per stack index: first 2 frames outside libheaptide + first frame in the user's own source
    var req = String()
    var keys = List[Int]()
    var mods = List[String]()
    var exe = String(maps[0].path[byte=maps[0].path.rfind("/") + 1 :]) if len(maps) > 0 else String()
    var fixed = List[Bool]()
    for m in maps:
        fixed.append(is_exec(m.path))
    for w in wanted:
        for ip in stacks[w].ips:
            for mi in range(len(maps)):
                ref m = maps[mi]
                if ip >= m.start and ip < m.end:
                    if not m.path.endswith("libheaptide.so"):
                        req += '"' + m.path + '" 0x' + hex(ip - (0 if fixed[mi] else m.start) - 1)[byte=2:] + "\n"
                        keys.append(w)
                        mods.append(String(m.path[byte=m.path.rfind("/") + 1 :]))
                    break
    var inp = dir + "/.sym." + pid + ".in"
    var outp = dir + "/.sym." + pid + ".out"
    with open(inp, "w") as f:
        f.write(req)
    var cmd = String("I=", sq(inp), "\n", DEBUGINFOD, FIND_SYMBOLIZER, " --no-inlines < ", sq(inp), " > ", sq(outp), "\0")
    var rc = external_call["system", Int32](cmd.unsafe_ptr())
    var text = String()
    if rc == 3 << 8:  # no symbolizer: sites name modules only
        print("note: no llvm-symbolizer (install LLVM or set HEAPTIDE_SYMBOLIZER); sites show modules only", file=stderr)
        for _ in range(len(keys)):
            text += "??\n??:0\n\n"
    elif rc != 0:
        raise Error("FAIL symbolize: llvm-symbolizer failed")
    else:
        with open(outp, "r") as f:
            text = f.read()
    var lines = text.split("\n")
    var sites = Dict[Int, String]()
    var shown = Dict[Int, Int]()  # frames shown; 100 + n once a user frame is in
    var li = 0
    for ki in range(len(keys)):
        var k = keys[ki]
        while li < len(lines) and lines[li].byte_length() == 0:
            li += 1
        if li + 1 >= len(lines):
            break
        var loc = String(lines[li + 1])
        var func = String(lines[li])
        if func == "??":
            func = "[" + mods[ki] + "]"
        li += 2
        var n = shown.get(k, 0)
        # user frame: own source file, or (no debug info) a frame in the program's own executable
        var user = (user_file(String(loc[byte=: loc.find(":")])) if loc.find(":") > 0 else False) or (loc.startswith("??") and mods[ki] == exe)
        # tracemalloc's hooks are heaptide's own instrumentation in --py mode, like libheaptide
        if n >= 100 or (n >= 2 and not user) or func.startswith("tracemalloc_"):
            continue
        var slash = loc.rfind("/")
        var frame = func if loc.startswith("??") else String(func, " ", String(loc[byte=slash + 1 :]) if slash >= 0 else loc)
        sites[k] = (sites[k] + " <- " + frame) if k in sites else frame
        shown[k] = 100 + n if user else n + 1
    return sites^


def fit(b: List[UInt8], smp_at: Int, a: Int, nsamp: Int, span: Float64, limit: Float64) -> SIMD[DType.float64, 4]:
    # least-squares fit of live bytes over samples [a, nsamp): (bytes/s, rise = slope x span, mean,
    # 1 when the rise is > 64 KiB and > 5% of the mean, or above `limit` bytes when that is set).
    # The rise is projected over the same span for every window, so a later, shorter window
    # cannot shrink a steady leak under the threshold; only a slope that really flattened passes.
    var m = Float64(nsamp - a)
    var sx = 0.0
    var sy = 0.0
    var sxx = 0.0
    var sxy = 0.0
    for k in range(a, nsamp):
        var at = smp_at + k * 24
        var x = Float64(u64(b, at) - u64(b, smp_at + a * 24)) / 1e9
        var y = Float64(u64(b, at + 8) + u64(b, at + 16))
        sx += x
        sy += y
        sxx += x * x
        sxy += x * y
    var den = m * sxx - sx * sx
    var slope = (m * sxy - sx * sy) / den if den > 0 else 0.0
    var grow = slope * span
    return SIMD[DType.float64, 4](slope, grow, sy / m, 1.0 if (grow > 65536.0 and grow > 0.05 * (sy / m)) or (limit > 0 and grow > limit) else 0.0)


def main() raises:
    var args = argv()
    if len(args) < 2:
        print("usage: heaptide report DIR [--json] [--top N] [--supp FILE] [--fail-on any|leaks|growth|none] [--growth-limit MIB]", file=stderr)
        exit(2)
    var dir = String(args[1])
    var as_json = False
    var n_top = 10
    var fail_on = String("any")
    var limit = 0.0  # --growth-limit, bytes; 0 = relative rule only
    # glibc's dlerror buffer: freed by libc at exit without passing through malloc's hooks
    var supp: List[String] = ["_dlerror_run"]
    var i = 2
    while i < len(args):
        var flag = String(args[i])
        if flag == "--json":
            as_json = True
            i += 1
            continue
        if flag not in ["--top", "--growth-limit", "--fail-on", "--supp"]:
            print("FAIL report: unknown option", flag, file=stderr)
            exit(2)
        if i + 1 >= len(args):
            print("FAIL report:", flag, "needs a value", file=stderr)
            exit(2)
        var v = String(args[i + 1])
        try:
            if flag == "--top":
                n_top = Int(v)
                if n_top < 1:
                    raise Error()
            elif flag == "--growth-limit":
                limit = Float64(v) * 1048576.0
                if not (limit > 0):
                    raise Error()
            elif flag == "--fail-on":
                fail_on = v
                if fail_on not in ["any", "leaks", "growth", "none"]:
                    raise Error()
            else:
                with open(v, "r") as f:
                    for l in f.read().split("\n"):
                        if l.byte_length() > 0 and not l.startswith("#"):
                            supp.append(String(l))
        except:
            print("FAIL report: bad value for", flag, ":", v, "(--top N >= 1, --growth-limit MIB > 0, --fail-on any|leaks|growth|none, --supp readable file)", file=stderr)
            exit(2)
        i += 2

    var bad = False
    var nbin = 0
    var quiet = List[String]()
    var json = String("[")
    var names_in_dir = List[String]()
    try:
        names_in_dir = listdir(dir)
    except:
        print("FAIL report: cannot read", dir, file=stderr)
        exit(2)
    # the main process (most call stacks) is never collapsed into QUIET
    var main_bin = String()
    var main_n: UInt64 = 0
    for name in names_in_dir:
        if name.startswith("heaptide.") and name.endswith(".bin"):
            with open(dir + "/" + name, "r") as f:
                var h = f.read_bytes(24)
                if len(h) == 24 and u64(h, 16) >= main_n:
                    main_n = u64(h, 16)
                    main_bin = name
    for name in names_in_dir:
        if not (name.startswith("heaptide.") and name.endswith(".bin")):
            continue
        nbin += 1
        var pid = String(name[byte=9 : name.byte_length() - 4])
        var b: List[UInt8]
        with open(dir + "/" + name, "r") as f:
            b = f.read_bytes()
        if len(b) < 64 or u64(b, 0) != 0x4544495450414548 or not pid.is_ascii_digit():
            print("FAIL report: bad header in", name, file=stderr)
            exit(2)
        # counts are bounded before any arithmetic (the shim writes < 2^24 stacks, < 2^31 frames,
        # < 2^21 samples), then the file must hold every section they promise
        if u64(b, 16) >= 1 << 25 or u64(b, 24) >= 1 << 31 or u64(b, 32) >= 1 << 21:
            print("FAIL report: impossible counts in", name, file=stderr)
            exit(2)
        var n = Int(u64(b, 16))
        var nips = Int(u64(b, 24))
        var nsamp = Int(u64(b, 32))
        var every = u64(b, 40)
        var has_blk = (u64(b, 48) & 1) == 1
        var ips_at = 64
        var rows_at = ips_at + nips * 8
        var dir_at = rows_at + n * ROW
        var ind_at = dir_at + n * 8
        var smp_at = ind_at + n * 8
        var blk_at = smp_at + nsamp * 24
        if len(b) < blk_at + (n * 8 if has_blk else 0):
            print("FAIL report: truncated file", name, file=stderr)
            exit(2)
        var stacks = List[Stack]()
        for s in range(n):
            var r = rows_at + s * ROW
            var off = Int(u32(b, r + 48))
            var depth = Int(b[r + 53])
            if off + depth > nips:
                print("FAIL report: stack", s + 1, "points past the frames in", name, file=stderr)
                exit(2)
            var ips = List[UInt64]()
            for d in range(depth):
                ips.append(u64(b, ips_at + (off + d) * 8))
            stacks.append(Stack(u64(b, r), u64(b, r + 8), u64(b, r + 16), u64(b, r + 24), u64(b, r + 32), u64(b, r + 40),
                                u64(b, dir_at + s * 8), u64(b, ind_at + s * 8),
                                u64(b, blk_at + s * 8) if has_blk else u64(b, r + 24), Int(b[r + 52]), ips^))

        var maps = load_maps(dir + "/heaptide." + pid + ".maps")
        var all_ids = List[Int]()
        var leaking = List[Int]()  # all of them: totals and suppressions must not stop at a display cap
        for s in range(len(stacks)):
            all_ids.append(s)
            if field(stacks[s], 0) > 0:
                leaking.append(s)
        var sections = [leaking^, top(stacks, 1, n_top, all_ids), top(stacks, 2, n_top, all_ids), top(stacks, 3, n_top, all_ids)]
        var wanted = List[Int]()
        var seen = List[Bool](length=len(stacks), fill=False)
        for sec in sections:
            for w in sec:
                if not seen[w]:
                    seen[w] = True
                    wanted.append(w)
        var sites = symbolize(stacks, wanted, maps, dir, pid)

        # LEAKS (after suppressions)
        var leak_d: UInt64 = 0
        var leak_i: UInt64 = 0
        var leak_lines = List[String]()
        var kept = List[Int]()
        for w in sections[0]:
            var site = sites.get(w, String("?"))
            var skip = False
            for p in supp:
                if p in site:
                    skip = True
            if not skip:
                kept.append(w)
                leak_d += stacks[w].direct
                leak_i += stacks[w].indirect
        var shown = top(stacks, 0, 1000, kept)  # JSON lists up to 1000, text up to --top
        for w in shown:
            var site = sites.get(w, String("?"))
            if len(leak_lines) < n_top:
                leak_lines.append(String("  ", human(stacks[w].direct + stacks[w].indirect), " (direct ", stacks[w].direct,
                                         ", indirect ", stacks[w].indirect, ", ", stacks[w].leaked_n, " blocks) ", kind_name(stacks[w].kind), "  ", site))

        # GROWTH: slope of live bytes after the first 20% of samples; when that fails, the window
        # start moves later (up to half the run) to skip a long setup, and the earliest window whose
        # slope, projected over the full span, passes wins
        var verdict = String("too short")
        var g = SIMD[DType.float64, 4](0)
        var settled = 0
        if nsamp >= 5:
            settled = nsamp // 5
            var span = Float64(u64(b, smp_at + (nsamp - 1) * 24) - u64(b, smp_at + settled * 24)) / 1e9
            g = fit(b, smp_at, settled, nsamp, span, limit)
            var j = 9
            while g[3] > 0 and j <= 20:
                var a = nsamp * j // 40
                var h = fit(b, smp_at, a, nsamp, span, limit)
                if h[3] == 0:
                    g = h
                    settled = a
                j += 1
            verdict = "FAIL" if g[3] > 0 else "PASS"
            if g[3] > 0 and (fail_on == "any" or fail_on == "growth"):
                bad = True
        var slope = g[0]
        var grow = g[1]
        var steady = String()
        if verdict == "PASS" and settled > nsamp // 5:
            steady = short(Float64(u64(b, smp_at + settled * 24) - u64(b, smp_at)) / 1e9)
        if leak_d + leak_i > 0 and (fail_on == "any" or fail_on == "leaks"):
            bad = True

        if as_json:
            if json.byte_length() > 1:
                json += ","
            json += String('{"pid":', pid, ',"direct":', leak_d, ',"indirect":', leak_i, ',"growth":"', verdict,
                           '","growth_bytes_per_s":', Int(slope), ',"growth_window_bytes":', Int(max(grow, 0.0)),
                           ',"window_from_s":', steady if steady.byte_length() > 0 else String("0"), ',"leaks":[')
            var first = True
            for w in shown:
                var site = sites.get(w, String("?"))
                if not first:
                    json += ","
                first = False
                json += String('{"bytes":', stacks[w].direct + stacks[w].indirect, ',"direct":', stacks[w].direct,
                               ',"indirect":', stacks[w].indirect, ',"blocks":', stacks[w].leaked_n, ',"kind":"',
                               kind_name(stacks[w].kind), '","site":', jstr(site), '}')
            json += "]}"
            continue

        var peak_sum: UInt64 = 0
        for st in stacks:
            peak_sum += st.peak_b
        if name != main_bin and peak_sum < 1 << 20 and leak_d + leak_i < 65536 and verdict != "FAIL":
            quiet.append(pid + " (" + (String(maps[0].path[byte=maps[0].path.rfind("/") + 1 :]) if len(maps) > 0 else String("?")) + ")")
            continue
        print("== heaptide pid", pid, " exe", maps[0].path if len(maps) > 0 else String("?"), " stacks", n, " samples", nsamp, "every", every, "ms")
        print("LEAKS  direct", human(leak_d), " indirect", human(leak_i))
        for l in leak_lines:
            print(l)
        print("GROWTH ", verdict, " slope", human(UInt64(max(slope, 0.0))), "/s  over window", human(UInt64(max(grow, 0.0))),
              String(" (", Int(100.0 * max(grow, 0.0) / g[2]) if g[2] > 0 else 0, "% of mean)"),
              ("  window from " + steady + " s") if steady.byte_length() > 0 else String())
        var names: List[String] = ["PEAK (live at peak)", "HOTSPOTS (allocations)", "TEMPORARIES (alloc then free)"]
        for s in range(3):
            print(names[s])
            for w in sections[s + 1]:
                print("  ", field(stacks[w], s + 1), " ", kind_name(stacks[w].kind), "  ", sites.get(w, String("?")))
        var py = dir + "/py." + pid + ".txt"
        try:
            with open(py, "r") as f:
                print("PYTHON (still allocated at exit, by line)")
                print(f.read())
        except:
            pass

    if len(quiet) > 0 and not as_json:
        var q = String()
        for x in quiet:
            q += " " + x
        print("QUIET (peak < 1 MiB, leaks < 64 KiB, no growth):" + q)
    if nbin == 0:
        print("FAIL report: no heaptide.*.bin in", dir, file=stderr)
        exit(2)
    if as_json:
        print(json + "]")
    exit(1 if bad else 0)
