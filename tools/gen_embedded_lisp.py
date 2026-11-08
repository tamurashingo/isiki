#!/usr/bin/env python3
"""カーネルへ同梱する Lisp ソースを C のバイト配列へ変換する(生成器 + 検査器)。

    python3 tools/gen_embedded_lisp.py --output src/c/init_lisp_src.c
    python3 tools/gen_embedded_lisp.py --check  src/c/init_lisp_src.c
    python3 tools/gen_embedded_lisp.py --print-inputs
    python3 tools/gen_embedded_lisp.py --self-test

同梱する理由と設計は docs/plans/local-load-and-core-image.md(M1)。

**C の文字列リテラルにしない。** 72KB のソースをリテラルにするとエスケープ
(`"`、`\\`、改行、三文字表記)の処理が要り、処理系の文字列リテラル長の上限
(C11 5.2.4.1 の下限は 4095 文字)にも触れる。バイト配列なら入力を 1 byte も
解釈しないので、どちらも起きない。

**長さは配列から導出する**(規則 13: 意味のある件数は記録せず導出する)。
`_Static_assert` が「配列の要素数 == 入力ファイルの wc -c」を**コンパイル時に**
突き合わせ、`--check` が「生成物の中身 == いまディスクにある入力」を
**ホスト側で**突き合わせる。前者は生成物の内部矛盾を、後者は生成物の
古さ(入力を変えたのに再生成されていない)を捕まえる。

検出器なので陽性対照を持つ(規則 8): `--self-test` が壊した生成物を
わざと食わせ、`--check` が**本当に落ちること**を確かめる。
"""

import argparse
import os
import re
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --------------------------------------------------------------------------
# 同梱するファイルと、C 側から見える名前。**ここが唯一の真実源**で、
# Makefile は --print-inputs で依存を受け取る(書き写すと黙って食い違う。
# Makefile:380 付近の print-build-flag-signature / print-transpile-src と同じ理由)。
#
# init.lisp     基盤ライブラリ。起動時に読む(M1-T3)
# disassemble.lisp 補助ライブラリ。設計判断 L2 により同梱する。
#                  **この PR では生成するだけで、読むのは次の PR**
# 宣言は src/c/init_lisp_src.h(手書き)。生成物がそれを #include するので、
# 名前や型が食い違えばコンパイルが落ちる
# --------------------------------------------------------------------------
EMBEDDED = [
    ("src/lisp/init.lisp", "g_init_lisp_src"),
    ("src/lisp/disassemble.lisp", "g_disassemble_lisp_src"),
]

BYTES_PER_LINE = 16

#: 生成物を機械で読み直すための印。path / symbol / 入力の byte 数
MARKER_RE = re.compile(r"^/\* ISIKI-EMBED: (\S+) (\S+) (\d+) \*/$")


def _marker(path, symbol, size):
    return "/* ISIKI-EMBED: %s %s %d */" % (path, symbol, size)


def _read_input(path):
    full = os.path.join(REPO_ROOT, path)
    with open(full, "rb") as f:
        data = f.read()
    if len(data) == 0:
        raise SystemExit("gen_embedded_lisp: %s が空。空配列は C として不正になる" % path)
    return data


def generate(entries):
    """生成する C ソースの全文を返す。"""
    out = []
    out.append("/* 自動生成。編集しない(tools/gen_embedded_lisp.py が作る)。")
    out.append("   git 管理対象外で、同梱対象の Lisp が変わった時だけ再生成される")
    out.append("   (Makefile の $(INIT_LISP_SRC) ルール)。")
    out.append("")
    out.append("   長さは sizeof から導出する(規則 13)。_Static_assert が")
    out.append("   「配列の要素数 == 入力の wc -c」をコンパイル時に突き合わせる。 */")
    out.append('#include "init_lisp_src.h"')
    for path, symbol in entries:
        data = _read_input(path)
        size = len(data)
        out.append("")
        out.append(_marker(path, symbol, size))
        out.append("const UINT8 %s[] = {" % symbol)
        for i in range(0, size, BYTES_PER_LINE):
            chunk = data[i:i + BYTES_PER_LINE]
            out.append("".join("0x%02x," % b for b in chunk))
        out.append("};")
        out.append('_Static_assert(sizeof(%s) == %du,' % (symbol, size))
        out.append('               "%s: length != wc -c %s");' % (symbol, path))
        out.append("const UINT32 %s_len = (UINT32)sizeof(%s);" % (symbol, symbol))
    out.append("")
    return "\n".join(out)


def write_output(out_path, entries):
    """生成物を書き出す。
    **毎回 supersede で上書きする**($(LISP_COMPILED) の生成と同じ作法)。
    「内容が同じなら書かない」は mtime を据え置くので make がこのルールを
    毎回実行し続ける形になる。**無条件に作り直さない**ための歯止めは
    Makefile 側のファイル依存($(INIT_LISP_SRC): $(INIT_LISP_EMBED_SRC))が担う。
    """
    text = generate(entries)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(text)


# --------------------------------------------------------------------------
# 検査器: 生成物を**読み直して**入力と突き合わせる。
# 生成した側のデータ構造は使わず、吐かれたテキストから拾い直す
# --------------------------------------------------------------------------
HEX_RE = re.compile(r"^(?:0x[0-9a-f]{2},)+$")


def parse_generated(text):
    """生成物のテキストから [(path, symbol, declared_size, bytes, assert_size)] を拾う。"""
    lines = text.split("\n")
    found = []
    i = 0
    while i < len(lines):
        m = MARKER_RE.match(lines[i])
        if m is None:
            i += 1
            continue
        path, symbol, declared = m.group(1), m.group(2), int(m.group(3))
        i += 1
        head = "const UINT8 %s[] = {" % symbol
        if i >= len(lines) or lines[i] != head:
            raise ValueError("%s: 配列の開始行が無い(%r)" % (symbol, lines[i] if i < len(lines) else None))
        i += 1
        data = bytearray()
        while i < len(lines) and lines[i] != "};":
            if not HEX_RE.match(lines[i]):
                raise ValueError("%s: バイト列として読めない行がある: %r" % (symbol, lines[i]))
            for tok in lines[i].rstrip(",").split(","):
                data.append(int(tok, 16))
            i += 1
        if i >= len(lines):
            raise ValueError("%s: 配列が閉じていない" % symbol)
        i += 1
        assert_re = re.compile(r"^_Static_assert\(sizeof\(%s\) == (\d+)u,$" % re.escape(symbol))
        am = assert_re.match(lines[i]) if i < len(lines) else None
        if am is None:
            raise ValueError("%s: _Static_assert の行が無い(%r)"
                             % (symbol, lines[i] if i < len(lines) else None))
        assert_size = int(am.group(1))
        i += 1
        len_line = "const UINT32 %s_len = (UINT32)sizeof(%s);" % (symbol, symbol)
        if len_line not in lines:
            raise ValueError("%s: 長さを sizeof から導出する行が無い" % symbol)
        found.append((path, symbol, declared, bytes(data), assert_size))
    return found


def check(out_path, entries, verbose=True):
    """生成物が入力と一致しているかを調べる。問題の一覧(空なら合格)を返す。"""
    problems = []
    if not os.path.exists(out_path):
        return ["%s が無い(make build / make transpile で生成される)" % out_path]
    with open(out_path, encoding="utf-8") as f:
        text = f.read()
    try:
        found = parse_generated(text)
    except ValueError as e:
        return ["%s: %s" % (out_path, e)]

    by_symbol = {sym: (p, d, b, a) for (p, sym, d, b, a) in found}
    for path, symbol in entries:
        if symbol not in by_symbol:
            problems.append("%s が生成物に無い(入力 %s)" % (symbol, path))
            continue
        gen_path, declared, data, assert_size = by_symbol.pop(symbol)
        src = _read_input(path)
        if gen_path != path:
            problems.append("%s: 印の入力パスが %s(期待 %s)" % (symbol, gen_path, path))
        if len(data) != len(src):
            problems.append("%s: 配列の要素数 %d != wc -c %s = %d"
                            % (symbol, len(data), path, len(src)))
        if declared != len(src):
            problems.append("%s: 印の長さ %d != wc -c %s = %d"
                            % (symbol, declared, path, len(src)))
        if assert_size != len(src):
            problems.append("%s: _Static_assert の長さ %d != wc -c %s = %d"
                            % (symbol, assert_size, path, len(src)))
        if data != src:
            # 長さが合っていても中身が古いことはある(入力を書き換えて長さが
            # 変わらなかった場合)。最初に食い違う位置を出す
            off = next((k for k in range(min(len(data), len(src))) if data[k] != src[k]), None)
            problems.append("%s: 中身が %s と一致しない(最初の相違 offset=%s)"
                            % (symbol, path, off))
        elif verbose:
            print("ok  %-24s %7d byte == wc -c %s" % (symbol, len(data), path))
    for symbol in by_symbol:
        problems.append("%s は同梱対象に無いのに生成物へ入っている" % symbol)
    return problems


# --------------------------------------------------------------------------
# 陽性対照(規則 8): 壊した生成物を食わせて、--check が**落ちること**を確かめる。
# 落ちなければ検出器が対象を見られていないので、こちらが失敗する
# --------------------------------------------------------------------------
def self_test():
    failures = []

    def expect(name, problems, should_fail):
        fired = len(problems) > 0
        if fired != should_fail:
            failures.append("%s: 期待 %s / 実際 %s (%s)"
                            % (name, "検出" if should_fail else "合格",
                               "検出" if fired else "合格", problems))
            print("NG  %s" % name)
        else:
            print("ok  %s" % name)

    with tempfile.TemporaryDirectory() as tmp:
        rel = os.path.relpath(os.path.join(tmp, "selftest_input.lisp"), REPO_ROOT)
        with open(os.path.join(tmp, "selftest_input.lisp"), "wb") as f:
            f.write(b'(defun self-test-fixture (x) (+ x 1))\n')
        entries = [(rel, "g_selftest_src")]
        out = os.path.join(tmp, "selftest.c")
        write_output(out, entries)

        with open(out, encoding="utf-8") as f:
            good = f.read()

        # 陰性対照: 無傷の生成物は合格しなければならない
        expect("無傷の生成物は合格する", check(out, entries, verbose=False), False)

        def corrupt(name, text):
            with open(out, "w", encoding="utf-8") as f:
                f.write(text)
            expect(name, check(out, entries, verbose=False), True)

        # 1. 配列を 1 byte 短くする(長さだけが食い違う形)
        corrupt("配列が 1 byte 短いのを検出する",
                good.replace(",0x0a,\n};", ",\n};"))
        # 2. 長さだけ合っていて中身が違う形
        corrupt("中身が 1 byte 違うのを検出する",
                good.replace("0x28,0x64", "0x29,0x64", 1))
        # 3. _Static_assert の長さだけを書き換えた形
        m = re.search(r"_Static_assert\(sizeof\(g_selftest_src\) == (\d+)u,", good)
        corrupt("_Static_assert の長さの食い違いを検出する",
                good.replace("== %su," % m.group(1), "== %su," % (int(m.group(1)) + 1), 1))
        # 4. 印の長さだけを書き換えた形
        corrupt("印の長さの食い違いを検出する",
                re.sub(r"(ISIKI-EMBED: \S+ \S+ )(\d+)",
                       lambda mm: mm.group(1) + str(int(mm.group(2)) + 1), good, count=1))
        # 5. 長さを sizeof から導出する行を手書きの数値に変えた形
        corrupt("長さを sizeof から導出していないのを検出する",
                good.replace("const UINT32 g_selftest_src_len = (UINT32)sizeof(g_selftest_src);",
                             "const UINT32 g_selftest_src_len = 38;"))
        # 6. 生成物が無い形
        os.remove(out)
        expect("生成物が無いのを検出する", check(out, entries, verbose=False), True)

    if failures:
        print("\ngen_embedded_lisp --self-test: 失敗 %d 件" % len(failures))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("\ngen_embedded_lisp --self-test: 全件 OK")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", metavar="PATH", help="バイト配列の C ソースを生成する")
    ap.add_argument("--check", metavar="PATH", help="生成物を入力と突き合わせる")
    ap.add_argument("--print-inputs", action="store_true", help="同梱対象のパスを空白区切りで出す")
    ap.add_argument("--self-test", action="store_true", help="検出器の陽性対照")
    args = ap.parse_args(argv)

    if args.print_inputs:
        print(" ".join(path for path, _ in EMBEDDED))
        return 0
    if args.self_test:
        return self_test()
    if args.output:
        write_output(args.output, EMBEDDED)
        sizes = ", ".join("%s=%d" % (os.path.basename(p), len(_read_input(p)))
                          for p, _ in EMBEDDED)
        print("gen_embedded_lisp: %s (%s)" % (args.output, sizes))
        return 0
    if args.check:
        problems = check(args.check, EMBEDDED)
        if problems:
            print("\ngen_embedded_lisp --check: 不一致 %d 件" % len(problems))
            for p in problems:
                print("  - %s" % p)
            print("\n  同梱対象を編集したあとは make build か"
                  " `python3 tools/gen_embedded_lisp.py --output %s` で再生成する" % args.check)
            return 1
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
