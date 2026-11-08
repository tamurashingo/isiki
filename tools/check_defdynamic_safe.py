#!/usr/bin/env python3
"""共有される大域状態の初期宣言が「安全な形」であることの検出器(issue #187)。

## 何を見ているか

`(defdynamic NAME VALUE-FORM)` の VALUE-FORM が、**既に値があればそれをそのまま
書き戻す形**になっていることだけを見る。許す形は 2 つ:

    (defdynamic *x* (dynamic *x*))              ; 初期値が nil のもの
    (defdynamic *x* (or (dynamic *x*) INIT))    ; 初期値が nil でないもの

## なぜ

`defdynamic` は **value-form を評価してから無条件に `os_set_dynamic` で上書きする**
(`src/c/eval.c` の `eval_defdynamic`、JIT は `src/c/za.c` の `za_compile_defdynamic`、
AOT は `src/lisp/transpile.lisp` の `transpile-defdynamic`。3 経路とも同じ規約)。
そして `(dynamic *x*)` は**未登録なら静かに nil を返す**
(`src/c/runtime.c` の `os_get_dynamic`)。

したがって素の初期値を書くと、**先に登録された値を無言で消す。**
過去に実際に事故が起きている(#47 / #48 の真因: AOT のトップレベルフォームが
登録した `write-from!` / `read-into!` のメソッドを、あとから走る `defdynamic` が
nil で踏み潰した)。

基盤ライブラリ(`src/lisp/init.lisp`)は**カーネルへ同梱され、起動ごとに必ず読まれる**
ようになった(`src/c/kernel.c` の `run_boot_entry`)。この事故の露出は
**「試験のときだけ」から「毎回」に変わった**。だから機械で見る。

**「登録済みで値が nil」と「未登録」は Lisp 側から区別できない**
(述語が無い。`g_dynamic_bindings` は Lisp へ公開されていない)。
初期値が nil のものはどちらでも結果が nil なので区別が要らず、
初期値が nil でないものは `or` で「偽なら初期値」に落とす。これが取れる最善の形である。

## 使い方

    python3 tools/check_defdynamic_safe.py             # 検査(make test から)
    python3 tools/check_defdynamic_safe.py --list      # 一覧を出す(完了条件の「一覧」)
    python3 tools/check_defdynamic_safe.py --self-test # 陽性対照(規則 8)
"""

import os
import sys
import tempfile

SCAN_DIR = "src/lisp"

# 走査から外すファイル。**理由を必ず書く**
EXEMPT = {
    # ホスト側(roswell/SBCL)で走るトランスパイラ自身。ゲストの初期宣言ではなく、
    # `(defdynamic name value-form)` という**処理する側の資料**として現れる
    # (transpile-defdynamic の docstring)
    "transpile.lisp": "ホスト側のトランスパイラ自身(ゲストの初期宣言ではない)",
}


def strip_comments(text):
    """Lisp のコメント(;)を落とす。文字列リテラルの中の ; は落とさない。
    落とした分は空白で埋めて**オフセットと行番号を保つ**(位置を報告するため)"""
    out = []
    in_str = False
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if in_str:
            if ch == "\\" and i + 1 < n:
                out.append(ch)
                out.append(text[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
            out.append(ch)
            i += 1
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if ch == ";":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def read_form(text, start):
    """text[start] の '(' から対応する ')' までを返す。文字列リテラルを考慮する。
    @return (フォームの文字列, 終端の次のインデックス)。閉じなければ (None, None)"""
    assert text[start] == "("
    depth = 0
    in_str = False
    i = start
    n = len(text)
    while i < n:
        ch = text[i]
        if in_str:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1], i + 1
        i += 1
    return None, None


def split_elements(form):
    """フォーム "(a b (c d))" を ["a", "b", "(c d)"] に割る(最上位の要素だけ)"""
    body = form[1:-1]
    elems = []
    cur = []
    depth = 0
    in_str = False
    i = 0
    n = len(body)
    while i < n:
        ch = body[i]
        if in_str:
            cur.append(ch)
            if ch == "\\" and i + 1 < n:
                cur.append(body[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            cur.append(ch)
        elif ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            cur.append(ch)
        elif ch.isspace() and depth == 0:
            if cur:
                elems.append("".join(cur))
                cur = []
        else:
            cur.append(ch)
        i += 1
    if cur:
        elems.append("".join(cur))
    return elems


def normalize(s):
    """空白の並びを 1 つにそろえる(複数行にまたがる形を比較するため)"""
    return " ".join(s.split())


def collect(root):
    """[(rel, lineno, name, value_form, safe, why)] を返す"""
    found = []
    base = os.path.join(root, SCAN_DIR)
    for fn in sorted(os.listdir(base)):
        if not fn.endswith(".lisp") or fn in EXEMPT:
            continue
        rel = "%s/%s" % (SCAN_DIR, fn)
        raw = open(os.path.join(base, fn), encoding="utf-8").read()
        text = strip_comments(raw)
        pos = 0
        while True:
            idx = text.find("(defdynamic", pos)
            if idx < 0:
                break
            # "(defdynamicfoo" のような別名を拾わない
            after = idx + len("(defdynamic")
            if after < len(text) and not (text[after].isspace() or text[after] == "("):
                pos = after
                continue
            form, nxt = read_form(text, idx)
            if form is None:
                found.append((rel, text.count("\n", 0, idx) + 1, "?", "", False,
                              "括弧が閉じていない"))
                break
            lineno = text.count("\n", 0, idx) + 1
            elems = split_elements(form)
            if len(elems) < 3:
                found.append((rel, lineno, elems[1] if len(elems) > 1 else "?", "",
                              False, "value-form が無い(初期値を書かない形は認めない)"))
            else:
                name = elems[1]
                value = normalize(elems[2])
                safe, why = is_safe(name, value)
                found.append((rel, lineno, name, value, safe, why))
            pos = nxt
    return found


def is_safe(name, value):
    """安全な形かを判定する。@return (safe, why)"""
    want_read = "(dynamic %s)" % name
    if value == want_read:
        return True, "既存値をそのまま書き戻す"
    if value.startswith("(or " + want_read + " ") and value.endswith(")"):
        return True, "既存値が偽のときだけ初期値へ落とす"
    return False, ("value-form が %s でも (or %s INIT) でもない"
                   % (want_read, want_read))


def report(found, show_list):
    unsafe = [f for f in found if not f[4]]
    if show_list or unsafe:
        print("共有される大域状態の初期宣言 %d 件(%s/*.lisp):" % (len(found), SCAN_DIR))
        for rel, lineno, name, value, safe, why in found:
            print("  [%s] %s:%d  %s  <- %s" %
                  ("OK" if safe else "NG", rel, lineno, name, value or "(value-form 無し)"))
    if unsafe:
        print("", file=sys.stderr)
        print("ERROR: 安全でない初期宣言が %d 件ある" % len(unsafe), file=sys.stderr)
        for rel, lineno, name, _value, _safe, why in unsafe:
            print("  %s:%d  %s: %s" % (rel, lineno, name, why), file=sys.stderr)
        print("", file=sys.stderr)
        print("defdynamic は value-form を評価してから**無条件に上書きする**。",
              file=sys.stderr)
        print("素の初期値を書くと、起動前半に機械語化された層が書き込んだ登録を"
              "無言で消す(#47/#48 の真因)。", file=sys.stderr)
        print("次のどちらかの形にすること:", file=sys.stderr)
        print("  (defdynamic *x* (dynamic *x*))            ; 初期値が nil のもの",
              file=sys.stderr)
        print("  (defdynamic *x* (or (dynamic *x*) INIT))  ; 初期値が nil でないもの",
              file=sys.stderr)
        return 1
    print("check_defdynamic_safe: OK (初期宣言 %d 件すべてが安全な形)" % len(found))
    return 0


SELF_TEST_CASES = [
    # (資料, 安全であるべきか, 説明)
    ("(defdynamic *x* (dynamic *x*))", True, "既存値の書き戻し"),
    ('(defdynamic *x* (or (dynamic *x*) "/"))', True, "or で初期値へ落とす"),
    ("(defdynamic *x*\n  (or (dynamic *x*) (%%f)))", True, "複数行にまたがる or"),
    ("(defdynamic *x* nil)", False, "素の nil(これが #47/#48 の形)"),
    ('(defdynamic *x* "/")', False, "素の初期値"),
    ("(defdynamic *x* (dynamic *y*))", False, "**別の名前**を読んでいる"),
    ("(defdynamic *x* (or (dynamic *y*) nil))", False, "or の中が別の名前"),
    ("(defdynamic *x*)", False, "value-form が無い"),
    ("(defdynamic *x* (if (dynamic *x*) (dynamic *x*) 1))", False,
     "if 版は認めない(形を 2 つに固定して検出器を単純に保つ)"),
]


def self_test():
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, SCAN_DIR))
        for i, (src, want_safe, desc) in enumerate(SELF_TEST_CASES):
            path = os.path.join(tmp, SCAN_DIR, "case.lisp")
            with open(path, "w", encoding="utf-8") as fp:
                # コメントの中に危険な形を置いて、拾わないことも同時に見る
                fp.write(";; (defdynamic *comment-only* nil)\n")
                fp.write(src + "\n")
            found = collect(tmp)
            if len(found) != 1:
                print("self-test NG[%d]: %s -> 件数が %d(コメントを拾った?)"
                      % (i, desc, len(found)), file=sys.stderr)
                ok = False
                continue
            got_safe = found[0][4]
            if got_safe != want_safe:
                print("self-test NG[%d]: %s -> safe=%s (期待 %s)"
                      % (i, desc, got_safe, want_safe), file=sys.stderr)
                ok = False
            else:
                print("self-test [%d] %s: %s"
                      % (i, "安全と判定" if want_safe else "**落ちた**", desc))
    if ok:
        print("check_defdynamic_safe --self-test: OK "
              "(陽性 %d 件・陰性 %d 件)"
              % (sum(1 for c in SELF_TEST_CASES if not c[1]),
                 sum(1 for c in SELF_TEST_CASES if c[1])))
        return 0
    return 1


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    return report(collect(root), "--list" in args)


if __name__ == "__main__":
    sys.exit(main())
