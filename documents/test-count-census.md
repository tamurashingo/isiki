# 試験件数の点検 —— 実機 1 周で全段の件数を突き合わせた記録(#202 / T1)

測定日 2026-11-09。基点コミット `07c4a0b`(= `main`)。**コードは 1 行も変えていない。**
足したのは数えるだけの道具 1 本(`tools/census_test_counts.py`)で、
`make` からも CI からも呼ばれない。**判定の仕組みは触っていない**(それは #204)。

背景: `test-qemu-load-gc-stress` が **19 件のうち 3 件しか走っていないのに
「0 failed」**と報告していた(#197 / #199 / #200)。
**同じことがほかの段でも起きていないか**を確かめるのがこの点検である。

---

## 0. 結論

### 0-1. 縮んでいる段は見つからなかった —— ただし測れた範囲でだけ

`make test-qemu-all` の **24 段すべてで、実測の件数が期待件数と一致した**
(食い違い 0)。加えて `#count` 行による**ファイル単位の照合 95 点も全部一致**した。
CI 専用の 5 段も別に回し、**5 段すべて一致**した(§3)。

**「全段 0 failed だったから大丈夫」とは書かない。** 今回の事故はまさに
「0 failed なのに縮んでいた」形だったので、根拠は **件数が合ったこと**の側に置く。
縮んでいないと言えるのは、**件数を突き合わせた 29 段についてだけ**である。
突き合わせられなかった範囲は §5 に列挙する。

### 0-2. いちばんの収穫は「期待件数の数え方が 4 か所で間違っていた」こと

調査(`docs/investigation/detector-integrity.refs.md` §0-G)の静的な期待件数は、
**実機の値と 1 段目から 46 件ずれた**(期待 5225 / 実測 5178)。
これは**縮みではなく数え方の誤り**で、内訳を全部特定した(§2-2)。

**「本来の件数」を手で数えてはいけない**という規則 13(R-count)の実例が、
検出器の点検そのもので出た。この点検では数え方を機械化し、
**95 点のファイル単位の実測 + 29 段の合計**と照合して合わせてある
(調査時の検証点は 5 点だった)。

---

## 1. 測り方(再現できる形で)

```sh
# 本番の 1 周。ログは切らずに全部保存する
make test-qemu-all 2>&1 | tee tmp/t1-qemu-all-<日時>.log

# CI 専用の 5 段(test-qemu-all に入っていない)
for m in m1_base m2_za m3_za_ext5 m4_environment fat16_m0; do
  make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_$m.lisp
done

# 突き合わせ(段ごと)
python3 tools/census_test_counts.py --compare <ログ>
# 突き合わせ(#count 行ごと。いちばん細かい)
python3 tools/census_test_counts.py --per-file <ログ>
# 全台本の棚卸し(期待件数・証人の有無・どこで走るか)
python3 tools/census_test_counts.py --inventory
# 静的な数え方がいちばん外しやすい場所の洗い出し
python3 tools/census_test_counts.py --deferred-audit
```

- 実測ログ: `tmp/t1-qemu-all-20261109-143437.log`(1,464 行)/
  `tmp/t1-qemu-ci-20261109-144633.log`。**どちらも切っていない**
- 1 周の所要: 約 12 分(KVM 無しのサンドボックス。8 コア)
- `make` は 1 段でも落ちればそこで止まるので、**24 段ぶんの合否の一行が
  ログに並んだこと自体が「全段が走った」ことの証拠**である

### 1-1. 「どこで走るか」も導出である

`--inventory` の「どこで走るか」の列は、`Makefile` の `test-qemu-all` の
レシピ(呼び出す中間ターゲットも辿る)と `.github/workflows/ci.yml` から
導出している。`make test-qemu` だけは台本の名前をコマンド行に書かないので、
**ゲスト側の既定値**(`src/c/kernel.c:91` の `g_qemu_test_boot_script`)を読んでいる。

---

## 2. 期待件数の数え方

### 2-1. 定義

```
段の期待(= 合否の一行の passed + failed)
  = 台本が読む試験ファイルのアサーション数の合計
  + 2 × (isiki-test-load で読むファイルの本数)      … 出口の検査自身の 2 件
  + 台本自身に書かれたトップレベルのアサーション数

ファイルの期待(= #count 行の値)
  = そのファイルのアサーション数(**+2 はしない**)
```

**`#count` に +2 が入らないのが要点である。** `isiki-file-end-check` は
件数 `n` を確定して `#count` を書いた**あとで** 2 件の `assert-equal` を走らせる
(`test/lisp/test_framework.lisp:237-240`)。だから 2 件は
**どの `#count` にも入らず、合計にだけ入る。**

### 2-2. 調査の静的な見積りが外れていた 4 か所(+ 1 か所の算数)

1 段目(`qemu_boot_test.lisp`)で期待 5225 対 実測 5178 の差 47 は、全部これで説明がつく:

| # | 何を間違えていたか | 向き | この段での件数 |
|---|---|---|---:|
| 1 | **`assert-output` を 1 件と数えていた。** これは集計を一切触らない入れ物のマクロで、`with-standard-output` + `let` に展開するだけである(`test/lisp/test_framework.lisp:321-327`)。中に書いた `assert-equal` が数えられる | 過大 | **-24** |
| 2 | **集計を元に戻す捕獲の中の 19 件。** `test/lisp/test_framework_test.lisp:25` の `tf-capture` は枠組み自身を試すために `*isiki-test-pass*` / `*isiki-test-fail*` を復元する。走るが件数には出ない | 過大 | **-19** |
| 3 | **呼ばれない身代わりの中の 4 件。** `test/lisp/init_test.lisp:20,29,39,63` の `should-not-be-called-*` は、`(and 1 nil (should-not-be-called-and))` のように**短絡して評価されない位置**で呼ばれる。中の `(assert-equal 0 1)` は「呼ばれたら落ちる」ために置いてある | 過大 | **-4** |
| 4 | **台本自身のトップレベルのアサーションを足していなかった。** `test/lisp/qemu_boot_test.lisp` の `(assert-equal t *isiki-test-syntax-load-result*)` | **過小** | **+1** |
| 5 | §0-G の合計欄の算数。内訳の和は 5224 / 2803 だが、表の合計は 5225 / 2804 と書かれている | 過大 | -1 |

> 調査は「マクロの本体」と「反復の本体」は全件確認していた(`refs.md` §0-E)。
> **見ていなかったのは「集計を復元する捕獲」と「短絡して呼ばれない身代わり」と
> 「集計を触らない入れ物マクロ」の 3 つ**である。いずれも
> 「アサーションの字面はあるが実行時には数に出ない」形で、共通点は
> **字面を数えると過大になる**ことだった。

### 2-3. 直した数え方の検証

| 検証の点 | 数 | 結果 |
|---|---:|---|
| `#count` 行とのファイル単位の照合(`--per-file`) | **95 点** | **全部一致** |
| 段の合計との照合(`--compare`) | **29 段** | **全部一致** |
| 文書に残っていた過去の実測(`refs.md` §0-E) | 5 点 | 全部一致(25 / 21 / 7 / 54 / 21) |

**95 点のファイル単位の一致が、この数え方のいちばん強い根拠である。**
1 ファイルでも数え方を外していれば、そこだけ合わなくなる。

---

## 3. 全段の表 —— 実際の件数 / 期待件数 / 判定

### 3-1. `make test-qemu-all`(24 段。2026-11-09)

| # | 段 | 実測 passed/failed | 期待 | ファイル単位の証人 | 判定 |
|---:|---|---:|---:|---|---|
| 1 | `test-qemu`(`qemu_boot_test.lisp`) | 5178 / 0 | 5178 | 5059 / 59 行 | 一致 |
| 2 | `qemu_boot_no_init_signal.lisp` | 15 / 0 | 15 | **無**(自前集計) | 一致 |
| 3 | `test-qemu-no-9p`(C 側) | 8 / 0 | 8 | **無** | 一致 |
| 4 | `qemu_boot_load_disk.lisp` | 23 / 0 | 23 | 21 / 1 行 | 一致 |
| 5 | `qemu_boot_error_escape_gc.lisp` | 378 / 0 | 378 | 358 / 8 行 | 一致 |
| 6 | `qemu_boot_m5_ide.lisp` | 5 / 0 | 5 | **無** | 一致 |
| 7 | `qemu_boot_m6_fat16.lisp` [fat16] | 155 / 0 | 155 | **無** | 一致 |
| 8 | `qemu_boot_fat_callback_transfer.lisp` [fat16] | 17 / 0 | 17 | 15 / 1 行 | 一致 |
| 9 | `qemu_boot_load_gc_stress.lisp` [fat16] | 21 / 0 | 21 | 19 / 1 行 | 一致 |
| 10 | `qemu_boot_fat32.lisp` [fat32] | 138 / 0 | 138 | **無** | 一致 |
| 11 | `qemu_boot_fat32_primary_boot.lisp` | 11 / 0 | 11 | **無** | 一致 |
| 12 | `qemu_boot_partition.lisp` [gpt_multi] | 38 / 0 | 38 | **無** | 一致 |
| 13 | `qemu_boot_partition.lisp` [mbr_multi] | 38 / 0 | 38 | **無** | 一致 |
| 14 | `qemu_boot_bench_jit.lisp` | 58 / 0 | 58 | 54 / 2 行 | 一致 |
| 15 | `qemu_boot_bench_interp.lisp` | 29 / 0 | 29 | 27 / 1 行 | 一致 |
| 16 | `qemu_boot_for_semantics.lisp` | 29 / 0 | 29 | 27 / 1 行 | 一致 |
| 17 | `qemu_boot_jit_frame.lisp` | 241 / 0 | 241 | 235 / 3 行 | 一致 |
| 18 | `qemu_boot_jit_limits.lisp` | 68 / 0 | 68 | 66 / 1 行 | 一致 |
| 19 | `qemu_boot_stack_guard.lisp` | 26 / 0 | 26 | 24 / 1 行 | 一致 |
| 20 | `qemu_boot_heap_exhaustion.lisp` | 42 / 0 | 42 | 40 / 1 行 | 一致 |
| 21 | `qemu_boot_compare_single.lisp` | 54 / 0 | 54 | 52 / 1 行 | 一致 |
| 22 | `qemu_boot_compare_fixnum.lisp` | 55 / 0 | 55 | 53 / 1 行 | 一致 |
| 23 | `qemu_boot_inline_muldiv.lisp` | 46 / 0 | 46 | 44 / 1 行 | 一致 |
| 24 | `qemu_boot_safety3.lisp` | 25 / 0 | 25 | 23 / 1 行 | 一致 |

> **[2026-11-09 追記 / #204] 3 番(`test-qemu-no-9p`)の合否はこの形では出ない。**
> 上の表と付録は数え上げの道具が整えた形で書いてあるが、**実物のログに
> `==== isiki tests: 8 passed, 0 failed ====` は無い。** 出るのは
> `[no9p] ok <名前>` が 8 行と `[no9p] 8 passed, 0 failed` の 1 行である
> (C 側の `run_boot_smoke`)。**`isiki tests:` だけを探す検査はこの段を黙って飛ばす。**
> 24 段のうち合否の一行が出るのは 23 本だけである。
> 2 番(`qemu_boot_no_init_signal.lisp`)は一行の形は同じだが、
> **出しているのは枠組みではなく台本の自前集計**で、1 件ごとに `#ck` を出す。
> 名簿(§7-3)はこの 3 種類を読み取り方の列として持っている。

**合計 6,698 件。** うち `#count` 行が 1 ファイルずつ裏づけているのは 6,117 件、
**ファイル単位の証人が無い段は 8 段 408 件**(`logic.md` §0 の見積り 404 件と
ほぼ一致。差は `assert-output` の数え方)。

> **[2026-11-09 追記 / #203] 「ファイル単位の証人」の列はもう全部埋まっている。**
> この表は 2026-11-09・基点 `07c4a0b` の**実測の記録**なので書き換えない。
> いまは無証人の段が 1 つも無く、件数も 10 段で上がっている(§5-2)。
> **現在の値は名簿のほうが真実源である**: `python3 tools/check_test_count_ratchet.py --files`

### 3-2. CI 専用の 5 段(`test-qemu-all` に入っていない)

| # | 段 | 実際(passed/failed) | 期待 | 証人(#count の合計 / 行数) | 判定 |
|---:|---|---:|---:|---|---|
| 1 | `test/lisp/qemu_boot_m1_base.lisp` | 2759 / 0 | 2759 | **無証人** | 一致 |
| 2 | `test/lisp/qemu_boot_m2_za.lisp` | 678 / 0 | 678 | 656 / 11 行 | 一致 |
| 3 | `test/lisp/qemu_boot_m3_za_ext5.lisp` | 399 / 0 | 399 | **無証人** | 一致 |
| 4 | `test/lisp/qemu_boot_m4_environment.lisp` | 128 / 0 | 128 | **無証人** | 一致 |
| 5 | `test/lisp/qemu_boot_fat16_m0.lisp` | 8 / 0 | 8 | **無証人** | 一致 |

**合計 3,972 件。** このうち `#count` 行があるのは `qemu_boot_m2_za.lisp` の 656 件だけで、
**残り 3,316 件はファイル単位の証人が無い**(`logic.md` §0 の「CI の無証人 3,728 件」に対応。
差は `assert-output` と捕獲・身代わりの数え方)。
**それでも段の合計は 5 段すべて期待と一致した**(§7-1)。

`qemu_boot_m1_base.lisp` は**無証人の段のうち最大**(2,759 件 = CI の無証人のほぼ全部)で、
ここが縮んでいないことが今回いちばん確かめたかった点である。

> **[2026-11-09 追記 / #203]** この 4 段の無証人も埋めた。`m1_base` は 13 本に
> 証人が付き、件数は 2,759 → 2,785(= 2,759 + 2 × 13)になった。**試験は増えていない**(§5-2)。

---

## 4. 食い違いの説明(1 件ずつ)

実測と期待の食い違いは **0 件**だった。以下は**期待の側の訂正**である。

| 段 | 以前の記録 / 見積り | 今回の実測 | 説明 |
|---|---:|---:|---|
| `qemu_boot_test` | 記録 4,886(2026-09-26、`documents/unbound-arity-survey.md:110`) | **5178** | 記録後に試験が増えた。基準値を保守している場所が無かった |
| `qemu_boot_test` | 静的な見積り 5225(`refs.md` §0-G) | **5178** | **数え方の誤り**(§2-2 の 1〜5) |
| `qemu_boot_jit_frame` | 記録 87(2026-10-30) | **241** | 記録後に `jit_mixed_frame_test.lisp`(136 件)が増えた。**静的な見積り 241 と実測が一致**したので、この説明で確定 |
| `qemu_boot_stack_guard` | 記録 23(2026-10-30)/ 20(2026-10-07) | **26** | 記録後に増えた。静的な見積り 26 と一致 |
| `qemu_boot_m6_fat16` | 静的な見積り 157(`refs.md` §0-B) | **155** | `fat16_test.lisp` の `assert-output` 2 件を数えていた(§2-2 の 1) |
| `qemu_boot_fat32` | 静的な見積り 140 | **138** | 同じ(`assert-output` 2 件) |
| `qemu_boot_m1_base` | 静的な見積り 2804(`refs.md` §0-B) | **2759** | 同じ系統(`assert-output` 22 / 捕獲 19 / 身代わり 4 / 算数 1 = -46。+1 は台本自身の分) |
| `qemu_boot_m2_za` | 静的な見積り 679(`refs.md` §0-B) | **678** | 手で足した +1 のずれ |
| `perf_read_fat16` / `perf_read_fat32` | 静的な見積り 6 / 6 | **未測定(導出では 16)** | `defun` の中の 5 件を **3 回呼ぶ**。素の数え方ではどちらの向きにも外れる(§5-3) |

> **「本当に縮んでいるもの」は 1 件も無かった。**
> 食い違いは全部「記録が古い」か「数え方が間違っていた」で説明がついた。

---

## 5. 突き合わせられなかった範囲(**ここは「分からない」と書く**)

### 5-1. そもそも測っていない段

| 区分 | 本数 | 期待件数の合計 | なぜ測っていないか |
|---|---:|---:|---|
| 手で走らせる段 | **16 本** | 419 | `test-qemu-all` にも CI にも入っていない。性能計測 4 本は 1 本あたり 14〜25 分かかる(`documents/fat16-fat32-write-perf.md` 系の記録)。`for_gc_stress` / `stack_guard_gc` / `alloc_*` / `declare_audit` は別のビルドフラグが要る |
| `Makefile` / CI のどこにも無い段 | **7 本** | 46 | `qemu_boot_abi_bench` / `declare_bench` / `diag_m12_host` / `inline_phase0` / `inline_phase0b` / `inline_phase3` / `instcount_gate_check`。**文書に手順だけが残っている**(§7-2) |

### 5-2. 測ったが「どのファイルが縮んだか」は言えない段 —— **埋めた(#203 / T2)**

> **[2026-11-09 追記 / #203] この節は「言えない」と書いていた。それはもう事実ではない。**
> **29 段すべてで、どのファイル(どの項目)が縮んだかが言える。**
> 以下、元の記述を残したうえで何が変わったかを書く。

#### 以前(この点検の時点)

**ファイル単位の証人(`#count` 行)が無い 8 段 408 件**(§3-1 の #2,3,6,7,10,11,12,13)。
§3-2 の CI 専用の 4 段(`m1_base` / `m3_za_ext5` / `m4_environment` / `fat16_m0`、
3,316 件)を足して **合計 12 段**が無証人だった
(issue #203 のコメントは「13 段」と書いているが、**この表から導出すると 12 段**である。
8 + 4 = 12。`m2_za` は 11 行の証人つき、`qemu_boot_test` は 59 行の証人つき)。

段の合計は観測できたので**段としては突き合わせられた**が、
**合計が合っていても内訳が入れ替わっていたら気づけなかった**
(例: あるファイルが 2 件減って別のファイルが 2 件増えた場合)。

> `logic.md` §0 の「無証人率 84%」は**ファイル単位の証人**の話である。
> 段の合計はどの段でも観測できるので、**この点検のやり方(合計 vs 静的な期待)は
> 無証人の段にも効く。** 効かないのは内訳の特定だけである。
> これは #204 の設計で使える材料になった。

#### いま

**証人の経路を `isiki-test-load` 1 本に寄せた。** 無証人だった 10 段で、
試験ファイル **40 本**(重複を除くと 38 本)の読み込みを素の `load` から
`isiki-test-load` へ変えた。残りの 2 段は枠組みを使えないので別の形で済んでいる:

| 以前の無証人 | いま | 証人の形 |
|---|---|---|
| `m5_ide` / `m6_fat16` / `fat32` / `fat32_primary_boot` / `partition`(gpt/mbr)/ `m1_base` / `m3_za_ext5` / `m4_environment` / `fat16_m0`(**10 段**) | `isiki-test-load` に寄せた | `#count <ファイル> <件数>` |
| `qemu_boot_no_init_signal`(自前集計) | **元から 1 件 1 行で名前つき**だった。名簿を名前の一覧にした | `#ck <名前>`(15 項目) |
| `test-qemu-no-9p-run`(C 側) | 同じ。`[no9p] ok <名前>` が 8 行 | `[no9p] ok <名前>`(8 項目) |

判定は**新しい道具を作らず** `tools/check_test_count_ratchet.py` に足した
(検出は #204 の仕事で、こちらは**内訳の特定**である)。名簿は `FILES` で
**29 段 158 項目**。段の合計との関係は等式で縛ってある:

```
段の合計 = Σ(ファイルの件数) + 2 × (証人の本数) + 台本自身の件数
```

この等式が**証人の無いアサーションを見つける者**でもある。素の `load` で読んだ
アサーションは `#count` に出ず合計にだけ入るので、残差が正になる。
加えて `#at`(進捗)と `#count`(件数)が 1 対 1 になるという
`logic.md` §B-2 の**使われていなかった不変条件**を使っている:
**`#at` だけがあって `#count` が無いファイルは「読み始めて出口へ着かなかった」**
(= 丸ごと落ちた。調査 §9-4-7)。これを見る者がいなかった。

#### 基準値を動かしたこと(規則 16)

`isiki-test-load` は出口の検査 2 件を足すので、**10 段の件数が上がった。**
**アサーションは 1 件も足していない**(`新 = 旧 + 2 × 新たな証人の本数`)。
内訳は `tools/check_test_count_ratchet.py` の `COUNTS` の上の表にある。
合計 +80 件 = 2 × 40 本で、名簿全体は **10,670 → 10,750 件**。

#### 陽性対照(規則 8)

| 対照 | どこで確かめたか |
|---|---|
| ある試験ファイルのアサーションを 1 件消すと**そのファイル名が出て落ちる** | **実機**。`fat16_m0` を 1 段回した。`bitwise_char_test.lisp` の `(assert-equal #\A (code-char 65))` を消す → `#count ... 7`、照合器が「ファイル `test/lisp/bitwise_char_test.lisp` の件数が 1 件減っている(名簿 8 / 実測 7)」 |
| 証人の無いファイルが増えると落ちる(静的) | `make check-test-count-witness`。台本に `(load "test/lisp/newly_added_test.lisp")` を足す → 「**素の load で**読んでいる」で落ちる。**QEMU は要らない** |
| 証人の無いアサーションが合計にだけ入ると落ちる(ログ) | **実物のログ**。寄せる前の `tmp/t1-qemu-ci-20261109-144633.log` を新しい名簿に食わせると「合計 128 件に対し、証人で説明がつくのは 0 件(差 +128)」 |
| ファイルが丸ごと落ちると**そのファイル名が出て落ちる** | 実機のログから `#count` の行だけを落とした資料。「**進捗の行(#at)は出ている**ので、そのファイルを読み始めてから出口へ着く前に止まっている」 |
| 通常の 1 周で誤って鳴らない(陰性) | 実機 1 段(`fat16_m0`、OK)。加えて**実物の T1 のログ**(`tmp/t1-qemu-all-*.log`)で、寄せていない 17 段のファイル単位の照合が**1 件も鳴らない**ことを確認した(鳴るのは寄せた 7 段ぶんだけ。ログが古いので当然である) |

#### ★ まだ実機で `#count` を見ていない内訳

寄せた 40 本のうち **31 本**は、同じファイルが `test-qemu` の段で
`#count` つきで走っており、T1 の実測と**同じ値**である(裏づけあり)。
残りは次の 7 本(9 か所)で、**段の合計は T1 で実測されている**が
`#count` そのものは ★ 未確認である:

| 段 | ファイル | 導出 | 合計との関係 |
|---|---|---:|---|
| `m5_ide` | `ide_test.lisp` | 5 | 段の合計 5(実測)から**一意に決まる**(ほかは `src/lisp/partition.lisp` で 0 件) |
| `fat32` | `fat32_test.lisp` | 138 | 段の合計 138(実測)から**一意に決まる** |
| `fat32_primary_boot` | `fat32_primary_boot_test.lisp` | 11 | 段の合計 11(実測)から**一意に決まる** |
| `m6_fat16` | `fat16_test.lisp` / `device_test.lisp` | 145 / 10 | 和 155 は実測。**内訳の分かれ目が ★** |
| `partition`(gpt / mbr) | `partition_test.lisp` / `file_cmd_test.lisp` | 19 / 19 | 和 38 は実測。**内訳の分かれ目が ★** |
| `fat16_m0` | `bitwise_char_test.lisp` | 8 | **実機で確認した**(今回 1 段回した) |

つまり ★ が本当に残っているのは **2 段の「内訳の分かれ目」だけ**(`m6_fat16` と
`partition`)で、どちらもディスクイメージが要る段である。
**1 周回せば確定する**(鳴ったら名簿の分かれ目を直せばよい。合計は動かない)。

### 5-3. 期待件数が素の数え方では出ない段

`defun` の中にアサーションがあり、**その `defun` が複数回呼ばれる**段。
`--deferred-audit` で全試験ファイルを洗ったところ、該当は **2 本だけ**だった:

| 試験ファイル | 本体の件数 | 呼び出し箇所 | 素の数え方 | 実際に走る数 |
|---|---:|---:|---:|---:|
| `test/lisp/perf_read_fat16_test.lisp` | 5(`%%perf-read-check`) | 3 | 1 または 6 | **5×3 + 1 = 16** |
| `test/lisp/perf_read_fat32_test.lisp` | 5(同じ) | 3 | 1 または 6 | **16** |

`refs.md` §0-B はこれを **6 / 6** と見ていた(本体を 1 回だけ数えた)。
この道具の素の数え方だと **1 / 1** になる(本体を数えない)。
**どちらも外れる。** 呼び出し回数を掛けないと出ない。
この 2 段は手で走らせる段(性能計測)なので今回は測っていない。

### 5-4. 動的に増えるアサーション —— **無いことを機械的に確かめた**

反復(`for` / `while` / `dotimes` / `dolist` / `mapcar` / `mapc`)の**本体**に
アサーションがあるファイルを全試験ファイル(50 本超)で探した結果、**0 件**だった。
`(assert-equal X (for ...))` の形は引数側なので 1 回のままである。
`defmacro` の本体にあるのは `test/lisp/defmacro_test.lisp` の
`should-not-be-called`(1 件)だけで、これは呼ばれない身代わりかつ
**このファイルはどの台本からも読まれていない。**

したがって **「件数が実行時に増える」経路は、いま 1 本も使われていない。**
`perf_read_fat*` の `defun` × 呼び出し回数(§5-3)だけが例外である。

### 5-5. そのほか確かめていないこと

- **2 回目の実行**。この点検は 1 周だけである。`test-qemu-all` は先頭で
  ディスクイメージを消すので、周回ごとの汚れは入らない。だが
  **単体で 2 回走らせたときの挙動は確かめていない**(`refs.md` §E の
  「作り直さない段」4 つ。★ 未検証のまま)
- **生成物が最新かどうか**。`test/lisp/isiki_test_jit.lisp`(1,211 件)は
  `tools/gen_isiki_test_jit.py` の生成物で、生成器は走らせていない。
  現物を数えた値が実機と一致したので**現物とゲストはそろっている**が、
  **生成器と現物がそろっているかは見ていない**
- **flake の扱い**(規則 4)。今回の 1 周で `[FLAKE]` は 1 行も出ていない。
  再試行は 1 度も起きていない

---

## 6. 期待件数の棚卸し(全台本)

`python3 tools/census_test_counts.py --inventory` の出力をそのまま置く。
**この表は手で保守しない**(導出である)。
「どこで走るか」の `all` = `make test-qemu-all`、`CI` = `.github/workflows/ci.yml`、
`手` = `Makefile` にターゲットはあるが上の 2 つに入っていない、
`Makefile/CI に無い` = 文書に手順だけがある。

| 台本 | 期待件数 | 証人つき本数 | 証人なし本数 | どこで走るか |
|---|---:|---:|---:|---|
| `qemu_boot_abi_bench.lisp` | 4 | 0 | 1 | **Makefile/CI に無い** |
| `qemu_boot_alloc_audit.lisp` | 21 | 1 | 0 | 手 |
| `qemu_boot_alloc_breakdown.lisp` | 7 | 1 | 0 | 手 |
| `qemu_boot_bench_construct.lisp` | 20 | 0 | 1 | 手 |
| `qemu_boot_bench_interp.lisp` | 29 | 1 | 4 | all |
| `qemu_boot_bench_jit.lisp` | 58 | 2 | 3 | all |
| `qemu_boot_compare_fixnum.lisp` | 55 | 1 | 0 | all |
| `qemu_boot_compare_single.lisp` | 54 | 1 | 0 | all |
| `qemu_boot_declare_audit.lisp` | 12 | 1 | 0 | 手 |
| `qemu_boot_declare_bench.lisp` | 19 | 0 | 1 | **Makefile/CI に無い** |
| `qemu_boot_diag_m12_host.lisp` | 1 | 0 | 0 | **Makefile/CI に無い** |
| `qemu_boot_error_escape_gc.lisp` | 378 | 8 | 0 | all |
| `qemu_boot_fat16_m0.lisp` | 8 | 0 | 1 | CI |
| `qemu_boot_fat32.lisp` | 138 | 0 | 1 | all / CI |
| `qemu_boot_fat32_primary_boot.lisp` | 11 | 0 | 1 | all / CI |
| `qemu_boot_fat_callback_transfer.lisp` | 17 | 1 | 0 | all / CI |
| `qemu_boot_float_contagion_bench.lisp` | 15 | 0 | 1 | 手 |
| `qemu_boot_for_expansion.lisp` | 8 | 0 | 1 | 手 |
| `qemu_boot_for_gc_stress.lisp` | 15 | 1 | 2 | 手 |
| `qemu_boot_for_semantics.lisp` | 29 | 1 | 2 | all |
| `qemu_boot_heap_exhaustion.lisp` | 42 | 1 | 0 | all |
| `qemu_boot_imm_leak.lisp` | 15 | 0 | 1 | 手 |
| `qemu_boot_inline_muldiv.lisp` | 46 | 1 | 0 | all |
| `qemu_boot_inline_phase0.lisp` | 6 | 0 | 0 | **Makefile/CI に無い** |
| `qemu_boot_inline_phase0b.lisp` | 5 | 0 | 0 | **Makefile/CI に無い** |
| `qemu_boot_inline_phase3.lisp` | 6 | 0 | 0 | **Makefile/CI に無い** |
| `qemu_boot_instcount_gate_check.lisp` | 5 | 0 | 0 | **Makefile/CI に無い** |
| `qemu_boot_jit_frame.lisp` | 241 | 3 | 0 | all |
| `qemu_boot_jit_limits.lisp` | 68 | 1 | 0 | all |
| `qemu_boot_load_disk.lisp` | 23 | 1 | 0 | all |
| `qemu_boot_load_gc_stress.lisp` | 21 | 1 | 1 | all |
| `qemu_boot_m1_base.lisp` | 2759 | 0 | 13 | CI |
| `qemu_boot_m1_cost.lisp` | 0 | 0 | 0 | 手 |
| `qemu_boot_m2_za.lisp` | 678 | 11 | 0 | CI |
| `qemu_boot_m2_za_stress.lisp` | 254 | 0 | 2 | 手 |
| `qemu_boot_m3_za_ext5.lisp` | 399 | 0 | 14 | CI |
| `qemu_boot_m4_environment.lisp` | 128 | 0 | 3 | CI |
| `qemu_boot_m5_ide.lisp` | 5 | 0 | 2 | all / CI |
| `qemu_boot_m6_fat16.lisp` | 155 | 0 | 2 | all / CI |
| `qemu_boot_no_init_signal.lisp` | 15 (自前集計) | 0 | 0 | all |
| `qemu_boot_partition.lisp` | 38 | 0 | 2 | all / CI |
| `qemu_boot_perf_fat16.lisp` | 13 | 0 | 1 | 手 |
| `qemu_boot_perf_fat32.lisp` | 13 | 0 | 1 | 手 |
| `qemu_boot_perf_read_fat16.lisp` | 1 | 0 | 1 | 手 |
| `qemu_boot_perf_read_fat32.lisp` | 1 | 0 | 1 | 手 |
| `qemu_boot_read_file_native.lisp` | 15 | 0 | 1 | 手 |
| `qemu_boot_safety3.lisp` | 25 | 1 | 0 | all |
| `qemu_boot_stack_guard.lisp` | 26 | 1 | 0 | all |
| `qemu_boot_stack_guard_gc.lisp` | 9 | 1 | 1 | 手 |
| `qemu_boot_test.lisp` | 5178 | 59 | 0 | all |

### 6-1. 入れ物の中のアサーションの洗い出し(`--deferred-audit`)

| 試験ファイル | 数えた件数 | 入れ物の中 | assert-output | 件数を持つ defun(本体の件数, 出現回数) |
|---|---:|---:|---:|---|
| `defmacro_test.lisp` | 12 | 1 | 0 | `should-not-be-called`(1, 3) |
| `disassemble_test.lisp` | 69 | 0 | 1 | — |
| `fat16_test.lisp` | 145 | 0 | 2 | — |
| `fat32_test.lisp` | 138 | 0 | 2 | — |
| `init_test.lisp` | 496 | 4 | 0 | `should-not-be-called-and`(1, 1), `should-not-be-called-or`(1, 1), `should-not-be-called-cond`(1, 1), `should-not-be-called-while`(1, 1) |
| `isiki_test.lisp` | 653 | 0 | 11 | — |
| `isiki_test_jit.lisp` | 1211 | 0 | 11 | — |
| `perf_read_fat16_test.lisp` | 1 | 5 | 0 | `%%perf-read-check`(5, 3) |
| `perf_read_fat32_test.lisp` | 1 | 5 | 0 | `%%perf-read-check`(5, 3) |
| `room_test.lisp` | 13 | 0 | 1 | — |
| `test_framework.lisp` | 0 | 2 | 0 | `isiki-file-end-check`(2, 1) |
| `test_framework_test.lisp` | 29 | 19 | 0 | — |

字面はあるが実行時には数に出ない(か、掛け算になる)のはこの 12 本だけである。
読み方は §2-2 と §5-3。

> **棚卸し表の `perf_read_fat16` / `perf_read_fat32` の「期待 1」は正しくない。**
> この道具は `defun` の中のアサーションを数えないので、呼び出し回数を掛ける
> §5-3 の 2 本だけは表の値が過小になる(正しくは 16)。
> **道具が外す場所を道具自身の出力で特定できる形にしてある**のが `--deferred-audit` で、
> この 2 本はそこに必ず出る。

---

## 7. 気づいたこと

### 7-1. 「合計の照合」は無証人の段にも効く

`logic.md` は「無証人の段は突き合わせられない」としていたが、
**それはファイル単位の話**である。段の合計(合否の一行)はどの段でも出るので、
**静的な期待と合計を比べるやり方は 29 段すべてに効いた。**
#204 で母艦側に件数の照合を置くなら、**まず段の合計を見るだけで
無証人の段も含めて全段を覆える**(内訳の特定は別の話)。

### 7-2. `Makefile` / CI のどこからも走らない段が 7 本ある

`qemu_boot_abi_bench` / `declare_bench` / `diag_m12_host` / `inline_phase0` /
`inline_phase0b` / `inline_phase3` / `instcount_gate_check`(合計 46 件)。
`diag_m12_host` を除く 6 本は `documents/` の中に手順として名前が残っている。
**落ちない検査は検出器ではない**(規則 8)という意味で、
これは「静かに減る」の族の別の形である。**直すのはこの仕事の外**だが記録しておく。

### 7-3. 「本来の件数」を保守する場所ができた(2026-11-09 追記。#204 / T3)

**この節は「保守している場所は 1 つも無い」と書いていた。それはもう事実ではない。**

`tools/check_test_count_ratchet.py` の `COUNTS` が**段ごとの件数の名簿**であり、
**`make test-qemu-all` の最後と CI の最後が、1 周のログと突き合わせる。**
初期値は §3-1 / §3-2 の表(= この記録)そのものである。

| | 以前 | いま |
|---|---|---|
| 件数の基準値 | **どこにも無い**(この表が最初の 1 つ。日付の付いた記録) | `COUNTS` 1 箇所。**両方向で落ちる**(減っても増えても) |
| 突き合わせ | 手で 1 回 | `make test-qemu-all` と CI の最後に自動 |
| 段が走らなかった場合 | **何も出ない**(#208) | **名簿にある段がログに無ければ落ちる** |
| no-9p の 8 件 | `Makefile` の grep が直書き(**2 箇所目の基準値**だった) | 名簿へ移した。`Makefile` は `--stage` で名簿に問い合わせる |
| 読み取り方 | `isiki tests:` を探すだけ(**C 側の段を飛ばす**) | 名簿の列。`isiki` / `self-tally` / `no9p` の 3 種類。**知らない読み方は落ちる** |
| 内訳(どのファイルが縮んだか) | **言えない段が 12 段あった**(§5-2) | `FILES` 1 箇所。**29 段 158 項目。全段で言える**(#203 / T2) |
| 証人があるかどうか | **人が数えていた**(§3 の列) | `make check-test-count-witness`。台本と名簿の突き合わせ。**QEMU が要らない** |

**この表自身はいまも日付の付いた記録である。** 腐るのを止めているのは
表ではなく名簿のほうで、**名簿を動かすときは理由を同じ場所に書く**(規則 16)。

なお `tools/census_test_counts.py`(静的な数え上げ)は**検査には組み込んでいない。**
あれは「なぜ数が変わったか」を調べる道具として残す。理由は §5-3 にある:
静的な数え上げは推定であり、`perf_read_fat16/32` のように `defun` の中の 5 件を
3 回呼ぶ形は**今も静的には出ない**。推定を権威にすると、推定が壊れたときに
**静かに通る**。権威は固定した名簿のほうに置いた。

> **§6 の棚卸しが `qemu_boot_m1_cost.lisp` を期待 0 件と扱っていた**のも同じ話である
> (自前集計の 6 件を数え上げの道具が知らない)。**期待 0 件の段は、縮んでも差が出ない。**
> だから名簿の側では **0 件を正当な値として認めない**(`check_roster` が落とす)。

### 7-4. 1 周は 12 分だった

`logic.md` §8-7 は「1 周に 1 時間以上かかる」(記録あり)としていたが、
この機械(8 コア・KVM 無し)では **約 12 分**だった。
CI 専用の 5 段を足しても 20 分以内に収まる。
**「1 周は高い」という前提は、少なくともこの機械では見直せる。**

---

## 付録: 生の合否の一行(ログからそのまま)

ログ本体は `tmp/`(git 管理外)にあるので、**判定の根拠になる行だけここに写す。**
順序はログの出現順。`make` は 1 段でも落ちればそこで止まるので、
**この並びが途切れていないこと自体が「全段が走った」ことの証拠**である。

### make test-qemu-all(`tmp/t1-qemu-all-20261109-143437.log`)

```
 1  test-qemu                                                  ==== 5178 passed, 0 failed ====
 2  test/lisp/qemu_boot_no_init_signal.lisp                    ==== 15 passed, 0 failed ====
 3  test-qemu-no-9p-run                                        ==== 8 passed, 0 failed ====
 4  test/lisp/qemu_boot_load_disk.lisp                         ==== 23 passed, 0 failed ====
 5  test/lisp/qemu_boot_error_escape_gc.lisp                   ==== 378 passed, 0 failed ====
 6  test/lisp/qemu_boot_m5_ide.lisp                            ==== 5 passed, 0 failed ====
 7  test/lisp/qemu_boot_m6_fat16.lisp [fat16_test.img]         ==== 155 passed, 0 failed ====
 8  test/lisp/qemu_boot_fat_callback_transfer.lisp [fat16_test.img] ==== 17 passed, 0 failed ====
 9  test/lisp/qemu_boot_load_gc_stress.lisp [fat16_test.img]   ==== 21 passed, 0 failed ====
10  test/lisp/qemu_boot_fat32.lisp [fat32_test.img]            ==== 138 passed, 0 failed ====
11  test/lisp/qemu_boot_fat32_primary_boot.lisp                ==== 11 passed, 0 failed ====
12  test/lisp/qemu_boot_partition.lisp [gpt_multi_test.img]    ==== 38 passed, 0 failed ====
13  test/lisp/qemu_boot_partition.lisp [mbr_multi_test.img]    ==== 38 passed, 0 failed ====
14  test/lisp/qemu_boot_bench_jit.lisp                         ==== 58 passed, 0 failed ====
15  test/lisp/qemu_boot_bench_interp.lisp                      ==== 29 passed, 0 failed ====
16  test/lisp/qemu_boot_for_semantics.lisp                     ==== 29 passed, 0 failed ====
17  test/lisp/qemu_boot_jit_frame.lisp                         ==== 241 passed, 0 failed ====
18  test/lisp/qemu_boot_jit_limits.lisp                        ==== 68 passed, 0 failed ====
19  test/lisp/qemu_boot_stack_guard.lisp                       ==== 26 passed, 0 failed ====
20  test/lisp/qemu_boot_heap_exhaustion.lisp                   ==== 42 passed, 0 failed ====
21  test/lisp/qemu_boot_compare_single.lisp                    ==== 54 passed, 0 failed ====
22  test/lisp/qemu_boot_compare_fixnum.lisp                    ==== 55 passed, 0 failed ====
23  test/lisp/qemu_boot_inline_muldiv.lisp                     ==== 46 passed, 0 failed ====
24  test/lisp/qemu_boot_safety3.lisp                           ==== 25 passed, 0 failed ====
```

### CI 専用 5 段(`tmp/t1-qemu-ci-20261109-144633.log`)

```
 1  test/lisp/qemu_boot_m1_base.lisp                           ==== 2759 passed, 0 failed ====
 2  test/lisp/qemu_boot_m2_za.lisp                             ==== 678 passed, 0 failed ====
 3  test/lisp/qemu_boot_m3_za_ext5.lisp                        ==== 399 passed, 0 failed ====
 4  test/lisp/qemu_boot_m4_environment.lisp                    ==== 128 passed, 0 failed ====
 5  test/lisp/qemu_boot_fat16_m0.lisp                          ==== 8 passed, 0 failed ====
```
