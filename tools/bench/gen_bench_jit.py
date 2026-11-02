#!/usr/bin/env python3
"""[性能測定] src/lisp/bench_jit.lisp と src/lisp/bench_interp.lisp を
src/lisp/bench_aot.lisp から生成する。

なぜ生成するのか(documents/performance-measurement.md「JIT 経路の基準値」節):

ベンチの本体の S 式が経路ごとに違うと、その数字は比較できない。
そして同じ S 式を 2 箇所に書けば、いつか片方だけ直される。
したがって本体の真実源は src/lisp/bench_aot.lisp **だけ**とし、他の経路は
そこからの機械的な写しにする。

  jit     関数名を %%bench-aot-* -> %%bench-jit-* へ置き換えるだけ
  interp  同じ本体を (defglobal %%bench-interp-* (lambda ...)) で包む
          (**lambda は必ずインタプリタになる**。下記)

**ファイル名が gen_bench_jit.py のままである理由。**
2026-11-01 に interp を足したので「jit」の名は実体より狭い。それでも
改名しなかったのは、この名前が documents/for-expansion.md・
documents/records-and-measurement-rules.md の棚卸し表・
docs/investigation/compiler-optimization.refs.md から引かれていて、
**改名すると引用側の 3 箇所が静かに嘘になる**ためである(規則 13 の
「引用された側も直す」と同じ理由。直すこと自体は可能だが、計器を足す
作業の中で参照の張り替えを混ぜると、どちらが原因で検査が落ちたのか
分からなくなる)。生成器は**1 つのまま**で、そこが要点である。

**なぜ interp は lambda で包むのか。**
インタプリタ経路を測るには「同じ本体を os_eval で評価させる」必要がある。
`defun` は src/c/eval.c の eval_defun が必ず za_try_compile_defun を通すので、
JIT に乗る形を書いた時点で JIT 経路になってしまう。**カーネルに JIT を切る
つまみは無い**(za_try_compile_defun に大域の無効化フラグは存在せず、
ISIKIOS_UNIT_TEST のときだけ常に nil を返す)。
一方 eval_lambda は make_interpreted_function を直接呼ぶ**だけ**で、
コンパイルを試しもしない(src/c/eval.c)。つまり
**「lambda は必ずインタプリタ」は既存の実装の性質であり、新しいつまみではない。**
これを使う。新しいつまみを足すと、そのつまみ自体が測定対象を変える。

**interp 版で呼べない関数がある。**
`defglobal` は変数を定義するので、本体から別のベンチ関数を**名前で**呼ぶ形
(%%bench-aot-tailrec が %%bench-aot-tailrec-step を呼ぶ等)は
interp 版では `<undefined-function>` になる。呼び先も lambda にすると
funcall を挟むことになり、**本体の S 式が真実源と変わってしまう**ので
そうしない。代わりに生成器が「呼べない」ものを見つけて印を付け、
ドライバ(tools/bench/run_interp_bench.sh)がその印を grep して拒否する。
**どれが呼べないかは記録ではなく導出である**(規則 13)。

Lisp のマクロ 1 つから両方を作る形(作業指示書の案 a)は採れない。理由:
  - AOT トランスパイラは src/lisp/transpile.lisp 内の *macro-expanders* に
    移植済みのマクロしか展開しない。入力ファイル中の defmacro は展開されない。
  - transpile.lisp の toplevel-defun-p は (car form) が literal の 'defun で
    あることを要求する(transpile.lisp:2213)。defun を生成するマクロ呼び出しは
    「defun 以外のトップレベルフォーム」に分類され、transpile-toplevel-form へ
    回ってコンパイルに失敗する。
どちらも実装を読んで確認した(2026-10-02)。

生成物は git 管理下に置く(ゲストは実行時に 9p 越しの
src/lisp/bench_jit.lisp / bench_interp.lisp を load するため、
ビルド時生成では間に合わない)。
同期は tools/bench/check_bench_jit_sync.sh が diff で固定する。
"""

import argparse
import re
import sys

SRC = "src/lisp/bench_aot.lisp"
DST_JIT = "src/lisp/bench_jit.lisp"
DST_INTERP = "src/lisp/bench_interp.lisp"

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

INTERP_HEADER_FMT = ''';;;; 構文別ベンチマークスイートの**解釈実行(インタプリタ)経路**の実装。
;;;;
;;;; **このファイルは生成物である。直接編集しないこと。**
;;;;   真実源: src/lisp/bench_aot.lisp
;;;;   生成:   python3 tools/bench/gen_bench_jit.py
;;;;   同期の固定: make check-bench-jit-sync (tools/bench/check_bench_jit_sync.sh)
;;;;
;;;; 本体の S 式は bench_aot.lisp と1文字も違わない。差分は
;;;;   (defun %%bench-aot-X (args) BODY)
;;;;     -> (defglobal %%bench-interp-X (lambda (args) BODY))
;;;; の**包み方だけ**である。
;;;;
;;;; [性能測定] documents/performance-measurement.md「解釈実行の経路の基準値」節。
;;;;
;;;; **なぜ lambda なのか。** defun は src/c/eval.c の eval_defun が必ず
;;;; za_try_compile_defun を通すので、書いた時点で JIT 経路になる。
;;;; eval_lambda は make_interpreted_function を直接呼ぶだけで、コンパイルを
;;;; 試しもしない。**「lambda は必ずインタプリタ」は既存の実装の性質であり、
;;;; この計器のために足したつまみではない**(カーネルに JIT を切るつまみは無い)。
;;;;
;;;; **測定の前に %%za-compiled-p が NIL であることを必ず確認すること**
;;;; (test/lisp/bench_interp_guard.lisp)。JIT 側のゲートの**逆向き**である。
;;;; lambda が将来コンパイルされるようになったら、この行は黙って
;;;; 「インタプリタを測ったつもりで JIT を測った数字」になる。
;;;; 名前は根拠にならない(issue #114)。
;;;;
;;;; **他の経路の数字と直接比べてはいけない。** N が 1 桁違う
;;;; (解釈実行は AOT/JIT より 1〜2 桁遅いので、同じ N では測定時間が爆発する)。
;;;; この行は「解釈実行の経路の中での前後比較」にだけ使う。
;;;;
;;;; **呼べない関数がある**(下記)。本体が別のベンチ関数を名前で呼ぶものは
;;;; defglobal では解決できない。呼び先も lambda にすると funcall を挟むことに
;;;; なり本体の S 式が真実源と変わるので、そうはしない。
;;;; ドライバはこの印を grep して拒否する(件数は導出。規則 13)。
{uncallable}'''


def extract_body(src_text):
    """真実源から ';;; --- ' で始まる最初の節見出し以降を切り出す。"""
    lines = src_text.split("\n")
    for i, line in enumerate(lines):
        if line.startswith(";;; --- "):
            return "\n".join(lines[i:])
    sys.exit("ERROR: bench_aot.lisp に ';;; --- ' で始まる節見出しが無い")


def rename(body, suffix):
    renamed, n = re.subn(r"%%bench-aot-", "%%bench-" + suffix + "-", body)
    if n == 0:
        sys.exit("ERROR: %%bench-aot- が本体に1つも無い(関数名の規約が変わった?)")
    if "bench-aot" in renamed:
        sys.exit("ERROR: 置換後も bench-aot が残っている: "
                 + repr([l for l in renamed.split("\n") if "bench-aot" in l]))
    return renamed


def generate(src_text):
    return HEADER + "\n" + rename(extract_body(src_text), "jit")


# ---------------------------------------------------------------------------
# interp 版の生成
#
# **なぜ括弧を数えるのか。** defun を defglobal + lambda へ包み替えるには
# 「その defun がどこで終わるか」が要る。行数や空行で区切ると、本体に空行を
# 1 つ入れただけで生成物が壊れる(しかも diff は通ってしまう形で壊れうる)。
# 数えるのは 25 行で済むので数える。文字列と ; コメントと #\ 文字リテラルは
# 括弧として数えない(真実源にはいま文字列は無いが、入ったときに静かに
# 壊れる形にはしない)。
# ---------------------------------------------------------------------------
def toplevel_forms(text):
    """トップレベルの (…) の範囲を (開始, 終了) の組で返す。"""
    i, n, depth, start = 0, len(text), 0, None
    out = []
    while i < n:
        ch = text[i]
        if ch == ";":
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if ch == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if ch == "#" and i + 1 < n and text[i + 1] == "\\":
            i += 3
            continue
        if ch == "(":
            if depth == 0:
                start = i
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                out.append((start, i + 1))
            elif depth < 0:
                sys.exit(f"ERROR: 括弧が閉じすぎている(位置 {i})")
        i += 1
    if depth != 0:
        sys.exit("ERROR: 括弧が閉じていない(真実源が壊れている)")
    return out


# (defun NAME (p1 p2 ...) で始まることを要求する。AOT の制約により仮引数は
# シンボルのみ(bench_aot.lisp 冒頭の記述方針)なので、括弧は入らない
DEFUN_HEAD = re.compile(r"\(defun\s+(\S+)\s+\(([^()]*)\)")


def generate_interp(src_text):
    body = rename(extract_body(src_text), "interp")
    out = []
    uncallable = []
    pos = 0
    for start, end in toplevel_forms(body):
        out.append(body[pos:start])   # 節見出しと注釈はそのまま運ぶ
        pos = end
        form = body[start:end]
        m = DEFUN_HEAD.match(form)
        if not m:
            sys.exit("ERROR: トップレベルに defun 以外のフォームがある: "
                     + repr(form[:60]))
        name, params = m.group(1), m.group(2)
        inner = form[m.end():-1]      # defun の閉じ括弧だけを落とす(本体は逐語)
        # **本体が別のベンチ関数を名前で呼んでいたら呼べない。**
        # defglobal は変数を定義するので、関数位置の名前は解決されない
        if "%%bench-interp-" in inner:
            uncallable.append(name)
            out.append(f";;; [呼べない] {name} "
                       f"-- 本体が別のベンチ関数を名前で呼ぶ"
                       f"(defglobal は変数なので関数位置では解決されない)\n")
        out.append(f"(defglobal {name} (lambda ({params}){inner}))")
    out.append(body[pos:])
    if not uncallable:
        header_note = ";;;;   (呼べないものは無い)\n"
    else:
        header_note = "".join(f";;;;   [呼べない] {u}\n" for u in uncallable)
    return INTERP_HEADER_FMT.format(uncallable=header_note) + "\n" + "".join(out)


TARGETS = {"jit": (generate, DST_JIT), "interp": (generate_interp, DST_INTERP)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?",
                    help="書き出し先。省略すると 2 つとも既定の場所へ書く")
    ap.add_argument("--target", choices=sorted(TARGETS), default=None,
                    help="path を指定したときにどちらを生成するか(既定 jit)")
    args = ap.parse_args()

    with open(SRC, encoding="utf-8") as f:
        src = f.read()

    if args.path is None:
        if args.target is not None:
            sys.exit("ERROR: --target は path と一緒に指定すること")
        targets = [(gen, dst) for gen, dst in TARGETS.values()]
    else:
        gen, _ = TARGETS[args.target or "jit"]
        targets = [(gen, args.path)]

    for gen, dst in sorted(targets, key=lambda t: t[1]):
        with open(dst, "w", encoding="utf-8") as f:
            f.write(gen(src))
        print(f"generated {dst} from {SRC}")


if __name__ == "__main__":
    main()
