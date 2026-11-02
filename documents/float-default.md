# float 既定の確定と多重定義の解消

> 実施日: 2026-09-19 / ブランチ: `feature/float-default-consolidation`(分岐元 `feature/compiler-optimization` `a7f5c22`)
> 関連: `documents/single-float.md`(PR #79)、`documents/float-contagion.md`(PR #80)、
> `documents/float-math-contagion.md`(PR #81)、`documents/convert-float.md`(PR #82)
>
> **これは機能追加ではない。** 決定の確定と、静かにずれる経路の封鎖である。

---

## 1. 決定: `*read-default-float-format*` の既定は `<double-float>` のまま

**既定を `<single-float>` へ切り替えないことを決めた。**

PR #81(c-2)で「既定を single へ戻せる可能性が出る」と書いた前提条件は
実際に揃っている。数学関数は既定に従うようになり、`(sqrt 2)` も
single を返せる。**技術的には切り替えられる。それでも切り替えない。**

### 理由

1. **素朴に書いた数値計算が黙って精度を失う。**
   single 既定だと `1.5` の有効桁が約7桁になる。PR #79 で
   `(%approx= 1.4142135623730951 (sqrt 2))` の類が14件落ちたのは、
   まさにこれが表に出た形だった。あのときは「(c) が入れば直る」と考えたが、
   **直るのは型の不一致であって、精度が落ちること自体は直らない。**

2. **single 即値化の価値は既定とは独立に成立している。**
   PR #80 の実測で single = 0 byte/演算、double = 32 byte/演算、
   速度は float 演算部分で約1.9倍。**性能が要る場所で `1.5f0` と書けば、
   この利益はすでに全部得られる。** 既定を変えて得られるのは
   「`f0` と書かずに済む」という記述の手間だけである。

3. **明示的に `f0` を書いた人は精度を落とすことを承知している。**
   既定を変えるということは、「明示せずに速い経路へ乗れる」という利便性のために
   **何も言っていない人の精度を下げる**ことである。
   default は「黙っていたときに何が起きるか」を決める場所なので、
   安全側に倒すのが筋である。

4. **CommonLisp が single 既定なのは 1984 年の判断である。**
   当時は double が高価だった。いま同じ設計を選ぶ処理系は多くない。
   ISLisp は `*read-default-float-format*` 自体を規定していないので、
   CL に合わせる義務もない。

### したがって

- 既定は `<double-float>`
- single を使いたいときは **`1.5f0` と書く**か、`convert` で変換する(PR #82)
- `*read-default-float-format*` を動的に切り替える機能は残す
  (`dynamic-let` で囲めば局所的に single にできる)

---

## 2. 多重定義の解消

### 2-1 直前の状態 — 既定が3箇所にあった

`documents/convert-float.md` §7 で洗い出した状態。

| # | 場所 | 効く範囲 |
|---|---|---|
| A | `init.lisp` の `(defdynamic *read-default-float-format* '<double-float>)` | 実行時 |
| B | `transpile.lisp` の `read-all-forms` のホストCL束縛 | AOT変換時 |
| C | `runtime.h` の `READ_DEFAULT_FLOAT_FORMAT_FALLBACK_IS_SINGLE` | 変数が読めないとき |

**単一の真実源ではなく、食い違っても何も検出しなかった。**

### 2-2 A と C を1つにした

**C を単一の真実源にし、A はそこから導出する。**
`*most-positive-fixnum*` が `FIXNUM_MAGNITUDE_MASK` から来ているのと同じ形である。

```
runtime.h  DEFAULT_FLOAT_FORMAT_IS_SINGLE
    ↓ primitive_default_float_format
    ↓ %%DEFAULT-FLOAT-FORMAT
init.lisp  (defdynamic *read-default-float-format* (%%default-float-format))
```

**定数名も変えた。** 以前の `READ_DEFAULT_FLOAT_FORMAT_FALLBACK_IS_SINGLE` は
「フォールバック値」という名前だったが、実体は「既定」である。
`DEFAULT_FLOAT_FORMAT_IS_SINGLE` にして、次の2つを兼ねることを明示した。

1. `init.lisp` の `defdynamic` が読む初期値
2. 動的変数が読めないとき(ブート最初期・unbound・想定外の値・Cのユニットテスト)の
   フォールバック先

**この2つを別の定数にすると、片方だけ動かしたときに
「init.lisp を評価するまでとそれ以降で既定が違う」という状態が作れてしまう。**
同じでなければならないものは同じ定数にする。

**`init.lisp` に `<double-float>` というシンボルは1つも書かれていない。**

### 2-3 B は probe テストで守る

**`transpile.lisp` はホスト CL 上で動くので `runtime.h` を読めない。**
ここだけは手で合わせるしかない。

そこで**ずれたら落ちる仕掛け**を入れた。

```lisp
;; src/lisp/init_aot.lisp
(defun %aot-float-format-probe () 1.5)
```

- `init_aot.lisp` は**AOTコンパイルされる**(インタプリタは読まない)ので、
  この関数が返す値の型は **`transpile.lisp` が何を既定だと思っているか**そのものになる
- `test/lisp/float_default_test.lisp` が、それを**実行時の既定と比べる**

```lisp
(assert-equal (%%class-name (class-of (parse-number "1.5")))
              (%%class-name (class-of (%aot-float-format-probe))))
(assert-equal (dynamic *read-default-float-format*)
              (%%class-name (class-of (%aot-float-format-probe))))
```

**「AOT対象に裸のfloatリテラルが1つも無いから踏まれていない」という状態を
逆手に取った形である。** わざと1つ置く。

probe には**それが probe であることを示すコメント**を付けてある。
意図を知らない人が「使われていない関数」として消すと、検出が黙って無効になる。

---

## 3. `(convert x <float>)` は変数を見ている(決め打ちではない)

ISLisp の変換表にある `(convert 3 <float>) => 3.0`(仕様書 p.65、
`test/lisp/isiki_test.lisp:568`)が**何によって成立しているか**を確定させた。

```
(convert 3 <float>)
  → %convert の ((<float>) ... ((integerp obj) (float obj)) ...)
  → primitive_float
  → os_make_float_of_kind(math_result_kind(args), ...)
  → math_result_kind: 整数だけなので os_read_default_float_format_is_single()
```

**`*read-default-float-format*` を見た結果である。** double 決め打ちではない。

PR #81(c-2)で `float`(FLOAT関数)を既定に従わせたときに、この性質が入った。
既定が `<double-float>` である以上、仕様例の `3.0`(double)と一致する。

**「たまたま一致している」のではなく、既定が double だから一致している。**
既定を single にすれば `(convert 3 <float>)` は single を返す。
`test/lisp/float_default_test.lisp` §4 が `dynamic-let` で両方向を固定している。

---

## 4. 記録: 本作業では直さない既知の問題

一連の float 作業(PR #79〜#82)で判明した、**直していない**問題。

### 4-1 `(= nan nan)` が真を返す(**解決済み**)

> **【追記】`feature/nan-comparison` で解決した。** `number_compare` を三値から
> 非順序を含む四値へ改め、呼び出し側を述語経由にした。`(= nan nan)` は偽、
> `(/= nan nan)` は真になる。NaN 判定は `(/= x x)` で足りるため、
> 下記が提案していた専用述語は不要になった。詳細は `documents/nan-comparison.md`。

> 以下は当時の記録である。

`number_compare`(runtime.c)が IEEE 754 の非順序性を扱っていない。

```c
return da < db ? -1 : (da > db ? 1 : 0);
```

NaN はどちらの比較も偽になるので **0(等しい)** が返る。
IEEE では NaN はどの値とも等しくなく、**自分自身とも等しくない**のが正しい。

`<` `>` `<=` `>=` も同じ経路なので、同様に怪しい。
IEEE では NaN との比較はすべて偽(`<=` `>=` も含む)であるべきだが、
現在は `<=` が真を返す。

**実害が小さいのは NaN を作る経路が限られているからにすぎない**
(`(/ 0.0d0 0.0d0)` 等。`(sqrt -1.0d0)` は明示的な検査で `<domain-error>` になる)。
**性質としては壊れている。**

**副作用: NaN の判定に `(= x x)` が使えない。**
PR #82 のテストでは印字結果(`"NAN"`)で判定している。
直すときは、NaN 判定の述語(`%float-nan-p` 相当)も一緒に用意するのが筋である。

直すと比較演算全体の挙動が変わるため、独立した作業にすること。

### 4-2 クラスの同一性判定が2通りある

**別名(`<short-float>` / `<long-float>`)はクラスオブジェクトの同一性で
実現されている**(PR #79。`*classes*` に同じオブジェクトを2つの名前で登録)。

そのため**オブジェクトで比較する箇所では自動的に働く**。

| 箇所 | 比較のしかた | 別名 |
|---|---|---|
| `subclassp` | `(eq c1 c2)` + `%%class-supers` を辿る | **働く** |
| `typep` | designator を `%find-class` で解決してから `subclassp` | **働く** |
| `class-of` | `%find-class` で**生成する**側。ユーザ入力で分岐しない | 対象外(別名は決して返さない) |
| **`%convert`** | **`(case class-name ...)` = シンボル一致** | **働かない。`case` のキーに明示が要る** |

`src/lisp` 全体を見て、**ユーザが渡したクラス指定子で分岐しているのは
`%convert` だけ**である。ほかはすべてオブジェクト比較か、名前を生成する側だった。

**次に別名を足すときは `%convert` の `case` にも足すこと。**
`%convert` にはその旨のコメントを入れてある。

`%convert` をオブジェクト比較へ作り変える案もあるが、既存7つの変換先すべての
挙動を変えることになるので採っていない(PR #82)。

### 4-3 `make test` は Lisp 層の変更に対して当てにならない

c-2(PR #81)で `%quotient2` の `(float ...)` 見落としが
**`make test`(8489件)を通過し、実機テストでしか出なかった。**

`quotient` / `reciprocal` が Lisp 側の定義で、C のユニットテストには
乗らないためである。

**Lisp 層(`src/lisp/*.lisp`)を変えたときは、`make test` が通っても
安心材料にならない。** 必ず `make test-qemu` を通すこと。

同じ理由で、PR #82 の1周目に落ちた1件(`(convert 'abc <string>)` の期待値)も
実機テストでしか出なかった。

### 4-4 float リテラルの掃き出しは「リテラル」だけでは足りない

c-2(PR #81)で `(float dividend)` のような**変換呼び出し**を見落とした。

掃き出すべきは「**入力の型を継承せずに float を作るもの**」全部であり、
リテラルはその一種にすぎない。

`documents/float-math-contagion.md` §3-2 に記録済み。

---

## 5. 検証

### 5-1 [最重要] わざとずらして、検出器が落ちることを確認した

**試していない検出器は働かない。** 実際にずらして確かめた。

手順:

1. `transpile.lisp` の `read-all-forms` の束縛を `'double-float` → `'single-float` に変える
2. `make build` → 生成された `src/c/lisp_compiled.c` を見ると、probe が
   `os_make_float(1.5)` から **`os_make_single_float(1.5f)` に変わっている**
3. `make test-qemu`

結果:

```
==== isiki tests: 4049 passed, 3 failed ====
[NG] (%%CLASS-NAME (CLASS-OF (%AOT-FLOAT-FORMAT-PROBE))) => <SINGLE-FLOAT> (expected <DOUBLE-FLOAT>)
[NG] (%%CLASS-NAME (CLASS-OF (%AOT-FLOAT-FORMAT-PROBE))) => <SINGLE-FLOAT> (expected <DOUBLE-FLOAT>)
```

**probe の2つのアサーションが両方とも落ちた。** 設計どおりである。

(3件目は当方のテストの書き間違いで、`(class-of (dynamic *read-default-float-format*))`
と書いていた。この変数が保持しているのは**クラス名シンボル**なので `<SYMBOL>` になる。
修正済み。**この検証をしていなければ、そのまま混入していた。**)

4. `transpile.lisp` を元に戻し、`git diff` が空になることを確認

`runtime.h` 側は `init.lisp` が導出しているので、**ずれること自体が起きない**
(定数を変えれば `%%default-float-format` の戻り値が変わり、`defdynamic` が
それをそのまま使う)。確認は §5-2 の「既定が C 側と一致していること」で兼ねる。

### 5-2 挙動が変わっていないこと

本作業は既定を変えていないので、数値の挙動は一切変わらない。

```lisp
(class-of 1.5)          ; => <DOUBLE-FLOAT>
(class-of 1.5f0)        ; => <SINGLE-FLOAT>
(class-of (sqrt 2))     ; => <DOUBLE-FLOAT>
(convert 3 <float>)     ; => 3.0
```

### 5-3 回帰

| 実行 | 結果 |
|---|---|
| `make test` | **8489 OK / 0 NG**(変化なし) |
| `make build`(実機 PE32+) | 成功 |
| `make test-qemu`(256M) | **4051 passed / 0 failed**(本作業前は 4032) |
| `QEMU_MEM=96M make test-qemu` | **4051 passed / 0 failed** |

`SMALL_HEAP_SIZE` は触っていない。組み込みを1つ(`%%DEFAULT-FLOAT-FORMAT`)
足したので窓が動く可能性を見ていたが、85KB のまま `runtime_test` が通った。

---

## 6. この作業で float 関連は一区切り

| PR | 内容 |
|---|---|
| #79 | single-float の即値化、型階層(`<fixnum>` / `<bignum>` / float 各形式) |
| #80 | 算術と比較の型昇格(c-1) |
| #81 | 数学関数の型保存(c-2) |
| #82 | `convert` による形式変換 |
| 本PR | 既定の確定と多重定義の解消 |

残っている float 関連の課題は §4 の4件と、
`documents/single-float.md` / `documents/float-contagion.md` に記録した
将来の枠(double-float の即値化 = タグ 0xB、ratio = タグ 0xD)である。
