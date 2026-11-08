#!/usr/bin/env python3
"""基盤ライブラリの二重読み込みを禁じる検出器(issue #184 / 決定 D2 / リスク R8)。

src/lisp/init.lisp と src/lisp/disassemble.lisp は**カーネルへ同梱され、起動時に
必ず読まれる**(src/c/kernel.c の run_boot_entry)。したがって試験の boot-entry
スクリプトや計測のドライバが冒頭で (load "src/lisp/init.lisp") を呼ぶと
**同じものを 2 回読む**ことになる。

二重読み込みは黙って通る。マクロ 31 個が再定義され、JIT したコードページは
**解放されない**(documents/constraint-jit-code-pages-are-immortal.md)ので、
Immobilized Space を恒久的に二重消費するだけで、試験は合格のまま出てくる。
**だから人が気づけない。この検出器がその役をする。**

逆に言えば、(load "src/lisp/init.lisp") を外した試験は
「同梱が効いていること」を毎回確かめる形になっている。

使い方:
    python3 tools/check_no_double_init_load.py            # 検査(make test から)
    python3 tools/check_no_double_init_load.py --self-test # 陽性対照(規則 8)
"""

import os
import re
import sys
import tempfile

# 同梱されているので、どこからも load してはならないソース
EMBEDDED = ("src/lisp/init.lisp", "src/lisp/disassemble.lisp")

# 走査する対象。試験の boot-entry・計測のドライバ・生成器を含める
SCAN_DIRS = ("test", "tools")
SCAN_EXTS = (".lisp", ".sh", ".py")

# 走査から外すもの。**理由を必ず書く**
EXEMPT = {
    # この検出器自身。EMBEDDED の名前を資料として持つ
    "tools/check_no_double_init_load.py":
        "検出器自身(パターンそのものを持つ)",
}

# 「実行としての load」だけを拾う。Lisp のコメント(;)とシェル/python のコメント(#)は除く。
# シェルの `echo '(load "src/lisp/init.lisp")'` のように引用符に包まれた形も拾う
# (boot-entry スクリプトを生成する側がここに当たる)
LOAD_RE = re.compile(r'\(\s*load\s+"(' + "|".join(re.escape(p) for p in EMBEDDED) + r')"')


def strip_comment(line, path):
    """行からコメント部分を落とす。文字列リテラルの中の ; / # は落とさない"""
    markers = (";",) if path.endswith(".lisp") else ("#",)
    out = []
    in_str = False
    i = 0
    while i < len(line):
        ch = line[i]
        if in_str:
            if ch == "\\":
                out.append(ch)
                if i + 1 < len(line):
                    out.append(line[i + 1])
                    i += 2
                    continue
            elif ch == '"':
                in_str = False
            out.append(ch)
        else:
            if ch == '"':
                in_str = True
                out.append(ch)
            elif ch in markers:
                break
            else:
                out.append(ch)
        i += 1
    return "".join(out)


def scan_file(path, rel):
    hits = []
    try:
        with open(path, encoding="utf-8") as fp:
            lines = fp.readlines()
    except (OSError, UnicodeDecodeError):
        return hits
    for n, line in enumerate(lines, 1):
        body = strip_comment(line, rel)
        m = LOAD_RE.search(body)
        if m:
            hits.append((rel, n, m.group(1), line.rstrip("\n")))
    return hits


def scan(root):
    hits = []
    for d in SCAN_DIRS:
        base = os.path.join(root, d)
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in sorted(filenames):
                if not fn.endswith(SCAN_EXTS):
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if rel in EXEMPT:
                    continue
                hits.extend(scan_file(full, rel))
    return hits


def report(hits):
    if not hits:
        print("check_no_double_init_load: OK "
              "(同梱済みのソースを load している箇所は無い)")
        return 0
    print("ERROR: 同梱済みのソースを load している箇所がある(二重読み込み)", file=sys.stderr)
    for rel, n, target, text in hits:
        print("  %s:%d  %s を load している" % (rel, n, target), file=sys.stderr)
        print("      %s" % text.strip(), file=sys.stderr)
    print("", file=sys.stderr)
    print("%s はカーネルへ同梱され、起動時に必ず読まれる"
          "(src/c/kernel.c の run_boot_entry)。" % " / ".join(EMBEDDED), file=sys.stderr)
    print("もう一度 load すると同じ定義が二重に JIT され、Immobilized Space を"
          "恒久的に二重消費する(コードページは解放されない)。", file=sys.stderr)
    print("**それでも試験は合格のまま通るので、人は気づけない。** load 行を外すこと。",
          file=sys.stderr)
    return 1


def self_test():
    """陽性対照(規則 8): 違反を置いたら落ち、置かなければ通ることを見せる"""
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for d in SCAN_DIRS:
            os.makedirs(os.path.join(tmp, d, "lisp"), exist_ok=True)

        probe = os.path.join(tmp, "test", "lisp", "qemu_boot_fixture.lisp")
        driver = os.path.join(tmp, "tools", "lisp", "run_fixture.sh")

        # 陰性(通るべき): load が無い / コメントの中にだけある / 別のファイルを load する
        with open(probe, "w", encoding="utf-8") as fp:
            fp.write(';; 従来は (load "src/lisp/init.lisp") で始まっていた\n'
                     '(load "test/lisp/test_framework.lisp")\n')
        with open(driver, "w", encoding="utf-8") as fp:
            fp.write("#!/bin/sh\n"
                     "# echo '(load \"src/lisp/init.lisp\")'  <- 外した\n"
                     "echo '(load \"test/lisp/test_framework.lisp\")'\n")
        hits = scan(tmp)
        if hits:
            print("self-test NG: 陰性対照が落ちた: %r" % (hits,), file=sys.stderr)
            ok = False
        else:
            print("self-test: 陰性対照(load 無し・コメントのみ)は通った")

        # 陽性1(落ちるべき): 試験の boot-entry が init.lisp を load する
        with open(probe, "a", encoding="utf-8") as fp:
            fp.write('(load "src/lisp/init.lisp")\n')
        hits = scan(tmp)
        if len(hits) != 1 or hits[0][2] != "src/lisp/init.lisp":
            print("self-test NG: 陽性対照1を検出できなかった: %r" % (hits,), file=sys.stderr)
            ok = False
        else:
            print("self-test: 陽性対照1(boot-entry の load)を検出した")

        # 陽性2(落ちるべき): ドライバが生成する boot-entry に埋め込む形
        with open(driver, "a", encoding="utf-8") as fp:
            fp.write("echo '(load \"src/lisp/disassemble.lisp\")'\n")
        hits = scan(tmp)
        targets = sorted(h[2] for h in hits)
        if targets != ["src/lisp/disassemble.lisp", "src/lisp/init.lisp"]:
            print("self-test NG: 陽性対照2を検出できなかった: %r" % (hits,), file=sys.stderr)
            ok = False
        else:
            print("self-test: 陽性対照2(生成器が echo する load)を検出した")

    if ok:
        print("check_no_double_init_load --self-test: OK")
        return 0
    return 1


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if "--self-test" in sys.argv[1:]:
        return self_test()
    return report(scan(root))


if __name__ == "__main__":
    sys.exit(main())
