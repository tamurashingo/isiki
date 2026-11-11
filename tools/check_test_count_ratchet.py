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

## 内訳の特定(#203 / T2)

段の合計だけでは **「どこかが縮んだ」までしか言えない。** 1 周 12 分かかる機械で
場所が分からないのは高くつくので、**ファイル単位(項目単位)の名簿**を同じ道具に足した。
**新しい検出器を作らなかったのは、検出が二重になるのを避けるため**である:
段の合計(`COUNTS`)が検出で、内訳(`FILES`)が特定。両者は等式で結んである。

元は `documents/test-count-census.md` §5-2 の「測ったが『どのファイルが縮んだか』は
言えない段」(12 段)である。

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
    python3 tools/check_test_count_ratchet.py --files       # ファイル単位の名簿(#203)
    python3 tools/check_test_count_ratchet.py --witness     # 証人の出どころ(ログ不要。#203)
    python3 tools/check_test_count_ratchet.py --self-test   # 計器自身の対照

**ログは切らずに全部渡すこと。** `tail` で切ると段を見落とす(T1 で実際に起きた)。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

# --- 周(どの 1 周で走るか) ------------------------------------------------
ALL = "all"  # make test-qemu-all
CI = "ci"  # .github/workflows/ci.yml の QEMU の段

ROUNDS = (ALL, CI)

# 出口の検査(`isiki-file-end-check`)自身のアサーション数。
# **`#count` を書いた「あと」に走る**ので、どの `#count` にも入らず
# **段の合計にだけ入る**(`documents/test-count-census.md` §2-1)。
# 1 本読むごとに 2 件。ここを動かすときは test_framework.lisp のほうが真実源である
END_CHECK_ASSERTS = 2

# 台本の名前で名乗らない段の、台本のパス。`make test-qemu` は台本の名前を
# コマンド行に書かないので、**ゲスト側の既定値**から引く
# (`src/c/kernel.c:91` の `g_qemu_test_boot_script`。census §1-1 と同じ出どころ)。
# 食い違いは下の check_witness_sources が落とす
STAGE_SCRIPTS = {
    "test-qemu": "test/lisp/qemu_boot_test.lisp",
}
KERNEL_DEFAULT_SCRIPT_RE = re.compile(
    r'g_qemu_test_boot_script\[\d+\]\s*=\s*"([^"]+)"'
)

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
#
# **[2026-11-09 / #203] 10 段の件数を上げた。理由をここに書く(規則 16)。**
#
# 内訳が言えなかった 10 段で、試験ファイルの読み込みを素の `load` から
# `isiki-test-load` へ寄せた(証人の経路を 1 本にする決定。下の RESOURCES 参照)。
# `isiki-test-load` は**出口の検査 2 件**を足すので、
#
#     新しい件数 = 前の件数 + 2 × (新たに証人を付けた本数)
#
# で機械的に上がる。**試験が増えたのではない**(アサーションは 1 件も足していない)。
# 1 本あたり +2 の内訳は 1 本あたり 1 回ずつの declaim 漏れの検査と件数 > 0 の検査で、
# どちらも **new ではなく、ほかの 19 段ではすでに走っていたもの**である。
#
# | 段 | 前 | 新たな証人 | 後 |
# |---|---:|---:|---:|
# | m5_ide | 5 | 1 | 7 |
# | m6_fat16 | 155 | 2 | 159 |
# | fat32 | 138 | 1 | 140 |
# | fat32_primary_boot | 11 | 1 | 13 |
# | partition [gpt] | 38 | 2 | 42 |
# | partition [mbr] | 38 | 2 | 42 |
# | m1_base | 2759 | 13 | 2785 |
# | m3_za_ext5 | 399 | 14 | 427 |
# | m4_environment | 128 | 3 | 134 |
# | fat16_m0 | 8 | 1 | 10 |
#
# 合計 +80 件 = 2 × 40 本。**この表は手で足した数字ではない**:
# 下の FILES の内訳と `check_files_roster` の等式が同じ値を導き、
# 食い違えば `make test` が落ちる(規則 13)。
#        段の名前(ログの中での名乗り)                                    件数  読み方   周
COUNTS = (
    #
    # **[2026-11-11 / #199] ここから 5 段の件数が動いた。理由をここに書く(規則 16)。**
    #
    # 「クラスタ数が減る上書き(縮小)」を FAT16/FAT32 の両方へ実装した
    # (src/lisp/fat16.lisp の %fat16-shrink-file / fat32.lisp の %fat32-shrink-file)。
    # それまでは `(t nil)` で**何もせず nil を返していた**ため、
    # open-output-file が中身のある既存ファイルを開けなかった(#199)。
    # 足した確認は
    #   - 塊が減る上書きが成功すること / 大きさが縮むこと
    #   - **解放された塊が再利用されること**(陽性対照。規則 8)
    #   - **二重使用が起きていないこと**(鎖の長さと他ファイルの中身。陽性対照)
    #   - 高い層からの 3 通りの上書き(小さく / 同じ / 大きく)と open-output-file
    #   - 失敗の文面が区別できること(io_signal_test.lisp。#199 は
    #     「解決は成功しているのに cannot resolve file」で原因探しを遠回りさせた)
    # **件数は手で数えていない(規則 13)。出どころは**
    #     python3 tools/census_test_counts.py --detail --static <台本>
    # で、fat16_test.lisp 145→171 / fat32_test.lisp 138→164 /
    # io_signal_test.lisp 33→36(io_signal_test.lisp は 3 段から読まれる)。
    #
    ("test-qemu", 5181, "isiki", (ALL,)),
    # 枠組みを使えない段(条件クラスが 1 つも無い状態を要件とする)。**自前集計**で、
    # 合否の一行の形は枠組みと同じだが出している主体が別である。`#ck` を数える
    ("test/lisp/qemu_boot_no_init_signal.lisp", 15, "self-tally", (ALL,)),
    # C 側の起動スモーク。**合否の一行が出ない唯一の段**で、`[no9p] ok` を数える
    ("test-qemu-no-9p-run", 8, "no9p", (ALL,)),
    ("test/lisp/qemu_boot_load_disk.lisp", 23, "isiki", (ALL,)),
    # [#215] 初期アプリ: ページャ。**新しく足した段**(試験が増えた分で、
    # 既存の段の件数は 1 つも動かしていない)。内訳は下の FILES に 2 本ある
    # [#216] 46 -> 56。ページャを 1 打鍵で動かした分の対照が増えた
    # (pager_test.lisp に §10 を 6 件、pager_app_disk_test.lisp に §5/§6 を 4 件。
    # 内訳 18 + 34 = 52 に、ファイルの出口の検査 2 本 x 2 件 = 4 を足した 56)。
    # **既定値や既存の対照は 1 つも書き換えていない**(規則 16: 期待値を黙って上げない)
    ("test/lisp/qemu_boot_app_pager.lisp", 56, "isiki", (ALL, CI)),
    # [#214] 初期アプリ: inspect(値の構造の探索)。**新しく足した段**で、
    # 既存の段の件数は 1 つも動かしていない。内訳は下の FILES に 2 本ある
    ("test/lisp/qemu_boot_app_inspect.lisp", 123, "isiki", (ALL, CI)),
    # [#216] 端末の土台(カーソル位置指定とキー 1 つ待ち)。**新しく足した段**で、
    # 既存の段の件数は ページャの 46 -> 52 以外は 1 つも動かしていない。
    # 内訳は下の FILES に 1 本ある
    # (内訳 64 + ファイルの出口の検査 2 件 = 66)
    ("test/lisp/qemu_boot_term.lisp", 66, "isiki", (ALL, CI)),
    # [#217] 初期アプリ: ファイラ(一覧・移動・ファイルを開く)。**新しく足した段**で、
    # 既存の段の件数は 1 つも動かしていない。内訳は下の FILES に 2 本ある
    # (内訳 47 + 131 = 178 に、ファイルの出口の検査 2 本 x 2 件 = 4 を足した 182)。
    # 件数は手で数えていない(規則 13)。出どころは
    #     python3 tools/census_test_counts.py --detail --static test/lisp/qemu_boot_app_filer.lisp
    ("test/lisp/qemu_boot_app_filer.lisp", 182, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_error_escape_gc.lisp", 381, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_m5_ide.lisp", 7, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img]", 185, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_fat_callback_transfer.lisp [fat16_test.img]", 17, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_load_gc_stress.lisp [fat16_test.img]", 21, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_fat32.lisp [fat32_test.img]", 166, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_fat32_primary_boot.lisp", 13, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_partition.lisp [gpt_multi_test.img]", 42, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_partition.lisp [mbr_multi_test.img]", 42, "isiki", (ALL, CI)),
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
    #
    # **[2026-11-09 / #208] ここから 6 段を足した。理由をここに書く(規則 16)。**
    #
    # この 6 段は **`Makefile` と CI の全 686 コミットのどの時点にも登録された
    # ことがない。** 文書に手順だけが残っていて、**一度も走っていなかった。**
    # 件数が名簿に入ることで、**段がまるごと消えても落ちる**ようになる(#208 の趣旨)。
    #
    # 件数は手で数えていない(規則 13)。出どころは
    #
    #     python3 tools/census_test_counts.py --detail --static <台本>
    #
    # で、6 / 21 / 6 / 5 / 6 / 5。うち 4 段は**実機の単一段で実測と一致**している
    # (2026-11-09): inline_phase0 = 6 passed / inline_phase0b = 5 passed /
    # declare_bench = 19 passed(素の load のとき。寄せて +2 = 21 ★)/
    # inline_phase3 は下の表のとおり。
    # instcount_gate_check と abi_bench は ★ 単一段を走らせていない。
    #
    # abi_bench と declare_bench は本体を**素の load から isiki-test-load へ寄せた**
    # (#203 の決定)。寄せると出口の検査が 1 本あたり 2 件増えるので
    # **4 -> 6 / 19 -> 21**。試験が増えたのではない。
    ("test/lisp/qemu_boot_instcount_gate_check.lisp", 5, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_abi_bench.lisp", 6, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_inline_phase3.lisp", 6, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_inline_phase0.lisp", 6, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_inline_phase0b.lisp", 5, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_declare_bench.lisp", 21, "isiki", (ALL,)),
    # --- ここから下は CI だけで走る段(test-qemu-all に入っていない) ---
    ("test/lisp/qemu_boot_m1_base.lisp", 2788, "isiki", (CI,)),
    ("test/lisp/qemu_boot_m2_za.lisp", 678, "isiki", (CI,)),
    ("test/lisp/qemu_boot_m3_za_ext5.lisp", 427, "isiki", (CI,)),
    ("test/lisp/qemu_boot_m4_environment.lisp", 134, "isiki", (CI,)),
    ("test/lisp/qemu_boot_fat16_m0.lisp", 10, "isiki", (CI,)),
)

# --- ファイル単位の名簿(#203 / T2)----------------------------------------
#
# **上の COUNTS は「段がいくつ走ったか」までしか言わない。** 1 周 12 分の機械で
# 「どこかが縮んだ」だけ分かって場所が分からないのは高くつく。
# ここは**どのファイル(どの項目)が縮んだか**を言うための名簿である。
#
# 埋めている穴: `documents/test-count-census.md` §5-2
# 「測ったが『どのファイルが縮んだか』は言えない段」。
#
# **検出はここで二重にならない。** 段の合計(COUNTS)は #204 の仕事で、
# こちらは**内訳の特定**に足す。両者は下の等式で結ばれている:
#
#     段の合計 = Σ(ファイルの件数) + 2 × (証人の本数) + 台本自身の件数
#
# 2 は `isiki-file-end-check` が `#count` を書いた**あと**に走らせる 2 件で、
# **どの `#count` にも入らず合計にだけ入る**(census §2-1)。
# `self-tally` / `no9p` の段は枠組みを使わないので 2 は付かない。
#
# **この等式が、証人の無いアサーションを見つける者である。** 素の `load` で
# 読んだファイルのアサーションは `#count` に出ないので、合計だけが増える。
# 等式の残差がそれを指す(§B-2 の「進捗の行と件数の行は 1 対 1」の運用版)。
#
# 証人の形は読み方で違うが、**3 つとも「名前つきで 1 件 1 行」**である。
# だから項目単位の特定が成り立つ:
#
# | 読み方 | 証人の行 | 1 項目 |
# |---|---|---|
# | `isiki` | `#count <ファイル> <件数>` | 試験ファイル 1 本 |
# | `self-tally` | `#ck <名前>` | 検査 1 件 |
# | `no9p` | `[no9p] ok <名前>` / `[no9p] NG <名前>` | 検査 1 件 |
#
# **初期値は手で数えていない**(規則 13)。出どころは
#
#     python3 tools/census_test_counts.py --file-roster
#
# の出力をそのまま貼ったもので、T1 の実測(`tmp/t1-qemu-all-20261109-143437.log`
# / `tmp/t1-qemu-ci-20261109-144633.log`)の `#count` **95 点と 1 件も食い違わない**。
# 証人の無かった段の値は**導出である**(★ 実機未確認のものは §5-2 に印をつけた)。
# ただし導出は段の合計の等式で縛られており、合計は T1 で 29/29 実測と一致している。
#
# 書式: 段 -> (台本自身のトップレベルのアサーション数, ((項目, 件数), ...))
# **順序はログに出る順**(台本の読み込み順)。照合は名前で行うので順序は
# 判定に使わないが、台本との突き合わせ(check_witness_sources)で順序まで見る。
#
# **件数を直すときは理由を同じ場所に書くこと**(規則 16)。
FILES = {
    # test-qemu … 読み方 isiki / 台本自身 1 件 / 証人 59 本 / 導出の合計 5181 件
    "test-qemu": (1, (
        ("test/lisp/test_framework_test.lisp", 29),
        ("test/lisp/init_test.lisp", 496),
        ("test/lisp/toplevel_abort_test.lisp", 17),
        ("test/lisp/handler_exit_test.lisp", 41),
        ("test/lisp/arith_signal_test.lisp", 40),
        ("test/lisp/index_signal_test.lisp", 51),
        ("test/lisp/undefined_signal_test.lisp", 46),
        ("test/lisp/io_signal_test.lisp", 36),
        ("test/lisp/type_unbound_test.lisp", 79),
        ("test/lisp/arity_signal_test.lisp", 51),
        ("test/lisp/isiki_test.lisp", 653),
        ("test/lisp/isiki_test_syntax.lisp", 11),
        ("test/lisp/isiki_test_jit.lisp", 1211),
        ("test/lisp/za_test.lisp", 245),
        ("test/lisp/za_test_ext5.lisp", 69),
        ("test/lisp/za_test_ext7.lisp", 24),
        ("test/lisp/za_test_ext8.lisp", 31),
        ("test/lisp/za_test_ext9.lisp", 22),
        ("test/lisp/za_test_ext10.lisp", 23),
        ("test/lisp/za_test_ext11.lisp", 8),
        ("test/lisp/za_test_ext12.lisp", 10),
        ("test/lisp/za_test_ext13.lisp", 32),
        ("test/lisp/za_test_ext14.lisp", 24),
        ("test/lisp/za_test_ext15.lisp", 25),
        ("test/lisp/za_test_ext16.lisp", 31),
        ("test/lisp/za_test_ext17.lisp", 28),
        ("test/lisp/za_test_ext18.lisp", 50),
        ("test/lisp/za_test_ext19.lisp", 22),
        ("test/lisp/environment_pages_test.lisp", 20),
        ("test/lisp/environment_literal_slots_test.lisp", 80),
        ("test/lisp/environment_utilities_test.lisp", 28),
        ("test/lisp/room_test.lisp", 13),
        ("test/lisp/aot_leaf_gc_test.lisp", 6),
        ("test/lisp/za_code_imm_test.lisp", 23),
        ("test/lisp/za_abi_probe_test.lisp", 2),
        ("test/lisp/fn_cell_cache_test.lisp", 11),
        ("test/lisp/frame_definition_test.lisp", 33),
        ("test/lisp/frame_variable_test.lisp", 18),
        ("test/lisp/frame_cell_gc_test.lisp", 27),
        ("test/lisp/jit_nest_depth_test.lisp", 8),
        ("test/lisp/go_exit_reuse_test.lisp", 10),
        ("test/lisp/jit_code_packing_test.lisp", 19),
        ("test/lisp/declaim_test.lisp", 44),
        ("test/lisp/inline_builtin_test.lisp", 64),
        ("test/lisp/inline_decl_test.lisp", 103),
        ("test/lisp/fixnum_signed_arith_test.lisp", 118),
        ("test/lisp/single_float_test.lisp", 120),
        ("test/lisp/float_contagion_test.lisp", 127),
        ("test/lisp/float_math_contagion_test.lisp", 112),
        ("test/lisp/convert_float_test.lisp", 86),
        ("test/lisp/float_default_test.lisp", 19),
        ("test/lisp/nan_comparison_test.lisp", 100),
        ("test/lisp/divide_direct_call_test.lisp", 34),
        ("test/lisp/declare_typed_add_test.lisp", 35),
        ("test/lisp/declare_typed_sub_test.lisp", 69),
        ("test/lisp/declare_typed_mul_test.lisp", 91),
        ("test/lisp/declare_typed_div_test.lisp", 105),
        ("test/lisp/declare_types_test.lisp", 63),
        ("test/lisp/disassemble_test.lisp", 69),
    )),
    # test/lisp/qemu_boot_no_init_signal.lisp … 読み方 self-tally / 台本自身 0 件 / 証人 15 本 / 導出の合計 15 件
    "test/lisp/qemu_boot_no_init_signal.lisp": (0, (
        ("no-condition-class", 1),
        ("no-simple-error", 1),
        ("builtin-class-ok", 1),
        ("length-returned", 1),
        ("elt-returned", 1),
        ("subseq-returned", 1),
        ("div-returned", 1),
        ("no-depth-overflow", 1),
        ("still-alive", 1),
        ("cons-alive", 1),
        ("gc-alive", 1),
        ("sg-compiled", 1),
        ("sg-shallow", 1),
        ("sg-deep-returns", 1),
        ("sg-alive-after", 1),
    )),
    # test-qemu-no-9p-run … 読み方 no9p / 台本自身 0 件 / 証人 8 本 / 導出の合計 8 件
    "test-qemu-no-9p-run": (0, (
        ("9p-absent", 1),
        ("let-returns-1", 1),
        ("condition-class-registered", 1),
        ("disassemble-defined", 1),
        ("app-disk-named", 1),
        ("app-disk-mounted", 1),
        ("app-disk-load", 1),
        ("app-hello-42", 1),
    )),
    # test/lisp/qemu_boot_load_disk.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 23 件
    "test/lisp/qemu_boot_load_disk.lisp": (0, (
        ("test/lisp/load_disk_test.lisp", 21),
    )),
    # test/lisp/qemu_boot_app_pager.lisp … 読み方 isiki / 台本自身 0 件 / 証人 2 本 / 導出の合計 56 件
    "test/lisp/qemu_boot_app_pager.lisp": (0, (
        ("test/lisp/pager_app_disk_test.lisp", 18),
        ("test/lisp/pager_test.lisp", 34),
    )),
    # test/lisp/qemu_boot_app_inspect.lisp … 読み方 isiki / 台本自身 0 件 / 証人 2 本 / 導出の合計 123 件
    "test/lisp/qemu_boot_app_inspect.lisp": (0, (
        ("test/lisp/inspect_app_disk_test.lisp", 18),
        ("test/lisp/inspect_test.lisp", 101),
    )),
    # test/lisp/qemu_boot_term.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 66 件
    "test/lisp/qemu_boot_term.lisp": (0, (
        ("test/lisp/term_test.lisp", 64),
    )),
    # test/lisp/qemu_boot_app_filer.lisp … 読み方 isiki / 台本自身 0 件 / 証人 2 本 / 導出の合計 182 件
    "test/lisp/qemu_boot_app_filer.lisp": (0, (
        ("test/lisp/filer_app_disk_test.lisp", 47),
        ("test/lisp/filer_test.lisp", 131),
    )),
    # test/lisp/qemu_boot_error_escape_gc.lisp … 読み方 isiki / 台本自身 4 件 / 証人 8 本 / 導出の合計 381 件
    "test/lisp/qemu_boot_error_escape_gc.lisp": (4, (
        ("test/lisp/toplevel_abort_test.lisp", 17),
        ("test/lisp/handler_exit_test.lisp", 41),
        ("test/lisp/arith_signal_test.lisp", 40),
        ("test/lisp/index_signal_test.lisp", 51),
        ("test/lisp/undefined_signal_test.lisp", 46),
        ("test/lisp/io_signal_test.lisp", 36),
        ("test/lisp/type_unbound_test.lisp", 79),
        ("test/lisp/arity_signal_test.lisp", 51),
    )),
    # test/lisp/qemu_boot_m5_ide.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 7 件
    "test/lisp/qemu_boot_m5_ide.lisp": (0, (
        ("test/lisp/ide_test.lisp", 5),
    )),
    # test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img] … 読み方 isiki / 台本自身 0 件 / 証人 2 本 / 導出の合計 185 件
    "test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img]": (0, (
        ("test/lisp/fat16_test.lisp", 171),
        ("test/lisp/device_test.lisp", 10),
    )),
    # test/lisp/qemu_boot_fat_callback_transfer.lisp [fat16_test.img] … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 17 件
    "test/lisp/qemu_boot_fat_callback_transfer.lisp [fat16_test.img]": (0, (
        ("test/lisp/fat_callback_transfer_test.lisp", 15),
    )),
    # test/lisp/qemu_boot_load_gc_stress.lisp [fat16_test.img] … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 21 件
    "test/lisp/qemu_boot_load_gc_stress.lisp [fat16_test.img]": (0, (
        ("test/lisp/load_gc_stress_test.lisp", 19),
    )),
    # test/lisp/qemu_boot_fat32.lisp [fat32_test.img] … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 166 件
    "test/lisp/qemu_boot_fat32.lisp [fat32_test.img]": (0, (
        ("test/lisp/fat32_test.lisp", 164),
    )),
    # test/lisp/qemu_boot_fat32_primary_boot.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 13 件
    "test/lisp/qemu_boot_fat32_primary_boot.lisp": (0, (
        ("test/lisp/fat32_primary_boot_test.lisp", 11),
    )),
    # test/lisp/qemu_boot_partition.lisp [gpt_multi_test.img] … 読み方 isiki / 台本自身 0 件 / 証人 2 本 / 導出の合計 42 件
    "test/lisp/qemu_boot_partition.lisp [gpt_multi_test.img]": (0, (
        ("test/lisp/partition_test.lisp", 19),
        ("test/lisp/file_cmd_test.lisp", 19),
    )),
    # test/lisp/qemu_boot_partition.lisp [mbr_multi_test.img] … 読み方 isiki / 台本自身 0 件 / 証人 2 本 / 導出の合計 42 件
    "test/lisp/qemu_boot_partition.lisp [mbr_multi_test.img]": (0, (
        ("test/lisp/partition_test.lisp", 19),
        ("test/lisp/file_cmd_test.lisp", 19),
    )),
    # test/lisp/qemu_boot_bench_jit.lisp … 読み方 isiki / 台本自身 0 件 / 証人 2 本 / 導出の合計 58 件
    "test/lisp/qemu_boot_bench_jit.lisp": (0, (
        ("test/lisp/bench_jit_test.lisp", 39),
        ("test/lisp/bench_jit_guard_test.lisp", 15),
    )),
    # test/lisp/qemu_boot_bench_interp.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 29 件
    "test/lisp/qemu_boot_bench_interp.lisp": (0, (
        ("test/lisp/bench_interp_test.lisp", 27),
    )),
    # test/lisp/qemu_boot_for_semantics.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 29 件
    "test/lisp/qemu_boot_for_semantics.lisp": (0, (
        ("test/lisp/for_semantics_test.lisp", 27),
    )),
    # test/lisp/qemu_boot_jit_frame.lisp … 読み方 isiki / 台本自身 0 件 / 証人 3 本 / 導出の合計 241 件
    "test/lisp/qemu_boot_jit_frame.lisp": (0, (
        ("test/lisp/jit_frame_test.lisp", 13),
        ("test/lisp/jit_rsp_balance_test.lisp", 86),
        ("test/lisp/jit_mixed_frame_test.lisp", 136),
    )),
    # test/lisp/qemu_boot_jit_limits.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 68 件
    "test/lisp/qemu_boot_jit_limits.lisp": (0, (
        ("test/lisp/jit_limits_test.lisp", 66),
    )),
    # test/lisp/qemu_boot_stack_guard.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 26 件
    "test/lisp/qemu_boot_stack_guard.lisp": (0, (
        ("test/lisp/stack_guard_test.lisp", 24),
    )),
    # test/lisp/qemu_boot_heap_exhaustion.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 42 件
    "test/lisp/qemu_boot_heap_exhaustion.lisp": (0, (
        ("test/lisp/heap_exhaustion_test.lisp", 40),
    )),
    # test/lisp/qemu_boot_compare_single.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 54 件
    "test/lisp/qemu_boot_compare_single.lisp": (0, (
        ("test/lisp/compare_single_test.lisp", 52),
    )),
    # test/lisp/qemu_boot_compare_fixnum.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 55 件
    "test/lisp/qemu_boot_compare_fixnum.lisp": (0, (
        ("test/lisp/compare_fixnum_test.lisp", 53),
    )),
    # test/lisp/qemu_boot_inline_muldiv.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 46 件
    "test/lisp/qemu_boot_inline_muldiv.lisp": (0, (
        ("test/lisp/inline_muldiv_test.lisp", 44),
    )),
    # test/lisp/qemu_boot_safety3.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 25 件
    "test/lisp/qemu_boot_safety3.lisp": (0, (
        ("test/lisp/safety3_test.lisp", 23),
    )),
    # [#208] 足した 6 段。**4 段は項目(= 証人つきで読むファイル)を持たない。**
    # アサーションが全部台本の最上位にあるからで、
    # 「どのファイルが縮んだか」の答えは**台本そのもの 1 つしかない。**
    # 下の check_files_roster はこの形だけ項目が空であることを許す
    # (台本自身の件数が 0 でないことを条件にしてある)
    "test/lisp/qemu_boot_instcount_gate_check.lisp": (5, ()),
    # 計測の仕組み自身の合格判定。**現役の計器の自己対照はここだけ**で、
    # しかもいちばん安い(10 秒)。計器が黙って壊れるのを止める #208 の趣旨に合う
    "test/lisp/qemu_boot_abi_bench.lisp": (0, (
        ("test/lisp/abi_bench.lisp", 4),
    )),
    "test/lisp/qemu_boot_inline_phase3.lisp": (6, ()),
    # **絶対値で固定した生成コード長 3 件はここだけが見ている。**
    # 過去に実際に 181 byte の差で落ちたが、この段が周回の外だったので
    # 全回帰では拾えず、計器を手で回したときに初めて分かった
    # (documents/inline-arith.md:1085-1090)。それが #208 の穴そのものである
    "test/lisp/qemu_boot_inline_phase0.lisp": (6, ()),
    "test/lisp/qemu_boot_inline_phase0b.lisp": (5, ()),
    # **型ごとの呼び先の選び方 4 件のうち 3 件は、常時走る段に同等物が無い**
    # (double 宣言つき `+` が GENERIC を呼び、fixnum/single 特化版を呼ばないこと。
    #  `-` `*` `/` には declare_typed_*_test.lisp に対応行があるが `+` には無い)
    "test/lisp/qemu_boot_declare_bench.lisp": (0, (
        ("test/lisp/declare_types_bench.lisp", 19),
    )),
    # test/lisp/qemu_boot_m1_base.lisp … 読み方 isiki / 台本自身 1 件 / 証人 13 本 / 導出の合計 2788 件
    "test/lisp/qemu_boot_m1_base.lisp": (1, (
        ("test/lisp/test_framework_test.lisp", 29),
        ("test/lisp/init_test.lisp", 496),
        ("test/lisp/toplevel_abort_test.lisp", 17),
        ("test/lisp/handler_exit_test.lisp", 41),
        ("test/lisp/arith_signal_test.lisp", 40),
        ("test/lisp/index_signal_test.lisp", 51),
        ("test/lisp/undefined_signal_test.lisp", 46),
        ("test/lisp/io_signal_test.lisp", 36),
        ("test/lisp/type_unbound_test.lisp", 79),
        ("test/lisp/arity_signal_test.lisp", 51),
        ("test/lisp/isiki_test.lisp", 653),
        ("test/lisp/isiki_test_syntax.lisp", 11),
        ("test/lisp/isiki_test_jit.lisp", 1211),
    )),
    # test/lisp/qemu_boot_m2_za.lisp … 読み方 isiki / 台本自身 0 件 / 証人 11 本 / 導出の合計 678 件
    "test/lisp/qemu_boot_m2_za.lisp": (0, (
        ("test/lisp/za_test.lisp", 245),
        ("test/lisp/fn_cell_cache_test.lisp", 11),
        ("test/lisp/frame_definition_test.lisp", 33),
        ("test/lisp/frame_variable_test.lisp", 18),
        ("test/lisp/frame_cell_gc_test.lisp", 27),
        ("test/lisp/jit_nest_depth_test.lisp", 8),
        ("test/lisp/jit_code_packing_test.lisp", 19),
        ("test/lisp/declaim_test.lisp", 44),
        ("test/lisp/inline_builtin_test.lisp", 64),
        ("test/lisp/fixnum_signed_arith_test.lisp", 118),
        ("test/lisp/disassemble_test.lisp", 69),
    )),
    # test/lisp/qemu_boot_m3_za_ext5.lisp … 読み方 isiki / 台本自身 0 件 / 証人 14 本 / 導出の合計 427 件
    "test/lisp/qemu_boot_m3_za_ext5.lisp": (0, (
        ("test/lisp/za_test_ext5.lisp", 69),
        ("test/lisp/za_test_ext7.lisp", 24),
        ("test/lisp/za_test_ext8.lisp", 31),
        ("test/lisp/za_test_ext9.lisp", 22),
        ("test/lisp/za_test_ext10.lisp", 23),
        ("test/lisp/za_test_ext11.lisp", 8),
        ("test/lisp/za_test_ext12.lisp", 10),
        ("test/lisp/za_test_ext13.lisp", 32),
        ("test/lisp/za_test_ext14.lisp", 24),
        ("test/lisp/za_test_ext15.lisp", 25),
        ("test/lisp/za_test_ext16.lisp", 31),
        ("test/lisp/za_test_ext17.lisp", 28),
        ("test/lisp/za_test_ext18.lisp", 50),
        ("test/lisp/za_test_ext19.lisp", 22),
    )),
    # test/lisp/qemu_boot_m4_environment.lisp … 読み方 isiki / 台本自身 0 件 / 証人 3 本 / 導出の合計 134 件
    "test/lisp/qemu_boot_m4_environment.lisp": (0, (
        ("test/lisp/environment_pages_test.lisp", 20),
        ("test/lisp/environment_literal_slots_test.lisp", 80),
        ("test/lisp/environment_utilities_test.lisp", 28),
    )),
    # test/lisp/qemu_boot_fat16_m0.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 10 件
    "test/lisp/qemu_boot_fat16_m0.lisp": (0, (
        ("test/lisp/bitwise_char_test.lisp", 8),
    )),
}


# 素の `load` で読んでよい `test/lisp/` のファイル。**理由を必ず書く。**
#
# [決定] **証人の経路は `isiki-test-load` 1 本に寄せた。** 試験ファイルを
# 素の `load` で読むのを禁じ、素の `load` は「アサーションを 1 件も持たない
# 資料・下敷き」にだけ許す。理由:
#
# - 経路が 2 本あると、ログを見た機械には**どちらで読まれたのか分からない。**
#   「進捗の行と件数の行が 1 対 1」という不変条件(元調査 §B-2)が使えるのは、
#   読み方が 1 本だからである
# - 素の `load` は**本番の機能**でもある(`src/lisp/*.lisp` を読む)。
#   試験の数え上げの都合をそちらへ持ち込むと、言語機能に試験の事情が混じる
# - 寄せれば「証人が無いこと」が**静的に**言える(下の check_witness_sources)
#
# **この表は権威ではない。** 権威は上の等式のほうで、ここに載せたファイルが
# あとからアサーションを持ち始めたら、合計だけが増えて等式の残差で鳴る。
# つまり**この表に足しても漏れは恒久化しない。**
RESOURCES = {
    "test/lisp/qemu_boot_load_gc_stress.lisp": (
        ("test/lisp/gc_debug_guard.lisp", "GC_DEBUG ビルドかどうかの下敷き。アサーション 0 件"),
    ),
    "test/lisp/qemu_boot_bench_jit.lisp": (
        ("test/lisp/bench_jit_guard.lisp", "ベンチの前提を確かめる下敷き。アサーション 0 件"),
        ("test/lisp/bench_jit_eval_control.lisp", "ベンチの対照の定義。アサーション 0 件"),
    ),
    "test/lisp/qemu_boot_bench_interp.lisp": (
        ("test/lisp/bench_jit_guard.lisp", "同上"),
        ("test/lisp/bench_interp_guard.lisp", "同上"),
        ("test/lisp/bench_interp_control.lisp", "同上"),
    ),
    "test/lisp/qemu_boot_for_semantics.lisp": (
        ("test/lisp/bench_jit_guard.lisp", "同上"),
    ),
}

# 枠組み自身。アサーションは出口の検査の 2 件として合計へ入るので、
# **ここだけは素の `load` でなければならない**(自分自身を証人つきで読めない)
FRAMEWORK = "test/lisp/test_framework.lisp"

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

# [#203 / T2] **項目単位の証人の行。**
#
# `#count <ファイル> <件数>`。**行末まで数字であることを要求する。**
# `test/lisp/disassemble_test.lisp:319` が
#   `#count dis-add    kernel=0 immobilized=0 gc-heap=0`
# という**別の目的の `#count` 行**を出しているので、緩い正規表現だと拾ってしまう。
# 拾っても「名簿に無い証人」として鳴る形にはしてあるが、**誤報は計器を殺す**ので
# 形で切り分ける(規則 8 の陰性対照にこの行を入れてある)。
FILE_COUNT_RE = re.compile(r"^#count (\S+) (\d+)\s*$")
# 進捗の行。`isiki-test-load` が **load の前に**出す(test_framework.lisp の
# `isiki-test-mark`)。**件数の行と 1 対 1 になる**のが元調査 §B-2 の不変条件で、
# `#at` だけがあって `#count` が無いのは「そのファイルの途中で止まった」痕跡である
# (調査 §9-4-7。**これを見る者がいなかった**)
PROGRESS_RE = re.compile(r"^#at (\S+) t=")
# `self-tally` の項目(名前つき)。上の SELF_CK_RE は数えるだけなので別に持つ
SELF_CK_NAME_RE = re.compile(r"^#ck\s+(\S+)")
# `no9p` の項目(名前つき)
NO9P_NAME_RE = re.compile(r"\[no9p\] (?:ok|NG)\s+(\S+)")

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


# --- 項目単位の照合(#203 / T2)---------------------------------------------


def read_file_items(reader, segment):
    """段の区間から**名前つき 1 件 1 行の証人**を取り出す。

    返り値 (項目 {名前: 件数}, 進捗に名乗ったファイル集合, 重複した名前)。
    進捗の集合は `isiki` のときだけ中身がある(ほかの読み方に `#at` は無い)。
    """
    items = {}
    dup = []
    progressed = set()
    if reader == "isiki":
        for _lineno, line in segment:
            m = FILE_COUNT_RE.match(line)
            if m:
                name = m.group(1)
                if name in items:
                    dup.append(name)
                items[name] = int(m.group(2))
                continue
            m = PROGRESS_RE.match(line)
            if m:
                progressed.add(m.group(1))
        return items, progressed, dup
    name_re = SELF_CK_NAME_RE if reader == "self-tally" else NO9P_NAME_RE
    for _lineno, line in segment:
        m = name_re.search(line)
        if m:
            name = m.group(1)
            if name in items:
                dup.append(name)
            items[name] = items.get(name, 0) + 1
    return items, progressed, dup


def judge_files(stage, reader, segment, actual_total):
    """**どの項目が縮んだか**を言う。返り値は NG の行のリスト。

    段の合計(COUNTS)の照合とは別の仕事である。合計が合っていても
    内訳が入れ替わっていたらここで鳴る(census §5-2 がまさにそれを
    「言えない」と書いていた)。
    """
    ng = []
    entry = FILES.get(stage)
    if entry is None:
        return [
            "NG: 段 %s のファイル単位の名簿が無い。"
            "tools/check_test_count_ratchet.py の FILES に書くこと" % stage
        ]
    script_own, expected_items = entry
    found, progressed, dup = read_file_items(reader, segment)
    what = "ファイル" if reader == "isiki" else "項目"

    for name in dup:
        ng.append(
            "NG: 段 %s で%s %s の証人の行が 2 度出ている。"
            "同じファイルを 2 回読んでいないか、ログの作り方を確かめること" % (stage, what, name)
        )

    for name, count in expected_items:
        if name not in found:
            if reader == "isiki" and name in progressed:
                ng.append(
                    "NG: 段 %s の%s %s に件数の行(#count)が無い。"
                    "**進捗の行(#at)は出ている**ので、そのファイルを読み始めてから"
                    "出口へ着く前に止まっている(ゲストが落ちた・ハングした)。期待 %d 件。"
                    "これは合計だけを見ていたら「段が落ちた」までしか言えない形である"
                    % (stage, what, name, count)
                )
            else:
                ng.append(
                    "NG: 段 %s の%s %s の証人の行が 1 行も無い(期待 %d 件)。"
                    "**そのファイル・検査がまるごと走っていない。**"
                    "読み込みを外したなら FILES の名簿からも外し、理由を書くこと(規則 16)"
                    % (stage, what, name, count)
                )
            continue
        actual = found[name]
        if actual == count:
            continue
        ng.append(
            "NG: 段 %s の%s %s の件数が %d 件%s(名簿 %d / 実測 %d)。"
            "%s意図した変更なら FILES の \"%s\" の \"%s\" を %d に直し、"
            "理由を同じ場所に書くこと(規則 16)"
            % (
                stage,
                what,
                name,
                abs(count - actual),
                "減っている" if actual < count else "増えている",
                count,
                actual,
                "**静かにアサーションを失った可能性がある**(#200)。" if actual < count else "",
                stage,
                name,
                actual,
            )
        )

    expected_names = {n for n, _ in expected_items}
    for name in sorted(found):
        if name not in expected_names:
            ng.append(
                "NG: 段 %s に名簿に無い証人の行がある: %s(%d 件)。"
                "試験ファイルを足したなら FILES に書くこと"
                % (stage, name, found[name])
            )
    if reader == "isiki":
        for name in sorted(progressed - set(found) - expected_names):
            ng.append(
                "NG: 段 %s に名簿に無いファイルの進捗の行(#at %s)がある。"
                "件数の行も無いので、読み始めて止まったか名簿が古い" % (stage, name)
            )

    # --- 等式。**証人の無いアサーションを見つける者である** ---
    #
    #   段の合計 = Σ(実測の件数) + 2 × (証人の本数) + 台本自身の件数
    #
    # 2 は出口の検査自身の 2 件(`#count` を書いた**あと**に走るのでどの
    # `#count` にも入らない。census §2-1)。素の `load` で試験ファイルを
    # 読むと、そのアサーションは合計にだけ入って `#count` に出ないので、
    # **残差がそのぶんだけ正になる。**
    if actual_total is not None:
        end_check = END_CHECK_ASSERTS * len(found) if reader == "isiki" else 0
        accounted = sum(found.values()) + end_check + script_own
        if accounted != actual_total:
            ng.append(
                "NG: 段 %s の合計 %d 件に対し、証人で説明がつくのは %d 件だけである"
                "(Σ件数 %d + 出口の検査 %d + 台本自身 %d)。差 %+d。\n"
                "    正なら**証人の無いアサーションが混ざっている**"
                "(試験ファイルを素の load で読んでいないか。"
                "読むなら isiki-test-load を使うこと)。\n"
                "    負なら台本自身の件数が変わったので、"
                "FILES の \"%s\" の 1 つめの数(いま %d)を直すこと"
                % (
                    stage,
                    actual_total,
                    accounted,
                    sum(found.values()),
                    end_check,
                    script_own,
                    actual_total - accounted,
                    stage,
                    script_own,
                )
            )
    return ng


def check_files_roster():
    """ファイル単位の名簿**自身**の検査。ログを見る前に落とす。

    いちばん大事なのは**段の合計との等式**で、これが通っていれば
    内訳の名簿は T1 で実測された合計に縛られていることになる。
    """
    problems = []
    stage_names = [n for n, _, _, _ in COUNTS]
    for stage in stage_names:
        if stage not in FILES:
            problems.append(
                "段 %s がファイル単位の名簿(FILES)に無い。"
                "**内訳が言えない段を増やさないこと**(それが #203 で埋めた穴である)" % stage
            )
    for stage in FILES:
        if stage not in stage_names:
            problems.append("FILES にあるが COUNTS に無い段: %s" % stage)
    for stage, count, reader, _rounds in COUNTS:
        entry = FILES.get(stage)
        if entry is None:
            continue
        script_own, items = entry
        if not items:
            # [#208] **台本自身にしかアサーションが無い段**は項目を持たない。
            # 証人の行(`#count`)を出すのは isiki-test-load だけで、台本の最上位の
            # アサーションはどの `#count` にも入らないからである。
            # この形では「どのファイルが縮んだか」の答えは台本そのもの 1 つなので、
            # 内訳が無くても**特定はできている。**
            # ただし**件数まで 0 なら何も見ていない**ので、そこは落とす
            # (COUNTS の「0 件の段は認めない」と同じ理由)
            if script_own <= 0:
                problems.append(
                    "段 %s は項目が空で、台本自身の件数も %d である。"
                    "**何も見ていない名簿になっている**(縮んでも差が出ない)。"
                    "証人つきで読むファイルを書くか、台本自身の件数を入れること"
                    % (stage, script_own)
                )
            continue
        seen = set()
        for name, n in items:
            if name in seen:
                problems.append("段 %s の項目 %s が重複している" % (stage, name))
            seen.add(name)
            if n <= 0:
                problems.append(
                    "段 %s の項目 %s の件数が %d。**0 件の項目は認めない**"
                    "(縮んでも差が出ない。COUNTS と同じ理由)" % (stage, name, n)
                )
        end_check = END_CHECK_ASSERTS * len(items) if reader == "isiki" else 0
        derived = sum(n for _, n in items) + end_check + script_own
        if derived != count:
            problems.append(
                "段 %s の内訳の和が段の合計と合わない(COUNTS %d / 内訳から導出 %d)。"
                "Σ件数 %d + 出口の検査 %d + 台本自身 %d。"
                "**どちらかが間違っている。**段の合計は T1 で実測されているので、"
                "まず内訳(FILES)のほうを疑うこと"
                % (
                    stage,
                    count,
                    derived,
                    sum(n for _, n in items),
                    end_check,
                    script_own,
                )
            )
    return problems


# 台本の中の読み込み。**どちらで読んでいるかを静的に区別する**
SCRIPT_LOAD_RE = re.compile(r'\((load|isiki-test-load)\s+"([^"]+)"\)')


def script_for_stage(stage):
    """段の名乗りから台本のパスを出す。台本を持たない段(C 側)は None。"""
    base = stage.split(" [")[0]
    return base if base.endswith(".lisp") else STAGE_SCRIPTS.get(base)


def check_witness_sources(root="."):
    """**「証人が無いこと」を静的に落とす。**

    いままで証人の有無は人が数えていた(census §3 の「証人の有無」の列)。
    ここで見るのは 2 つ:

    1. 台本が `isiki-test-load` で読むファイルの**並び**が名簿と一致すること。
       足したのに名簿に書いていなければ落ちる。外したのに名簿に残っていても落ちる
    2. 素の `load` で `test/lisp/` のファイルを読んでいないこと。
       読んでよいのは RESOURCES に理由つきで載せたものと枠組み自身だけ

    **1 周 12 分かからずに落ちる**のがこの検査の値で、`make test` に入っている。
    """
    problems = []
    for stage, _count, reader, _rounds in COUNTS:
        if reader != "isiki":
            continue  # 枠組みを使わない段(C 側・自前集計)に load は無い
        script = script_for_stage(stage)
        if script is None:
            problems.append(
                "段 %s の台本が分からない。STAGE_SCRIPTS に足すこと" % stage
            )
            continue
        path = os.path.join(root, script)
        if not os.path.exists(path):
            problems.append("段 %s の台本 %s が無い" % (stage, script))
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        witnessed = []
        for how, target in SCRIPT_LOAD_RE.findall(text):
            if how == "isiki-test-load":
                witnessed.append(target)
                continue
            if target == FRAMEWORK:
                continue
            if not target.startswith("test/lisp/"):
                continue  # src/lisp の基盤ライブラリ。試験ファイルではない
            allowed = [p for p, _why in RESOURCES.get(script, ())]
            if target not in allowed:
                problems.append(
                    "台本 %s が %s を**素の load で**読んでいる。"
                    "試験ファイルは isiki-test-load で読むこと"
                    "(そうでないと件数の行が出ず、縮んでも**どのファイルか言えない**)。"
                    "アサーションを 1 件も持たない資料なら、"
                    "tools/check_test_count_ratchet.py の RESOURCES に理由つきで載せること"
                    % (script, target)
                )
        expected = [n for n, _ in FILES.get(stage, (0, ()))[1]]
        if witnessed != expected:
            missing = [n for n in expected if n not in witnessed]
            added = [n for n in witnessed if n not in expected]
            detail = []
            if missing:
                detail.append("名簿にあるが台本が読まない: " + ", ".join(missing))
            if added:
                detail.append("台本が読むが名簿に無い: " + ", ".join(added))
            if not detail:
                detail.append("並びが違う(台本の読み込み順に合わせること)")
            problems.append(
                "段 %s の台本 %s と FILES の名簿が食い違う。%s"
                % (stage, script, " / ".join(detail))
            )
    for script, entries in RESOURCES.items():
        for target, why in entries:
            if not why.strip():
                problems.append(
                    "RESOURCES の %s / %s に理由が書かれていない。"
                    "**理由の無い例外は、ただの穴である**" % (script, target)
                )
    return problems



# ===========================================================================
# [#208] **配線の名簿** —— 試験台本が「どこから走るのか」を 1 箇所に固定する
# ===========================================================================
#
# ## なぜ必要か
#
# 棚卸し(`documents/test-count-census.md`)で、**`Makefile` にも継続的検査にも
# どこにも書かれていない試験台本が 6 本(+ その本体 2 本)**見つかった(#208)。
# 文書に手順だけが残っていて、**一度も走っていない。**
# 「落ちない検査は検出器ではない」の別の形で、書いた人は守られていると思っている。
#
# **しかも元の報告は 7 本で、1 本は誤検出だった。**
# `test/lisp/qemu_boot_diag_m12_host.lisp` は `test/c/script_test.c` が読んでいて
# `make test` で毎回走っている。数え上げの道具(`tools/census_test_counts.py` の
# `where_it_runs`)が**起点を `Makefile` と継続的検査の 2 経路しか見ていなかった**のが
# 原因で、**「どこからも走らない」という報告自体が道具の穴によるもの**だった。
#
# だから閉じる穴は 2 つある:
#
#   1. 配線されていない台本が増えるのを止める(本命)
#   2. **起点の種類を取りこぼして誤検出するのを止める**(誤検出も計器を殺す)
#
# 2 は「知らない読み取り方を黙って飛ばすのが穴である」(上の `READERS`)と
# まったく同じ形なので、**同じ形で閉じる: 起点の種類を名簿にし、知らない種類で落ちる。**
#
# ## 作法(この道具のほかの名簿と同じ)
#
# - **名簿は下の `WIRING` 1 箇所だけ**
# - **両方向で落ちる。** 名簿に無い実物が現れたら落ちる / 名簿にあるのに実物が
#   無くなったら落ちる / **名簿の「どこから走る」が導出と食い違ったら落ちる**
#   (「意図して未配線」と書いたのに実は走っている場合も落ちる)
# - 「意図して未配線」には**理由と日付**を必ず書く。空なら落ちる
#   (`RESOURCES` と同じ作法。**理由の無い例外は、ただの穴である**)
# - 次に名簿へ書くべき行をメッセージに出す
# - `--wiring-self-test` で陽性対照・陰性対照を持つ(規則 8)。
#   **対照に使う木はその場で合成する**(本物の木には「未配線」が 1 本も無いので、
#   実物だけでは陽性対照が取れない)
#
# ## 名簿が覆う範囲
#
# `git ls-files test/` の**全部**(台本・本体・母艦側の単体試験の原簿・資料を含む)。
# 「試験ファイルだけ」に絞らないのは、**絞った範囲の外に落ちたものが見えなくなる**のが
# #208 そのものだからである。
#
# ## コメントの中の名前を数えないこと(誤検出の真因その 2)
#
# 調査(`docs/investigation/never-run-stages.logic.md` §9-6)は、同じ探索で
#
#   - **偽陽性**: `test/lisp/abi_bench.lisp` は `src/c/runtime.h` の**コメントに
#     名前が書いてあるだけ**で「辿れた」と出た。実行はされていない
#   - **偽陰性**: `test/c/test_assert.h` は同ディレクトリの
#     `#include "test_assert.h"`(パス接頭辞なし)で読まれているのに「辿れない」と出た
#
# の両方を出している。だから導出は
#
#   (a) **コメントを剥がしてから**名前を探す
#   (b) **同ディレクトリの相対参照を解決する**
#
# の 2 つを必ず行う。どちらも下の自己試験に陽性対照がある。
# いま実際に効いている例: `test/c/load_test.c` のコメントは
# `test/lisp/partition_test.lisp` と `test/c/mount_test.c` の名前を含み、
# `test/c/repl_test.c` のコメントは `test/lisp/toplevel_abort_test.lisp` を含む。
# **剥がさないと「母艦側の単体試験が走らせている」と誤って読める。**

# 起点の種類の名簿。**知らない種類はここで落とす。** 値は (引数が要るか, 説明)
WIRING_KINDS = {
    "all": (False, "make test-qemu-all の 1 周(自動の周回)"),
    "ci": (False, ".github/workflows/ci.yml の段"),
    "manual": (True, "名前付きの手で呼ぶ make のターゲット(引数 = ターゲット名)"),
    "host": (True, "母艦側の単体試験が run_lisp_file で読む(引数 = その C の原簿)"),
    "hostbuild": (True, "母艦側の単体試験の原簿そのもの(引数 = Makefile の変数名)"),
    "loaded": (False, "配線済みの台本が load / isiki-test-load で読む"),
    "included": (False, "配線済みの原簿が #include で読む(同ディレクトリの相対参照を含む)"),
    "fixture": (True, "道具が資料として名指しで読む(引数 = その道具)"),
    "unwired": (True, "**意図して未配線。**引数 = 理由と日付(空なら落ちる)"),
}

# **配線の名簿。ここだけを書き換える。**
#
# 初期値は**手で書いていない**(規則 13)。出どころは
#
#     python3 tools/check_test_count_ratchet.py --wiring-derive
#
# の出力をそのまま貼ったもので、実物の木(`git ls-files test`)190 本と
# 1 行ずつ対応している。**だから名簿そのものに「嘘」は入れられない** ——
# 導出と食い違えば下の check_wiring が落ちる。
#
# **配線を変えたときは理由を同じ場所に書くこと**(規則 16)。
# 「鳴ったから名簿を直した」だけの更新は、この計器を形骸化させる。
#
# [2026-11-09 / #208] **未配線 8 本を 0 本にした。** 内訳と理由:
#   - 6 本の台本(+ 本体 2 本)を `test-qemu-all` へ配線した
#     (`qemu_boot_instcount_gate_check` / `_abi_bench` / `_inline_phase3` /
#      `_inline_phase0` / `_inline_phase0b` / `_declare_bench`)
#   - 当初の設計は 4 本を消す判断だったが、**合否の項目が常時走る段で
#     覆われていなかったので 3 本は消さなかった**
#     (documents/never-run-stages-decisions.md)
#   - `qemu_boot_diag_m12_host.lisp` は**元から走っていた。**
#     数え上げの道具が起点を 2 種類しか見ていなかっただけである(= 誤検出)
#
# [2026-11-11 / #215] **3 本足した**(初期アプリ: ページャ)。
#   - `test/lisp/qemu_boot_app_pager.lisp` …… `make test-qemu-all` と CI の両方へ
#     段として足した(Makefile の test-qemu-all-stages と .github/workflows/ci.yml)
#   - `test/lisp/pager_app_disk_test.lisp` / `test/lisp/pager_test.lisp` ……
#     その段が `isiki-test-load` で読む
# **既存の配線は 1 つも変えていない。**
#
# [2026-11-11 / #214] **3 本足した**(初期アプリ: inspect)。
#   - `test/lisp/qemu_boot_app_inspect.lisp` …… `make test-qemu-all` と CI の両方へ
#     段として足した(Makefile の test-qemu-all-stages と .github/workflows/ci.yml)。
#     **CI にも段を入れたのは、#214 の完了条件が「文字列入力ストリームを渡した
#     試験が CI で走る」だからである**(`test-qemu-all` だけでは足りない)
#   - `test/lisp/inspect_app_disk_test.lisp` / `test/lisp/inspect_test.lisp` ……
#     その段が `isiki-test-load` で読む
# **既存の配線は 1 つも変えていない。**
#
# [2026-11-11 / #216] **3 本足した**(端末の土台: カーソル位置指定とキー 1 つ待ち)。
#   - `test/c/keymap_test.c` …… 母艦の試験として足した
#     (`TEST_SRC_KEYMAP` / `TEST_BIN_KEYMAP`。**キーの取り扱いに付いた最初の試験**で、
#     それまで 0 件だった)
#   - `test/lisp/qemu_boot_term.lisp` …… `make test-qemu-all` と CI の両方へ
#     段として足した(Makefile の test-qemu-all-stages と .github/workflows/ci.yml)
#   - `test/lisp/term_test.lisp` …… その段が `isiki-test-load` で読む
# **既存の配線は 1 つも変えていない。**
#
# [2026-11-11 / #217] **3 本足した**(初期アプリ: ファイラ)。
#   - `test/lisp/qemu_boot_app_filer.lisp` …… `make test-qemu-all` と CI の両方へ
#     段として足した(Makefile の test-qemu-all-stages と .github/workflows/ci.yml)。
#     **CI にも段を入れたのは、#217 の完了条件が「文字列入力ストリームを渡した
#     試験が CI で走る」だからである**(`test-qemu-all` だけでは足りない)
#   - `test/lisp/filer_app_disk_test.lisp` / `test/lisp/filer_test.lisp` ……
#     その段が `isiki-test-load` で読む
# **既存の配線は 1 つも変えていない。**
#
WIRING = (
    ("test/bench/estimator_fixture.tsv", ("fixture:tools/bench/check_estimator.sh",)),
    ("test/c/clock_test.c", ("hostbuild:TEST_SRC_CLOCK",)),
    ("test/c/disasm_test.c", ("hostbuild:TEST_SRC_DISASM",)),
    ("test/c/eval_test.c", ("hostbuild:TEST_SRC_EVAL",)),
    ("test/c/format_test.c", ("hostbuild:TEST_SRC_FORMAT",)),
    ("test/c/framebuffer_test.c", ("hostbuild:TEST_SRC_FRAMEBUFFER",)),
    ("test/c/ide_test.c", ("hostbuild:TEST_SRC_IDE",)),
    ("test/c/keymap_test.c", ("hostbuild:TEST_SRC_KEYMAP",)),
    ("test/c/lisp_compiled_test.c", ("hostbuild:TEST_SRC_LISP_COMPILED",)),
    ("test/c/lisp_test.c", ("hostbuild:TEST_SRC_LISP",)),
    ("test/c/load_test.c", ("hostbuild:TEST_SRC_LOAD",)),
    ("test/c/mount_test.c", ("hostbuild:TEST_SRC_MOUNT",)),
    ("test/c/p9_test.c", ("hostbuild:TEST_SRC_P9",)),
    ("test/c/print_test.c", ("hostbuild:TEST_SRC_PRINT",)),
    ("test/c/process_test.c", ("hostbuild:TEST_SRC_PROCESS",)),
    ("test/c/reader_test.c", ("hostbuild:TEST_SRC_READER",)),
    ("test/c/repl_test.c", ("hostbuild:TEST_SRC_REPL",)),
    ("test/c/runtime_test.c", ("hostbuild:TEST_SRC_RUNTIME",)),
    ("test/c/script_test.c", ("hostbuild:TEST_SRC_SCRIPT",)),
    ("test/c/stream_lisp_test.c", ("hostbuild:TEST_SRC_STREAM_LISP",)),
    ("test/c/stream_test.c", ("hostbuild:TEST_SRC_STREAM",)),
    ("test/c/subprimitive_test.c", ("hostbuild:TEST_SRC_SUBPRIMITIVE",)),
    ("test/c/test_assert.h", ("included",)),
    ("test/c/virtio9p_test.c", ("hostbuild:TEST_SRC_VIRTIO9P",)),
    ("test/fixtures/pr113_body.md", ("fixture:tools/check_pr_claims.py",)),
    ("test/lisp/abi_bench.lisp", ("loaded",)),
    ("test/lisp/alloc_breakdown.lisp", ("loaded",)),
    ("test/lisp/alloc_reason_test.lisp", ("loaded",)),
    ("test/lisp/aot_leaf_gc_test.lisp", ("loaded",)),
    ("test/lisp/arith_signal_test.lisp", ("loaded",)),
    ("test/lisp/arity_signal_test.lisp", ("loaded",)),
    ("test/lisp/audit_control_post.lisp", ("fixture:tools/bench/run_paint_audit.sh",)),
    ("test/lisp/audit_control_pre.lisp", ("fixture:tools/bench/run_paint_audit.sh",)),
    ("test/lisp/bench_construct_test.lisp", ("loaded",)),
    ("test/lisp/bench_interp_control.lisp", ("loaded",)),
    ("test/lisp/bench_interp_guard.lisp", ("loaded",)),
    ("test/lisp/bench_interp_test.lisp", ("loaded",)),
    ("test/lisp/bench_jit_eval_control.lisp", ("loaded",)),
    ("test/lisp/bench_jit_guard.lisp", ("loaded",)),
    ("test/lisp/bench_jit_guard_test.lisp", ("loaded",)),
    ("test/lisp/bench_jit_test.lisp", ("loaded",)),
    ("test/lisp/bitwise_char_test.lisp", ("loaded",)),
    ("test/lisp/compare_fixnum_test.lisp", ("loaded",)),
    ("test/lisp/compare_single_test.lisp", ("loaded",)),
    ("test/lisp/convert_float_test.lisp", ("loaded",)),
    ("test/lisp/declaim_test.lisp", ("loaded",)),
    ("test/lisp/declare_audit_test.lisp", ("loaded",)),
    ("test/lisp/declare_typed_add_test.lisp", ("loaded",)),
    ("test/lisp/declare_typed_div_test.lisp", ("loaded",)),
    ("test/lisp/declare_typed_mul_test.lisp", ("loaded",)),
    ("test/lisp/declare_typed_sub_test.lisp", ("loaded",)),
    ("test/lisp/declare_types_bench.lisp", ("loaded",)),
    ("test/lisp/declare_types_test.lisp", ("loaded",)),
    ("test/lisp/defmacro_test.lisp", ("host:test/c/script_test.c",)),
    ("test/lisp/device_test.lisp", ("loaded",)),
    ("test/lisp/disassemble_test.lisp", ("loaded",)),
    ("test/lisp/divide_direct_call_test.lisp", ("loaded",)),
    ("test/lisp/environment_literal_slots_test.lisp", ("loaded",)),
    ("test/lisp/environment_pages_test.lisp", ("loaded",)),
    ("test/lisp/environment_utilities_test.lisp", ("loaded",)),
    ("test/lisp/environments_predefined_test.lisp", ("host:test/c/script_test.c",)),
    ("test/lisp/fat16_test.lisp", ("loaded",)),
    ("test/lisp/fat32_primary_boot_test.lisp", ("loaded",)),
    ("test/lisp/fat32_test.lisp", ("loaded",)),
    ("test/lisp/fat_callback_transfer_test.lisp", ("loaded",)),
    ("test/lisp/file_cmd_test.lisp", ("loaded",)),
    ("test/lisp/filer_app_disk_test.lisp", ("loaded",)),
    ("test/lisp/filer_test.lisp", ("loaded",)),
    ("test/lisp/fixnum_signed_arith_test.lisp", ("loaded",)),
    ("test/lisp/float_contagion_bench.lisp", ("loaded",)),
    ("test/lisp/float_contagion_test.lisp", ("loaded",)),
    ("test/lisp/float_default_test.lisp", ("loaded",)),
    ("test/lisp/float_math_contagion_test.lisp", ("loaded",)),
    ("test/lisp/fn_cell_cache_test.lisp", ("loaded",)),
    ("test/lisp/for_expansion_test.lisp", ("loaded",)),
    ("test/lisp/for_gc_stress_test.lisp", ("loaded",)),
    ("test/lisp/for_semantics_test.lisp", ("loaded",)),
    ("test/lisp/frame_cell_gc_test.lisp", ("loaded",)),
    ("test/lisp/frame_definition_test.lisp", ("loaded",)),
    ("test/lisp/frame_variable_test.lisp", ("loaded",)),
    ("test/lisp/gc_debug_guard.lisp", ("loaded",)),
    ("test/lisp/go_exit_reuse_test.lisp", ("loaded",)),
    ("test/lisp/handler_exit_test.lisp", ("loaded",)),
    ("test/lisp/heap_exhaustion_test.lisp", ("loaded",)),
    ("test/lisp/ide_test.lisp", ("loaded",)),
    ("test/lisp/imm_leak_test.lisp", ("loaded",)),
    ("test/lisp/index_signal_test.lisp", ("loaded",)),
    ("test/lisp/init_test.lisp", ("host:test/c/script_test.c", "loaded",)),
    ("test/lisp/inline_builtin_test.lisp", ("loaded",)),
    ("test/lisp/inline_decl_test.lisp", ("loaded",)),
    ("test/lisp/inline_muldiv_test.lisp", ("loaded",)),
    ("test/lisp/inspect_app_disk_test.lisp", ("loaded",)),
    ("test/lisp/inspect_test.lisp", ("loaded",)),
    ("test/lisp/io_signal_test.lisp", ("loaded",)),
    ("test/lisp/isiki_test.lisp", ("loaded",)),
    ("test/lisp/isiki_test_jit.lisp", ("loaded",)),
    ("test/lisp/isiki_test_syntax.lisp", ("loaded",)),
    ("test/lisp/jit_code_packing_test.lisp", ("loaded",)),
    ("test/lisp/jit_frame_test.lisp", ("loaded",)),
    ("test/lisp/jit_limits_test.lisp", ("loaded",)),
    ("test/lisp/jit_mixed_frame_test.lisp", ("loaded",)),
    ("test/lisp/jit_nest_depth_test.lisp", ("loaded",)),
    ("test/lisp/jit_rsp_balance_test.lisp", ("loaded",)),
    ("test/lisp/load_disk_test.lisp", ("loaded",)),
    ("test/lisp/load_gc_stress_fixture.lisp", ("loaded",)),
    ("test/lisp/load_gc_stress_test.lisp", ("loaded",)),
    ("test/lisp/nan_comparison_test.lisp", ("loaded",)),
    ("test/lisp/pager_app_disk_test.lisp", ("loaded",)),
    ("test/lisp/pager_test.lisp", ("loaded",)),
    ("test/lisp/partition_test.lisp", ("loaded",)),
    ("test/lisp/perf_fat16_test.lisp", ("loaded",)),
    ("test/lisp/perf_fat32_test.lisp", ("loaded",)),
    ("test/lisp/perf_read_fat16_test.lisp", ("loaded",)),
    ("test/lisp/perf_read_fat32_test.lisp", ("loaded",)),
    ("test/lisp/qemu_boot_abi_bench.lisp", ("all",)),
    ("test/lisp/qemu_boot_alloc_audit.lisp", ("manual:test-qemu-alloc-audit",)),
    ("test/lisp/qemu_boot_alloc_breakdown.lisp", ("manual:test-qemu-alloc-breakdown",)),
    ("test/lisp/qemu_boot_app_filer.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_app_inspect.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_app_pager.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_bench_construct.lisp", ("manual:test-qemu-bench-construct-check",)),
    ("test/lisp/qemu_boot_bench_interp.lisp", ("all",)),
    ("test/lisp/qemu_boot_bench_jit.lisp", ("all",)),
    ("test/lisp/qemu_boot_compare_fixnum.lisp", ("all",)),
    ("test/lisp/qemu_boot_compare_single.lisp", ("all",)),
    ("test/lisp/qemu_boot_declare_audit.lisp", ("manual:test-qemu-declare-audit",)),
    ("test/lisp/qemu_boot_declare_bench.lisp", ("all",)),
    ("test/lisp/qemu_boot_diag_m12_host.lisp", ("host:test/c/script_test.c",)),
    ("test/lisp/qemu_boot_error_escape_gc.lisp", ("all",)),
    ("test/lisp/qemu_boot_fat16_m0.lisp", ("ci",)),
    ("test/lisp/qemu_boot_fat32.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_fat32_primary_boot.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_fat_callback_transfer.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_float_contagion_bench.lisp", ("manual:test-qemu-float-contagion-bench",)),
    ("test/lisp/qemu_boot_for_expansion.lisp", ("manual:test-qemu-for-expansion",)),
    ("test/lisp/qemu_boot_for_gc_stress.lisp", ("manual:test-qemu-for-gc-stress",)),
    ("test/lisp/qemu_boot_for_semantics.lisp", ("all",)),
    ("test/lisp/qemu_boot_heap_exhaustion.lisp", ("all",)),
    ("test/lisp/qemu_boot_imm_leak.lisp", ("manual:test-qemu-imm-leak",)),
    ("test/lisp/qemu_boot_inline_muldiv.lisp", ("all",)),
    ("test/lisp/qemu_boot_inline_phase0.lisp", ("all",)),
    ("test/lisp/qemu_boot_inline_phase0b.lisp", ("all",)),
    ("test/lisp/qemu_boot_inline_phase3.lisp", ("all",)),
    ("test/lisp/qemu_boot_instcount_gate_check.lisp", ("all",)),
    ("test/lisp/qemu_boot_jit_frame.lisp", ("all",)),
    ("test/lisp/qemu_boot_jit_limits.lisp", ("all",)),
    ("test/lisp/qemu_boot_load_abort_positive.lisp", ("manual:test-qemu-load-abort-positive",)),
    ("test/lisp/qemu_boot_load_disk.lisp", ("all",)),
    ("test/lisp/qemu_boot_load_gc_stress.lisp", ("all",)),
    ("test/lisp/qemu_boot_m1_base.lisp", ("ci",)),
    ("test/lisp/qemu_boot_m1_cost.lisp", ("manual:test-qemu-m1-cost",)),
    ("test/lisp/qemu_boot_m2_za.lisp", ("ci",)),
    ("test/lisp/qemu_boot_m2_za_stress.lisp", ("manual:test-qemu-stress",)),
    ("test/lisp/qemu_boot_m3_za_ext5.lisp", ("ci",)),
    ("test/lisp/qemu_boot_m4_environment.lisp", ("ci",)),
    ("test/lisp/qemu_boot_m5_ide.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_m6_fat16.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_no_init_signal.lisp", ("all",)),
    ("test/lisp/qemu_boot_partition.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_perf_fat16.lisp", ("manual:test-qemu-perf",)),
    ("test/lisp/qemu_boot_perf_fat32.lisp", ("manual:test-qemu-perf",)),
    ("test/lisp/qemu_boot_perf_read_fat16.lisp", ("manual:test-qemu-perf-read",)),
    ("test/lisp/qemu_boot_perf_read_fat32.lisp", ("manual:test-qemu-perf-read",)),
    ("test/lisp/qemu_boot_read_file_native.lisp", ("manual:test-qemu-read-file-native",)),
    ("test/lisp/qemu_boot_safety3.lisp", ("all",)),
    ("test/lisp/qemu_boot_stack_guard.lisp", ("all",)),
    ("test/lisp/qemu_boot_stack_guard_gc.lisp", ("manual:test-qemu-stack-guard-gc",)),
    ("test/lisp/qemu_boot_term.lisp", ("all", "ci",)),
    ("test/lisp/qemu_boot_test.lisp", ("all",)),
    ("test/lisp/read_file_native_test.lisp", ("loaded",)),
    ("test/lisp/rest_test.lisp", ("host:test/c/script_test.c",)),
    ("test/lisp/room_test.lisp", ("loaded",)),
    ("test/lisp/safety3_test.lisp", ("loaded",)),
    ("test/lisp/single_float_test.lisp", ("loaded",)),
    ("test/lisp/square_test.lisp", ("host:test/c/script_test.c",)),
    ("test/lisp/stack_guard_gc_test.lisp", ("loaded",)),
    ("test/lisp/stack_guard_test.lisp", ("loaded",)),
    ("test/lisp/term_test.lisp", ("loaded",)),
    ("test/lisp/test_framework.lisp", ("loaded",)),
    ("test/lisp/test_framework_test.lisp", ("loaded",)),
    ("test/lisp/toplevel_abort_test.lisp", ("loaded",)),
    ("test/lisp/transpile_fixture.lisp", ("fixture:tools/let_histogram_report.py",)),
    ("test/lisp/type_unbound_test.lisp", ("loaded",)),
    ("test/lisp/undefined_signal_test.lisp", ("loaded",)),
    ("test/lisp/za_abi_probe_test.lisp", ("loaded",)),
    ("test/lisp/za_code_imm_test.lisp", ("loaded",)),
    ("test/lisp/za_test.lisp", ("loaded",)),
    ("test/lisp/za_test_ext10.lisp", ("loaded",)),
    ("test/lisp/za_test_ext11.lisp", ("loaded",)),
    ("test/lisp/za_test_ext12.lisp", ("loaded",)),
    ("test/lisp/za_test_ext13.lisp", ("loaded",)),
    ("test/lisp/za_test_ext14.lisp", ("loaded",)),
    ("test/lisp/za_test_ext15.lisp", ("loaded",)),
    ("test/lisp/za_test_ext16.lisp", ("loaded",)),
    ("test/lisp/za_test_ext17.lisp", ("loaded",)),
    ("test/lisp/za_test_ext18.lisp", ("loaded",)),
    ("test/lisp/za_test_ext19.lisp", ("loaded",)),
    ("test/lisp/za_test_ext5.lisp", ("loaded",)),
    ("test/lisp/za_test_ext7.lisp", ("loaded",)),
    ("test/lisp/za_test_ext8.lisp", ("loaded",)),
    ("test/lisp/za_test_ext9.lisp", ("loaded",)),
    ("test/lisp/za_test_stress.lisp", ("loaded",)),
)
# 導出した実物 199 本 / うち未配線 0 本
# 導出した実物 196 本 / うち未配線 0 本


TESTDIR_VAR = "test/c"
SRCDIR_VAR = "src/c"

# 母艦側の単体試験が Lisp の台本を走らせる呼び出し。**呼び出しの形だけを見る**
# (コメントの中の名前は数えない)
RUN_LISP_RE = re.compile(r'run_lisp_file\s*\([^;{}]*?"([^"]+)"\s*\)')
# C の取り込み。**パス接頭辞の無い形(同ディレクトリ)を含む**
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
# 台本の読み込み。証人つき/素のどちらも「走る」ことの根拠にはなる
# (どちらで読んだかは上の check_witness_sources の仕事である)
WIRING_LOAD_RE = re.compile(r'\((?:load|isiki-test-load)\s+"([^"]+)"\s*\)')
MAKE_RECUR_RE = re.compile(r"\$\(MAKE\)\s+(?:\S+=\S+\s+)*(\S+)")
MAKE_VAR_RE = re.compile(r"^([A-Z][A-Z0-9_]*)\s*[:+?]?=\s*(.*)$", re.M)
MAKE_RULE_RE = re.compile(r"^([A-Za-z0-9_./$()-]+(?:\s+[A-Za-z0-9_./$()-]+)*)\s*:(?!=)")


def _census():
    """数え上げの道具を取り込む。

    **「名前の出どころからコメントを剥がす」実装を 1 本にするため**である。
    2 本に分かれると片方だけ直って**道具によって答えが違う**状態になり、
    それはいま直している穴(#208 の誤検出)とまったく同じ族である。
    `tools/census_test_counts.py` は数えるだけの道具だが、
    剥がし方の実装はそちらが持っている(そちらが先に必要だった)。
    """
    # **バイトコードを書かせない。** `tools/__pycache__/` は版管理の対象外に
    # なっていないので、取り込むだけで作業木に落とし物が増える
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import census_test_counts

    return census_test_counts


def strip_c_comments(text):
    """C の `//` と `/* */` を剥がす(実装は数え上げの道具と共用)。"""
    return _census().strip_c_comments(text)


def strip_hash_full_line(text):
    """**行まるごとのコメント**を落とす(実装は数え上げの道具と共用)。"""
    return _census().strip_make_comments(text)


def strip_lisp_comments(text):
    """Lisp の行コメントを剥がす(実装は数え上げの道具と共用)。"""
    return _census().strip_lisp(text, keep_strings=True)


def _read(root, path):
    with open(os.path.join(root, path), encoding="utf-8") as f:
        return f.read()


def make_rules(mk):
    """`Makefile` を「ターゲット名 -> レシピ本体」にする。コメント行は先に落とす。"""
    out = {}
    lines = mk.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        m = MAKE_RULE_RE.match(line)
        if m and not line.startswith("\t"):
            body = []
            j = i + 1
            while j < len(lines) and (lines[j].startswith("\t") or lines[j].strip() == ""):
                if lines[j].startswith("\t"):
                    body.append(lines[j])
                j += 1
            text = "\n".join(body)
            for nm in m.group(1).split():
                out[nm] = out.get(nm, "") + "\n" + text
            i = j
            continue
        i += 1
    return out


def recipe_closure(rules, start):
    """`$(MAKE) <t>` を辿って、そのターゲットから届くレシピ本体を全部つなげる。"""
    seen = set()
    queue = [start]
    texts = []
    while queue:
        t = queue.pop()
        if t in seen or t not in rules:
            continue
        seen.add(t)
        texts.append(rules[t])
        queue += MAKE_RECUR_RE.findall(rules[t])
    return "\n".join(texts), seen


def test_files(root="."):
    """名簿が覆う実物の一覧。`git ls-files test` が真実源。

    版管理の外のものを数えないのが要点(`tmp/` の作りかけが名簿に入ると
    毎回鳴る)。git が使えない合成の木では木を歩く。
    """
    try:
        r = subprocess.run(
            ["git", "ls-files", "test"], cwd=root,
            capture_output=True, text=True, check=True,
        )
        files = sorted(p for p in r.stdout.split("\n") if p)
        if files:
            return files
    except (OSError, subprocess.CalledProcessError):
        pass
    files = []
    for base, _dirs, fs in os.walk(os.path.join(root, "test")):
        for f in fs:
            files.append(os.path.relpath(os.path.join(base, f), root))
    return sorted(files)


def _resolve_ref(origin, target, files):
    """参照先を木のパスに直す。**同ディレクトリの相対参照を解決する**(偽陰性の修正)。"""
    if target in files:
        return target
    cand = os.path.normpath(os.path.join(os.path.dirname(origin), target))
    if cand in files:
        return cand
    return None


def derive_wiring(root="."):
    """**実物の木から**「どこから走るか」を導出する。

    返り値 path -> set((種別, 引数))。名簿はこれと突き合わせるだけで、
    名簿の側に「走る」と書いても導出できなければ落ちる(嘘を書けない)。
    """
    mk = strip_hash_full_line(_read(root, "Makefile"))
    ci = strip_hash_full_line(_read(root, ".github/workflows/ci.yml"))
    rules = make_rules(mk)
    all_text, all_targets = recipe_closure(rules, "test-qemu-all")
    # `make test-qemu` は台本の名前をコマンド行に書かない。ゲスト側の既定値から引く
    m = KERNEL_DEFAULT_SCRIPT_RE.search(_read(root, "src/c/kernel.c"))
    if m:
        all_text += "\n" + m.group(1)
    mkvars = {}
    for name, val in MAKE_VAR_RE.findall(mk):
        mkvars[name] = val.replace("$(TESTDIR)", TESTDIR_VAR).replace("$(SRCDIR)", SRCDIR_VAR)
    mt = re.search(r"^test:(.*)$", mk, re.M)
    test_deps = mt.group(1) if mt else ""

    files = test_files(root)
    found = {p: set() for p in files}

    def mark(path, kind, arg=None):
        if path in found:
            found[path].add((kind, arg))

    for p in files:
        if p in all_text:
            mark(p, "all")
        if p in ci:
            mark(p, "ci")
    # 名前付きの手で呼ぶターゲット。**自動の周回から辿れるものは除く**
    for t in rules:
        if t in all_targets:
            continue
        text, _ = recipe_closure(rules, t)
        for p in files:
            if p in text and ("all", None) not in found[p]:
                mark(p, "manual", t)
    # 母艦側の単体試験が走らせる台本(**コメントを剥がしてから呼び出しの形だけを見る**)
    for p in files:
        if p.endswith(".c"):
            for target in RUN_LISP_RE.findall(strip_c_comments(_read(root, p))):
                mark(target, "host", p)
    # 母艦側の単体試験の原簿そのもの。`Makefile` の変数に名前があり、
    # **その変数が `test:` の前提に並んでいる**こと
    for p in files:
        for name, val in mkvars.items():
            if p in val and ("$(%s)" % name) in test_deps:
                mark(p, "hostbuild", name)
    # 推移。**配線済みのものが読んでいるもの**だけを辿る(到達可能性である)
    for _round in range(20):
        changed = False
        for p in files:
            if not found[p]:
                continue
            if p.endswith(".lisp"):
                for target in WIRING_LOAD_RE.findall(strip_lisp_comments(_read(root, p))):
                    t = _resolve_ref(p, target, files)
                    if t and ("loaded", None) not in found[t]:
                        mark(t, "loaded")
                        changed = True
            elif p.endswith(".c") or p.endswith(".h"):
                for target in INCLUDE_RE.findall(strip_c_comments(_read(root, p))):
                    t = _resolve_ref(p, target, files)
                    if t and ("included", None) not in found[t]:
                        mark(t, "included")
                        changed = True
        if not changed:
            break
    # 道具が資料として名指しで読むもの(**ほかで説明がつかないものだけ**)。
    #
    # **この道具自身は走査から外す。** 下の WIRING は実物のパスを 190 行並べて
    # いるので、自分を読むと「名簿に書いてあるから配線されている」という
    # 円環になり、**名簿が自分で自分を正当化してしまう。**
    # 名簿にしか名前が無いものは「配線されていない」が正しい答えである。
    selfname = os.path.basename(os.path.abspath(__file__))
    rest = [p for p in files if not found[p]]
    if rest:
        for base, _dirs, fs in os.walk(os.path.join(root, "tools")):
            for f in fs:
                rp = os.path.relpath(os.path.join(base, f), root)
                if f == selfname:
                    continue
                if not rp.endswith((".py", ".sh", ".lisp", ".c")):
                    continue
                try:
                    t = _read(root, rp)
                except (UnicodeDecodeError, OSError):
                    continue
                t = strip_lisp_comments(t) if rp.endswith(".lisp") else (
                    strip_c_comments(t) if rp.endswith(".c") else strip_hash_full_line(t))
                for p in rest:
                    if p in t:
                        mark(p, "fixture", rp)
    return found


def _wiring_words(kinds):
    return [k if a is None else "%s:%s" % (k, a) for k, a in sorted(kinds)]


def fmt_wiring(kinds):
    """導出の集合を人が読む形へ。"""
    if not kinds:
        return "unwired:<理由と日付を書くこと>"
    return " + ".join(_wiring_words(kinds))


def roster_line(path, kinds):
    """**次に名簿へ書くべき行をそのまま出す**(規則: 次に書くものをメッセージに出す)。"""
    words = _wiring_words(kinds) or ["unwired:<理由と日付を書くこと>"]
    return '    ("%s", (%s)),' % (path, "".join('"%s", ' % w for w in words).rstrip())


def check_wiring(root=".", roster=None):
    """**起動を要さない静的検査。** 名簿と実物を両方向で突き合わせる。"""
    roster = WIRING if roster is None else roster
    problems = []
    # --- 名簿自身の筋 ---
    seen = set()
    parsed = {}
    for path, wiring in roster:
        if path in seen:
            problems.append("名簿に %s が 2 行ある" % path)
        seen.add(path)
        if not wiring:
            problems.append(
                "名簿の %s に配線が書かれていない。"
                "%s のどれかを書くこと" % (path, "/".join(sorted(WIRING_KINDS)))
            )
            continue
        kinds = set()
        for entry in wiring:
            kind, _, arg = entry.partition(":")
            if kind not in WIRING_KINDS:
                problems.append(
                    "名簿の %s の起点の種類 %r を知らない。"
                    "tools/check_test_count_ratchet.py の WIRING_KINDS に足すこと"
                    "(**知らない起点を黙って飛ばしたのが #208 の誤検出の真因である**)"
                    % (path, kind)
                )
                continue
            needs_arg, _why = WIRING_KINDS[kind]
            if needs_arg and not arg.strip():
                if kind == "unwired":
                    problems.append(
                        "名簿の %s は「意図して未配線」だが**理由が空である。**"
                        "`unwired:<何のために残すのか / いつ決めたのか>` の形で書くこと。"
                        "**理由の無い例外は、ただの穴である**" % path
                    )
                else:
                    problems.append(
                        "名簿の %s の起点 %s に引数が無い(%s)" % (path, kind, _why)
                    )
                continue
            if not needs_arg and arg.strip():
                problems.append(
                    "名簿の %s の起点 %s に引数は要らない(%r が付いている)" % (path, kind, arg)
                )
                continue
            kinds.add((kind, arg.strip() or None))
        parsed[path] = kinds

    derived = derive_wiring(root)

    # --- 両方向 ---
    for path in sorted(set(derived) - seen):
        problems.append(
            "試験の木に %s があるが**名簿に無い。**次の行を WIRING に足すこと:\n"
            "%s\n"
            "  **どこからも走らないなら `unwired:<理由と日付>` と書く**"
            "(理由を書かせるのがこの検査の目的である)"
            % (path, roster_line(path, derived[path]))
        )
    for path in sorted(seen - set(derived)):
        problems.append(
            "名簿に %s があるが**実物が無い**(版管理から消えたか名前が変わった)。"
            "名簿の行も外すこと(消した理由は documents/ に残す。規則 16)" % path
        )

    # --- 名簿の「どこから走る」が導出と合うか ---
    for path in sorted(set(parsed) & set(derived)):
        want = parsed[path]
        got = derived[path]
        if ("unwired", None) in {(k, None) for k, _ in want}:
            pass
        unwired = [a for k, a in want if k == "unwired"]
        if unwired:
            if got:
                problems.append(
                    "名簿は %s を「意図して未配線」と言っているが、**実際は走っている**"
                    "(導出: %s)。名簿を直すこと —— "
                    "**配線されたものを未配線のままにしておくと、"
                    "次に本当に外れたときに気づけない**" % (path, fmt_wiring(got))
                )
            continue
        if not got:
            problems.append(
                "名簿は %s を %s と言っているが、**どの起点からも辿れない。**\n"
                "  走らせたいなら Makefile / CI / 母艦側の単体試験のどれかに配線し、\n"
                "  配線しないと決めたなら `unwired:<理由と日付>` と書くこと(#208)"
                % (path, fmt_wiring(want))
            )
            continue
        missing = want - got
        if missing:
            problems.append(
                "名簿の %s の配線 %s が**導出できない**(導出できたのは %s)。"
                "名簿の行を次に直すこと:\n"
                "%s"
                % (path, fmt_wiring(missing), fmt_wiring(got), roster_line(path, got))
            )
    return problems


# --- 配線の検査の自己対照(#208。規則 8)------------------------------------
#
# **本物の木では陽性対照が取れない。** いまの木には「意図して未配線」が 1 本も
# 無く(#208 でぜんぶ配線した)、名簿にしか無い行も無いからである。
# だから対照に使う木は**その場で合成する**(既存の synth_log と同じ考え方)。

WIRING_SYNTH_MAKEFILE = """\
SRCDIR = src/c
TESTDIR = test/c
TEST_SRC_FOO = $(SRCDIR)/foo.c $(TESTDIR)/foo_test.c

test: check-something $(TEST_SRC_FOO)
\t@echo building

test-qemu-all:
\t$(MAKE) test-qemu-all-stages

test-qemu-all-stages:
\t$(MAKE) test-qemu
\t$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_wired.lisp

test-qemu-hand:
\t$(MAKE) test-qemu-milestone MILESTONE=test/lisp/qemu_boot_byhand.lisp
"""

WIRING_SYNTH_CI = """\
jobs:
  qemu:
    steps:
      - run: make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_ci.lisp
"""

WIRING_SYNTH_KERNEL = """\
static void boot(void) {
  g_qemu_test_boot_script[0] = "test/lisp/qemu_boot_test.lisp";
}
"""

# 母艦側の単体試験。**走らせているのは 1 本だけで、もう 1 本は
# コメントの中にしか名前が無い**(偽陽性の対照)。
# 取り込みは**同ディレクトリの相対参照**で書く(偽陰性の対照)
WIRING_SYNTH_HOST_C = """\
#include "helper.h"
/* test/lisp/only_in_comment_test.lisp がこの経路を説明している */
static void t(void) {
  // test/lisp/also_only_in_comment_test.lisp も走らせていない
  run_lisp_file(proc, env, "test/lisp/by_host_test.lisp");
}
"""


def wiring_synth_tree():
    """対照に使う木をその場で作る。返り値は根のパス(呼び手が消す)。"""
    root = tempfile.mkdtemp(prefix="wiring-selftest-")

    def put(rel, text):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    put("Makefile", WIRING_SYNTH_MAKEFILE)
    put(".github/workflows/ci.yml", WIRING_SYNTH_CI)
    put("src/c/kernel.c", WIRING_SYNTH_KERNEL)
    put("test/c/foo_test.c", WIRING_SYNTH_HOST_C)
    put("test/c/helper.h", "#define H 1\n")
    # 自動の周回から呼ばれる台本。1 本は試験ファイルを読む(推移の対照)
    put("test/lisp/qemu_boot_test.lisp", '(isiki-test-load "test/lisp/leaf_test.lisp")\n')
    put("test/lisp/qemu_boot_wired.lisp", "(nothing)\n")
    put("test/lisp/qemu_boot_ci.lisp", "(nothing)\n")
    put("test/lisp/qemu_boot_byhand.lisp", "(nothing)\n")
    put("test/lisp/leaf_test.lisp", "(assert-equal 1 1)\n")
    put("test/lisp/by_host_test.lisp", "(assert-equal 1 1)\n")
    # **コメントの中にしか名前が無い 2 本。** 走っていないのが正しい答え
    put("test/lisp/only_in_comment_test.lisp", "(assert-equal 1 1)\n")
    put("test/lisp/also_only_in_comment_test.lisp", "(assert-equal 1 1)\n")
    return root


WIRING_SYNTH_ROSTER = (
    ("test/c/foo_test.c", ("hostbuild:TEST_SRC_FOO",)),
    ("test/c/helper.h", ("included",)),
    ("test/lisp/also_only_in_comment_test.lisp", ("unwired:名前がコメントにしか無い対照 2026-11-09",)),
    ("test/lisp/by_host_test.lisp", ("host:test/c/foo_test.c",)),
    ("test/lisp/leaf_test.lisp", ("loaded",)),
    ("test/lisp/only_in_comment_test.lisp", ("unwired:名前がコメントにしか無い対照 2026-11-09",)),
    ("test/lisp/qemu_boot_byhand.lisp", ("manual:test-qemu-hand",)),
    ("test/lisp/qemu_boot_ci.lisp", ("ci",)),
    ("test/lisp/qemu_boot_test.lisp", ("all",)),
    ("test/lisp/qemu_boot_wired.lisp", ("all",)),
)


def _roster_without(path):
    return tuple(r for r in WIRING_SYNTH_ROSTER if r[0] != path)


def _roster_with(path, wiring):
    return _roster_without(path) + ((path, wiring),)


def wiring_self_test():
    ok = True

    def check(name, got, want):
        nonlocal ok
        if got != want:
            ok = False
        print("  [%s] %s: %s (期待 %s)" % ("OK" if got == want else "NG", name, got, want))

    print("[wiring-self-test] 配線の名簿の検査の対照(規則 8)")
    root = wiring_synth_tree()
    try:
        def probs(roster):
            return check_wiring(root, roster)

        # --- 陰性対照: 合成した木 + 正しい名簿では鳴らない ---
        base = probs(WIRING_SYNTH_ROSTER)
        check("陰性 合成の木で鳴らない", base, [])

        # **偽陰性の対照**: 同ディレクトリの相対参照(`#include "helper.h"`)を
        # 辿れていること。辿れていなければ上の名簿の `included` が導出できず鳴る
        derived = derive_wiring(root)
        check(
            "陰性 同ディレクトリの相対参照を辿れる(偽陰性の修正)",
            ("included", None) in derived["test/c/helper.h"],
            True,
        )
        # **偽陽性の対照**: コメントの中にしか名前が無い 2 本は配線されていない
        check(
            "陰性 コメントの中の名前を配線と数えない(偽陽性の修正)",
            derived["test/lisp/only_in_comment_test.lisp"]
            | derived["test/lisp/also_only_in_comment_test.lisp"],
            set(),
        )

        # --- 陽性 1: 台本を 1 本足して名簿に書かない ---
        newfile = os.path.join(root, "test/lisp/brand_new_test.lisp")
        with open(newfile, "w", encoding="utf-8") as f:
            f.write("(assert-equal 1 1)\n")
        p1 = probs(WIRING_SYNTH_ROSTER)
        check("陽性 1 名簿に無い台本で落ちる", bool(p1), True)
        check(
            "陽性 1 次に書く行を出す",
            any('("test/lisp/brand_new_test.lisp", ("unwired:' in x for x in p1),
            True,
        )
        os.remove(newfile)
        check("陰性 1 戻すと鳴らない", probs(WIRING_SYNTH_ROSTER), [])

        # --- 陽性 2: 「意図して未配線」に理由が無い ---
        p2 = probs(_roster_with("test/lisp/only_in_comment_test.lisp", ("unwired:",)))
        check("陽性 2 理由の無い未配線で落ちる", bool(p2), True)
        check(
            "陽性 2 理由が空だと言う",
            any("理由の無い例外は、ただの穴である" in x for x in p2),
            True,
        )
        check(
            "陽性 2b 引数ごと無いときも落ちる",
            bool(probs(_roster_with("test/lisp/only_in_comment_test.lisp", ("unwired",)))),
            True,
        )

        # --- 陽性 3: 名簿にあるのに実物が無い ---
        p3 = probs(WIRING_SYNTH_ROSTER + (("test/lisp/vanished_test.lisp", ("loaded",)),))
        check("陽性 3 実物の無い行で落ちる", bool(p3), True)
        check(
            "陽性 3 その名前を出す",
            any("test/lisp/vanished_test.lisp" in x and "実物が無い" in x for x in p3),
            True,
        )

        # --- 陽性 4: 名簿が「走っている」と嘘を書く ---
        p4 = probs(_roster_with("test/lisp/only_in_comment_test.lisp", ("host:test/c/foo_test.c",)))
        check("陽性 4 コメントだけの台本を host と書くと落ちる", bool(p4), True)
        check(
            "陽性 4 どの起点からも辿れないと言う",
            any("どの起点からも辿れない" in x for x in p4),
            True,
        )
        p4b = probs(_roster_with("test/lisp/qemu_boot_byhand.lisp", ("all",)))
        check("陽性 4b 手で呼ぶ段を all と書くと落ちる", bool(p4b), True)

        # --- 陽性 5: 配線されているのに「意図して未配線」と書く ---
        p5 = probs(_roster_with("test/lisp/qemu_boot_wired.lisp", ("unwired:もう使わない 2026-11-09",)))
        check("陽性 5 走っているのに未配線と書くと落ちる", bool(p5), True)
        check(
            "陽性 5 実際は走っていると言う",
            any("実際は走っている" in x for x in p5),
            True,
        )

        # --- 陽性 6: 知らない起点の種類 ---
        p6 = probs(_roster_with("test/lisp/leaf_test.lisp", ("somehow:x",)))
        check("陽性 6 知らない起点の種類で落ちる", bool(p6), True)
        check(
            "陽性 6 #208 の真因だと言う",
            any("#208 の誤検出の真因" in x for x in p6),
            True,
        )
        # 引数の要らない種別に引数を付けた / 要るのに付けない
        check(
            "陽性 6b 引数の過不足で落ちる",
            bool(probs(_roster_with("test/lisp/leaf_test.lisp", ("loaded:よけいな引数",))))
            and bool(probs(_roster_with("test/lisp/qemu_boot_byhand.lisp", ("manual",)))),
            True,
        )
        # 名簿の行が 2 つある
        check(
            "陽性 6c 同じ台本が 2 行あると落ちる",
            bool(probs(WIRING_SYNTH_ROSTER + (("test/lisp/leaf_test.lisp", ("loaded",)),))),
            True,
        )
    finally:
        shutil.rmtree(root, ignore_errors=True)

    # --- 陰性対照の本番: **いまの木で誤って鳴らない** ---
    check("陰性 本物の木で鳴らない", check_wiring("."), [])
    return ok


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
    # [#203 / T2] 段の区間そのもの。**内訳の特定**(judge_files)に渡す
    stage_segments = {}
    strays = []  # 名簿に無い段で見つけた合否
    unreadable = []  # どの読み取り方でも拾えなかった合否の形の行
    unexpected_round = []  # 名簿にはあるが、この周では走らないはずの段

    for name, segment in segments:
        if name in expected:
            reader = READERS[expected[name][1]]
            found, consumed = reader(segment)
            readings.setdefault(name, []).extend(found)
            stage_segments.setdefault(name, []).extend(segment)
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
            # [#203 / T2] **ここで内訳を言う。** 合計が合っていても走らせる
            # (合計が合ったまま内訳が入れ替わる形は、合計だけでは見えない)
            ng.extend(judge_files(name, reader, stage_segments.get(name, []), actual))

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
    problems = check_roster() + check_files_roster()
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


def synth_body(
    stage,
    reader,
    failed=0,
    item_delta=None,
    drop_item=None,
    drop_item_fully=None,
    unwitnessed=0,
):
    """段の中身(件数の出方)を作る。読み取り方ごとに形が違う。

    **[#203 / T2] 名簿の内訳どおりに 1 件 1 行の証人を出す。**
    以前はここが `#count test/lisp/synthetic_test.lisp N` の 1 行だけだった。
    内訳の照合を足したので、**合成ログも内訳を持たないと陰性対照が成立しない。**

    つまみ:
      item_delta       … (項目, 増減)。その項目の件数をずらす(本番の「縮み」)
      drop_item        … 件数の行だけを落とす(`#at` は残る = 途中で止まった)
      drop_item_fully  … 進捗の行も件数の行も落とす(まるごと読まれなかった)
      unwitnessed      … 証人の無いアサーションを合計にだけ足す(素の load の形)
    """
    own, roster_items = FILES[stage]
    items = [(n, c) for n, c in roster_items if n != drop_item_fully]
    out = []
    end_check = 0
    total = own + unwitnessed
    for name, count in items:
        if item_delta and item_delta[0] == name:
            count += item_delta[1]
        if reader == "isiki":
            # **進捗の行が先。** load の前に出る(test_framework.lisp)
            out.append("#at %s t=100 += 10\n" % name)
            if name == drop_item:
                continue  # 件数の行が出ないまま止まった
            out.append("#count %s %d\n" % (name, count))
            end_check += END_CHECK_ASSERTS
            total += count
            continue
        if name == drop_item:
            continue
        total += count
        if reader == "self-tally":
            out.append("#ck %s\n" % name)
        else:
            out.append("[no9p] ok   %s\n" % name)
    total += end_check
    passed = total - failed
    if reader == "isiki":
        out.append("\n")
        verdict = "==== isiki tests: %d passed, %d failed ====\n" % (passed, failed)  # PASS-LINE: FIXTURE 合成ログ(isiki の読み取り方)
        out.append(verdict)
        return out
    if reader == "self-tally":
        for i in range(failed):
            out.insert(0, "#ck synthetic-ng-%d\n" % i)
            out.insert(0, "[NG] synthetic-ng-%d\n" % i)
        out.append("\n")
        verdict = "==== isiki tests: %d passed, %d failed ====\n" % (passed, failed)  # PASS-LINE: FIXTURE 合成ログ(self-tally の読み取り方)
        out.append(verdict)
        return out
    if reader == "no9p":
        out.insert(0, "[no9p] boot smoke begin\n")
        out.append("[no9p] %d passed, %d failed\n" % (passed, failed))  # PASS-LINE: FIXTURE 合成ログ(no9p の読み取り方)
        return out
    raise AssertionError("合成できない読み取り方: %r" % reader)


def synth_log(
    round_name,
    style="make",
    drop=None,
    delta=None,
    fail=None,
    extra=None,
    drop_item=None,
    drop_item_fully=None,
    unwitnessed=None,
):
    """名簿どおりの 1 周のログを合成する。

    drop             … この段をまるごと落とす(段が走らなかったログ)
    delta            … (段, 増減)。**その段の最後の項目**の件数をずらす。
                       内訳と合計が同じだけ動くので、本番の縮みと同じ形になる
    fail             … (段, 失敗件数)。失敗を混ぜる
    extra            … 追加で書く行(名簿に無い段など)
    drop_item        … (段, 項目)。件数の行だけ落とす(進捗の行は残る)
    drop_item_fully  … (段, 項目)。進捗の行も落とす
    unwitnessed      … (段, 件数)。証人の無いアサーションを合計にだけ足す
    """
    lines = ["make[1]: Entering directory '/repo'\n"]
    for name, count, reader, rounds in COUNTS:
        if round_name not in rounds or name == drop:
            continue
        item_delta = None
        if delta and delta[0] == name:
            # **最後の項目**を動かす(どれでもよいが、固定しておくと対照が読める)
            item_delta = (FILES[name][1][-1][0], delta[1])
        lines.append(synth_stage_header(name, style))
        lines.extend(
            synth_body(
                name,
                reader,
                failed=fail[1] if fail and fail[0] == name else 0,
                item_delta=item_delta,
                drop_item=drop_item[1] if drop_item and drop_item[0] == name else None,
                drop_item_fully=(
                    drop_item_fully[1]
                    if drop_item_fully and drop_item_fully[0] == name
                    else None
                ),
                unwitnessed=(
                    unwitnessed[1] if unwitnessed and unwitnessed[0] == name else 0
                ),
            )
        )
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
        "陽性 件数 -1 のとき次に書く数字(184)を出す",
        "COUNTS の \"%s\" を 184 に" % victim in ng_text(synth_log(ALL, delta=(victim, -1))),
        True,
    )
    check("陽性 件数 +1 で落ちる", passes(synth_log(ALL, delta=(victim, 1))), False)
    check(
        "陽性 件数 +1 のとき次に書く数字(186)を出す",
        "COUNTS の \"%s\" を 186 に" % victim in ng_text(synth_log(ALL, delta=(victim, 1))),
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

    # ------------------------------------------------------------------
    # [#203 / T2] **内訳の特定**の対照。ここから下は「どのファイルが縮んだか」
    # を言えることの対照である(census §5-2 で「言えない」と書かれていた穴)
    # ------------------------------------------------------------------

    # 名簿自身の等式(内訳の和 = 段の合計)
    check("内訳の名簿の検査が通る", check_files_roster(), [])
    # **証人の出どころ**(台本が素の load で試験ファイルを読んでいないこと)
    check("証人の出どころの検査が通る", check_witness_sources(), [])

    # 陽性対照 A: **ある試験ファイルのアサーションが 1 件減ったら、
    # そのファイル名が出て落ちる。**無証人だった段で確かめる(これが本題)
    # **1 項目 1 件の読み方では「1 件減る」は「その行が消える」である**
    # (`#ck` / `[no9p] ok` は 1 件 1 行なので、件数だけを減らす形が無い)。
    # だから自前集計と C 側の段は drop_item で縮ませる
    for stage, victim_file, how in (
        ("test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img]", "test/lisp/device_test.lisp", "delta"),
        ("test/lisp/qemu_boot_m3_za_ext5.lisp", "test/lisp/za_test_ext19.lisp", "delta"),
        ("test/lisp/qemu_boot_no_init_signal.lisp", "sg-alive-after", "drop"),
        ("test-qemu-no-9p-run", "app-hello-42", "drop"),
    ):
        rnd = CI if stage == "test/lisp/qemu_boot_m3_za_ext5.lisp" else ALL
        if how == "delta":
            log = synth_log(rnd, delta=(stage, -1))
        else:
            log = synth_log(rnd, drop_item=(stage, victim_file))
        check("陽性 %s の 1 件減で落ちる" % victim_file, passes(log, rnd), False)
        check(
            "陽性 縮んだ先の名前(%s)が出る" % victim_file,
            victim_file in ng_text(log, rnd),
            True,
        )

    # 陽性対照 B: **証人の無いファイルが増えたら落ちる。**
    # 2 つの経路で見る。
    #   B-1(静的): 台本が素の load で試験ファイルを読んだら、ログを見る前に落ちる
    #   B-2(ログ): 合計だけが増えて証人が無ければ、等式の残差で落ちる
    unwit = synth_log(ALL, unwitnessed=(victim, 7))
    check("陽性 B-2 証人の無い 7 件で落ちる", passes(unwit), False)
    check(
        "陽性 B-2 その理由を「証人の無いアサーション」と言う",
        "証人の無いアサーションが混ざっている" in ng_text(unwit),
        True,
    )

    # 陽性対照 C: **ファイルが丸ごと落ちたら、そのファイル名が出て落ちる。**
    # 2 つに分かれる(痕跡が違う。調査 §9-4-7)
    #   C-1: 進捗の行はあるが件数の行が無い = 読み始めて出口へ着かなかった
    #   C-2: どちらも無い = そもそも読まれなかった
    died = synth_log(ALL, drop_item=(victim, "test/lisp/fat16_test.lisp"))
    check("陽性 C-1 途中で止まったファイルで落ちる", passes(died), False)
    check(
        "陽性 C-1 そのファイル名が出る",
        "test/lisp/fat16_test.lisp" in ng_text(died),
        True,
    )
    check(
        "陽性 C-1 進捗の行だけがある形だと言う",
        "**進捗の行(#at)は出ている**" in ng_text(died),
        True,
    )
    gone = synth_log(ALL, drop_item_fully=(victim, "test/lisp/fat16_test.lisp"))
    check("陽性 C-2 読まれなかったファイルで落ちる", passes(gone), False)
    check(
        "陽性 C-2 そのファイル名が出る",
        "test/lisp/fat16_test.lisp" in ng_text(gone),
        True,
    )

    # 陽性対照 D: 名簿に無い証人の行(試験ファイルを足したのに名簿に書いていない)
    added = [line for _, line in synth_log(ALL)]
    for i, line in enumerate(added):
        if line.startswith("#count test/lisp/fat16_test.lisp"):
            added.insert(i, "#at test/lisp/brand_new_test.lisp t=1 += 1\n")
            added.insert(i + 1, "#count test/lisp/brand_new_test.lisp 4\n")
            break
    added = [(i, line) for i, line in enumerate(added, start=1)]
    check("陽性 D 名簿に無い証人の行で落ちる", passes(added), False)
    check(
        "陽性 D その名前を出す",
        "test/lisp/brand_new_test.lisp" in ng_text(added),
        True,
    )

    # 陰性対照の念押し: **別の目的の `#count` 行で鳴らないこと。**
    # test/lisp/disassemble_test.lisp:319 が
    #   `#count dis-add    kernel=0 immobilized=0 gc-heap=0`
    # を出している。**形が似た行は誤報の種**なので、実物を入れて鳴らないことを見る
    other_count = [line for _, line in synth_log(ALL)]
    for i, line in enumerate(other_count):
        if line.startswith("#count test/lisp/fat16_test.lisp"):
            other_count.insert(i, "#count dis-add    kernel=0 immobilized=0 gc-heap=0\n")
            other_count.insert(i, "#count dis-caller kernel=0 immobilized=0 gc-heap=0\n")
            break
    other_count = [(i, line) for i, line in enumerate(other_count, start=1)]
    check("陰性 別の目的の #count 行で鳴らない", passes(other_count), True)

    # 陽性対照 E: **静的な検査**。台本が素の load で試験ファイルを読む形
    saved_files = FILES
    try:
        # 名簿から 1 本外す = 台本が読んでいるのに名簿に無い状態
        globals()["FILES"] = dict(
            FILES,
            **{
                "test/lisp/qemu_boot_m4_environment.lisp": (
                    FILES["test/lisp/qemu_boot_m4_environment.lisp"][0],
                    FILES["test/lisp/qemu_boot_m4_environment.lisp"][1][:-1],
                )
            }
        )
        problems = check_witness_sources()
        check("陽性 E 台本と内訳の名簿が食い違うと落ちる", bool(problems), True)
        check(
            "陽性 E その名前を出す",
            any("environment_utilities_test.lisp" in p for p in problems),
            True,
        )
        check("陽性 E 等式も崩れる", bool(check_files_roster()), True)
    finally:
        globals()["FILES"] = saved_files
    check("陰性 E 名簿を戻すと通る", check_witness_sources(), [])

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
    parser.add_argument(
        "--files",
        action="store_true",
        help="ファイル単位の名簿を出す(段の合計は内訳から導出。#203)",
    )
    parser.add_argument(
        "--witness",
        action="store_true",
        help="**ログを見ずに**証人の出どころを見る(台本と内訳の名簿の突き合わせ。#203)",
    )
    parser.add_argument("--self-test", action="store_true", help="計器自身の対照")
    parser.add_argument(
        "--wiring",
        action="store_true",
        help="**ログを見ずに**試験の木の配線を名簿と突き合わせる(#208)",
    )
    parser.add_argument(
        "--wiring-derive",
        action="store_true",
        help="配線の名簿の初期値を**実物の木から導出して刷る**(手で書かないため。規則 13)",
    )
    parser.add_argument(
        "--wiring-self-test", action="store_true", help="配線の検査自身の対照(#208)"
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return 0 if self_test() else 1

    if args.wiring_self_test:
        return 0 if wiring_self_test() else 1

    if args.wiring_derive:
        derived = derive_wiring(".")
        print("WIRING = (")
        for path in sorted(derived):
            print(roster_line(path, derived[path]))
        print(")")
        print(
            "# 導出した実物 %d 本 / うち未配線 %d 本"
            % (len(derived), sum(1 for v in derived.values() if not v))
        )
        return 0

    if args.wiring:
        problems = check_wiring(".")
        for prob in problems:
            print("NG(配線): " + prob, file=sys.stderr)
        if problems:
            return 1
        tally = {}
        for _path, wiring in WIRING:
            for entry in wiring:
                tally[entry.split(":")[0]] = tally.get(entry.split(":")[0], 0) + 1
        print(
            "[test-file wiring] 実物 %d 本を名簿に列挙 —— "
            "**どこからも走らない台本は無い**" % len(WIRING)
        )
        for kind in sorted(tally):
            print("  %-10s %4d  (%s)" % (kind, tally[kind], WIRING_KINDS[kind][1]))
        print("  OK")
        return 0

    if args.witness:
        problems = check_files_roster() + check_witness_sources()
        for p in problems:
            print("NG(証人): " + p, file=sys.stderr)
        if problems:
            return 1
        stages = len(FILES)
        items = sum(len(v[1]) for v in FILES.values())
        total = sum(c for _, c, _, _ in COUNTS)
        print(
            "[test-count witness] %d 段 / 証人 %d 項目 / 合計 %d 件 —— "
            "**どの段でも内訳が言える**" % (stages, items, total)
        )
        print("  OK")
        return 0

    if args.files:
        problems = check_files_roster()
        for p in problems:
            print("NG(内訳の名簿): " + p, file=sys.stderr)
        for stage, count, reader, rounds in COUNTS:
            own, items = FILES[stage]
            end_check = END_CHECK_ASSERTS * len(items) if reader == "isiki" else 0
            print(
                "%s  (%s / %s)  合計 %d = Σ%d + 出口 %d + 台本 %d"
                % (
                    stage,
                    reader,
                    ",".join(rounds),
                    count,
                    sum(n for _, n in items),
                    end_check,
                    own,
                )
            )
            for name, n in items:
                print("    %-56s %5d" % (name, n))
        print(
            "内訳の名簿 全体 %d 段 / %d 項目"
            % (len(FILES), sum(len(v[1]) for v in FILES.values()))
        )
        return 1 if problems else 0

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
