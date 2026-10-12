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


def load_maps(text: String) raises -> List[Mapping]:
    # one Mapping per file: [lowest start, highest end), base = lowest start
    var out = List[Mapping]()
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
    # indices (from `among`) of the n largest non-zero values of field `key`, largest first, ties by position
    var ids = List[Int]()
    var vals = List[UInt64]()
    for i in among:
        var v = field(stacks[i], key)
        if v > 0:
            ids.append(i)
            vals.append(v)
    var order = List[Int](capacity=len(ids))
    for j in range(len(ids)):
        order.append(j)

    def before(a: Int, b: Int) {imm vals} -> Bool:
        return vals[a] > vals[b] or (vals[a] == vals[b] and a < b)

    sort(order, before)
    var picked = List[Int](capacity=min(n, len(order)))
    for j in range(min(n, len(order))):
        picked.append(ids[order[j]])
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


def elf_type(path: String) -> Int:  # 0 not ELF (a font, a data file); 2 ET_EXEC: the symbolizer wants the absolute address
    try:
        with open(path, "r") as f:
            var h = f.read_bytes(18)
            return Int(h[16]) if len(h) == 18 and h[0] == 0x7F and h[1] == 0x45 and h[2] == 0x4C and h[3] == 0x46 else 0
    except:
        return 0


def user_file(path: String) -> Bool:
    return path.startswith("/") and not path.startswith("/usr/") and not path.startswith("/opt/") and not ("/.venv/" in path)


@fieldwise_init
struct Frame(Copyable, Movable):
    var func: String  # symbol, py::name file, [jit runtime] or [module+0xoffset]
    var loc: String  # file:line:col from the symbolizer, "??..." when unknown
    var user: Bool  # the user's own code: own source file, own executable, a Python function
    var shown: Bool  # False: libheaptide's own frame, or an address in no mapping

    def text(self) -> String:
        if self.loc.startswith("??"):
            return self.func
        var slash = self.loc.rfind("/")
        return String(self.func, " ", String(self.loc[byte=slash + 1 :]) if slash >= 0 else self.loc)


def resolve_frames(ips: List[UInt64], roots: List[UInt64], maps: List[Mapping], dir: String, pid: String, mut root_names: List[String]) raises -> List[Frame]:
    # One llvm-symbolizer batch for these (distinct) addresses, plus the globals in roots ("DATA").
    var p_lo = List[UInt64]()  # CPython's perf map (-X perf): one stub range per Python function
    var p_hi = List[UInt64]()
    var p_name = List[String]()
    try:
        with open("/tmp/perf-" + pid + ".map", "r") as f:
            var text = f.read()
            for l in text.split("\n"):
                var f3 = l.split(" ", 2)
                if len(f3) == 3:
                    var nm = String(f3[2][byte=4:]) if f3[2].startswith("py::") else String(f3[2])
                    var colon = nm.find(":")
                    p_lo.append(hexval(f3[0]))
                    p_hi.append(p_lo[len(p_lo) - 1] + hexval(f3[1]))
                    p_name.append(String("py::", nm[byte=:colon], " ", nm[byte=nm.rfind("/") + 1 :]) if colon > 0 else String("py::", nm))
    except:
        pass
    var exe = String(maps[0].path[byte=maps[0].path.rfind("/") + 1 :]) if len(maps) > 0 else String()
    var fixed = List[Bool]()
    var elf = List[Bool]()
    for m in maps:
        var t = elf_type(m.path)
        fixed.append(t == 2)
        elf.append(t != 0)
    var req = String()
    var at = List[Int]()  # per address: its request, or -1 (never shown)
    var mods = List[String]()  # per request
    var pys = List[String]()  # a Python function or JIT runtime that names the frame, else ""
    var offs = List[UInt64]()  # module-relative address: names a frame no symbol table covers
    for ip in ips:
        var q = len(mods)
        for mi in range(len(maps)):
            ref m = maps[mi]
            if ip >= m.start and ip < m.end:
                if m.path.startswith("/memfd:"):  # JIT code (QML's V4): no symbols, named by its runtime
                    var tag = String(m.path[byte=: m.path.find(" (deleted)")]) if m.path.find(" (deleted)") > 0 else m.path
                    req += '"' + maps[0].path + '" 0x0\n'  # a quiet ?? answer
                    mods.append(String())
                    offs.append(0)
                    pys.append(String("[jit ", tag[byte=tag.rfind(":") + 1 :], "]"))
                elif not m.path.endswith("libheaptide.so"):  # outside an ELF file (a bad unwind into a font): unnamed
                    req += ('"' + m.path + '" 0x' + hex(ip - (0 if fixed[mi] else m.start) - 1)[byte=2:] + "\n") if elf[mi] else ('"' + maps[0].path + '" 0x0\n')
                    mods.append(String(m.path[byte=m.path.rfind("/") + 1 :]))
                    pys.append(String())
                    offs.append(ip - (0 if fixed[mi] else m.start))
                break
        else:  # in no file: a perf-map stub is a Python function
            for pi in range(len(p_lo)):
                if ip >= p_lo[pi] and ip < p_hi[pi]:
                    req += '"' + maps[0].path + '" 0x0\n'
                    mods.append(String())
                    offs.append(0)
                    pys.append(p_name[pi])
                    break
        at.append(q if len(mods) > q else -1)
    var nq = len(mods)
    for ri in range(len(roots)):  # globals holding a chain: "DATA" lookups answer name + "start size"
        var found = False
        for mi in range(len(maps)):
            ref m = maps[mi]
            if roots[ri] >= m.start and roots[ri] < m.end:
                offs.append(roots[ri] - (0 if fixed[mi] else m.start))
                req += 'DATA "' + m.path + '" 0x' + hex(offs[len(offs) - 1])[byte=2:] + "\n"
                mods.append(String(m.path[byte=m.path.rfind("/") + 1 :]))
                pys.append(String())
                found = True
                break
        if not found:
            mods.append(String())  # keeps root_names aligned with roots: no request, no answer
            pys.append(String("?"))
            offs.append(0)
    var inp = dir + "/.sym." + pid + ".in"
    var outp = dir + "/.sym." + pid + ".out"
    with open(inp, "w") as f:
        f.write(req)
    var cmd = String("I=", sq(inp), "\n", DEBUGINFOD, FIND_SYMBOLIZER, " --no-inlines < ", sq(inp), " > ", sq(outp), "\0")
    var rc = external_call["system", Int32](cmd.unsafe_ptr())
    var text = String()
    if rc == 3 << 8:  # no symbolizer: sites name modules only
        print("note: no llvm-symbolizer (install LLVM or set HEAPTIDE_SYMBOLIZER); sites show modules only", file=stderr)
        for _ in range(len(mods)):
            text += "??\n??:0\n\n"
    elif rc != 0:
        raise Error("FAIL symbolize: llvm-symbolizer failed")
    else:
        with open(outp, "r") as f:
            text = f.read()
    var lines = text.split("\n")
    var answers = List[Frame]()
    var li = 0
    for qi in range(len(mods)):
        if qi >= nq and pys[qi] == "?":  # a root in no mapping
            root_names.append(String("?"))
            continue
        while li < len(lines) and lines[li].byte_length() == 0:
            li += 1
        var func = String(lines[li]) if li < len(lines) else String("??")
        var loc = String(lines[li + 1]) if li + 1 < len(lines) else String("??:0")
        li += 2
        if pys[qi].byte_length() > 0:
            func = pys[qi]
        elif func == "??":
            func = "[" + mods[qi] + "+0x" + hex(offs[qi])[byte=2:] + "]"
        elif func.find("::") >= 0 and func.find("(") > 0:  # Mojo/C++: the signature is noise, keep the qualified name
            var name = String(func[byte=: func.find("(")])
            func = name^
        if qi >= nq:  # DATA: name, "start size", and on newer LLVM the declaration's file:line
            if li < len(lines) and lines[li].byte_length() > 0:
                li += 1
            root_names.append(String("global ", func, " (", mods[qi], ")") if not func.startswith("[") else func)
            continue
        # user frame: own source file, or (no debug info) a frame in the program's own executable
        var user = (user_file(String(loc[byte=: loc.find(":")])) if loc.find(":") > 0 else False) or (loc.startswith("??") and mods[qi] == exe and not exe.startswith("python")) or (pys[qi].byte_length() > 0 and not func.startswith("[jit "))  # a static python: its frames are the interpreter
        answers.append(Frame(func^, loc^, user, True))
    var frames = List[Frame]()
    for a in at:
        frames.append(answers[a].copy() if a >= 0 else Frame(String(), String("??"), False, False))
    return frames^


def symbolize(stacks: List[Stack], wanted: List[Int], roots: List[UInt64], maps: List[Mapping], dir: String, pid: String) raises -> Dict[Int, String]:
    # site per stack index: first 2 frames outside libheaptide + first frame in the user's own source
    var idx = Dict[UInt64, Int]()  # each address once in the batch (a frame shared by many stacks was asked many times)
    var uniq = List[UInt64]()
    for w in wanted:
        for ip in stacks[w].ips:
            if ip not in idx:
                idx[ip] = len(uniq)
                uniq.append(ip)
    var root_names = List[String]()
    var frames = resolve_frames(uniq, roots, maps, dir, pid, root_names)
    var sites = Dict[Int, String]()
    for w in wanted:
        var n = 0  # frames shown; 100 + n once a user frame is in
        for ip in stacks[w].ips:
            ref f = frames[idx[ip]]
            if not f.shown or n >= 100 or (n >= 2 and not f.user) or (f.func.startswith("[jit ") and w in sites and sites[w].endswith(f.func)):
                continue
            sites[w] = (sites[w] + " <- " + f.text()) if w in sites else f.text()
            n = 100 + n if f.user else n + 1
    for ri in range(len(roots)):
        if root_names[ri] != "?":  # in no mapping: unnamed, as the printer expects
            sites[-1 - ri] = root_names[ri]
    return sites^


def chain(held: Span[UInt64, _], w: Int) -> List[UInt64]:
    # who holds site w: its first holder's holder, up to 6 hops; ends at a root (bit 63 a global's
    # address, bit 62 a thread, bit 61 memory from before attach, bit 60 a memfd heap), at 0 (held by nothing recorded) or where a site repeats
    var out = List[UInt64]()
    var at = w
    while len(out) < 6 and at < len(held) and held[at] != 0:
        var h = held[at]
        if h in out:
            break
        out.append(h)
        if h >> 60 != 0:
            break
        at = Int(h) - 1
    return out^


def held_by(c: List[UInt64], sites: Dict[Int, String], roots: List[UInt64]) -> String:
    var t = String()
    for h in c:
        var name: String
        if h >> 63 == 1:
            name = String("global ?")
            for ri in range(len(roots)):
                if roots[ri] == h & ~(UInt64(1) << 63):
                    name = sites.get(-1 - ri, name)
        elif h >> 62 == 1:
            name = String("a thread's stack")
        elif h >> 61 == 1:
            name = String("memory from before attach")
        elif h >> 60 == 1:
            name = String("a memfd mapping (a VM or JIT heap)")
        else:
            name = sites.get(Int(h) - 1, String("?"))
        t += (" << " if t.byte_length() > 0 else "") + name  # "<-" already joins frames inside a site
    return t^


def tl_name(id: UInt64, stacks: List[Stack], sites: Dict[Int, String]) -> String:
    # a timeline row: a site (with its HIP stream, interned as a last frame with bit 63) or a pseudo id
    if id == 0xFFFFFFFF:
        return String("(device: held beyond live blocks)")
    if id == 0xFFFFFFFE:
        return String("(async pool: reserved, unused)")
    var w = Int(id) - 1
    var name = sites.get(w, String("?"))
    if w < len(stacks) and len(stacks[w].ips) > 0:
        var last = stacks[w].ips[len(stacks[w].ips) - 1]
        if last >> 63 == 1:
            name += String(" [stream 0x", hex(last & ~((UInt64(1) << 63) | 1))[byte=2:], "]")
    return name^


def tl_columns(b: List[UInt8], tl_at: Int, ntl: Int, id: UInt64, nsamp: Int) -> List[UInt64]:
    # 40 columns over the run, each the most this id held in its samples (values hold until changed)
    var cols = List[UInt64](length=40, fill=0)
    var hit = List[Bool](length=40, fill=False)  # a column no sample falls in carries the one before
    var at = 0
    var v: UInt64 = 0
    for k in range(ntl + 1):
        var s = nsamp if k == ntl else Int(u64(b, tl_at + 8 + k * 16) >> 32)
        if k < ntl and u64(b, tl_at + 8 + k * 16) & 0xFFFFFFFF != id:
            continue
        for x in range(at, min(s, nsamp)):
            cols[x * 40 // nsamp] = max(cols[x * 40 // nsamp], v)
            hit[x * 40 // nsamp] = True
        if k < ntl:
            at = s
            v = u64(b, tl_at + 16 + k * 16)
    for c in range(1, 40):
        if not hit[c]:
            cols[c] = cols[c - 1]
    return cols^


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


def site_key(s: String) -> String:
    # a site without its ":line:col": a change that only moves code keeps its sites
    # (a ":" is dropped only when digits follow it: "QString::append" keeps its "::")
    var out = String()
    var colon = False
    var skip = False
    for c in s.codepoint_slices():
        if c == ":":
            if colon:
                out += ":"
            colon = True
            skip = False
            continue
        if (colon or skip) and c.is_ascii_digit():
            colon = False
            skip = True
            continue
        if colon:
            out += ":"
        colon = False
        skip = False
        out += c
    if colon:
        out += ":"
    return out^


def load_sites(path: String) raises -> Dict[String, List[UInt64]]:
    # --sites file: direct, indirect, peak, live, kind, site per line; a site in several processes sums
    var d = Dict[String, List[UInt64]]()
    with open(path, "r") as f:
        for line in f.read().split("\n"):
            var p = line.split("\t")
            if len(p) != 6:
                continue
            var k = String(p[4], " ", site_key(String(p[5])))
            var v = d.get(k, List[UInt64](length=4, fill=0))
            for j in range(4):
                v[j] += UInt64(Int(p[j]))
            d[k] = v^
    return d^


def diff(a_path: String, b_path: String, fail_on: String, as_json: Bool) raises -> Int:
    # NEW: a leak site only NEW has; GROWN: direct or peak up > 64 KiB and > 10% (a new peak site too);
    # GONE / SHRUNK the other way. Exit 1 on NEW (fail_on new) or NEW/GROWN (grown).
    var a = load_sites(a_path)
    var b = load_sites(b_path)
    for k in b.keys():  # an unnamed frame is [module+offset]: it moves with every build
        if k.find("+0x") >= 0:
            print("heaptide diff: warning: unnamed frames (no llvm-symbolizer?) only match between identical binaries", file=stderr)
            break
    var groups: List[String] = ["NEW", "GROWN", "GONE", "SHRUNK"]
    var lines = List[List[String]](length=4, fill=List[String]())
    var json = String("{")
    var keys = List[String]()
    for e in b.items():
        keys.append(e.key)
    for e in a.items():
        if e.key not in b:
            keys.append(e.key)
    for k in keys:
        var x = a.get(k, List[UInt64](length=4, fill=0))
        var y = b.get(k, List[UInt64](length=4, fill=0))
        var g = -1
        if x[0] == 0 and y[0] > 0:
            g = 0
        elif y[0] == 0 and x[0] > 0:
            g = 2
        else:
            for j in [0, 2]:
                var lo = min(x[j], y[j])
                if max(x[j], y[j]) - lo > 65536 and max(x[j], y[j]) - lo > lo // 10:
                    g = 1 if y[j] > x[j] else 3
        if g >= 0:
            lines[g].append(String(human(x[0]), " -> ", human(y[0]), " direct, ", human(x[2]), " -> ", human(y[2]), " peak  ", k))
            json += String("," if json.byte_length() > 1 else "", '{"group":"', groups[g], '","site":', jstr(k), ',"direct":[', x[0], ",", y[0], '],"peak":[', x[2], ",", y[2], "]}")
    if as_json:
        print(String('{"changes":[', json[byte=1:], "]}"))
    else:
        for g in range(4):
            for l in lines[g]:
                print(groups[g], " ", l)
    if fail_on == "none":
        return 0
    return 1 if len(lines[0]) > 0 or (fail_on == "grown" and len(lines[1]) > 0) else 0


def metric(s: Stack, m: Int) -> UInt64:  # 0 leaked, 1 live, 2 peak, 3 allocated, 4 temporaries
    if m == 0:
        return s.direct + s.indirect
    if m == 1:
        return s.live_b
    if m == 2:
        return s.peak_b
    return s.bytes if m == 3 else s.temps


def flame(stacks: List[Stack], maps: List[Mapping], b: List[UInt8], smp_at: Int, nsamp: Int, dir: String, pid: String, attached: Bool, folded: Int) raises -> String:
    # Every stack that made or holds anything, merged into one call tree (root -> kind -> frames, outermost
    # first): JSON for heaptide flame, or with folded >= 0 one "f;f;f bytes" line per stack for any viewer.
    var idx = Dict[UInt64, Int]()
    var uniq = List[UInt64]()
    for st in stacks:
        for ip in st.ips:
            if ip not in idx:
                idx[ip] = len(uniq)
                uniq.append(ip)
    var none = List[String]()
    var fr = resolve_frames(uniq, List[UInt64](), maps, dir, pid, none)
    var exe = String(maps[0].path[byte=maps[0].path.rfind("/") + 1 :]) if len(maps) > 0 else String("?")
    if folded >= 0:
        var out = String()
        for st in stacks:
            var w = metric(st, folded)
            if w == 0:
                continue
            var line = String(exe, " (", pid, ");[", kind_name(st.kind), "]")
            for d in range(len(st.ips) - 1, -1, -1):
                ref f = fr[idx[st.ips[d]]]
                if f.shown:
                    line += ";" + f.text().replace(";", ":")
            out += line + " " + String(w) + "\n"
        return out^
    # frames: one per (function, file, line); strings interned
    var fid = List[Int]()  # per distinct address
    var keys = Dict[String, Int]()
    var names = Dict[String, Int]()
    var files = Dict[String, Int]()
    var f_name = List[Int]()
    var f_file = List[Int]()
    var f_line = List[Int]()
    var f_cat = List[Int]()  # 0 user, 1 python, 2 system, 3 jit
    for ref f in fr:
        if not f.shown:
            fid.append(-1)
            continue
        var c1 = f.loc.find(":")
        var c2 = f.loc.find(":", c1 + 1) if c1 >= 0 else -1
        var file = String(f.loc[byte=:c1]) if c1 > 0 and not f.loc.startswith("??") else String()
        var line = 0
        try:
            line = Int(f.loc[byte=c1 + 1 : c2 if c2 > 0 else f.loc.byte_length()]) if file.byte_length() > 0 else 0
        except:
            pass
        var k = String(f.func, "\t", file, "\t", line)
        if k not in keys:
            keys[k] = len(f_name)
            if f.func not in names:
                names[f.func] = len(names)
            if file not in files:
                files[file] = len(files)
            f_name.append(names[f.func])
            f_file.append(files[file])
            f_line.append(line)
            f_cat.append(1 if f.func.startswith("py::") else (3 if f.func.startswith("[jit ") else (0 if f.user else 2)))
        fid.append(keys[k])
    var parent: List[Int] = [-1]
    var frame: List[Int] = [-100]  # -100 the process, -1 - kind a kind node, else a frame
    var selfw = List[List[UInt64]]()
    for _ in range(5):
        selfw.append([UInt64(0)])
    var child = Dict[Int, Int]()  # parent * stride + frame -> node
    var stride = len(f_name) + 200
    var totals = List[UInt64](length=5, fill=0)
    for st in stacks:
        var cur = 0
        var d = len(st.ips)
        var fr_k = -1 - st.kind
        while True:  # the kind node, then the frames from the outermost in
            var key = cur * stride + fr_k + 101
            if key in child:
                cur = child[key]
            else:
                child[key] = len(parent)
                parent.append(cur)
                frame.append(fr_k)
                for m in range(5):
                    selfw[m].append(0)
                cur = len(parent) - 1
            fr_k = -1000
            while d > 0 and fr_k < 0:
                d -= 1
                fr_k = fid[idx[st.ips[d]]]
            if fr_k < 0:
                break
        for m in range(5):
            selfw[m][cur] += metric(st, m)
            totals[m] += metric(st, m)
    var o = String('{"pid":', pid, ',"exe":', jstr(maps[0].path if len(maps) > 0 else String("?")), ',"attached":', "true" if attached else "false",
                   ',"metrics":["leaked","live","peak","allocated","temporaries"],"totals":[')
    for m in range(5):
        o += String(totals[m]) + ("," if m < 4 else "]")
    def strs(d: Dict[String, Int]) -> String:
        var a = List[String](length=len(d), fill=String())
        for e in d.items():
            a[e.value] = jstr(e.key)
        return String("[", ",".join(a), "]")
    def ints(a: List[Int]) -> String:
        var t = String("[")
        for i in range(len(a)):
            t += String(a[i]) + ("," if i + 1 < len(a) else "")
        return t + "]"
    o += ',"names":' + strs(names) + ',"files":' + strs(files)
    o += ',"frames":{"name":' + ints(f_name) + ',"file":' + ints(f_file) + ',"line":' + ints(f_line) + ',"cat":' + ints(f_cat) + "}"
    o += ',"nodes":{"parent":' + ints(parent) + ',"frame":' + ints(frame) + ',"self":['
    for m in range(5):
        var t = String("[")
        for i in range(len(selfw[m])):
            t += String(selfw[m][i]) + ("," if i + 1 < len(selfw[m]) else "")
        o += t + ("]," if m < 4 else "]]}")
    # live bytes over time, at most 1500 points (each the bucket's highest)
    var step = max(1, (nsamp + 1499) // 1500)
    var ts = String()
    var cpu = String()
    var gpu = String()
    var t0 = u64(b, smp_at) if nsamp > 0 else 0
    var i = 0
    while i < nsamp:
        var hc: UInt64 = 0
        var hg: UInt64 = 0
        for j in range(i, min(i + step, nsamp)):
            hc = max(hc, u64(b, smp_at + j * 24 + 8))
            hg = max(hg, u64(b, smp_at + j * 24 + 16))
        var sep = "," if i > 0 else ""
        ts += sep + short(Float64(u64(b, smp_at + i * 24) - t0) / 1e9)
        cpu += sep + String(hc)
        gpu += sep + String(hg)
        i += step
    o += String(',"samples":{"t":[', ts, '],"cpu":[', cpu, '],"gpu":[', gpu, "]}")
    var py = List[String]()
    try:
        with open(dir + "/py." + pid + ".txt", "r") as f:
            for l in f.read().split("\n"):
                if l.byte_length() > 0:
                    py.append(jstr(String(l.strip())))
    except:
        pass
    return o + ',"python":[' + ",".join(py) + "]}"


def main() raises:
    var args = argv()
    if len(args) >= 4 and String(args[1]) == "--diff":
        var fo = String("new")
        var js = False
        for j in range(4, len(args)):
            if String(args[j]) == "--json":
                js = True
            elif String(args[j - 1]) == "--fail-on":
                fo = String(args[j])
        if fo not in ["new", "grown", "none"]:
            print("FAIL diff: --fail-on new|grown|none", file=stderr)
            exit(2)
        exit(diff(String(args[2]), String(args[3]), fo, js))
    if len(args) < 2:
        print("usage: heaptide report DIR [--json] [--top N] [--supp FILE] [--fail-on any|leaks|growth|none] [--growth-limit MIB]", file=stderr)
        exit(2)
    var dir = String(args[1])
    var as_json = False
    var n_top = 10
    var fail_on = String("any")
    var limit = 0.0  # --growth-limit, bytes; 0 = relative rule only
    var sites_out = String()  # --sites FILE: one line per site, for diff
    var flame_data = False  # --flame-data: the call tree as JSON, for heaptide flame
    var folded = -1  # --folded METRIC: "frame;frame;frame bytes" lines
    var flames = List[String]()
    # glibc's dlerror buffer: freed by libc at exit without passing through malloc's hooks
    var supp: List[String] = ["_dlerror_run"]
    var i = 2
    while i < len(args):
        var flag = String(args[i])
        if flag == "--json" or flag == "--flame-data":
            as_json = as_json or flag == "--json"
            flame_data = flame_data or flag == "--flame-data"
            i += 1
            continue
        if flag not in ["--top", "--growth-limit", "--fail-on", "--supp", "--sites", "--folded"]:
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
            elif flag == "--sites":
                sites_out = v
                with open(v, "w") as f:  # truncated once; each process appends its sites
                    f.write("")
            elif flag == "--folded":
                var ms: List[String] = ["leaked", "live", "peak", "allocated", "temporaries"]
                for m in range(len(ms)):
                    if ms[m] == v:
                        folded = m
                if folded < 0:
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
        var has_held = (u64(b, 48) & 4) == 4  # since v0.1.4: per site, its first holder
        var has_maps = (u64(b, 48) & 2) == 2  # since v0.1.3 the maps text ends the .bin; older: a .maps file
        var attached = (u64(b, 48) & 16) == 16  # since v0.1.5: live bytes start at 0 at attach, so no growth verdict
        var nmaps = Int(u64(b, 56)) if has_maps and u64(b, 56) < 1 << 31 else 0
        var ips_at = 64
        var rows_at = ips_at + nips * 8
        var dir_at = rows_at + n * ROW
        var ind_at = dir_at + n * 8
        var smp_at = ind_at + n * 8
        var blk_at = smp_at + nsamp * 24
        var held_at = blk_at + (n * 8 if has_blk else 0)
        var has_tl = (u64(b, 48) & 8) == 8  # since v0.1.4, --gpu-timeline: {sample << 32 | id, bytes} per change
        var tl_at = held_at + (n * 8 if has_held else 0)
        var ntl = Int(u64(b, tl_at)) if has_tl and len(b) >= tl_at + 8 and u64(b, tl_at) < 1 << 21 else 0
        var maps_at = tl_at + (8 + ntl * 16 if has_tl else 0)
        if len(b) < maps_at + nmaps or (has_maps and u64(b, 56) >= 1 << 31):
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

        var maps_text: String
        if has_maps:
            maps_text = String(from_utf8_lossy=Span(b)[maps_at : maps_at + nmaps])
        else:
            with open(dir + "/heaptide." + pid + ".maps", "r") as f:
                maps_text = f.read()
        var maps = load_maps(maps_text)
        if flame_data or folded >= 0:
            flames.append(flame(stacks, maps, b, smp_at, nsamp, dir, pid, attached, folded))
            continue
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
        var held = List[UInt64]()
        if has_held:
            for s in range(n):
                held.append(u64(b, held_at + s * 8))
        var roots = List[UInt64]()
        for w in sections[1]:  # PEAK sites: name every holder on their chains in the same batch
            for h in chain(held, w):
                if h >> 63 == 1:
                    roots.append(h & ~(UInt64(1) << 63))
                elif h >> 60 == 0 and not seen[Int(h) - 1]:
                    seen[Int(h) - 1] = True
                    wanted.append(Int(h) - 1)
        var tl_ids = List[UInt64]()  # timeline rows, with the most each held
        var tl_peak = List[UInt64]()
        for k in range(ntl):
            var id = u64(b, tl_at + 8 + k * 16) & 0xFFFFFFFF
            var v = u64(b, tl_at + 16 + k * 16)
            var i = 0
            while i < len(tl_ids) and tl_ids[i] != id:
                i += 1
            if i == len(tl_ids):
                tl_ids.append(id)
                tl_peak.append(0)
                if id < 0xFFFFFFFE and Int(id) - 1 < len(stacks) and not seen[Int(id) - 1]:
                    seen[Int(id) - 1] = True
                    wanted.append(Int(id) - 1)
            tl_peak[i] = max(tl_peak[i], v)
        var sites = symbolize(stacks, wanted, roots, maps, dir, pid)
        if sites_out.byte_length() > 0:
            var t = String()
            for w in wanted:
                t += String(stacks[w].direct, "\t", stacks[w].indirect, "\t", stacks[w].peak_b, "\t", stacks[w].live_b, "\t", kind_name(stacks[w].kind), "\t", sites.get(w, String("?")), "\n")
            with open(sites_out, "a") as f:
                f.write(t)

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
        var verdict = String("attached" if attached else "too short")
        var g = SIMD[DType.float64, 4](0)
        var settled = 0
        if nsamp >= 5 and not attached:
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
            json += '],"held":['  # PEAK sites with a recorded holder chain
            first = True
            for w in sections[1]:
                var c = chain(held, w)
                if len(c) > 0:
                    json += String("" if first else ",", '{"site":', jstr(sites.get(w, String("?"))), ',"held_by":', jstr(held_by(c, sites, roots)), '}')
                    first = False
            json += '],"gpu_timeline":['
            for i in range(len(tl_ids)):
                json += String("," if i > 0 else "", '{"site":', jstr(tl_name(tl_ids[i], stacks, sites)), ',"points":[')
                var firstp = True
                for k in range(ntl):
                    if u64(b, tl_at + 8 + k * 16) & 0xFFFFFFFF == tl_ids[i]:
                        var t = (u64(b, smp_at + Int(u64(b, tl_at + 8 + k * 16) >> 32) * 24) - u64(b, smp_at)) // 1000000
                        json += String("" if firstp else ",", "[", t, ",", u64(b, tl_at + 16 + k * 16), "]")
                        firstp = False
                json += "]}"
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
        if attached:
            print("GROWTH  n/a (attached: live bytes count from 0 at attach, a ramp is not growth; use heaptide diff)")
        else:
            print("GROWTH ", verdict, " slope", human(UInt64(max(slope, 0.0))), "/s  over window", human(UInt64(max(grow, 0.0))),
                  String(" (", Int(100.0 * max(grow, 0.0) / g[2]) if g[2] > 0 else 0, "% of mean)"),
                  ("  window from " + steady + " s") if steady.byte_length() > 0 else String())
        if len(tl_ids) > 0 and nsamp > 0:
            var ramp: List[String] = [" ", ".", ":", "-", "=", "+", "*", "#", "%", "@"]
            var order = List[Int]()  # the 5 rows that held the most
            for _ in range(min(5, len(tl_ids))):
                var best = -1
                for i in range(len(tl_ids)):
                    if i not in order and (best < 0 or tl_peak[i] > tl_peak[best]):
                        best = i
                order.append(best)
            var top_b = max(tl_peak[order[0]], 1)
            print("GPU TIMELINE (40 columns over", Float64((u64(b, smp_at + (nsamp - 1) * 24) - u64(b, smp_at)) // 100000000) / 10, "s, most held per column)")
            for i in order:
                var line = String()
                for c in tl_columns(b, tl_at, ntl, tl_ids[i], nsamp):
                    line += ramp[0 if c == 0 else max(1, Int(c * 9 // top_b))]
                print("  ", human(tl_peak[i]), "  ", line, "  ", tl_name(tl_ids[i], stacks, sites))
        var names: List[String] = ["PEAK (live at peak)", "HOTSPOTS (allocations)", "TEMPORARIES (alloc then free)"]
        for s in range(3):
            print(names[s])
            for w in sections[s + 1]:
                print("  ", field(stacks[w], s + 1), " ", kind_name(stacks[w].kind), "  ", sites.get(w, String("?")))
                var c = chain(held, w)
                if s == 0 and len(c) > 0:
                    print("      held by:", held_by(c, sites, roots))
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
    if folded >= 0:
        print("".join(flames), end="")
        exit(0)
    if flame_data:
        print("[" + ",".join(flames) + "]")
        exit(0)
    if as_json:
        print(json + "]")
    exit(1 if bad else 0)
