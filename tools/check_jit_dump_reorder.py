#!/usr/bin/env python3
"""フレーム内の領域を**並べ替えただけ**であることを、生成コードから確かめる。

## なぜ別の判定が要るのか

`tools/check_jit_dump_identity.py` は「バイト列のハッシュが一致すること」を見る。
これは**生成コードを 1 バイトも変えていない**ことの判定で、強い。

**しかし並べ替えには使えない。** 並べ替えはフレーム内の領域の位置を変えるので、
**rsp 相対変位(disp32)の値が変わる**。これは並べ替えの定義そのものであって、
変わらなければ並べ替えられていない。実測(2026-10-30、案 d 第 2 段):

    size(byte 長が変わった)                      0 件
    maskedbytes(アドレス即値の数/位置が変わった)   0 件
    hash のみ(長さは同じでバイト列が変わった)      1340 件  ← 全関数

つまり素のハッシュは**必ず全関数で落ちる**ので、「並べ替え以外は何も変わっていない」
ことを示せない。

## この判定が見るもの

za.c の JIT_DUMP は、変位の 4 byte を伏せた 2 本目のハッシュ(`disphash=`)を出す。
伏せる位置は値ではなく**出力した場所**で決まる(`jit_emit_rsp_disp32` が印をつける)。
フレーム内の番地参照は例外なくこの 1 つの出口を通るので、漏れない。

したがって並べ替えの合否は次で判定できる:

| 量 | 並べ替えで | それ以外の変更で |
|---|---|---|
| 関数の数 | 同じ | 変わりうる |
| `size`(byte 長) | **同じ** | 変わりうる |
| `maskedbytes` | **同じ** | 変わりうる |
| `dispbytes`(変位の byte 数) | **同じ** | 変わりうる |
| **`disphash`** | **同じ** | **変わる** |
| `hash` | **変わる**(変位の値が変わるため) | 変わる |

**`hash` が全関数で一致してしまった場合も落とす。** 変位が 1 つも変わっていない
= 並べ替えが効いていないということなので、これも異常である(--expect-moved)。

## 使い方

    python3 tools/check_jit_dump_reorder.py 基準のシリアルログ 今回のシリアルログ
    python3 tools/check_jit_dump_reorder.py --self-test

`make jit-reorder-check` が、保存済みの基準(tmp/identity/jit/save.serial.txt)に対して
今回のブートを突き合わせる。
"""

import re
import sys
import tempfile

#: za.c が出す行。dispbytes / disphash は 2026-10-30 に足した(古い基準には無い)
DUMP_RE = re.compile(
    r"\[JITDUMP\]\s+size=(\d+)\s+maskedbytes=(\d+)\s+hash=([0-9a-f]{16})"
    r"(?:\s+dispbytes=(\d+)\s+disphash=([0-9a-f]{16}))?")


def parse_log(path):
    """シリアルログから [JITDUMP] 行を**出現順に**抜き出す。"""
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = DUMP_RE.search(line)
            if m:
                rows.append({
                    "size": int(m.group(1)),
                    "maskedbytes": int(m.group(2)),
                    "hash": m.group(3),
                    "dispbytes": int(m.group(4)) if m.group(4) is not None else None,
                    "disphash": m.group(5),
                })
    return rows


def check(base, cur, quiet=False, expect_moved=True):
    """並べ替えだけであることを判定する。戻り値は終了コード。"""
    out = (lambda *a: None) if quiet else (lambda *a: print(*a))

    if not base or not cur:
        print("ERROR: [JITDUMP] 行が無い(基準 %d 行 / 今回 %d 行)。"
              "JIT_DUMP=1 のビルドで流したか確認すること" % (len(base), len(cur)),
              file=sys.stderr)
        return 2

    missing = [n for n, r in (("基準", base[0]), ("今回", cur[0])) if r["disphash"] is None]
    if missing:
        print("ERROR: %s のダンプに disphash= が無い。**変位を伏せたハッシュを出す "
              "za.c で取り直すこと**(2026-10-30 に足した)" % " と ".join(missing),
              file=sys.stderr)
        return 2

    out("=== 並べ替えだけであることの判定 ===")
    out("    | 量 | 基準 | 今回 | 差 |")
    out("    |---|---:|---:|---:|")
    out("    | 関数の数 | %d | %d | %+d |" % (len(base), len(cur), len(cur) - len(base)))
    for key in ("size", "maskedbytes", "dispbytes"):
        b = sum(r[key] for r in base)
        c = sum(r[key] for r in cur)
        out("    | %s の合計 | %d | %d | %+d |" % (key, b, c, c - b))

    if len(base) != len(cur):
        print("NG: 関数の数が違う。**並べ替えだけではない**", file=sys.stderr)
        return 1

    bad = {"size": 0, "maskedbytes": 0, "dispbytes": 0, "disphash": 0}
    first = {}
    moved = 0
    for i, (b, c) in enumerate(zip(base, cur)):
        for key in ("size", "maskedbytes", "dispbytes", "disphash"):
            if b[key] != c[key]:
                bad[key] += 1
                first.setdefault(key, (i, b[key], c[key]))
        if b["hash"] != c["hash"]:
            moved += 1

    out("")
    out("    変位の値が変わった関数(hash の不一致): %d / %d 件" % (moved, len(cur)))

    if any(bad.values()):
        print("", file=sys.stderr)
        print("NG: 並べ替え以外が変わっている。", file=sys.stderr)
        for key, n in bad.items():
            if n:
                i, bv, cv = first[key]
                print("    %-12s %4d 件ずれている。最初の 1 件: 順位 %d  基準 %s -> 今回 %s"
                      % (key, n, i, bv, cv), file=sys.stderr)
        if bad["disphash"]:
            print("", file=sys.stderr)
            print("    **disphash が違うのは「変位以外のバイトが変わった」という意味である。**",
                  file=sys.stderr)
            print("    並べ替えでは変位しか動かないので、ここが落ちたら"
                  "並べ替え以外の変更が混ざっている。", file=sys.stderr)
        return 1

    if expect_moved and moved == 0:
        print("", file=sys.stderr)
        print("NG: **変位が 1 つも変わっていない。** 並べ替えが効いていない"
              "(または基準と同じ木を測っている)。", file=sys.stderr)
        return 1

    out("")
    out("OK: %d 関数すべてで size / maskedbytes / dispbytes / **disphash** が一致した。"
        % len(cur))
    out("    = 変位の値だけが変わっており、**それ以外のバイトは 1 つも変わっていない**。")
    out("    変位が変わった関数は %d 件(並べ替えが効いていることの裏)。" % moved)
    return 0


SELF_BASE = """[JITDUMP] size=16 maskedbytes=0 hash=aaaaaaaaaaaaaaaa dispbytes=4 disphash=1111111111111111
[JITDUMP] size=32 maskedbytes=8 hash=bbbbbbbbbbbbbbbb dispbytes=8 disphash=2222222222222222
"""


def self_test():
    """この判定自身の陽性対照・陰性対照(規則 8)。"""
    print("[self-test] 並べ替え判定の陽性対照・陰性対照")
    ok = True

    def run(label, cur_text, want_rc, base_text=SELF_BASE, expect_moved=True):
        nonlocal ok
        with tempfile.TemporaryDirectory() as d:
            bp, cp = d + "/b.txt", d + "/c.txt"
            open(bp, "w").write(base_text)
            open(cp, "w").write(cur_text)
            rc = check(parse_log(bp), parse_log(cp), quiet=True, expect_moved=expect_moved)
        if rc == want_rc:
            print("OK(%s): rc=%d (期待 %d)" % (label, rc, want_rc))
        else:
            print("FAIL(%s): rc=%d。期待は %d" % (label, rc, want_rc), file=sys.stderr)
            ok = False

    # 陰性対照: 変位の値だけが変わった = 並べ替え
    run("陰性対照・変位だけ変わった(= 並べ替え)",
        SELF_BASE.replace("hash=aaaaaaaaaaaaaaaa", "hash=cccccccccccccccc"), 0)
    # 陽性対照 1: 変位以外が変わった
    run("陽性対照1・disphash が変わった(変位以外が変わった)",
        SELF_BASE.replace("disphash=1111111111111111", "disphash=9999999999999999"), 1)
    # 陽性対照 2: byte 長が変わった
    run("陽性対照2・size が変わった", SELF_BASE.replace("size=16", "size=24"), 1)
    # 陽性対照 3: 変位の byte 数が変わった
    run("陽性対照3・dispbytes が変わった", SELF_BASE.replace("dispbytes=4", "dispbytes=12"), 1)
    # 陽性対照 4: 関数の数が変わった
    run("陽性対照4・関数の数が変わった",
        SELF_BASE + "[JITDUMP] size=8 maskedbytes=0 hash=dddddddddddddddd dispbytes=4 disphash=3333333333333333\n", 1)
    # 陽性対照 5: 何も動いていない(並べ替えが効いていない)
    run("陽性対照5・変位が 1 つも動いていない", SELF_BASE, 1)
    # 陽性対照 6: 古い基準(disphash が無い)とは比べずに止まる
    run("陽性対照6・古い基準では止まる", SELF_BASE, 2,
        base_text="[JITDUMP] size=16 maskedbytes=0 hash=aaaaaaaaaaaaaaaa\n")

    if ok:
        print("self-test 成功: 変位以外の変化・長さの変化・件数の変化・"
              "「何も動いていない」・古い基準のすべてで落ちることを確かめた")
        return 0
    return 1


def main(argv):
    if "--self-test" in argv:
        return self_test()
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    return check(parse_log(args[0]), parse_log(args[1]),
                 expect_moved="--allow-unmoved" not in argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
