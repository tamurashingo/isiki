#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""**読み込みの打ち切りが母艦に残っているか**を見る(#197)。

## なぜ必要か

`load` は個々の最上位の式が打ち切られても**成功を返し、理由を画面にしか出さない**。
画面も直列線もつないでいない段では、母艦から見て**何も起きていないのと区別が付かない**。
実害の形は「アプリ(や試験ファイル)が**半分だけ静かに欠ける**」ことで、
成果物に残る痕跡は無かった(`docs/investigation/detector-t4-passline.logic.md` §4-3)。

さらに、打ち切りとして**数えられてさえいない**脱出があった:
対応する `catch` の無い `throw` と、対応する `tagbody` の無い `go` は
生の脱出シグナルのまま最上位へ戻るので、件数も増えず画面にも出なかった(同 §C-2)。

ゲスト側(`src/c/load.c` / `src/c/kernel.c`)で数えて直列線へ出すようにし、
**その行を読む者**がこの道具である。

## 何を見るか(4 つ)

1. **行があること。** 無ければ落ちる。
   **出ないことを成功と読ませてはいけない**(#204 / #208 で学んだ形で、
   「段がまるごと走らなくても何も出ない」がいちばん危ない)
2. **ちょうど 1 本であること。** 1 周 1 本の決まり。
   2 本あれば、あとから `0` の行を足した形を疑う
3. **総数が 0 であること。** 1 件でも落ちる
4. 落ちるときは**どのファイルで何件欠けたか**と**理由**を出す。
   総数だけでは直せない(#197 の実害は「どのファイルが半分欠けたか」である)

## 読む行(`src/c/load.c` / `src/c/kernel.c` が出す)

    [load] abort <名前>: <理由>              打ち切り 1 件ごと
    [load] file <パス>: aborted <件数>       ファイル 1 本ごと(1 件以上のときだけ)
    [load] aborted top-level forms: <総数>   1 周の終わりに 1 回。**0 件でも出す**

**合否の一行(`N passed, M failed`)には似せていない。**
`tools/check_pass_line.py` の在庫は「合否の一行を書く箇所」を固定しているので、
新しく直列線へ出す行がその形に似ていると在庫の勘定に入ってしまう。

## 隣の検出器との境界

| 検出器 | 見るもの |
|---|---|
| `check_test_count_ratchet.py`(#204) | 段ごとの**走ったアサーションの数** |
| `check_pass_line.py`(#205) | **合否の一行そのもの**(偽造) |
| 本件(#197) | **読み込まれなかった最上位の式の数** |

## 使い方

    python3 tools/check_load_abort_line.py --verify tmp/serial.txt   判定(段の中から呼ぶ)
    python3 tools/check_load_abort_line.py --self-test               計器自身の対照(規則 8)
"""

import argparse
import os
import re
import sys
import tempfile

# 総数の 1 行。**行まるごと**で見る(前後に何か付いた行は当たらない)。
# 直列線は 1 行ずつ素で出るので、前置きの時刻などは付かない
TOTAL_LINE = re.compile(r"^\[load\] aborted top-level forms: (\d+)$")
# どのファイルで何件か / 理由。落ちたときに出すためだけに読む
FILE_LINE = re.compile(r"^\[load\] file (.+): aborted (\d+)$")
ABORT_LINE = re.compile(r"^\[load\] abort (.+?): (.*)$")

HOWTO = (
    "    この行は src/c/kernel.c が 1 周の終わりに必ず出す"
    "(os_load_report_aborts。**0 件でも出す**)。\n"
    "    理由と直し方: documents/load-abort-visibility.md"
)


def verify(path, out=sys.stdout):
    """直列線のログを判定する。合格なら 0、不合格なら 1 を返す。"""
    if not os.path.exists(path):
        print("NG: 直列線のログ %s が無い。**打ち切りの行を誰も見ていない状態である**"
              % path, file=out)
        print(HOWTO, file=out)
        return 1

    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")

    totals = []    # (行番号, 総数)
    files = []     # (行番号, パス, 件数)
    reasons = []   # (行番号, 名前, 理由)
    for i, raw in enumerate(lines, start=1):
        line = raw.replace("\r", "").rstrip()
        m = TOTAL_LINE.match(line)
        if m:
            totals.append((i, int(m.group(1))))
            continue
        m = FILE_LINE.match(line)
        if m:
            files.append((i, m.group(1), int(m.group(2))))
            continue
        m = ABORT_LINE.match(line)
        if m:
            reasons.append((i, m.group(1), m.group(2)))

    def detail():
        for lineno, p, n in files[:10]:
            print("      行 %d: %s が %d 件欠けた" % (lineno, p, n), file=out)
        for lineno, name, why in reasons[:10]:
            print("      行 %d: %s: %s" % (lineno, name, why), file=out)
        if not files and not reasons:
            print("      (どのファイル・どの理由かの行が無い。"
                  "**総数だけでは直せない**ので、出ていないこと自体が異常である)",
                  file=out)

    if not totals:
        print("NG: 打ち切り件数の行が 1 行も無い(%s)。"
              "**出ないことを成功と読んではいけない**" % path, file=out)
        print("    ゲストが 1 周の終わりまで到達していないか、"
              "直列線が母艦のファイルへ落ちていない可能性がある。", file=out)
        print(HOWTO, file=out)
        return 1

    if len(totals) > 1:
        print("NG: 打ち切り件数の行が %d 行ある(行 %s)。1 周 1 本の決まりである"
              % (len(totals), ", ".join(str(t[0]) for t in totals)), file=out)
        print("    **あとから 0 の行を足した形**を疑う。"
              "本物は 1 周の終わりに 1 回だけ出る。", file=out)
        detail()
        return 1

    lineno, total = totals[0]
    if total != 0:
        print("NG: 読み込みで打ち切られた最上位の式が %d 件ある(行 %d)。"
              "**ファイルが半分だけ静かに欠けている**" % (total, lineno), file=out)
        detail()
        print("    load は打ち切りがあっても成功を返す契約である"
              "(src/c/load.h)。**失敗にするのではなく、見えるようにしてある。**", file=out)
        print("    欠けたのが意図どおりなら、台本からその式を外すこと"
              "(件数の基準を上げる口は置いていない。規則 16)。", file=out)
        return 1

    print("OK: 読み込みで打ち切られた最上位の式は 0 件(行 %d)" % lineno, file=out)
    return 0


# ===========================================================================
# [規則 8] 計器自身の陽性対照・陰性対照。
#
# **対照に使うログはその場で合成する。** 本物の直列線のログは測った機械にしか
# 無いので、実物に依存する試験は CI で動かない(#204 の自己試験と同じ作法)。
#
# **陽性対照を 4 本置いてある。** 1 本目(打ち切りがある)だけでは
# 「行が出ない」形を見ていないことになり、それがいちばん危ない形である。
# ===========================================================================

NEG = "[load] aborted top-level forms: 0\n"

CASES = (
    # (説明, ログの中身 or None(= ファイルを作らない), 期待する終了コード)
    ("陰性対照・通常の 1 周(0 件)",
     "booting\n" + NEG, 0),
    ("陽性対照1・打ち切りがある(台本が半分欠けた)",
     "[load] abort test/lisp/x.lisp: <unbound-variable> nope\n"
     "[load] file test/lisp/x.lisp: aborted 1\n"
     "[load] aborted top-level forms: 1\n", 1),
    ("陽性対照2・catch の無い throw(調査 §C-2 の穴)",
     "[load] abort test/lisp/x.lisp: uncaught catch-exit (throw with no matching catch)\n"
     "[load] file test/lisp/x.lisp: aborted 1\n"
     "[load] aborted top-level forms: 1\n", 1),
    ("陽性対照3・行が出ない(ログはあるが行が無い)",
     "booting\nno line here\n", 1),
    ("陽性対照4・ログそのものが無い",
     None, 1),
    ("陽性対照5・あとから 0 の行を足した(2 本になる)",
     "[load] aborted top-level forms: 3\n" + NEG, 1),
    ("陰性対照・似た行に引っかからない(行まるごとで見る)",
     "note: [load] aborted top-level forms: 7 <- これは引用である\n" + NEG, 0),
)


def self_test():
    ok = True
    tmpdir = tempfile.mkdtemp(prefix="load-abort-selftest-")
    print("[self-test] 打ち切り件数の行の判定(#197)")
    for desc, body, want in CASES:
        path = os.path.join(tmpdir, "serial.txt")
        if os.path.exists(path):
            os.remove(path)
        if body is not None:
            with open(path, "w", encoding="utf-8") as f:
                f.write(body)
        buf = io_capture()
        rc = verify(path, out=buf)
        mark = "OK" if rc == want else "FAIL"
        if rc != want:
            ok = False
        print("  %s(%s): rc=%d(期待 %d)" % (mark, desc, rc, want))
        for line in buf.getvalue().rstrip("\n").split("\n"):
            if line:
                print("      | %s" % line)
    if ok:
        print("self-test 成功: **行が無いときにも落ちる**ことまで確かめた"
              "(陽性対照 5 本 / 陰性対照 2 本)")
        return 0
    print("self-test 失敗", file=sys.stderr)
    return 1


def io_capture():
    import io
    return io.StringIO()


def main(argv):
    ap = argparse.ArgumentParser(description="読み込みの打ち切りが母艦に残っているかを見る(#197)")
    ap.add_argument("--verify", metavar="LOG", help="直列線のログを判定する")
    ap.add_argument("--self-test", action="store_true", help="計器自身の対照(規則 8)")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.verify:
        return verify(args.verify)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
