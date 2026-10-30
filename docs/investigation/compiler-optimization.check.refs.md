# 根拠の参照マップ: compiler-optimization

- 確認日: 2026-10-29
- 対象リビジョン: `feature/jit-frame-rightsize` = `4cef194`
  (分岐元 `feature/compiler-optimization` = `964f525`、`origin/feature/compiler-optimization` も同じ)
- 作業木: 未コミットの変更なし。未追跡は `HANDOVER_COMPILER_OPTIMIZATION.md` と
  `JIT_FRAME_RIGHTSIZE.md` の 2 本だけ(`git status --porcelain`)
- 環境: `/dev/kvm` 無し、`vmx`/`svm` 0 件 → QEMU は全て TCG。`OVMF_CODE` は
  `.envrc` の `/usr/share/OVMF/OVMF_CODE_4M.fd`

---

## 0. 実行したコマンドと結果(要約)

| コマンド | 結果 |
|---|---|
| `make test` | **exit 0。`FAIL` 0 件**(ログ 5,823 行)。`docker`(`isiki-builder`)で 42 本をコンパイルして実行 |
| `make check-eval-error-budget check-bench-jit-sync check-estimator check-jit-bail-lines` | 全て OK。陽性対照も OK(下記 §9) |
| `OVMF_CODE=... make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_jit_frame.lisp` | `87 passed, 0 failed`。`#jit-frame ZA_FRAME_TOTAL=7480 256KB で 35 段` / `#jit-rsp fsz=7480 leaf(sub 2/add 2/pop 2) tail(sub 2/add 4/pop 4)` |
| `OVMF_CODE=... make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_stack_guard.lisp` | `23 passed, 0 failed`。`#stack-guard max-depth=27 (記録値 27)` |
| `python3 tools/jit_frame_breakdown.py` | 合計 7480、`内訳の合計は ZA_FRAME_TOTAL と一致している` |
| `OVMF_CODE=... make test-qemu-jit-bench` | **exit 0。`検出器: 既定の帯を超えたずれはありません。`** 全 12 行(aot 6 / jit 6、N=1,000,000、median 3 回)。73 起動、約 34 分(TCG) |
| `make test-qemu-all` | **未実行**。GC_DEBUG 再ビルド + 16 ミルストーン + `test-qemu-bench-jit`。TCG のみのため 1 時間超の見込み |
| `make test-qemu-macro-parity` | **未実行**(QEMU 1 起動 + roswell。期待値表から判定した) |

---

## 1. G1 — 案 d(JIT フレームの実寸化)

### 第 1 段(出口の数え上げ)= 実現済み

- 実装: `test/lisp/jit_rsp_balance_test.lisp`
  - `%jrb-exit-p`(L:189-196)— `ret` = `C3` / `jmp r11` = `41 FF E3`。
    `call r11` = `41 FF D3` と ModRM で区別
  - `%jrb-add-rsp-40-p`(L:83-86)— スタックガードの断念経路が出す
    `48 83 C4 28`(フレーム生成前の出口)
  - `%jrb-audit`(L:198-258)— 戻り値に `nexit` / `exorphan` / `nteardown` / `npreframe` を追加
  - アクセサ `%jrb-nexit` / `%jrb-exorphan` / `%jrb-nteardown` / `%jrb-npreframe`(L:265-268)
  - 判定: L:271-311(`1-C` 節)。9 関数で `exorphan = 0`、
    `npreframe > 0` かつ `npreframe = nsub`(= 入口の数。0 になればスタックガードの消失)
- 陽性対照(規則 8):
  - `3-A`(L:337-376)命令を丸ごと `nop` で潰す → `orphan > 0`、個数一致が崩れる、
    **かつ `bad = 0`(即値の一致は通ってしまう)**
  - `3-B`(L:378-386)即値を 1 byte 変える → `bad > 0`、`orphan = 0`
  - `3-D`(L:388-413)`pop r13` を `nop` 2 つで潰して出口を残す →
    `exorphan > 0`、個数一致が崩れる、**`bad = 0`**
  - `3-C`(L:415-422)戻したコードを実際に呼んで値を確認(戻し忘れも落ちる)
- 文書: `documents/jit-static-code-check.md`(`4cef194` で +39 行。残る穴 = `jmp rel32`)
- コミット: `4cef194 test(jit): 出口そのものを数える(案 d 第 1 段)。入れた直後に 1 件見つかった`
  - 件数 66 → 87。実行で `87 passed, 0 failed` を再現
  - 入れた直後に見つけた 1 件 = PR #121 が追加した 4 つ目の出口(スタックガードの断念経路)

### 第 2〜6 段 = 未着手

- `src/c/za.c:1744` `#define ZA_FRAME_TOTAL (0x28 + ZA_FRAME_EXTRA)` — 単一定数のまま
- 寸法の出力箇所は 5 箇所。**いずれも定数を直接 emit している(後埋めではない)**:
  - `src/c/za.c:6995`, `7025` — `jit_sub_rsp_imm32(ZA_FRAME_TOTAL)`(入口 2)
  - `src/c/za.c:4930`, `4954`, `6833` — `jit_add_rsp_imm32(ZA_FRAME_TOTAL)`(出口 3。
    4930/4954 が末尾呼び出し経路で本体コンパイル中に出る)
  - emitter 本体: `src/c/za.c:591-592` `jit_sub_rsp_imm32` / `jit_add_rsp_imm32`
- 後埋めの既存機構は分岐先専用: `jit_emit_*_rel32_placeholder` / `jit_patch_rel32`
  (`documents/jit-frame-survey.md:321` が「技法は既に多用されている」と書いている)
- 領域の並び(`python3 tools/jit_frame_breakdown.py` 出力):
  `ZA_OFF_QQ_BASE` 4816 +2496 / `ZA_OFF_FLET_BASE` 3984 +64 /
  `ZA_OFF_FLET_OLD_BASE` 4048 +768(= flet 832)/ `ZA_OFF_LOCAL_BASE` 656 +1536 /
  `ZA_OFF_LET_SAVED_HEAD_BASE` 2192 +128(= let 1664)/ `ZA_OFF_CALL_BASE` 2320 +1568。
  **可変長にすべき領域がフレーム末尾に無い**(QQ の後ろに `SETQ_TMP` / `PARAM_BASE` /
  `SAVED_R14` が 128 byte ある)
- 第 5 段(穴 296)の調査は `documents/jit-frame-survey.md:82-93`(位置)、`775-800`
  (正体 = 拡張 15 の引っ越し跡、参照 0 の `ZA_OFF_*` は無い、詰めても +1 段)。
  **扱いの判断は未記録**
- 根拠の統計: `documents/jit-frame-survey.md:735-760`
  (`src/lisp` の実行時 load 側 114 本中 quasiquote 4 / flet 0。
  97.5% / 95.7% / 93.2%)

### 検出器(G1 の条件に挙がっている 4 つ)

| 検出器 | 場所 | 今回の確認 |
|---|---|---|
| 出口の数え上げ | `test/lisp/jit_rsp_balance_test.lisp:271-311` | 通過(QEMU) |
| `sub`/`add` 整合 | 同 L:226-266(`1-A` 値の一致 / `1-B` 対の一致 + orphan 0) | 通過(QEMU) |
| 再帰深さ(間接) | `test/lisp/jit_frame_test.lisp:28-65`(`%jit-frame-size`、`(<= jit-frame-size 7480)`) | 通過。向きは現在「増えたら落ちる」 |
| 再帰深さ(直接) | `test/lisp/stack_guard_test.lisp:63-85`(`sgt-max`、`>= 20` かつ `<= 34`) | 通過。`max-depth=27` |
| 内訳の合計一致 | `tools/jit_frame_breakdown.py`(`make jit-frame-breakdown`) | 合計 7480 で一致 |

付随条件の状態:

- 混在ブートの相互呼び出し / 末尾呼び出し: 該当テストなし(全関数同寸法のため混在が存在しない)
- 予備領域の導出式: `src/c/za.c:1802` `ZA_STACK_PER_FRAME`(`f5cb71a` で導出式化)、
  `1840` `ZA_STACK_SIGNAL_RESERVE`、`1856-1858` `_Static_assert`。
  試算は `documents/stack-guard.md:546` 付近(24,432)。縮んだ実測はまだ無い
- AOT 生成物の同一性: 仕組み・手順なし。`src/c/lisp_compiled.c` は gitignore 対象で
  比較の基準も用意されていない

---

## 2. G2 — 性能回帰ゼロ

- 検出器: `tools/bench/check_bench_baseline.py`
  - 双方向: L:338-354(`abs(diff) <= band` でなければ NG、
    `direction = "遅くなった" if diff > 0 else "**速くなった**"`)
  - 冒頭 L:14 に「遅くなった側だけ捕まえても足りない」と明記
  - 推定量の一致要求 L:306-311、試行回数 L:319-329、N の一致 L:333-337
  - 校正点検査 `check_calibration` L:117-148(`c/for` 真値 4.000)
  - 陽性対照用スイッチ L:418, 445(`--no-eval-gate`)
- 記録値: `tools/bench/bench_baseline.tsv`(26 行。c 10 / aot 10 / jit 6)
  - aot/loop 167.96、aot/arith 236.92、jit/loop 146.87、jit/arith 175.37、
    jit/funcall 368.07、jit/tailrec 399.27。帯は aot/jit 20.00、c 0.50
  - ヘッダに「#114 の悪化を含んだあとの値」「for は 2026-10-04 に有効へ戻した」
    「funcall/tailrec を 2026-10-07 に取り直した」が記録済み
- 基準値の扱いの決着: `documents/performance-measurement.md:3923-3926`
  「AOT 側の値は issue #114 の悪化を含んだあとの値である。…
   #114 を直したら記録を取り直すこと」
- 陽性対照の記録: `documents/performance-measurement.md:4022-4084`
  (`fixnum_add_signed_unchecked` に無意味ループを入れて aot 行だけが
   +178.71 / +98.15 で落ち、jit 行は動かなかった)
- ドライバ: `tools/bench/run_jit_bench.sh`(L:184, 187 で照合を呼ぶ)、
  `tools/bench/run_construct_bench.sh`(L:147, 150)
- 走る場所: `make test-qemu-jit-bench`(Makefile:1025)、
  `make test-qemu-construct-bench`(Makefile:1001)。
  **`make test`・`make test-qemu-all` には入っていない**
  (`test-qemu-all` に入るのは `test-qemu-bench-jit` = 正当性テストのみ。Makefile:1033)
- 今回の実測(2026-10-29、`4cef194`、median 3 回、N=1,000,000):

```
=== 記録値との照合(検出器、双方向) ===
[OK] aot/arith:   237.69 (記録 236.92 ±20.00, 差  +0.77)
[OK] aot/for:     255.56 (記録 270.51 ±20.00, 差 -14.95)
[OK] aot/funcall: 296.67 (記録 295.41 ±20.00, 差  +1.26)
[OK] aot/let:     605.19 (記録 605.16 ±20.00, 差  +0.03)
[OK] aot/loop:    182.16 (記録 167.96 ±20.00, 差 +14.20)
[OK] aot/tailrec: 366.05 (記録 369.28 ±20.00, 差  -3.23)
[OK] jit/arith:   174.96 (記録 175.37 ±20.00, 差  -0.41)
[OK] jit/for:     197.78 (記録 199.08 ±20.00, 差  -1.30)
[OK] jit/funcall: 363.96 (記録 368.07 ±20.00, 差  -4.11)
[OK] jit/let:     558.11 (記録 557.74 ±20.00, 差  +0.37)
[OK] jit/loop:    145.50 (記録 146.87 ±20.00, 差  -1.37)
[OK] jit/tailrec: 399.23 (記録 399.27 ±20.00, 差  -0.04)

--- 注意(落としはしない) ---
[注意] aot/for:  差 -14.95  -- 帯 ±20.00 には収まるが 3 回測定の分解能 ±7.00 を超えている
[注意] aot/loop: 差 +14.20  -- 同
検出器: 既定の帯を超えたずれはありません。
```

  - ゲート 2(`os_eval` 区間の N に対する傾き)は 12 行すべて `+0.000` で
    `OK(落ちていない)`
  - 細い帯を超えた 2 行は、`bench_baseline.tsv` ヘッダが
    「もともといちばん幅の広い 2 行(aot/loop 幅 14.8、aot/for 幅 15.3)」として
    特定している行と**一致する**。新規の退行ではない
  - `c` 行 10 本と `aot/branch` `aot/cons` `aot/nontailrec` `aot/vector` は
    `[未測定]`(このドライバの対象外。`make test-qemu-construct-bench` 側。約 50 分)
- Issue #114: open。コメント 1 件で `ac3dc54` を bisect で特定、
  機序(C 側高速 path のインライン化喪失、`call` 3→5、1 呼び出し約 29 命令)まで記載。
  末尾に「直し方の見当(試案を測っただけ。未コミット)」

---

## 3. G3 — safety

- 受け皿(値の保持): `src/c/runtime.h:857-862`
  (`OPTIMIZE_SAFETY` / `OPTIMIZE_PACK` / `OPTIMIZE_DEFAULT = PACK(1,1,1)`)
- 解釈: `src/c/eval.c:533`(`declaim` 特殊形式)、`616-620`(speed / safety / space)
- 報告: `src/c/runtime.c:1658-1666`(`%%optimize-of` 系)、`2107`(既定値の解決)
- **コード生成での参照は 0 件。** `OPTIMIZE_SAFETY` の全参照は
  `eval.c:616,620` / `runtime.h:859` / `runtime.c:1661` / `za.c:7341`(コメント) だけ。
  `src/c/za.c:1771` は逆に「[この検査は safety で外せない] 型の検査ではなく資源の検査である」
- 4 分類の確定と記録: `documents/declaim-design.md:151-185`(表 + 1 だけが
  「プログラマの要求」であることの注記)。`documents/stack-guard.md:457-473`
  (4 番目の族としてスタック残量を追加)。`documents/islisp-implementation-defined.md:132-135`
  (`safety 0` は仕様違反モード)
- `_unchecked` の実体: `src/c/runtime.c:6310`(add)、`6368`(mul)、`6411`(div)。
  先頭コメント L:6391「`_unchecked` が外しているのはタグ検査だけ。ゼロ検査は外していない」
- 嘘の宣言の監査: `src/c/runtime.c:6551-6580`(`ISIKIOS_DECLARE_AUDIT` /
  `DECLARE_AUDIT_FIXNUM` / `DECLARE_AUDIT_SINGLE`)。適用箇所は
  `6599,6600,6624,6625,6687,6688,6712,6713,7030,7031,7056,7057,7226,7227,7260,7261`
  (+ - * / × fixnum/single の 2 引数)。
  使い方は `documents/declare-typed-add.md:207-210`、
  `documents/declare-typed-arith.md:137-140, 520, 817, 850`、
  `documents/records-and-measurement-rules.md:244`
- `_unchecked` × `EVAL-ERROR` の決着: **記録が見つからない。**
  `documents/islisp-implementation-defined.md` / `declaim-design.md` /
  `declare-typed-arith.md` に該当の記述なし
  (`declare-typed-arith.md:707-715` はゼロ除算が `EVAL-ERROR` になる話で別件)
- インライン展開による届け方: `src/c/runtime.h:877-880` に `INLINE_BIT_` は
  `CAR` / `CDR` / `NULL` / `EQ` の 4 つのみ。safety 用のビットは無い

---

## 4. G4 — 型特化・インライン展開の仕上げ

- `*` `/` のインライン展開(JIT): **無い。**
  `src/c/za.c:2995-3045` `za_emit_arith_call_or_inline` は冒頭 L:2996 で
  `wrapper_fn != primitive_add2 && wrapper_fn != primitive_subtract2` なら
  `call` を出して戻る
- 型特化の呼び先選択: `src/c/za.c:3080-3089`
  (`primitive_add2_fixnum` / `subtract2_fixnum` / `multiply2_fixnum` / `divide2_fixnum`)
  — **特化 call はある。インライン展開は + - のみ**
- 比較演算の型特化: **無い。** `src/c/za.c:4245,4249,4253` は
  `primitive_less_than2` / `num_equal2` / `greater_than2` を
  `inline_bit = 0` で呼ぶだけ。`*_fixnum` 版の比較関数は `src/c/runtime.c` に存在しない
  (`grep "less_than2_fixnum"` 0 件)
- 8 → 22 命令の件の記録: issue #112 本文「2 引数版の比較 6 個(優先度高)…
  成功経路が +5 命令」「`cmov` の 8 命令へ完全に戻せる」。
  `documents/type-system-survey.md` の「`<` / `>` / `=` に fixnum インライン化が無い」
- 進行中: PR #94(open, WIP, base `feature/compiler-optimization`,
  head `feature/instcount-gate`, 最終更新 2026-09-23)
  - `git log feature/compiler-optimization..feature/instcount-gate` = 9 コミット
    (`36cdad6 feat(inline): + - fixnum を inline 宣言で展開する(Phase 3)` 等)
  - `git grep INLINE_BIT_ feature/instcount-gate -- src/c/runtime.h` →
    `INLINE_BIT_ADD/SUB/MUL/DIV`(bit 4〜7)+ `INLINE_BIT_COUNT 8`。
    **このブランチ(`feature/compiler-optimization` 系)には無い**
  - PR 本文のチェックリスト: `[x] + fixnum のインライン化(- も同時)` /
    `[ ] - * / fixnum`
  - `git rev-list --count feature/instcount-gate..feature/compiler-optimization` = **69**

---

## 5. G5 — AOT が declare をどこまで見ているか

- 記録: `documents/declare-types.md:321-329` §5-4
  「AOT トランスパイラ(`src/lisp/transpile.lisp`)は declare を扱わない。
   `declare` を書くと `transpile-defun` の『bodyは単一式のみ対応です』でビルドが止まる。
   …当面はこのままにした」
- `src/lisp/transpile.lisp` の `declare` は全て `(declare (ignore ...))`
  (= ホスト SBCL 向け。L:636,641,676,697,712,809,825,843,851,863,871,903,956,961,
  1175,1177,1199,1399,1487,1506,1526,1815,1975,1984,1995,2047,2055,2140,2203 …)
- AOT の算術の呼び先: `src/lisp/transpile.lisp:131`(`+ → primitive_add`)、
  `573`(`(+ 2 . "primitive_add2")`)。**`primitive_add2_fixnum` 等は呼んでいない**
- 方位文書 §7-6 が未解決として挙げている(「いちばん手を入れていない経路。
  C 側の特化関数は揃っているのに AOT は一度も呼んでいない」)
- 併記されている限界: `documents/declare-types.md:335-347` §5-5(`let*` では効かない)、
  §5-3(`let` 束縛変数の型は関数オブジェクトに残らない)

---

## 6. G6 — インタプリタループの 96 byte/反復

- 数値: `documents/for-expansion.md:9`(確保量 JIT 32→0、インタプリタ 928→96)、
  `381`、`393`(★ を付けた前提が外れたこと)、`465-481` §5-6
- 心当たり: `documents/for-expansion.md:479`
  「残っている 96 は『インタプリタでループを回すこと自体の費用』である
   (`go` が作る `MAGIC_GO_EXIT` の instance **など**)」
  → **「など」で止まっており内訳の分解がない**
- 価値判断の記録: `documents/for-expansion.md:182`
  「`while` は 0 なので性能の面でも `while` のままで損は無い」
  (= `for` 固有分についての判断。96 そのものの対処 / 見送りの記録ではない)
- 方位文書 §7-2 が未解決として挙げている(「内訳を出すまで価値は未知」)

---

## 7. G7 — expand-for を入れ子に揃える

- 期待値表: `tools/macro_parity_expected.tsv`
  - `FOR cosmetic` 行に「**2026-10-04 に guest を新展開形へ移植した。**
    残る差は一時変数を置く場所だけで、host は 1 つの let に append、guest は入れ子の let。
    理由は JIT の `ZA_MAX_LOCALS_PER_LET=4`(`za.c:963`)」
  - 判定の意味(ヘッダ): `cosmetic` は「**同じになったら落ちる**」
    → 揃えたら期待値表も直す必要がある
- 照合器: `tools/check_macro_parity.sh`(`make test-qemu-macro-parity`、Makefile:1045)。
  host 側は roswell、guest 側は QEMU 1 起動
- 関連: `documents/for-expansion.md:263-330` §3(意味論を 3 経路で固定、27 件)、
  `test/lisp/for_semantics_test.lisp`
- `ZA_MAX_LOCALS_PER_LET` の変更はゴール定義で範囲外

---

## 8. G8 — 仕様バグ 2 件

### #110(`<storage-exhausted>`)

- 実装(JIT のスタック枯渇のみ): `src/c/za.c:1756`(検査の位置)、
  `1855`(注意書き)、`1894-1915`(`os_za_signal_stack_exhausted`。
  「`<storage-exhausted>` はスロットを持たないので initargs は nil」)、
  `1866-1890`(`g_za_stack_limit_normal` / `_floor`)
- クラス定義: `src/lisp/init.lisp:605`。AOT 側 `src/lisp/init_aot.lisp:705`
- 試験: `test/lisp/stack_guard_test.lisp:41-53`(深さ 1000 / 500 で
  `assert-error-class`)、`113`(ハンドラ経由)、`129`。
  `test/lisp/stack_guard_gc_test.lisp:30,35`(GC_DEBUG)。
  `test/lisp/qemu_boot_no_init_signal.lisp:87`
- **残っている穴**:
  - ヒープ枯渇は今も停止: `src/c/runtime.c:244` `os_panic("out of memory (lisp heap exhausted)")`
    (`os_alloc_bytes` 内)。Immobilized は `runtime.c:1792`
    `os_panic("immobilized space exhausted")`
  - 対象範囲の明記: `documents/stack-guard.md:3`
    「**対象は JIT 経路だけである。** インタプリタと AOT は守られていない(§4)」
  - Issue #110 の本文はヒープ / Immobilized 枯渇の話(`os_panic`、
    `make-instance` がヒープを要る問題)。**本文が指す対象は未解決**
- 文書: `documents/stack-guard.md`、`documents/jit.md:404`、
  `documents/error-unwind-survey.md:976` §6-2 #8

### #119(for マクロの GC 下永続破損)

- 調査: `documents/for-expansion.md:28-120` §1(再現せず)。
  issue #119 本文に 6 節(根拠文書が存在しない / 再現せず /
  保持された展開形が GC ルートでない / GC 起動点が 4 つだけ / 残る不確かさ /
  閉じてよい条件)
- 回帰試験: `test/lisp/for_gc_stress_test.lisp`、
  `make test-qemu-for-gc-stress`(Makefile:1071。GC_DEBUG=1 を自分で渡す。約 8 分。
  `test-qemu-all` には入れない)、ゲート `test/lisp/gc_debug_guard.lisp`
- 回避の現状(**戻されていない**):
  - `src/lisp/ide.lisp:40-48`(while のまま。「**for へ戻すのは issue #119 を
    閉じてから**」)
  - `src/lisp/fat32.lisp:145-148`(同様)
  - `src/lisp/fat16.lisp` も while
- 閉じる条件(issue 本文 §6)の 2 通りはどちらも未達

---

## 9. G9 — 計器・検出器

### 規則 15 の検出器

- 規則そのもの: `documents/records-and-measurement-rules.md:497-560` §規則 15
  (`e89aee3` / PR #122。**このブランチに入っている**)
- 検出器: **このブランチに無い。**
  - `ls tools/ | grep pr.claims` 0 件、`grep -n "check-pr-claims" Makefile` 0 件、
    `test/fixtures/` 無し
  - `git merge-base --is-ancestor feature/check-pr-claims HEAD` → **NO**
  - 実体は `bf598ba feat(rules): PR 本文の成果物をマージ後に突き合わせる検出器(規則 15)`
    → `tools/check_pr_claims.py`、`make check-pr-claims PR=113 [AT=rev]`、
    `make test` 内の `check-pr-claims-self-test`、`test/fixtures/pr113_body.md`
  - PR #123 は **merged**(`mergedAt 2026-10-29T21:50:20Z`、
    mergeCommit `d8bebd0`、base `feature/compiler-optimization`)
  - `gh api repos/:owner/:repo/branches/feature%2Fcompiler-optimization` →
    `d8bebd0`。**ローカルの `feature/compiler-optimization` と
    `origin/feature/compiler-optimization` はどちらも `964f525` で古い**
  - `make test` のログに `check-pr-claims-self-test` は現れない(実行確認済み)
  - 時刻の対照: `bf598ba` 15:29:22 / `4cef194` 15:35:16
    → **6 分差。分岐元を更新せずに枝を切ったため入っていない**
- 遠隔に作業ブランチは無い:
  `gh api repos/:owner/:repo/branches/feature%2Fjit-frame-rightsize` → 404

### 方位文書 §4 の検出器一覧(17 行)の実在と陽性対照

| 方位文書の記載 | 実在 | 陽性対照 |
|---|---|---|
| `_Static_assert`(表の件数) | あり。`src/c/za.c:885,889,1003,1405,1856,1858`、`src/c/runtime.h:70-78`、`src/c/za_jit_tags.h:43,48,52` | **見つからない**(コンパイル時なので「意図的に落とした」記録なし) |
| `#count`(テスト件数) | **部分的。** 出力は `test/lisp/test_framework.lisp:49`(`#at ~A t=~D += ~D`)と `297`(総数)。方位文書が言う件数検査 `isiki-file-end-check` は **0 件**(`grep -rn isiki-file-end-check test/ src/`) | — |
| `check-macro-order` | **このブランチに無い。** `git grep -n check-macro-order feature/instcount-gate -- Makefile` → `Makefile:290-291` にのみ存在 | — |
| `%%current-inline` 前後比較 | あり。`test/lisp/inline_builtin_test.lisp:16,28,85,90,96,99,101,116,121,122,127,130,132,158` | 汚染しないことの前後比較が本体 |
| `check_eval_error_budget.py` | あり。`tools/check_eval_error_budget.py`。予算は `BUDGETS = {"c_return": 7, "lisp_quote": 2}`(L:114-121) | **あり。** L:310-378(`--self-test`。予算 ±1 で必ず落ちる)。実行で OK |
| `check_jit_bail_lines.py` | あり | **あり。** L:182-184(「za.c を 1 行ずらすと検出器は落ちた」)。実行で OK |
| `check_estimator` | `tools/bench/check_estimator.sh`、`make check-estimator`(Makefile:302) | **あり。** 実行出力「陽性対照: min 系は校正点を外して落ちる → OK(min-slope は落ちた)/ OK(slope-of-mins は落ちた)」 |
| `check_bench_baseline.py` | あり | **あり。** `documents/performance-measurement.md:4022-4084` |
| `jit-frame-breakdown` | `tools/jit_frame_breakdown.py`、`make jit-frame-breakdown`(Makefile:1054) | 合計一致が自己検査。意図的に壊した記録は見つからない |
| 再帰深さ(間接) | `test/lisp/jit_frame_test.lisp:28-65` | `documents/jit-frame-survey.md:367` 付近に陽性対照の節あり |
| 再帰深さ(直接) | `test/lisp/stack_guard_test.lisp:63-85` | 下限 20 / 上限 34 の両側で落ちる形。`documents/stack-guard.md:519-546` |
| `sub`/`add` 整合 | `test/lisp/jit_rsp_balance_test.lisp` | **あり(3 つ)。** §1 参照。`documents/jit-frame-survey.md:664,713` |
| `macro-parity` | `tools/check_macro_parity.sh`、`make test-qemu-macro-parity` | 期待値 `cosmetic` / `DIVERGENT` が「同じになったら落ちる」=双方向。意図的発火の記録は `tools/macro_parity_expected.tsv` ヘッダのみ |
| `jit_limits_test.lisp` | あり | **あり。** L:7,14(「本ファイルはその計器の陽性対照を兼ねる」)。`documents/jit-frame-survey.md:578,630` |
| `check_macro_forward_refs.py` | **このブランチに無い。** `git log --all --diff-filter=A --name-only -- "*macro_forward*"` → `tools/check_macro_forward_refs.py`。`git branch -a --contains 8ead49d` → `feature/instcount-gate` のみ。`git merge-base --is-ancestor 8ead49d feature/compiler-optimization` → **NO** | — |
| `test-qemu-error-escape-gc` | あり。Makefile:916-917、`test/lisp/qemu_boot_error_escape_gc.lisp` | PR #121 本文 L:332 に「期待値を 999 にすると」の陽性対照。今回未実行 |

### 期待値を更新したら PR 本文に書く運用

- PR #121 本文に 4 件の期待値更新が記載(L:446「`check_jit_bail_lines.py` が
  BAIL-LINE 期待値 14 件のずれを捕まえました」、L:467 `disassemble_test.lisp`、
  L:468 `jit_limits_test.lisp` を序数化)
- 予算 6 → 7 の変更は `tools/check_eval_error_budget.py:80-116` に理由つきで記録
  (「observed-but-untested として記録する」「この区別を書いておく」まで含む)
- ただし `documents/error-unwind-survey.md:13` の冒頭注記は「予算 6」のまま(古い)

### documents/ 側は正しく書き分けている

- `documents/error-unwind-survey.md:35-38`
  「次の 2 点だけは `feature/instcount-gate` 側にしか無く、本書では扱いを分けてある。
   - `tools/check_macro_forward_refs.py` と `make check-macro-order`
   - `test_framework.lisp` の `isiki-file-end-check`(ファイル単位の `#count` 検査)」
  → **食い違っているのは方位文書 §4 のみ**

---

## 10. G10 — 回帰と PR

- `make test`: **実行して exit 0 / FAIL 0 件**
  (ログ: scratchpad `make_test.log` 5,823 行)。
  含まれるホスト側ゲート 4 つも通過(Makefile:316 の依存)
- `make test-qemu-all`(Makefile:772-788)の内容:
  `test-qemu` / no_init_signal / **error-escape-gc(GC_DEBUG 再ビルド)** /
  m5_ide / m6_fat16 / fat_callback_transfer / fat32 / fat32_primary_boot /
  partition ×2 / bench-jit / for_semantics / **jit_frame** / jit_limits / stack_guard
  → **未実行**。うち `jit_frame` と `stack_guard` は個別に実行して通過
- PR の状態:
  - `gh pr list --state open` → **#94 のみ**
  - 作業ブランチの PR: 無し(ブランチ自体が遠隔に無い)
  - 分岐元 → main の PR: 無し。`git rev-list --count main..feature/compiler-optimization` = **164**

---

## 11. 参照した Issue / PR 番号

- Issue: #110(open)、#112(open)、#114(open, コメント 1)、#119(open)、
  #97 / #107 / #108 / #109 / #111(open, 範囲外)
- PR: #94(open, WIP)、#113 / #115 / #116 / #117 / #118 / #120 / #121 / #122 /
  #123(いずれも merged)、#87(closed)
- ブランチ: `feature/jit-frame-rightsize`(ローカルのみ)、
  `feature/compiler-optimization`(ローカル・遠隔とも `964f525` で古い。
  GitHub 上の実体は `d8bebd0`)、`feature/check-pr-claims`(`bf598ba`、マージ済み)、
  `feature/instcount-gate`(PR #94。69 コミット遅れ)

---

## 12. 使ったコマンド一覧

```
git log --oneline -40 ; git branch -a ; git status --porcelain
git merge-base --is-ancestor feature/check-pr-claims HEAD
git merge-base --is-ancestor 8ead49d feature/compiler-optimization
git log --oneline feature/compiler-optimization..feature/instcount-gate
git rev-list --count feature/instcount-gate..feature/compiler-optimization
git rev-list --count main..feature/compiler-optimization
git log --all --diff-filter=A --name-only -- "*macro_forward*" "*macro-order*"
git branch -a --contains 8ead49d
git show --stat 4cef194 ; git show --stat bf598ba
git ls-tree -r --name-only feature/instcount-gate | grep -i "macro\|check"
git grep -n "INLINE_BIT_" feature/instcount-gate -- src/c/runtime.h
git grep -n "check-macro-order" feature/instcount-gate -- Makefile tools test
gh issue list --state all --limit 50
gh issue view 110|112|114|119 --json number,title,state,body,comments
gh pr list --state all --limit 30 ; gh pr list --state open --limit 20
gh pr view 94|121|123 --json ...
gh api repos/:owner/:repo/branches/feature%2Fcompiler-optimization
gh api repos/:owner/:repo/branches/feature%2Fjit-frame-rightsize   # 404
grep -n "^[a-zA-Z_-]*:" Makefile
grep -rn "OPTIMIZE_SAFETY|_unchecked|DECLARE_AUDIT|INLINE_BIT|ZA_FRAME_TOTAL" src/c
make test
make check-eval-error-budget check-bench-jit-sync check-estimator check-jit-bail-lines
python3 tools/jit_frame_breakdown.py
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd make test-qemu-milestone \
    MILESTONE=test/lisp/qemu_boot_jit_frame.lisp
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd make test-qemu-milestone \
    MILESTONE=test/lisp/qemu_boot_stack_guard.lisp
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd make test-qemu-jit-bench
```

**変更していないもの**: ソース・テスト・設定・Issue・PR。
`make` が書き換えた生成物は `tmp/` 配下、`test-results.txt`(gitignore 対象)、
`esp_dir`/`images` のビルド成果物のみ。

---

## Phase 5 の確認（2026-10-30）

マイルストン単位の実現済みチェックの根拠。**仮想機械は 1 回も起動していない。**
ベンチ・全回帰も未実行。秒〜分で終わるものだけを走らせた。

### 実行したコマンド

```
# --- 参照の照合（ローカルブランチは動かしていない。fetch のみ）---
git branch -a -vv
git remote -v
git fetch origin --prune
git rev-parse feature/compiler-optimization origin/feature/compiler-optimization
git ls-remote --heads origin | grep -E "jit-frame-rightsize|compiler-optimization"
git log --oneline 964f525..d8bebd0
git rev-list --left-right --count feature/compiler-optimization...origin/feature/compiler-optimization
git rev-list --left-right --count feature/compiler-optimization...feature/jit-frame-rightsize
git rev-list --left-right --count origin/feature/compiler-optimization...feature/jit-frame-rightsize
git rev-list --left-right --count origin/main...origin/feature/compiler-optimization
git rev-list --left-right --count origin/feature/compiler-optimization...origin/feature/instcount-gate
git log -3 --format="%h %ad %s" --date=iso
git status --short
git ls-files | grep -iE "pr_claim|pr-claim|check_pr"
git ls-tree -r --name-only origin/feature/compiler-optimization | grep -iE "pr_claim|check_pr"

# --- GitHub（参照のみ）---
gh pr view 94 --json number,state,title,headRefName,baseRefName,updatedAt,isDraft
gh issue view 110 --json number,state,title,updatedAt,stateReason
gh issue view 112 --json number,state,title,updatedAt,stateReason
gh issue view 114 --json number,state,title,updatedAt,stateReason
gh pr list --state open --limit 30 --json number,title,headRefName,baseRefName,updatedAt
gh pr list --state merged --limit 10 --json number,title,mergedAt,baseRefName

# --- 仕組みの有無（grep / ls）---
grep -nE "^[a-zA-Z0-9_.-]+:" Makefile
grep -rn "JIT_DUMP" --include=Makefile --include="*.sh" --include="*.py" .
find tools -maxdepth 2 -type f | sort
grep -rn "_Static_assert" src/c | grep -iE "frame|rsp|16|align"
grep -rn "ZA_FRAME_TOTAL" src/c
grep -n "% 16|16 の剰余|余りが 8|16 で割" src/c/za.c      # 0 件
grep -rn "376|368" documents/                              # 無関係な一致のみ
grep -rln "死領域|死んでいる領域" documents/ docs/
grep -rn "safety" src/c/*.c src/c/*.h
grep -rn "primitive_[a-z_0-9]*_fixnum" src/c/*.c
grep -rn "storage-exhausted|STORAGE_EXHAUSTED" src/c/ src/lisp/
grep -c "define INLINE_BIT_" src/c/runtime.h
grep -rn "7480" test/lisp/jit_frame_test.lisp test/lisp/jit_rsp_balance_test.lisp
grep -nE "^FOR|\bFOR\b" tools/macro_parity_expected.tsv
grep -n "EXTRA_CFLAGS" Makefile
grep -rn "SMALL_HEAP_SIZE" test/c/runtime_test.c
grep -n "check_pr_claims|前方参照|検出器" HANDOVER_COMPILER_OPTIMIZATION.md

# --- 実行したもの（秒〜分）---
make check-eval-error-budget check-bench-jit-sync check-estimator check-jit-bail-lines
make jit-frame-breakdown
python3 tools/jit_frame_breakdown.py QQ=1     # 合計一致の陽性対照を作れるか試した
```

### 条件 ID ごとの根拠

#### M1-T1（参照の同期と作業ブランチの公開）= 未着手

- `git fetch origin --prune` の出力が `964f525..d8bebd0  feature/compiler-optimization`。
  **ローカル `feature/compiler-optimization` は `964f525`、遠隔は `d8bebd0`。一致していない。**
- 差分 2 コミット = `d8bebd0`（PR #123 のマージコミット）と `bf598ba`
  （`feat(rules): PR 本文の成果物をマージ後に突き合わせる検出器(規則 15)`）
- `git rev-list --left-right --count feature/compiler-optimization...origin/feature/compiler-optimization`
  → `0  2`（ローカルが 2 遅れ）
- `git ls-remote --heads origin` に `refs/heads/feature/jit-frame-rightsize` **は無い**
  （返るのは `feature/compiler-optimization` のみ）。`git branch -vv` でも作業ブランチに upstream 無し
- 差分コミット数: 作業ブランチ `4cef194` は ローカル分岐元 から **1 コミット先**、
  遠隔分岐元 からは **1 先・2 遅れ**
- `origin/main...origin/feature/compiler-optimization` → `0  166`（計画書の 164 から +2）

#### M1-T2（D1 と PR #123 の取り込み）= 未着手

- `git ls-files | grep check_pr` → **0 件**（作業ブランチに無い）
- `git ls-tree -r --name-only origin/feature/compiler-optimization | grep check_pr`
  → `tools/check_pr_claims.py`（遠隔分岐元にはある）
- `Makefile` のターゲット一覧に突き合わせ用のターゲットは無い

#### M1-T3 / M1-T4（PR #94 の取り込みと数字の棚卸し）= 未着手

- `git log -3` の最新は `4cef194 2026-10-29 15:35:16 test(jit): 出口そのものを数える(案 d 第 1 段)`。
  **取り込みのコミットは存在しない**
- `grep -c "define INLINE_BIT_" src/c/runtime.h` → **4**
  （`INLINE_BIT_CAR` / `CDR` / `NULL` / `EQ`。`runtime.h:877-880`）。
  計画書が言う「8 個」は #94 側のもので、入っていない
- `find tools -maxdepth 2 -type f` に `check_macro_forward_refs.py` **無し**。
  `Makefile` に `check-macro-order` **無し**
- `gh pr view 94` → `OPEN` / `headRefName: feature/instcount-gate` / `updatedAt 2026-09-23`
- `git rev-list --left-right --count origin/feature/compiler-optimization...origin/feature/instcount-gate`
  → `71  9`（遅れは 69 → **71**）
- 命令数の計器そのもの（`tools/plugins/isiki_instcount.c`、`Makefile:1111 test-qemu-instcount`、
  `Makefile:1001 test-qemu-construct-bench`、`Makefile:1025 test-qemu-jit-bench`）は
  **現ブランチに既にある**。#94 が足すのは「区間の指定」なので、
  計画書の「①②③の 3 成果物」のうち②は *既存の計器の拡張* である点に注意

#### M1-T5（小さいヒープの再調整）= 未着手

- `test/c/runtime_test.c:2259-2266`:
  `#ifndef SMALL_HEAP_SIZE` / `#define SMALL_HEAP_SIZE (85 * 1024)` / `malloc(SMALL_HEAP_SIZE)`。
  上書きできる形になっている
- `grep -n "EXTRA_CFLAGS" Makefile` → `19`（説明）/ `23`（既定値）/ `82,85`（説明）/
  `91`（`BUILD_FLAG_SIGNATURE`）/ **`235`（カーネルのリンク行）のみ**。
  `Makefile:316` の `test:` ターゲットが並べるネイティブ試験の `gcc` 行（21 本）には
  `EXTRA_CFLAGS` が **1 つも入っていない** → **つまみはビルドの口に繋がっていない**（確認済み）

#### M2-T1（生成物の同一性比較手順）= 未着手

- `Makefile` のターゲット一覧（`grep -nE "^[a-zA-Z0-9_.-]+:" Makefile`）に比較用のターゲットは無い。
  `transpile`（`Makefile:211`）は生成するだけ
- `find tools -maxdepth 2 -type f` に退避・再生成・比較を行うスクリプトは無い
  （`tools/bench/check_inline_expansion.sh` は生成物の *中身* を見る別目的の門番）

#### M2-T2（深さの検出器の範囲と期待値）= 未着手。ただし直書きの所在の列挙は済

- 直接版: `test/lisp/stack_guard_test.lisp:72-85`。探索は `(sgt-max 15 40)` で
  **上端 40 のまま**、帯は `>= 20` / `<= 34` のまま、記録値 27
- 間接版: `test/lisp/jit_frame_test.lisp:28`（`%jit-frame-size`）、
  `:44`（`%jit-frame-probe`）、`:47`（`defglobal jit-frame-size`）。
  **読む対象は `%jit-frame-probe` 1 つだけ**で、ファイル内にその旨のコメントがある
- 直書きの所在（計画書の列挙が実物と一致することを確認）:
  `test/lisp/jit_frame_test.lisp:63`（`(assert-equal t (if (<= jit-frame-size 7480) t nil))`）、
  `test/lisp/jit_rsp_balance_test.lisp:263,264`（`(assert-equal 7480 (%jrb-fsz ...))`）。
  **行番号まで一致** → 「所在が列挙されている」は実質達成

#### M2-T3（方位文書の訂正と陽性対照 4 件）= 未着手。1 件は「作り方」が判明

- `HANDOVER_COMPILER_OPTIMIZATION.md:125` に `check-macro-order`、
  `:137` に `check_macro_forward_refs.py` が検出器として挙がっている。
  どちらも現ブランチに存在しない（上記 M1-T3 の確認）→ 食い違いは未訂正
- `_Static_assert` 群の所在: `src/c/runtime.h:384,385,387`、`src/c/za.c:885,889,1003`、
  `src/c/runtime.c:192,197`。**フレーム総量に対するものは 1 つも無い**
- `jit-frame-breakdown` の合計一致の陽性対照:
  `python3 tools/jit_frame_breakdown.py QQ=1` を実行 → 総量 7480 → 5456、
  予備領域 24432 → 22352 と動いたが、**「内訳の合計は ZA_FRAME_TOTAL と一致している」も
  「予備領域は下限を満たし、各項の合計と一致している」も通ったまま**（終了コード 0）。
  つまみ（`tools/jit_frame_breakdown.py:74-92` の `overrides`）は両辺を同時に動かすので
  **陽性対照にならない**。作るには内訳と総量の一方だけを動かす細工が必要
- `error-escape-gc`: `Makefile:916 test-qemu-error-escape-gc`（`GC_DEBUG=1` の別ビルド）。
  **実行していない**（別ビルド + ゲスト起動のため今回の制約の外）

#### M2-T4（G5 の網羅調査と結論）= 一部実現（前回と同じ）

- `documents/declare-types.md:321-343` §5-4「AOT トランスパイラ（`src/lisp/transpile.lisp`）は
  declare を扱わない」= `transpile-defun` の「bodyは単一式のみ対応です」でビルドが止まる、
  当面はこのままにした旨が記録済み
- 同文書 `:40,54-61,105-111,271-288` には **JIT 側で declare を剥がす位置の決定**が記録されている。
  AOT 側で剥がすかどうかの判断は見つからなかった → M2-T4 の 3 番目の条件は未達

#### M2-T6（食い違いの整理と死領域の記録）= 未着手

- `grep -rn "376|368" documents/` の一致はすべて無関係
  （`unbound-arity-survey.md:178` の計測値、`investigation-defun-page-cost.md:62` の 368,816 など）
- `grep -rln "死領域|死んでいる領域"` の一致は
  `docs/investigation/compiler-optimization.logic.md` / `.refs.md` / `docs/plans/compiler-optimization.md`
  のみ → **「事実の正」とされる `documents/` には無い**
- 推定量の記述: `make check-estimator` の出力で `median` が `<= 選択中` と表示されることを確認。
  文書側の記述の訂正は別途（今回は文書を変更していない）

#### M2-T7（生成コードのバイト列比較手順）= 未着手

- `grep -rn "JIT_DUMP"` の一致は `Makefile:67`（説明）/ `:72`（`JIT_DUMP_FLAGS` の定義）/
  `:91`（ビルドの印に含める）/ `:235`、`:277`（コンパイル行）の **5 件だけ**。
  **ハッシュを取る / 2 状態を比較するスクリプトもターゲットも無い**

#### M3-T2（16 の剰余条件の検査化）= 未着手

- `src/c/za.c:1744` `#define ZA_FRAME_TOTAL (0x28 + ZA_FRAME_EXTRA)`。
  この定数に対する `_Static_assert` は無い（`grep -rn "ZA_FRAME_TOTAL" src/c` の全 14 件を確認）
- `src/c/za.c:1730-1740` のコメントに「16の倍数」「16バイト境界の関係を変えない」と書かれているが、
  `grep -n "% 16|16 の剰余|余りが 8|16 で割" src/c/za.c` は **0 件** → 検査になっていない

#### M3-T3 / M4（並べ替えと実寸化）= 未着手

- `make jit-frame-breakdown` の出力（2026-10-30 実行）:
  `ZA_OFF_ENV_VAL 16` から `ZA_OFF_SAVED_R14 7424/16` までの並びは前回と同一。
  `ZA_FRAME_EXTRA = 7440` / `ZA_FRAME_TOTAL = 7480` / 256KB で 35 段。
  `ZA_OFF_QQ_BASE 4816 / 2496`、`ZA_OFF_FLET_BASE 3984 / 64` + `ZA_OFF_FLET_OLD_BASE 4048 / 768`（= 832）
- 予備領域: `ZA_STACK_PER_FRAME 7536` / `ZA_SIGNAL_PATH_MAX 8696` /
  `ZA_HANDLER_ALLOWANCE 4096` / `ZA_STACK_HARD_FLOOR_WANT 4096` / 下限 24424 /
  **`ZA_STACK_SIGNAL_RESERVE 24432`**（M4-T7 の「前」の値として使える）
- 寸法の即値は `src/c/za.c:4930,4954,6833`（`jit_add_rsp_imm32(ZA_FRAME_TOTAL)`）、
  `:6995,7025`（`jit_sub_rsp_imm32(ZA_FRAME_TOTAL)`）で **コンパイル時定数を直接出力**。
  後埋め機構は無い

#### M4-T8（寸法を区別して読む計器）= 未着手だが「区別して返す」能力は既にある

- `test/lisp/jit_rsp_balance_test.lisp:106`（`%jrb-audit f`）→ `:158` が
  `(list nsub nadd bad fsz npop orphan nexit exorphan nteardown npreframe)` を返し、
  `:163` の `%jrb-fsz` がそこから寸法を取り出す。**引数は関数オブジェクト**
- 同ファイル `:235-259` では **9 つの試験関数**（`%jrb-leaf` `%jrb-plain` `%jrb-tail` `%jrb-rec`
  `%jrb-nlx` `%jrb-catch` `%jrb-tagbody` `%jrb-qq` `%jrb-flet`）に対して他の指標を読んでいる。
  寸法だけが `:263,264` の 2 関数に限られているのは **呼び出し側の都合**
- `test/lisp/jit_frame_test.lisp:28` の `%jit-frame-size` も引数は関数オブジェクト
- → 「どの関数のものかを区別して返す」は **手段としては達成済み**。
  残るのは呼び出しの増設と `7480` の置き換え、および大小 2 値の差を示す試験

#### M5（性能回帰）= 未着手

- `gh issue view 114` → `OPEN` / `updatedAt 2026-09-29T05:43:24Z`
- `make check-eval-error-budget check-bench-jit-sync check-estimator check-jit-bail-lines`
  → 終了コード 0。内訳:
  - EVAL-ERROR 予算 7（C）/ 2（Lisp）で一致
  - `bench_jit.lisp` と `bench_aot.lisp` の同期 OK
  - 校正点 `c/for` 真値 4.000: `median 3.946`（選択中・OK）、
    陽性対照 `min-slope 2.780` / `slope-of-mins 2.780` は **落ちた**（意図どおり）
  - BAIL-LINE 期待値 15 件一致、陽性対照（1 行ずらす）で落ちた
- 直しのコミットは無い（`git log -3`、`git status --short` に変更なし）

#### M6（safety 0）= 未着手

- `grep -rn "safety" src/c/*.c src/c/*.h` の一致:
  `src/c/runtime.h:577,857`（シンボルと詰め方の定義）、`src/c/runtime.c:71,1666,1709,2107,3042,3213`
  （シンボルの作成・GC・取り出し）、`src/c/eval.c:533,617`（`declaim` の解釈）、
  `src/c/runtime.c:7340-7344`（「将来 safety 0 のときだけ…」という *計画のコメント*）、
  `src/c/za.c:1771`（「この検査は safety で外せない」という *注記*）。
  `src/c/mount.c:674` は無関係
- **生成の判断に safety の値を使っている箇所は 1 つも無い**
- `gh issue view 112` → `OPEN` / `updatedAt 2026-09-29T02:31:44Z`
- D7 の材料: `documents/declare-typed-arith.md:81-94`（§2-2 `_unchecked` への分解）、
  `:672`（「`_unchecked` が外しているのは『タグ検査』だけである」）。
  `EVAL-ERROR` については `:707-715`（**ゼロ除算が値として返す**話）と `:821` のみで、
  **無検査経路に流れてきた場合の扱いは未記録**

#### M7（型特化とインライン展開）= 未着手

- `grep -rn "primitive_[a-z_0-9]*_fixnum" src/c/*.c` の固有名は
  `primitive_add2_fixnum` / `primitive_subtract2_fixnum` / `primitive_multiply2_fixnum` /
  `primitive_divide2_fixnum` の **4 つだけ**（`src/c/disasm_symtab.c:5845,5848,5853,5856` に登録）。
  `src/c/za.c:3080,3083,3086,3089` が宣言された型から呼び先を選ぶ
- 比較側: `primitive_less_than2` / `primitive_greater_than2` / `primitive_num_equal` /
  `primitive_greater_than` / `primitive_greater_equal` は存在するが
  **`_fixnum` 付きは 1 つも無い** → 特化関数は 0 件（前回確定の再確認）
- 宣言で入り切りする展開の対象は `INLINE_BIT_` 4 つ（car / cdr / null / eq）で算術は含まれない

#### M8（解釈実行の計器とヒープ枯渇）= 未着手

- `grep -rn "storage-exhausted|STORAGE_EXHAUSTED" src/c/ src/lisp/` の一致:
  `src/lisp/init.lisp:605`（クラス定義）、`src/lisp/init_aot.lisp:705`（移動の注記）、
  `src/c/za.c:1756,1855,1894,1915`（**スタック側の signal のみ**）。
  **ヒープ側で signal する箇所は 0 件**
- `gh issue view 110` → `OPEN` / `updatedAt 2026-09-29T02:30:59Z`
- 解釈実行を測る行・理由別の数え上げは `tools/bench/bench_baseline.tsv` と
  `tools/bench/` の構成から見て存在しない（`find tools -maxdepth 2 -type f` の一覧に該当なし）

#### M9（展開形の統一と main への PR）= 未着手

- `tools/macro_parity_expected.tsv:23` の `FOR` が `cosmetic` のまま
  （`ZA_MAX_LOCALS_PER_LET=4` が理由であることも同じ行に書かれている）。
  表全体は `same` 10 件 / `cosmetic` 2 件
- `gh pr list --state open` → **#94 のみ**。main へ出す PR も作業ブランチの PR も無い
- `origin/main...origin/feature/compiler-optimization` → `0  166`

### 公開側の状態（参照のみ）

- `gh pr list --state open --limit 30` → #94 のみ
- `gh pr list --state merged --limit 10` → 最新が #123（2026-10-29T21:50:20Z）。
  **2026-10-29 以降に新しい PR は無い**
- `gh issue view 110 / 112 / 114` → すべて `OPEN`、`stateReason` 空、最終更新は 3 件とも 2026-09-29
- #119 は指示により確認対象外

### 変更していないもの

ソース・テスト・設定・Issue・PR・**ローカルの git 参照**。
`git fetch origin --prune` で遠隔追跡参照（`refs/remotes/origin/*`）のみ更新した。
`make jit-frame-breakdown` と `python3 tools/jit_frame_breakdown.py QQ=1` は
一時ディレクトリに C を吐いてコンパイルするだけで、リポジトリのファイルは書き換えない。
`make check-*` 4 つも読み取りのみ。
新規に書いたのは `docs/investigation/compiler-optimization.check.md` への追記と本ファイルへの追記だけ。

---

## 参照している一時文書について（2026-10-30 追記）

`HANDOVER_COMPILER_OPTIMIZATION.md`（方位文書）と `JIT_FRAME_RIGHTSIZE.md`（案 d 指示書）は
**git 管理外**で、2026-10-30 に削除した（claude.ai で設計していた内容を Claude Code へ移行するための
一時文書だった）。**内容は `docs/plans/compiler-optimization.md` と本調査ファイル群に取り込まれている**ので、
本文中のこれらへの参照は「出典の記録」として読むこと（ファイルは存在しない）。
