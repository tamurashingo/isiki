#!/usr/bin/env python3
"""EVAL-ERROR を「値として返す」箇所の残数を数え、予算(上限)と突き合わせる。

## なぜ必要か

documents/error-unwind-survey.md の調査で分かったとおり、isiki-os の
エラー処理の穴は「コンディションを signal せず、シンボル EVAL-ERROR を
**普通の値として返す**」ことにある。`(list 1 (div 1 0) 2)` が
`(1 EVAL-ERROR 2)` になり、評価が止まらない(仕様 §9.1(a) 違反)。

これを 50 箇所ぶん潰していくのだが、**潰し漏れは静かに残る。**
戻り値の形が変わるだけなので、テストが無い経路は誰も気づかない。
だから数を数える計器を先に置く(documents/pitfalls.md の omission list、
documents/control-transfer-survey.md §7 と同じ考え方)。

## 予算(ratchet)

上限を**超えたら**失敗する。これは「新しい EVAL-ERROR 返しを足すな」。
上限を**下回っても**失敗する。これは「減らしたら予算も下げろ」。
下回ったときに黙って通すと、予算がすぐ形骸化して計器の意味が無くなる。
どちらの場合も、次に書くべき数字をメッセージに出す。

**予算は下の BUDGETS 1 箇所だけで定義する。** フェーズごとにここを下げる。

## 数えているもの

- `c_return`  : src/c/**.c の `return g_sym_eval_error`(生成物は除く)。
                調査時点で 50。これが本体
- `lisp_quote`: src/lisp/*.lisp の `'eval-error`。Lisp 側にも同じ病気があり
                (init_aot.lisp の slot-value/set-slot-value がエラー分岐で
                'eval-error を返し、file-cmd.lisp がそれを eq で受けている)、
                C 側だけ直しても片手落ちになる。producer と consumer を
                分けず総数で見るのは、両方 0 になるのが終点だから

いずれもコメントは数えない(C の // と /* */、Lisp の ;)。

## 使い方

    python3 tools/check_eval_error_budget.py            # 検査(make test から呼ばれる)
    python3 tools/check_eval_error_budget.py --list     # 箇所を全部出す
    python3 tools/check_eval_error_budget.py --self-test # 計器自身の陽性/陰性対照
"""

import os
import re
import sys

# --- 予算。**ここだけを書き換える。** -------------------------------------
# フェーズごとに下げていく。下げ忘れると「減ったぞ」と言って落ちる。
#
# [P4-2] 44 -> 26 まで下げたあと、**1 箇所だけ意図的に増やして 27 にした。**
# os_signal_condition に深さ打ち切り(SIGNAL_MAX_DEPTH)の脱出路を足したためで、
# これは「EVAL-ERROR を値として返す設計」を広げたのではなく、
# **条件システム自体が使えない状態のフォールバック**(既存の
# 「init.lisp 未ロード時」と同じ位置づけ)である。詳細は
# test/lisp/qemu_boot_no_init_signal.lisp の冒頭を見ること。
#
# [P4-3] 27 -> 21。未定義関数 / immutable-binding / 「関数でないものの呼び出し」の
# 6 箇所を signal へ移した。
#
# **runtime.c の os_make_array_from_nested_list の 3 箇所はこのまま残す。**
# あそこの EVAL-ERROR は評価器へ漏れる戻り値ではなく、唯一の呼び出し元である
# reader.c が直後に g_sym_read_error へ翻訳するための内部ステータスである
# (reader.c の「array == g_sym_eval_error」の分岐)。リーダーの構文エラーは
# 条件ではなく読み取りエラーの経路で報告するのが現在の設計なので、
# 数だけを合わせるために signal へ変えるのは筋が悪い。
#
# [P4-4] 21 -> 6。load.c 3 + stream_lisp.c 12 を片付けた。
# 残る 6 箇所はいずれも**意図して残すもの**:
#   runtime.c 3  条件システムが使えないときのフォールバック(深さ打ち切り/
#                make-instance 等が無い/クラスが引けない)
#   runtime.c 3  os_make_array_from_nested_list。reader.c が直後に
#                g_sym_read_error へ翻訳する内部ステータスで、評価器へは漏れない
# **したがってこの予算はもう下がらない。** 下げようとする変更があれば、
# それは上のどちらかの性質を変えているということなので、先に設計を見直すこと。
BUDGETS = {
    "c_return": 6,
    # [P4-4] 5 -> 2。file-cmd.lisp の read-file-into-vector / write-vector-to-file が
    # 見ていた EVAL-ERROR 返しは、open-input-stream / open-output-file / file-length が
    # signal(または仕様どおりの nil)へ変わったことで**起きなくなった**ため落とした。
    # 残る 2 箇所は init_aot.lisp の slot-value / set-slot-value のエラー分岐。
    "lisp_quote": 2,
}
# --------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# トランスパイラ/ツールの生成物。行数も内容も再生成で変わるので数えない
GENERATED = {"lisp_compiled.c", "lisp_compiled_fixture.c", "disasm_symtab.c"}

C_PATTERN = re.compile(r"\breturn\s+g_sym_eval_error\b")
LISP_PATTERN = re.compile(r"'eval-error\b", re.IGNORECASE)


def strip_c_comments(text):
    """C の // と /* */ を空白に潰す。文字列リテラル・文字定数の中は触らない。

    行数と桁数を保つため、消す代わりに空白へ置き換える(改行は残す)。
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue

        if c == "/" and nxt == "*":
            out.append("  ")
            i += 2
            while i < n and not (text[i] == "*" and text[i + 1 : i + 2] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue

        if c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i])
                    out.append(text[i + 1])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                if text[i] == "\n" and quote == "'":
                    # 閉じない ' は除算やアポストロフィではなく壊れた入力。抜ける
                    i += 1
                    break
                i += 1
            continue

        out.append(c)
        i += 1
    return "".join(out)


def strip_lisp_comments(text):
    """Lisp の `;` 以降を空白に潰す。文字列リテラルと文字定数 #\\; は触らない。"""
    out = []
    i = 0
    n = len(text)
    in_string = False
    while i < n:
        c = text[i]
        if in_string:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            out.append(c)
            i += 1
            continue
        if c == "#" and text[i : i + 2] == "#\\":
            # 文字定数。#\; の ; をコメント開始と誤らない
            out.append(text[i : i + 3])
            i += 3
            continue
        if c == ";":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def count_in_text(text, pattern, stripper):
    """コメントを除いた本文から pattern に当たる行番号を返す(1 始まり)。"""
    stripped = stripper(text)
    hits = []
    for lineno, line in enumerate(stripped.split("\n"), start=1):
        for _ in pattern.finditer(line):
            hits.append(lineno)
    return hits


def scan():
    """(category -> [(relpath, lineno), ...]) を返す。"""
    found = {"c_return": [], "lisp_quote": []}

    c_dir = os.path.join(REPO_ROOT, "src", "c")
    for dirpath, _dirnames, filenames in os.walk(c_dir):
        for name in sorted(filenames):
            if not name.endswith(".c") or name in GENERATED:
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, REPO_ROOT)
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
            for lineno in count_in_text(text, C_PATTERN, strip_c_comments):
                found["c_return"].append((rel, lineno))

    lisp_dir = os.path.join(REPO_ROOT, "src", "lisp")
    for name in sorted(os.listdir(lisp_dir)):
        if not name.endswith(".lisp"):
            continue
        path = os.path.join(lisp_dir, name)
        rel = os.path.relpath(path, REPO_ROOT)
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        for lineno in count_in_text(text, LISP_PATTERN, strip_lisp_comments):
            found["lisp_quote"].append((rel, lineno))

    return found


LABEL = {
    "c_return": "src/c/**.c の `return g_sym_eval_error`",
    "lisp_quote": "src/lisp/*.lisp の `'eval-error`",
}


def report(found, show_list):
    failed = False
    for key in ("c_return", "lisp_quote"):
        hits = found[key]
        budget = BUDGETS[key]
        per_file = {}
        for rel, lineno in hits:
            per_file.setdefault(rel, []).append(lineno)
        print(f"[eval-error budget] {LABEL[key]}: {len(hits)} 箇所 (予算 {budget})")
        for rel in sorted(per_file):
            lines = per_file[rel]
            if show_list:
                print(f"    {rel}: {len(lines)}  ({', '.join(str(n) for n in lines)})")
            else:
                print(f"    {rel}: {len(lines)}")
        # NG は stderr へ出すので、先に stdout を吐き切って順序を揃える
        # (揃えないと「NG の行だけ先に出て、どのファイルの話か分からない」出力になる)
        sys.stdout.flush()
        if len(hits) > budget:
            print(
                f"  NG: 予算 {budget} を {len(hits) - budget} 箇所超えた。"
                f"EVAL-ERROR を値として返す箇所を増やさないこと"
                f"(signal へ寄せる。documents/error-unwind-survey.md §A-4)",
                file=sys.stderr,
            )
            failed = True
        elif len(hits) < budget:
            print(
                f"  NG: 予算 {budget} に対して {budget - len(hits)} 箇所減っている。"
                f"tools/check_eval_error_budget.py の BUDGETS[\"{key}\"] を "
                f"{len(hits)} に下げること(予算を下げないと計器が形骸化する)",
                file=sys.stderr,
            )
            failed = True
        else:
            print("  OK")
    return failed


# --- 計器自身の検証(陽性対照・陰性対照) ----------------------------------

SELF_TEST_C_POSITIVE = """
lisp_val_t f(void) {
    return g_sym_eval_error;
}
"""

SELF_TEST_C_NEGATIVE = """
/* return g_sym_eval_error; ← ブロックコメントの中 */
lisp_val_t f(void) {
    // return g_sym_eval_error;  ← 行コメントの中
    const char *s = "return g_sym_eval_error";  /* 文字列の中は数える(誤検出の許容側) */
    if (x == g_sym_eval_error) { return nil; }   /* 比較は producer ではない */
    return nil;
}
"""

SELF_TEST_LISP_POSITIVE = "(defun f () 'eval-error)\n"

SELF_TEST_LISP_NEGATIVE = """;; 'eval-error ← コメントの中
(defun f () (if (eq x #\\;) 1 2))
"""


def self_test():
    ok = True

    def check(name, got, want):
        nonlocal ok
        mark = "OK" if got == want else "NG"
        if got != want:
            ok = False
        print(f"  [{mark}] {name}: {got} (期待 {want})")

    print("[self-test] 計器自身の陽性対照・陰性対照")
    check(
        "C 陽性(素の return が 1 件)",
        len(count_in_text(SELF_TEST_C_POSITIVE, C_PATTERN, strip_c_comments)),
        1,
    )
    # 陰性側: コメント 2 件は数えず、比較 1 件も数えない。文字列の中の 1 件だけ残る
    check(
        "C 陰性(コメント2件と比較1件を数えない)",
        len(count_in_text(SELF_TEST_C_NEGATIVE, C_PATTERN, strip_c_comments)),
        1,
    )
    check(
        "Lisp 陽性",
        len(count_in_text(SELF_TEST_LISP_POSITIVE, LISP_PATTERN, strip_lisp_comments)),
        1,
    )
    check(
        "Lisp 陰性(コメントを数えない・#\\; をコメント開始と誤らない)",
        len(count_in_text(SELF_TEST_LISP_NEGATIVE, LISP_PATTERN, strip_lisp_comments)),
        0,
    )

    # 予算判定そのものの陽性対照: 実際の数に +1 / -1 した予算で必ず落ちること
    found = scan()
    real = len(found["c_return"])
    saved = BUDGETS["c_return"]
    try:
        BUDGETS["c_return"] = real - 1
        over = report_quiet(found)
        check("予算超過で落ちる", over, True)
        BUDGETS["c_return"] = real + 1
        under = report_quiet(found)
        check("予算未達で落ちる", under, True)
        BUDGETS["c_return"] = real
        exact = report_quiet(found)
        check("ちょうどで通る", exact, False)
    finally:
        BUDGETS["c_return"] = saved
    return ok


def report_quiet(found):
    """report と同じ判定だけを行う(出力しない)。self-test 用。"""
    for key in ("c_return", "lisp_quote"):
        if len(found[key]) != BUDGETS[key]:
            return True
    return False


def main(argv):
    if "--self-test" in argv:
        return 0 if self_test() else 1
    found = scan()
    failed = report(found, show_list="--list" in argv)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
