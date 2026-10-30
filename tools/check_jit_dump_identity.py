#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""JIT が出力した機械語のバイト列が「1 バイトも変わっていない」ことを、
関数ごとのダンプ行で突き合わせる。

**なぜ要るか。** `JIT_DUMP=1`(`-DISIKIOS_JIT_DUMP`)でビルドすると、JIT が
コンパイルした関数ごとに `[JITDUMP] size=… maskedbytes=… hash=…` がシリアルへ
出る(`src/c/za.c`、`za_try_compile_defun` の末尾)。**出す側はあるが、
比べる側が無かった。** documents/jit-tag-constants.md §5.2 の
「1038 関数、完全一致」は人が目で突き合わせた結果で、手順として残っていない。

フレームの配置を並べ替えるような変更では「命令が 1 つも増えていない / バイト長が
1 バイトも変わっていない」を全関数で言う必要がある。ダンプ行はそれを持っている:

    size=        その関数が出力した機械語の byte 長
    maskedbytes= そのうちアドレス即値としてマスクした byte 数
    hash=        マスク後のバイト列の FNV-1a 64bit

**hash が一致すれば、マスクした位置以外のバイトは 1 バイトも違わない。**
アドレス即値をマスクするのは、`movabs` に焼かれる絶対アドレスが起動ごとに
変わるため(UEFI のロードアドレス・Lisp ヒープ・nil)。マスクの判定は
`[0x10000, 2^48)` の値域で、小さい定数(タグ値・MAGIC・argc・fixnum)は
マスクされない — **つまり検出したいタグ/レイアウト定数の変化は見える。**

**見えないもの(過大に主張しないため):**
  - どのアドレスが焼かれたかの変化(両方ともアドレスの値域にあれば区別できない)
  - 関数の名前。ダンプ行は名前を持たないので、突き合わせは**出現順**で行う。
    順序が変わるだけでも相違として落ちる(順序だけなら、そう報告する)
  - 「命令数」そのもの。持っているのは byte 長とバイト列である。
    ただしバイト列が一致すれば命令列も一致する(逆は成り立たない)

使い方:
    # 退避と比較(ふつうは tools/jit_dump_identity.sh 経由、make から呼ぶ)
    check_jit_dump_identity.py save  serial.txt --dir tmp/identity/jit \\
        --meta signature=-DISIKIOS_JIT_DUMP --meta qemu-mem=256M
    check_jit_dump_identity.py check serial.txt --dir tmp/identity/jit \\
        --meta signature=-DISIKIOS_JIT_DUMP --meta qemu-mem=256M

    # 2 つのログを直接比べる
    check_jit_dump_identity.py compare base_serial.txt new_serial.txt

    # 検出器の自己試験(規則 8)。QEMU もビルドも要らない
    check_jit_dump_identity.py --self-test

終了コード:
    0  同一(関数数・byte 長・マスク数・バイト列すべて一致)
    1  相違(**これが検出**)
    2  比較が成立しない(ダンプ行が 0 行 / 基準が無い / 条件が違う)
"""

import os
import re
import sys
import time

#: za.c が出す行。size/maskedbytes/hash の 3 つだけを持つ(関数名は無い)
DUMP_RE = re.compile(
    r"\[JITDUMP\]\s+size=(\d+)\s+maskedbytes=(\d+)\s+hash=([0-9a-f]{16})")

FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def fnv1a_64(data):
    """za.c の `[JITDUMP]` が使っているハッシュと同じもの。

    自己試験で「生成コードを 1 バイト変えると hash が変わる」ことを
    実際に確かめるために置いてある(**値の出所を za.c と同じにしておかないと、
    自己試験が検出器の感度を確かめたことにならない**)。
    """
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return "%016x" % h


def parse_log(path):
    """シリアルログから [JITDUMP] 行を**出現順に**抜き出す。

    ログには試験の出力や他の診断行が混ざるので、正規表現に合う行だけを拾う。
    行が途中で切れている(ゲストが落ちた)場合はその行だけが落ちる。
    """
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            m = DUMP_RE.search(line)
            if m:
                rows.append((int(m.group(1)), int(m.group(2)), m.group(3), lineno))
    return rows


def write_dump(path, rows):
    with open(path, "w", encoding="utf-8") as f:
        f.write("# idx\tsize\tmaskedbytes\thash\n")
        for i, (size, masked, h, _ln) in enumerate(rows):
            f.write("%d\t%d\t%d\t%s\n" % (i, size, masked, h))


def read_dump(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            idx, size, masked, h = line.rstrip("\n").split("\t")
            rows.append((int(size), int(masked), h, int(idx)))
    return rows


def totals(rows):
    return (len(rows), sum(r[0] for r in rows), sum(r[1] for r in rows))


def compare(base, cur, quiet=False, base_name="基準", cur_name="今回"):
    """出現順に突き合わせる。戻り値は 0(同一) か 1(相違)。"""
    bn, bbytes, bmask = totals(base)
    cn, cbytes, cmask = totals(cur)

    diffs = []
    for i in range(min(bn, cn)):
        if base[i][:3] != cur[i][:3]:
            diffs.append(i)

    same_multiset = sorted(r[:3] for r in base) == sorted(r[:3] for r in cur)

    if not quiet:
        print("=== JIT 生成コードの同一性比較(出現順) ===")
        print()
        print("    | 量 | %s | %s | 差 |" % (base_name, cur_name))
        print("    |---|---|---|---|")
        print("    | コンパイルした関数 | %d | %d | %+d |" % (bn, cn, cn - bn))
        print("    | 出力した機械語の合計 byte | %d | %d | %+d |"
              % (bbytes, cbytes, cbytes - bbytes))
        print("    | うちアドレス即値としてマスクした byte | %d | %d | %+d |"
              % (bmask, cmask, cmask - bmask))
        print()

    if bn == cn and not diffs:
        if not quiet:
            print("OK: %d 関数すべてで size / maskedbytes / バイト列のハッシュが一致した。"
                  % bn)
            print("    **マスクしたアドレス即値以外、生成コードは 1 バイトも変わっていない。**")
        return 0

    if not quiet:
        if bn != cn:
            print("**関数の数が違う。** 基準 %d → 今回 %d" % (bn, cn))
            print("  コンパイルされる関数の顔ぶれか、断念(bail)の有無が変わっている。")
            print("  (条件が同じなら顔ぶれは変わらない。MILESTONE / QEMU_MEM を確かめること)")
            print()
        if diffs:
            print("**同じ順位で中身が違う関数が %d 件。** 最初の %d 件:"
                  % (len(diffs), min(10, len(diffs))))
            print()
            print("    | 順位 | 量 | %s | %s |" % (base_name, cur_name))
            print("    |---|---|---|---|")
            for i in diffs[:10]:
                bsz, bmk, bh, _ = base[i]
                csz, cmk, ch, _ = cur[i]
                if bsz != csz:
                    print("    | %d | size | %d | %d (%+d) |" % (i, bsz, csz, csz - bsz))
                if bmk != cmk:
                    print("    | %d | maskedbytes | %d | %d (%+d) |"
                          % (i, bmk, cmk, cmk - bmk))
                if bh != ch:
                    print("    | %d | hash | %s | %s |" % (i, bh, ch))
            print()
            kinds = {
                "size(byte 長が変わった)": sum(1 for i in diffs if base[i][0] != cur[i][0]),
                "maskedbytes(アドレス即値の数/位置が変わった)":
                    sum(1 for i in diffs if base[i][1] != cur[i][1]),
                "hash のみ(長さは同じでバイト列が変わった)":
                    sum(1 for i in diffs
                        if base[i][0] == cur[i][0] and base[i][1] == cur[i][1]),
            }
            print("    内訳:")
            for k, v in kinds.items():
                print("      %-50s %d 件" % (k, v))
            print()
        if same_multiset:
            print("**中身の集合は一致している。順序だけが違う。**")
            print("  コンパイルの順番が変わっただけなら生成コードは変わっていないが、")
            print("  **順番が変わる理由そのものを説明できないかぎり同一とは言えない。**")
            print()
        print("NG: 生成コードが変わっている。")
    return 1


# ---- 基準の退避と突き合わせ ------------------------------------------------

def parse_meta_args(items):
    meta = {}
    for it in items:
        if "=" not in it:
            sys.exit("ERROR: --meta は key=value の形で渡すこと: %r" % it)
        k, v = it.split("=", 1)
        meta[k] = v
    return meta


def write_meta(path, meta):
    with open(path, "w", encoding="utf-8") as f:
        f.write("# JIT 生成コードの同一性比較の基準(この条件で取った)\n")
        for k in sorted(meta):
            f.write("%s\t%s\n" % (k, meta[k]))


def read_meta(path):
    meta = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            k, _, v = line.rstrip("\n").partition("\t")
            meta[k] = v
    return meta


def require_rows(rows, log_path):
    """[罠] ダンプ行が 0 行なら、**比較は成立していない**。

    JIT_DUMP=1 でビルドしていない / ゲストが JIT を 1 度も通らなかった、の
    どちらでも 0 行になる。0 行同士を「一致」と呼ぶと、計器が死んでいるのに
    緑が出る(documents/pitfalls.md 原則6 そのもの)。
    """
    if rows:
        return 0
    print("ERROR: %s に [JITDUMP] 行が 1 行も無い。**比較は成立していない**" % log_path,
          file=sys.stderr)
    print("       JIT_DUMP=1 でビルドしたか / ゲストが JIT を通ったかを確かめること",
          file=sys.stderr)
    return 2


def cmd_save(log, out_dir, meta):
    t0 = time.time()
    rows = parse_log(log)
    rc = require_rows(rows, log)
    if rc:
        return rc
    os.makedirs(out_dir, exist_ok=True)
    write_dump(os.path.join(out_dir, "dump.tsv"), rows)
    write_meta(os.path.join(out_dir, "meta.txt"), meta)
    n, b, m = totals(rows)
    print("=== JIT 生成コードの基準を退避した ===")
    print("    置き場   : %s  (**make clean で消える**)" % out_dir)
    print("    関数     : %d" % n)
    print("    合計byte : %d (うちマスク %d)" % (b, m))
    for k in sorted(meta):
        print("    %-9s: %s" % (k, meta[k]))
    print("    所要     : %.2f 秒(ログの解析のみ。QEMU の時間は含まない)" % (time.time() - t0))
    return 0


def cmd_check(log, out_dir, meta):
    t0 = time.time()
    base_path = os.path.join(out_dir, "dump.tsv")
    meta_path = os.path.join(out_dir, "meta.txt")
    if not os.path.exists(base_path):
        print("ERROR: 基準が無い(%s)。先に save を実行すること" % base_path, file=sys.stderr)
        print("       (tmp/ は make clean で消えるので、clean を挟んだなら取り直し)",
              file=sys.stderr)
        return 2
    if os.path.exists(meta_path):
        base_meta = read_meta(meta_path)
        bad = [(k, base_meta.get(k), v) for k, v in sorted(meta.items())
               if base_meta.get(k) != v]
        if bad:
            print("ERROR: 基準と条件が違う。**違う条件で取った基準と比べてはならない**",
                  file=sys.stderr)
            for k, was, now in bad:
                print("       %s: 基準[%s] いま[%s]" % (k, was, now), file=sys.stderr)
            return 2
    base = read_dump(base_path)
    cur = parse_log(log)
    rc = require_rows(cur, log)
    if rc:
        return rc
    rc = compare(base, cur)
    print("    所要: %.2f 秒(ログの解析と突き合わせのみ)" % (time.time() - t0))
    return rc


# ---- 自己試験(規則 8) -----------------------------------------------------

def _line(size, masked, h):
    return "[JITDUMP] size=%d maskedbytes=%d hash=%s\n" % (size, masked, h)


def self_test():
    """検出器が「対象を見られる」ことを先に示す。

    QEMU もビルドも要らない。合成したログを使うが、**ハッシュは za.c と同じ
    FNV-1a で実際のバイト列から計算する**ので、「生成コードを 1 バイト変えると
    落ちる」を本当に確かめている(ハッシュ文字列を手で書き換えただけでは、
    検出器がバイトの変化を見られることの証明にならない)。
    """
    import io
    import contextlib
    import tempfile

    fails = []

    def expect(name, rc, want, what):
        if rc == want:
            print("OK(%s): %s" % (name, what))
        else:
            sys.stdout.flush()
            print("FAIL(%s): %s — 期待 rc=%d だが rc=%d" % (name, what, want, rc),
                  file=sys.stderr)
            sys.stderr.flush()
            fails.append(name)

    def run_compare(base_rows, cur_rows):
        # quiet=False で呼んで出力ごと捕まえる。**「落ちた」だけでなく
        # 「何を相違として報告したか」まで確かめたいので、捨ててはいけない**
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            rc = compare(base_rows, cur_rows)
        return rc, sink.getvalue()

    # 本物のバイト列から作った基準。3 関数ぶん
    bodies = [
        bytes((0x55, 0x48, 0x89, 0xE5, 0x5D, 0xC3)),
        bytes(range(0x40, 0x40 + 32)),
        bytes((0x48, 0x83, 0xEC, 0x20) + tuple(range(0x80, 0x80 + 60))),
    ]
    base = [(len(b), 0, fnv1a_64(b), i) for i, b in enumerate(bodies)]

    # 陰性対照: 同じバイト列から作ったダンプは一致する
    same = [(len(b), 0, fnv1a_64(b), i) for i, b in enumerate(bodies)]
    rc, _ = run_compare(base, same)
    expect("陰性対照", rc, 0, "同じバイト列なら 同一")

    # 陽性対照1: 生成コードを **1 バイトだけ** 変える(長さは同じ)
    flipped = list(bodies)
    b = bytearray(flipped[1])
    b[7] ^= 0x01
    flipped[1] = bytes(b)
    cur = [(len(x), 0, fnv1a_64(x), i) for i, x in enumerate(flipped)]
    rc, out = run_compare(base, cur)
    expect("陽性対照1", rc, 1, "生成コードを 1 バイト変えると 相違として落ちる")
    if rc == 1 and "hash" not in out:
        print("FAIL(陽性対照1): hash の相違として報告されていない", file=sys.stderr)
        fails.append("陽性対照1-報告")

    # 陽性対照2: byte 長が 1 増える(命令が 1 つ増えた形)
    longer = list(bodies)
    longer[2] = longer[2] + b"\x90"
    cur = [(len(x), 0, fnv1a_64(x), i) for i, x in enumerate(longer)]
    rc, out = run_compare(base, cur)
    expect("陽性対照2", rc, 1, "byte 長が 1 増えると 相違として落ちる")
    if rc == 1 and "size" not in out:
        print("FAIL(陽性対照2): size の相違として報告されていない", file=sys.stderr)
        fails.append("陽性対照2-報告")

    # 陽性対照3: maskedbytes だけが変わる(アドレス即値の数/位置が動いた)
    cur = [(s, m + 8, h, i) for (s, m, h, i) in base]
    rc, out = run_compare(base, cur)
    expect("陽性対照3", rc, 1, "maskedbytes が変わると 相違として落ちる")

    # 陽性対照4: 関数が 1 つ減る(コンパイルを断念した形)
    rc, _ = run_compare(base, base[:-1])
    expect("陽性対照4", rc, 1, "関数が 1 つ減ると 相違として落ちる")

    # 陽性対照5: 中身は同じで順序だけ入れ替わる
    swapped = [base[1], base[0], base[2]]
    rc, out = run_compare(base, swapped)
    expect("陽性対照5", rc, 1, "順序だけが変わっても 相違として落ちる")
    if rc == 1 and "順序だけが違う" not in out:
        print("FAIL(陽性対照5): 「順序だけが違う」と診断されていない", file=sys.stderr)
        fails.append("陽性対照5-診断")

    with tempfile.TemporaryDirectory() as d:
        # 雑音耐性: 試験の出力や他の診断行が混ざっても同じ結果になる
        noisy = os.path.join(d, "noisy.txt")
        with open(noisy, "w", encoding="utf-8") as f:
            f.write("isiki-test-load src/lisp/init.lisp\n")
            for (s, m, h, _i) in base:
                f.write("[ALIGN] region=2 n=123\n")
                f.write(_line(s, m, h))
            f.write("3269 passed, 0 failed\n")
        rows = parse_log(noisy)
        if [r[:3] for r in rows] == [r[:3] for r in base]:
            print("OK(雑音耐性): 他のシリアル出力が混ざっても %d 行だけを拾う" % len(rows))
        else:
            print("FAIL(雑音耐性): 抜き出しが %d 行(期待 %d 行)"
                  % (len(rows), len(base)), file=sys.stderr)
            fails.append("雑音耐性")

        # ガード1: ダンプ行が 0 行なら比較は成立しない(= 計器が死んでいる)
        empty = os.path.join(d, "empty.txt")
        with open(empty, "w", encoding="utf-8") as f:
            f.write("3269 passed, 0 failed\n")
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            rc = cmd_save(empty, os.path.join(d, "box1"), {})
        expect("ガード1", rc, 2,
               "[JITDUMP] 行が 0 行なら「一致」と言わずに止まる(計器が死んでいる形)")

        # ガード2: 条件(ビルドの印など)が違う基準とは比べない
        good = os.path.join(d, "good.txt")
        with open(good, "w", encoding="utf-8") as f:
            for (s, m, h, _i) in base:
                f.write(_line(s, m, h))
        box = os.path.join(d, "box2")
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            rc = cmd_save(good, box, {"signature": "-DISIKIOS_JIT_DUMP",
                                      "qemu-mem": "256M"})
        expect("準備", rc, 0, "基準を退避できた")
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            rc = cmd_check(good, box, {"signature": "-DISIKIOS_JIT_DUMP",
                                       "qemu-mem": "256M"})
        expect("陰性対照2", rc, 0, "同じログ・同じ条件なら save → check が 同一")
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            rc = cmd_check(good, box, {"signature": "-DISIKIOS_JIT_DUMP",
                                       "qemu-mem": "96M"})
        expect("ガード2", rc, 2, "QEMU_MEM が違う基準とは比べずに止まる")
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            rc = cmd_check(good, os.path.join(d, "nothing"), {})
        expect("ガード3", rc, 2, "基準が無ければ止まる")

    print()
    if not fails:
        print("OK: JIT ダンプ同一性検出器の自己試験はすべて通った(QEMU もビルドも要らない)")
        return 0
    print("NG: %d 件が期待どおりに動かない(%s)。**検出器が効いていない**"
          % (len(fails), ", ".join(fails)), file=sys.stderr)
    return 1


def main(argv):
    args = argv[1:]
    if "--self-test" in args:
        return self_test()
    if not args:
        sys.exit(__doc__)

    out_dir = "tmp/identity/jit"
    meta = []
    positional = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--dir":
            i += 1
            out_dir = args[i]
        elif a == "--meta":
            i += 1
            meta.append(args[i])
        elif a.startswith("--"):
            sys.exit("ERROR: 知らないオプション: %s" % a)
        else:
            positional.append(a)
        i += 1

    if not positional:
        sys.exit(__doc__)
    sub, rest = positional[0], positional[1:]
    if sub == "save":
        if len(rest) != 1:
            sys.exit("usage: save <serial.log> [--dir D] [--meta k=v]…")
        return cmd_save(rest[0], out_dir, parse_meta_args(meta))
    if sub == "check":
        if len(rest) != 1:
            sys.exit("usage: check <serial.log> [--dir D] [--meta k=v]…")
        return cmd_check(rest[0], out_dir, parse_meta_args(meta))
    if sub == "compare":
        if len(rest) != 2:
            sys.exit("usage: compare <base.log> <cur.log>")
        base, cur = parse_log(rest[0]), parse_log(rest[1])
        rc = require_rows(base, rest[0]) or require_rows(cur, rest[1])
        if rc:
            return rc
        return compare(base, cur, base_name=rest[0], cur_name=rest[1])
    if sub == "extract":
        if len(rest) != 1:
            sys.exit("usage: extract <serial.log>")
        rows = parse_log(rest[0])
        rc = require_rows(rows, rest[0])
        if rc:
            return rc
        write_dump("/dev/stdout", rows)
        return 0
    sys.exit("ERROR: 知らないサブコマンド: %s\n%s" % (sub, __doc__))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
