# 参照マップ —— どこからも走らない段(#208)

実装担当・レビュー担当向け。論理要約 `docs/investigation/never-run-stages.logic.md` の
節番号に対応する実装箇所を置く。基点 `419a305`(= `main`)。**コードは変更していない。**

---

## 0. 計器の記号 ↔ 実物の対応(論理要約の全節で使う)

| 記号 | 台本 | 本体(読み込まれるファイル) | 件数(導出) |
|---|---|---|---:|
| 計器 1 | `test/lisp/qemu_boot_abi_bench.lisp` | `test/lisp/abi_bench.lisp` | 4 |
| 計器 2 | `test/lisp/qemu_boot_declare_bench.lisp` | (入口のみ) | — |
| 計器 3 | `test/lisp/qemu_boot_diag_m12_host.lisp` | (台本自身。10 行) | 1 |
| 計器 4 | `test/lisp/qemu_boot_inline_phase0.lisp` | (台本自身。154 行) | 6 |
| 計器 5 | (入口 = 計器 2) | `test/lisp/declare_types_bench.lisp` | 19 |
| 計器 6 | `test/lisp/qemu_boot_inline_phase0b.lisp` | (台本自身。127 行) | 5 |
| 計器 4 の後続 | `test/lisp/qemu_boot_inline_phase3.lisp` | (台本自身。138 行) | 6 |
| 計器 7 | `test/lisp/qemu_boot_instcount_gate_check.lisp` | (台本自身。76 行) | 5 |

件数の出どころ(手で数えていない。規則 13):

```sh
python3 tools/census_test_counts.py --inventory          # 合計 46
```

出どころの棚卸し: `documents/test-count-census.md:226`(§5-1 の表)、
`documents/test-count-census.md:367,376,377,390-393`(§6 の棚卸し)、
`documents/test-count-census.md:456-462`(§7-2)。

---

## §0-1 計器 3 は誤検出 —— 実際に走っている

| 事実 | 箇所 |
|---|---|
| 母艦側の単体試験が直接読み込む | `test/c/script_test.c:332` |
| その単体試験のソース集合 | `Makefile:190`(`TEST_SRC_SCRIPT`)、`Makefile:191`(`TEST_BIN_SCRIPT`) |
| ビルドと実行 | `Makefile:783`(ビルド)、`Makefile:852`(docker で実行) |
| `make test` の前提に入っている | `Makefile:738`(`$(TEST_SRC_SCRIPT)`) |
| 継続的検査が `make test` を呼ぶ | `.github/workflows/ci.yml:22` |

誤検出の原因:

| | 箇所 |
|---|---|
| 「どこで走るか」の導出が 2 経路だけ | `tools/census_test_counts.py:379-439`(`where_it_runs`)。`Makefile` と `.github/workflows/ci.yml` だけを読む(`:386-387`) |
| 名前が `Makefile` に無ければ「Makefile/CI に無い」と断定 | `tools/census_test_counts.py:437-439` |
| 直すべき場所 | 同関数に第 3 の起点として `test/c/*.c` の `run_lisp_file(...)` を足す(`test/c` 全体で 10 箇所) |

---

## §0-2 ビルド定義と継続的検査の全履歴に一度も無い

実行したコマンド(全 686 コミット。`git rev-list --count HEAD` = 686):

```sh
for n in qemu_boot_abi_bench qemu_boot_declare_bench qemu_boot_inline_phase0 \
         qemu_boot_inline_phase0b qemu_boot_inline_phase3 \
         qemu_boot_instcount_gate_check qemu_boot_diag_m12_host; do
  git log --oneline -S"$n" -- Makefile
  git log --oneline -S"$n" -- .github/
done
```

**7 本すべてで出力 0 行。** 陽性対照(検索自体が効いていること):

```sh
git log --oneline -S"qemu_boot_inline_muldiv" -- Makefile   # => eed5639
```

---

## §0-3 / §9-4 計器 5 は今走らせると落ちる

| 事実 | 箇所 |
|---|---|
| 長さの一致を固定している 3 件 | `test/lisp/declare_types_bench.lisp:140-142` |
| 当時の前提(宣言は生成コードを変えない) | `test/lisp/declare_types_bench.lisp:5-11`、`documents/declare-types.md:417-428`(`code-len=735` 対 `735`) |
| 文書自身の予告 | `documents/declare-types.md:530-531`「最適化を入れたらここに差が出るはずである」 |
| **前提を崩した分岐** | `src/c/za.c:3345-3346`(宣言なし = `primitive_add2` / `primitive_subtract2` → `is_add`/`is_sub` が**無条件に真** = 展開される)対 `src/c/za.c:3365-3370`(宣言つき = `primitive_add2_fixnum` → `za_inline_enabled(INLINE_BIT_ADD)` が無いと展開されない) |
| その差の実測値 | `documents/inline-arith.md:1063-1068`。`+fix-nodecl` 21.94 対 `+fix-decl` 62.94 / `-fix-nodecl` 22.94 対 `-fix-decl` 75.94(per-call 命令数) |
| 生成コード長の差 | `test/lisp/qemu_boot_inline_phase3.lisp:71`「inline と nodecl の一致、decl との差 45」 |
| 対象の関数 | `test/lisp/declare_types_bench.lisp:86-97`(`dtb-fix-d` は `-`/`+`/`*`/`=` を各 1 回。`+` と `-` の 2 箇所で各 45 byte の差 = 約 90 byte ずれる見込み ★) |

**落ちると見た 3 件:** `:140`(fixnum)、`:141`(single)、`:142`(double)。
`:141`/`:142` も `(- n 1)` の `n` が `<fixnum>` 宣言つきなので同じ差が乗る ★。

**通ると見た 16 件:** `:120-128`(`%%za-compiled-p` 9 件)、
`:131-134`(`%%declared-types-of` 4 件)、`:150-152`(値の一致 3 件)。

---

## §1 調査範囲 —— 読んだもの

- 台本 7 本と本体 2 本(全文)
- `Makefile`、`.github/workflows/ci.yml`、`tools/run_qemu_milestone.sh`、
  `tools/plugins/isiki_instcount.c`(コメントのみ)
- `tools/census_test_counts.py`、`tools/check_test_count_ratchet.py`
- `src/c/za.c`(`za_emit_arith_call_or_inline` 周辺)、`src/c/bench_subprimitive.c`、
  `src/c/runtime.c`(登録表)、`src/c/disasm_lisp.c`、`src/c/kernel.c`(`run_boot_entry`)
- `documents/` 7 本(下記 §13)
- `git log --follow` / `git log -S` / `git show`(読み取りのみ)

**走らせていないもの:** `make build` / `make test` / `make test-qemu*` / QEMU / docker。

---

## §2 ドメイン概念 ↔ 実装

| 概念 | 実装 |
|---|---|
| 段 | `make test-qemu-milestone MILESTONE=...`(`Makefile:1590-1604`) |
| 自動の周回 | `Makefile:1360-1420` 付近の `test-qemu-all` 系(段の並びの真実源) |
| 継続的検査の段 | `.github/workflows/ci.yml:42-89` |
| 手で呼ぶ段 | `Makefile:617-618`(宣言の監査)、`:636-645`(確保の監査)、`:1441-1442`(性能)など |
| 既定の台本(名前が Makefile に無い経路) | `src/c/kernel.c:91`(`g_qemu_test_boot_script`)、`:95`(`cc_load`) |
| 計測の 2 段構え | `Makefile:1852-1869`(`test-qemu-instcount-gate`)。パス 1 = `:1855`、パス 2 = `:1859-1861` |
| 区間なしの計測 | `Makefile:1838-1839`(`test-qemu-instcount`) |
| 割り込みの補正定数 | `Makefile:1619`(`ISR_INSNS_PER_TICK ?= 914`) |
| 補助部品(版管理外) | `Makefile:1613`(`INSTCOUNT_PLUGIN`)、`:1621` 以降(docker でビルド) |
| 区間の印 | `src/c/bench_subprimitive.c:429-442`(`os_bench_gate_open`/`cc_gate_open`)、`:455-457`(`cc_gate_hits`)、`:469-473`(`cc_gate_plugin_args`)、登録は `:515-523` |
| 件数の名簿(段) | `tools/check_test_count_ratchet.py:173`(`COUNTS`) |
| 件数の名簿(ファイル) | 同 `:254`(`FILES`)。書式は `:249` |
| 証人の静的検査 | 同 `:943-1010`(`check_witness_sources`)。入口は `Makefile:407-409`(`check-test-count-witness`) |
| 名簿自身の検査 | 同 `:1024-1047`(`check_roster`)。0 件の段を認めない = `:1041-1046` |
| 読み取り方の表 | 同 `:721`(`READERS`)。知らない読み方で落とす = `:1032-1037`(**メッセージに #208 と書いてある**) |
| 素の読み込みの例外表 | 同 `:519`(`RESOURCES`)。理由が空なら落とす = `:1006-1012` |
| 読み込みの正規表現 | 同 `:934`(`SCRIPT_LOAD_RE`) |
| 枠組みの除外 | 同 `:539`(`FRAMEWORK`) |
| 段 → 台本 | 同 `:112`(`STAGE_SCRIPTS`) |

---

## §3 責務の分担 —— 「名簿に載らない段は見えない」の根拠

| 主張 | 箇所 |
|---|---|
| 名簿が段の在庫の権威 | `documents/test-count-census.md:466-480`(§7-3 の表。「段が走らなかった場合: 以前は**何も出ない**(#208)」) |
| 証人の静的検査は `COUNTS` を回るだけ | `tools/check_test_count_ratchet.py:957`(`for stage, _count, reader, _rounds in COUNTS:`) |
| 数え上げの道具は検査に組み込まれていない | `documents/test-count-census.md:482-490`(§7-3 末尾) |

---

## §4 処理の流れ

| 段階 | 箇所 |
|---|---|
| 合否の一行の判定(1 箇所に寄せた) | `tools/run_qemu_milestone.sh` 本体の `check_pass_line.py --verify` 呼び出し(ファイル末尾のループ内)、`tools/check_pass_line.py` |
| 読み込みの打ち切りの判定 | 同ループ内の `check_load_abort_line.py --verify`。ログの指定は `Makefile:1532`(`LOAD_ABORT_LOG`)、`:1529`(`QEMU_SERIAL_FLAG`) |
| 結果が無いときだけ 1 回やり直す | `tools/run_qemu_milestone.sh`(冒頭の設計コメント + 末尾のループ) |
| 1 周の最後に名簿と突き合わせる | `tools/check_test_count_ratchet.py:1205`(`run`)、呼び出しは `Makefile` の `test-qemu-all` 末尾 |
| **パス 1 は普通の 1 段と同じ**(§7-2 の根拠) | `Makefile:1855`(`$(MAKE) test-qemu-milestone MILESTONE=$(MILESTONE)`。プラグイン指定なし) |
| パス 1 が落ちれば make が止まる | 同行に `-` が無い(= エラーを無視しない) |

---

## §5 各計器の中身(行番号つき)

### 計器 1 —— `test/lisp/abi_bench.lisp`

| 中身 | 行 |
|---|---|
| 目的(呼び出し規約刷新の前後比較の基盤) | `:1-22` |
| 計測の本体(確保量 / 回収回数) | `:27-38` |
| 3 シナリオ(床 / 高速路 / 一般路) | `:44-66` |
| **合否 4 件**(機械語に落ちていること) | `:69-72` |
| 記録(n=500) | `:78-80` |
| 入口 | `test/lisp/qemu_boot_abi_bench.lisp:1-5` |

使う口: `%%heap-used-bytes`(`src/c/runtime.c` 登録 / `src/lisp/transpile.lisp:230`)、
`%%gc-collect-count`(`src/c/runtime.c:3946`)、`%%za-compiled-p`(`:3888`)、`DIV`(`:3850`)。

### 計器 4 —— `test/lisp/qemu_boot_inline_phase0.lisp`

| 中身 | 行 |
|---|---|
| 目的(展開前の基準値) | `:1-9` |
| **「補正表と 1 対 1」の記述(指す先が無い)** | `:9` |
| 測る関数 17 本 | `:16-36` |
| 反復数 `*p0-n* = 200000` / 暖機 2000 | `:43-44` |
| 暖機(区間の外) | `:48-61` |
| **合否 3 件**(機械語に落ちていること) | `:64-66` |
| **合否 1 件**(`%%current-inline` が nil) | `:68` |
| 区間の前に回収を起こす | `:73-76` |
| 区間 25 本 | `:94-124` |
| 確保量 8 本 | `:134-141` |
| **合否 2 件**(印の通過回数 26) | `:151-152` |

### 計器 6 —— `test/lisp/qemu_boot_inline_phase0b.lisp`

| 中身 | 行 |
|---|---|
| 目的(追測。3 つの疑い) | `:1-8` |
| 呼び先の検出(逆アセンブルのコメント照合) | `:14-20` |
| **合否 4 件**(型ごとの呼び先の選び方) | `:49-52` |
| 累積して計算が消えていないことを見る | `:55-85` |
| 区間 12 本 | `:111-123` |
| **合否 1 件**(印の通過回数 13) | `:125` |

### 計器 4 の後続 —— `test/lisp/qemu_boot_inline_phase3.lisp`

| 中身 | 行 |
|---|---|
| 目的と実装前に宣言した期待値 | `:1-10` |
| 測る関数 17 本(`*`/`/` の inline は `:29-32`、比較は `:38-43`) | `:16-43` |
| **絶対値で固定した生成コード長 3 件** | `:74-76` |
| その 3 件の由来(+181 の記録) | `:68-73` |
| **値の一致 2 件** | `:77-78` |
| 区間 19 本 | `:96-114` |
| **合否 1 件**(印の通過回数 20)と更新の履歴 | `:128-136` |

### 計器 7 —— `test/lisp/qemu_boot_instcount_gate_check.lisp`

| 中身 | 行 |
|---|---|
| 目的(計器自身の合格判定。陰性対照 / 陽性対照の分担) | `:1-12` |
| パス 1 が取りに来る行 | `:18` |
| **合否 2 件**(機械語に落ちていること) | `:36-37` |
| **合否 1 件**(値) | `:38` |
| 区間 3 本(空 / 床 / 本命 / 再測) | `:55-67` |
| **合否 2 件**(印の通過回数 4) | `:71-72` |

### 計器 5 —— `test/lisp/declare_types_bench.lisp`

| 中身 | 行 |
|---|---|
| 目的と前提(宣言は生成コードを変えない) | `:1-17` |
| コンパイル時だけを測る(`*dtb-compile-n* = 1000` × 5 種) | `:30-76`(`:53` が n) |
| 測る関数(宣言あり / なし / 対照) | `:86-117` |
| **合否 9 件**(機械語に落ちていること) | `:120-128` |
| **合否 4 件**(宣言が記録されている) | `:131-134` |
| **合否 3 件(落ちる見込み)**(長さの一致) | `:140-142` |
| **合否 3 件**(値の一致) | `:150-152` |
| 実行時の反復数 `*dtb-run-n* = 1000000` / double は 200000 | `:174-176` |
| 9 回の計測 | `:178-186` |

### 計器 3 —— `test/lisp/qemu_boot_diag_m12_host.lisp`

全 10 行。目的は `:1-6`、**合否 1 件**は `:10`。

---

## §5-2 不変条件の根拠

| 不変条件 | 箇所 |
|---|---|
| 区間数の期待値は台本が持つ | `qemu_boot_inline_phase0.lisp:151-152` / `_phase0b.lisp:125` / `_phase3.lisp:136` / `instcount_gate_check.lisp:71-72` |
| 区間を足して期待値を直し忘れて落ちた(実例) | `documents/compare-type-specialized.md:146-151`、`qemu_boot_inline_phase3.lisp:128-135`(12→16→18→20 の履歴) |
| 区間の並びは出てきた順で対応付ける | `Makefile:1867-1874`(awk。`ins[j++]`) |
| 区間の中の回収 | `qemu_boot_inline_phase0.lisp:70-76` |
| 暖機は区間の外 | `qemu_boot_inline_phase0.lisp:46-48`、`instcount_gate_check.lisp:32-35` |
| 刻み 1 回 = 914 命令(実測) | `documents/performance-measurement.md:3697-3718`、`Makefile:1614-1619` |
| 整数の宣言は非負を意味しない | `documents/compare-type-specialized.md:131-136`、`qemu_boot_inline_phase3.lisp:35-37` |
| **宣言なしが展開され、宣言つきは展開されない** | `src/c/za.c:3345-3346` 対 `:3365-3370`(コメントは `:3357-3360`) |

---

## §6-2 仕事の状態

| 計器 | 根拠 |
|---|---|
| 計器 1 | `documents/abi-redesign.md:320-329`(ABI-M0〜M8 すべて「完了」、M9+ は「未着手」)。最終値の記録は `:1033`、`:1053` |
| 計器 4 | `documents/inline-arith.md:207`(Phase 0 の手順)。展開は Phase 3 で入った(`36cdad6`) |
| 計器 6 | `documents/inline-arith.md:353`。出自 `66aca32` |
| 計器 4 の後続 | 更新の履歴: `36cdad6`(2026-09-23 新設)→ `a5ffe3e`(2026-10-30 絶対長 +181)→ `eecd4dd`(2026-11-03 比較 single、区間 +4)→ `b7c1342`(2026-11-06 比較 fixnum、区間 +2)→ `eed5639`(2026-11-07 `*`/`/` の inline、区間 +2)。測定の記録は `documents/inline-arith.md:1050-1090`、`documents/compare-type-specialized.md:100-130`、`documents/heap-exhaustion.md:581-597` |
| 計器 5 | `documents/declare-types.md:415-443`(当時の記録)、`:530-531`(予告) |
| 計器 7 | `documents/performance-measurement.md:3628-3726`。`documents/records-and-measurement-rules.md:764`(陽性対照は別ビルドが要るのでこのファイルは受け持たない) |
| 計器 3 | 出自 `dc660f0`(2026-08-23)。`test/c/script_test.c:332` |

---

## §7 出入り口

| | 箇所 |
|---|---|
| 成果物 | `test-results.txt`(ゲストが書く)。パス 1 が `#gate` 行を拾う = `Makefile:1856-1858` |
| 直列線のログ | `Makefile:1529-1532` |
| 補助部品 | `tools/plugins/isiki_instcount.so`(**版管理外**)。ビルドは `Makefile:1621` 以降。ソースは `tools/plugins/isiki_instcount.c:60-68`(印の約束)、`:125-136`、`:225` |
| 基盤ライブラリの読み込み(逆アセンブラが使えること) | `src/c/kernel.c:351-360`(`run_boot_entry`)。`disasm-items` は `src/lisp/disassemble.lisp:50`、`disasm-item-comment` は `:28` |

---

## §7-2 「量は手のまま、合否だけ自動に入れる」の根拠

- パス 1 = `Makefile:1855`。プラグイン指定が無く、`test-qemu-milestone` と同一
- 印の通過回数はゲスト側の C が数える(`src/c/bench_subprimitive.c:425-430,455-457`)。
  **プラグインとは独立**なので、補助部品が無くても `%%gate-hits` の合否は出る
- 生成コード長は `%%DISASM-CODE-LEN`(`src/c/disasm_lisp.c:355`)。同じく独立

---

## §8 制約

| 制約 | 箇所 |
|---|---|
| 名簿に段を足すことになる | `tools/check_test_count_ratchet.py:173`(`COUNTS`)、`:254`(`FILES`)、`:249`(書式 = 台本自身の件数 + 項目) |
| 0 件の段を認めない | 同 `:1041-1046` |
| 台本自身の件数だけの段は表現できる | 同 `:255`(`test-qemu` が台本自身 1 件)、`:317`(台本自身 0 件 + 証人 15) |
| 素の読み込みの禁止と例外表 | 同 `:969-991`、`:519`(`RESOURCES`) |
| 枠組みの読み込みは例外 | 同 `:973-974`、`:539` |
| → 計器 4 / 6 / 4 の後続 / 7 は項目が台本自身なのでそのまま載る | 各台本の `(load "test/lisp/test_framework.lisp")` 1 行だけ(`phase0.lisp:11` ほか) |
| → 計器 1 / 5 は本体を別ファイルから素の `load` で読む | `qemu_boot_abi_bench.lisp:2`、`qemu_boot_declare_bench.lisp:2`。`isiki-test-load` へ寄せると**件数 +2**(`test/lisp/test_framework.lisp:265-269`) |
| 1 周 12 分の記録 | `documents/test-count-census.md:493-497`(§7-4) |
| 重い段(14〜25 分)の族 | `documents/test-count-census.md:222`(§5-1) |

---

## §9-2 重さの桁(導出)

| 導出に使った値 | 箇所 |
|---|---|
| 1 区間 = 20 万回 | `phase0.lisp:43`、`phase0b.lisp:62`、`phase3.lisp:47`、`instcount_gate_check.lisp:29` |
| 区間数(台本が宣言している値から 空の区間 1 を引く) | 26→25 / 13→12 / 20→19 / 4→3 |
| 1 区間 ≒ 1.78〜1.86 億命令・276〜304 刻み | `documents/performance-measurement.md:3700-3705` |
| 床 `f-id` = 178,419,377 命令 | `documents/inline-arith.md:1062` |
| 倍精度の区間は重い(per-call 475.95 対 21.94) | `documents/inline-arith.md:1072`、`documents/compare-type-specialized.md:107-110` |
| 起動の基底 ≒ 11 億命令 | `documents/performance-measurement.md:3700` 付近(設計3 の説明) |
| 計器 5 の刻み(fixnum 252 / single 540) | `documents/declare-types.md:518-521` |
| 1 刻み = 10ms | `test/lisp/declare_types_bench.lisp:13-14`、`src/c/clock.c`(`TICKS_PER_SECOND`) |

---

## §9-3 重複の根拠 —— 常時走る段が同じことを見ている

| 走らない計器の項目 | **常時走る段にある同等以上のもの** |
|---|---|
| 計器 4 `:68`(`%%current-inline` が nil) | `test/lisp/inline_decl_test.lisp:33`(**同じ式**)。加えて `:34-41` で declaim の増減も見る |
| 計器 6 `:49-52`(型ごとの呼び先) | `test/lisp/inline_decl_test.lisp:253-258`、`:294`(single も)/ `test/lisp/declare_typed_mul_test.lisp:45-55`(**double → generic も `:54`**)/ `test/lisp/declare_typed_sub_test.lisp` / `_div_test.lisp` |
| 計器 5 `:131-134`(宣言の記録) | `test/lisp/declare_types_test.lisp`(`%%declared-types-of` 23 件。`:43`,`:47`,`:50` ほか) |
| 計器 5 `:140-142`(長さの相対比較) | `test/lisp/compare_single_test.lisp:71`(相対比較。`test-qemu-compare-single` 段)/ `test/lisp/declare_audit_test.lisp:43-55`(ただしこちらは別ビルド設定の手で呼ぶ段) |
| 計器 4 `:64-66` / 計器 7 `:36-37`(機械語に落ちていること) | 多数(`za_test.lisp` ほか) |
| **覆われていないもの** | 計器 4 の後続 `:74-76`(**絶対値の生成コード長**)/ 計器 7 の区間の再現性(`:65-67` の 2 回測定)/ 計器 1 の確保量の記録 |

どの段が常時走るか: `test/lisp/inline_decl_test.lisp` と
`test/lisp/declare_types_test.lisp` と `test/lisp/declare_typed_mul_test.lisp` は
`test/lisp/qemu_boot_test.lisp` が読む(= `make test-qemu`。自動の周回の 1 段目)。

---

## §9-5 存在しない「補正表」

```sh
git log --oneline -S"補正表" -- Makefile   # => 0 行
grep -n "補正表" Makefile                  # => 0 行
```

言及元は `test/lisp/qemu_boot_inline_phase0.lisp:9`。
いまの集計は `Makefile:1867-1874` の awk が `#seg` の出現順で対応付けている。

---

## §9-6 試験ディレクトリ全体の棚卸し(機械的)

やり方(読み取りのみ。再現できる形で残す):

```python
# 起点 = Makefile / .github/workflows/ci.yml / test/c/*.c / src/c/*.c / tools/ / src/lisp/
# $(TESTDIR) -> test/c、$(SRCDIR) -> src/c を展開してから、名前の出現を推移的に辿る
```

`git ls-files test/` = **190 本**。辿れなかったのは **7 本**:

| 辿れなかったもの | 判定 |
|---|---|
| `test/lisp/qemu_boot_declare_bench.lisp` | **本物**(未配線) |
| `test/lisp/declare_types_bench.lisp` | **本物**(上の本体) |
| `test/lisp/qemu_boot_inline_phase0.lisp` | **本物** |
| `test/lisp/qemu_boot_inline_phase0b.lisp` | **本物** |
| `test/lisp/qemu_boot_inline_phase3.lisp` | **本物** |
| `test/lisp/qemu_boot_instcount_gate_check.lisp` | **本物** |
| `test/c/test_assert.h` | **偽陽性。**同ディレクトリ内の `#include "test_assert.h"`(パス接頭辞なし)で読まれている |

**偽陰性も 1 件出た:**
`test/lisp/qemu_boot_abi_bench.lisp` と `test/lisp/abi_bench.lisp` は
「辿れた」と出るが、辿れた理由は **`src/c/runtime.h` のコメントに名前が書いてある**
だけで、**実行はされていない**。

> 再発防止の検査は **(a) コメントを剥がす (b) 起点の種類を名簿で明示する
> (c) 同ディレクトリ内の相対参照を扱う** の 3 つが必要である(§12-2)。

その他、`test/c` 経由でのみ辿れるもの(名簿の対象外。§9-7):
`test/lisp/defmacro_test.lisp` / `environments_predefined_test.lisp` /
`init_test.lisp` / `rest_test.lisp` / `square_test.lisp` /
`qemu_boot_diag_m12_host.lisp`(← `test/c/script_test.c`)、
`heap_exhaustion_test.lisp`(← `test/c/runtime_test.c`)、
`io_signal_test.lisp` / `load_gc_stress_test.lisp`(← `test/c/load_test.c`)、
`toplevel_abort_test.lisp`(← `test/c/repl_test.c`)、
`test/c/mount_test.c`(← `test/c/load_test.c`)。

---

## §9-1 文書が穴を 1 度記録している

`documents/inline-arith.md:1085-1090`:
> `qemu_boot_inline_phase3.lisp` の静的コード長の期待値 3 件は、**+181 byte ずれていた**
> …… **この boot ファイルは `test-qemu-all` に入っていない**ので全回帰では拾えず、
> この計器を回したときに初めて落ちた。**期待値が「全回帰の外」に置かれている形**は、
> 検出器の網羅性の穴として記録しておく。

同じ趣旨: `documents/test-count-census.md:456-462`(§7-2)。

---

## §11 絶対値 3 件が「通る見込みが高い」とした根拠(★)

| 根拠 | 箇所 |
|---|---|
| 2026-10-30 に +181 へ直した | `a5ffe3e`(06:20 UTC)。`qemu_boot_inline_phase3.lisp:68-76` |
| その直後に生成器のフレーム寸法が関数ごとになった | `f6722af`(08:30 UTC)。`a5ffe3e` の**子孫**(`git merge-base --is-ancestor a5ffe3e f6722af` = YES) |
| **しかし 2026-11-01 にこの計器を回し、10 区間すべて ±0.00 の記録がある** | `documents/heap-exhaustion.md:581-597`。パス 1 は失敗で止まるので、**この時点で 3 件は通っていた** |
| 2026-11-03 にも回している(区間数の期待値だけが落ちた) | `documents/compare-type-specialized.md:100-130`、`:146-151` |
| それ以降に生成器を触ったのは 9 コミット。うち算術の出力に関わるのは 2 つ | `b7c1342`、`eed5639`、`cf605b1`(安全度 3。**既定では 1 命令も増えないと明記** = `src/c/za.c:3374-3383`) |
| `+`/`-` の出力は変えないと明記 | `src/c/za.c:3487-3490`(「add / sub の経路は 1 命令も変えない」)、`:3520-3522`(`if (is_add || is_sub)` で囲んだ) |

★ それでも実行していないので未検証である。

---

## §12 再発防止案の置き場所(提案。実装していない)

寄せる先の既存の作法:

| 真似る点 | 既存の箇所 |
|---|---|
| 名簿は 1 箇所 / 両方向で落ちる / 次に書く数字を出す | `tools/check_test_count_ratchet.py:30-40`(作法の宣言) |
| 知らない種別で落とす | 同 `:1032-1037`(`READERS` に無い読み方) |
| 理由の無い例外を認めない | 同 `:1006-1012`(`RESOURCES`) |
| 0 件を認めない | 同 `:1041-1046` |
| 起動を要さない静的検査として `make test` に入れる | `Makefile:407-409`(`check-test-count-witness`)、`Makefile:738`(前提に並んでいる) |
| 自己対照を木の合成で作る | `tools/check_test_count_ratchet.py:1244-1376`(`synth_*`)、`:1378`(`self_test`) |
| 自己対照の入口の作り方 | `Makefile:392-398`、`:428-431`、`:687-692` などの `*-self-test` |
| 予算形の別例 | `tools/check_eval_error_budget.py`、`tools/check_pass_line.py`、`tools/check_frame_access_entries.py` |

**具体案(1 つ):**
`tools/check_test_count_ratchet.py` に `--wiring`(名簿 `WIRING`)を足し、
`make check-test-file-wiring` と `make check-test-file-wiring-self-test` を
`Makefile:738` の `test` の前提に並べる。名簿は
`(台本, 配線の種別, 理由)` の 3 列で、種別は
`all` / `ci` / `manual:<ターゲット名>` / `host:<C のソース>` / `unwired:<理由と日付>`。
**`all`/`ci`/`manual` は `Makefile` と `ci.yml` から導出して検証する**(名簿が嘘なら落ちる)。
`host` は `test/c/*.c` の `run_lisp_file` の文字列から導出する。
未配線の本数と合計件数は `tools/census_test_counts.py` から導出して出す(規則 13)。

あわせて `tools/census_test_counts.py:379-439`(`where_it_runs`)に
`test/c/*.c` を第 3 の起点として足す(§0-1)。

---

## §13 文書に残る手順(6 本)—— 行番号

| 計器 | 手順の記述 |
|---|---|
| 計器 1 | `documents/abi-redesign.md:347-348`。関連: `:344`、`:634`、`:702`、`:904`、`:1033`、`:1053` |
| 計器 5(入口 = 計器 2) | `documents/declare-types.md:418`。関連: `:417`、`:530`、`:732` |
| 計器 4 | `documents/inline-arith.md:207` |
| 計器 6 | `documents/inline-arith.md:353` |
| 計器 4 の後続 | `documents/inline-arith.md:1054`、`documents/compare-type-specialized.md:104`、`documents/heap-exhaustion.md:584` |
| 計器 7 | `documents/performance-measurement.md:3635`。`documents/records-and-measurement-rules.md:764` |

いずれの入口も現存: `Makefile:1590`(`test-qemu-milestone`)、
`Makefile:1852`(`test-qemu-instcount-gate`)。
2 段構えは `Makefile:1613` / `:1621` 以降 の補助部品(版管理外)が要る。

---

## 付録 A. 7 本が呼ぶ口の在否(1 つずつ確認した)

| 口 | 登録箇所 | 在否 |
|---|---|---|
| `%%HEAP-USED-BYTES` | `src/c/runtime.c`(`primitive_heap_used_bytes`)、`src/lisp/transpile.lisp:230` | 有 |
| `%%GC-COLLECT-COUNT` | `src/c/runtime.c:3946` | 有 |
| `%%ZA-COMPILED-P` | `src/c/runtime.c:3888` | 有 |
| `%%IMM-SPACE-USED-BYTES` | `src/c/runtime.c:3958` | 有 |
| `%%EVAL-IN-ENVIRONMENT` | `src/c/runtime.c:3893` | 有 |
| `%%CURRENT-ENVIRONMENT` | `src/c/runtime.c`(登録表) | 有 |
| `%%CURRENT-INLINE` | `src/c/runtime.c:3952`(実装 `:2102-2105`) | 有 |
| `%%INLINE-OF` | `src/c/runtime.c:3955` | 有 |
| `%%DECLARED-TYPES-OF` | `src/c/runtime.c:3956`(実装の説明 `:2165`) | 有 |
| `%%DISASM-CODE-LEN` | `src/c/disasm_lisp.c:355` | 有 |
| `%%DISASM-CODE-BASE` | `src/c/disasm_lisp.c:353` | 有 |
| `%%GATE-OPEN` / `-CLOSE` / `-HITS` / `-PLUGIN-ARGS` | `src/c/bench_subprimitive.c:517-523`(呼び出しは `src/c/kernel.c:421`) | 有 |
| `DIV` | `src/c/runtime.c:3850` | 有 |
| `disasm-items` / `disasm-item-comment` | `src/lisp/disassemble.lisp:50` / `:28`(起動時に読まれる = `src/c/kernel.c:358-359`) | 有 |

**消えた口を呼んでいる台本は 1 本も無い。**

## 付録 B. 走らせていないこと(明示)

`make build` / `make test` / `make test-qemu*` / QEMU / docker / `git checkout` /
`git switch` / `git pull` / worktree —— **いずれも実行していない。**
使ったのは `git log` / `git show` / `git diff` / `git ls-files` /
`git merge-base --is-ancestor` / `git rev-list --count` / `grep` / `sed` / `python3`
(読み取りのみ)と、`tools/census_test_counts.py --inventory`(数え上げのみ)。
