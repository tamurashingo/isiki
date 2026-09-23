#!/usr/bin/env python3
"""マクロの前方参照を検出する。

**マクロ定義より前で使われたマクロは、展開されずに関数呼び出しとして解釈される。**
その名前の関数は存在しないので EVAL-ERROR が**値として**返り(制御転送ではない。
src/c/eval.h の os_is_control_transfer を参照)、黙って流れて捨てられる。
エラーにならないので、**その式は静かに何もしなかったことになる。**

とくに defun は JIT が定義時にコンパイルするため、そのとき未定義のマクロは
**call として焼き込まれる。** 後からマクロを定義しても手遅れである。

実際に踏んだ: test_framework.lisp の isiki-test-load が assert-equal のマクロ
定義より前にあり、declaim 漏れの検出器が一度も走っていなかった
(documents/inline-arith.md §8-1)。

使い方: python3 tools/check_macro_forward_refs.py
終了コード 0 = 前方参照なし、1 = あり。
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def strip_comments(text):
    r"""; から行末を落とす。文字列の中と #; は残す。"""
    out, i, n = [], 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i+1]); i += 2; continue
            if c == '"':
                in_str = False
            i += 1; continue
        if c == '"':
            in_str = True; out.append(c); i += 1; continue
        if c == "#" and i + 1 < n and text[i+1] == "\\":
            out.append(text[i:i+3]); i += 3; continue
        if c == ";":
            while i < n and text[i] != "\n":
                i += 1
            continue
        out.append(c); i += 1
    return "".join(out)

def runtime_order():
    """実行時(インタプリタ/JIT)の読み込み順。qemu_boot_test.lisp を正とする。"""
    order = []
    boot = os.path.join(ROOT, "test/lisp/qemu_boot_test.lisp")
    src = strip_comments(open(boot, encoding="utf-8").read())
    for m in re.finditer(r'\((?:load|isiki-test-load)\s+"([^"]+)"', src):
        p = m.group(1)
        order.append(p)
        if p.endswith("src/lisp/init.lisp"):
            # init.lisp は途中で別ファイルを load する
            sub = strip_comments(open(os.path.join(ROOT, p), encoding="utf-8").read())
            for m2 in re.finditer(r'\(load\s+"([^"]+)"', sub):
                order.append(m2.group(1))
    return order

def aot_order():
    """AOT トランスパイラの入力順。Makefile の TRANSPILE_LISP_SRC を正とする。

    **こちらも同じ罠を持つ。** トランスパイラは defmacro を見てから展開するので、
    定義より前に使われたマクロは展開されず、生成 C では関数呼び出しになる。"""
    mk = open(os.path.join(ROOT, "Makefile"), encoding="utf-8").read()
    m = re.search(r"^TRANSPILE_LISP_SRC\s*=\s*(.+)$", mk, re.M)
    if not m:
        return []
    return m.group(1).split()

def scan(order, label):
    stream, pos = [], 0
    for path in order:
        full = os.path.join(ROOT, path)
        if not os.path.exists(full):
            continue
        text = strip_comments(open(full, encoding="utf-8").read())
        for lineno, line in enumerate(text.split("\n"), 1):
            stream.append((pos, path, lineno, line))
            pos += 1

    defs = {}
    for idx, path, lineno, line in stream:
        for m in re.finditer(r"\(defmacro\s+([^\s()]+)", line):
            name = m.group(1)
            if name not in defs:
                defs[name] = (idx, path, lineno)

    bad = []
    for name, (didx, dpath, dline) in sorted(defs.items()):
        pat = re.compile(r"\(" + re.escape(name) + r"(?=[\s()])")
        for idx, path, lineno, line in stream:
            if idx >= didx:
                break
            if pat.search(line):
                bad.append((name, path, lineno, line.strip()[:78], dpath, dline))

    print("[%s] ファイル %d / defmacro %d" % (label, len(order), len(defs)), end="")
    if not bad:
        print(" -> 前方参照 0 件")
        return 0
    print(" -> **前方参照 %d 件**" % len(bad))
    for name, path, lineno, line, dpath, dline in bad:
        print("  %s:%d  (%s ...)   <- 定義は %s:%d" % (path, lineno, name, dpath, dline))
        print("      %s" % line)
    return 1

def main():
    rc = 0
    rc |= scan(runtime_order(), "実行時(init.lisp + テスト)")
    rc |= scan(aot_order(), "AOT(トランスパイラ入力)")
    if rc:
        print()
        print("マクロ定義より前で使われたマクロは展開されず、関数呼び出しになる。")
        print("その名前の関数は存在しないので EVAL-ERROR が**値として**返り、")
        print("制御転送ではないので黙って流れて捨てられる。**エラーにならない。**")
        print("とくに defun は JIT が定義時にコンパイルするので call が焼き込まれる。")
    return rc

if __name__ == "__main__":
    sys.exit(main())
