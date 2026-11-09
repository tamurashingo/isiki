# 検出器の完全性 —— 参照マップ(#197 / #200)

実装担当・レビュー担当向け。論理要約 `detector-integrity.logic.md` の項目番号に対応する。
調査日 2026-11-09 / 基点コミット `07c4a0b`(= `main`。当時 `feature/m7-typespec` は main と同一)。
**QEMU は 1 回も起動していない。** 現物の件数はすべて静的な見積りである。

---

## §0 — ★ ほかにも同じ形で壊れうる段(いちばん重要)

### 0-A 件数の証人がどこに付いているか

| 要素 | 実装箇所 |
|---|---|
| ファイル出口の検査(件数の証人) | `test/lisp/test_framework.lisp:236-241` `isiki-file-end-check` |
| 件数の行の出力 | `test/lisp/test_framework.lisp:238` (`"#count ~A ~D~%"`) |
| 件数に対する唯一の検査 | `test/lisp/test_framework.lisp:241` `(assert-equal (list path t) (list path (> n 0)))` ← **`n > 0` しか見ない** |
| 出口の検査を呼ぶ唯一の経路 | `test/lisp/test_framework.lisp:69-83` `isiki-test-load`(`:82` で呼ぶ) |
| **素の `load` で読んだファイル** | 出口の検査が走らない = `#count` 行が出ない = 母艦に件数が残らない |

### 0-B 証人の無い段の一覧(静的なアサーション数)

数え方: `(assert-equal|assert-error|assert-error-class|assert-float-close|assert-output`
の出現数(コメント・文字列を除去して計上)。`isiki-test-load` 経由のファイルは
出口の検査自身が 2 件を足すので合計は「+2」。

| # | 台本 | 内容ファイル(素の `load`) | 静的件数 | どこで走るか |
|---|---|---|---|---|
| 1 | `test/lisp/qemu_boot_m1_base.lisp` | `test_framework_test` 48 / `init_test` 500 / `toplevel_abort_test` 17 / `handler_exit_test` 41 / `arith_signal_test` 40 / `index_signal_test` 51 / `undefined_signal_test` 46 / `io_signal_test` 33 / `type_unbound_test` 79 / `arity_signal_test` 51 / `isiki_test` 664 / `isiki_test_syntax` 11 / `isiki_test_jit` 1222 | **2804** | **CI のみ**(`.github/workflows/ci.yml:35`) |
| 2 | `test/lisp/qemu_boot_m3_za_ext5.lisp` | `za_test_ext5`69 `ext7`24 `ext8`31 `ext9`22 `ext10`23 `ext11`8 `ext12`10 `ext13`32 `ext14`24 `ext15`25 `ext16`31 `ext17`28 `ext18`50 `ext19`22 | **399** | CI のみ(`ci.yml:43`) |
| 3 | `test/lisp/qemu_boot_m4_environment.lisp` | `environment_pages_test`20 / `environment_literal_slots_test`80 / `environment_utilities_test`28 | **128** | CI のみ(`ci.yml:47`) |
| 4 | `test/lisp/qemu_boot_fat16_m0.lisp` | `bitwise_char_test` 8 | **8** | CI のみ(`ci.yml:55`) |
| 5 | `test/lisp/qemu_boot_m6_fat16.lisp` | `fat16_test`147 / `device_test`10 | **157** | `Makefile:1265` + `ci.yml:59` |
| 6 | `test/lisp/qemu_boot_fat32.lisp` | `fat32_test` 140 | **140** | `Makefile:1268` + `ci.yml:70` |
| 7 | `test/lisp/qemu_boot_partition.lisp` | `partition_test`19 / `file_cmd_test`19 | **38**(×2 イメージ) | `Makefile:1270-1271` + `ci.yml:78,82` |
| 8 | `test/lisp/qemu_boot_fat32_primary_boot.lisp` | `fat32_primary_boot_test` 11 | **11** | `Makefile:1269` + `ci.yml:74` |
| 9 | `test/lisp/qemu_boot_m5_ide.lisp` | `ide_test` 5 | **5** | `Makefile:1264` + `ci.yml:51` |
| 10 | `test/lisp/qemu_boot_m2_za_stress.lisp` | `za_test`245 / `za_test_stress`9 | **254** | `Makefile:1318`(手で) |
| 11 | `test/lisp/qemu_boot_read_file_native.lisp` | `read_file_native_test` 15 | 15 | `Makefile:1327` |
| 12 | `test/lisp/qemu_boot_imm_leak.lisp` | `imm_leak_test` 15 | 15 | `Makefile:1697` |
| 13 | `test/lisp/qemu_boot_for_expansion.lisp` | `for_expansion_test` 8 | 8 | `Makefile:1702` |
| 14 | `test/lisp/qemu_boot_bench_construct.lisp` | `bench_construct_test` 20 | 20 | `Makefile:1707` |
| 15 | `qemu_boot_perf_fat16` / `perf_fat32` / `perf_read_fat16` / `perf_read_fat32` | 13 / 13 / 6 / 6 | 38 | `Makefile:1337-1338,1403-1404` |
| 16 | `test/lisp/qemu_boot_float_contagion_bench.lisp` | `float_contagion_bench` 15 | 15 | `Makefile:1715` |

**合計**:
- `make test-qemu-all`(`Makefile:1257-1282`)の中で証人が無いのは #5〜#9 = **389 件**
  + §0-C の自前集計 15 件 = **404 件**
- CI(`ci.yml:35-82`)で証人が無いのは #1〜#9 = **3,728 件**。
  証人があるのは `qemu_boot_m2_za.lisp`(679)と `qemu_boot_fat_callback_transfer.lisp`(17)の
  **696 件だけ** → **無証人率 84.3%**

### 0-C 自前の集計を持つ段(共通の枠組みへの変更が届かない)

| 項目 | 実装箇所 |
|---|---|
| 台本 | `test/lisp/qemu_boot_no_init_signal.lisp` |
| 成果物を自分で開く | `:30` `(defglobal *s* (open-output-file "/9p/test-results.txt"))` |
| 自前の集計 | `:31-32` `*pass*` / `*fail*`、`:34-44` `ck` |
| 1 件ごとの行 | `:41` `"#ck ~A~%"`(**`#count` ではない**) |
| 合否の一行を自分で組む | `:114` `"~%==== isiki tests: ~D passed, ~D failed ====~%"` |
| 確認の件数 | `grep -c '^(ck ' = 15` |
| 走る場所 | `Makefile:1188` `test-qemu-no-embedded-init` → **`test-qemu-all` に入っている**(`Makefile:1260`) |

### 0-D 「ファイル全体が 1 つの最上位フォーム」= 今回の事故と同じ構造

| ファイル | 最上位フォーム数 | アサーション | 読み方 | 段 |
|---|---|---|---|---|
| `test/lisp/load_gc_stress_test.lisp` | **1**(`:28` の `(if (require-gc-debug-build ...) (progn ...))`) | 19 | `isiki-test-load` | `Makefile:1669-1671`(イメージ再作成で対処済み) |
| `test/lisp/for_gc_stress_test.lisp` | **1**(`:18`) | 13 | `isiki-test-load` | `Makefile:1679-1680`。**`test-qemu-all` に無い** |
| `test/lisp/stack_guard_gc_test.lisp` | **1**(`:17`) | 7 | `isiki-test-load` | `Makefile:1689-1690`。**`test-qemu-all` に無い** |
| `test/lisp/za_abi_probe_test.lisp` | 2 | 2 | `isiki-test-load` | `qemu_boot_test.lisp` |
| `test/lisp/environments_predefined_test.lisp` | 1 | 1 | — | **どの台本からも読まれていない** |

### 0-E 数え方の検証(実測との一致)

| 段 | 実測の記録 | 記録の場所 | 静的な見積り |
|---|---|---|---|
| `qemu_boot_safety3` | `#count ... 23` / `25 passed, 0 failed` | `test-results.txt`(作業ツリーに残っていた前回実行分) | 23 / 25 **一致** |
| `qemu_boot_load_gc_stress` | `#count ... 3`(本来 19) | `documents/load-path-contract.md:160-164` | 19 **一致** |
| `test-qemu-alloc-audit` | 19 / `21 passed` | `documents/heap-allocation-reasons.md:218` | 19 / 21 **一致** |
| `test-qemu-alloc-breakdown` | 5 / `7 passed` | `documents/heap-allocation-reasons.md:219` | 5 / 7 **一致** |
| `test-qemu-compare-single` | `53 passed, 1 failed`(= 54) | `documents/compare-type-specialized.md:165` | 54 **一致** |

数えられない / 要注意なもの:
- `defmacro` の本体にアサーションを置いているファイルを全件確認した
  (`isiki_test.lisp` 4 / `isiki_test_jit.lisp` 4 / `defmacro_test.lisp` 2 /
  `frame_definition_test.lisp` 1 / `jit_limits_test.lisp` 1)。
  **本体に入っているのは `test/lisp/defmacro_test.lisp:13` の 1 件だけ**で、
  そこは「呼ばれないこと」を見る身代わりであり、かつこのファイルはどの台本からも読まれていない
- 反復(`for` / `while`)の**本体**にアサーションがあるファイルは見つからなかった
  (`(assert-equal X (for ...))` の形は引数側なので 1 件のまま)
- 生成物: `test/lisp/isiki_test_jit.lisp`(1222 件)は `tools/gen_isiki_test_jit.py` の生成物。
  **現物をそのまま数えた**(生成器は走らせていない)
- ビルド切り替えで丸ごと飛ぶ: `test/lisp/gc_debug_guard.lisp:29-47` `require-gc-debug-build`

### 0-F 静的な見積りと記録が食い違う段(いずれも記録が古いことで説明がつく)

| 段 | 記録 | 日付 | 静的な見積り | 説明 |
|---|---|---|---|---|
| `qemu_boot_jit_frame` | `87 passed, 0 failed` | `docs/investigation/compiler-optimization.check.refs.md:19`(2026-10-30) | **241** | 記録後に `test/lisp/jit_mixed_frame_test.lisp`(136 件)が追加された |
| `qemu_boot_stack_guard` | `23 passed, 0 failed` | 同 `:20`(2026-10-30) / `documents/stack-guard.md:549` は `20 passed`(2026-10-07) | **26** | 記録後に 3 件増 |
| `qemu_boot_test`(`make test-qemu`) | `4,886 passed`(2026-09-26 `documents/unbound-arity-survey.md:110`)が最新 | — | **5225** | 基準値を保守している場所が無い |

→ **「本来の件数」の単一の真実源が存在しない。** これが机上で断定できない理由。

### 0-G 全段の静的な期待件数(1 周回せば全段を一度に点検できる表)

> **【訂正 2026-11-09 / #202 T1】この表の期待件数は 4 か所の数え方の誤りを含む。**
> 実機 1 周(29 段)と突き合わせた結果は `documents/test-count-census.md` にある。
> 誤りは (1) `assert-output` を 1 件と数えていた(集計を触らない入れ物マクロ)、
> (2) `tf-capture` の捕獲 19 件(走るが集計が復元される)、
> (3) 短絡して呼ばれない身代わり 4 件、(4) 台本自身のトップレベルのアサーションを
> 足していなかった、の 4 つ + 合計欄の算数 1 件。
> 主な訂正値: `qemu_boot_test` 5225 → **5178**、`m1_base` 2804 → **2759**、
> `m2_za` 679 → **678**、`m6_fat16` 157 → **155**、`fat32` 140 → **138**、
> `perf_read_fat16/32` 6 → **16**(`defun` × 呼び出し 3 回)。
> **表は消していない。** 数え方を間違えた場所の記録として残す(規則 6)。
> 数え直しは `python3 tools/census_test_counts.py --inventory` で導出できる(規則 13)。

| 段(台本) | 期待件数 | 証人 | `test-qemu-all` | CI |
|---|---|---|---|---|
| `qemu_boot_test.lisp` | 5225 | 全件あり | ○(`Makefile:1259`) | — |
| `qemu_boot_no_init_signal.lisp` | 15 | **自前集計** | ○(`:1260`) | — |
| (9P 無しの起動。C 側) | 8 | **件数が判定に書いてある** | ○(`:1261`) | — |
| `qemu_boot_load_disk.lisp` | 23 | あり | ○(`:1262`) | — |
| `qemu_boot_error_escape_gc.lisp` | 378 | あり | ○(`:1263`) | — |
| `qemu_boot_m5_ide.lisp` | 5 | **無** | ○(`:1264`) | ○ |
| `qemu_boot_m6_fat16.lisp` | 157 | **無** | ○(`:1265`) | ○ |
| `qemu_boot_fat_callback_transfer.lisp` | 17 | あり | ○(`:1266`) | ○ |
| `qemu_boot_load_gc_stress.lisp` | 21 | あり(単一フォーム) | ○(`:1267`) | — |
| `qemu_boot_fat32.lisp` | 140 | **無** | ○(`:1268`) | ○ |
| `qemu_boot_fat32_primary_boot.lisp` | 11 | **無** | ○(`:1269`) | ○ |
| `qemu_boot_partition.lisp` ×2 | 38 ×2 | **無** | ○(`:1270-1271`) | ○×2 |
| `qemu_boot_bench_jit.lisp` | 58 | あり | ○(`:1272`) | — |
| `qemu_boot_bench_interp.lisp` | 29 | あり | ○(`:1273`) | — |
| `qemu_boot_for_semantics.lisp` | 29 | あり | ○(`:1274`) | — |
| `qemu_boot_jit_frame.lisp` | 241 | あり | ○(`:1275`) | — |
| `qemu_boot_jit_limits.lisp` | 68 | あり | ○(`:1276`) | — |
| `qemu_boot_stack_guard.lisp` | 26 | あり | ○(`:1277`) | — |
| `qemu_boot_heap_exhaustion.lisp` | 42 | あり | ○(`:1278`) | — |
| `qemu_boot_compare_single.lisp` | 54 | あり | ○(`:1279`) | — |
| `qemu_boot_compare_fixnum.lisp` | 55 | あり | ○(`:1280`) | — |
| `qemu_boot_inline_muldiv.lisp` | 46 | あり | ○(`:1281`) | — |
| `qemu_boot_safety3.lisp` | 25 | あり | ○(`:1282`) | — |
| `qemu_boot_m1_base.lisp` | 2804 | **無** | — | ○ |
| `qemu_boot_m2_za.lisp` | 679 | あり | — | ○ |
| `qemu_boot_m3_za_ext5.lisp` | 399 | **無** | — | ○ |
| `qemu_boot_m4_environment.lisp` | 128 | **無** | — | ○ |
| `qemu_boot_fat16_m0.lisp` | 8 | **無** | — | ○ |
| `qemu_boot_declare_audit.lisp` | 12 | あり | — | — |
| `qemu_boot_alloc_audit.lisp` | 21 | あり | — | — |
| `qemu_boot_alloc_breakdown.lisp` | 7 | あり | — | — |
| `qemu_boot_m2_za_stress.lisp` | 254 | **無** | — | — |
| `qemu_boot_for_gc_stress.lisp` | 15 | あり(単一フォーム) | — | — |
| `qemu_boot_stack_guard_gc.lisp` | 9 | あり(単一フォーム) | — | — |
| `qemu_boot_read_file_native` / `imm_leak` / `for_expansion` / `bench_construct` / `perf_*`4 / `float_contagion_bench` | 15/15/8/20/13+13+6+6/15 | **無** | — | — |

再現コマンド(この表を作り直す道具は `/tmp` に置いただけでリポジトリには入れていない):
アサーション数は `(assert-equal|assert-error|assert-error-class|assert-float-close|assert-output`
の出現数(`;` 以降とコメントを除去、`#\;` の誤検出を回避)。

### 0-H 母艦の通しと CI で見ている試験ファイルの集合が違う

`qemu_boot_test.lisp` にあって CI のどの段にも無いファイル **18 本**:
`aot_leaf_gc_test` `convert_float_test` `declare_typed_add/sub/mul/div_test`
`declare_types_test` `divide_direct_call_test` `float_contagion_test` `float_default_test`
`float_math_contagion_test` `go_exit_reuse_test` `inline_decl_test` `nan_comparison_test`
`room_test` `single_float_test` `za_abi_probe_test` `za_code_imm_test`

逆に CI にあって `make test-qemu` に無いのは FAT / 区画 / 装置系 10 本(別の段で見ている)。

---

## §A 成果物は誰がどう書くか

| 項目 | 実装箇所 |
|---|---|
| 成果物を開く(= 空で作る)主体 | **Lisp の試験枠組み。** `test/lisp/test_framework.lisp:11` `(defglobal *isiki-test-stream* (open-output-file "/9p/test-results.txt"))` |
| タイミング | **枠組みを `load` した瞬間**(台本の先頭)。`test/lisp/test_framework.lisp:7-9` のコメントが「内容ファイル側に `load` を持たせない」理由として再オープン＝上書きを挙げている |
| 進捗の行 | `:46-56` `isiki-test-mark` → `"#at ~A t=~D += ~D~%"`(`:51`)。`finish-output` まで行う(`:54`) |
| 件数の行 | `:238` |
| 監査モードの 1 件ごとの行 | `:146-166` `isiki-audit-record` → `"#t ..."` |
| 合否の一行を作る唯一の箇所 | `:343` `"~%==== isiki tests: ~D passed, ~D failed ====~%"`(`isiki-test-report`、`:329-344`) |
| C 側は書くか | **書かない。** `src/c/*.c` に `test-results` への書き込みは無い(`grep -rn "test-results" src/c/` は `kernel.c:197` のコメントのみ) |

### A-1 途中で打ち切られても書かれる理由(3 段)

1. 成果物は枠組みの `load` 時点で既に存在する(`test_framework.lisp:11`)
2. `isiki-test-report` は台本の**独立した最上位フォーム**
   (例: `test/lisp/qemu_boot_load_gc_stress.lisp:9` が内容の `load`、`:11` が報告)
3. `os_load_from_stream` は**打ち切られたフォームを飛ばして次へ進む** —
   `src/c/load.c:101-120`(`aborted` を数えて `print_load_abort` し、ループを継続)。
   したがって台本の `:9` が打ち切られても `:11` は必ず走る

### A-2 今回の事故の経路(構造から確定)

- `test/lisp/load_gc_stress_test.lisp:69` `(defglobal *lgs-out* (open-output-file "/lgs/GCSTRES.LSP"))`
  が 2 回目の実行で失敗 → これは**アサーションの外**なので
  `test_framework.lisp:180-189` `isiki-test-begin` の中断検出(`*isiki-test-attempt*`)に乗らない
- ファイル全体が `:28` の 1 フォームなので以後の節が全部消える
- `isiki-file-end-check` は `n=3` を書き(`:238`)、`n > 0` なので通る(`:241`)
- 記録: `documents/load-path-contract.md:149-175`、`Makefile:1650-1668`

### A-3 「 0 failed」を grep している箇所(= 判定の口。**3 つに分かれている**)

| # | 箇所 | 対象 | 備考 |
|---|---|---|---|
| 1 | `tools/run_qemu_milestone.sh:115` | `test-results.txt` | `test-qemu-milestone` 経由の**全段** |
| 2 | `Makefile:1226-1228` | `test-results.txt` | **`test-qemu` は判定役を通らず自前に `test -f` + `grep`** |
| 3 | `Makefile:1177` | `tmp/.../no9p.serial.txt` | **`'[no9p] 8 passed, 0 failed'` と件数を固定**。`Makefile:1162-1163` が「件数を増やしたら数字も直すこと」と明記 |
| — | `Makefile:1419` | — | 監査実行(`test-qemu-audit-run`)は**意図的に検証しない** |
| — | `Makefile:1521` | — | ベンチのゲート 1 が同じ grep に依存(`tools/bench/bench_common.sh:42`) |

### A-4 合否の一行を**偽造している**箇所(判定を厳しくするとき直す対象)

| 箇所 | 内容 |
|---|---|
| `tools/mandel_run.lisp:22,47` | `"==== isiki tests: 1 passed, 0 failed ===="` を自分で書く |
| `tools/gen_jit_nest_probe.py:47-49` | 同上(生成する Lisp に書かせる) |
| `tools/jit_dump_identity.sh:198` / `tools/check_jit_dump_identity.py:383,395` | 自己試験の合成資料として `"3269 passed, 0 failed"` |

---

## §B 件数はどこまで出ているか

| 問い | 答え(事実) | 根拠 |
|---|---|---|
| 何を数えているか | **試験ファイル 1 本ごと。** その `isiki-test-load` の入口から出口までの `pass + fail` の増分 | `test_framework.lisp:73`(`before-count`)、`:237`(`n`) |
| どこへ出るか | **成果物のみ。** 画面にも直列線にも出ない | `:238` が `*isiki-test-stream*` 宛 |
| 成果物に入るか | **入る**(`isiki-test-load` 経由のファイルに限る) | 同上 |
| 照合している箇所 | **ゲスト側 1 つだけ**(`:241` の `n > 0`)。**母艦側は 0 件** | `grep -rn '#count' Makefile tools/ .github/` → `Makefile:1664`(コメント)のみ |
| ほかの `#` 行の消費者 | `#audit`/`#t`/`#file` → `tools/bench/parse_paint_audit.py:44-70`。`#GUEST` → `tools/check_macro_parity.sh:35`。`#gate` → `Makefile:1736-1739` | **`#count` を読む消費者だけが存在しない** |
| 件数の設計意図 | `test_framework.lisp:225-235` が「件数が唯一の証人だった」と書いている(由来は `documents/inline-arith.md` §8-1) | — |
| 棚卸しの記述 | `documents/records-and-measurement-rules.md:767` が「`#count` 行を出すので出力差分に出る」としている = **人間の目が消費者という前提** | — |

---

## §C 打ち切り件数は今どこまで届くか

| 項目 | 実装箇所 |
|---|---|
| 件数を返す関数 | `src/c/load.c:45-46` `os_load_from_stream(..., int *out_aborted_count)`。宣言は `src/c/load.h:36-37`、契約は `:25,31` |
| 加算する箇所 | `src/c/load.c:117-119`(`if (out_aborted_count != 0) (*out_aborted_count)++;`) |
| 理由の出力先 | `src/c/load.c:30-43` `print_load_abort` → **`proc->stdout_buffer`**(画面)。`src/c/process.h:24` で `frame_buffer *stdout_buffer` |
| **件数を使っている唯一の呼び元** | `src/c/kernel.c:160-180` `load_embedded_lisp`。`:165` で受け、`:170-174` で非 0 なら `os_panic`。意図は `:146-149` |
| 使っていない呼び元(すべて `0` を渡す) | `src/c/load.c:166`(9P 旧経路)/ `:183`(マウント→9P)/ `:214`(FAT 経由)。`:164-165` に「打ち切り件数は受け取らない」と明記 |
| 台本自身の読み込み | `src/c/kernel.c:95` `cc_load(...)` → 上の `cc_load` 経路 = **件数は捨てられる** |
| **Lisp 側から読む口** | **無い。** `grep -rn "aborted" src/c/*.c \| grep "cc_\|%%"` → 0 件。組み込み関数も診断関数も存在しない |
| 近い前例(件数を C で数えて使う) | `src/c/kernel.c:205-218` `g_smoke_pass`/`g_smoke_fail` + `smoke_ck`、`:318-320` で直列線へ `" passed, "` / `" failed"` |
| 試験枠組みが使う読み込み経路 | `test/lisp/test_framework.lisp:75` の `(load path)` → `cc_load`(`src/c/load.c:131-`)→ `os_load_from_stream(..., 0)`。**件数は届かない** |

### C-1 今回の事故で打ち切り件数は非ゼロだったはずか

**構造上そうなる(事実)。** ただし「数えられて捨てられた」のではなく
**ポインタが NULL なので加算自体が起きなかった**(`src/c/load.c:117`)。
内訳(いずれも `out_aborted_count == 0` の呼び出し):

1. `test/lisp/load_gc_stress_test.lisp` の読み込み … 打ち切り **1 件**
   (ファイル全体が 1 フォームなので最大 1)
2. 台本 `qemu_boot_load_gc_stress.lisp` の読み込み … **0 件**
   (内側の `load` は正常に `g_sym_t` を返すため)

`print_load_abort`(`src/c/load.c:30-43`)は**呼ばれており**、
理由は画面バッファに書かれている。これが #197 の「母艦に残らない」の実体。

### C-2 母艦へ出す経路の前例

| 経路 | 実装箇所 | 備考 |
|---|---|---|
| 直列線 | `src/c/interrupt.c:191` `os_diag_serial_write`。QEMU 側は `Makefile:1173` `-serial file:...` | **`test-qemu-no-9p` だけが `-serial` を付けている。`test-qemu-milestone`(`Makefile:1471-1484`)と `test-qemu`(`:1213-1223`)は付けていない** |
| 成果物 | 上の §A | Lisp 側からしか書けない |

---

## §D 先例としての ratchet(予算)

| 項目 | 実装箇所 |
|---|---|
| 基準値の置き場 | `tools/check_eval_error_budget.py:138` `BUDGETS = {...}`。**「ここだけを書き換える」と明記**(`:29`) |
| 更新のしかた | 下回ったとき `:322-327` が「`BUDGETS["<key>"]` を `<実数>` に下げること」と**次に書くべき数字をメッセージに出す** |
| 双方向 | `:316`(超過で NG)と `:321`(未達で NG)。方針は `:22-26` |
| 自己試験 | `:376-409` `self_test()`。`:396-406` が**実数 ±1 の予算で必ず落ちること**まで見る |
| 門番 | `Makefile:376-377`(本体)/ `:384-385`(自己試験)。**両方 `make test` に入っている**(`Makefile:671`) |
| **文書の記述が古い** | `documents/records-and-measurement-rules.md:767` は「`make test` も CI もこれ(自己試験)を呼んでいない」とするが、**`Makefile:671` に `check-eval-error-budget-self-test` が入っている**(現状は呼ばれている) |

### D-1 「件数が減ったら落とす」への転用可否

**形はそのまま使える(双方向の ratchet + 自己試験 + 次の数字を出す)。**
ただし**入力の出自が違う**ので、そこだけ別の前例に寄せる必要がある:

| | `check_eval_error_budget.py` | 件数の ratchet |
|---|---|---|
| 観測値の出自 | **母艦のソース走査**(QEMU 不要・即時) | **ゲストの実行成果物**(QEMU 1 周が要る) |
| 基準値の保守 | 本人が下げるだけ | 試験を増やすたびに増える = 更新が頻繁 |

→ **出自が「ゲストの成果物 + リポジトリの基準値ファイル」の前例がすでにある**:

| 前例 | 実装箇所 | 性質 |
|---|---|---|
| ベンチの基準値 | `tools/bench/check_bench_baseline.py`(`:44` `BASELINE_COLS = 10`)+ `tools/bench/bench_baseline.tsv` | **双方向**(`:14-19` が理由を書いている)。帯つき。`revision`/`date` 列を持つ = **数字に出自を持たせる**という規則 |
| 件数を判定に直書き | `Makefile:1177` (`'[no9p] 8 passed, 0 failed'`) | **現存する唯一の件数照合**。手で直す前提(`Makefile:1162-1163`) |
| 行番号の期待値 | `tools/check_jit_bail_lines.py` + `Makefile:410-412`(本体と自己試験を両方呼ぶ) | `--self-test` あり |
| フレーム項目の数え上げ | `tools/check_frame_access_entries.py:158` `self_test()` / `Makefile:584-588` | `make test` に入っている(`:671`) |
| 再試行の仕組みの自己試験 | `tools/run_qemu_milestone.sh:47-96` / `Makefile:545-546` `check-qemu-retry-self-test` | `make test` に入っている(`:671`)。**4 経路すべてを意図的に発火させる**形 |

ratchet 形の仕掛けの全体一覧は `documents/records-and-measurement-rules.md:752-775` の表にある。

---

## §E いま壊れている段 → §0 を参照

追加の材料:

| 項目 | 実装箇所 |
|---|---|
| 2 回目の実行で変わる段の対処(唯一) | `Makefile:1670` `rm -f $(FAT16_DISK_IMG)`。理由は `:1655-1668` |
| イメージを消しているのはほかに 2 箇所だけ | `Makefile:1258`(`test-qemu-all` の先頭)、`Makefile:1336`(性能計測) |
| イメージはあれば作り直されない | `Makefile:1471` `test-qemu-milestone: build $(QEMU_DISK_IMG) ...`(前提条件なので存在すればそのまま) |
| FAT マウント先へ書く試験(= 2 回目で失敗しうる) | `test/lisp/fat16_test.lisp:446` / `fat32_test.lisp:390` / `fat_callback_transfer_test.lisp:40,60,86` / `perf_fat16_test.lisp:37` / `perf_fat32_test.lisp:23` / `load_gc_stress_test.lisp:69` |
| そのうち段がイメージを作り直すもの | `perf_*` のみ(`Makefile:1336`)、`load_gc_stress`(`:1670`)、`test-qemu-all` 経由(`:1258`) |
| **作り直さない段**(単体で 2 回走らせると変わる) | `Makefile:1265`(m6_fat16)/ `:1266`(fat_callback_transfer)/ `:1268`(fat32)/ `:1327`(read_file_native)。**ただし最上位フォームが多数に分かれているので、縮む前に NG として鳴る見込み ★ 未検証** |
| 9P(母艦共有)へ書く試験は安全 | `za_test.lisp` の `/9p/tmp/ckpt-*`、`isiki_test.lisp:1201-1370` 等。母艦の FS が上書きする |
| 過去に同族で壊れていた記録 | `documents/records-and-measurement-rules.md:787-812`(`test-qemu-stack-guard-gc` が常に 1 件 NG。原因は `gc_debug_guard.lisp` を `isiki-test-load` で読んでいたこと)。修正コミット `3b79ddf` |
| 検出器の棚卸し | `documents/records-and-measurement-rules.md:740-786` |

---

## §F 判定の口

| 項目 | 実装箇所 |
|---|---|
| 判定役 | `tools/run_qemu_milestone.sh`。判定は `:113-124` |
| 結果あり・合格 | `:115-117`(`grep -q " 0 failed"` → `exit 0`)← **条件を足すならここ** |
| 結果あり・失敗 | `:118-123`。**再試行しない**(`:119` のコメント) |
| 結果なし | `:126-146`。`MAX_ATTEMPTS`(既定 2、`:101`)まで再試行し、`:140` で `tmp/qemu_flakes.txt` に追記 |
| QEMU のコマンド行の唯一の真実源 | `Makefile:1471-1484`。スクリプトは第 2 引数で受けるだけ(`:40-44`) |
| 自己試験 | `:47-96`。4 経路(結果なし 2 回 / 結果あり失敗 / 1 回目で成功 / 2 回目で成功)。`Makefile:545-546`、`make test` に入っている(`:671`) |
| **判定役を通らない口** | `Makefile:1226-1228`(`test-qemu`)。`Makefile:1177`(`test-qemu-no-9p`) |
| ミルストンを呼ぶ側 | `Makefile:1257-1282`(`test-qemu-all`)、`.github/workflows/ci.yml:35-82` |

### F-1 再試行との関係(設計上の論点)

現在の分岐は **「結果の有無」** で切れている(`:113`)。
件数の条件を `:115-117` の中に足すと:

- **「結果はあるが件数が足りない」は `:118-123` の「本物の失敗」側に落ちる = 再試行されない。**
  これは今の方針(`:29-32`「失敗を再試行で隠してはいけない」)と整合する
- ただし `:144` の `[FLAKE]` 記録は `:126` 以降(結果なし)でしか積まれないので、
  **件数不足は flake 記録に入らない**。記録の意味は変わらない
- 自己試験(`:76-83`)は 4 経路を合成資料で発火させている。
  件数の条件を足すなら **5 つ目の経路(結果あり・不合格 0・件数不足 → 落ちる)**を
  同じ形で足せる。合成資料は `printf` で `test-results.txt` を作っているだけなので
  **QEMU 不要で書ける**(`:78-83`)
- `:82` の既存の合成資料は `#count` 行を持たないので、**件数を必須にすると
  自己試験の資料 3 本と §A-4 の偽造 3 箇所が全部落ちる。** 移行のしかたは設計判断
