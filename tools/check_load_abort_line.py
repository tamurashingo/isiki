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

## 「0 件なら合格」ではない —— 名簿で見る

**最初は「総数が 0 でなければ落ちる」で書いた。通常の 1 周で鳴った。**
実測すると `make test-qemu` の 1 周には打ち切りが **7 件**ある。
中身を見ると 7 件すべてが**打ち切りの仕組み自身の回帰試験**で、
わざと最上位で打ち切っている(`documents/load-abort-visibility.md` §3-3)。

だから判定は**名簿**にした(`tools/check_eval_error_budget.py` の予算、
`tools/check_test_count_ratchet.py` の件数の名簿と同じ作法):

1. **行があること。** 無ければ落ちる。
   **出ないことを成功と読ませてはいけない**(#204 / #208 で学んだ形で、
   「段がまるごと走らなくても何も出ない」がいちばん危ない)
2. **ちょうど 1 本であること。** 1 周 1 本の決まり。
   2 本あれば、あとから `0` の行を足した形を疑う
3. **名簿に無いファイルで打ち切りがあれば落ちる。** これが本命である
   (「ファイルが半分だけ静かに欠ける」はここで鳴る)
4. **名簿にあるファイルでも件数が違えば落ちる。両方向で。**
   増えた = 新しく欠けた。減った = わざとの打ち切りが効かなくなった
   (黙って通すと計器が形骸化する。規則 16)。**次に書く数字を出す**
5. **総数と、ファイルごとの件数の合計が一致すること。**
   ファイルの行が 1 本消えた形と、総数だけを書き換えた形がここで鳴る
6. 落ちるときは**どのファイルで何件欠けたか**と**理由**を出す。
   総数だけでは直せない(#197 の実害は「どのファイルが半分欠けたか」である)

### 名簿に無いファイルが**出てこない**ことは見ていない(境界)

段ごとに読むファイルが違うので、**名簿にある段が 1 つも出てこない周**は
正常にありうる(`make test-qemu-no-9p` は `toplevel_abort_test.lisp` を読まない)。
したがって「わざとの打ち切りがある試験ファイルが**まるごと走らなかった**」は
この道具では鳴らない。**それは `tools/check_test_count_ratchet.py`(#204)の
受け持ちである**(段ごとの件数の名簿が、段がまるごと消えたら落ちる)。

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
    python3 tools/check_load_abort_line.py --list tmp/serial.txt     名簿の候補を導出する(規則 13)
    python3 tools/check_load_abort_line.py --roster                  名簿を出す(合計は導出)
    python3 tools/check_load_abort_line.py --self-test               計器自身の対照(規則 8)
"""

import argparse
import os
import re
import sys
import tempfile

# --- 名簿。**ここだけを書き換える。** ---------------------------------------
#
# 導出のしかた(規則 13。手で数えた数字は 1 つも無い):
#     python3 tools/check_load_abort_line.py --list <直列線のログ>
# が (ファイル, 件数) をそのまま出す。足りない / 余っているときは
# 検査のメッセージが**次に書くべき数字**を出す。
#
# **ここに載るのは「わざと最上位で打ち切る試験」だけである。**
# 打ち切りの仕組みそのものの回帰試験がそれで、台本から外すわけにはいかない。
# **これは「打ち切りを許す予算」ではない。** 新しいファイルが 1 件でも打ち切られたら
# 落ちる。わざとなら名簿に足し、**なぜわざとなのかをここに書く。**
#
#        ファイル                               件数  なぜわざと打ち切るのか
EXPECTED = (
    ("test/lisp/toplevel_abort_test.lisp", 3,
     "最上位への中断そのものの回帰試験。with-environment の中 / 素 / "
     "switch-environment のあとの 3 通りで (error ...) を最上位に置いている"),
    ("test/lisp/handler_exit_test.lisp", 1,
     "ハンドラからの脱出の試験。捕まえずに最上位まで上げる 1 件が要る"),
    ("test/lisp/arith_signal_test.lisp", 1,
     "算術のエラー(DIV (1 0))を最上位まで上げる 1 件"),
    ("test/lisp/isiki_test.lisp", 2,
     "%abort-top-level が AOT 化されたあとも伝播することの回帰試験(#27 / M12 Phase7)。"
     "%%funcall-by-name 経由と通常の呼び出しの 2 通り"),
)
# --------------------------------------------------------------------------

ROSTER = {path: n for path, n, _why in EXPECTED}

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

    # --- 名簿との突き合わせ(両方向) ---
    seen = {}
    for _ln, path, n in files:
        seen[path] = seen.get(path, 0) + n

    bad = []
    for path in sorted(seen):
        got = seen[path]
        if path not in ROSTER:
            bad.append("名簿に無いファイルで打ち切りがある: %s が %d 件。"
                       "**このファイルは半分だけ静かに欠けている。**"
                       "わざとなら EXPECTED に `(\"%s\", %d, \"理由\")` を足すこと"
                       % (path, got, path, got))
        elif got != ROSTER[path]:
            direction = "増えた(新しく欠けた)" if got > ROSTER[path] else \
                        "減った(わざとの打ち切りが効かなくなった)"
            bad.append("%s の打ち切りが %s: 名簿 %d 件 -> 実測 %d 件。"
                       "**次に書く数字は %d**(意図した変更なら名簿を直す。規則 16)"
                       % (path, direction, ROSTER[path], got, got))

    # --- 総数とファイルごとの合計の一致 ---
    # ファイルの行が 1 本消えた形(= どのファイルか分からなくなる)と、
    # 総数だけを書き換えた形がここで鳴る
    per_file_sum = sum(seen.values())
    if per_file_sum != total:
        bad.append("総数 %d と、ファイルごとの件数の合計 %d が合わない。"
                   "**どのファイルかの行が落ちているか、総数が書き換わっている**"
                   % (total, per_file_sum))

    # --- 理由の行の本数が総数と合うこと ---
    if len(reasons) != total:
        bad.append("理由の行が %d 本しかない(総数は %d)。"
                   "**理由が出ていない打ち切りがある**" % (len(reasons), total))

    if bad:
        print("NG: 読み込みの打ち切りが名簿と合っていない(%d 件)" % len(bad), file=out)
        for msg in bad:
            print("  - %s" % msg, file=out)
        detail()
        print("    load は打ち切りがあっても成功を返す契約である"
              "(src/c/load.h)。**失敗にするのではなく、見えるようにしてある。**", file=out)
        print("    名簿の導出: python3 tools/check_load_abort_line.py --list <ログ>", file=out)
        print("    理由と直し方: documents/load-abort-visibility.md", file=out)
        return 1

    print("OK: 読み込みの打ち切りは %d 件(行 %d)。"
          "**どれも名簿どおり**(名簿の合計は %d 件 / %d ファイル)"
          % (total, lineno, sum(ROSTER.values()), len(ROSTER)), file=out)
    if total:
        for path in sorted(seen):
            print("    %s: %d 件(わざと: %s)"
                  % (path, seen[path],
                     [w for f, _n, w in EXPECTED if f == path][0]), file=out)
    return 0


def list_candidates(path, out=sys.stdout):
    """ログから名簿の候補を導出して出す(規則 13。手で数えさせない)。"""
    if not os.path.exists(path):
        print("NG: 直列線のログ %s が無い" % path, file=out)
        return 1
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    seen = {}
    total = None
    for raw in lines:
        line = raw.replace("\r", "").rstrip()
        m = FILE_LINE.match(line)
        if m:
            seen[m.group(1)] = seen.get(m.group(1), 0) + int(m.group(2))
            continue
        m = TOTAL_LINE.match(line)
        if m:
            total = int(m.group(1))
    print("--- このログから導出した名簿の候補 ---", file=out)
    for p_ in sorted(seen):
        print('    ("%s", %d, "理由をここに書く"),' % (p_, seen[p_]), file=out)
    print("  ファイル数 %d / 合計 %d 件(総数の行は %s)"
          % (len(seen), sum(seen.values()),
             "無し" if total is None else str(total)), file=out)
    return 0


def print_roster(out=sys.stdout):
    """名簿そのものを出す。**合計は導出する**(規則 13)。"""
    print("--- わざと最上位で打ち切る試験の名簿 ---", file=out)
    for path, n, why in EXPECTED:
        print("    %-42s %d 件  %s" % (path, n, why), file=out)
    print("  ファイル数 %d / 合計 %d 件"
          % (len(EXPECTED), sum(n for _p, n, _w in EXPECTED)), file=out)
    return 0


# ===========================================================================
# [規則 8] 計器自身の陽性対照・陰性対照。
#
# **対照に使うログはその場で合成する。** 本物の直列線のログは測った機械にしか
# 無いので、実物に依存する試験は CI で動かない(#204 の自己試験と同じ作法)。
#
# **「打ち切りがある」だけを陽性対照にしてはいけない。** いちばん危ないのは
# 「行が出ない」形(段がまるごと走らなかったのに何も出ない)なので、それも入れる。
# 名簿は**両方向**で落ちることを見る(減る側を見ないと計器が形骸化する。規則 16)。
# ===========================================================================

NEG = "[load] aborted top-level forms: 0\n"


def _round(entries):
    """名簿どおりの 1 周ぶんのログを合成する。entries は (ファイル, 件数)。"""
    out = []
    total = 0
    for path, n in entries:
        for i in range(n):
            out.append("[load] abort %s: reason %d\n" % (path, i))
        out.append("[load] file %s: aborted %d\n" % (path, n))
        total += n
    out.append("[load] aborted top-level forms: %d\n" % total)
    return "".join(out)


# **本物の 1 周の形**(2026-11-09 に make test-qemu で実測した 7 件。
# documents/load-abort-visibility.md §3-3)。
# ログそのものは測った機械にしか無いので、**形だけを合成する**
REAL_ROUND = tuple((p_, n) for p_, n, _w in EXPECTED)

CASES = (
    # (説明, ログの中身 or None(= ファイルを作らない), 期待する終了コード)
    ("陰性対照・打ち切りが 1 件も無い周(名簿のファイルを読まない段)",
     "booting\n" + NEG, 0),
    ("陰性対照・**通常の 1 周**(わざとの打ち切り 7 件。名簿どおり)",
     "booting\n" + _round(REAL_ROUND), 0),
    ("陽性対照1・名簿に無いファイルが欠けた(本命)",
     _round(REAL_ROUND[:1] + (("test/lisp/newly_broken_test.lisp", 1),)), 1),
    ("陽性対照2・名簿のファイルの件数が**増えた**(新しく欠けた)",
     _round((("test/lisp/toplevel_abort_test.lisp", 4),)), 1),
    ("陽性対照3・名簿のファイルの件数が**減った**(わざとの打ち切りが効かなくなった。規則 16)",
     _round((("test/lisp/toplevel_abort_test.lisp", 2),)), 1),
    ("陽性対照4・catch の無い throw(調査 §C-2 の穴)",
     "[load] abort test/lisp/x.lisp: uncaught catch-exit (throw with no matching catch)\n"
     "[load] file test/lisp/x.lisp: aborted 1\n"
     "[load] aborted top-level forms: 1\n", 1),
    ("陽性対照5・行が出ない(ログはあるが行が無い)",
     "booting\nno line here\n", 1),
    ("陽性対照6・ログそのものが無い",
     None, 1),
    ("陽性対照7・あとから 0 の行を足した(2 本になる)",
     "[load] aborted top-level forms: 3\n" + NEG, 1),
    ("陽性対照8・どのファイルかの行だけが消えた(総数と合わない)",
     "[load] abort test/lisp/toplevel_abort_test.lisp: reason 0\n"
     "[load] aborted top-level forms: 1\n", 1),
    ("陽性対照9・総数だけを 0 に書き換えた",
     "[load] abort test/lisp/toplevel_abort_test.lisp: reason 0\n"
     "[load] file test/lisp/toplevel_abort_test.lisp: aborted 3\n" + NEG, 1),
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
        pos = len([c for c in CASES if c[2] == 1])
        neg = len(CASES) - pos
        print("self-test 成功: **行が無いときにも落ちる**ことと、"
              "**名簿が両方向で落ちる**ことまで確かめた"
              "(陽性対照 %d 本 / 陰性対照 %d 本)" % (pos, neg))
        return 0
    print("self-test 失敗", file=sys.stderr)
    return 1


def io_capture():
    import io
    return io.StringIO()


def main(argv):
    ap = argparse.ArgumentParser(description="読み込みの打ち切りが母艦に残っているかを見る(#197)")
    ap.add_argument("--verify", metavar="LOG", help="直列線のログを判定する")
    ap.add_argument("--list", metavar="LOG", dest="list_log",
                    help="ログから名簿の候補を導出して出す(規則 13)")
    ap.add_argument("--roster", action="store_true", help="名簿を出す(合計は導出)")
    ap.add_argument("--self-test", action="store_true", help="計器自身の対照(規則 8)")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.roster:
        return print_roster()
    if args.list_log:
        return list_candidates(args.list_log)
    if args.verify:
        return verify(args.verify)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
