# `convert` による float 形式の変換

> 実施日: 2026-09-19 / ブランチ: `feature/convert-float-format`(分岐元 `feature/compiler-optimization` `55f0433`)
> 関連: `documents/single-float.md`(PR #79)、`documents/float-contagion.md`(PR #80)、
> `documents/float-math-contagion.md`(PR #81)

---

## 0. 先に結論

1. **狭める手段は C 側に作るしかなかった。** 型昇格(c-1)は広いほうへ寄せるので
   `(* double 1.0f0)` は double のまま、`(float x)` は float をそのまま返す。
   Lisp だけでは double を single へ落とせない。`%%NARROW-TO-SINGLE-FLOAT` を足した。
2. **`%convert` のディスパッチはクラス名シンボルの `case` である。**
   クラスオブジェクトの同一性ではないので、**`<short-float>`/`<long-float>` は
   自動では効かない**(§4-1)。`case` のキーに並べて明示した。
3. **`<float>` を `<domain-error>` にはできない。** 指示書 §3-3 は抽象クラスを
   エラーにすると定めているが、`<float>` は **ISLisp の変換表にある**うえ、
   仕様例 `(convert 3 <float>) => 3.0` が既存テストに入っている(§3-2)。
4. **無限大と NaN は作れる。** `(/ 1.0d0 0.0d0)` / `(/ 0.0d0 0.0d0)`。
   どちらも single で表現できるのでそのまま通す(§4-2)。
5. **`(= nan nan)` が真を返す。** `number_compare` が IEEE の非順序性を扱っていない。
   本作業の対象外だが、NaN の判定に `(= x x)` が使えないので記録しておく(§4-2)。
6. **`*read-default-float-format*` の既定は3箇所に散らばっている**(§7)。
   単一の真実源ではなく、**食い違っても何も検出しない。**

---

## 1. 仕様

### 1-1 対応する組み合わせ

| 変換元 | 変換先 | 挙動 |
|---|---|---|
| double | `<single-float>` | **狭める。** 精度は落ちる。範囲外は §1-2 |
| single | `<double-float>` | 広げる。値は保たれる |
| single | `<single-float>` | 恒等 |
| double | `<double-float>` | 恒等 |
| fixnum / bignum | `<single-float>` | float 化。範囲外は §1-2 |
| fixnum / bignum | `<double-float>` | float 化 |
| 数値でない | どちらも | `<domain-error>` |

別名 `<short-float>` / `<long-float>` はそれぞれ `<single-float>` / `<double-float>` と同じ。

**float → 整数は含めない。** `floor`/`ceiling`/`round`/`truncate` が既にあり、
丸め方向を引数なしで決められないため。

### 1-2 範囲外とその境界

| 入力 | 挙動 | 理由 |
|---|---|---|
| single の最大値を超える有限値 | **`<domain-error>`** | 有限の答えが無い |
| ちょうど single の最大値 | 通る | 境界のオフバイワンに注意 |
| **無限大** | そのまま無限大 | single でも表現できる値である |
| **NaN** | そのまま NaN | 同上 |
| single の非正規化数より小さい値 | **0 へ丸める。エラーにしない** | 0 は有限の表現可能な結果で、丸めとして連続している |
| double にした時点であふれる bignum | **`<domain-error>`** | 元が有限なので「あふれた」であって「無限大だった」ではない |

**アンダーフローをエラーにしないのは、オーバーフローと非対称に見えるが意図的である。**
0 への丸めは正しく丸めた結果だが、オーバーフローには有限の答えが無い。

### 1-3 `convert` の書き方

ISLisp の `convert` は class-name を**評価しない**ので `(convert 3 <float>)` と裸で書く。

**クオート付き `(convert 3 '<float>)` も同じ意味に取るようにした。**
評価しない引数にクオートを付けたくなるのは自然な間違いで、そのまま渡すと
class-name が `(quote <float>)` というリストになり、`case` がどの枝にも当たらず
**原因の分かりにくい `<domain-error>`** になる。仕様準拠のプログラムがこの形に
意味を持たせることはないので、受けて差し支えない。

---

## 2. 実装

### 2-1 狭める — `%%NARROW-TO-SINGLE-FLOAT`(C)

**Lisp だけでは書けない。** c-2 の検証で判明したとおり、
double を single へ狭める演算子が言語側に存在しない。

- 型昇格は広いほうへ寄せる → `(* double 1.0f0)` は double のまま
- `(float x)` は float をそのまま返す(型保存)

そこで C に1つ足した。**丸めと範囲判定の両方をここで行う。**

範囲判定を C に寄せたのは §4-3 の理由による。`(float)(double)` の結果が
無限大になったかだけで判定すると、**入力が元々無限大だった場合と、変換で
あふれた場合を区別できない。** そこで「入力が float か整数か」で先に分ける。

```c
if (is_float(val)) {
    double d = os_float_value(val);
    if (d != d)                              return 単精度NaN;        /* NaN */
    if (d > max_double || d < -max_double)   return 単精度無限大;      /* 元から無限大 */
    if (d > max_single || d < -max_single)   return nil;              /* 範囲外 */
    return 単精度へ丸める;
}
if (整数) {
    double d = to_double(val);
    if (d > max_double || d < -max_double)   return nil;   /* 元は有限なのであふれた */
    if (d > max_single || d < -max_single)   return nil;   /* 範囲外 */
    return 単精度へ丸める;
}
return nil;   /* 数値でない */
```

範囲外では **nil を返し、`<domain-error>` の送出は Lisp 側に任せる**。
期待クラスの指定や条件クラスの組み立ては Lisp のほうが素直だからである。
戻り値が single-float(タグ 0x4)か nil(TAG_CONS のセンチネル)かはタグで
区別できるので、正常値と混ざることはない。

**境界値はハードコードしない。** PR #79 で C 側に作った
`SINGLE_FLOAT_MAX_BITS` / `DOUBLE_FLOAT_MAX_BITS` から union 経由で導く。

### 2-2 広げる — Lisp で書ける

`(* obj 1.0d0)`。1.0 倍は値を変えず(無限大・NaN も素通しする)、
型昇格の規則で整数・single・double のどれを渡しても double になる。

---

## 3. 調査

### 3-1 `convert` の現状(指示書 §4-1)

`src/lisp/init.lisp` の `%convert`。**C 側には無い。**

```lisp
(defun %convert (obj class-name)
  (case class-name
    ((<character>) ...)
    ...))
```

#### 対応していた組み合わせ(全数)

| 変換先 | 受け付ける入力 |
|---|---|
| `<character>` | character(恒等)、integer(`code-char`) |
| `<integer>` | integer(恒等)、character(`char-code`)、string(`parse-number`) |
| `<float>` | float(恒等)、integer(`float`)、string(`parse-number`) |
| `<symbol>` | symbol(恒等)、string、character |
| `<string>` | string(恒等)、symbol、character、number |
| `<general-vector>` | general-vector(恒等)、string、list |
| `<list>` | list(恒等)、string、general-vector |
| 上記以外 | `<domain-error>`(`(t (%convert-error ...))`) |

#### ディスパッチは**名前の比較**

`(case class-name ...)` は `%case-expand` が `(member key '(k1 k2 ...))` へ
展開する形で、**クラスオブジェクトの同一性ではなくシンボルの一致**を見ている。

したがって **`<short-float>`/`<long-float>` は自動では効かない。**
`*classes*` 上は同じクラスオブジェクトを指していても(PR #79)、
`case` のキーに現れない名前は `(t ...)` へ落ちて `<domain-error>` になる。

**対処**: `case` のキーに並べて明示した。

```lisp
((<single-float> <short-float>) (%convert-to-single-float obj class-name))
((<double-float> <long-float>)  (%convert-to-double-float obj class-name))
```

クラスオブジェクトの比較へ作り変える案も考えたが、既存の7つの変換先すべての
挙動を変えることになり、回帰の範囲が広すぎる。**将来別名を足すときは
`case` のキーにも足す必要がある**ことをコメントに書いた。

### 3-2 `<float>` を `<domain-error>` にはできない(指示書 §3-3 からの変更)

指示書 §3-3 は

```lisp
(convert 1.5d0 '<float>)     ; => <domain-error>
(convert 1.5d0 '<number>)    ; => <domain-error>
```

と定めているが、**`<float>` については採れなかった。**

- **ISLisp の変換表に `<float>` がある。** 仕様 §17 の conversion table は
  `<character>` / `<integer>` / `<float>` / `<symbol>` / `<string>` /
  `<general-vector>` / `<list>` を対象としている
- **仕様例が既存テストに入っている。** `test/lisp/isiki_test.lisp:568`
  `(assert-equal 3.0 (convert 3 <float>))` は仕様書 p.65 の転記である
- 指示書 §3-1 自身が「**`convert` が既に実装している組み合わせを壊さないこと**」と
  定めており、§3-3 と衝突する

したがって **`<float>` は従来どおり通す**(どちらの形式になるかは
`*read-default-float-format*` に従う)。`<number>` をはじめ変換表に無いクラスは
**従来から `(t ...)` の枝で `<domain-error>` になっており、§3-3 の要求は
そちらでは既に満たされている。**

### 3-3 無限大と NaN(指示書 §4-2)

**どちらも作れる。**

| 式 | 結果 |
|---|---|
| `(/ 1.0d0 0.0d0)` | `+INF` |
| `(/ -1.0d0 0.0d0)` | `-INF` |
| `(/ 0.0d0 0.0d0)` | `NAN` |
| `(+ *most-positive-single-float* *most-positive-single-float*)` | single の `+INF`(c-1 のテストが既に固定している) |
| `(sqrt -1.0d0)` | **`<domain-error>`**(NaN ではない。明示的な検査がある) |

0 除算が IEEE の値を返すのは `primitive_divide` の既知の簡略化である
(「domain-error 未実装のための簡略化」とコメントがある)。

#### `(= nan nan)` が真を返す

`number_compare` は

```c
return da < db ? -1 : (da > db ? 1 : 0);
```

で、NaN はどちらの比較も偽になるので **0(等しい)を返す。**
IEEE の非順序性を扱っていない。

**本作業の対象外**(`convert` とは無関係で、直すと比較全体の挙動が変わる)だが、
**NaN の判定に `(= x x)` が使えない**ので、テストでは印字結果
(`"NAN"`)で判定している。

### 3-4 範囲判定(指示書 §4-3)

`*most-positive-single-float*` は **PR #79 で single-float になっている**
(C 由来の `SINGLE_FLOAT_MAX_BITS` から導出)。指示書 §5-3 は
「double として保持されているはず」としているが、実際は single である。

そのため `(convert *most-positive-single-float* <single-float>)` は**恒等の経路**を
通り、狭める側の境界を踏まない。テストでは

```lisp
(convert (convert *most-positive-single-float* <double-float>) <single-float>)
```

と double 経由で往復させ、**狭める側の境界を実際に踏む**形にしてある。

---

## 4. 検証

### 4-1 追加したテスト

`test/lisp/convert_float_test.lisp`(86件、新規)。

| 節 | 内容 |
|---|---|
| 1 | 基本の組み合わせ(double↔single、整数から、bignum から、クオート付き) |
| 2 | 値の保存。single→double→single は戻り、**double→single→double は戻らない** |
| 3 | 範囲外(正負両方、bignum のあふれ) |
| 3b | **境界ちょうど。** double 経由で往復させ、狭める側の境界を実際に踏む |
| 4 | アンダーフローが 0 になり、エラーにならない |
| 5 | 無限大 / NaN がそのまま通る |
| 6 | 抽象クラス・非数値・float→整数 |
| 7 | 別名 `<short-float>` / `<long-float>` |
| 8 | **既存の `convert` 全組み合わせ**(§3-1 で列挙したもの) |

### 4-2 回帰

| 実行 | 結果 |
|---|---|
| `make test` | **8489 OK / 0 NG**(変化なし) |
| `make build`(実機 PE32+) | 成功 |
| `make test-qemu`(256M) | **4032 passed / 0 failed**(本作業前は 3946) |
| `QEMU_MEM=96M make test-qemu` | **4032 passed / 0 failed** |

`SMALL_HEAP_SIZE` は触っていない。組み込みを1つ(`%%NARROW-TO-SINGLE-FLOAT`)
足したので再調整が要るかと見ていたが、`make test` の
`runtime_test` はそのまま通った(85KB のまま窓の中にある)。

#### 1周目に落ちた1件

`(convert 'abc <string>)` の期待値を `"abc"` と書いていた。
シンボル名は intern 時に大文字化される(`os_make_symbol` が
`os_make_string_for(name, 1 /* uppercase */)` を呼ぶ)ので `"ABC"` が正しい。
**テスト側の誤りで、製品の挙動は正しかった。**

c-2 の教訓どおり、`convert` は Lisp 側の定義なので `make test` では
この種の誤りは検出できない。実機テストで出た。

---

## 5. 本作業に含めないもの

- float → 整数の変換(`floor`/`truncate` がある)
- `(= nan nan)` の IEEE 非順序性(§3-3)
- `*read-default-float-format*` の既定変更(§7)
- double-float の即値化(タグ 0xB は将来用のまま)

---

## 6. 性能

`convert` は変換のたびに呼ばれる関数で、ホットループの対象ではない。
**計測していない。**

single へ変換した結果は即値なのでヒープを確保しない(c-1 の
`os_make_float_of_kind` と同じ経路)。double へ変換すると
`MAGIC_FLOAT` を1つ確保する。これは c-1 で実測済みの性質そのもので、
本作業で変わっていない。

---

## 7. 保留中の判断への入力: `*read-default-float-format*` の既定は3箇所にある

指示書 §8 が「この2つが単一の真実源になっているか、手で合わせる形かによって、
切り替えの手順が変わる」として確認を求めている。**調べた結果、2つではなく3つあり、
どれも手で合わせる形である。**

| # | 場所 | 値 | 効く範囲 |
|---|---|---|---|
| A | `src/lisp/init.lisp:960` `(defdynamic *read-default-float-format* '<double-float>)` | `<double-float>` | 実行時のリーダ・プリンタ・数学関数・`float`/`convert` |
| B | `src/lisp/transpile.lisp:2184` `read-all-forms` の `(let ((*read-default-float-format* 'double-float)) ...)` | `double-float` | **AOT のコンパイル時**。ホスト CL のリーダが `src/lisp` を読むときの型 |
| C | `src/c/runtime.h` `READ_DEFAULT_FLOAT_FORMAT_FALLBACK_IS_SINGLE` = 0 | double 相当 | 動的変数が読めないとき(ブート最初期・unbound・想定外の値・C のユニットテスト) |

**単一の真実源ではない。** A は Lisp のフォーム、B はホスト CL の特殊変数、
C は C のマクロで、互いを参照していない。

### 食い違っても何も検出しない

B が A と違っていても、**`src/lisp` の AOT 対象に接尾辞なしの float リテラルが
1つも無いので、現状では観測されない**(PR #81 §4-3 で確認済み)。
つまり今は「たまたま踏んでいない」だけである。

C が A と違うと、`init.lisp` の `defdynamic` を評価するまでの読み取りが別の型になる。
これは意図的な設計(ブート初期に落ちないことを優先)だが、**A を変えるときに
C も合わせるべきかどうかは別途判断が要る。**

### 切り替えの手順への示唆

既定を `<single-float>` へ倒す PR では、

1. **A・B・C の3つを同時に変える**(どれか1つでも残すと経路によって型が変わる)
2. できれば**単一の真実源にする**。B はホスト CL 上で動くので runtime.h も
   init.lisp も直接は読めないが、`transpile.lisp` が `init.lisp` の
   `defdynamic` 行を読んで決める、あるいは両者が読む小さな設定ファイルを置く、
   といった形は取れる
3. **食い違いを検出する仕掛けを1つ入れる。** 例えば AOT 対象のどこかに
   接尾辞なしの float リテラルを1つ置き、その型が実行時の
   `(dynamic *read-default-float-format*)` と一致することを実機テストで見る。
   今は観測手段が無いので、静かにずれたままになる
