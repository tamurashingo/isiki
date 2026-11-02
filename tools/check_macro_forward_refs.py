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

使い方:
    python3 tools/check_macro_forward_refs.py             # 走査する
    python3 tools/check_macro_forward_refs.py --self-test # 検出器自身の陽性/陰性対照
終了コード 0 = 前方参照なし、1 = あり。
"""
import os, re, sys, tempfile

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

def scan(order, label, quiet=False):
    """order の順に読み、定義より前で使われた defmacro を数える。前方参照の件数を返す。"""
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

    if quiet:
        return len(bad)
    print("[%s] ファイル %d / defmacro %d" % (label, len(order), len(defs)), end="")
    if not bad:
        print(" -> 前方参照 0 件")
        return 0
    print(" -> **前方参照 %d 件**" % len(bad))
    for name, path, lineno, line, dpath, dline in bad:
        print("  %s:%d  (%s ...)   <- 定義は %s:%d" % (path, lineno, name, dpath, dline))
        print("      %s" % line)
    return len(bad)

# self-test 用の資料。**使う側**と**定義する側**を別ファイルに分けてある。
# この 2 つを読む順を入れ替えるだけで陽性/陰性が切り替わるので、
# 「検出器が見ているのは順序である」ことをそのまま確かめられる。
SELF_TEST_USE = """(defun caller () (self-test-macro 1))
"""

SELF_TEST_DEF = """(defmacro self-test-macro (x) `(+ ,x 1))
"""

# 同一ファイル内の前方参照。実際に踏んだ形はこちら
# (test_framework.lisp の isiki-test-load が assert-equal の定義より前にあった)
SELF_TEST_SAME_FILE = """(defun caller () (self-test-macro 1))
;; ↓ 使うより後ろで定義している。**これが事故の形**
(defmacro self-test-macro (x) `(+ ,x 1))
"""

def self_test():
    """規則 8: 検出器を**意図的に発火させて**、効いていることを確かめる。

    陽性対照: 定義より前でマクロを使っている資料を与え、前方参照が 1 件以上
              報告されること。報告されなければ検出器は働いていない。
    陰性対照: 定義順が正しい資料では 0 件になること。
              (何にでも発火する検出器は、発火しない検出器と同じく役に立たない)

    資料は tempfile に作り、**リポジトリの中には何も残さない。**
    """
    ok = True

    def check(label, got, want):
        nonlocal ok
        if got == want:
            print("OK(%s): 前方参照 %d 件 (期待 %d 件)" % (label, got, want))
            return
        ok = False
        print("FAIL(%s): 前方参照 %d 件。期待は %d 件。**検出器が効いていない**"
              % (label, got, want), file=sys.stderr)

    print("[self-test] マクロ前方参照検出器の陽性対照・陰性対照")
    with tempfile.TemporaryDirectory(prefix="macro_forward_refs_selftest_") as tmp:
        use = os.path.join(tmp, "use.lisp")
        define = os.path.join(tmp, "define.lisp")
        same = os.path.join(tmp, "same.lisp")
        open(use, "w", encoding="utf-8").write(SELF_TEST_USE)
        open(define, "w", encoding="utf-8").write(SELF_TEST_DEF)
        open(same, "w", encoding="utf-8").write(SELF_TEST_SAME_FILE)

        # 陽性: 使う側のファイルが先に読まれる -> 定義より前の使用なので捕まる
        check("陽性対照・ファイル跨ぎ", scan([use, define], "self-test", quiet=True), 1)
        # 陽性: 1 ファイルの中で、使用が定義より上にある
        check("陽性対照・同一ファイル内", scan([same], "self-test", quiet=True), 1)
        # 陰性: 定義側のファイルが先に読まれる -> 正しい順なので何も出ない
        check("陰性対照・定義が先", scan([define, use], "self-test", quiet=True), 0)

    if ok:
        print("self-test 成功: 定義より前の使用を**ファイル跨ぎでも同一ファイル内でも**"
              "報告し、正しい順では報告しないことを確かめた")
    return 0 if ok else 1

def main(argv):
    if "--self-test" in argv:
        return self_test()
    n = scan(runtime_order(), "実行時(init.lisp + テスト)")
    n += scan(aot_order(), "AOT(トランスパイラ入力)")
    if n:
        print()
        print("マクロ定義より前で使われたマクロは展開されず、関数呼び出しになる。")
        print("その名前の関数は存在しないので EVAL-ERROR が**値として**返り、")
        print("制御転送ではないので黙って流れて捨てられる。**エラーにならない。**")
        print("とくに defun は JIT が定義時にコンパイルするので call が焼き込まれる。")
    return 1 if n else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
