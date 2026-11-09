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
import sys

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
    ("test-qemu", 5178, "isiki", (ALL,)),
    # 枠組みを使えない段(条件クラスが 1 つも無い状態を要件とする)。**自前集計**で、
    # 合否の一行の形は枠組みと同じだが出している主体が別である。`#ck` を数える
    ("test/lisp/qemu_boot_no_init_signal.lisp", 15, "self-tally", (ALL,)),
    # C 側の起動スモーク。**合否の一行が出ない唯一の段**で、`[no9p] ok` を数える
    ("test-qemu-no-9p-run", 8, "no9p", (ALL,)),
    ("test/lisp/qemu_boot_load_disk.lisp", 23, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_error_escape_gc.lisp", 378, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_m5_ide.lisp", 7, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img]", 159, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_fat_callback_transfer.lisp [fat16_test.img]", 17, "isiki", (ALL, CI)),
    ("test/lisp/qemu_boot_load_gc_stress.lisp [fat16_test.img]", 21, "isiki", (ALL,)),
    ("test/lisp/qemu_boot_fat32.lisp [fat32_test.img]", 140, "isiki", (ALL, CI)),
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
    # --- ここから下は CI だけで走る段(test-qemu-all に入っていない) ---
    ("test/lisp/qemu_boot_m1_base.lisp", 2785, "isiki", (CI,)),
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
    # test-qemu … 読み方 isiki / 台本自身 1 件 / 証人 59 本 / 導出の合計 5178 件
    "test-qemu": (1, (
        ("test/lisp/test_framework_test.lisp", 29),
        ("test/lisp/init_test.lisp", 496),
        ("test/lisp/toplevel_abort_test.lisp", 17),
        ("test/lisp/handler_exit_test.lisp", 41),
        ("test/lisp/arith_signal_test.lisp", 40),
        ("test/lisp/index_signal_test.lisp", 51),
        ("test/lisp/undefined_signal_test.lisp", 46),
        ("test/lisp/io_signal_test.lisp", 33),
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
    # test/lisp/qemu_boot_error_escape_gc.lisp … 読み方 isiki / 台本自身 4 件 / 証人 8 本 / 導出の合計 378 件
    "test/lisp/qemu_boot_error_escape_gc.lisp": (4, (
        ("test/lisp/toplevel_abort_test.lisp", 17),
        ("test/lisp/handler_exit_test.lisp", 41),
        ("test/lisp/arith_signal_test.lisp", 40),
        ("test/lisp/index_signal_test.lisp", 51),
        ("test/lisp/undefined_signal_test.lisp", 46),
        ("test/lisp/io_signal_test.lisp", 33),
        ("test/lisp/type_unbound_test.lisp", 79),
        ("test/lisp/arity_signal_test.lisp", 51),
    )),
    # test/lisp/qemu_boot_m5_ide.lisp … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 7 件
    "test/lisp/qemu_boot_m5_ide.lisp": (0, (
        ("test/lisp/ide_test.lisp", 5),
    )),
    # test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img] … 読み方 isiki / 台本自身 0 件 / 証人 2 本 / 導出の合計 159 件
    "test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img]": (0, (
        ("test/lisp/fat16_test.lisp", 145),
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
    # test/lisp/qemu_boot_fat32.lisp [fat32_test.img] … 読み方 isiki / 台本自身 0 件 / 証人 1 本 / 導出の合計 140 件
    "test/lisp/qemu_boot_fat32.lisp [fat32_test.img]": (0, (
        ("test/lisp/fat32_test.lisp", 138),
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
    # test/lisp/qemu_boot_m1_base.lisp … 読み方 isiki / 台本自身 1 件 / 証人 13 本 / 導出の合計 2785 件
    "test/lisp/qemu_boot_m1_base.lisp": (1, (
        ("test/lisp/test_framework_test.lisp", 29),
        ("test/lisp/init_test.lisp", 496),
        ("test/lisp/toplevel_abort_test.lisp", 17),
        ("test/lisp/handler_exit_test.lisp", 41),
        ("test/lisp/arith_signal_test.lisp", 40),
        ("test/lisp/index_signal_test.lisp", 51),
        ("test/lisp/undefined_signal_test.lisp", 46),
        ("test/lisp/io_signal_test.lisp", 33),
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
            problems.append("段 %s の項目が空。内訳の無い名簿は何も見ていない" % stage)
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
        "陽性 件数 -1 のとき次に書く数字(158)を出す",
        "COUNTS の \"%s\" を 158 に" % victim in ng_text(synth_log(ALL, delta=(victim, -1))),
        True,
    )
    check("陽性 件数 +1 で落ちる", passes(synth_log(ALL, delta=(victim, 1))), False)
    check(
        "陽性 件数 +1 のとき次に書く数字(160)を出す",
        "COUNTS の \"%s\" を 160 に" % victim in ng_text(synth_log(ALL, delta=(victim, 1))),
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
    args = parser.parse_args(argv)

    if args.self_test:
        return 0 if self_test() else 1

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
