#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test/lisp の BAIL-LINE 期待値を src/c/za.c と照合する。

JIT の断念箇所の診断(%%DIAG-ZA-BAIL-AT)は __LINE__ を記録する。だから za.c に
1 行入れただけで、境界テストの期待値が全部ずれる。ずれに気付くのが QEMU の
テスト(1 回 8 分)だと遅いので、ホスト側・1 秒で落ちるようにする。

照合するのは test/lisp/*.lisp に書かれた次の形:

    (assert-equal 3662 (%%diag-za-bail-at 0))   ; BAIL-LINE: ZA_MAX_LET_DEPTH

za.c 側の正は、その行に実際に書かれている注記:

    { ZA_BAIL_LINE(); return 0; }   /* [診断] 容量上限 ZA_MAX_LET_DEPTH */

同じ定数の断念箇所は複数あってよい(ZA_MAX_NLX_DEPTH は 6 箇所)。期待値は
そのどれか 1 つと一致していればよい ... のではなく、**書かれている行そのもの**が
その定数の注記を持っていることを見る。別の定数の行に変わっていたら落ちる。

使い方:
    tools/check_jit_bail_lines.py              照合する
    tools/check_jit_bail_lines.py --fix        ずれていたら書き換える
    tools/check_jit_bail_lines.py --self-test  検出器が落ちることを確かめる(規則 8)
"""
import contextlib
import glob
import io
import os
import re
import sys
import tempfile

ANNOT = re.compile(r"ZA_BAIL_LINE\(\);.*\[診断\] 容量上限 (ZA_MAX_[A-Z_0-9]+)")
EXPECT = re.compile(
    r"\(assert-equal\s+(\d+)\s+\(%%diag-za-bail-at\s+0\)\)(.*?);\s*BAIL-LINE:\s*(ZA_MAX_[A-Z_0-9]+)"
)


def read_annotations(za_path):
    """za.c の行番号 -> 定数名。注記のある断念箇所だけ。"""
    out = {}
    with open(za_path, encoding="utf-8") as f:
        for i, line in enumerate(f, start=1):
            m = ANNOT.search(line)
            if m:
                out[i] = m.group(1)
    return out


def by_const(annot):
    out = {}
    for line, const in annot.items():
        out.setdefault(const, []).append(line)
    for v in out.values():
        v.sort()
    return out


def check(za_path, lisp_files, fix=False, quiet=False):
    annot = read_annotations(za_path)
    if not annot:
        print("ERROR: %s に `[診断] 容量上限` の注記が 1 つも無い" % za_path, file=sys.stderr)
        return 2
    index = by_const(annot)
    bad = 0
    checked = 0
    for path in lisp_files:
        with open(path, encoding="utf-8") as f:
            src = f.read()
        lines = src.split("\n")
        changed = False
        for ln, text in enumerate(lines):
            m = EXPECT.search(text)
            if not m:
                continue
            checked += 1
            want_line = int(m.group(1))
            const = m.group(3)
            have = annot.get(want_line)
            if have == const:
                continue
            bad += 1
            cands = index.get(const, [])
            print(
                "%s:%d: 期待値 %d は %s の断念箇所ではない (za.c:%d は %s)"
                % (path, ln + 1, want_line, const, want_line, have or "注記なし"),
                file=sys.stderr,
            )
            if not cands:
                print("    %s の断念箇所が za.c に無い。定数を消したなら、このテストも消すこと"
                      % const, file=sys.stderr)
                continue
            print("    %s の断念箇所: %s" % (const, " ".join(str(c) for c in cands)),
                  file=sys.stderr)
            if fix:
                if len(cands) == 1:
                    newline = cands[0]
                else:
                    # 複数ある場合は、ずれる前に一番近かったものへ寄せる
                    newline = min(cands, key=lambda c: abs(c - want_line))
                lines[ln] = text.replace(m.group(0),
                                         m.group(0).replace(m.group(1), str(newline), 1), 1)
                print("    --fix: %d -> %d" % (want_line, newline), file=sys.stderr)
                changed = True
        if changed:
            with open(path, "w", encoding="utf-8") as f:
                f.write("\n".join(lines))
    if checked == 0:
        print("ERROR: BAIL-LINE: の注釈が 1 つも見つからない(検出器が空振りしている)",
              file=sys.stderr)
        return 2
    if bad and not fix:
        print("FAIL: BAIL-LINE 期待値 %d 件がずれている(%d 件検査)。"
              "`tools/check_jit_bail_lines.py --fix` で直せる" % (bad, checked), file=sys.stderr)
        return 1
    if fix and bad:
        print("FIXED: %d 件書き換えた(%d 件検査)" % (bad, checked))
        return 0
    if not quiet:
        print("OK: BAIL-LINE 期待値 %d 件すべてが za.c の断念箇所と一致" % checked)
    return 0


def self_test(za_path, lisp_files):
    """規則 8: 検出器が本当に落ちることを確かめる。

    za.c の先頭に空行を 1 行だけ入れた複製を作ると、すべての断念箇所の行番号が
    1 ずれる。そのとき検出器が 1 を返さなければ、検出器は効いていない。
    """
    with open(za_path, encoding="utf-8") as f:
        src = f.read()
    with tempfile.TemporaryDirectory() as d:
        shifted = os.path.join(d, "za_shifted.c")
        with open(shifted, "w", encoding="utf-8") as f:
            f.write("\n" + src)
        # 中の check は「落ちるのが正しい」ので、その出力は捨てる
        # (make test のログに FAIL が並ぶと本物の失敗と見分けが付かない)
        sink = io.StringIO()
        with contextlib.redirect_stderr(sink), contextlib.redirect_stdout(sink):
            rc = check(shifted, lisp_files, fix=False, quiet=True)
    if rc == 1:
        print("OK(陽性対照): za.c を 1 行ずらすと検出器は落ちた")
        return 0
    print("FAIL(陽性対照): za.c を 1 行ずらしても検出器が落ちない(rc=%d)。"
          "検出器が効いていない" % rc, file=sys.stderr)
    return 1


def main(argv):
    args = argv[1:]
    za_path = "src/c/za.c"
    if args and not args[0].startswith("--"):
        za_path = args.pop(0)
    lisp_files = sorted(glob.glob("test/lisp/*.lisp"))
    if "--self-test" in args:
        return self_test(za_path, lisp_files)
    return check(za_path, lisp_files, fix="--fix" in args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
