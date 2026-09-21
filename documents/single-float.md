# single-float の導入と型階層の拡張

> 実施日: 2026-09-19 / ブランチ: `feature/single-float-and-types`(分岐元 `feature/compiler-optimization` `5e9c762`)
> 関連: `documents/type-system-survey.md`(Phase 4 準備調査)、`documents/tag4-design.md`、
> `documents/tag4-step2.md`(タグ4bit化)、`documents/gc-audit-handoff.md`
>
> 本作業の範囲は (a) 内部表現 / (b) リーダ / (d) print / (e) 型階層 の4つ。
> **(c) 演算の型昇格は含まない。** したがって完了時点でも
> **single 同士の演算結果は double になる**(既知の中間状態)。

---

## 0. 先に結論

1. **タグ 0x4 は本当に空いていた。** 値を作る経路が無く、`os_tag_is_heap_ref` は
   明示的に偽を返す枝に置かれていた(PR #78 で「除外リストではなく列挙」に
   したのが効いている)。GC 側に触る必要は**無かった**。
2. **`is_float` 1 箇所を直すと、算術・比較・`floatp` が全部 single を受ける。**
   数としての扱いは合流点が 1 つしかないので、(c) 抜きでも single-float は
   普通の数として使える。
3. **`f`/`d` は接尾辞ではなく指数マーカーにした。** `3.14f` がシンボルのままで
   いられるのはこのためで、`f1` のような既存のシンボルも壊れない。
4. **print は 17 桁固定では駄目だった。** binary32 の刻みが粗いので、
   `1.5f20` が `1.50000002E20` になる。**同じ値に読み戻せる最短の桁数**を
   1〜9 で探す形に変えた(double 側の出力は一切変えていない)。
5. **`*read-default-float-format*` の初期値だけ指示書から変えた**
   (`<single-float>` → `<double-float>`)。演算の型昇格 (c) と
   整数→float 変換の型が決まるまで `<single-float>` は成立しないことを
   実測で確認した(§2-3a)。**切り替えは `init.lisp` の1行**である。
6. **既存の潜在バグを2つ踏んだ**(どちらも §2-7)。
   `primitive_abs` が float を整数用の `decompose` へ落としていたこと、
   `primitive_class_supers` が nil をデリファレンスしていたこと。
   前者は「single-float が即値だから落ちる」、後者は
   「グローバルの配置が変わったから落ちる」という形で表に出た。

---

## 1. 調査 (指示書 4 章)

### 1-1 タグまわり (4-1)

`src/c/runtime.h` のタグ表(4bit、PR #78):

```
 値   種別    型
 0x0  即値    fixnum
 0x2  即値    character
 0x4  即値    single-float   ← 本作業で実装
 0x6/0x8/0xA/0xC 即値 未割当
 0xE  即値    予約(MAGIC_* の下位4bit)
 0x1  アドレス cons      0x3 symbol   0x5 string   0x7 instance
 0x9  アドレス raw ptr   0xB 未割当   0xD 未割当   0xF forward
```

- **0x4 が未使用であることの確認**: `grep -rn "TAG_SINGLE_FLOAT" src/ test/` の
  結果は「定義本体・`_Static_assert`・`os_tag_is_heap_ref` の case・テスト3箇所」
  だけで、**値を作る経路は1つも無かった**。
- **GC が 0x4 を追いかけないことの確認**: `os_tag_is_heap_ref` は全16値を並べた
  `switch` で、0x4 は `return 0` 側に明示的に置かれている。既存テスト
  `test_unassigned_and_reserved_tags_are_not_followed_by_gc`(runtime_test.c)が
  これを固定している。**GC 側の変更は不要。**
- **TAG_CHAR と同じ形にした**:

  | | TAG_CHAR | TAG_SINGLE_FLOAT |
  |---|---|---|
  | 値の場所 | bit32-63 | bit32-63 |
  | シフト量の定数 | `CHAR_VALUE_SHIFT` | `SINGLE_FLOAT_VALUE_SHIFT` |
  | bit4-31 | 未使用(0) | 未使用(0) |

  シフト量は**別の定数**にしてある。値はどちらも 32 だが、CHAR のコードポイント幅と
  float の仮数幅は独立に動きうる(`FIXNUM_VALUE_SHIFT` と `TAG_MASK` を分けたのと同じ理由)。

- **ビットパターンの移動は union**。`*(UINT32 *)&f` は strict aliasing 違反で、
  最適化で黙って別の値になりうる。

### 1-2 リーダの現状 (4-2)

`src/c/reader.c`:

- `read_atom` がトークンを**整数 / float / シンボル**のどれとして読むかを決める。
  判定は2段構え:
  1. `is_float_token_char` が全文字を通し、かつ数字を1つ以上含むか(粗いフィルタ)
  2. 通れば `parse_float_token` が ISLisp §19.2 の構文に照らす。外れると `read-error`
- `parse_float_token` は `strtod` を使わず、**桁を仮数へ逐次累積し、最後に
  10 のべきを掛け/割る**手書きの変換をしている。
- `3f` が今までシンボルだったのは、単に `'f'` が `is_float_token_char` を
  通らなかったから。`'e'` は通るので、**`3e` は今でも read-error** である。

#### `f`/`d` を足すときに現れた問題

`'f'`/`'d'` を `is_float_token_char` に足すと、`e` とまったく同じ扱いにした場合

- `3.14f` → read-error(指示書の表は**シンボル**と定めている)
- `f1` / `d2` → read-error(**これまで普通に読めていたシンボルが読めなくなる**)

になる。そこで次の規則にした。

> `e`/`E` は従来どおり(構文から外れたら read-error)。
> **新設の `f`/`F`/`d`/`D` を含むトークンだけ、構文から外れたらシンボルへ戻す。**

`e`/`E` 側を触らないのは、single-float とは無関係な既存挙動を動かさないため。
実装は `has_extended_float_marker`(reader.c)。

対象トークンの棚卸し: `src/lisp` と `test/lisp` 全体を
`[+-]?[0-9]+(\.[0-9]+)?[fFdD][+-]?[0-9]+` で grep した結果は**0件**。
つまり「今までシンボルだったものが float になる」トークンは既存コードに無い。

### 1-3 `class-of` の現状 (4-3)

`src/lisp/init_aot.lisp:456`(AOT トランスパイル対象)。15 分岐の `cond`:

```
 1 %%class-instance-p     9 general-vector-p
 2 %%standard-classp     10 general-array*-p
 3 %%builtin-classp      11 floatp        ← ここの内側を分けた
 4 null                  12 integerp      ← ここの内側を分けた
 5 consp                 13 numberp
 6 symbolp               14 functionp
 7 characterp            15 streamp
 8 stringp               (t) <object>
```

**分岐順序は動かしていない。** `floatp` / `integerp` の**内側**で `if` を1つ足す形に
した。整数は 11 番目まで落ちてから `fixnump`(タグ検査4命令)→ `bignump`(8命令)の
順で見る。安い述語を先に置く。

### 1-4 影響を受ける既存コード (4-4)

`class-of` の戻り値が `<INTEGER>` → `<FIXNUM>`/`<BIGNUM>`、`<FLOAT>` →
`<SINGLE-FLOAT>`/`<DOUBLE-FLOAT>` に変わる。全リポジトリを
`class-of` / `typep` / `subclassp` で grep した結果:

| 箇所 | 件数 | 判定 |
|---|---:|---|
| `class-of` の出現 | 45 | うち**修正が必要なのは 6 件** |
| `typep` / `subclassp` で数値型を見ているもの | 5 | **修正不要**(階層で吸収される) |

**修正した 6 件:**

| ファイル | 内容 |
|---|---|
| `test/lisp/fixnum_signed_arith_test.lisp:123-127` | `<integer>` 期待 5 件 → `<fixnum>` 3 件 / `<bignum>` 2 件。`typep ... '<integer>` の確認を 2 件追加 |
| `test/lisp/init_test.lisp:232` | `(eq (class-of 5) (%find-class '<integer>))` → `'<fixnum>`。`<bignum>` の確認を 1 件追加 |

**修正不要だったもの:**

- `(typep 5 '<integer>)` → `<fixnum>` ⊂ `<integer>` なので依然 `t`
- `(typep 5 '<float>)` → 依然 `nil`
- `(subclassp (%find-class '<integer>) (%find-class '<number>))` → 階層は不変
- 総称関数のディスパッチ(`%method-applicable-p` → `(subclassp (class-of arg) specializer)`)
  → `<integer>` や `<float>` を specializer にした `defmethod` は今までどおり適用される
- `report-condition` の `(%%class-name (class-of condition))` → 数値型と無関係

`class-of` が返しうるクラス名の総数は 45 → **51**(`<fixnum>` `<bignum>`
`<single-float>` `<double-float>` の4つと、別名 `<short-float>` `<long-float>` の2つ)。
`%find-class` は 51 要素の assoc 線形探索になる。

---

## 2. 仕様として決めたこと

### 2-1 内部表現

```
 63                            32 31            4  3   0
+--------------------------------+---------------+-----+
|  IEEE754 binary32 (32bit)      |  未使用(0)     | 0100|
+--------------------------------+---------------+-----+
```

- `os_make_single_float(float)` / `os_single_float_value(lisp_val_t)` /
  `os_is_single_float(lisp_val_t)`(runtime.h、後者は `static inline`)
- シフト量は `SINGLE_FLOAT_VALUE_SHIFT`(= 32)。**ハードコードしていない**
- double-float は従来どおり `MAGIC_FLOAT` の `TAG_INSTANCE`。
  **タグ 0xB は将来の double 即値化用に空けたまま**

### 2-2 `is_float` を single にも広げた

single-float が「普通の数」として振る舞うために直したのは `is_float` と
`os_float_value` の**2 箇所だけ**である。

```
is_float ─┬─ any_float      → primitive_add/subtract/multiply/divide の float 経路
          ├─ to_double      → 値の取り出し
          ├─ number_compare → = < > <= >=
          ├─ primitive_floatp1
          └─ floor/ceiling/truncate/round/sqrt/float
```

`os_float_value` は**即値とインスタンスの両方を受ける**ようにした。
ここを直さないと即値をアドレスとしてデリファレンスする。

JIT の `+`/`-` 高速 path(`za_emit_arith_call_or_inline`)は
`(a|b) & (TAG_MASK|FIXNUM_SIGN_BIT)` で弾くので、タグ 0x4 は必ず低速 path へ落ちる。
**JIT 側の変更は不要。**

### 2-3 リーダ

| 入力 | 結果 |
|---|---|
| `3.14f` / `3f` / `3.14d` / `f1` / `d2` | シンボル |
| `3f10` | single-float(3.0 × 10^10) |
| `1.5f0` / `1.5F0` | single-float |
| `1.5d0` / `1.5D0` | double-float |
| `1.5e0` / `1.5E0` / `1.5` | `*read-default-float-format*` に従う |
| `3.14e` | **read-error(従来どおり)** |

#### `e` の後に `f` を足す書き方はできない(副作用)

**`1.0e38f0` は意図どおりに読まれない。** `f` は**接尾辞ではなく指数マーカー**
なので、`1.0e38` の後にさらに `f0` が続く形になり、意図した 10^38 にならない。

single で 10^38 を書きたいなら **`1.0f38`** とする。負の指数も同様に `1.0f-40`。

これは §2-3 の「`f`/`d` は接尾辞ではなく指数マーカーにした」(`3.14f` を
シンボルのままにするため)という決定の帰結である。曖昧さを消す判断としては
正しかったが、**「`e` 記法で書いてから single にする」書き方が無くなった**という
副作用が付いている。

**実際に踏んだ。** PR #87(single-float 算術)の非正規化数テストで
`(/ 1.0f0 1.0e38f0)` と書き、`2.5343845f-7` という意図しない値になって
**テストとして機能していなかった**。`1.0f-40` と書き直して解決した。
非正規化数のように「その値でなければ意味がない」テストでは、
**書いた値が本当にその値かを印字で確かめること。**

#### `*read-default-float-format*`

- 値は `<single-float>` または `<double-float>` の**シンボル**
  (クラスオブジェクトではないので、クラス登録との起動順序に依存しない)
- **初期値は `<double-float>`**(`src/lisp/init.lisp`)。**指示書の `<single-float>` から変更している。理由は §2-3a**
- **フォールバック先も `<double-float>`**
  (`READ_DEFAULT_FLOAT_FORMAT_FALLBACK_IS_SINGLE` = 0、runtime.h)

フォールバックが効くのは次の3つで、どれも**エラーにせず既定へ倒す**
(リーダが落ちるほうが害が大きい):

1. `g_dynamic_bindings` がまだ空(ブート最初期)。シンボルを作る前に抜ける
2. 動的変数が未定義(`os_get_dynamic` が `nil` を返す)
3. 想定外の値が入っている(`<BOGUS>` 等)

**フォールバックを double にした理由**: single-float 導入前の挙動と一致するので、
`defdynamic` を評価するまでの読み取りで意味が変わらない。
ユニットテストのバイナリ(`g_dynamic_bindings` が空)でも既存の期待値がそのまま通る。

`load` は「1フォーム読む→評価する」の繰り返しなので、`init.lisp` の
`(defdynamic *read-default-float-format* '<single-float>)` より**前**に float
リテラルを置くとフォールバックで読まれる。この順序依存はコメントに明記した。

なお動的変数は `g_dynamic_bindings` という**単一のグローバル**で、
**プロセス間で共有される**(`documents/type-system-survey.md` §4)。
F1 で `setq` すると F2 の読み取りも印字も変わる。本作業では現状の仕組みに従い、
挙動を明記するにとどめた。

#### 2-3a 初期値を `<double-float>` にした理由(指示書からの変更点)

指示書 §3-2 は「**初期値は `<single-float>`**(CommonLisp と同じ)」としている。
実装して実測した結果、**演算の型昇格 (c) と整数→float 変換の型を決めるまで、
この既定値は成立しない**ことが分かったので、`<double-float>` にした。

##### 実測 1: `equal` はタグ一致を要求する

`values_equal`(runtime.c)は最初にタグを比べ、違えば偽を返す。
したがって `<single-float>` を既定にすると、

```lisp
(assert-equal 15.0 (+ 12 3.0))   ; 期待値 = single、実際 = double → NG
```

のように「リテラルは single、計算結果は double」という食い違いが
**テストスイート全体で表に出る**。(c) が入っていない以上、
`any_float(args)` が全部 double にするので避けようがない。

`test/lisp` の `(assert-equal <float literal> ...)` は **63 箇所**ある。

##### 実測 2: (c) が入っても直らないものがある

より本質的なのはこちら。`script_test`(init.lisp + init_test.lisp を
ホスト側で流す構成)を `<single-float>` 既定で走らせると
**14 件が NG** になり、その中身は次の形だった。

```lisp
(defun %approx= (a b) (< (abs (- a b)) 0.000000001))
(assert-equal t (%approx= 1.4142135623730951 (sqrt 2)))   ; NG
```

`1.4142135623730951` が単精度に丸められて 1.41421354 になり、
`(sqrt 2)`(倍精度 1.4142135623730951)との差 2.4e-8 が許容誤差 1e-9 を超える。

**`(sqrt 2)` のような整数→float 変換が double を返す以上、これは (c) を入れても
直らない。** 既定を `<single-float>` にするなら、
「整数→float はどちらの型を返すか」をライブラリ全体で決め直す必要がある。
それは本作業の範囲を大きく超える。

##### したがって

- 本 PR の既定は `<double-float>`。**float リテラルの型と値は従来どおり**で、
  既存コードの数値の振る舞いは変わらない(変わるのは `class-of` の戻り値だけで、
  それは (e) の目的そのもの)
- single-float は `1.5f0` と書くか、`*read-default-float-format*` を切り替えれば使える
- 既定を `<single-float>` へ倒すのは **`init.lisp` の1行**。(c) と
  整数→float 変換の型を決める PR でまとめて行うのが筋

#### 精度(**保証しない**)

**double で組み立ててから single へ落とす。**
現在のリーダは `mantissa * 10^total_exp` を浮動小数点の掛け算/割り算の
ループで組み立てており、`strtod` のような正しい丸めをしていない。

- decimal → single の直接変換に比べ、**二重丸め**が入る
- ただし元の組み立て自体が正確でないので、単精度に限った劣化ではない

**この実装は精度を保証しない。** 後から直接パースへ変更する余地を残す。

### 2-4 print

`*read-default-float-format*` と**型が一致すれば接尾辞なし、違えば接尾辞付き**。

```
既定 = <double-float>(現在)      既定 = <single-float>
  (print 1.5d0)  => 1.5            (print 1.5f0)  => 1.5
  (print 1.5f0)  => 1.5f0          (print 1.5d0)  => 1.5d0
  (print 1.5d20) => 1.5E20         (print 1.5d20) => 1.5d20
  (print 1.5f20) => 1.5f20         (print 1.5f20) => 1.5E20
```

- 固定小数点表記のときは `d0` / `f0` のように**0 指数を補う**
- E 表記のときは**指数マーカーそのもの**を `d` / `f` に差し替える
- 小文字にするのは、ISLisp の E 表記(大文字 `E`)と見た目で区別するため
- `print` / `~S` / `~A` / `~G` / `format-float` は**すべて同じ規則**を通る
  (表示経路によって型の見え方が食い違わないようにした)

#### 桁数 — 17 桁固定では駄目だった

`os_print_double_to_sink` は有効 17 桁(double を可逆にできる桁数)を上位から
取り出し、末尾の 0 を落としている。single をこれに通すと binary32 の丸め誤差が
そのまま見える:

```
1.5f20  →  1.50000002024002560E20   (17桁)
        →  1.50000002E20            (9桁にしても同じ)
```

「可逆に必要な 9 桁」を出す限りノイズは消えない。そこで single については
**同じ値に読み戻せる最短の桁数を 1〜9 で探す**形にした。

- 読み戻しの判定は `digits_to_double`(print.c)を通す。これは
  **reader.c の `parse_float_token` とまったく同じ手順**で組み立てる
- したがって「print した文字列をこのリーダで読み直すと元の float に戻る」ことが
  そのまま保証される
- **double 側には適用していない。** 既存の出力を1文字も変えないため

### 2-5 型階層

```
<number>
  <integer>
    <fixnum>
    <bignum>
  <float>
    <single-float>   (別名 <short-float>)
    <double-float>   (別名 <long-float>)
```

- **ISLisp 準拠を意図的に捨てる変更**である。ISLisp の整数クラスは `<integer>`
  ただ1つで、`<fixnum>`/`<bignum>` は存在しない(`type-system-survey.md` §4-1)
- `<short-float>` / `<long-float>` は**別名**。`*classes*` に**同一のクラス
  オブジェクトを2つ目の名前で登録**している。サブクラスにすると `class-of` が
  決して返さない到達不能クラスができるため
- `class-of` は `<SHORT-FLOAT>` / `<LONG-FLOAT>` を**決して返さない**

### 2-6 定数

| 定数 | 由来 |
|---|---|
| `*most-positive-fixnum*` | `FIXNUM_MAGNITUDE_MASK` → `%%FIXNUM-MAGNITUDE-MASK`(既存経路) |
| `*most-negative-fixnum*` | 上の符号反転(fixnum は符号＋絶対値なので対称) |
| `*most-positive-single-float*` | `SINGLE_FLOAT_MAX_BITS` → `%%MOST-POSITIVE-SINGLE-FLOAT` |
| `*most-negative-single-float*` | `SINGLE_FLOAT_MAX_BITS`\|符号bit → `%%MOST-NEGATIVE-SINGLE-FLOAT` |
| `*most-positive-double-float*` | `DOUBLE_FLOAT_MAX_BITS` → `%%MOST-POSITIVE-DOUBLE-FLOAT` |
| `*most-negative-double-float*` | `DOUBLE_FLOAT_MAX_BITS`\|符号bit → `%%MOST-NEGATIVE-DOUBLE-FLOAT` |

**Lisp 側に数値リテラルは1つも無い。**

負側も専用のプリミティブにしたのは、`(- 0 x)` と書くと (c) 未実装のため
single が double へ落ちてしまうからである。ビット単位では符号 bit を立てるだけなので
丸めも起きない。

**`*most-positive-float*` / `*most-negative-float*` も C 由来に変えた。**
従来は `1.7976931348623157E308` を直書きしていたが、

1. リーダの組み立てが正しい丸めをしないので、そもそも DBL_MAX ちょうどにならない
2. `*read-default-float-format*` の値しだいで型が変わってしまう
   (既定を `<single-float>` にすると **single に落ちて無限大になる**)

の2点でC側と黙ってずれる。値・型ともに従来(double 側の端)と同じになるよう
`*most-positive-double-float*` を参照する形にした。

`*pi*` も `3.141592653589793d0` と明示した。現在の既定(`<double-float>`)では
付けなくても同じ値だが、既定を切り替えたときに有効7桁の単精度へ落ちないようにする。

---

## 2-7 実装中に見つかった既存バグ

### `primitive_abs` が float を整数として分解していた

```c
lisp_val_t primitive_abs(...) {
    if ((val & TAG_MASK) == TAG_FIXNUM) { ... }
    signed_mag_t m;
    decompose(val, &m);        /* ← float もここへ来ていた */
```

`decompose` は非 fixnum を無条件に `(v & ~TAG_MASK)` としてデリファレンスし、
word1 を sign、word2 を limb 数、word3 を limb 配列ポインタとして読む。
`MAGIC_FLOAT` のインスタンスを渡すと **double のビットパターンを sign として
読む**という、元から間違った動作だった。

`(abs 2.0)` や `(abs -0.0)` が通っていたのは偶然である。どちらも
ビットパターンの下位32bitが0で、`(int)obj[1]` が 0 になり
「符号なし」と判定されて `val` がそのまま返っていた。

**single-float は即値なので、同じ経路に入ると即座に落ちる。**
`1.4142135f0 & ~TAG_MASK` は有効なアドレスではない。
`script_test` が `(abs (- a b))` でSIGSEGVしたのがこれである。

直した箇所は2つ:

1. `primitive_abs` に `is_float` の分岐を足した。**型は保存する**
   (single を渡したら single が返る。符号の除去なので (c) の対象ではない)
2. `decompose` が fixnum でも bignum でもない値を受けたら、
   デリファレンスせずマグニチュード0として返すようにした。
   整数専用の関数(`div`/`mod`/`gcd`/`lcm`/`isqrt`)に float を渡すのは
   元々定義域エラーであり、**正しい答えを出すつもりはない**。
   「未定義の入力で segfault しない」ことだけを保証する

### `primitive_class_supers` が nil をデリファレンスしていた

```c
lisp_val_t primitive_class_supers(lisp_val_t args, lisp_val_t env) {
    UINT64 *obj = (UINT64 *)(cc_car(args) & ~TAG_MASK);
    return obj[2];      /* ← nil が来ると g_nil_cell の外を読む */
}
```

`subclassp` は `(eq c1 c2)` が偽なら無条件に `%%class-supers` を呼ぶ。
`class-of` が **nil を返したとき**(そのクラス名が `*classes*` に未登録のとき)、
ここへ nil が渡る。`g_nil_cell` は 16byte しかないので `obj[2]` は**その外**を読む。

読めた値がたまたま nil なら素通りし、そうでなければ落ちる。つまり
**グローバル変数の並び順しだいで壊れる**状態だった。

`lisp_compiled_test`(組み込みクラスを登録せずに AOT コードだけを叩く構成)で
これが表に出た。single-float でグローバルが増えてリンカの配置が変わり、
今まで偶然 nil を読んでいた場所が別の値になった。AddressSanitizer の出力:

```
ERROR: AddressSanitizer: global-buffer-overflow
    #0 primitive_class_supers src/c/runtime.c:8586
    #1 lisp_ll_subclassp__step_fixed
    #2 lisp_ll_method_applicable_p__fixed
    ...
0x... is located 0 bytes to the right of global variable 'g_nil_cell' (size 16)
```

クラスオブジェクトでない値には**空リストを返す**ようにした。
`subclassp` は正しく偽になり、意味も変わらない。

---

## 2-8 残した課題

| 課題 | 備考 |
|---|---|
| **(c) 演算の型昇格** | `single × single → single`。次の作業。これが入るまで single 即値の性能上の利点は出ない |
| **`*read-default-float-format*` の既定を `<single-float>` へ** | (c) と「整数→float 変換はどちらの型を返すか」を決めてから。`init.lisp` の1行 |
| **リーダの正しい丸め** | `mantissa * 10^exp` のループをやめて直接パースへ。精度を保証できるようになる |
| **double 側の最短表現** | single にだけ入れた。double へ広げると既存の出力が変わる(`0.33333333333333331` → `0.3333333333333333` など)ので、やるなら独立した PR で |
| **`create-string` 等の整数→float 変換の型** | `(sqrt 2)` は現状 double を返す。既定を single にするならここも決める必要がある |
| **double-float の即値化(タグ 0xB)** | 仮数だけで52bitあるので即値にはできない。枠は空けたまま |
| **ratio(タグ 0xD)** | 未着手 |

---

## 3. 本作業に含めないもの

- **(c) 演算の型昇格。** `single × single → single` は次の作業。
  現状は `any_float(args)` で全部 double にしているので、
  **`(+ 1.5f0 1.5f0)` は `<DOUBLE-FLOAT>` を返す**
- `declare` / `declaim` の型宣言(Phase 4a-2)
- double-float の即値化(タグ 0xB は空けたまま)
- ratio(タグ 0xD)
- 最短表現の double への適用(既存出力を変えないため見送り)

---

## 4. 検証

### 4-1 追加したテスト

| 場所 | 件数 | 内容 |
|---|---:|---|
| `test/c/runtime_test.c` | 4関数 | ビットパターンの往復(0.0/-0.0/非正規化数/inf/NaN)、数としての振る舞い、C由来の境界定数、GCを跨いだ不変性 |
| `test/c/reader_test.c` | 11関数 | f/F/d/D マーカー、接尾辞にならないこと、`f1`/`d2` がシンボルのままであること、`3.14e` が read-error のままであること、`*read-default-float-format*` の両方向とフォールバック |
| `test/c/print_test.c` | 6関数 | 既定と一致/不一致の接尾辞、E表記のマーカー差し替え、single の0と負値 |
| `test/lisp/single_float_test.lisp` | 118 | 上の全部を実機(QEMU)側で。型階層・別名・定数6つ・read/print往復・GC・(c)未実装の明示 |

`test/lisp/single_float_test.lisp` は `qemu_boot_test.lisp` から load する。

### 4-2 ビットパターンの往復(5-1)

`test_single_float_bit_pattern_round_trip` で12パターンを確認した。

- `+0.0` / `-0.0`(**別のビットパターンとして保たれる**。数としては `equal` で等しい)
- 最小の非正規化数(`0x00000001`)/ 最大の非正規化数(`0x007FFFFF`)/ 最小の正規化数
- `FLT_MAX` / `-FLT_MAX`
- `+inf` / `-inf` / quiet NaN

**union でしか触らないので、扱えない値は無い。** inf も NaN もそのまま往復する。
即値が bit4-31 を汚さないことも同時に確認している。

C 側で構築した値とリーダが作った値の一致は `test_os_read_single_float_marker_lowercase`
ほかで確認した(`os_single_float_value(v) == 1.5f` の形で C リテラルと比べている)。

### 4-3 GC(5-6)

2段構えで見た。

1. `test_single_float_is_a_number_and_a_float` / `test_single_floats_survive_gc_unchanged`
   (C側): ルートに置いた single-float 8個が `os_gc_collect()` 2回を跨いで
   ビットパターンも値も変わらないこと
2. `test/lisp/single_float_test.lisp` §9(実機): ゴミを作って
   **実際に GC が起きたことを `%%gc-collect-count` で確認してから**値を見る
   (発火していなければ何も検証していないことになるため、前提条件として確かめる)

   捨てるゴミの量は**ヒープ総量から決める**(`(* 3 (%%heap-total-bytes))`)。
   最初は固定の10万回(約3MB)にしていたが、**256M では一度もGCが起きず**
   このアサーションが落ちた。固定回数にすると `QEMU_MEM` を増やしたときに
   テストが黙って何も見なくなる。`aot_leaf_gc_test` が同じ理由で
   ヒープ総量の数倍を確保している

GC がタグ 0x4 をポインタとして追いかけていれば、生のビットパターンを
ヒープオブジェクトとして複製して word0 に転送先を書き込む(静かなヒープ破壊)。
値が変わるか落ちるかのどちらかになる。どちらも起きていない。

### 4-4 SMALL_HEAP_SIZE の再調整

組み込み関数を5個(`%%SINGLE-FLOAT-P` と float 境界の定数4つ)足したので、
`os_bootstrap` 直後の生存量が増え、**82KB ではハングするようになった**
(memory: `builtin_count_small_heap_calibration` が記録しているとおり、
組み込みを1つ足すたびにここは動く)。

掃引結果(`timeout` は**コンテナの中**で効かせた):

```
82KB: ハング — GC後も確保不能(4625件出たところで止まる)
83KB: 5416 OK / 0 NG
84KB: 5416 OK / 0 NG
85KB: 5416 OK / 0 NG   ← 採用(窓83〜86の中央寄り)
86KB: 5416 OK / 0 NG
87KB: isqrtテストの「GCが発火する」アサーションがNG
88KB: bignum加算テストのアサーションもNG
```

### 4-5 回帰

| 実行 | 結果 |
|---|---|
| `make test` | **8457 OK / 0 NG**(本作業前は 8403。+54) |
| `make build`(実機 PE32+) | 成功 |
| `make test-qemu`(256M) | **3705 passed / 0 failed**(本作業前は 3584。+121) |
| `QEMU_MEM=96M make test-qemu` | **3705 passed / 0 failed** |

JIT の焼き込み監査は `gc-heap=0`(`#count dis-add kernel=17 immobilized=0 gc-heap=0` /
`#count dis-caller kernel=26 immobilized=1 gc-heap=0`)。
生成コードに GC ヒープ上のアドレスは1つも入っていない。

#### 途中で1件落ちたもの(修正済み)

最初の 256M の実行で `(> *SF-GC-AFTER* *SF-GC-BEFORE*)` が NG になった。
**GC が1回も起きていなかった** — ゴミを固定の10万回(約3MB)しか作っておらず、
256M のヒープでは足りなかった。

これは「テストが壊れた」のではなく「**テストが何も検証していなかった**」ケースである。
前提条件として GC 回数を見ていたので気づけた。ヒープ総量から量を決める形に直した。

