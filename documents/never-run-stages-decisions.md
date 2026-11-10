# どこからも走らない試験台本 —— 1 本ごとの判断と根拠(#208)

2026-11-09。基点 `419a305`(= `main`)。
調査は `docs/investigation/never-run-stages.logic.md` と `.refs.md`。

この文書は**判断の記録**である。規則 16(R-rebase)の趣旨で、
「何をどうしたか」ではなく**何の役目で、なぜその判断になったか**を残す。

---

## 0. いちばん先に伝えること —— **1 本も消さなかった**

設計(#208 のコメント)は **4 本を消す**判断だった。
実装では**消さずに 4 本とも自動の周回へ入れた。** 理由は 2 つで、どちらも実物で確かめた:

1. **消す根拠が成り立たなかった。** 消す根拠は「合否の項目は常時走る段で
   覆われている」だったが、**4 本とも覆われていない項目を持っていた**(§2)。
   覆われていないものを消すのは、**検出器を減らすこと**である
2. **「嘘を固定している」が事実ではなかった。** 設計は
   `test/lisp/declare_types_bench.lisp:140-142` を「#128 で前提が逆転したので
   走らせたら落ちる」と読んでいた。**走らせたら 19 件すべて通った**(§3)

**消す判断そのものが悪かったのではない。** 判断の入力(調査の「覆っている」)が
読みであって実測でなかった。**調査の読みをそのまま実行に移すと検出器が減る**ことの実例として
残しておく。

### 走らせて分かったこと(4 本の単一段。2026-11-09)

| 台本 | 事前の予測(調査 §11 / 設計。**測る前に書かれていた**。規則 11) | 実測 |
|---|---|---|
| `qemu_boot_declare_bench.lisp` | **落ちる**(長さの一致 3 件が今は偽) | **19 passed, 0 failed**。予測は外れ |
| `qemu_boot_inline_phase0b.lisp` | 通る見込み ★ | **5 passed, 0 failed** |
| `qemu_boot_inline_phase0.lisp` | 通る見込み ★ | **6 passed, 0 failed** |
| `qemu_boot_inline_phase3.lisp` | 通る見込みが高い ★ | **6 passed, 0 failed** |

**4 つの予測のうち 1 つが外れた。** 外れたのは「落ちる」と断定していたほうである。

走らせていない 2 本(★ 未検証): `qemu_boot_instcount_gate_check.lisp` /
`qemu_boot_abi_bench.lisp`。どちらも秒で終わる安い段で、1 周で確かめる。

所要時間は運用の目安として書く(規則 9 のとおり、性能の主張は時間ではなく
確保量と命令数で行う。ここは「周回がどれだけ長くなるか」の見積りである):
declare_bench 82 秒 / inline_phase0b 90 秒 / inline_phase0・inline_phase3 は各 1 分台。

---

## 1. 7 本の名簿 —— 何をどうしたか

| 台本 | 件数 | 設計の判断 | **実際にしたこと** |
|---|---:|---|---|
| `test/lisp/qemu_boot_inline_phase3.lisp` | 6 | 周回に足す | **周回に足した** |
| `test/lisp/qemu_boot_instcount_gate_check.lisp` | 5 | 周回に足す | **周回に足した** |
| `test/lisp/qemu_boot_abi_bench.lisp` | 4 → **6** | 周回に足す | **周回に足した**(証人を寄せて +2) |
| `test/lisp/qemu_boot_inline_phase0.lisp` | 6 | 消す | **消さずに周回に足した**(§2-1) |
| `test/lisp/qemu_boot_inline_phase0b.lisp` | 5 | 消す | **消さずに周回に足した**(§2-2) |
| `test/lisp/qemu_boot_declare_bench.lisp` + `declare_types_bench.lisp` | 19 → **21** | 消す | **消さずに周回に足した**(§2-3 / §3) |
| `test/lisp/qemu_boot_diag_m12_host.lisp` | 1 | 触らない。道具を直す | **触っていない。道具を直した**(§4) |

件数は手で数えていない(規則 13)。出どころ:

```sh
python3 tools/census_test_counts.py --detail --static test/lisp/qemu_boot_abi_bench.lisp ...
```

`abi_bench` と `declare_bench` の **4 → 6 / 19 → 21** は、本体を素の `load` から
`isiki-test-load` へ寄せた分である(#203 の決定。出口の検査が 1 本あたり 2 件)。
**アサーションは 1 件も足していない。**

---

## 2. 「覆われているか」を実物で確かめた —— **覆われていなかったもの**

常時走る段の集合は `Makefile` の `test-qemu-all-stages` から導出した。
母艦側で走る Lisp は `test/c/*.c` の `run_lisp_file` から導出して **6 本だけ**で、
型宣言・JIT 特化・計測の印に触るものは 1 本も無い。

### 2-1. `qemu_boot_inline_phase0.lisp`(6 件)

| 行 | 何を見ているか | 覆っている先 | 判定 |
|---|---|---|---|
| `:64` | primitive を呼ばない関数が機械語に落ちる | `za_test_ext10.lisp:56` / `declare_types_test.lisp:16`(段 `test-qemu`) | 覆われている |
| `:65` | fixnum 宣言つき `+` が機械語に落ちる | `declare_types_test.lisp:20` / `declare_typed_add_test.lisp:24` / `inline_muldiv_test.lisp:33` | 覆われている |
| `:66` | **double 宣言つき `/`** が機械語に落ちる | `declare_types_test.lisp:58` は double 宣言 × `+`。`declare_typed_div_test.lisp:43-45` は generic/fixnum/single のみ | **厳密には覆われていない ★**(`declare_typed_div_test.lisp:49` が「3 引数 `/` は乗らない」を固定しているとおり、**演算子と引数の数で結果が変わる**計器なので外挿できない) |
| `:68` | 大域の inline 宣言集合が空 | `inline_decl_test.lisp:33`(**同じ式**) | 覆われている |
| `:151` `:152` | 印の通過回数(open/close = 26) | **無し**。常時走る段に `%%gate-hits` を使う試験は**1 件も無かった** | **覆われていない** |

### 2-2. `qemu_boot_inline_phase0b.lisp`(5 件)

| 行 | 何を見ているか | 覆っている先 | 判定 |
|---|---|---|---|
| `:49` | **double 宣言つき `+` の呼び先が GENERIC** | `-`/`*`/`/` には対応行がある(`declare_typed_sub_test.lisp:55` / `_mul_test.lisp:54` / `_div_test.lisp:61`)が、**`+` には無い**。`declare_typed_add_test.lisp` には逆アセンブルを見るアサーションが 1 件も無い | **覆われていない** |
| `:50` | 宣言なし `+` の呼び先が GENERIC | `inline_decl_test.lisp:253` | 覆われている |
| `:51` `:52` | **double 宣言つき `+` が fixnum / single 特化版を呼ばないこと** | **無し。** 他の演算子の否定は fixnum ↔ single の取り違えだけで、**double を主語にした否定は全演算子に存在しない** | **覆われていない** |
| `:125` | 印の通過回数(open = 13) | **無し**(上と同じ) | **覆われていない** |

### 2-3. `declare_types_bench.lisp`(19 件)

16 件は覆われている(`%%za-compiled-p` 9 件は多数の段が見ており、
`%%declared-types-of` 4 件は `declare_types_test.lisp:43,50,57,65` が**期待値まで同一**、
値の一致 3 件は `declare_typed_add_test.lisp:30-63` ほかが境界まで含めて厚く覆っている)。

**覆われていない 3 件**: `:140-142`(宣言の有無で**生成コード長が一致**すること)。
常時走る段にあるのは `inline_decl_test.lisp:240-243` と
`inline_muldiv_test.lisp:44-45` で、**どちらも「一致しない」ほうを固定している。**
つまり同等物ではなく**境界の反対側**である(§3)。

### 2-4. まとめ

**4 本とも覆われていない項目を持っていた。** したがって 4 本とも消していない。
いちばん重いのは `phase0b:49/51/52` で、
**「double 宣言は特化版を呼ばない」を見ている試験は木の中にここしか無い。**

---

## 3. `declare_types_bench.lisp` が測っている「現在の事実」

設計は「この計器は『型宣言の有無で生成コードが変わる / 宣言ありのほうが遅い』を
実測していた。計器を消しても測定結果は残す」と指示していた。
**実測し直したら、どちらも今は成り立たない。** 以下が現在の事実である。

### 3-1. 生成コードの長さは、いまも宣言の有無で一致する

```
[DTBENCH] code-len fixnum=2432 single=2436 double=2536 (宣言あり/なしで一致)
==== isiki tests: 19 passed, 0 failed ====
```

理由は `src/c/za.c` の `za_operand_decl_type` である。
**特化版やインライン展開が選ばれるのは、両方のオペランドの型が宣言から分かるときだけ。**
この計器の演算は必ず片側がリテラルか式である
(`(= n 0)` / `(- n 1)` / `(* n 3)` / `(+ acc (* n 3))`)。
だから #128 で型特化と宣言制御の展開が入っても、**ここの生成コードは変わらない。**

**したがって `:140-142` は嘘ではなく、境界を押さえている現役の不変条件である:**
「**片側がリテラル/式なら、宣言があっても生成コードは変わらない**」。
反対側(両オペランドを宣言すると 871 対 916 で差が出る)は
`inline_decl_test.lisp:240-243` が常時走る段で固定している。
**2 つで 1 組であり、片方を消すと組が崩れる。**

### 3-2. 「宣言ありのほうが遅い」は再現しない

```
[DTBENCH] run 差分(宣言あり - 宣言なし):              fixnum=-20 single=32 double=-4
[DTBENCH] run 対照(なし - なし、同一条件のノイズ):    fixnum=-16 single=48 double=-4
```

**3 つの差はすべて、同一条件を 2 度測った対照の差の内側にある。**
差があると書くには対照より大きいことを先に見る必要がある(規則 8)。
この計器が**自分の中に対照群を持っている**ことが、そのまま効いている。

### 3-3. コンパイル時の費用

```
[DTBENCH] compile 差分: fixnum=0 single=4 double=0 unknown=4 (宣言なし比)
```

刻みで 0〜4(1 刻み = 10ms、n=1000)。**確保量は宣言の有無で 1002000 対 997888。**

---

## 4. 誤検出 1 本 —— 道具のほうを直した

`test/lisp/qemu_boot_diag_m12_host.lisp` は **`test/c/script_test.c:332` が読んでいて
`make test` で毎回走っている。** 台本には触っていない。

直したのは `tools/census_test_counts.py` の `where_it_runs` で、
**起点が `Makefile` と継続的検査の 2 種類しか無かった。**
母艦側の単体試験(`test/c/*.c` の `run_lisp_file`)を第 3 の起点として足した。

直す前 / 直したあと:

```
| `qemu_boot_diag_m12_host.lisp` | 1 | 0 | 0 | **Makefile/CI に無い** |
| `qemu_boot_diag_m12_host.lisp` | 1 | 0 | 0 | 母艦 test/c/script_test.c |
```

**「どこからも走らない」という元の報告自体が、道具の穴によるものだった。**
46 件のうち 1 件は最初から走っていた(残り 45 件が本物)。

### 同じ探索の偽陽性と偽陰性も直した

| | 実例 | 直し方 |
|---|---|---|
| **偽陽性**(コメントの中の名前) | `test/c/repl_test.c` のコメントは `test/lisp/toplevel_abort_test.lisp` の名前を含み、`test/c/load_test.c` のコメントは `test/lisp/partition_test.lisp` と `test/c/mount_test.c` を含む。剥がさないと「母艦側が走らせている」と誤って読める | **コメントを剥がしてから**名前を探す(`strip_c_comments`)。`run_lisp_file(...)` の**呼び出しの形だけ**を見る |
| **偽陰性**(同ディレクトリの相対参照) | `test/c/test_assert.h` は `#include "test_assert.h"`(パス接頭辞なし)で読まれているのに「辿れない」と出ていた | 参照元のディレクトリを基準に**相対参照を解決する** |

どちらも `make check-test-file-wiring-self-test` に陰性対照を置いた。

**Makefile のレシピの中にコメント行を書くと同じ族の穴が開く**ことも分かった。
`where_it_runs` はレシピを「タブで始まる行の連続」として読むので、行頭 `#` で
レシピが途中で終わったと判断され、**それ以降の段が「どこからも走らない」に化ける。**
読み手のほうを直した上で、`test-qemu-all-stages` にその注意書きを置いた。

---

## 5. 再発を止める検査

`tools/check_test_count_ratchet.py --wiring`(`make check-test-file-wiring`)。
**起動を要さない静的検査**で、`make test` の前提に並べた。

- 名簿 `WIRING` に **`git ls-files test` の 190 本すべて**を列挙する
- 起点の種類は名簿 `WIRING_KINDS` にある 9 種だけで、
  **知らない種類ではログを見る前に落ちる**(これが #208 の誤検出の真因だった)
- **両方向で落ちる**: 名簿に無い実物 / 実物の無い名簿の行 /
  名簿の「どこから走る」が導出と食い違う /
  **「意図して未配線」と書いたのに実は走っている**
- 「意図して未配線」は**理由と日付が無ければ落ちる**
- 名簿の初期値は手で書いていない(規則 13):
  `python3 tools/check_test_count_ratchet.py --wiring-derive` の出力

いまの木の内訳(`make check-test-file-wiring` が導出して出す):

```
[test-file wiring] 実物 190 本を名簿に列挙 —— **どこからも走らない台本は無い**
  all          28     ci           11     fixture       5
  host          6     hostbuild    21     included      1
  loaded      108     manual       17
```

**「意図して未配線」は 0 本である。** だから陽性対照は合成した木で取る
(`--wiring-self-test`。21 件の対照)。

### 名簿が自分を正当化しないようにした

名簿は実物のパスを 190 行並べているので、「道具が資料として読む」の走査が
**自分自身を読むと「名簿に書いてあるから配線されている」という円環**になる。
走査からこの道具自身を外した。**名簿にしか名前が無いものは「配線されていない」が正しい。**

---

## 6. 件数の名簿への反映(#204)

`COUNTS` と `FILES` に 6 段を足した。**29 段 158 項目 → 35 段 160 項目。**
`make check-test-count-witness` の等式(段の合計 = Σ件数 + 2 × 証人の本数 + 台本自身)が
6 段すべてで閉じている。

足した 6 段のうち 4 段は**項目を持たない**(アサーションが全部台本の最上位にある)。
`check_files_roster` は「項目が空」をそれまで一律に落としていたので、
**台本自身の件数が 0 でないときだけ**許すようにした。
この形では「どのファイルが縮んだか」の答えは台本そのもの 1 つしかないので、
内訳が無くても特定はできている。**件数まで 0 なら何も見ていない**ので、そこは落とす。

---

## 7. 残っている ★(読みで埋めていないもの)

- ★ `qemu_boot_instcount_gate_check.lisp`(5 件)と `qemu_boot_abi_bench.lisp`(6 件)は
  単一段を走らせていない。どちらも秒で終わる段で、1 周で確かめる
- ★ `abi_bench` / `declare_bench` を `isiki-test-load` へ寄せた分の **+2** は、
  #203 で 40 本に同じことをした実績からの導出で、この 2 本では実機未確認
- ★ `phase0:66`(double 宣言つき `/` が機械語に落ちる)は、覆われていないと判定したが
  「性質としては `declare_types_test.lisp:58` が近い」。
  **覆われていないほうに倒した**(外挿できない理由は §2-1)
- ★ 周回が何分伸びるかは測っていない。足した 6 段の所要時間の和は 5〜6 分台と見ている
- ★ `strip_make_comments` が「行まるごとのコメントだけ落とす」で足りるのは、
  いまの木に行の途中のコメントが無いからである(行の途中の `#` を落とすと
  `sed -n 's/^#gate //p'` のような本物のコマンドが切れて、新しい偽陰性になる)
