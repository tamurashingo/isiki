# `init.lisp` の `for` を新展開形にする — 準備と、止まっているところ

> **状態: 移植は行っていない。** 作業指示書 §2-3 に従って止めてある。
> 理由は §1(`ide.lisp` の GC バグが再現しない)。判断を仰いでいる。
>
> 準備として済んでいること: §1 の再現の試み、§2 の展開形の保持と GC ルートの確認、
> §3 の二重定義マクロの照合(**仕組みを常設化した**)、§4 の意味論の固定。

## 0. やろうとしていること

`init.lisp` の `for` を、`transpile.lisp` の `expand-for` と同じ**新展開形**にする。

| | 展開形 | 1 反復あたりの確保 |
|---|---|---:|
| `init.lisp`(インタプリタ / JIT) | 毎反復 `(let ((%for-next-values (list step1 step2))) (setq i (car ...)) ...)` | **32.00 byte(cons 2 個)** |
| `transpile.lisp`(AOT) | 一時変数へ全部評価してから書き戻す | **0** |

得られるもの:

1. `ide.lisp:40-45` の既知の GC バグが閉じる(**仮説。§1 で確かめようとして再現しなかった**)
2. インタプリタと JIT の `for` から毎反復の cons 2 個が消える
3. `for` の基準値が有効に戻る(いま `bench_baseline.tsv` で帯が `-`)
4. `transpile.lisp:721` のコメントが再び正しくなる

---

## 1. `ide.lisp` の GC バグは再現しなかった(2026-10-04)

### 1-1 直す前に再現させようとした理由

「原因が同じ」は**仮説**である。直してから「消えました」と言っても、
**消したのが移植なのか、別の何かが隠したのかが分からない。**

### 1-2 まず、記録が無い

`src/lisp/ide.lisp:40-45` は根拠として `documents/partition.md` PART-M4 調査を挙げている。

> **`documents/partition.md` は、どのブランチにも 1 度も存在しない。**
> `git log --all --diff-filter=A -- "documents/partition*.md"` が空である。

バグの記述は `ide.lisp` / `fat16.lisp` / `fat32.lisp` / `transpile.lisp` の
コメントにしか無く、**再現手順は残っていない。** 規則 1 の事故の形である
(「確認済み」と書くなら何をどう確認したかを同じ行に書く)。

分かっているのはコメントが書いていることだけ:

- `for` マクロ(ループ本体に毎回新規生成される `let`)は **GC が特定のタイミングで
  走ると以後永久に(そのループ内での)結果が壊れる**
- **1 回でも発生すると回復しない**
- **300,000 回の `(setq junk (cons i junk))` 連続実行では再現しない**ことは確認済み
- `fat32.lisp:146` はこう一般化している: **`for` に限らず、`tagbody`/`go` の
  ループ本体内で毎回新規に評価・確保される `let` 束縛は同種のリスクを持つ**

### 1-3 やったこと

| 試したこと | 結果 |
|---|---|
| **GC を確保ごとに強制**(`%%DIAG-GC-STRESS 1`、GC_DEBUG ビルド)。GC が約 9,000 回走る | **全経路で正しい値** |
| 累算が fixnum / cons リスト / 文字列(`set-elt`)/ 3 変数 | **すべて正しい** |
| インタプリタ / JIT / AOT の 3 経路 | **すべて正しい** |
| 通常ビルドで n=200,000 まで伸ばす | 正しい。ただし**ヒープが大きくて GC が 1 回も走らなかった**(`gc=0`) |
| **塗り潰し(`GC_PAINT`)で stale 読みを能動検出**(`%%DIAG-GC-TRAP-HITS`) | **trap 0 件。`while` も `for`(JIT/インタプリタ/AOT)も 0** |

実測の抜粋(`GC_DEBUG=1`、`%%diag-gc-stress` を段階的に強める。期待値 1999000):

```
#stress-off  gc=0    interp=1999000 jit=1999000 aot=1999000 while-jit=1999000
#stress-100  gc=8    interp=1999000 jit=1999000 aot=1999000 while-jit=1999000
#stress-10   gc=89   interp=1999000 jit=1999000 aot=1999000 while-jit=1999000
#stress-1    gc=896  interp=1999000 jit=1999000 aot=1999000 while-jit=1999000
#stress-off  gc=8916 interp=1999000 jit=1999000 aot=1999000 while-jit=1999000
```

塗り潰し下(`GC_DEBUG=1 GC_PAINT=1`、`stress=1000`):

```
#while      r=300   trap=0 gc=0
#for-jit    r=300   trap=0 gc=1
#for-interp r=300   trap=0 gc=20
#for-aot    r=44850 trap=0 gc=20
```

### 1-4 踏んだ罠 — `GC_DEBUG=1` は test 側にも渡さないと効かない

最初の試行は `gc=0` のまま結果が正しく、「GC_STRESS が効いていない」と分かるまで
1 ブート無駄にした。

```
GC_DEBUG=1 make build                        # GC_DEBUG つきのイメージができる
make test-qemu-milestone MILESTONE=...        # ← build に依存しているので
                                              #   **通常ビルドで作り直される**
```

`test-qemu-milestone` は `build` に依存するので、フラグを渡さないと
**その場で通常ビルドへ戻される。** Makefile のビルドスタンプのコメントが
警告している罠そのものである。正しくは:

```
env GC_DEBUG=1 make test-qemu-milestone MILESTONE=...
```

確認は `grep -cE " cc_diag_gc_stress$" tmp/nm_pass2.txt`(1 なら入っている)と
`cat tmp/.gc-debug-flags`。

### 1-5 GC_STRESS の限界(この計器で分かること / 分からないこと)

**`%%DIAG-GC-STRESS` は `os_alloc_bytes` の中でしか GC を起こさない。**
つまり**確保を含まない窓では GC が起きない。**

`%for-next-values` を作ってから `(car ...)` で読むまでの区間には確保が無いので、
**その区間に GC を置くことはこの計器ではできない。**
ただし `(list (+ i 1) (cons i acc))` の評価中(cons 版)と、
`let` の適用で環境を確保するところには確保があるので、
**そこは stress=1 で毎反復突いている。** 塗り潰しの trap も 0 だった。

### 1-6 §2-4 の仮説は成立しない — 展開形は保持されていない

作業指示書は「**展開済みの S 式がどこかに保持されていて、その中のヒープ参照が
GC で移動し、保持側が更新されない**」という形を疑っていた。調べた結果:

| 問い | 答え |
|---|---|
| マクロ展開の結果はどこかに保持されるか(memo / Function Cell / frame) | **されない。** 展開のキャッシュは実装に存在しない(`primitive_macroexpand_1` が呼ばれるたびに作る) |
| 1 回の `for` 評価のあいだは? | `eval_tagbody` が body リストを全反復にわたって保持する |
| それは GC ルートに入っているか | **入っている。** `eval_tagbody` は `args` / `pos` / `env` を `GC_PROTECT` している(`src/c/eval.c:1039-1042`) |

**したがって「保持された展開形が GC ルートでない」という問題は見つからなかった。**
issue として切り出す対象は無い。

### 1-7 考えられること(未確定)

- **既に別の修正で閉じている。** PART-M4 以降に GC 安全性の体系的な監査が入っている
  (`documents/gc-audit-handoff.md`、`known-bug-frame-cell-gc.md`、
  `mount_file_node` / `gc_relocate_stream` の `self_handle` の修正など)。
  **コメントだけが残った**可能性がある
- **再現条件がもっと狭い。** 「GC が特定のタイミングで走ると」の「特定の」が、
  この計器では突けない位置にある
- **GC_DEBUG ビルドが隠している。** 通常ビルドで GC を任意の位置に起こす手段が
  いまは無い(`%%DIAG-GC-STRESS` は GC_DEBUG 専用)

> **作業指示書 §2-3 に従い、ここで止めている。**
> 「再現しなかったので、たぶん直っている」で進めてはいけない。

---

## 2. 二重定義されたマクロは 17 個(§3)

`transpile.lisp:721` のコメントは `for`/`while` について
「`init.lisp` の `defmacro` と**同じ展開規則**」と書いていて、**それが嘘になっていた。**

> **2 つの実装が同じであると主張して、照合するものが何も無かった。**
> 片方を直すと、もう片方のコメントが静かに嘘になる形である。

### 2-1 数えた

| | 個数 | 内訳 |
|---|---:|---|
| `init.lisp` の `defmacro` | **31** | |
| `transpile.lisp` の `*macro-expanders*` | **14** | `let` `let*` `cond` `case` `case-using` `setf` **`for`** `while` `with-open-input-stream` `with-open-input-file` `with-open-output-stream` `with-open-output-file` `defclass` `defmethod` |
| `*macro-expanders*` に無いが transpiler が別に実装している | **3** | `and` `or` `defgeneric`(`transpile-expr` 側で直接扱う) |
| **二重定義の合計** | **17** | |

`init.lisp` にあって transpiler が扱わない 14 個(`assure` `class` `convert`
`dynamic-let` `ignore-errors` `set-dynamic` `the` `with-environment`
`with-error-output` `with-handler` `with-open-io-file` `with-open-io-stream`
`with-standard-input` `with-standard-output`)は二重定義ではない。

### 2-2 照合できる形があった — 常設化した

`make test-qemu-macro-parity`(`tools/check_macro_parity.sh`)。

- **フォームは 1 箇所だけに書く**(`tools/macro_parity_forms.sexp`)。
  host 用と guest 用のドライバはそこから生成する
  (`tools/gen_macro_parity_drivers.py`。`bench_jit.lisp` と同じ方針)
- **host 側**: roswell で `transpile.lisp` を読み、`*macro-expanders*` の展開関数を呼ぶ
- **guest 側**: QEMU で `init.lisp` の `macroexpand-1` を呼ぶ
- gensym の名前(`#:G498` 対 `G7`)は畳んでから比べる。**名前の違いは意味の違いではない**
- 判定は `tools/macro_parity_expected.tsv`:

| 判定 | 意味 |
|---|---|
| `same` | gensym を除いて完全一致すること。**違ったら落ちる** |
| `cosmetic` | 違うが意味は同じ。**同じになったら落ちる**(理由が消えたので行を直す) |
| `DIVERGENT` | **意味が違う**既知の不一致。**同じになったら落ちる** |

### 2-3 照合の結果(2026-10-04、`f19b434`)

| マクロ | 判定 | host と guest | 説明 |
|---|---|---|---|
| `LET` | cosmetic | **違う** | host は body を `(progn ...)` で包む。AOT の「`lambda` の body は単一式」制約のため。意味は同じ |
| `LET*` | same | 一致 | |
| `COND` | same | 一致 | |
| `CASE` | same | 一致 | gensym 名のみ違う |
| `CASE-USING` | same | 一致 | gensym 名のみ違う |
| `SETF` | same | 一致 | |
| **`FOR`** | **DIVERGENT** | **違う** | **host は一時変数へ全部評価してから書き戻す新展開形。guest は毎反復 `(list step...)` を作る旧展開形** |
| `WHILE` | same | 一致 | |
| `WITH-OPEN-INPUT-STREAM` | same | 一致 | |
| `WITH-OPEN-INPUT-FILE` | same | 一致 | |
| `WITH-OPEN-OUTPUT-STREAM` | same | 一致 | |
| `WITH-OPEN-OUTPUT-FILE` | same | 一致 | |

**12 件を機械的に照合して、本物の不一致は `for` だけだった。**

### 2-4 照合できていないもの

| マクロ | なぜ |
|---|---|
| `defclass` / `defmethod` | `*macro-expanders*` にはあるが、展開にクラス登録が要るので代表フォームを作れていない。**未照合** |
| `and` / `or` / `defgeneric` | `*macro-expanders*` を通らず `transpile-expr` が直接扱うので、**呼べる展開関数が無い。** 読んで比べるしかない。**未照合** |

**「できない」ではなく「やっていない」。** 前者は 2 つ、後者は 3 つである。

### 2-5 確保量での固定は代用品である

PR #116 で入れた「`jit-for` は 32 byte/反復、`aot-for` は 0」という回帰は、
**確保量が同じで意味が違う変化は捕まえられない。**
展開形そのものを突き合わせる §2-2 の照合が本体で、確保量はその裏づけである。

---

## 3. 意味論を 3 経路で固定した(§4)

**移植はコピーではない。** `test/lisp/for_semantics_test.lisp`(27 件)に
**移植前の**意味論を固定した。`make test-qemu-all` で常時回る。
**移植したあとに 1 件も落ちないことを確認する。**

### 3-1 並列代入 — 3 経路で一致した(いちばん危ないところ)

旧展開形が毎反復 `(list step1 step2)` を作っていたのは、**まさに並列代入のため**
かもしれない(全部評価してから、まとめて代入する形)。
新展開形が `list` を使わずに並列性を保っているかを、交換で判定した。

```lisp
(for ((i 0 j) (j 1 i) (k 0 (+ k 1))) ((>= k n) (+ (* i 10) j)))
```

並列なら 1 反復ごとに入れ替わる。逐次(`i` を先に書き換えて `j` がその新値を読む)なら
両方 1 になる。

| n | インタプリタ | JIT | AOT | 生成した JIT | 並列の期待 | 逐次なら |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 | 1 | 1 | 1 | 1 = `(0 1)` | 1 |
| 1 | **10** | **10** | **10** | **10** | 10 = `(1 0)` | 11 |
| 2 | 1 | 1 | 1 | 1 | 1 = `(0 1)` | 11 |
| 3 | **10** | **10** | **10** | **10** | 10 = `(1 0)` | 11 |

> **3 経路すべて並列である。AOT は間違っていない。**
> PR #116 の「AOT 側の展開形のほうが新しい」という結論はそのまま成立する。

probe は `src/lisp/bench_aot.lisp` の `%%bench-aot-for-swap` に置いた
(生成される `bench_jit.lisp` 側が JIT 版になるので、**1 箇所の追加で AOT と JIT の
両方が手に入る**)。

既存のベンチでも間接的に確認できる。`%%bench-aot-for` は
`(acc 0 (+ acc i))` で `acc` の step が `i` を読むので、
**並列なら旧 `i` を足して n(n−1)/2、逐次なら新 `i` を足して n(n+1)/2** になる。
n=300 の実測は **44850 = 300×299/2** で、並列である。

### 3-2 step の評価回数 / end-test の評価順 / 戻り値

n=5 のときの実測:

| | 回数 |
|---|---:|
| step 式の評価 | **5**(1 反復 1 回) |
| end-test の評価 | **6**(5 反復 + 最後の判定 = **本体の前**) |
| 本体の評価 | **5** |
| 戻り値 | `(5 5)` |

### 3-3 境界と変数の見え方

| | 実測 |
|---|---|
| step 式の省略 `(fixed 7)` | `(3 7)`(変わらない) |
| 変数 1 個 | `3` |
| 変数 0 個 `(for () (t 'done))` | `DONE` |
| **本体から変数が見えるか** | 見える。`(setq acc (cons i acc))` が効いて `(2 1 0)` |
| **本体の `setq` が外へ残るか** | 残る。`for-sem-seen` = 2 |

**「ループ本体の `let` 廃止」は変数の見え方を変えていない**(AOT 側で既に廃止済みで、
AOT も同じ答えを返している)。

---

## 4. 移植するときに残っている手順

§1 の判断が付いたら、次の順で進める。

1. **予測を先に書く**(規則 11)。確保量は `jit-for` が **32.00 → 0** と断言できる。
   JIT の命令数は幅で宣言する。**AOT は 1 命令も動かないはず**(既に新展開形。
   これが対照群になる)
2. `init.lisp` の `for` を `expand-for` と同じ形へ移植する
   (`%for-let-bindings` に一時変数を足し、`%for-setqs` を 2 段
   「全 step を一時変数へ」「一時変数から束縛変数へ」に分ける)
3. `make test-qemu-macro-parity` の `FOR` を `DIVERGENT` → `same` へ直す
   (**直さないと落ちる。それが狙いである**)
4. `test/lisp/for_semantics_test.lisp` が 27 件とも通ることを確認する。
   §5 の確保量の 2 行は `32` → `0` に直し、**経緯をコメントに残す**
5. `transpile.lisp:721` のコメントを「同じ展開規則」に戻す
   (**そこで初めて本当になる**)
6. `bench_baseline.tsv` の `aot/for` と `jit/for` の帯を `-` から戻して測り直す
7. `ide.lisp` / `fat16.lisp` / `fat32.lisp` のコメントを更新する。
   **`documents/partition.md` が存在しないことも書く**
