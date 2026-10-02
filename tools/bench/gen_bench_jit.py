#!/usr/bin/env python3
"""[性能測定] src/lisp/bench_jit.lisp を src/lisp/bench_aot.lisp から生成する。

なぜ生成するのか(documents/performance-measurement.md「JIT 経路の基準値」節):

ベンチの本体の S 式が AOT 版と JIT 版で違うと、2 つの数字は比較できない。
そして同じ S 式を 2 箇所に書けば、いつか片方だけ直される。
したがって本体の真実源は src/lisp/bench_aot.lisp **だけ**とし、JIT 版は
関数名を %%bench-aot-* -> %%bench-jit-* へ置き換えた機械的な写しにする。

Lisp のマクロ 1 つから両方を作る形(作業指示書の案 a)は採れない。理由:
  - AOT トランスパイラは src/lisp/transpile.lisp 内の *macro-expanders* に
    移植済みのマクロしか展開しない。入力ファイル中の defmacro は展開されない。
  - transpile.lisp の toplevel-defun-p は (car form) が literal の 'defun で
    あることを要求する(transpile.lisp:2213)。defun を生成するマクロ呼び出しは
    「defun 以外のトップレベルフォーム」に分類され、transpile-toplevel-form へ
    回ってコンパイルに失敗する。
どちらも実装を読んで確認した(2026-10-02)。

生成物は git 管理下に置く(ゲストは実行時に 9p 越しの
src/lisp/bench_jit.lisp を load するため、ビルド時生成では間に合わない)。
同期は tools/bench/check_bench_jit_sync.sh が diff で固定する。
"""

import re
import sys

SRC = "src/lisp/bench_aot.lisp"
DST = "src/lisp/bench_jit.lisp"

HEADER = ''';;;; 構文別ベンチマークスイートのJIT経路(defun->za_try_compile_defun)実装。
;;;;
;;;; **このファイルは生成物である。直接編集しないこと。**
;;;;   真実源: src/lisp/bench_aot.lisp
;;;;   生成:   python3 tools/bench/gen_bench_jit.py
;;;;   同期の固定: make test-bench-jit-sync (tools/bench/check_bench_jit_sync.sh)
;;;;
;;;; 本体の S 式は bench_aot.lisp と1文字も違わない(関数名の
;;;; %%bench-aot-* -> %%bench-jit-* の置換だけが差分)。そうしないと
;;;; AOT 経路と JIT 経路の命令数を比較しても意味を持たない。
;;;;
;;;; [性能測定] documents/performance-measurement.md「JIT 経路の基準値」節を参照。
;;;; AOT 版(%%bench-aot-*、カーネルへ埋め込み済みのネイティブコード)と違い、
;;;; こちらは実行時に load して defun させるため za_try_compile_defun が走る。
;;;; **測定の前に %%za-compiled-p が T であることを必ず確認すること**
;;;; (test/lisp/bench_jit_guard.lisp)。インタプリタへ落ちた関数を測ると
;;;; 「JIT を測ったつもりでインタプリタを測った数字」が基準値として残る。
'''


def generate(src_text):
    lines = src_text.split("\n")
    for i, line in enumerate(lines):
        if line.startswith(";;; --- "):
            body_start = i
            break
    else:
        sys.exit("ERROR: bench_aot.lisp に ';;; --- ' で始まる節見出しが無い")
    body = "\n".join(lines[body_start:])
    renamed, n = re.subn(r"%%bench-aot-", "%%bench-jit-", body)
    if n == 0:
        sys.exit("ERROR: %%bench-aot- が本体に1つも無い(関数名の規約が変わった?)")
    if "bench-aot" in renamed:
        sys.exit("ERROR: 置換後も bench-aot が残っている: "
                 + repr([l for l in renamed.split("\n") if "bench-aot" in l]))
    return HEADER + "\n" + renamed


def main():
    with open(SRC, encoding="utf-8") as f:
        out = generate(f.read())
    if len(sys.argv) > 1:
        path = sys.argv[1]
    else:
        path = DST
    with open(path, "w", encoding="utf-8") as f:
        f.write(out)
    print(f"generated {path} from {SRC}")


if __name__ == "__main__":
    main()
