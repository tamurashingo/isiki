# 参照マップ —— 合否の一行の偽造(#205)と打ち切りが残らないこと(#197)

論理要約: `docs/investigation/detector-t4-passline.logic.md`。
**節番号は論理要約と対応**(§9-5 の (A)〜(E) もそのまま使う)。

前提:
- **コードは変更していない。QEMU は 1 回も起動していない。**
  実行したのは `python3 tools/census_test_counts.py --inventory`
  (合否に影響しない読み取り専用の道具)のみ
- 行番号は 2026-11-09 の `feature/m7-typespec`(`5278e5f` 時点)
- 事実には行番号を添えた。**★ 未検証** は読みだけで、実行していないもの
- **件数は手で数えていない**(規則 13)。数えた手段を §A-0 / §B-0 に置き、
  本文はその出力を引く

先行調査: `docs/investigation/detector-integrity.logic.md` / `.refs.md`(#197/#200 の T0)。
本文書はその §A-3 / §A-4 / §B / §C を**検索式つきで再導出し、落ちていたものを足した**ものである。

---

## §A 合否の一行 —— 書く側(#205)

### A-0 導出の手段(規則 13)

```sh
# 書く側(= 文字列を組む箇所)。docs/ と documents/ は記録なので除く
grep -rn '==== isiki tests' --include='*.lisp' --include='*.py' --include='*.sh' \
     --include='*.c' . | grep -v '^./docs/' | grep -v '^./documents/'

# C 側で要約を組む箇所(文言が違うので別に取る)
grep -rn '" passed, "' src/c/

# 読む側(= 判定・分類)
grep -rn '0 failed' Makefile tools/ .github/
```

**この 3 本の検索の和集合が、合否の一行に関わる全箇所である。**
以下の表はその出力を分類しただけで、手で数え足したものは無い。

### A-1 書く側の分類(論理要約 §3 / §9-1 / §9-2 / §9-3)

| 分類 | 箇所 | 根拠にしている数 |
|---|---|---|
| **本物(枠組み)** | `test/lisp/test_framework.lisp:343-344` | `*isiki-test-pass*` / `*isiki-test-fail*`(`:12-13`)。増やすのは `assert-*` の展開形(`:195` `:198` `:210` `:213` `:255` `:258` `:300` `:303` `:308`)と中断検出(`:183` / `:333`) |
| **本物(C / 9P の無い起動)** | `src/c/kernel.c:316-321` | `g_smoke_pass` / `g_smoke_fail`(`:204-205`)。1 件ずつ `smoke_ck`(`:208-218`)で加算。**これは本物の集計である** |
| **自前集計 1**(枠組みが使えない前提) | `test/lisp/qemu_boot_no_init_signal.lisp:114` | 自前の `*pass*`/`*fail*`(`:31-32`)+ `ck`(`:34-43`)。理由は `:27-28` |
| **自前集計 2**(同) | `test/lisp/qemu_boot_m1_cost.lisp:81` | 自前の `*pass*`/`*fail*` + `ck`(`:52`)。`(ck ...)` は `:74-79` の 6 件。理由は `:15-18` |
| **偽造 1(計測プローブ)** | `tools/mandel_run.lisp:47`(理由は `:22`) | **固定文字列。** 検査は 1 件も走らない。`git ls-files` に載っている(ファイル先頭 `:12-16` は「追跡しない」と書いているが**実際は追跡されている**) |
| **偽造 2(計測プローブ)** | `tools/gen_jit_nest_probe.py:47-49` | 同上。生成する Lisp に書かせる |
| **合成資料 1(判定役の自己試験)** | `tools/run_qemu_milestone.sh:76-82` | `--self-test`(`:53-88`)が 4 経路を発火させるための偽の成果物。`:78` は**不合格側**の資料 |
| **合成資料 2(比較器の駆動側)** | `tools/jit_dump_identity.sh:198` | 偽のシリアルログ(`:191-199`)。`test-results.txt` ではなく `$box/serial.txt` |
| **合成資料 3(比較器本体)** | `tools/check_jit_dump_identity.py:383` / `:395` | `--self-test` の雑音耐性・ガード 1 用。一時ディレクトリへ書く |

> **偽造 2 件と合成資料 3 本という内訳は、issue #205 本文の「偽造 3 / 合成資料 3」と
> 1 件ずれる。** 先行調査の `detector-integrity.refs.md:215-221` は
> 判定役の自己試験(`run_qemu_milestone.sh:76-82`)を表に載せておらず、
> 代わりに比較器の 2 ファイルを 1 行にまとめていた。
> **どちらが正しいかは分類の粒度の問題なので、本文書は「件数を書かず、
> 上の検索式の出力を分類する」形に替えた**(規則 13)。
> 設計の対象は「本物でない 5 箇所」で、うち**判定を通すために書いているのは 2 箇所**
> (偽造 1 / 偽造 2)、**残り 3 箇所は判定役にかけるための資料**である。

### A-2 集計変数を外から書き換えられるか(論理要約 §5-4 / §9-1)

```sh
grep -rn 'isiki-test-pass\|isiki-test-fail\|isiki-test-attempt' \
     --include='*.lisp' . | grep -v '^./test/lisp/test_framework.lisp:'
```

| 方向 | 箇所 | 評価 |
|---|---|---|
| **増やす(安全側)** | `test/lisp/bench_jit_guard.lisp:55`、`test/lisp/bench_interp_guard.lisp:64`、`test/lisp/gc_debug_guard.lisp:36` | ゲートが落ちたことを合否へ伝える正当な用途。**不合格を増やす向きなので偽造にならない** |
| **戻す(危険側)** | `test/lisp/test_framework_test.lisp:37-40`(`tf-capture` `:25-41`) | **正当。** 陰性対照が出した不合格を退避値へ戻す。意図は `:11-12` / `:20-21` |
| **戻す(危険側)** | `test/lisp/bench_interp_test.lisp:95-101` | **正当。** 陽性対照が増やした 1 件を戻す。意図は `:99-100`。増分 1 件の確認は戻した後(`:105`) |
| **戻す(危険側)** | `test/lisp/bench_jit_guard_test.lisp:48-54` | **正当。** 同じ形 |

事実:

- `*isiki-test-pass*` / `*isiki-test-fail*` / `*isiki-test-attempt*` は
  `defglobal`(`test_framework.lisp:12-13` / `:177`)なので、
  **どの試験ファイルからでも `setq` で任意の値に書ける。**
  `test_framework_test.lisp:23-24` が「`defglobal` なので関数の中からの `setq` が
  呼び出し元にも見える」ことを明記している
- **`*isiki-test-pass*` を外から書いている箇所は `tf-capture` の復元 1 つだけ**
  (上の検索の出力より)
- **正当な 3 箇所はいずれも「自分が入る前の値へ戻す」形で、総量を下げてはいない。**
  ただし**構造上は総量の書き換えと同一**で、枠組み側に区別する仕掛けは無い

**★ 未検証(設計判断)**: 区別の材料になりうるもの。
(1) 枠組みが「退避 → 復元」を関数として提供し、復元の回数と件数を成果物へ 1 行出す
(= 乱用が差分に現れる)。
(2) 復元は「直前に自分が増やしたぶん以下」しか許さない(下限を覚える)。
(3) `*isiki-test-attempt*` は復元しない(= 中断検出に必ず乗る)。
いずれも未実装・未測定。

### A-3 `*isiki-test-stream*` を差し替え・再オープンできるか

| 事実 | 箇所 |
|---|---|
| ストリームも `defglobal` なので差し替えられる | `test_framework.lisp:11`、差し替えの実例は `test_framework_test.lisp:31` / `:37` |
| **`tf-capture` は `funcall` が打ち切られると復元しない**(`:32` の直後に復元が無く、`:37-40` は `let` の本体の末尾にある) | `test_framework_test.lisp:32-40` |
| その場合、以後の全出力(合否の一行を含む)が文字列ストリームへ行き、成果物には届かない → 判定役は「成果物はあるが合格の文字列が無い」で**落とす**(安全側) **★ 未検証** | `tools/run_qemu_milestone.sh:115-122` |
| 同じ成果物を**開き直す**と切り詰められる。枠組みの冒頭がその注意を書いている | `test_framework.lisp:6-9`。実例は `tools/mandel_run.lisp:41`、`tools/gen_jit_nest_probe.py:48` |

### A-4 合否の一行を**読む側**(= 判定の口。**3 つに分かれている**)

| # | 箇所 | 対象 | 「出なかった」ときの扱い |
|---|---|---|---|
| 1 | `tools/run_qemu_milestone.sh:113-123` | `test-results.txt` | **成果物なし → 1 回やり直し(`:126-144`)、2 回目も無ければ `exit 1`。成果物あり・文字列なし → やり直さず `exit 1`(`:118-122`)。** 成功には倒れない |
| 2 | `Makefile:1226-1228` | `test-results.txt` | `test -f` → `cat` → `grep -q`。**`test-qemu` は判定役を通らず自前。** やり直しの区別は無いが、どちらも make が落ちる |
| 3 | `Makefile:1177-1181` | `$(BUILD_TMPDIR)/no9p.serial.txt` | **`'[no9p] 8 passed, 0 failed'` と件数まで固定。現存する唯一の件数照合。** 件数を増やしたら直す旨が `:1162-1163` |
| — | `Makefile:1419` / `:1446-1448` | 監査の実行 | **意図的に検証しない**(その旨 `:1419-1421`)。分類は `tools/bench/parse_paint_audit.py:80-96` が行い、要約の行が無い場合は `:88-90` で `INCOMPLETE`。**成功には倒れない** |
| — | `tools/bench/bench_common.sh:42-47` | ベンチのゲート 1 | `make test-qemu-milestone` の終了コードに乗るだけ(= #1 を使う) |
| — | `.github/workflows/ci.yml:33-82` | 自動検査の 12 段 | すべて `make test-qemu-milestone` = #1。各段に `timeout-minutes` があるので固まれば段が落ちる |

**「出なかった」を成功と読む経路は、この表の範囲では見つからなかった。**
`qemu-system-x86_64` を起こす箇所は `grep -rn 'qemu-system-x86_64' Makefile tools/` で
6 件、うち `Makefile:1118` / `:1128` は `run` / `debug`(対話。判定しない)、
残り 4 件は上の表に入る。

### A-5 **判定は部分文字列である**(論理要約 §9-1 の 3 番目)

事実:

- `tools/run_qemu_milestone.sh:115` は `grep -q " 0 failed" test-results.txt`。
  **行の形も位置も見ていない。合否の一行と結び付いていない。**
  `Makefile:1228` も同じ
- 先頭の空白のおかげで `"10 failed"` `"20 failed"` には当たらない(確認済み: `" 0 failed"` は
  `0` の直前に空白を要求する)
- **しかし「成果物のどこかの行に `" 0 failed"` があればよい」ので、
  本物の合否の一行が `"5 failed"` でも通る ★ 未検証**(実行していない)。
  現にそういう行を出している箇所は無い(上の A-0 の 3 本目の検索の出力より)

突き方(§9-5 の派生): `[NG]` 行は `~S` で式をそのまま印字する
(`test_framework.lisp:203-204`)。期待値に `" 0 failed"` を含む文字列リテラルを置いた
アサーションを 1 件落とすと、`[NG]` 行にその文字列が出る。**★ 未検証。**

### A-6 偽造を止めたときに落ちるもの(移行の順序。論理要約 §8-1)

| 直す対象 | 箇所 | 取りうる形 |
|---|---|---|
| 偽造 1 / 2(計測プローブ) | `tools/mandel_run.lisp:41-49`、`tools/gen_jit_nest_probe.py:47-49` | **最小の形(テンプレート)を用意する。**§D を参照 |
| 合成資料 1〜3 | `tools/run_qemu_milestone.sh:76-82`、`tools/jit_dump_identity.sh:191-199`、`tools/check_jit_dump_identity.py:383,395` | **対象外印。** 1 と 2〜3 は性質が違う(1 は判定役そのものの資料、2〜3 は別の比較器の資料で `test-results.txt` ではない) |
| 自前集計 1 / 2 | `qemu_boot_no_init_signal.lisp:114`、`qemu_boot_m1_cost.lisp:81` | 件数の行を自分で出す / 対象外印。**共通の枠組みへの変更は届かない** |
| 件数固定の判定 | `Makefile:1177` | 既に件数を見ている。厳しくするときの**前例** |

---

## §B 件数の行 —— 誰も読んでいない(#197 / 論理要約 §5-8, §5-9)

### B-0 導出の手段

```sh
grep -rn '#count' Makefile tools/ .github/ test/     # 消費者を探す
grep -rn '#at '   Makefile tools/ .github/           # 進捗の行の消費者を探す
```

### B-1 出力と消費

| 問い | 答え(事実) | 箇所 |
|---|---|---|
| 何を数えているか | **試験ファイル 1 本ごと。** 入口から出口までの `pass + fail` の増分 | `test_framework.lisp:72-73`(`before-count`)、`:237`(`n`) |
| どこへ出るか | **成果物のみ。** 画面にも直列線にも出ない | `:238` が `*isiki-test-stream*` 宛 |
| 出るのはいつか | 件数つきの読み込み(`isiki-test-load` `:69-83`)の出口。**素の `(load ...)` では出ない** | `:82` → `:236-240` |
| 照合している箇所 | **ゲスト側 1 つだけ**(`:240` の `(> n 0)`)。**母艦側は 0 件** | 上の `grep` の出力は `Makefile:1664`(コメント)と `tools/census_test_counts.py` だけ |
| その道具は呼ばれているか | **呼ばれていない。** 自分でそう書いてある | `tools/census_test_counts.py:6`(「`make` からも CI からも呼ばれない。合否に一切影響しない」) |
| 進捗の行(`#at`)の消費者 | **0 件。** `grep` の出力が空 | `test_framework.lisp:46-53` が出すだけ |
| 進捗の行は常に出るか | `*isiki-test-progress*` が `t` のときだけ(`:35` で `t`)。**これを `nil` にしている箇所は在庫に無い**(`grep -rn 'isiki-test-progress'` の出力は枠組みの中だけ) | `:35` / `:47` |

### B-2 **進捗の行と件数の行は 1 対 1 になる**(未使用の不変条件)

事実:
- `isiki-test-load` は入口で `isiki-test-mark`(`:70` → `#at`)を出し、出口で
  `isiki-file-end-check`(`:82` → `#count`)を出す
- **したがって片方だけがある = そのファイルが丸ごと落ちた**(§9-5 (D))
- 見ている者は**ゲストにも母艦にも 1 人もいない**(B-1 の `grep` の出力)

### B-3 出口の検査が捕まえられる範囲

| 形 | `#count` の値 | 捕まるか | 箇所 |
|---|---|---|---|
| 全部走った | 本来の数 | — | `:237-238` |
| **大きな 1 つの式の途中で失敗(§9-5 (C))** | **0 < n < 本来の数** | **捕まらない**(`(> n 0)` しか見ない) | `:240`。実例の記録は `documents/load-path-contract.md:149-175`、`Makefile:1660-1665` |
| ファイルの全アサーションが落ちた式の中にあった | 0 | **捕まる**(`:240` が不合格になる) | `:240` |
| **ファイルが丸ごと落ちた(§9-5 (D))** | **行が出ない** | **捕まらない** | `:82` に到達しない |
| アサーションの中で中断 | 本来の数(中断分は次の入口で `fail`) | **捕まる** | `:180-188` / `:331-335` |

補足(事実): `#count` は `:237` で `n` を確定して `:238` で出し、`:239-240` の
アサーション 2 件はそのあとに走る。**この 2 件はどの `#count` にも入らず、合計にだけ入る**
(`tools/census_test_counts.py:22-25` が同じことを書いている)。
最後のアサーションが中断していた場合、その `fail` は `:239` の `isiki-test-begin` が
拾うので**次のファイルの `#count` 側に乗る。**

### B-4 台本が読み込みの戻り値を見ている箇所

```sh
grep -rn 'isiki-test-load\|(load "' test/lisp/qemu_boot_*.lisp | grep defglobal
```

**出力は 1 件**: `test/lisp/qemu_boot_test.lisp:29-30`
(`*isiki-test-syntax-load-result*` へ受けて `:30` で `assert-equal t`)。
理由は `:25-28`、対象側の理由は `test/lisp/isiki_test_syntax.lisp:7-10`
(「構文エラーで中断した場合に『0 failed』で通ってしまわないように」)。
**残りの読み込み行(同ファイル `:4-28` / `:32-78` ほか)は戻り値を見ていない。**

ただし事実として、**この見張りは「`load` が異常終了したこと」しか捕まえない**:
`load` は打ち切りがあっても `g_sym_t` を返すので(§C-1)、§9-5 (C) は通る。

---

## §C 打ち切り件数 —— 今どこまで届くか(#197)

### C-1 経路

| 項目 | 箇所 |
|---|---|
| 件数を返す口 | `src/c/load.c:45-46` の第 4 引数 `int *out_aborted_count`。契約は `src/c/load.h:22-37`(`:24-26` が「打ち切りは失敗にならない」、`:33-34` が「不要なら 0 を渡す」) |
| 加算する箇所 | `src/c/load.c:117-119`。**`if (out_aborted_count != 0)` で囲まれている = `0` を渡した呼び元では加算自体が起きない** |
| 打ち切りの判定 | `src/c/load.c:101-104`(`os_eval_top_level_ex` の `out_aborted`) |
| 理由の出力先 | `src/c/load.c:30-43` `print_load_abort` → `proc->stdout_buffer`(= 画面)。型は `src/c/process.h:24` |
| **画面は母艦へ出ない** | `grep -rn 'stdout_buffer' src/c/*.c` の出力に直列線への写しは無い。直列線へ書くのは `src/c/interrupt.c:191-193` `os_diag_serial_write` で、`stdout_buffer` とはつながっていない。`test-qemu-milestone` の QEMU 行(`Makefile:1474-1484`)に `-serial` は無い(`-display none` だけ) |
| **件数の唯一の消費者** | `src/c/kernel.c:160-183` `load_embedded_lisp`。`:165` で受け、`:170-174` で非 0 なら `os_panic`。意図は `:146-156` |
| 使っていない呼び元(すべて `0`) | `src/c/load.c:166`(相対 = 9P)/ `:183`(マウント → 9P)/ `:214`(FAT 経由)。`:164-165` に「打ち切り件数は受け取らない」と明記 |
| 台本自身の読み込み | `src/c/kernel.c:94-95` `run_qemu_boot_test` → `cc_load`。**戻り値も捨てている**(代入していない) |
| 試験枠組みが使う読み込み | `test/lisp/test_framework.lisp:74` の `(load path)` → `cc_load`(`src/c/load.c:129-`) → `os_load_from_stream(..., 0)`。**件数は届かない** |
| **Lisp 側から読む口** | **無い。** `grep -rn 'aborted' src/c/*.c` の出力に組み込み関数・診断関数は無い |
| 既存の単体試験(設計の足場) | `test/c/load_test.c:339-402`。`:374-375` が陰性対照(打ち切り 0)、`:386-399` が陽性対照(打ち切り 1。`(return-from %top-level 9)` で起こす) |
| 近い前例(C で数えて母艦へ出す) | `src/c/kernel.c:204-218` + `:316-321`(直列線へ `" passed, "` / `" failed"`) |

### C-2 **打ち切りとして数えられない非局所脱出がある**(論理要約 §9-4-4)

事実:
- `src/c/eval.c:1348-1358` —— 打ち切り(`*out_aborted = 1`)になるのは
  **宛先が `%TOP-LEVEL` で、かつ `MAGIC_BLOCK_EXIT`** のときだけ。
  `:1351-1352` が「打ち切りの判定だけは `MAGIC_BLOCK_EXIT` に限る =
  `%abort-top-level` が作るものだけ」と明記
- `src/c/eval.c:1024-1027`(`eval_throw` の注記) —— 「対応する `catch` が動的に外側に
  無い場合の扱いは未実装(仕様上は control-error、**本実装ではシグナルがそのまま
  最上位まで伝播する**)」
- したがって捕まらない脱出が `os_load_from_stream` に戻ると、
  `src/c/load.c:104` の `if (aborted)` に入らない →
  **件数も増えず、`print_load_abort` も呼ばれない**(画面にも出ない)

**★ 未検証**: `go` / `tagbody` 由来の脱出が同じ分類に入るかは読んでいない。
宛先が生きていない `return-from` は `src/c/eval.c:955-960` が `<control-error>` を
signal するので**打ち切り側(= 数えられる)に入る**と読める。

### C-3 「静かに半分欠ける」が成立するか(論理要約 §9-5)

**成立する。** 2 通りある。

| | 形 | 成果物に残る痕跡 | 判定 |
|---|---|---|---|
| **(C)** | 試験ファイルの本体が 1 つの最上位フォームで、その前半で失敗 | `#count` が小さくなる(`> 0` なら通る) | **合格になる** |
| **(D)** | 試験ファイルが丸ごと読めない(構文誤り / 置き場所に無い / 割り当てが解けない) | **`#at` だけがあり `#count` が無い** | **合格になる** |

(D) の経路の裏づけ(すべて事実):
1. `test_framework.lisp:74` の `(load path)` → `cc_load` → 構文誤りなら
   `src/c/load.c:74-77` が `os_signal_io_error`、ファイルが無ければ `:162`、
   割り当てが解けなければ `:220`
2. `os_signal_io_error`(`src/c/runtime.c:5743-5769`)は `<simple-error>` を signal する
3. ハンドラが無ければ最上位への中断になり、**台本側**のフォームを評価していた
   `os_eval_top_level_ex`(台本を読んでいる `os_load_from_stream`、
   `src/c/kernel.c:95` 経由)が `aborted = 1` を受ける
4. そこの `out_aborted_count` は `0`(`src/c/load.c:166`)→ **加算されない**
5. 理由は `print_load_abort` で画面へ(`src/c/load.c:120` → `:30-43`)→ **母艦に出ない**
6. 台本の残りのフォームは読まれ続け、`isiki-test-report`(`:329-344`)は走る
7. `*isiki-test-attempt*` は進んでいない(`isiki-test-begin` を通っていない)ので
   中断検出(`:331`)にも乗らない

**★ 未検証(ただし構造から確定と見る)**: 上の 1〜7 はすべて実装から読んだもので、
実機で再現していない。再現するなら「`test/lisp/qemu_boot_test.lisp:24` が読む
ファイルの末尾に閉じ括弧 1 つを足し、`make test-qemu` が通ることを見る」が最短。
**QEMU を使うので今回はやっていない。**

### C-4 9P の無い起動の段も「読めたこと」で判定している

`src/c/kernel.c:306-307`:
`smoke_ck("app-disk-load", SMOKE_EVAL("(load \"/app/hello.lisp\")", &value) && value == g_sym_t)`。
**`load` は打ち切りがあっても `g_sym_t` を返す**(`src/c/load.h:24-26`)ので、
この判定は半分欠けても通る。
ただし次の `:311-312`(`(isiki-hello)` が 42 を返すこと)が中身の評価を見ているので、
**この段は結果として守られている**(`:309` にその意図が書かれている。`:303-304` が完了条件の宣言)。
= **「読めた」ではなく「中身が効いている」を見るのが既存の作法である。**

---

## §D 計測プローブの最小の形(テンプレート。#205 の完了条件の 1 つ)

現状 2 本が偽造している(§A-1)。**両者が実際に必要としているのは次の 3 つだけ**
(それぞれのファイルを読んで確認):

| 要求 | 現状の実装 |
|---|---|
| 判定役(`tools/run_qemu_milestone.sh:115`)を通ること | 合格の一行を自分で書く |
| 測った値を母艦へ残すこと | `tools/mandel_run.lisp:42-46` は `#mandel` 行、`tools/gen_jit_nest_probe.py:44-46` は別ファイル `/9p/tmp/jit_nest_probe.txt` |
| 枠組みに依存しないこと | `tools/mandel_run.lisp:18-21` が理由を書いている(**古い `main` と同じドライバを使うため**) |

**★ 未検証(設計判断)**。取りうる最小の形は 2 つ:

- **(D-1) 枠組みを使う。** `(load "test/lisp/test_framework.lisp")` →
  不変条件を 1 件以上 `assert-equal` →`(isiki-test-report)`。
  これで件数も合否も本物になる。**`tools/mandel_run.lisp` だけは採れない**
  (`:18-21` の「古い `main` には `isiki-file-end-check` が無い」という制約)
- **(D-2) 自前集計の既存の形に揃える。** `test/lisp/qemu_boot_m1_cost.lisp:52`
  の `ck` と同じ 4 行を写し、`#ck` 行と合否の一行を自分で組む。
  **これは既に 2 段で使われている形で、「固定文字列を書く」より 1 段良い**
  (件数が実際の判定から出る)。ただし対象外印か、`tools/census_test_counts.py:165`
  の `SELF_TALLY` への登録が必要

---

## §E 印の乱用を止める自己試験を置く場所(#205 の完了条件の 1 つ)

| 事実 | 箇所 |
|---|---|
| **母艦だけで走る検査と、その自己試験を並べる段が既にある** | `Makefile:671` の `test:` の依存。`check-*` 11 本 + `*-self-test` 10 本 |
| その段は自動検査に入っている(実機不要) | `.github/workflows/ci.yml:21-22` |
| `--self-test` を持つ道具の一覧は導出できる | `grep -rl -- '--self-test' tools/`(12 ファイル)+ `tools/aot_identity.sh` / `tools/jit_dump_identity.sh` は `self-test` 引数 |
| 「陽性対照のある検出器は `--self-test` を持つ」という規約化は**未着手**と記録されている | `documents/records-and-measurement-rules.md:864-868` |
| 既に「予算形(数が増えたら落ちる)」の前例がある | `tools/check_eval_error_budget.py`(`Makefile:379-385`。`:379-382` に「自己試験を持っていたのに効かなかった」事故の記録) |
| 判定役の自己試験の作法(4 経路を全部発火させる) | `tools/run_qemu_milestone.sh:46-88` |

**★ 未検証(設計判断)**: 「印の乱用を止める自己試験」は実機を使わずに書ける。
`grep` で (a) 合否の一行を書く箇所、(b) 集計変数を戻す箇所を列挙し、
**対象外印の付いた一覧と突き合わせて差があれば落ちる**形にすれば、
`Makefile:671` の列に並べられる。陽性対照は「印の無い偽造を 1 つ足した一時ファイルで
落ちること」で作れる(`tools/check_jit_bail_lines.py` と同じ作法。`Makefile:409-412`)。

---

## §F 設計で触ると広く影響する箇所

| 直す対象 | 影響範囲 |
|---|---|
| `test/lisp/test_framework.lisp:343-344`(合否の一行の形) | 読む側 3 つ(§A-4)+ 合成資料 3 本(§A-1)+ `tools/census_test_counts.py:227` + `tools/bench/parse_paint_audit.py:73-76` |
| `test/lisp/test_framework.lisp:240`(出口の検査) | 全段のアサーション総数が変わる(`+2/ファイル` の関係。`tools/census_test_counts.py:22-25` が依拠)。`documents/test-count-census.md` の 29 段の記録も変わる |
| `src/c/load.c:166` / `:183` / `:214`(件数を受け取る) | `cc_load` の戻り値の意味を変えると `src/c/kernel.c:306-307`、`test/lisp/qemu_boot_test.lisp:29-30`、`test/c/load_test.c` が効く |
| 組み込み関数を 1 つ足す | 小さいヒープの再調整(既知の罠。`documents/` の記録と `Makefile` の `SMALL_HEAP_FLAGS`) |
| `tools/run_qemu_milestone.sh:115`(判定を厳しくする) | `Makefile:1228` と `Makefile:1177` も同時に直さないと穴が残る。`tools/bench/bench_common.sh:42-47` は #1 に乗っているので自動的に追随 |

---

## §G この文書で直した先行調査の誤り

| 先行調査の記述 | 本文書 |
|---|---|
| `detector-integrity.refs.md:215-221` の「偽造 3 箇所」 | **分類の粒度が違う。**判定役の自己試験(`tools/run_qemu_milestone.sh:76-82`)が落ちており、比較器の 2 ファイルが 1 行にまとめられていた。本文書は検索式の出力をそのまま分類する形に替えた(§A-0 / §A-1) |
| 同 `:278` ほか「自前の集計を持つ段が 1 つ」 | **2 段ある。** `test/lisp/qemu_boot_m1_cost.lisp:81` が落ちていた。`tools/census_test_counts.py:165` の `SELF_TALLY` も 1 段しか登録しておらず、`--inventory` は `qemu_boot_m1_cost.lisp` の期待件数を **0** と出す(実際は `(ck ...)` 6 件) |
| 同 §A-3 の「判定の口は 3 つ」 | **一致**(再導出で確認)。ただし**どれも「出なかった」を成功にはしない**ことを本文書で確かめた(§A-4) |
| 同 §B の「`#count` の消費者だけが存在しない」 | **一致。**併せて**`#at` の消費者も 0 件**であり、`#at` と `#count` の対応が未使用の不変条件であることを足した(§B-2) |
