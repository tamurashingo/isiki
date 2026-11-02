# 演算の型昇格: 数学関数 (c-2)

> 実施日: 2026-09-19 / ブランチ: `feature/float-math-contagion`(分岐元 `feature/compiler-optimization` `5292ad3`)
> 関連: `documents/float-contagion.md`(c-1)、`documents/single-float.md`(PR #79)
>
> 本作業の範囲は**数学関数が入力の型を保つこと**と、その前提になる
> **ライブラリ内の float リテラルの掃き出し**・**タグ 0x4 の分岐漏れの確認**。
>
> **`*read-default-float-format*` の既定を `<single-float>` へ戻す判断は含めない**(§7)。

---

## 0. 先に結論

1. **判定は c-1 の `args_float_kind` を再利用するだけで済んだ。**
   「float が絡めば kind の最大値、整数だけなら `*read-default-float-format*`」を
   `math_result_kind` 1関数にまとめ、各プリミティブは
   `os_make_float(...)` を `os_make_float_of_kind(math_result_kind(args), ...)` へ
   置き換えただけである。二項の `atan2` も同じ関数で正しくなる。
2. **Lisp 側で型を落としていたのは float リテラル3箇所だけだった。**
   `%expt-integer` の単位元・`expt` の零元・`atanh` の係数。
   比較にしか使っていないリテラル(`(<= -1.0 x 1.0)` 等)は直していない(§3)。
3. **AOT トランスパイラは float リテラルを1つも扱えなかった。**
   `transpile-expr` に `floatp` の分岐が無く、**float リテラルを含むファイルは
   AOT 化そのものが失敗する**(「未対応の式です」)。タグ 0x4 に限った話ではなく
   double も同じだった。`src/lisp` の AOT 対象に float リテラルが1つも無かったので
   踏まれていなかっただけである(§4-3)。
4. **ホスト CL と isiki-os で `*read-default-float-format*` の既定が逆だった。**
   CL は single、isiki-os は double。揃えないと接尾辞なしの `1.5` が
   「AOT では single、インタプリタでは double」に分かれる(§4-3)。
5. **タグ 0x4 の分岐漏れは他に無かった。** `os_eval` が許可リストではなく
   「SYMBOL と CONS 以外は自己評価」という形なので、新しい即値タグは自動的に
   正しく扱われる(§4-4)。

---

## 1. 型規則

### 1-1 数学関数

| 入力 | 出力 |
|---|---|
| single | **single** |
| double | double |
| 整数 | **`*read-default-float-format*` に従う** |

実装は `math_result_kind`(runtime.c)1つに集約した。

```c
static int math_result_kind(lisp_val_t args) {
    int kind = args_float_kind(args);              /* c-1 の型昇格規則 */
    if (kind != FLOAT_KIND_NONE) return kind;
    return os_read_default_float_format_is_single() ? FLOAT_KIND_SINGLE
                                                    : FLOAT_KIND_DOUBLE;
}
```

- **float が絡めば c-1 の規則そのまま。** `args_float_kind` は引数リストを
  丸ごと見るので、二項の `atan2` も新しい判定を書かずに正しくなる
- **整数だけなら既定に従う。** CommonLisp は有理数入力に single を返すと
  定めているが、isiki-os ではこの変数を型の中心に据えているのでそちらへ揃える。
  こうするとリテラルと数学関数の結果が同じ型になり、
  `(approx= 1.4142135623730951 (sqrt 2))` のような比較が**既定をどちらにしても成立する**
- **フォールバックは `os_read_default_float_format_is_single` が持っている**
  (ブート初期・unbound・想定外の値。`documents/single-float.md` §2-3)。
  c-2 で別に用意はしていない

### 1-2 内部計算は double、最後に丸める

**`sqrtf` / `sinf` などは使わない。** FPU(x87 / libm)で double のまま計算し、
結果を `os_make_float_of_kind` で目的の型へ落とす。

二重丸めが生じるが、リーダの精度仕様(保証しない、`documents/single-float.md`)と
同じ扱いで説明が付く。むしろ double で計算してから丸めるほうが、
真の単精度計算より結果が良くなる場合が多い。

この性質はテストで固定してある(`float_math_contagion_test.lisp` §6):

```lisp
(assert-equal t (= (sqrt 2.0f0) (* (sqrt 2.0d0) 1.0f0)))
```

`(* x 1.0f0)` は「single へ寄せる」操作(値は変えない)。
内部で単精度計算に切り替わっていたらこれが成り立たない。

### 1-3 `expt`

| 式 | 結果 |
|---|---|
| `(expt 2 10)` | `1024`、`<FIXNUM>`(整数のまま) |
| `(expt 2 100)` | `<BIGNUM>` |
| `(expt 2.0f0 10)` | `<SINGLE-FLOAT>` |
| `(expt 2.0f0 0)` | **`1.0f0`**(単位元が base の型) |
| `(expt 2 0.5f0)` | `<SINGLE-FLOAT>`(指数が float なので float) |
| `(expt 0 1.5f0)` | `<SINGLE-FLOAT>` の 0 |

**整数のべき乗は整数のまま。** 従来どおり。

---

## 2. 整数を返す関数(対象外)

`floor` / `ceiling` / `round` / `truncate` / `isqrt` は**整数を返す**ので
出力の型昇格は起きない。入力が single でも double でも整数を返すことを
テストで固定した(§5-5)。

`sqrt` は完全平方数なら整数を返す(`(sqrt 4)` → `2`)。これも従来どおり。

**`div` / `mod` は ISLisp 上は整数演算**である(`floor_divmod` が
`decompose` を通る整数専用の実装)。指示書 §3-5 は「入力が float なら float を
返すはず」としているが、**ISLisp §19 の `div`/`mod` は整数を要求する**ので
現状のままにした。`rem` は ISLisp には無い(CommonLisp の関数)。

---

## 3. ライブラリ内の float リテラルの掃き出し

**接尾辞なしの float リテラルは double である**(`*read-default-float-format*`
の既定)。それを単位元・零元・種として使うと、型昇格が正しく働いていても
結果が double に引きずられる。

`grep -rnE "[0-9]\.[0-9]|[0-9][eE][-+]?[0-9]" src/lisp/` の全件を精査した。
`fat16.lisp` / `fat32.lisp` の `8.3` はすべて**識別子の一部**
(`%fat16-8.3-field-bytes` 等)で、float リテラルではない。

実際の float リテラルは `init.lisp` に 6 箇所、`mandelbrot.lisp` に 8 箇所。

| 箇所 | 用途 | 判断 |
|---|---|---|
| `init.lisp` `%expt-integer` の `1.0` | **単位元** | **直した**。`(%float-one-like base)` へ |
| `init.lisp` `expt` の `(if (floatp x2) 0.0 0)` | **零元** | **直した**。`(%float-zero-like x2)` へ |
| `init.lisp` `atanh` の `(* 0.5 ...)` | **係数** | **直した**。整数 2 での除算へ |
| `init.lisp` `expt` の `(= x2 0.0)` | 比較のみ | 直さない。比較は広いほうへ揃うので型を汚さない |
| `init.lisp` `%check-unit-range` の `(<= -1.0 x 1.0)` | 比較のみ | 直さない。同上。返すのは `x` 自身 |
| `init.lisp` `%quotient2` の `(/ (float dividend) (float divisor))` | **変換呼び出し** | **直した**(§3-2)。リテラルではないので最初の掃き出しで見落とした |
| `init.lisp` `*pi*` の `3.141592653589793d0` | 定数 | 直さない。**明示的に double**(PR #79 で接尾辞を付けた) |
| `mandelbrot.lisp` の 8 箇所 | デモの座標・閾値 | 直さない。ライブラリではなくデモで、全部 double のまま完結している |

### 3-2 リテラルだけでは足りなかった — `(float x)` の呼び出し

最初の掃き出しは**float リテラル**だけを探したので、`%quotient2` を見落とした。

```lisp
;; 修正前
(if (and (or (fixnump dividend) (bignump dividend))
         (or (fixnump divisor) (bignump divisor))
         (= (mod dividend divisor) 0))
    (div dividend divisor)
    (/ (float dividend) (float divisor)))    ; ← ここ
```

**`(float x)` は整数を `*read-default-float-format*` に従って変換する。**
`(quotient 1 2.0f0)` のように片方だけが float のとき、整数側の `1` が
double になって結果を double へ引きずる。`(reciprocal 2.0f0)` と
`(expt 2.0f0 -1)`(reciprocal 経由)が double を返して実機テストで落ちた。

**float が絡むときは `/` にそのまま任せればよい。** `/` は c-1 で
整数 × single → single になっている。`float` への変換が要るのは
「両方整数で割り切れない」ときだけである。

```lisp
;; 修正後
(if (and (or (fixnump dividend) (bignump dividend))
         (or (fixnump divisor) (bignump divisor)))
    (if (= (mod dividend divisor) 0)
        (div dividend divisor)
        (/ (float dividend) (float divisor)))   ; 両方整数で割り切れない
    (/ dividend divisor))                        ; どちらかが float → / に任せる
```

`expt` の非整数指数の枝も同じ形だった(下記)。**掃き出しはリテラルだけでなく
`(float ...)` の呼び出しも対象にすること。** `src/lisp` の `(float ` は
全3箇所で、残る1つは `convert` の `((integerp obj) (float obj))` —
これは「整数を float に変換する」ことそのものが目的なので既定に従ってよい。

### 3-1 追加したヘルパー

```lisp
(defun %float-one-like (sample)  (if (%%single-float-p sample) 1.0f0 1.0d0))
(defun %float-zero-like (sample) (if (%%single-float-p sample) 0.0f0 0.0d0))
(defun %widen-float-like (x sample) (* x (%float-one-like sample)))
```

`%widen-float-like` は「x を sample と同じ(またはそれより広い)float 形式へ寄せる」。
1.0 倍は値を変えない(無限大・NaN も素通しする)ので、型だけを動かす手段として使える。
x が double で sample が single なら、型昇格の規則どおり double のままになる。

`expt` の非整数指数の枝で使っている。

```lisp
;; 以前: (exp (* (float x2) (log (float x1))))
(t (exp (* x2 (log (%widen-float-like x1 x2)))))
```

`(float x1)` だと整数の x1 が `*read-default-float-format*` に従ってしまい、
既定が double のとき `(expt 2 0.5f0)` が double になる。
**log へ渡す前に x1 を x2 の型へ寄せる**のが要点である。

---

## 4. タグ 0x4 の分岐漏れの確認

c-1 で `za_classify_operand` の漏れが**ベンチを書いて初めて**見つかったので、
同じ形が他にないかを機械的に洗った。

### 4-1 タグで分岐している箇所の全件

| 箇所 | 0x4 の扱い | 判定 |
|---|---|---|
| `os_tag_is_heap_ref`(runtime.h) | 全16値を並べた `switch` で明示的に偽 | ✓ PR #78 |
| `os_tag_holds_address`(runtime.h) | bit0 のみ見る | ✓ |
| `gc_copy_value`(runtime.c:2415) | `!os_tag_is_heap_ref` で即 return | ✓ |
| GC の走査 `switch`(runtime.c:2482 / 2799) | 上で弾かれるので到達しない | ✓ |
| `print_value` の `switch`(print.c) | `case TAG_SINGLE_FLOAT` あり | ✓ PR #79 |
| `values_equal` の `switch`(runtime.c) | `case TAG_SINGLE_FLOAT` あり | ✓ PR #79 |
| `align_tag_name` / 境界監査のヒストグラム | 即値は `os_align_audit_note_value` が early return | ✓ |
| `is_number_result`(reader.c) | `TAG_SINGLE_FLOAT` を数として認める | ✓ PR #79 |
| `primitive_numberp1` | 同上 | ✓ PR #79 |
| `za_classify_operand` 他3箇所(za.c) | 即値として movabs 埋め込み | ✓ **PR #80 で修正** |
| **`transpile-expr`(transpile.lisp)** | **分岐が無く、error で落ちる** | **c-2 で修正**(§4-3) |
| `os_eval`(eval.c) | **許可リストではない**(§4-4) | ✓ |
| 列型の `switch`(runtime.c:8402 他) | float は元から型エラー | ✓ 対象外 |

### 4-2 `class-of` / 算術 / 比較

PR #79 と c-1 で対応済み。`float_kind_of` が単一の判断元になっている。

### 4-3 AOT トランスパイラ — **float リテラルを扱えなかった**

`transpile-expr`(transpile.lisp)の分岐は

```
integerp / stringp / null / (eq t) / characterp / scope内シンボル / quote / ...
(t (error "transpile-expr: 未対応の式です: ~S" expr))
```

で、**`floatp` の分岐が無い。** タグ 0x4 に限った話ではなく、
**double のリテラルも同じく AOT 化に失敗する。**
`src/lisp` の AOT 対象(`init_aot.lisp` / `utility.lisp` / `device.lisp` /
`ide.lisp` / `partition.lisp` / `mount.lisp` / `file-node.lisp` /
`fat16.lisp` / `fat32.lisp` / `file-cmd.lisp` / `bench_aot.lisp`)に
float リテラルが1つも無かったので、踏まれていなかった。

**失敗の仕方はビルド時のエラーで、静かな誤りではない。** そこは救いである。

#### 直した内容

1. **`c-float-literal` を追加**して `floatp` の分岐を作った。
   single は即値(`os_make_single_float(1.5f)`)、
   double はヒープ(`os_make_float(1.5)`)で生成する式が違う。

   **10進の往復で正確さを保つ。** CL のプリンタは「読み直すと同じ値になる最短の
   10進表記」を出し、C コンパイラは10進リテラルを正しく丸めるので、
   両者を通してもビットパターンは変わらない。
   `*read-default-float-format*` をその値自身の型へ束縛して印字することで、
   指数マーカーの付かない素の10進表記が得られる。

2. **`read-all-forms` でホスト CL の `*read-default-float-format*` を
   `double-float` へ束縛した。**

   **CL の既定は single-float、isiki-os の既定は double-float で逆である。**
   揃えないと接尾辞なしの `1.5` が「AOT では single、インタプリタでは double」に
   分かれて静かに食い違う。接尾辞付き(`1.5f0` / `1.5d0`)は
   どちらの側でも同じ型に読まれるので影響しない。

#### 確認

`test/lisp/transpile_fixture.lisp` に float リテラルを含む関数を6つ足し、
`test/c/lisp_compiled_test.c` の `test_transpile_fixture_float_literals` で
AOT 生成コードの戻り値を直接見ている。
single / double / 接尾辞なし / 負値 / 指数付き / 演算との組み合わせ。

生成された C は次のとおりで、意図どおりである。

```c
return (tco_result_t){.is_tail_call = 0, .value = (os_make_single_float(1.5f))};
return (tco_result_t){.is_tail_call = 0, .value = (os_make_float(1.5))};       /* 1.5d0 */
return (tco_result_t){.is_tail_call = 0, .value = (os_make_float(1.5))};       /* 接尾辞なし */
return (tco_result_t){.is_tail_call = 0, .value = (os_make_single_float(-0.25f))};
return (tco_result_t){.is_tail_call = 0, .value = (os_make_single_float(1.5e10f))};
```

##### テストを書くときに踏んだ罠(製品側の問題ではない)

最初、テスト側に `extern` 宣言を書き忘れた。C11 の暗黙の int 宣言になり、
**戻り値の上位32bitが落ちて 0x4 になった。**
タグ(下位4bit)だけは残るので、

```
os_is_single_float(sf)            → 真   (タグは合っている)
os_single_float_value(sf) == 1.5f → 偽   (値が 0.0)
```

という「型は合っているのに値が0」という形で出る。
生成された C も呼び出し側の機械語も正しかったので、
**逆アセンブルまで見て初めて宣言漏れだと分かった。**
このテストファイルは fixture 関数ごとに `extern` 宣言を書く約束になっている。

### 4-4 `os_eval` は許可リストではない

```c
lisp_val_t os_eval(lisp_val_t exp, lisp_val_t env) {
    if (exp == nil) return nil;
    UINT64 tag = exp & TAG_MASK;
    if (tag == TAG_SYMBOL) return os_get_variable(exp, env);
    if (tag == TAG_CONS)   { ... }
    return exp;              /* それ以外は自己評価 */
}
```

**「SYMBOL と CONS 以外は自己評価」という除外リスト型**なので、
新しい即値タグを足しても自動的に正しく動く。ここに漏れは無い。

---

## 5. 検証

### 5-1 追加したテスト

| 場所 | 件数 | 内容 |
|---|---:|---|
| `test/lisp/float_math_contagion_test.lisp` | 104 | 全関数の single / double / 整数、`*read-default-float-format*` の両方向、`expt` の全ケース、精度、整数を返す関数、`quotient`/`reciprocal` |
| `test/c/lisp_compiled_test.c` | 11 | AOT 経路の float リテラル(single / double / 接尾辞なし / 負値 / 指数付き / 演算との組み合わせ) |
| `test/lisp/transpile_fixture.lisp` | 6関数 | 上記の被験体 |

### 5-2 精度 — 直接比べる手段が無いので消去法で見た

**「double の結果を single へ落とした値」と直接比べることはできない。**
double を single へ**狭める**演算子が存在しないためである。

- 型昇格は広いほうへ寄せるので `(* double 1.0f0)` は double のまま
- `(float x)` は float をそのまま返す(型保存)
- 第2引数で形式を指定する拡張は本作業では扱っていない(§1、指示書 §3-6)

最初これを取り違えて `(= (sqrt 2.0f0) (* (sqrt 2.0d0) 1.0f0))` と書き、
実機で5件落とした。**`(* x 1.0f0)` は「single へ寄せる」であって
「single へ狭める」ではない。**

代わりに **single 経路と整数経路が同じ計算を通っていること**を見ている。

```lisp
(dynamic-let ((*read-default-float-format* '<single-float>))
  (= (sqrt 2.0f0) (sqrt 2)))
```

どちらも「double で計算してから結果の型へ丸める」実装なので一致するはず。
single 入力のときだけ `sqrtf`/`expf` のような単精度版へ切り替えていたら、
最終ビットが食い違ってここが落ちる。

加えて、指示書 §5-4 の「直接比べる手段が無ければ値を出力して目視で確認してもよい」
に従い、値を test-results.txt へ出している。

```
[FMATH] sqrt2 single=1.4142135f0 double=1.4142135623730951
        exp1  single=2.7182817f0 double=2.718281828459045
```

どちらも double の結果の**最近傍 single** になっている。

### 5-3 回帰

| 実行 | 結果 |
|---|---|
| `make test` | **8489 OK / 0 NG**(c-2 前は 8478。+11) |
| `make build`(実機 PE32+) | 成功 |
| `make test-qemu-float-contagion-bench`(c-1 の回帰) | **15 passed / 0 failed**。`bytes/call: single=0 double=32 mixed=32 fixnum=0` で c-1 と同じ |
| `make test-qemu`(256M) | **3946 passed / 0 failed**(c-2 前は 3834) |
| `QEMU_MEM=96M make test-qemu` | **3946 passed / 0 failed** |

`%approx=` 系 14 件は既定が double のままなので通り続けている。

#### 1周目に落ちた12件の内訳

| 群 | 件数 | 内容 |
|---|---:|---|
| A | 5 | c-1 のテストが「c-2 で変わったら更新する」と書いていたマーカー。予定どおり |
| B | 5 | **テスト側の誤り**(§5-2 の「狭める演算子は無い」) |
| C | 2 | **本物のバグ**。`%quotient2` の `(float ...)`(§3-2) |

**C は実機テストでしか出なかった。** `make test` は通っていた
(`quotient`/`reciprocal` は Lisp 側の定義なので、C のユニットテストには乗らない)。

---

## 6. 性能

**本作業の主目的は性能ではない。** 数学関数は算術ほど頻繁には呼ばれない。

single の結果がヒープを確保しないことは c-1 と同じ理屈で成り立つ
(`os_make_float_of_kind(FLOAT_KIND_SINGLE, ...)` は即値を作るだけ)。

---

## 7. 次の判断: 既定を single へ戻すか

**本作業には含めない。** c-2 完了後に別途判断する。

PR #79 で既定を `<double-float>` にしたのは、`%approx=` 系 14 件が
「`(sqrt 2)` が double を返すのに対し、single リテラルの精度が足りない」
ために落ちたからだった(`documents/single-float.md` §2-3a)。

**c-2 でその前提は解消した。** 数学関数が `*read-default-float-format*` に
従うので、既定を single にすれば `(sqrt 2)` も single を返し、
single リテラルとの比較が成立する。

切り替え前に確認が要るもの:

| 項目 | 状態 |
|---|---|
| 整数 × single → single(`(assert-equal 15.0 (+ 12 3.0))` 系) | **c-1 で対応済み** |
| 数学関数が既定に従う | **c-2 で対応済み** |
| AOT トランスパイラが single リテラルを扱える | **c-2 で対応済み**(§4-3) |
| 既存テストの数値精度が single で足りるか | **未確認**。切り替え時に実測が要る |

**切り替えは `init.lisp` の1行だが、影響はテスト全体に及ぶ。**
独立した PR として、回帰の切り分けができる形で行うこと。
