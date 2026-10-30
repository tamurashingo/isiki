# 参照マップ: compiler-optimization(残っている部分)

- 調査日: 2026-10-29
- 対象リビジョン: `feature/jit-frame-rightsize` = `4cef194`
  (ローカルの `feature/compiler-optimization` = `964f525`。**遠隔の実体は `d8bebd0`** = PR #123 マージ後。
   ローカル参照が 1 マージぶん古い)
- 本ファイルは実装担当・レビュー担当向け。**設計担当には渡らない。**
- 既実現部分の参照は `docs/investigation/compiler-optimization.check.refs.md`(重複は再掲しない)
- 本調査では**コードを変更していない。ゲストも 1 回も起動していない**(行番号は読み取りのみ)

---

## A. 論理要約の節 → 実装箇所

### 論理 §3「フレーム配置の決定」/ §5-2「番地参照は 3 つの入口」

| 概念 | 実装 |
|---|---|
| 領域の定義の連鎖(起点と大きさ) | `src/c/za.c:1601`(`ZA_OFF_ENV_SAVED_HEAD`)〜`1744`(`ZA_FRAME_TOTAL`) |
| 可変にしたい領域 | `ZA_OFF_FLET_BASE` `za.c:1678`(+64)/ `ZA_OFF_FLET_OLD_BASE` `1692`(+768)= flet 832、<br>`ZA_OFF_QQ_BASE` `1704` + `ZA_QQ_LEVEL_SIZE` `1709`(416)× `ZA_MAX_QQ_DEPTH` `1701`(6)= 2496 |
| 可変領域の**上**にある常用領域(第 2 段で下げる対象、計 128 byte) | `ZA_OFF_SETQ_TMP_VAL` `1714`(32)/ `ZA_OFF_PARAM_BASE` `1729`(80)/ `ZA_OFF_SAVED_R14` `1740`(16) |
| 総量 | `ZA_FRAME_EXTRA` `za.c:1741` = 7440 / `ZA_FRAME_TOTAL` `1744` = `0x28 + EXTRA` = 7480 |
| 番地参照の唯一の 3 入口 | `za_load_slot` `za.c:2028` / `za_store_slot` `2030` / `za_addr_of_slot` `2032` |
| その下の共通エンコーダ(**常に disp32**) | `jit_emit_rsp_disp32` `za.c:556`、`jit_lea_reg_rsp` `565` / `jit_mov_reg_from_rsp` `566` / `jit_mov_rsp_from_reg` `567` |
| 領域ごとの番地計算(全部ここ経由) | `za_param_val_off` `1970` / `za_call_saved_head_off` `1975` / `za_arg_val_off` `1977` / `za_arith_val_off` `1984` / `za_nlx_val_off` `1989` / `za_local_val_off` `1994` / `za_let_saved_head_off` `2001` / `za_flet_old_val_off` `2005` / `za_qq_saved_head_off` `2013` / `za_qq_elem_val_off` `2016` / `za_qq_acc_val_off` `2022` |
| 数値直書きの rsp 相対参照は**0 件** | `grep -nE "za_(load\|store)_slot\([^,]+, *[0-9]+\)" src/c/za.c` → 0 件。`grep -nE "rsp" src/c/za.c` の残りは `jit_sub/add_rsp*` とインラインアセンブリ 2 行(`za.c:82,84`)のみ |

### 論理 §3「寸法の出力」/ §4-1

| 箇所 | 位置 | 本体コンパイルとの前後 |
|---|---|---|
| 確保(固定引数入口) | `jit_sub_rsp_imm32(ZA_FRAME_TOTAL)` `za.c:6995` | **前**(後埋め必要) |
| 確保(consリスト入口) | 同 `za.c:7025` | **前**(後埋め必要) |
| 戻す(末尾呼び出し・高速path) | `jit_add_rsp_imm32(ZA_FRAME_TOTAL)` `za.c:4930` | **本体中**(後埋め必要・任意個) |
| 戻す(末尾呼び出し・fallback) | 同 `za.c:4954` | **本体中**(後埋め必要・任意個) |
| 戻す(エピローグ) | 同 `za.c:6833`(`za_emit_epilogue` `za.c:6826`) | **後**(埋め不要) |
| エピローグの呼び出し箇所 | `za.c:7126`(通常)、`za.c:7146`(引数個数エラー側。**写し**) | どちらも本体より後 |
| 入口が 2 つになる条件 | `za.c:6962-6966`(`use_param_slots`)、判定は `za_form_disables_param_slots` `za.c:2398` |
| 命令の出力器 | `jit_sub_rsp_imm32` `za.c:591` / `jit_add_rsp_imm32` `za.c:592` |

### 論理 §3「後埋め」/ §9-2

| 概念 | 実装 |
|---|---|
| 相対距離の後埋め(既存。**絶対即値の口は無い**) | `jit_patch_rel32_target` `za.c:690` / `jit_patch_rel32` `703` / `jit_patch_rel8` `655` |
| プレースホルダ | `jit_emit_jmp_rel32_placeholder` `za.c:619` / `je` `608` / `jne` `627` / `jne rel8` `644` / `js` `665` / `jb` `677` |
| 「場所を配列に覚えて最後に埋める」前例 1 | 共有スタブへの jmp: `g_jit_trampoline_jmp_patch_offsets` `za.c:431`、上限 `ZA_MAX_TRAMPOLINE_JMPS` `za.c:430`(128)、記録 `jit_emit_jmp_to_trampoline` `za.c:718`、再計算 `za.c:7208-7218` |
| 「場所を配列に覚えて最後に埋める」前例 2 | 自己参照 movabs: `g_jit_reloc_patch_offsets` `za.c:417`、上限 `ZA_MAX_JIT_RELOCS` `za.c:412`(128)、記録 `za.c:469`、再計算 `za.c:7191-7206` |
| 上限超過時の劣化 | `g_jit_overflow = 1` → `za_try_compile_defun` が `g_jit_used = entry` で巻き戻して `nil` を返す(`za.c:7152-7156`)→ 呼び出し元が解釈実行へ(`src/c/eval.c:453` 付近) |
| コンパイルごとのリセット | `za.c:6945`(即値記録)、`6949`(ネスト)、`6951`(reloc)、`6952`(trampoline jmp) ← **寸法の後埋め状態もここに足す** |

### 論理 §3「スタック残量の検査」/ §9-1

| 概念 | 実装 |
|---|---|
| 生成される 3 命令の検査 | `za_emit_stack_guard` `za.c:1953`(呼び出しは `za.c:6991`, `7021`) |
| 断念経路(フレーム前の出口) | `za.c:1958-1963`(`sub rsp,40` / `call r11` / `add rsp,40` / `ret`) |
| 知らせる関数 | `os_za_signal_stack_exhausted` `za.c:1907` |
| 閾値のグローバル | `g_za_stack_limit` `za.c:1862` / `_normal` `1864` / `_floor` `1866` / 再入印 `g_za_in_stack_signal` `1868` / 発火回数 `1870` |
| 閾値の計算(**フレーム寸法を直接使う**) | `os_za_set_stack_low` `za.c:1874`、`1881`(normal)、`1882`(floor)。呼び出しは `src/c/interrupt.c:575` |
| 予備領域の導出式 | `ZA_STACK_SAVED_REGS` `za.c:1787` / `RETURN_ADDR` `1789` / `SHADOW_SPACE` `1791` / `ZA_STACK_PER_FRAME` `1802`(**フレーム寸法に比例する項**)/ `ZA_SIGNAL_PATH_MAX` `1812`(8696)/ `ZA_HANDLER_ALLOWANCE` `1824`(4096)/ `ZA_STACK_HARD_FLOOR_WANT` `1827`(4096)/ `ZA_STACK_RESERVE_MIN` `1830` / `ZA_STACK_SIGNAL_RESERVE` `1840`(24432) |
| 整合の静的検査 | `za.c:1856`, `1858` |
| 「safety で外せない」の明記 | `za.c:1771`(コメント)、`documents/declaim-design.md:151-185`、`documents/stack-guard.md:457-473` |

### 論理 §5-1(16 の剰余が 8)

- 根拠のコメント: `za.c:1711-1713`、`1668`(`392 mod 16 = 8` のため偶数を保つ)、`1730-1731`、`1738-1739`
- 計算: 7440(16 の倍数)+ 40 = 7480、7480 mod 16 = 8
- **機械的な歯止めは無い**(`_Static_assert` が無い)。足すならここ

### 論理 §9-11-4(最下段 40 byte の疑い)

- 「シャドウスペース」と書いている箇所: `za.c:1743`、`7019`
- しかし**呼び出しごとに自前で確保している**: `jit_call_r11` `za.c:345-349`(`sub rsp,32` / `call r11` / `add rsp,32`)、
  素の `call r11` は `za.c:347`(= jit_call_r11 内)と `za.c:1961`(ガードの断念経路。自前で `sub rsp,40`)の **2 箇所だけ**
  (`grep -n "0xD3" src/c/za.c`)
- 確かめ方: `documents/stack-guard.md` §5-2 と同じ「塗って最高水位を読む」方法

### 論理 §3「算術の生成」/ G4

| 概念 | 実装 |
|---|---|
| 加減算のインライン(**宣言に関係なく常に出る**) | `za_emit_arith_call_or_inline` `za.c:2995`。早期 return は `za.c:2996`(add2/subtract2 以外は call のみ) |
| 乗除算の差し込み口 | 同 `za.c:2996` の早期 return。呼び出しは `za.c:3200`(`za_compile_fold` `za.c:3111` から) |
| 型特化の呼び先選択(四則すべて揃っている) | `za_specialized_wrapper` `za.c:3077`、使用は `za.c:3167`(fold)と `za.c:3469`(binary) |
| 比較の生成(**特化もインラインも無い**) | `za.c:4243-4262`(`<` `4245` / `=` `4249` / `>` `4253` / `<=` `4257` / `>=` / `/=`)。いずれも `inline_bit = 0` で `za_compile_binary` `za.c:3440` |
| 比較の土台(費用の記録と「安くする 2 本の道」) | `primitive_less_than2` `src/c/runtime.c:7353`、直前のコメント `runtime.c:7318-7352` |
| fixnum 特化の比較関数 | **存在しない**(`grep "less_than2_fixnum" src/c` → 0 件) |
| インライン指定のビット | `src/c/runtime.h:877-880`(CAR/CDR/NULL/EQ の 4 つのみ)、解釈 `os_inline_bit_of` `runtime.c:2116` |
| 門番 | `za_inline_enabled` `za.c:2760`、参照は `za.c:3304`(null)、`3414`(car/cdr)、`3505`(eq) |
| インラインの実例(タグ検査を含む列) | `za_emit_inline_car_cdr` `za.c:3350-3370`(タグ検査 `3351-3353`、**空リスト判定 `3355-3356`**) |

### 論理 §3「安全性の値」/ G3

| 概念 | 実装 |
|---|---|
| 値の詰め方 | `src/c/runtime.h:857-862`(`OPTIMIZE_SPEED/SAFETY/SPACE/PACK`、既定 `PACK(1,1,1)`) |
| インラインビットとの相乗り | `runtime.h:869-874`(`DECLAIM_INLINE_SHIFT 8` / `DECLAIM_WITH_INLINE_BITS`) |
| 宣言の解釈(環境単位) | `src/c/eval.c:533`(declaim 特殊形式)、`610-625`(speed/safety/space、インラインビットの退避) |
| 生成器に届いているグローバル | `g_za_declaim` `za.c:2746`、設定は `za.c:6940`(`za_try_compile_defun` の引数 `optimize`) |
| **safety を読む箇所は 0 件** | `grep -n OPTIMIZE_SAFETY src/c` → `eval.c:616,620` / `runtime.h:859` / `runtime.c:1661` / `za.c:7341`(コメント)のみ |
| 関数本体の宣言の解釈(**型だけ**) | `os_scan_declarations` `runtime.c:4269` 付近、`decl_apply_spec`(`(type <class> v...)` 以外は return)。**optimize は黙って無視** |
| 無検査のコア | `fixnum_add_signed_unchecked` `runtime.c:6310` / `fixnum_multiply_signed_unchecked` `6368` / `fixnum_divide_signed_unchecked` `6411` |
| 監査ビルド | `runtime.c:6557-6580`(`ISIKIOS_DECLARE_AUDIT` / `DECLARE_AUDIT_FIXNUM` / `_SINGLE`)、適用は特化関数の入口のみ(`6599,6600,6624,6625,6687,6688,6712,6713,7030,7031,7056,7057,7226,7227,7260,7261`) |
| **生成器側に監査ビルドの分岐は 0 件** | `grep -n DECLARE_AUDIT src/c/za.c` → 0 件 |
| 監査ビルドの渡し方 | `make ... EXTRA_CFLAGS=-DISIKIOS_DECLARE_AUDIT`(`documents/declare-typed-add.md:210`、`declare-typed-arith.md:140`)。Makefile に専用変数は無い |
| 4 分類の確定 | `documents/declaim-design.md:151-185` |
| **4 分類に入っていない検査** | 引数個数検査: `za.c:7040-7112`(cons入口のみ。固定引数入口は +0)。Issue #112 が外す対象に挙げている。<br>制御転送検査: `za_emit_ct_check_and_jmp_if_transfer` `za.c:772` |

### 論理 §4-5 / §9-7(性能悪化)

| 概念 | 実装 |
|---|---|
| 原因コミット | `ac3dc54`「perf(runtime): 符号が絡む fixnum の加減算を bignum 機構に落とさない(改善A)」 |
| 外に出た高速路 | `fixnum_negate` `runtime.c:6278` / `fixnum_add_signed` `runtime.c:6337` / `fixnum_add_signed_unchecked` `runtime.c:6310` |
| 呼び出し側 | `primitive_add2` `runtime.c:6630`(`fixnum_add_signed` を呼ぶ)、`primitive_subtract2` `runtime.c:6718`(`fixnum_negate` + `fixnum_add_signed` の **2 回**) |
| 「正の場合を先に捌く」直しを入れる場所 | `runtime.c:6630`(add2 の先頭)、`runtime.c:6718-6720`(subtract2 の先頭)。出す列の見本は JIT 側 `za.c:3004-3030`(`or` → `test TAG_MASK\|FIXNUM_SIGN_BIT` → 生加算 + `js` / 生減算 + `jb`) |
| 低い最適化水準(展開されない理由) | `Makefile:231`、`Makefile:274`(`-O1 -nostdlib -mno-red-zone`) |
| 旧経路が「両方非負」限定だった記録 | `za.c:2988-2992`(JIT 側のコメント)、`documents/fixnum-signed-fastpath.md`、`documents/type-system-survey.md` §10-4 |
| `-0` を作らない不変条件 | `runtime.c:6298-6305`(コメント)、`os_make_fixnum_signed` がマグニチュード 0 の符号を落とす |

### 論理 §9-10 / G5(AOT)

| 概念 | 実装 |
|---|---|
| 本体 1 式制約(宣言を書くと止まる) | `src/lisp/transpile.lisp:2207`(`"transpile-defun: bodyは単一式のみ対応です"`)。宣言は `2205` のパラメータ検査の後 |
| 算術の呼び先(**汎用版固定**) | `transpile.lisp:573`(`(+ 2 . "primitive_add2")`)、`574`(`-`)、`131`(n項 `+ → primitive_add`) |
| 比較の呼び先 | `transpile.lisp:566-568`(`= < >`)、`140`(n項) |
| `transpile.lisp` 内の `declare` は全部ホスト SBCL 向け | `(declare (ignore ...))` のみ |
| 記録 | `documents/declare-types.md:321-329` §5-4、限界は §5-5 `335-347` |
| 生成物 | `src/c/lisp_compiled.c`(**`.gitignore:8`**)、`test/c/lisp_compiled_fixture.c`(`.gitignore:9`) |
| 生成の口 | `make transpile`(`Makefile:211`)。`Makefile:207-209` が docker + roswell で生成。**ゲスト不要** |

### 論理 §9-9 / G8(ヒープ枯渇)

| 概念 | 実装 |
|---|---|
| 確保と枯渇時の停止 | `os_alloc_bytes` `src/c/runtime.c:219`、回収の再試行 `240-242`、**停止 `runtime.c:244`** |
| 割り込みを止めている区間 | `runtime.c:221-223`(`cli`)、`248-250`(`sti`) |
| 固定番地領域の停止 | `os_imm_page_alloc` `runtime.c:1783`、**停止 `runtime.c:1792`** |
| 固定番地領域で**素直に失敗を返す**側(= 既に解決している形) | `os_imm_code_alloc`(宣言 `runtime.h:898` 付近)。呼び出し側の劣化は `za.c:7174-7178` |
| 回収の枯渇(こちらはバグ) | `runtime.c:2439` |
| signal が確保を伴う証拠 | `os_signal_condition` `runtime.c:4599`、`os_make_cons` が `4648`, `4655`、Lisp への呼び戻しが `4650`, `4656` |
| 深さの打ち切りと再入防止 | `SIGNAL_MAX_DEPTH` `runtime.c:4588`(16)、`g_signal_depth` `4589`、診断 `cc_diag_signal_overflows` `4592` |
| 条件クラス(スロットなし) | `src/lisp/init.lisp:605`、AOT 側 `src/lisp/init_aot.lisp:705` |
| スタック側の解法(構造の見本) | `za.c:1907`(開けて閉じる / 重ねない / エラー値へ落ちる) |
| 解釈実行の評価の入口(安全な地点の候補) | `os_eval` `src/c/eval.c:1163`。**いま何の検査も無い** |
| 守られていないことの表示 | `os_panic_stack_overflow` `runtime.c:748`、`766-771`(ガード発火数 0 = 守られていない経路) |
| 記録 | `documents/stack-guard.md:3`(対象は JIT のみ)、`documents/error-unwind-survey.md:976` §6-2 #8 |

### 論理 §9-8 / G1 の検出器

| 検出器 | 位置 | 実寸化で何を直すか |
|---|---|---|
| 間接版(プロローグの即値を読む) | `test/lisp/jit_frame_test.lisp:28-45`(`%jit-frame-size`。先頭 **64 byte** を走査)、題材 `%jit-frame-probe` `47`(let を使う葉)、期待値 `63`(`<= 7480`)、`65`(`> 0`) | 期待値を下げる。**題材がどの関数かが意味を持つ**ようになる |
| 直接版(深さを探る) | `test/lisp/stack_guard_test.lisp:74-82`(`sgt-try` / `sgt-max`)、**探索範囲 `(sgt-max 15 40)` が `83`**、判定 `87`(`>= 20`)、`88`(`<= 34`) | **範囲の上端 40 を上げる**(上げないと上限判定が無意味になる)+ 両側の期待値 |
| 出口の数え上げ(第 1 段) | `test/lisp/jit_rsp_balance_test.lisp:189-196`(`%jrb-exit-p`)、`83-86`(フレーム前の出口)、`198-258`(`%jrb-audit`)、判定 `287-311` | 題材の追加のみ(混在の検証) |
| sub/add の整合 | 同 `226-266`。**関数内で比較している**ので混在に影響されない | `%jrb-fsz` の期待値 `263`, `264`(**7480 の直書き 2 件**) |
| 混在の題材(既に存在) | `%jrb-qq` / `%jrb-flet`(`jit_rsp_balance_test.lisp` 内)、`%jrb-tail`(末尾呼び出し) | 相互呼び出しの試験を足す |
| 内訳の合計一致 | `tools/jit_frame_breakdown.py`(`make jit-frame-breakdown` = `Makefile:1054`)。名前表 `OFFSETS` `tools/jit_frame_breakdown.py:43-50`、`WANTED` `30-39`、検算 `171`(合計 == `ZA_FRAME_TOTAL`)、`201`(`PER_FRAME == TOTAL+56`) | **名前が消えると明示的に落ちる**(`76-79`)。2 つの検算を最小/最大の形へ言い直す |
| 7480 の直書き(全件) | `jit_frame_test.lisp:63` / `jit_rsp_balance_test.lisp:263,264`。**コメントは** `jit_frame_test.lisp:9,10,53,59,68` / `jit_rsp_balance_test.lisp:261` / `tools/jit_frame_breakdown.py:7` / `Makefile:1050` / `za.c:1748` | |

### 論理 §6-2(入れ子のコンパイル)

- 式の再帰の深さ: `g_za_compile_nest` `za.c:2740`、上限 `ZA_MAX_COMPILE_NEST` `za.c:2731`(60)、判定 `za.c:4128`
- **関数単位のコンパイルが入れ子になる想定は無い**: `za.c:6945-6952` が無条件リセット
- 似た前例の不健全さ: `za_form_disables_param_slots` `za.c:2398` は**マクロ展開前のフォームを走査**する
  (`za.c:2382-2397` のコメントが保守側に倒した理由を述べている)

### 論理 §9-11(コメントと実体の食い違い)

| # | 主張 | 実体 |
|---|---|---|
| 1 | 記録に使う推定量は「3 回の最小値」 — `tools/bench/run_jit_bench.sh:43`、`Makefile:1019-1022` | 既定は中央値 — `run_jit_bench.sh:56`(`BENCH_ESTIMATOR:-median`)、`tools/bench/check_bench_baseline.py:402`、`tools/bench/bench_baseline.tsv` の `estimator` 列 |
| 2 | `ZA_MAX_LET_DEPTH` は 8 — `za.c:1653-1655` のコメント | 16 — `za.c:1009` |
| 3 | 未使用は 272-655 — `za.c:1636-1637` | 360-655(296 byte)— `documents/jit-frame-survey.md:82-93`, `775-800` が正しく訂正済み |
| 4 | 最下段 40 byte は呼び出し規約のシャドウスペース — `za.c:1743`, `7019` | 呼び出しごとに自前確保(`jit_call_r11` `za.c:345`)。32 byte は未使用と見られる(**未確認**) |

---

## B. ゴール条件 → 検証に使える既存の手段

**「無い」と書いたものは、作るところまでが作業範囲。**

### G1 案 d(フレームの実寸化)

| 条件 | 使える手段 | 実行コマンド | 所要 |
|---|---|---|---|
| 寸法が関数ごとになっている | `%jit-frame-size`(間接)+ `%jrb-fsz`(全命令走査) | `make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_jit_frame.lisp` | ゲスト 1 起動 |
| 内訳の合計が合う | `tools/jit_frame_breakdown.py` | `make jit-frame-breakdown` | 秒(**ゲスト不要**。ホストの gcc を使う) |
| 出口がすべて解体を伴う | `jit_rsp_balance_test.lisp:287-311` | 同上のミルストン | 同上 |
| sub/add の値と個数の整合 | `jit_rsp_balance_test.lisp:226-266` | 同上 | 同上 |
| 大小混在の相互呼び出し・末尾呼び出し | **部分的にある。** 題材(`%jrb-qq` / `%jrb-flet` / `%jrb-tail`)は存在するが、**相互に呼び合う試験は無い** | — | — |
| 末尾呼び出しでサイズの違う関数へ飛ぶ | **無い。** 作る。`stack_guard_test.lisp:92-95` が「末尾呼び出しは深さを消費しない」を見ているのが近い | — | — |
| 予備領域が導出式どおり | `jit_frame_breakdown.py` の RESERVE 表(`tools/jit_frame_breakdown.py:176-203`) | `make jit-frame-breakdown` | 秒 |
| 深さが期待どおり | `stack_guard_test.lisp:83`(`sgt-max 15 40`) | `make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_stack_guard.lisp` | ゲスト 1 起動 |
| スタックガードが正しく発火 | `stack_guard_test.lisp:43,53,54,55`(`assert-error-class`)、`62`(打ち切り 0 件) | 同上 | 同上 |
| GC 下で LIFO 規律が破れない | `make test-qemu-error-escape-gc`(`Makefile:916`、**GC_DEBUG 再ビルド**)、`test/lisp/qemu_boot_error_escape_gc.lisp`、ゲート `test/lisp/gc_debug_guard.lisp` | `make test-qemu-error-escape-gc` | 再ビルド + 1 起動 |
| GC 下のスタック枯渇 | `make test-qemu-stack-guard-gc`(`Makefile:1081`)、`test/lisp/stack_guard_gc_test.lisp` | 同左 | 再ビルド + 1 起動 |
| 命令数が全行帯の中 | `check_bench_baseline.py`(双方向) | `make test-qemu-jit-bench` | **数十分**(記録 ~14 分 / 今回の実測 ~34 分) |
| **AOT の生成 C が 1 文字も変わらない** | **無い。** 手順を作る: `make transpile` → `src/c/lisp_compiled.c` を退避 → 変更後に `make transpile` → `cmp`。生成物は gitignore 対象なので基準も自分で持つ | `make transpile; cmp -s src/c/lisp_compiled.c <退避>` | 分(ゲスト不要) |
| 行番号のずれ | `tools/check_jit_bail_lines.py`(`--fix` あり) | `make check-jit-bail-lines` / `python3 tools/check_jit_bail_lines.py --fix` | 秒 |
| EVAL-ERROR の予算 | `tools/check_eval_error_budget.py`(予算 `c_return:7` `lisp_quote:2`、`L:114-121`) | `make check-eval-error-budget` | 秒 |
| 16 の剰余の不変条件 | **無い**(`_Static_assert` が無い)。足すのが安い | — | — |
| 最高水位の数え上げの穴 | **無い**(入口 3 つに強制する仕組みが無い)。`grep` で確認するしかない | — | — |

### G2 性能回帰ゼロ

| 条件 | 手段 | コマンド | 所要 |
|---|---|---|---|
| 両経路・双方向の照合 | `tools/bench/check_bench_baseline.py`(双方向 `L:338-354`、推定量一致 `306-311`、試行回数 `319-329`、N 一致 `333-337`、校正点 `117-148`) | `make test-qemu-jit-bench`(aot+jit × loop arith tailrec let for funcall) | 数十分 |
| c と aot の 10 カテゴリ | `tools/bench/run_construct_bench.sh` | `make test-qemu-construct-bench` | **約 50 分** |
| 基準値 | `tools/bench/bench_baseline.tsv`(aot/loop 167.96、aot/arith 236.92、jit/loop 146.87、jit/arith 175.37。帯 20.00、細い帯 7.0) | — | — |
| 取り直し | `BENCH_NO_CHECK=1` を付けて照合を外す(`bench_baseline.tsv` ヘッダの「取り直し方」) | `BENCH_NO_CHECK=1 make test-qemu-jit-bench` | 数十分 |
| インタプリタへ落ちていないこと | ゲート 1 `test/lisp/bench_jit_guard.lisp`、ゲート 2(os_eval 区間の傾き 0)。陽性対照は `BENCH_PATHS=jit BENCH_CASES=evalfall BENCH_REPEAT=1 BENCH_NO_CHECK=1 make test-qemu-jit-bench`(`Makefile:1013-1015`) | 同左 | 数十分 |
| 推定量の校正 | `tools/bench/check_estimator.sh`(真値 `c/for = 4.000`)、資料 `tools/bench/bench_calibration.tsv` | `make check-estimator` | 秒 |
| 2 経路のベンチが同一 S 式 | `tools/bench/check_bench_jit_sync.sh`、生成器 `tools/bench/gen_bench_jit.py`(真実源 `src/lisp/bench_aot.lisp` → `src/lisp/bench_jit.lisp`) | `make check-bench-jit-sync` | 秒 |
| 逆アセンブル差分で機序を見る | `test/lisp/disassemble_test.lisp`、`%%DISASM-*`。Issue #114 のコメントが使った形 | ゲスト | 分 |
| **解釈実行の経路を測る行** | **無い**(基準値に interpreter 経路は 1 行も無い) | — | — |

### G3 safety

| 条件 | 手段 | 備考 |
|---|---|---|
| 宣言の値が届いている | `test/lisp/declaim_test.lisp`、`%%OPTIMIZE-OF`(`runtime.c:1708`)、`%%CURRENT-INLINE`(`runtime.c:1685`) | 届いている。**読まれていないことの確認**は `grep -n OPTIMIZE_SAFETY src/c` |
| 宣言がテストファイルを汚染しない | `test/lisp/inline_builtin_test.lisp:16,28,85,...`(`%%current-inline` の前後比較) | 未マージ分が**ファイル出口の検査**に格上げする |
| 嘘の宣言の検出 | `EXTRA_CFLAGS=-DISIKIOS_DECLARE_AUDIT` で再ビルド → ベンチ一式を 1 回通す | **インライン展開した列は見えない**(§A の G3 表)。ビルド印が変わるので再ビルドが走る(`Makefile:91`) |
| 型特化の正しさ | `test/lisp/declare_typed_add_test.lisp` / `_sub_` / `_mul_` / `_div_`、`declare_types_test.lisp` | 既存 |
| 無検査経路に異常値が流れる件 | **記録も試験も無い** | 決定が先 |
| 引数個数検査を外す効果を測る端点 | **無い**(Issue #112「計測基盤の不足」)。作るなら `src/lisp/bench_aot.lisp` に `&rest` つき / 引数 4 個以上 / funcall 経由のループを足し、`tools/bench/gen_bench_jit.py` で JIT 版を再生成し、`src/c/bench_subprimitive.c` に対応物を足し、`bench_baseline.tsv` に行を足す | 同期検査が鏡を強制する |
| safety 0 が仕様違反モードである記録 | `documents/islisp-implementation-defined.md:132-135` | 既存 |

### G4 型特化・インライン展開

| 条件 | 手段 | 備考 |
|---|---|---|
| PR #94 の取り込み | `feature/instcount-gate`(9 コミット。`36cdad6` が算術のインライン)。**本当に衝突するのは 2 ファイル 4 箇所**: `Makefile`(`test:` の依存行)と `documents/records-and-measurement-rules.md`(規則 10・11 の予約節 `L:312-320`)。確認コマンド: `git merge-tree $(git merge-base feature/compiler-optimization feature/instcount-gate) feature/compiler-optimization feature/instcount-gate \| grep -n "^+<<<<<<<"` → 4 件 | 意味の衝突: `_Static_assert(sizeof(za_syms_t) == 27 * ...)` `za.c:1405` を 28 へ / ファイル出口検査が**試験ファイルごとに 2 件増える** |
| #94 が持ち込む計器 | 区間つき命令数ゲート: `make test-qemu-instcount-gate MILESTONE=...`(2 パス)、`ISR_INSNS_PER_TICK ?= 914`、`tools/plugins/isiki_instcount.c`、`src/c/bench_subprimitive.c`、`test/lisp/qemu_boot_instcount_gate_check.lisp` | 現ブランチには `make test-qemu-instcount`(区間なし)だけ `Makefile:1111` |
| #94 が持ち込む検出器 | `tools/check_macro_forward_refs.py` + `make check-macro-order`(`test:` の先頭依存)、`isiki-file-end-check`(`test/lisp/test_framework.lisp`。宣言漏れ + ファイル単位件数) | **現ブランチに両方とも無い** |
| #94 が持ち込む試験 | `test/lisp/inline_decl_test.lisp`、`qemu_boot_inline_phase0.lisp` / `_phase0b` / `_phase3` | |
| `*` `/` のインライン | **無い。** 差し込み口 `za.c:2996`。#94 はビット(`INLINE_BIT_MUL` / `_DIV`)だけ用意して**生成器では使っていない** | |
| 比較の型特化 | **無い。** 差し込み口 `za.c:4245,4249,4253`(+ `<=` `>=` `/=`)。土台側の特化関数も**作る必要がある** | 設計の記述は `runtime.c:7318-7352` |
| 予測を事前宣言する作法 | `documents/records-and-measurement-rules.md` 規則 11(#94 が入れる)、規則 12(R-path)`L:322`、規則 14(R-set)`L:455` | |

### G5 AOT が declare をどこまで見ているか

| 条件 | 手段 |
|---|---|
| 調べる口 | `src/lisp/transpile.lisp:2185-2210`(`transpile-defun`)、`560-580`(2 引数の呼び先表)、`125-160`(n 項の呼び先表) |
| 既存の記録 | `documents/declare-types.md:321-347` |
| 効かせた場合の影響を見る手段 | `make transpile` の生成物 diff(ゲスト不要)+ `make test-qemu-construct-bench` の aot 行 |
| 結論を書く先 | `documents/declare-types.md` §5-4 |

### G6 インタプリタループの 96 byte/反復

| 条件 | 手段 | 備考 |
|---|---|---|
| 合計を測る | `%%HEAP-USED-BYTES`(`runtime.c:1405`、登録 `3355`)。使用例は `documents/for-expansion.md:465-481` | 構造的に正確 |
| 内訳を出す | **無い。** 既存の確保監査(`ALIGN_AUDIT=1` → `ALIGN_SITE_ALLOC_BYTES` など `runtime.c:625-629`)は**確保の入口ごと**で、**理由別ではない** | 理由別の数え上げを足す必要がある |
| 単位の大きさ | 小さなインスタンス 32 byte(`os_make_instance` の `os_alloc_bytes(32)`)、対 16 byte | 96 の組み合わせは数通り |
| 候補 | 脱出の印のインスタンス(`MAGIC_GO_EXIT` `src/c/eval.c:1085`、`MAGIC_BLOCK_EXIT` `eval.c:930`) | `documents/for-expansion.md:479` が「など」で止めている |
| 回帰で固定する手段 | `test/lisp/for_expansion_test.lisp`、`make test-qemu-for-expansion`(`Makefile:1093`) | |

### G7 expand-for を揃える

| 条件 | 手段 |
|---|---|
| host 側(変える側) | `src/lisp/transpile.lisp:792-822`(`expand-for`。`811` が 1 つの let へ `append`)、補助 `768-790` |
| guest 側(既に新形。入れ子) | `src/lisp/init.lisp:227-240` |
| 照合 | `tools/check_macro_parity.sh`、`make test-qemu-macro-parity`(`Makefile:1045`)。期待値 `tools/macro_parity_expected.tsv` の `FOR cosmetic` 行(**同じになったら落ちる**) |
| 意味の固定 | `test/lisp/for_semantics_test.lisp`、`make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_for_semantics.lisp`(27 件。`documents/for-expansion.md:263-330`) |
| 影響(AOT の生成物が変わる) | `make transpile` の diff、`make test-qemu-construct-bench` の `aot/for` 行 |
| 範囲外の定数 | `ZA_MAX_LOCALS_PER_LET` `za.c:1010` は**変更しない**(ゴール定義) |

### G8 枯渇の signal

| 条件 | 手段 | 備考 |
|---|---|---|
| スタック側(既に実装) | `test/lisp/stack_guard_test.lisp`、`stack_guard_gc_test.lisp`、`test/lisp/qemu_boot_no_init_signal.lisp:87` | |
| ヒープ側 | **無い。** 枯渇を起こす試験も、`<storage-exhausted>` を出す経路も無い | 題材の候補: `test/lisp/room_test.lisp`(ヒープ残量の報告) |
| 枯渇を起こす手段 | **確認していない**(ヒープサイズを縮める `-D` があるかは未調査)。`EXTRA_CFLAGS` でヒープ定数を上書きできるかを先に見ること | `Makefile:91` でビルド印に入る |
| 条件システムが使えない状態での挙動 | `test/lisp/qemu_boot_no_init_signal.lisp`(init.lisp を読まない唯一の起動) | 既存 |
| GC との競合 | `make test-qemu-error-escape-gc`、`%%DIAG-GC-STRESS`(GC_DEBUG のみ) | |

### G9 計器・検出器

| 条件 | 手段 | 備考 |
|---|---|---|
| 規則 15 の検出器 | **作業ブランチに無い。** 実体は `bf598ba`(`feature/check-pr-claims`、PR #123 で分岐元へマージ済み)= `tools/check_pr_claims.py`、`make check-pr-claims PR=<n> [AT=<rev>]`、`make test` 内の `check-pr-claims-self-test`、`test/fixtures/pr113_body.md` | 遠隔の分岐元 = `d8bebd0`。ローカル参照 `964f525` が古い |
| 規則そのもの | `documents/records-and-measurement-rules.md:497-560` | **作業ブランチに入っている** |
| 方位文書の一覧の誤り | `HANDOVER_COMPILER_OPTIMIZATION.md` §4 の 3 件(`check-macro-order` / `check_macro_forward_refs.py` / `#count` のファイル単位検査)。正しい書き分けは `documents/error-unwind-survey.md:35-38` | 方位文書を直すのが G9 の一部 |
| 陽性対照がある検出器 | `tools/check_eval_error_budget.py --self-test`(`L:310-378`)、`tools/check_jit_bail_lines.py --self-test`(`L:182-184`)、`tools/bench/check_estimator.sh`、`check_bench_baseline.py`(記録は `documents/performance-measurement.md:4022-4084`)、`jit_rsp_balance_test.lisp:337-422`(3 つ)、`test/lisp/jit_limits_test.lisp:7,14`、深さ検出器(`documents/jit-frame-survey.md:367` / `documents/stack-guard.md:519-546`) | |
| **陽性対照が見つからない検出器** | `_Static_assert` 群(`za.c:885,889,1003,1405,1856,1858`、`runtime.h:70-78`、`za_jit_tags.h:43,48,52`)/ `tools/jit_frame_breakdown.py` の合計一致 / `make test-qemu-macro-parity`(意図的発火の記録は `tools/macro_parity_expected.tsv` ヘッダのみ)/ `make test-qemu-error-escape-gc`(PR #121 本文に手順はあるが未実行) | 洗い出しは G9 の条件 |
| ホスト側の門番 4 つ | `make check-eval-error-budget check-bench-jit-sync check-estimator check-jit-bail-lines`(`Makefile:287,294,302,311`、`test:` の依存 `Makefile:316`) | 秒 |

### G10 回帰と PR

| 条件 | 手段 | コマンド | 所要 |
|---|---|---|---|
| ネイティブ試験 | 42 本を docker 内でコンパイルして実行 | `make test` | 分(前回 exit 0 / FAIL 0) |
| 仮想機械の全回帰 | `Makefile:772-788`。中身は `test-qemu` / no_init_signal / **error-escape-gc(GC_DEBUG 再ビルド)** / m5_ide / m6_fat16 / fat_callback_transfer / fat32 / fat32_primary_boot / partition ×2 / bench-jit / for_semantics / jit_frame / jit_limits / stack_guard | `make test-qemu-all` | **1 時間超**(仮想化支援なし) |
| 単発ミルストン | `Makefile:937` | `OVMF_CODE=... make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_*.lisp` | 分 |
| ベンチ(回帰の判定には入っていない) | `test-qemu-jit-bench`(`Makefile:1025`)と `test-qemu-construct-bench`(`Makefile:1001`)は **`test-qemu-all` に入っていない**。入るのは `test-qemu-bench-jit`(`Makefile:1033`。正当性のみ) | | |
| PR | base は `feature/compiler-optimization`。**ローカルで merge しない** | `git push -u origin feature/jit-frame-rightsize; gh pr create --base feature/compiler-optimization` | |

---

## C. 環境・ビルドの前提

| 項目 | 実体 |
|---|---|
| 最適化水準 | `-O1`(`Makefile:231`, `274`)。`-nostdlib -mno-red-zone`、freestanding |
| ビルド印 | `BUILD_FLAG_SIGNATURE = GC_DEBUG_FLAGS + ALIGN_AUDIT_FLAGS + JIT_DUMP_FLAGS + EXTRA_CFLAGS`(`Makefile:91`)、スタンプ `Makefile:90,94-96`。**`EXTRA_CFLAGS` は印に入っている** |
| GC デバッグ | `GC_DEBUG=1`(`-DISIKIOS_GC_DEBUG`)、`GC_PAINT=1`(`Makefile:58`)。**別ビルドになる** |
| 監査ビルド | `EXTRA_CFLAGS=-DISIKIOS_DECLARE_AUDIT`(Makefile に専用変数は無い) |
| 16 byte 境界監査 | `ALIGN_AUDIT=1`(`Makefile:65`) |
| 仮想化支援 | **無い**(`/dev/kvm` なし)。すべて命令エミュレーション |
| OVMF | `.envrc` の `OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd` |
| ベンチの既定 N | `BENCH_N_C=200000000` / `BENCH_N_C_CONS=10000000` / `BENCH_N_AOT=1000000`(`Makefile:993-995`)、`BENCH_N_JIT=1000000`(`Makefile:1024`)、`BENCH_REPEAT=3`、`BENCH_ESTIMATOR=median`(`run_jit_bench.sh:55,56`) |

---

## D. 本調査で使った読み取り専用コマンド

```
sed -n / grep / git log / git branch -a / git show --stat
git merge-tree $(git merge-base feature/compiler-optimization feature/instcount-gate) \
    feature/compiler-optimization feature/instcount-gate      # 衝突の試算(書き込み無し)
git diff feature/compiler-optimization...feature/instcount-gate -- <path>
gh issue view 110 / 112 / 114 ; gh pr view 94
gh api repos/:owner/:repo/branches/feature%2Fcompiler-optimization
python3 tools/jit_frame_breakdown.py                           # 唯一実行した道具(ゲスト不要)
```

**変更していないもの**: ソース・テスト・設定・Issue・PR・git の参照。
`git fetch` も行っていない(参照の書き換えを避けた)ため、
**ローカルの分岐元参照は遠隔より 1 マージぶん古いまま**である。

---

# 追加調査(2026-10-29、2 回目)の参照マップ

論理要約の `A-1`〜`A-6` に対応する。

## A-1 末尾の解体命令の個数

| 概念 | 実装 |
|---|---|
| 解体命令 2 本が出る条件 | `src/c/za.c:4864`(`if (argc <= ZA_MAX_FIXED_ENTRY_PARAMS)` = 高速路の有無)。高速路側の `add` は `za.c:4930`、予備路側は `za.c:4954` |
| `ZA_MAX_FIXED_ENTRY_PARAMS` | `za.c:1728`(= 3) |
| **構造上の上限の出どころ** | 予備路が必ず出す共有スタブへの飛び越し `jit_emit_jmp_to_trampoline` `za.c:4957`(**呼び出しはこの 1 箇所だけ**: `grep -n jit_emit_jmp_to_trampoline src/c/za.c`)。記録配列 `g_jit_trampoline_jmp_patch_offsets` `za.c:431`、上限 `ZA_MAX_TRAMPOLINE_JMPS` `za.c:430`(128)、超過で `g_jit_overflow = 1`(`za.c:724`) |
| 導出すべき上限 | `2 * ZA_MAX_TRAMPOLINE_JMPS + 2`(出口 2 = `za_emit_epilogue` の 2 回の呼び出し `za.c:7126`, `7146`) |
| 末尾位置の伝播(数え方の根拠) | `za_compile_progn` `za.c:4051`(`is_last ? is_tail : 0` は `za.c:4073`)/ `za_compile_let` `za.c:3784`(同 `za.c:4020`)/ `if` の両枝 `za.c:4398`, `4409` / 一般呼び出しへの到達 `za.c:4500` / 末尾判定 `za.c:4746` |
| 特別扱いされる head の一覧(= 一般呼び出しにならないもの) | `za.c:4219-4484` の `head == syms->*` / `head == g_sym_*` 群。シンボル名の対応は `za.c:6909-6935`(`os_make_symbol("+")` 等) |
| 入れ子の上限(到達可能な末尾位置の深さの上限) | `ZA_MAX_COMPILE_NEST` `za.c:2731`(60)。境界試験 `test/lisp/jit_nest_depth_test.lisp:18-30`(59 は乗る / 60・100 は乗らない)、`test/lisp/jit_limits_test.lisp:170-173` |

**実測(静的、2026-10-29)**

- 方法: ホスト処理系で `src/lisp/transpile.lisp` を読み込み、
  `read-all-forms` + `macroexpand-all` で各 `defun` を展開し、
  上記の伝播規則で末尾位置をたどって一般呼び出しを数えた。
  使い捨てスクリプトはリポジトリ外(`scratchpad/tailcount.lisp`)に置いた。
  **リポジトリは変更していない。** 実行コマンド:
  `docker run --rm --user $(id -u):$(id -g) --entrypoint bash -v $PWD:/workspace -v <scratch>:/scratch isiki-builder -c 'cd /workspace && ros run --load /scratch/tailcount.lisp --quit'`
- 対象 2,022 defun / 読み取り失敗 7 件
- **実行時に JIT が見るファイルだけの最大: 48**
  (`src/lisp/init.lisp` の `%convert`、`init.lisp:798`。末尾位置 38 / 末尾の一般呼び出し 24)
- 2 位 `init.lisp` の `expt`(`init.lisp:1158`)= 14、3 位 `src/lisp/file-cmd.lisp` の
  `%filecmd-normalize-components-iter` = 8、以降 4 以下
- 分布(JIT 対象のみ): 0 が 904 / 1 が 55 / 2 が 694 / 3 が 3 / 4 が 24 / 8 が 1 / 14 が 1 / 48 が 1
- 事前変換専用・ホスト専用も含めた最大: 56(`src/lisp/transpile.lisp` の `transpile-expr`。
  **ホスト側でしか動かないので JIT には無関係**)
- 数え方の取りこぼし: ホスト側 `*macro-expanders*`(`transpile.lisp:284-298`)には
  `and` / `or` / `with-handler` 等が無い(`src/lisp/init.lisp:44`, `52`, `626` にはある)。
  **末尾位置に `and` / `or` が来る関数では少なめに出る**(上位の関数は該当せず)
- **正確な値を出すには計器が要る**: `jit_add_rsp_imm32` の発行回数を関数ごとに数えて
  `%%DIAG-*` から読めるようにする(本調査では足していない)

## A-2 フレーム最下段 40 byte

| 根拠 | 位置 |
|---|---|
| 総量 = 追加ぶん + 40 | `ZA_FRAME_TOTAL` `za.c:1744`(`0x28 + ZA_FRAME_EXTRA`)、`ZA_FRAME_EXTRA` `za.c:1741` |
| 最上段の記号は 7424 で終わる | `ZA_OFF_SAVED_R14` `za.c:1740`(= `ZA_OFF_PARAM_BASE + 80` = 7424、8 byte 使用) |
| → 7440〜7480 の 40 byte に対応する記号が無い | `tools/jit_frame_breakdown.py` の `OFFSETS`(`L:43-50`)に 7440 以上の項目が無い |
| 呼び出し側が自前で確保している | `jit_call_r11` `za.c:345-349`(`sub rsp,32` / `call r11` / `add rsp,32`)、ガードの断念経路 `za.c:1959-1962`(`sub rsp,40`)。素の `call r11` はこの 2 箇所だけ(`grep -n 0xD3 src/c/za.c`) |
| 16 の剰余が 8 である必要 | `za.c:1711-1713`, `1668`, `1730-1731`, `1738-1739`(コメントのみ。`_Static_assert` は無い) |

**死領域の内訳(領域の名目サイズ − 実際に参照される量)**

| 場所 | 名目 | 使用 | 死 | 位置 |
|---|---:|---:|---:|---|
| `ZA_OFF_ENV_SAVED_HEAD` | 16 | 8 | **8** | `za.c:1601`(退役スロットの跡は `za.c:1602-1603` のコメント) |
| `ZA_OFF_NLX_BASE` | 488 | 192 | **296** | `za.c:1630`(`ZA_MAX_NLX_DEPTH`=8 × 24) |
| `ZA_OFF_FLET_BASE` | 64 | 56 | 8 | `za.c:1678`、詰め物の明記は `za.c:1691` |
| `ZA_OFF_SETQ_TMP_VAL` | 32 | 24 | 8 | `za.c:1714`、詰め物の明記は `za.c:1716-1717` |
| `ZA_OFF_PARAM_BASE` | 80 | 72 | 8 | `za.c:1729`、詰め物の明記は `za.c:1730-1731` |
| `ZA_OFF_SAVED_R14` | 16 | 8 | 8 | `za.c:1740` |
| 上端(記号なし) | 40 | 0 | **40** | 上記 |
| **合計** | | | **376** | うち 8 は剰余のため残す → **368 が理屈上は外せる** |

**予備領域への波及**: 336 byte 外した場合 → `ZA_FRAME_TOTAL` 7480→7144、
`ZA_STACK_PER_FRAME`(`za.c:1802`)7536→7200、`ZA_STACK_RESERVE_MIN`(`za.c:1830`)
24424→24088、`ZA_STACK_SIGNAL_RESERVE`(`za.c:1840`、16 byte 切り上げ)24432→**24096**。
**指示書 §5-1 の予測(24,432 → 21,776)には届かない。**

**測定で確かめる場合**: `documents/stack-guard.md` §5-2 の方法は 1 段の総量しか見えない。
内側を見るには確保直後に上端を模様で埋める命令を出す必要がある(計器の追加)。
**構造からの証明で足りると判断した。**

## A-3 コンパイルの入れ子

| 概念 | 実装 |
|---|---|
| 展開がマクロ本体を解釈実行する | `za_macroexpand` `za.c:2289` → `primitive_macroexpand_1` `src/c/eval.c:1096` → `apply_macro`(`eval.c` 内、`eval_progn(body, call_env)` で**任意の式を評価**) |
| 1 関数ぶんの状態の一括初期化 | `za.c:6937`(`g_jit_overflow`)、`6940`(`g_za_declaim`)、`6942`(`g_za_param_decl_types`)、`6943`、`6945`(`g_za_imm_site_count`)、`6946`、`6949`(`g_za_compile_nest`)、`6950`、**`6951`(`g_jit_reloc_count`)**、**`6952`(`g_jit_trampoline_jmp_count`)**、`6956`(`g_za_literal_slot_alloc_count`) |
| 静かな破壊になるもの | `g_jit_reloc_count` を失うと `za.c:7191-7206` の再パッチが外側ぶんを行わない(作業用バッファの番地が焼き込まれたまま残る)。`g_jit_trampoline_jmp_count` は `za.c:7208-7218` が同様 |
| 検査が弱くなるもの | `g_za_imm_site_count` は `za.c:7268-7290` の GC 安全性の走査が使う |
| 劣化で済むもの | `g_za_compile_nest` は巻き戻しで下限を割り(UINT64 なので巨大値)、`za.c:4128` の判定で次の式から生成を捨てる |
| 「通常は 0 のはず」と既に書いてある | `za.c:6947-6948` のコメント |
| 除外される特殊形式(汎用関数定義の展開先が入る) | `za_is_excluded_special_form` `za.c:2341` 付近(`defglobal` 等。`defun` を含む) |

**走査の結果(2026-10-29)**: 全 `.lisp` の `defmacro` 29 個の本体を括弧の対応で切り出し、
`(defun ` / `(eval ` / `(load ` / `(defmacro ` / `(defclass ` / `(defgeneric ` の出現を見た。
候補 2 件はいずれも偽陽性:
`src/lisp/init.lisp:320`(`setf`。説明文中の例)、
`src/lisp/init.lisp:505`(`defgeneric`。**準引用のひな形の中**= 展開結果)。
→ **展開時に関数を定義するマクロは存在しない。**

**歯止めの案**: `za_try_compile_defun`(`za.c:6839`)の先頭に
「すでにコンパイル中か」の判定を 1 つ置き、立っていれば
既存の `{ ZA_BAIL_LINE(); return nil; }`(`za.c:6852` と同型)で解釈実行へ落とす。

## A-4 事前変換の生成物の同一性

| 項目 | 実測(2026-10-29) |
|---|---|
| 生成コマンド | `docker run --rm --user $(id -u):$(id -g) --entrypoint bash -v $PWD:/workspace isiki-builder -c 'ros run --load src/lisp/transpile.lisp --eval "(main-or-die)" --quit'`(`Makefile:197-199` / `206-208` と同一) |
| **所要** | **0.848 / 0.862 / 0.869 秒**(3 回。ゲスト不要・カーネルのビルド不要) |
| 1 回で両方生成されるか | **される**(`Makefile:200-204` のコメントどおり) |
| 比較対象 | `src/c/lisp_compiled.c`(3,872,728 byte)と `test/c/lisp_compiled_fixture.c`(862,503 byte)。**両方必要** |
| **決定性** | **3 回生成して md5 が 3 回とも一致**(`5763b588…` / `07ec21c5…`)。**2026-10-04 の既存生成物とも一致** |
| `make transpile` を叩くだけでは再生成されない | ファイル依存(`TRANSPILE_LISP_SRC` `Makefile:41`)なので最新なら no-op(実測 0.012 秒)。**比較には上の直接コマンドを使う** |
| 後片付け | 比較後は退避したものを `cp -p`(**時刻ごと**)で戻す。戻さないと `SRC`(`Makefile:44`)経由でカーネル全体が作り直される |

**本調査での扱い**: 退避 → 3 回生成 → 比較 → `cp -p` で復元。
`git status --porcelain` は調査前と同じ(未追跡 3 件のみ)。

## A-5 未マージの算術ブランチを入れたときに動く数字

| # | 動くもの | 位置 | 値 |
|---|---|---|---|
| 1 | 構造体の要素数の静的検査 | `src/c/za.c:1405` | 27 → **28** |
| 2 | インラインビットの個数と名前表の一致検査(新設) | `feature/instcount-gate` の `src/c/runtime.h`(`INLINE_BIT_COUNT 8`)と `src/c/runtime.c`(`g_inline_names[]` + `_Static_assert`) | 新設。片方だけ直すとビルドが止まる |
| 3 | 試験件数(ファイル出口の検査) | `feature/instcount-gate:test/lisp/test_framework.lisp` の `isiki-file-end-check`(1 ファイルあたり 2 件) | `test/lisp/qemu_boot_test.lisp` 57 load → **+114**、`qemu_boot_jit_frame.lisp` 2 → **+4**(87→91)、`qemu_boot_stack_guard.lisp` 1 → **+2**(23→25)、`qemu_boot_error_escape_gc.lisp` 8 → **+16(357→373)** |
| 3b | 新しい試験ファイル | `test/lisp/inline_decl_test.lisp`(275 行、`^(assert` が 90 行)+ `qemu_boot_test.lisp:43` へ 1 行追加 | 主要起動に **約 +92** |
| 3c | 既存試験の書き換え | `test/lisp/inline_builtin_test.lisp:91-104` | 1 件削除・5 件追加 = **+4** |
| 4 | **組み込み関数が 4 つ増える** | `feature/instcount-gate:src/c/bench_subprimitive.c`(`%%GATE-OPEN` / `%%GATE-CLOSE` / `%%GATE-HITS` / `%%GATE-PLUGIN-ARGS`) | **下記の小さいヒープ試験の余裕を食う** |

**357 の所在**: `JIT_FRAME_RIGHTSIZE.md` §7-4 と §9 の完了条件。
**リポジトリの中で 357 を固定している箇所は無い**(`test_framework.lisp:297` が合計を出すだけ)。
→ **指示書側の期待値を 373 へ直す**(PR 本文に書く対象)。

**小さいヒープ試験の余裕(実測、2026-10-29)**

| `SMALL_HEAP_SIZE` | 結果 |
|---|---|
| **85 KB(既定)** | **exit 0 / FAIL 0 件 / 出力 5,446 行** |
| 80 KB | **exit 124(60 秒で打ち切り)/ 出力 4,656 行で途切れる** |
| 75 KB | 同上(4,656 行) |
| 70 KB | 同上(4,656 行) |

- 既定値の定義: `test/c/runtime_test.c:2259-2261`、使用は `setup_small_heap` `runtime_test.c:2264-2266`
- 掃引の注意(文書化済み): `runtime_test.c:2254-2259`
  「`-DSMALL_HEAP_SIZE=...` で上書きして掃引」「timeout を**コンテナの中**で効かせること」
- **しかし `make test` のビルド行は `$(EXTRA_CFLAGS)` を渡していない**
  (`Makefile:317-321` は `-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS)` だけ)。
  → つまみが口に繋がっていない。**1 行足せば繋がる**
- 本調査での実行(参考。リポジトリは変更していない):
  ```
  docker run --rm --user $(id -u):$(id -g) --entrypoint gcc -v $PWD:/workspace isiki-builder \
    -std=c11 -Wall -Wextra -DISIKIOS_UNIT_TEST -DSMALL_HEAP_SIZE="(80 * 1024)" -I src/c \
    -o tmp/rt_probe src/c/runtime.c src/c/lisp.c src/c/process.c src/c/za.c src/c/eval.c \
       src/c/reader.c src/c/stream.c src/c/mount.c test/c/runtime_test.c -lm
  docker run --rm --user $(id -u):$(id -g) --entrypoint bash -v $PWD:/workspace isiki-builder \
    -c 'timeout 60 ./tmp/rt_probe'
  ```
  (`tmp/` の生成物は削除済み)

**動かないことを確認したもの**

- `tools/bench/bench_baseline.tsv` / `tools/bench/*.sh` / `*.py`: **差分に含まれない**
  (`git diff --stat feature/compiler-optimization...feature/instcount-gate` に `tools/bench` が無い)
- 命令数の計器: `tools/plugins/isiki_instcount.c` の差分は**追加のみ**
  (削除行はヘッダコメント 1 行だけ)。区間指定は `have_gate` で囲まれた opt-in で、
  既定の `total_insns` 出力経路は変わらない。**計器が数えるのはゲストの命令数**なので
  計器側の処理が増えても測定値は動かない
- `ISR_INSNS_PER_TICK ?= 914`(向こうの `Makefile`): 現ブランチに同名は 0 件
- 本当に衝突するのは **2 ファイル 4 箇所**: `Makefile`(`test:` の依存行 `Makefile:316`)と
  `documents/records-and-measurement-rules.md`(規則 10・11 の予約節 `L:312-320`)。
  **`test:` の解決では 5 つ全部を残す**(現ブランチの
  `check-eval-error-budget check-bench-jit-sync check-estimator check-jit-bail-lines` +
  向こうの `check-macro-order`)

## A-6 ヒープ枯渇を意図的に起こす手段

| 手段 | 位置 | 現在の挙動 | 所要 |
|---|---|---|---|
| **(1) ネイティブ試験のヒープを縮める** | `SMALL_HEAP_SIZE`(`test/c/runtime_test.c:2259`)。**80 KB 以下で到達**(上の実測) | `os_panic`(`src/c/runtime.c:244`)→ `os_panic` の無限ループ(`runtime.c:1155-1159`。ユニットテストでは `hlt` が出ない)→ **ハング** | ビルド数十秒 + 実行 ~60 秒 |
| **(2) ゲストで巨大な配列を 1 つ作る** | `primitive_create_vector` `src/c/runtime.c:9547` → `alloc_vector_block` `runtime.c:9477` 付近 → `os_alloc_bytes(8 * (1 + rank + total))`。**上限検査なし** | `os_panic`(`runtime.c:244`)→ 停止表示は**画面側のみ**(`runtime.c:1141-1145`。`os_panic_stack_overflow` `runtime.c:748` と違いシリアルへ出さない)→ 試験では電源断フックへ | **1 回の確保。即時** |
| (3) ゲストのメモリ量を減らす | `QEMU_MEM ?= 256M`(`Makefile:16`、使用は `Makefile:715,724,741,925,943`)。`Makefile:13` に 96M の例 | 同上 | 起動 1 回ぶん。しきい値の掃引が必要 |
| ヒープ量のコンパイル時定数 | **無い。** `os_heap_init(heap_base, heap_size)`(`runtime.c:1277`)は UEFI からの空き領域を半分に割るだけ(`runtime.c:1303-1313`)。渡し元は `src/c/kernel.c:133-136`(`os_boot_alloc_finalize`) | — | — |
| 固定番地領域の枯渇 | `IMM_SPACE_SIZE`(`runtime.c:1543`、16 MB)。`os_imm_page_alloc` `runtime.c:1783` で `os_panic`(`runtime.c:1792`) | 停止 | — |
| **期待値を見る仕組み(ヒープ側)** | **無い。** スタック側の `test/lisp/stack_guard_test.lisp:43,53,54,55,62` と同型で書ける | — | — |

**隣接する不具合(範囲外。記録のみ)**: `alloc_vector_block`(`runtime.c` 内)は
`total *= dims[i]` と `8 * (1 + rank + total)` に桁あふれ検査が無く、
`primitive_create_vector`(`runtime.c:9547-9549`)は要素数を
`cc_car(args) >> FIXNUM_VALUE_SHIFT` で読むだけで**型検査も負値検査も無い**
(fixnum は符号＋絶対値なので負値は巨大な要素数として読まれる)。
→ **枯渇の判定は掛け算より前の「要求された量」に対して行う必要がある。**(未実行・読み取りのみ)

## 追加調査で使った読み取り/測定コマンド

```
# A-1(使い捨てスクリプトはリポジトリ外)
docker run --rm --user $(id -u):$(id -g) --entrypoint bash \
  -v $PWD:/workspace -v <scratch>:/scratch isiki-builder \
  -c 'cd /workspace && ros run --load /scratch/tailcount.lisp --quit'
# A-3
python3 - <<'PY' ... (defmacro 本体の括弧対応で切り出して走査) PY
# A-4
cp -p src/c/lisp_compiled.c <scratch>/ ; cp -p test/c/lisp_compiled_fixture.c <scratch>/
time docker run ... 'ros run --load src/lisp/transpile.lisp --eval "(main-or-die)" --quit'   # x3
cmp -s <scratch>/orig_lisp_compiled.c src/c/lisp_compiled.c
cp -p <scratch>/orig_*.c <元の場所>        # 時刻ごと復元
# A-5 / A-6
docker run ... gcc -DSMALL_HEAP_SIZE="(80 * 1024)" ... -o tmp/rt_probe ...
docker run ... bash -c 'timeout 60 ./tmp/rt_probe'
git merge-tree $(git merge-base feature/compiler-optimization feature/instcount-gate) \
    feature/compiler-optimization feature/instcount-gate
```

**変更していないもの**: ソース・テスト・設定・Issue・PR・git の参照。
`src/c/lisp_compiled.c` と `test/c/lisp_compiled_fixture.c` は 3 回再生成したが、
**内容は一致しており、時刻ごと元に戻した**(`git status --porcelain` は調査前と同じ)。
`tmp/` の実験ビルドは削除済み。

---

## 参照している一時文書について（2026-10-30 追記）

`HANDOVER_COMPILER_OPTIMIZATION.md`（方位文書）と `JIT_FRAME_RIGHTSIZE.md`（案 d 指示書）は
**git 管理外**で、2026-10-30 に削除した（claude.ai で設計していた内容を Claude Code へ移行するための
一時文書だった）。**内容は `docs/plans/compiler-optimization.md` と本調査ファイル群に取り込まれている**ので、
本文中のこれらへの参照は「出典の記録」として読むこと（ファイルは存在しない）。
