#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PR 本文が「成果物」として挙げたものが、**マージ後の木に実在するか**を突き合わせる。

documents/records-and-measurement-rules.md 規則 15 (R-claimed)。

**なぜ要るか。** PR #113(2026-09-29 マージ)の本文は「新しく常設になったもの」4 件と
「文書」3 件を表で挙げていたが、**そのうち 3 件がマージされた木に存在しなかった:**

    make test-qemu-error-escape-gc が test-qemu-all に入っている(357 件) -> 無い
    documents/islisp-implementation-defined.md を新設した               -> 無い
    error-unwind-survey.md の冒頭に「現状ではない」と明記済み            -> 注記が無い

**誰も消していない。「最初から入っていなかった」。** 整理コミットが別ブランチ
(feature/arity-check)へコミットされていて、PR が merge した先端がそれより前だった。
**本文の書き手が見ていた作業木は正しかった。束ねた先端がそれと違っていた。**
PR が複数ブランチを束ねるとき(#113 は 7 本)に起きる。

**気づいたのは 1 か月後、それを使おうとしたとき**である。

使い方:
    tools/check_pr_claims.py 113                 # PR #113 の本文を今の木と突き合わせる
    tools/check_pr_claims.py 113 --at 33cc8b3    # 指定リビジョンの木と突き合わせる
    tools/check_pr_claims.py --body-file b.md    # gh を使わず本文をファイルから読む
    tools/check_pr_claims.py --self-test         # 検出器が落ちることを確かめる(規則 8)

**この検出器は「本文に出てくる名前が実在するか」しか見ない。**
「357 件通る」のような**中身の主張は見られない** — それは実際に動かすしかない
(#113 の件も、回収してから 357 件を実測して一致を確かめた)。
"""

import re
import subprocess
import sys

#: 自己試験に使う PR #113 の本文(gh に依存しないよう固定してある)
FIXTURE_113 = "test/fixtures/pr113_body.md"

# 本文から抜き出すファイルパス。拡張子で絞る(散文の中の語を拾わないため)
PATH_RE = re.compile(
    r"(?:documents|test|tools|src|images)/[A-Za-z0-9_./-]+"
    r"\.(?:md|lisp|py|sh|c|h|sexp|tsv|txt|json|img)"
)
# `make <target>` と、裸で書かれた test-qemu-* / check-* のターゲット名
TARGET_RE = re.compile(r"(?:make\s+|`)((?:test-qemu|check|jit-frame|let)-[a-z0-9-]+|test|build|image)\b")


def git_show(rev, path):
    r = subprocess.run(["git", "cat-file", "-e", "%s:%s" % (rev, path)],
                       capture_output=True, text=True)
    return r.returncode == 0


def read_at(rev, path):
    r = subprocess.run(["git", "show", "%s:%s" % (rev, path)],
                       capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else ""


def exists(path, rev):
    if rev:
        return git_show(rev, path)
    import os
    return os.path.exists(path)


def makefile_text(rev):
    if rev:
        return read_at(rev, "Makefile")
    return open("Makefile", encoding="utf-8").read()


def get_body(pr, body_file):
    if body_file:
        return open(body_file, encoding="utf-8").read()
    r = subprocess.run(["gh", "pr", "view", str(pr), "--json", "body", "-q", ".body"],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("ERROR: gh pr view に失敗した。--body-file で本文を渡すこと\n" + r.stderr[:500])
    return r.stdout


def check(body, rev, quiet=False):
    paths = sorted(set(PATH_RE.findall(body)))
    targets = sorted(set(TARGET_RE.findall(body)))
    mk = makefile_text(rev)
    target_defined = {t: re.search(r"^%s:" % re.escape(t), mk, re.M) is not None
                      for t in targets}

    missing_paths = [p for p in paths if not exists(p, rev)]
    missing_targets = [t for t in targets if not target_defined[t]]

    if not quiet:
        where = rev if rev else "作業木"
        print("突き合わせ先: %s" % where)
        print()
        print("| 本文に出てくるもの | 種別 | 実在 |")
        print("|---|---|---|")
        for p in paths:
            print("| `%s` | ファイル | %s |" % (p, "あり" if exists(p, rev) else "**無い**"))
        for t in targets:
            print("| `make %s` | ターゲット | %s |" % (t, "あり" if target_defined[t] else "**無い**"))
        print()

    if missing_paths or missing_targets:
        if not quiet:
            print("**本文が挙げているのに実在しないもの:**")
            for p in missing_paths:
                print("  - ファイル `%s`" % p)
            for t in missing_targets:
                print("  - ターゲット `make %s`" % t)
            print()
            print("NG: %d 件が実在しない。**本文が、その PR が含んでいないものを"
                  "成果として書いている**(規則 15)" % (len(missing_paths) + len(missing_targets)))
        return 1
    if not quiet:
        print("OK: 本文が挙げた %d 件(ファイル %d / ターゲット %d)はすべて実在する"
              % (len(paths) + len(targets), len(paths), len(targets)))
    return 0


def self_test():
    """規則 8: **実際に起きた事例**で、検出器が落ちることと通ることを確かめる。

    PR #113 の本文を使う。
      33cc8b3(PR #121 のマージ。#122 の回収より前) -> **落ちる**
      HEAD(#122 で回収したあと)                     -> **通る**

    合成した資料ではなく履歴そのものを使うので、
    「検出器が当時の事故を捕まえられたか」をそのまま確かめられる。

    **本文は test/fixtures/pr113_body.md に固定してある。**
    gh(= ネットワーク)に依存すると make test に入れられないため。
    固定した資料なので、GitHub 側で本文が編集されても自己試験は動かない。
    """
    body = open(FIXTURE_113, encoding="utf-8").read()
    before = check(body, "33cc8b3", quiet=True)
    after = check(body, "HEAD", quiet=True)
    ok = True
    if before == 1:
        print("OK(陽性対照): #113 の本文は 33cc8b3 の木に対して落ちた")
    else:
        print("FAIL(陽性対照): 33cc8b3 の木に対して落ちない。**検出器が効いていない**",
              file=sys.stderr)
        ok = False
    if after == 0:
        print("OK(陰性対照): #122 で回収したあとの木に対しては通った")
    else:
        print("FAIL(陰性対照): 回収したのに落ちる。突き合わせの基準が厳しすぎる",
              file=sys.stderr)
        ok = False
    return 0 if ok else 1


def main(argv):
    args = argv[1:]
    if "--self-test" in args:
        return self_test()
    rev = None
    body_file = None
    pr = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--at":
            i += 1
            rev = args[i]
        elif a == "--body-file":
            i += 1
            body_file = args[i]
        elif not a.startswith("--"):
            pr = a
        i += 1
    if pr is None and body_file is None:
        sys.exit(__doc__)
    return check(get_body(pr, body_file), rev)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
