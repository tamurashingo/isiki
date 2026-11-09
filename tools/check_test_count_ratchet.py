#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""段ごとの試験件数の**名簿**を持ち、実機 1 周のログと突き合わせる(#204 / T3)。

## なぜ必要か

`test-qemu-load-gc-stress` は **19 件のうち 3 件しか走っていないのに
「0 failed」**と報告していた(#197 / #199 / #200)。穴は 2 つある:

- **#200**: 段が静かにアサーションを失っても、残った分が通れば `0 failed` になる
- **#208**: 段が**まるごと走らなくても**何も出ない(合否の一行が出ないだけ)

件数はゲスト側で作られて成果物に入っているのに、**母艦側の消費者が 0 人**だった。
照合はゲスト側の「0 件より多いか」1 つだけで、「本来の件数」を保守している場所が
どこにも無かった(`documents/test-count-census.md` §7-3)。

この道具が**その 1 つ**になる。段ごとの件数を名簿として固定し、ログと突き合わせる。
**名簿にある段がログに無ければ落ちる**ので、#200 と #208 を同じ検査で閉じられる。

## 作法(`tools/check_eval_error_budget.py` にそろえる)

- **基準値は下の `COUNTS` 1 箇所だけ**に書く
- **両方向で落ちる。** 減った = 静かに縮んだ(本命)。増えた = 名簿も上げろ
  (黙って通すと計器が形骸化する。規則 16)
- どちらの場合も**次に書くべき数字をメッセージに出す**
- `--self-test` で**計器自身の陽性対照・陰性対照**を持つ(規則 8)。
  対照に使うログは**その場で合成する**。本物のログは測った機械にしか無いので、
  実物に依存する試験は CI で動かない

## 段の「名乗り」と、件数の「読み取り方」

**段ごとに件数の読み方が違いうる。** これは設計の中心である。
**`isiki tests:` だけを探す検査は C 側の起動スモークの段を黙って飛ばす。**
それは今直している穴とまったく同じ族なので、
**読み取り方を名簿の列にして、知らない読み方は失敗させる**形にしてある。
いま **3 種類**ある:

| 読み取り方 | 何を読むか | 出どころ |
|---|---|---|
| `isiki` | `==== isiki tests: N passed, M failed ====` | ゲストの試験枠(`test/lisp/test_framework.lisp`) |
| `self-tally` | `#ck` の**行数**と `[NG]` の行数(合否の一行は突き合わせに使う) | 台本の自前集計(`test/lisp/qemu_boot_no_init_signal.lisp` の `ck`) |
| `no9p`  | `[no9p] ok` / `[no9p] NG` の**行数**(集計行は突き合わせに使う) | C 側(`src/c/kernel.c` の `run_boot_smoke`) |

**`self-tally` は `isiki` と文字列の形が同じ**(台本が同じ書式で合否の一行を組む)。
だから「合否の一行を探す」だけでは**どちらで読んだのか分からない**。
これは危ない: 枠組みを使わない段が**枠組みの形の一行だけを書いて**合格を名乗れる
(`docs/investigation/detector-t4-passline.logic.md` §9-1「一行を直接書く」)。
そこでこの読み取り方は**1 件 1 行の `#ck` を数えるのを本体**にし、
合否の一行は突き合わせにしか使わない。`#ck` が無いのに合否の一行だけがある
ログは**読めない**として落ちる。

4 種類目が増えたときに**黙って飛ばさない**ために、次の 3 つを入れてある:

1. 名簿の `reader` が `READERS` に無ければ、ログを見る前に落ちる
2. その周で走るはずの段から**1 つも件数が読めなければ**落ちる
   (「読み方が変わった」も「段がまるごと走らなかった」もここで鳴る)
3. **合否の形をした行**(`N passed, M failed`)が、どの段の読み取り方でも
   拾われなかったら落ちる。**新しい形は素通りしない**

段の「名乗り」は 2 通りを受け付ける。どちらもログの実物から導出できる形である:

- `make` の再帰呼び出しの echo(`make test-qemu-milestone MILESTONE=... QEMU_DISK_IMG=...`)。
  `make test-qemu-all` の 1 周はこれで全段が分かる
- `### STAGE <段の名前>` の 1 行。**CI は段ごとに別のコマンド**なので再帰呼び出しの
  echo がログに出ない。そこで `tools/qemu_stage_log.sh` がこの行を頭に出す

## 使い方

    python3 tools/check_test_count_ratchet.py --round all --log tmp/qemu-all.log
    python3 tools/check_test_count_ratchet.py --round ci  --log tmp/qemu-ci.log
    # 1 段だけを見る(段の中から呼ぶ場合。ログ全体が 1 段ぶんだとみなす)
    python3 tools/check_test_count_ratchet.py --stage test-qemu-no-9p-run --log tmp/no9p.serial.txt
    python3 tools/check_test_count_ratchet.py --list        # 名簿を出す(合計は導出)
    python3 tools/check_test_count_ratchet.py --self-test   # 計器自身の対照

**ログは切らずに全部渡すこと。** `tail` で切ると段を見落とす(T1 で実際に起きた)。
"""

import argparse
import os
import re
import sys

# --- 周(どの 1 周で走るか) ------------------------------------------------
ALL = "all"  # make test-qemu-all
CI = "ci"  # .github/workflows/ci.yml の QEMU の段

ROUNDS = (ALL, CI)

# --- 名簿。**ここだけを書き換える。** ---------------------------------------
#
# 初期値の出どころ: `documents/test-count-census.md` の §3-1(24 段)/ §3-2
# (CI 専用 5 段)と付録の生の合否の一行。実機 1 周(2026-11-09、基点 07c4a0b)の
# 実測で、静的な導出と **29/29・ファイル単位 95/95** が一致している。
# **手で数えた数字は 1 つも無い**(規則 13)。
#
# 「どの周で走るか」の出どころ: `Makefile` の test-qemu-all のレシピと
# `.github/workflows/ci.yml`。CI は 12 段走らせ、そのうち 5 段(m1_base /
# m2_za / m3_za_ext5 / m4_environment / fat16_m0)は test-qemu-all に入っていない。
# 残り 7 段は両方で走るので、両方の周に名を連ねる。
#
# 同じ台本を条件を変えて 2 回走らせる段(partition の gpt/mbr、fat16/fat32 の別)は
# **別の段として数える。** 名乗りに使うディスクイメージまで含めて 1 段である。
#
# **件数を直すときは理由を同じ場所に書くこと**(規則 16)。
# 「鳴ったから名簿を上げた」だけの更新は、この計器を形骸化させる。
#
# **件数 0 の段は認めない**(`check_roster` が落とす)。理由は実例がある:
# 数え上げの道具(`tools/census_test_counts.py`)は `qemu_boot_m1_cost.lisp`(自前集計の
# 6 件)を**期待 0 件として扱っていた**(`documents/test-count-census.md` の棚卸しの行)。
# **期待 0 件の段は、縮んでも差が出ない。** 名簿の側で同じ罠を踏まないように、
# 0 件は「正常な状態」ではなく「名簿の不備」として扱う。
#
#        段の名前(ログの中での名乗り)                                    件数  読み方   周
COUNTS = (
    ("test-qemu", 5178, "isiki", (ALL,)),
    # 枠組みを使えない段(条件クラスが 1 つも無い状態を要件とする)。**自前集計**で、
    # 合否の一行の形は枠組みと同じだが出している主体が別である。`#ck` を数える
    ("test/lisp/qemu_boot_no_init_signal.lisp", 15, "self-tally", (ALL,)),
    # C 側の起動スモーク。**合否の一行が出ない唯一の段**で、`[no9p] ok` を数える
    ("test-qemu-no-9p-run", 8, "no9p", (ALL,)),
    ("test/lisp/qemu_boot_load_disk.lisp", 23, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_error_escape_gc.lisp", 378, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_m5_ide.lisp", 5, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img]", 155, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_fat_callback_transfer.lisp [fat16_test.img]", 17, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_load_gc_stress.lisp [fat16_test.img]", 21, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_fat32.lisp [fat32_test.img]", 138, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_fat32_primary_boot.lisp", 11, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_partition.lisp [gpt_multi_test.img]", 38, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_partition.lisp [mbr_multi_test.img]", 38, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_bench_jit.lisp", 58, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_bench_interp.lisp", 29, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_for_semantics.lisp", 29, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_jit_frame.lisp", 241, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_jit_limits.lisp", 68, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_stack_guard.lisp", 26, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_heap_exhaustion.lisp", 42, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_compare_single.lisp", 54, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_compare_fixnum.lisp", 55, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_inline_muldiv.lisp", 46, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_safety3.lisp", 25, "isiki", (ALL,)),
    # --- ここから下は CI だけで走る段(test-qemu-all に入っていない) ---
    ("test/lisp/qemu_boot_m1_base.lisp", 2759, "isiki", (CI,)),
    ("test/lisp/qemu_boot_m2_za.lisp", 678, "isiki", (CI,)),
    ("test/lisp/qemu_boot_m3_za_ext5.lisp", 399, "isiki", (CI,)),
    ("test/lisp/qemu_boot_m4_environment.lisp", 128, "isiki", (CI,)),
    ("test/lisp/qemu_boot_fat16_m0.lisp", 8, "isiki", (CI,)),
)

# --- ログの読み取り --------------------------------------------------------

# 段の名乗り(1): `### STAGE <名前>`。CI と手回しの 1 周がこれを出す。
# T1 の記録(tmp/t1-qemu-ci-*.log)は台本の短い名前(m1_base)で出しているので、
# `/` を含まない短い名前は台本のパスへ展開して受け取る
STAGE_MARK_RE = re.compile(r"^### STAGE (\S.*?)\s*$")
# 段の名乗り(2): make の再帰呼び出しの echo
MAKE_TARGET_RE = re.compile(r"^\s*(?:\$\(MAKE\)|make)\s+(?:\S+=\S+\s+)*(test-qemu\S*)")
MILESTONE_RE = re.compile(r"\bMILESTONE=(\S+)")
DISKIMG_RE = re.compile(r"\bQEMU_DISK_IMG=(\S+)")

# 読み取り方 `isiki`: ゲストの試験枠が出す合否の一行
ISIKI_RE = re.compile(r"==== isiki tests: (\d+) passed, (\d+) failed ====")
# 読み取り方 `self-tally`: 台本の自前集計。1 件 1 行の `#ck`、不合格は `[NG]` も出す。
# **`[NG]` は他の段でも「測定しなかった」等の報せに使われている**ので、
# 段の区間の中だけで数えること(ログ全体を grep してはいけない)
SELF_CK_RE = re.compile(r"^#ck\s+\S")
SELF_NG_RE = re.compile(r"^\[NG\]\s")

# 読み取り方 `no9p`: C 側の起動スモーク。1 件 1 行で出る
NO9P_OK_RE = re.compile(r"\[no9p\] ok\s")
NO9P_NG_RE = re.compile(r"\[no9p\] NG\s")
NO9P_SUM_RE = re.compile(r"\[no9p\] (\d+) passed, (\d+) failed")

# **合否の形をした行**の総当たり。どの読み取り方でも拾えなかったものを
# 「読み取り方が分からない」として落とすために使う(黙って飛ばさないため)
VERDICT_SHAPED_RE = re.compile(r"\d+\s+passed,\s*\d+\s+failed")


def normalize_stage(name):
    """名乗りを名簿の段の名前にそろえる。

    `### STAGE m1_base` のような短い名前(T1 の 1 周が使った形)は
    台本のパスへ展開する。`/` を含むものと `test-qemu...` はそのまま。
    """
    if "/" in name or name.startswith("test-qemu") or name.endswith(".lisp"):
        return name
    return "test/lisp/qemu_boot_%s.lisp" % name


def split_stages(lines):
    """ログを段ごとの区間に切る。

    返り値: [(段の名前, [(行番号, 行), ...]), ...]。先頭の名乗りより前の区間は
    段の名前を None にする(そこに合否の行があれば「名簿に無い段」として鳴る)。

    **名乗りが来たら区間を閉じる。**「包みのターゲット」(test-qemu-no-9p が
    test-qemu-no-9p-run を呼ぶ形)は中身の無い区間になるが、
    判定は名簿が主導なので、名簿に無い名前の空の区間は何も言わない。
    """
    segments = [(None, [])]
    for lineno, line in lines:
        mark = STAGE_MARK_RE.match(line)
        if mark:
            segments.append((normalize_stage(mark.group(1)), []))
            continue
        target = MAKE_TARGET_RE.match(line)
        if target:
            milestone = MILESTONE_RE.search(line)
            if milestone:
                name = milestone.group(1)
                img = DISKIMG_RE.search(line)
                if img:
                    name += " [" + os.path.basename(img.group(1)) + "]"
            else:
                name = target.group(1)
            segments.append((name, []))
            continue
        segments[-1][1].append((lineno, line))
    return segments


def read_isiki(segment):
    """合否の一行を読む。返り値 ([(passed, failed, 行番号)], 拾った行番号の集合)。"""
    readings = []
    consumed = set()
    for lineno, line in segment:
        m = ISIKI_RE.search(line)
        if m:
            readings.append((int(m.group(1)), int(m.group(2)), lineno))
            consumed.add(lineno)
    return readings, consumed


def tally_lines(segment, item_re, ng_re, sum_re, item_is_total, what):
    """**1 件 1 行の報せを数え、段自身の集計行と突き合わせる。**

    行を数えるのが本体で、集計行は突き合わせにしか使わない。
    集計行だけを見ると、**件数が 0 でも合格の一行だけを書けば通る**
    (`docs/investigation/detector-t4-passline.logic.md` §9-1)。
    1 件 1 行なら、行を数えれば「何件走ったか」がそのまま出る。

    item_is_total が真なら item_re は合否を問わず 1 件 1 行、
    偽なら item_re は合格の行だけを数える。

    食い違ったら件数を `None` にして「読めなかった」扱いにする
    —— どちらが正しいか機械には決められないので、人を呼ぶのが正しい。
    """
    items = ng = 0
    consumed = set()
    last = None
    summaries = []
    for lineno, line in segment:
        if item_re.search(line):
            items += 1
            consumed.add(lineno)
            last = lineno
            continue
        if ng_re.search(line):
            ng += 1
            consumed.add(lineno)
            last = lineno
            continue
        m = sum_re.search(line)
        if m:
            summaries.append((int(m.group(1)), int(m.group(2)), lineno))
            consumed.add(lineno)
            last = lineno
    if items == 0 and ng == 0 and not summaries:
        return [], consumed
    passed = (items - ng) if item_is_total else items
    failed = ng
    if passed < 0:
        return (
            [(None, None, last, "%s の行数 %d より不合格 %d のほうが多い" % (what, items, ng))],
            consumed,
        )
    if not summaries:
        return (
            [(None, None, last, "%s は数えられたが、段自身の集計の一行が無い" % what)],
            consumed,
        )
    for s_passed, s_failed, lineno in summaries:
        if (s_passed, s_failed) != (passed, failed):
            return (
                [
                    (
                        None,
                        None,
                        lineno,
                        "%s から数えた %d/%d(合格/不合格)と段自身の集計 %d/%d が食い違う。"
                        "**合否の一行だけを書いて合格を名乗る形**がこれに当たる"
                        % (what, passed, failed, s_passed, s_failed),
                    )
                ],
                consumed,
            )
    return [(passed, failed, last)], consumed


def read_self_tally(segment):
    """台本の自前集計。`#ck` が 1 件 1 行、不合格は `[NG]` も出る。"""
    return tally_lines(segment, SELF_CK_RE, SELF_NG_RE, ISIKI_RE, True, "`#ck`")


def read_no9p(segment):
    """C 側の起動スモーク。`[no9p] ok` / `[no9p] NG` が 1 件 1 行。"""
    return tally_lines(segment, NO9P_OK_RE, NO9P_NG_RE, NO9P_SUM_RE, False, "`[no9p] ok`")


# 読み取り方の登録簿。**名簿がここに無い読み方を名乗ったら落ちる。**
READERS = {
    "isiki": read_isiki,
    "self-tally": read_self_tally,
    "no9p": read_no9p,
}


# --- 判定 ------------------------------------------------------------------


def roster_for(round_name):
    return [e for e in COUNTS if round_name in e[3]]


def check_roster():
    """名簿自身の検査。**知らない読み取り方はログを見る前に落とす。**"""
    problems = []
    seen = set()
    for name, count, reader, rounds in COUNTS:
        if name in seen:
            problems.append("段の名前が重複している: %s" % name)
        seen.add(name)
        if reader not in READERS:
            problems.append(
                "段 %s の読み取り方 %r を知らない。"
                "tools/check_test_count_ratchet.py の READERS に読み方を足すこと"
                "(知らない読み方を黙って飛ばすのが #208 の穴である)" % (name, reader)
            )
        if not rounds or any(r not in ROUNDS for r in rounds):
            problems.append("段 %s の周 %r が不正(使えるのは %s)" % (name, rounds, ROUNDS))
        if count <= 0:
            problems.append(
                "段 %s の件数が %d。**0 件の段は認めない**(縮んでも差が出ないので、"
                "見張っているつもりで何も見ていないことになる)。"
                "本当の件数を入れること" % (name, count)
            )
    return problems


def judge(round_name, lines, single_stage=None):
    """ログを名簿と突き合わせる。返り値 (ok, 表の行, NG の行, 名簿の合計, 実測の合計)。

    single_stage を渡すと**ログ全体を 1 段ぶんとみなす**(段の中から呼ぶ場合。
    名乗りの行が無いログ —— no-9p のシリアル出力 —— を読むためにある)。
    """
    if single_stage is not None:
        entry = next((e for e in COUNTS if e[0] == single_stage), None)
        if entry is None:
            return (
                False,
                [],
                [
                    "NG: 段 %s は名簿に無い。"
                    "tools/check_test_count_ratchet.py の COUNTS に書くこと" % single_stage
                ],
                0,
                0,
            )
        roster = [entry]
        segments = [(single_stage, lines)]
    else:
        roster = roster_for(round_name)
        segments = split_stages(lines)
    expected = {name: (count, reader) for name, count, reader, _ in roster}
    known = {name for name, _, _, _ in COUNTS}
    readings = {}  # 段 -> [(passed, failed, 行番号) ...]
    strays = []  # 名簿に無い段で見つけた合否
    unreadable = []  # どの読み取り方でも拾えなかった合否の形の行
    unexpected_round = []  # 名簿にはあるが、この周では走らないはずの段

    for name, segment in segments:
        if name in expected:
            reader = READERS[expected[name][1]]
            found, consumed = reader(segment)
            readings.setdefault(name, []).extend(found)
        elif name in known:
            # 名簿にはあるが、この周の段ではない。件数が読めたら鳴らす
            found = []
            consumed = set()
            for _, reader in READERS.items():
                f, c = reader(segment)
                found.extend(f)
                consumed |= c
            if found:
                unexpected_round.append((name, found))
        else:
            # 名簿に無い段。**合否が読めたら「足したなら名簿に書け」**
            consumed = set()
            hits = [(lineno, line) for lineno, line in segment if VERDICT_SHAPED_RE.search(line)]
            if hits:
                strays.append((name, hits))
                consumed = {lineno for lineno, _ in hits}
        # **拾えなかった合否の形の行は、読み取り方が分からない行である**
        for lineno, line in segment:
            if lineno in consumed:
                continue
            if VERDICT_SHAPED_RE.search(line):
                unreadable.append((name, lineno, line.rstrip()))

    rows = []
    ng = []
    total_expected = 0
    total_actual = 0
    for name, count, reader, _ in roster:
        total_expected += count
        found = readings.get(name, [])
        if not found:
            rows.append((name, count, None, reader, "NG: 段がログに無い"))
            ng.append(
                "NG: 段 %s が %s の 1 周のログに 1 度も出ていない(期待 %d 件)。"
                "段がまるごと走っていないか、件数の出し方(%s)が変わった。"
                "**件数だけを見る検査はこれを見落とす**(#208)" % (name, round_name, count, reader)
            )
            continue
        if len(found) > 1:
            ng.append(
                "NG: 段 %s が 1 周のログに %d 回出ている(行 %s)。"
                "再試行(flake)なら 1 度しか出ないはずなので、ログの作り方を確かめること"
                % (name, len(found), ", ".join(str(f[2]) for f in found))
            )
        for reading in found:
            if len(reading) == 4:  # 読み取り側が理由つきで降参した
                rows.append((name, count, None, reader, "NG: 読めない"))
                ng.append("NG: 段 %s の件数が読めない(%d 行目): %s" % (name, reading[2], reading[3]))
                continue
            passed, failed, lineno = reading
            actual = passed + failed
            total_actual += actual
            if failed > 0:
                rows.append((name, count, actual, reader, "NG: %d 件失敗" % failed))
                ng.append(
                    "NG: 段 %s で %d 件が失敗している(%d 行目)。"
                    "件数が合っていても失敗は失敗である" % (name, failed, lineno)
                )
            elif actual < count:
                rows.append((name, count, actual, reader, "NG: %d 件減" % (count - actual)))
                ng.append(
                    "NG: 段 %s の件数が %d 件減っている(名簿 %d / 実測 %d、%d 行目)。"
                    "**静かにアサーションを失った可能性がある**(#200)。"
                    "原因を確かめ、意図した減りなら "
                    'tools/check_test_count_ratchet.py の COUNTS の "%s" を %d に直し、'
                    "減った理由を同じ場所に書くこと(規則 16)"
                    % (name, count - actual, count, actual, lineno, name, actual)
                )
            elif actual > count:
                rows.append((name, count, actual, reader, "NG: %d 件増" % (actual - count)))
                ng.append(
                    "NG: 段 %s の件数が %d 件増えている(名簿 %d / 実測 %d、%d 行目)。"
                    "増えたなら名簿も上げること: "
                    'tools/check_test_count_ratchet.py の COUNTS の "%s" を %d に直す。'
                    "黙って通すと計器が形骸化する(規則 16)"
                    % (name, actual - count, count, actual, lineno, name, actual)
                )
            else:
                rows.append((name, count, actual, reader, "OK"))

    for name, hits in strays:
        where = name if name is not None else "(名乗りより前)"
        ng.append(
            "NG: 名簿に無い段 %s が合否を出している(%s)。"
            "段を足したなら tools/check_test_count_ratchet.py の COUNTS に書くこと"
            % (where, ", ".join("%d 行目" % lineno for lineno, _ in hits))
        )

    for name, found in unexpected_round:
        ng.append(
            "NG: 段 %s は %s の 1 周で走る予定が無いのに合否を出している(%d 行目)。"
            "走らせ方を変えたなら COUNTS の周を直すこと"
            % (name, round_name, found[0][2])
        )

    for name, lineno, line in unreadable:
        ng.append(
            "NG: 合否の形をした行を、どの読み取り方でも拾えなかった"
            "(段 %s、%d 行目): %s\n"
            "    **読み取り方が 3 種類目になったということである。**"
            "tools/check_test_count_ratchet.py の READERS に読み方を足し、"
            "COUNTS の読み方の列を直すこと。黙って飛ばすのが #208 の穴である"
            % (name if name is not None else "(名乗りより前)", lineno, line)
        )

    return (not ng), rows, ng, total_expected, total_actual


def read_log(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return [(i, line) for i, line in enumerate(f, start=1)]


def run(round_name, logpath, quiet=False, single_stage=None):
    problems = check_roster()
    if problems:
        for p in problems:
            print("NG(名簿): " + p, file=sys.stderr)
        return False
    ok, rows, ng, total_expected, total_actual = judge(
        round_name, read_log(logpath), single_stage=single_stage
    )
    if not quiet:
        print(
            "[test-count ratchet] %s / ログ %s"
            % ("段 " + single_stage if single_stage else "周 " + round_name, logpath)
        )
        for name, count, actual, reader, verdict in rows:
            print(
                "  %-64s 名簿 %5d  実測 %5s  (%s)  %s"
                % (name, count, "-" if actual is None else actual, reader, verdict)
            )
        print(
            "  名簿 %d 段 合計 %d 件 / 読めた合計 %d 件"
            % (len(rows), total_expected, total_actual)
        )
        sys.stdout.flush()
    for line in ng:
        print("  " + line, file=sys.stderr)
    if ok and not quiet:
        print("  OK")
    return ok


# --- 計器自身の検証(陽性対照・陰性対照。規則 8) ----------------------------
#
# **対照は本物のログに依存させない。** 本物のログ(tmp/t1-qemu-*.log)は
# 測った機械にしか無いので、それを使う試験は CI で動かない。
# 代わりに**名簿から合成したログ**を使う。名簿が本体なので、
# 合成ログでも「名簿の 1 件を欠いたら鳴るか」は本物と同じ形で確かめられる。


def synth_stage_header(name, style):
    """段の名乗りを 1 行作る。style は実物のログに出る 2 通り。"""
    if style == "marker":
        return "### STAGE %s\n" % name
    # make の再帰呼び出しの echo を模す
    if name.endswith(".lisp") or " [" in name:
        script = name.split(" [")[0]
        line = "make test-qemu-milestone MILESTONE=%s" % script
        if " [" in name:
            line += " QEMU_DISK_IMG=tmp/%s" % name.split(" [")[1].rstrip("]")
        return line + "\n"
    return "make %s\n" % name


def synth_body(reader, passed, failed):
    """段の中身(件数の出方)を作る。読み取り方ごとに形が違う。"""
    if reader == "isiki":
        return [
            "#count test/lisp/synthetic_test.lisp %d\n" % passed,
            "\n",
            "==== isiki tests: %d passed, %d failed ====\n" % (passed, failed),  # PASS-LINE: FIXTURE 合成ログ(isiki の読み取り方)
        ]
    if reader == "self-tally":
        # 台本の自前集計。**1 件 1 行の `#ck` が本体**で、不合格は `[NG]` も出す
        out = []
        for i in range(failed):
            out.append("[NG] synthetic-ng-%d\n" % i)
            out.append("#ck synthetic-ng-%d\n" % i)
        out += ["#ck synthetic-%d\n" % i for i in range(passed)]
        out.append("\n")
        out.append("==== isiki tests: %d passed, %d failed ====\n" % (passed, failed))  # PASS-LINE: FIXTURE 合成ログ(self-tally の読み取り方)
        return out
    if reader == "no9p":
        out = ["[no9p] boot smoke begin\n"]
        out += ["[no9p] ok   synthetic-%d\n" % i for i in range(passed)]
        out += ["[no9p] NG   synthetic-ng-%d\n" % i for i in range(failed)]
        out.append("[no9p] %d passed, %d failed\n" % (passed, failed))  # PASS-LINE: FIXTURE 合成ログ(no9p の読み取り方)
        return out
    raise AssertionError("合成できない読み取り方: %r" % reader)


def synth_log(round_name, style="make", drop=None, delta=None, fail=None, extra=None):
    """名簿どおりの 1 周のログを合成する。

    drop  … この段をまるごと落とす(段が走らなかったログ)
    delta … (段, 増減)。件数をずらす
    fail  … (段, 失敗件数)。失敗を混ぜる
    extra … 追加で書く行(名簿に無い段など)
    """
    lines = ["make[1]: Entering directory '/repo'\n"]
    for name, count, reader, rounds in COUNTS:
        if round_name not in rounds or name == drop:
            continue
        passed = count
        failed = 0
        if delta and delta[0] == name:
            passed += delta[1]
        if fail and fail[0] == name:
            failed = fail[1]
            passed -= fail[1]
        lines.append(synth_stage_header(name, style))
        lines.extend(synth_body(reader, passed, failed))
    if extra:
        lines.extend(extra)
    return [(i, line) for i, line in enumerate(lines, start=1)]


def self_test():
    ok = True

    def check(name, got, want):
        nonlocal ok
        if got != want:
            ok = False
        print("  [%s] %s: %s (期待 %s)" % ("OK" if got == want else "NG", name, got, want))

    def passes(lines, round_name=ALL):
        good, _rows, ng, _te, _ta = judge(round_name, lines)
        return good

    def ng_text(lines, round_name=ALL):
        _good, _rows, ng, _te, _ta = judge(round_name, lines)
        return "\n".join(ng)

    print("[self-test] 件数の名簿の照合器の対照(規則 8)")

    # 名簿自身が筋を通しているか(知らない読み取り方が無いこと)
    check("名簿の検査が通る", check_roster(), [])

    # 陰性対照: 名簿どおりの合成ログは通る。**名乗りの 2 通りの両方で**
    for style in ("make", "marker"):
        check("陰性 all(名乗り %s)" % style, passes(synth_log(ALL, style)), True)
        check("陰性 ci(名乗り %s)" % style, passes(synth_log(CI, style), CI), True)

    # 陽性対照 1: 件数を 1 つ減らすと落ちる(#200 の族)
    victim = "test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img]"
    check("陽性 件数 -1 で落ちる", passes(synth_log(ALL, delta=(victim, -1))), False)
    check(
        "陽性 件数 -1 のとき次に書く数字(154)を出す",
        "COUNTS の \"%s\" を 154 に" % victim in ng_text(synth_log(ALL, delta=(victim, -1))),
        True,
    )
    check("陽性 件数 +1 で落ちる", passes(synth_log(ALL, delta=(victim, 1))), False)
    check(
        "陽性 件数 +1 のとき次に書く数字(156)を出す",
        "COUNTS の \"%s\" を 156 に" % victim in ng_text(synth_log(ALL, delta=(victim, 1))),
        True,
    )

    # 陽性対照 1b: **名簿の側を 1 つ動かしても落ちる。**
    # 上の対照はログを動かしたが、完了条件は「名簿の件数を 1 つ減らすと失敗する」なので
    # **名簿を動かす向きでも見せる**(どちらの向きのずれも落ちることの対照)
    good_log = synth_log(ALL)
    global COUNTS
    saved_counts = COUNTS
    try:
        for shift, label in ((-1, "名簿を 1 減らす"), (1, "名簿を 1 増やす")):
            COUNTS = tuple(
                (n, c + shift if n == victim else c, r, ro) for n, c, r, ro in saved_counts
            )
            check("陽性 %s と落ちる" % label, passes(good_log), False)
        COUNTS = saved_counts
        check("陰性 名簿を戻すと通る", passes(good_log), True)
    finally:
        COUNTS = saved_counts

    # 陽性対照 2: 段を 1 本まるごと抜くと落ちる(#208 の族。**ここが本命**)
    for dropped in (victim, "test-qemu-no-9p-run", "test/lisp/qemu_boot_safety3.lisp"):
        check(
            "陽性 段 %s を抜いて落ちる" % dropped,
            passes(synth_log(ALL, drop=dropped)),
            False,
        )
    check(
        "陽性 段を抜いたとき「ログに無い」と言う",
        "が all の 1 周のログに 1 度も出ていない" in ng_text(synth_log(ALL, drop=victim)),
        True,
    )

    # 陽性対照 3: ログにあるが名簿に無い段
    stray = [
        "make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_brand_new.lisp\n",
        "==== isiki tests: 3 passed, 0 failed ====\n",  # PASS-LINE: FIXTURE 名簿に無い段の陽性対照の資料
    ]
    check("陽性 名簿に無い段で落ちる", passes(synth_log(ALL, extra=stray)), False)
    check(
        "陽性 名簿に無い段をそう言う",
        "名簿に無い段" in ng_text(synth_log(ALL, extra=stray)),
        True,
    )

    # 陽性対照 4: failed > 0
    check("陽性 失敗が混ざると落ちる", passes(synth_log(ALL, fail=(victim, 2))), False)

    # 陽性対照 5: **読み取り方が 3 種類目の段**。黙って飛ばさないこと
    third_kind = [
        "make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_third_kind.lisp\n",
        "<<< third-kind tests >>> 7 passed, 0 failed\n",  # PASS-LINE: FIXTURE 知らない合否の形の陽性対照の資料
    ]
    check("陽性 3 種類目の形で落ちる", passes(synth_log(ALL, extra=third_kind)), False)
    # 名簿に無い段としても鳴るが、**名簿に書いた段で形が変わった場合**が本番である
    moved = synth_log(ALL, drop=victim)
    moved_lines = [line for _, line in moved]
    moved_lines.append(synth_stage_header(victim, "marker"))
    moved_lines.append("<<< third-kind tests >>> 155 passed, 0 failed\n")  # PASS-LINE: FIXTURE 段が入れ替わった陽性対照の資料
    moved = [(i, line) for i, line in enumerate(moved_lines, start=1)]
    check("陽性 名簿の段が別の形で出すと落ちる", passes(moved), False)
    check(
        "陽性 その理由を「読み取り方が 3 種類目」と言う",
        "読み取り方が 3 種類目" in ng_text(moved),
        True,
    )

    # 陽性対照 6: 名簿が知らない読み取り方を名乗ったら、ログを見る前に落ちる
    saved = COUNTS
    try:
        COUNTS = COUNTS + (("test/lisp/qemu_boot_unknown_reader.lisp", 1, "telepathy", (ALL,)),)
        problems = check_roster()
        check("陽性 知らない読み取り方で落ちる", bool(problems), True)
        check(
            "陽性 その理由を言う",
            any("READERS に読み方を足す" in p for p in problems),
            True,
        )
    finally:
        COUNTS = saved

    # 陽性対照 7: **自前集計の段が「合否の一行だけ」を書いたログ。**
    # 枠組みの形の一行は書けるので、`isiki tests:` を探すだけの検査は通してしまう
    # (docs/investigation/detector-t4-passline.logic.md §9-1「一行を直接書く」)。
    # **1 件 1 行の `#ck` を数える読み方だと、これは読めないので落ちる**
    self_tally_stage = "test/lisp/qemu_boot_no_init_signal.lisp"
    forged_lines = [line for _, line in synth_log(ALL, drop=self_tally_stage)]
    forged_lines.append(synth_stage_header(self_tally_stage, "marker"))
    forged_lines.append("==== isiki tests: 15 passed, 0 failed ====\n")  # PASS-LINE: FIXTURE 自前集計の段が一行だけを書く陽性対照の資料
    forged = [(i, line) for i, line in enumerate(forged_lines, start=1)]
    check("陽性 自前集計の段が一行だけを書くと落ちる", passes(forged), False)
    check(
        "陽性 その理由を言う(合否の一行だけを書いた形)",
        "**合否の一行だけを書いて合格を名乗る形**がこれに当たる" in ng_text(forged),
        True,
    )
    check(
        "陽性 自前集計の段の件数 -1 で落ちる",
        passes(synth_log(ALL, delta=(self_tally_stage, -1))),
        False,
    )

    # 陽性対照 8: `[no9p] ok` の行数と段自身の集計が食い違うログ
    broken_no9p = synth_log(ALL, drop="test-qemu-no-9p-run")
    broken_lines = [line for _, line in broken_no9p]
    broken_lines.append(synth_stage_header("test-qemu-no-9p-run", "marker"))
    broken_lines.extend(
        ["[no9p] ok   a\n", "[no9p] ok   b\n", "[no9p] 8 passed, 0 failed\n"]  # PASS-LINE: FIXTURE no9p の読み取り方の陽性対照の資料
    )
    broken = [(i, line) for i, line in enumerate(broken_lines, start=1)]
    check("陽性 no9p の行数と集計の食い違いで落ちる", passes(broken), False)

    # **陰性対照の念押し**: 包みのターゲットの echo(中身が無い段)で鳴らないこと。
    # make test-qemu-all は test-qemu-no-9p -> test-qemu-no-9p-run のように
    # 包みのターゲットを echo するので、ここで鳴ると本番で誤報になる
    wrapped = synth_log(ALL)
    wrapped_lines = []
    for _, line in wrapped:
        if line.startswith("make test-qemu-no-9p-run"):
            wrapped_lines.append("make test-qemu-no-9p\n")
            wrapped_lines.append("make BOOT_SMOKE=1 test-qemu-no-9p-run\n")
            continue
        wrapped_lines.append(line)
    wrapped = [(i, line) for i, line in enumerate(wrapped_lines, start=1)]
    check("陰性 包みのターゲットの echo で鳴らない", passes(wrapped), True)

    return ok


def main(argv):
    parser = argparse.ArgumentParser(description="段ごとの試験件数を名簿と突き合わせる")
    parser.add_argument("--round", choices=ROUNDS, help="どの 1 周のログか")
    parser.add_argument(
        "--stage",
        help="**ログ全体を 1 段ぶんとみなして**その段だけを見る(名乗りの行が無いログ用)",
    )
    parser.add_argument("--log", help="1 周の出力を切らずに落としたログ")
    parser.add_argument("--list", action="store_true", help="名簿を出す(合計は導出)")
    parser.add_argument("--self-test", action="store_true", help="計器自身の対照")
    args = parser.parse_args(argv)

    if args.self_test:
        return 0 if self_test() else 1

    if args.list:
        problems = check_roster()
        for p in problems:
            print("NG(名簿): " + p, file=sys.stderr)
        for round_name in ROUNDS:
            roster = roster_for(round_name)
            print(
                "[%s] %d 段 合計 %d 件"
                % (round_name, len(roster), sum(c for _, c, _, _ in roster))
            )
            for name, count, reader, _ in roster:
                print("  %-64s %5d  (%s)" % (name, count, reader))
        print("名簿 全体 %d 段 合計 %d 件" % (len(COUNTS), sum(c for _, c, _, _ in COUNTS)))
        return 1 if problems else 0

    if args.round and args.stage:
        parser.error("--round と --stage は同時に使えない(1 周か 1 段のどちらかである)")
    if not args.log or not (args.round or args.stage):
        parser.error("--log と、--round か --stage のどちらかが要る(--list / --self-test を除く)")
    if not os.path.exists(args.log):
        print(
            "NG: ログ %s が無い。出力を**切らずに**落としてから呼ぶこと" % args.log,
            file=sys.stderr,
        )
        return 1
    ok = run(args.round or "(段単体)", args.log, single_stage=args.stage)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
