# 演算の型昇格: 算術と比較 (c-1)

> 実施日: 2026-09-19 / ブランチ: `feature/float-contagion`(分岐元 `feature/compiler-optimization` `eba48a7`)
> 関連: `documents/single-float.md`(PR #79)、`documents/type-system-survey.md` §1-4、
> `documents/fixnum-signed-fastpath.md`(改善A)、`documents/bench-pinned-nil.md`(計測の作法)
>
> 本作業の範囲は**算術(`+` `-` `*` `/`)と比較(`<` `>` `<=` `>=` `=` `/=`)と
> 選択(`min` `max`)**。**数学関数は c-2**(§7)。

---

## 0. 先に結論

1. **型昇格は「kind の最大値」1つで書ける。** 整数 = 0、single = 1、double = 2 と
   すると、オペランドの最大値がそのまま結果の型になる。整数は 0 なので何も
   要求せず、float 側の形式に従う — これが「整数は float に従う」の実装である。
2. **比較はもともと正しかった。** `number_compare` が `to_double` で両方を
   double へ広げていたので、`(= 0.1f0 0.1d0)` は最初から偽だった。
   **c-1 で変更した箇所は無い。** テストで固定しただけである。
3. **型を直すだけでは確保は消えない。** `primitive_add2` は float を見ると
   cons を2個作って n 引数版へ委譲していた。**ここを直して初めて
   single × single の確保がゼロになる。** 改善Aで見つけた
   「32byte/回 の正体は bignum ではなく委譲用の cons だった」と同じ構図。
4. **JIT は「算術の高速路は変更不要、リテラルの扱いは要修正」だった。**
   指示書 §4-3 の想定は半分だけ当たっていた。`za_classify_operand` に
   タグ 0x4 の分岐が無く、**single-float リテラルを含む defun は
   まるごと JIT コンパイルを諦めていた**。C 側の確保をゼロにしても
   そこへ到達しないので、実測では single のほうが double より遅く、確保も多かった
   (128 byte/回 対 32 byte/回)。**ベンチを書いて初めて分かった**(§4-3b)。
5. **n 引数は左畳み込み + 毎ステップ丸め**にした。こうしないと
   `(+ a b c)` と `(+ (+ a b) c)` が食い違う(§3-2)。

---

## 1. 型昇格の規則(実装した表)

| 左 × 右 | 結果 |
|---|---|
| fixnum × fixnum | fixnum(超えたら bignum。従来どおり) |
| bignum × fixnum | 整数(従来どおり) |
| **fixnum × single** | **single** |
| **bignum × single** | **single** |
| **single × single** | **single** |
| fixnum × double | double |
| bignum × double | double |
| **single × double** | **double** |
| double × double | double |

実装は `src/c/runtime.c` の次の4つに集約した。

```c
#define FLOAT_KIND_NONE   0   /* 整数(何も要求しない) */
#define FLOAT_KIND_SINGLE 1
#define FLOAT_KIND_DOUBLE 2

static int  float_kind_of(lisp_val_t val);              /* 1値の種別 */
static int  float_kind_max(int a, int b);               /* 型昇格の規則そのもの */
static double float_round_to_kind(int kind, double v);  /* single なら単精度へ丸める */
static lisp_val_t os_make_float_of_kind(int kind, double v);  /* single はヒープを使わない */
```

`is_float(v)` は `float_kind_of(v) != FLOAT_KIND_NONE` になった。
`any_float(args)` は **`args_float_kind(args)` に置き換えて削除**した —
「float が含まれるか」と「結果の型」を別々に求めると必ずずれるので、
1回の走査で両方を出す形にした。

bignum × single で精度が落ちる(single の有効桁は約7桁)のは仕様どおりである。

---

## 2. 比較

**広いほうへ揃えてから比較する。狭いほうへ落とさない。**

```lisp
(= 1.5f0 1.5d0)     ; => t    1.5 は両形式で正確
(= 0.1f0 0.1d0)     ; => nil  single の 0.1 と double の 0.1 は数値として違う
(> 0.1f0 0.1d0)     ; => t    single の 0.1 を double へ広げると大きい
```

`number_compare`(runtime.c)は

```c
if (is_float(a) || is_float(b)) {
    double da = to_double(a);
    double db = to_double(b);
    ...
}
```

で、`to_double` は single を `(double)os_single_float_value(v)` へ広げる
(PR #79 で入れた)。**したがって最初から正しく、c-1 で変更した箇所は無い。**

`<` `>` `<=` `>=` `=` `/=` `min` `max` はすべて `number_compare` を通るので、
まとめて正しい。`min`/`max` は**引数そのものを返す**(ISLisp §19 の
「引数のうち最大/最小のもの」)ので型昇格の対象ではない。

```lisp
(class-of (max 1 2.0f0))   ; => <SINGLE-FLOAT>  (2.0f0 がそのまま返る)
(class-of (max 2 1.0f0))   ; => <FIXNUM>        (2 がそのまま返る)
```

**`1.5` は両形式で正確に表せるので、実装が誤っていても `(= 1.5f0 1.5d0)` は通る。**
テストには `0.1` 系を必ず入れてある(`test/lisp/float_contagion_test.lisp` §7、
`test_float_comparison_widens_to_double`)。

---

## 3. n 引数版の畳み込み順序

### 3-1 結果の型

**全オペランドの最も広い形式**(= kind の最大値)。左畳み込みの結果と一致する。

```lisp
(class-of (+ 1.5f0 1.5f0 1.5f0))   ; => <SINGLE-FLOAT>
(class-of (+ 1.5f0 1.5d0 1.5f0))   ; => <DOUBLE-FLOAT>
(class-of (+ 1 2 3 1.5f0))         ; => <SINGLE-FLOAT>
```

### 3-2 値 — 毎ステップ丸める

計算は double で行うが、**そこまでに見た形式へ毎回丸める**。

```c
double sum = 0.0;
int kind = FLOAT_KIND_NONE;
for (cur = args; cur != nil; cur = cc_cdr(cur)) {
    lisp_val_t v = cc_car(cur);
    kind = float_kind_max(kind, float_kind_of(v));
    sum = float_round_to_kind(kind, sum + to_double(v));
}
return os_make_float_of_kind(kind, sum);
```

**丸めを最後の1回だけにする案は採らなかった。** そのほうが精度は上がるが、
`(+ a b c)` と `(+ (+ a b) c)` の答えが食い違う。n 引数版と2引数版の合成が
一致しないのは、リファクタリングで結果が変わるということで、害のほうが大きい。

毎ステップ丸めにすると両者は**型も値も一致する**。テストで固定してある
(`float_contagion_test.lisp` §6、`0.1f0` を使って丸め回数まで見ている)。

`-` と `/` は第一引数の kind から始める(単項マイナスは第一引数の型をそのまま保つ)。

---

## 4. 調査 (指示書 4 章)

### 4-1 float が絡む演算の全数

`grep -n "any_float\|is_float(\|os_make_float\|to_double\|number_compare" src/c/runtime.c`
で洗い出した全件。

| 関数 | 現在の float 扱い | c-1 | 対応 |
|---|---|---|---|
| `primitive_add` | `any_float` → 全部 double | ○ | **kind 畳み込みへ** |
| `primitive_add2` | fixnum 高速路以外は cons 2個で委譲 | ○ | **float 高速路を追加(確保ゼロ)** |
| `primitive_subtract` | 同上 | ○ | 同上 |
| `primitive_subtract2` | 同上 | ○ | 同上 |
| `primitive_multiply` | 同上 | ○ | 同上 |
| `primitive_multiply2` | 同上 | ○ | 同上 |
| `primitive_divide` | 同上 | ○ | 同上 |
| `number_compare` | 両方 double へ広げて比較 | ○ | **変更不要**(既に正しい) |
| `<` `>` `<=` `>=` `=` `/=`(と `*2` 版) | `number_compare` 経由 | ○ | 変更不要 |
| `max` / `min` | `number_compare`、引数をそのまま返す | ○ | 変更不要(§2) |
| `abs` | PR #79 で型保存済み | ○ | 変更不要 |
| `quotient` / `reciprocal`(Lisp) | `(/ (float a) (float b))` | ○ | **変更不要**。`float` は float をそのまま返すので `/` の昇格が効く |
| `floor` `ceiling` `truncate` `round` | **整数を返す** | — | **対象外**。結果が整数なので float の型昇格は関係しない |
| `sqrt` | `os_make_float` 固定 | × | **c-2** |
| `log` | 同上 | × | c-2 |
| `exp` | 同上 | × | c-2 |
| `sin` / `cos` | 同上 | × | c-2 |
| `atan2` | 同上 | × | c-2 |
| `float`(FLOAT関数) | 整数→ `os_make_float`(double) | × | **c-2**。「整数→float はどちらの型か」は `(sqrt 2)` と同じ未決の問い |
| `expt`(Lisp) | 整数指数でも double になる | × | **c-2**。下の注を参照 |
| `%%most-positive-single-float` 他4つ | 定数 | — | 対象外 |
| `os_make_float` / `os_make_single_float` | 構築関数 | — | 対象外 |

#### c-2 へ回した判断

- **数学関数(`sqrt`/`log`/`exp`/`sin`/`cos`/`atan2`)**: 指示書 §1 が明示的に
  c-2 としている。入力の型を保つ実装(内部 double 計算 → 最後に single へ丸め)は
  c-1 の畳み込みとは別の話なので、まとめない。
- **`float`(FLOAT関数)**: 整数を渡したとき何を返すかが
  `*read-default-float-format*` と絡む未決の設計である。c-2 で決める。
- **`expt`**: 指示書 §1 が c-2 としている。ただし**理由は素直ではない**ので
  記録しておく。`expt` は整数指数なら `%expt-integer`(`*` の繰り返し二乗法)を
  使うので、`*` の型昇格がそのまま効きそうに見える。実際には

  ```lisp
  (defun %expt-integer (base power)
    (if (= power 0)
        (if (floatp base) 1.0 1)   ; ← この 1.0 が double リテラル
        ...))
  ```

  単位元の `1.0` が double なので、`(expt 1.5f0 2)` は途中で
  `(* 1.5f0 1.0d0)` を通って **double になる**。
  直すには単位元を base の型に合わせる必要があり、それは「型を保つ」という
  c-2 の仕事そのものなので c-2 へ回した。現状は
  `float_contagion_test.lisp` §13 で `<DOUBLE-FLOAT>` として固定してある。

### 4-2 2 引数版と n 引数版

`primitive_add` / `primitive_add2` のように、**2引数の高速路と n引数の一般路が
別々にある**。`+` `-` `*` に `*2` 版があり、`/` には無い。

**両方に型昇格を入れた。** 片方だけだと引数の個数で結果の型が変わる。
`float_contagion_test.lisp` §6 が2引数版と n引数版の一致を見ている。

`*2` 版に float 高速路を足したのは型のためだけではない。**cons を作らずに
済ませるため**である(§5)。

### 4-3 JIT への影響 — **算術の高速路は変更不要。リテラルの扱いは要修正だった**

指示書 §4-3 は「JIT の変更は不要なはず。そう確認したうえで進めること」と言っている。
確認した結果は**半分だけ当たり**だった。

- **算術のインライン高速路 → 本当に変更不要**(下記)
- **リテラルの分類 → 変更が必要だった**(§4-3b)。これは §4-3 が見ていなかった箇所で、
  実測(ベンチが `%%za-compiled-p` で落ちた)で見つかった

#### 算術の高速路(変更不要)

`za_emit_arith_call_or_inline`(`src/c/za.c:2658`)がインライン展開するのは
`primitive_add2` と `primitive_subtract2` だけで、その先頭は

```c
jit_mov_reg_reg(ZA_REG_R10, ZA_REG_RCX);
jit_or_reg_reg(ZA_REG_R10, ZA_REG_RDX);
jit_movabs_reg(ZA_REG_R9, TAG_MASK | FIXNUM_SIGN_BIT);
jit_test_reg_reg(ZA_REG_R10, ZA_REG_R9);
/* jne → fallback (wrapper_fn を間接 call) */
```

**single-float はタグ 0x4 なので `TAG_MASK` のビットが立ち、必ず `jne` で
fallback する。** fallback 先は `primitive_add2` / `primitive_subtract2` 自身で、
そこに今回の float 高速路が入っている。`*` と `/` はそもそもインライン化されず
常に wrapper を呼ぶ。

**したがって JIT が出す算術の機械語は1バイトも変える必要がない。**
PR #79 の記録を引くだけでなく、この箇所を実際に読んで確認した。

#### 4-3b リテラルの分類 — **ここは直す必要があった**

`za_classify_operand`(za.c)は裸のリテラルを次のように分類していた。

| タグ | 扱い |
|---|---|
| `TAG_FIXNUM` | `is_literal=1`。movabs で機械語に直接埋め込む |
| `TAG_CHAR` | `is_literal=1`。同上 |
| `TAG_INSTANCE`(float/bignum) | `is_literal=7`。GCルート付きスロット経由 |
| **`TAG_SINGLE_FLOAT`** | **どの分岐にも該当せず `return 0`** |

`za_classify_operand` が 0 を返すと、**その defun はまるごと JIT コンパイルを
諦める。** つまり **single-float リテラルを1つでも含む関数はインタプリタで動く。**

ベンチを書いて初めて気づいた。`%%za-compiled-p` が偽になり、確保量が
「ゼロになるはず」のところで 128 byte/回 出た。

```
[FCBENCH] single: n=2000 ticks=132 heap-delta=257856 bytes/call=128
[FCBENCH] double: n=2000 ticks=0   heap-delta=64048  bytes/call=32
[FCBENCH] fixnum: n=2000 ticks=0   heap-delta=48     bytes/call=0
[NG] (%%ZA-COMPILED-P (FUNCTION FCB-LOOP-SINGLE)) => NIL (expected T)
```

**single のほうが double より確保が多く、13倍遅かった。**
インタプリタは引数ごとに cons を作るので当然である。
C 側で `primitive_add2` の確保をゼロにしても、そこへ到達しなければ意味がない。

直し方は自明で、**single-float も即値なので fixnum/char と同じ `is_literal=1`
(movabs で埋め込む)にする**。GC が動かさないのでスロットもルート登録も要らず、
`os_tag_is_heap_ref(0x4)` が偽なので defun 末尾の焼き込み監査もそのまま通る。
double が `is_literal=7`(スロット経由)なのは `TAG_INSTANCE` で GC が動かすから
であって、single には当てはまらない。

直した箇所は3つ。

| 箇所 | 内容 |
|---|---|
| `za_classify_quoted_value` | `(quote 1.5f0)` を即値として扱う |
| `za_classify_operand` | 裸の `1.5f0` を即値として扱う(**これが本丸**) |
| `za_operand_is_safe_leaf` | movabs 1命令で済むので leaf に含める(GCルートの link/unlink を省ける) |

**この修正が無いと c-1 の性能上の主張が成り立たない。**
「型は正しいが確保は減らない」で終わっていた。

### 4-4 改善A の fixnum 高速路との関係

`fixnum_add_signed`(runtime.c)の先頭は

```c
if ((a & TAG_MASK) != TAG_FIXNUM || (b & TAG_MASK) != TAG_FIXNUM) { return 0; }
```

なので、float は必ずここで落ちる。**fixnum どうしの経路には一切触れていない。**
float 高速路は fixnum 高速路の**後ろ**に置いてあり、順序も変えていない。
`float_contagion_test.lisp` §12 が整数どうしの結果を固定している。

---

## 5. single 結果はヒープを確保しない

**c-1 の性能上の眼目。**

型昇格を `primitive_add` 側だけに入れても足りない。`primitive_add2` は

```c
lisp_val_t primitive_add2(lisp_val_t a, lisp_val_t b) {
    if (fixnum_add_signed(a, b, &sum)) return sum;
    GC_PROTECT(a); GC_PROTECT(b);
    lisp_val_t args = os_make_cons(a, os_make_cons(b, nil));   /* ← cons 2個 */
    return primitive_add(args, global_environment);
}
```

という形で、float を渡すと**委譲用の cons を2個作ってから**n引数版へ行く。
型は正しくなっても確保は残る。

これは改善A で見つけた構図とまったく同じである。あのとき「32byte/回」の正体は
bignum ではなく `primitive_add2` が一般路へ委譲するために作る cons 2個だった
(`documents/fixnum-signed-fastpath.md`)。

そこで `*2` 版に float 高速路を入れた。

```c
int kind = float_kind_max(float_kind_of(a), float_kind_of(b));
if (kind != FLOAT_KIND_NONE) {
    return os_make_float_of_kind(kind, to_double(a) + to_double(b));
}
```

`os_make_float_of_kind(FLOAT_KIND_SINGLE, x)` は `os_make_single_float((float)x)` で、
**タグ 0x4 の即値を作るだけなのでヒープに触らない。**

### 確認のしかた

「型が変わった」ではなく「**確保が消えた**」を直接見る。
`primitive_heap_used_bytes` はバンプポインタ `g_from_ptr - g_from_start` を
そのまま返すので、1byte の増加も見える。

- C 側: `test_single_float_arithmetic_allocates_nothing`(runtime_test.c)
  — 3000演算まわして `after == before` を確認。対照として double 版が
  増えることも同時に見る
- 実機: `test/lisp/float_contagion_bench.lisp` — `%%heap-used-bytes` の差分を
  **回帰アサーションにしてある**(`(assert-equal 0 (fcb-heap-delta ...))`)。
  観測ログではなくテストなので、cons が戻ってきたら落ちる

---

## 6. 計測

`make test-qemu-float-contagion-bench`(`test/lisp/float_contagion_bench.lisp`)。
**同一ブート内**で single / double / mixed / fixnum を2往復する。
4シナリオは**ループの形も回数も初期値の大きさも同じ**で、変えるのは
加算する定数の型だけにしてある(改善Aで初期値の不揃いが誤った結論を生んだ教訓、
`documents/type-system-survey.md` §7-4)。

`%%za-compiled-p` で4本とも JIT に乗っていることを確認してから測っている。

### 6-1 1回あたりのヒープ確保

固定オーバーヘッド(`funcall` の引数 cons 等で48byte)を差し引くため、
n を 500 → 1500 に変えた**差分**から出している。

```
[FCBENCH] bytes/call: single=0 double=32 mixed=32 fixnum=0
```

| シナリオ | bytes/call |
|---|---:|
| **single × single** | **0** |
| double × double | 32 |
| mixed(single累算器 + double) | 32 |
| fixnum(対照) | 0 |

**single は fixnum と同じくヒープを1byteも使わない。**
double の 32byte は `os_make_instance` の1オブジェクト分そのものである。

mixed が 32 なのは型昇格が効いている裏取りになる(1回目の加算で累算器が
double になり、以降は double のコストを払う)。

これは観測ログではなく**回帰アサーション**にしてある
(`(assert-equal 0 *fcb-bpc-single*)` / `(assert-equal 32 *fcb-bpc-double*)`)。
委譲用の cons が戻ってきたら落ちる。

### 6-2 速度

tick は 100Hz(1tick = 10ms)。n=200000、同一ブート内で2往復したレンジ。

```
[FCBENCH] ticks range: single=40..40 double=64..64 mixed=64..68 fixnum=12..12
```

| シナリオ | ticks(レンジ) | ms | ループ分(fixnum)を引いた値 |
|---|---|---:|---:|
| fixnum(対照) | 12..12 | 120 | — |
| **single** | **40..40** | **400** | 28 |
| double | 64..64 | 640 | 52 |
| mixed | 64..68 | 640..680 | 52..56 |

**single と double のレンジは重なっていない**(40..40 と 64..64)ので、
差が出たと言ってよい。全体で **1.6倍**、ループ自体のコストを引いた
float 演算部分では **約1.9倍**速い。

gc-delta はすべて 0(double の 6.4MB でも発火しなかった)なので、
この差は GC ではなく**確保そのもののコスト**である。

### 6-3 JIT リテラル修正の効果(§4-3b)

修正前後で single の数字がどう変わったかを記録しておく。

| | 修正前(JIT に乗らない) | 修正後(JIT に乗る) |
|---|---|---|
| `%%za-compiled-p` | **nil** | t |
| bytes/call | **128** | **0** |
| ticks | 132(n=**2000**) | 40(n=**200000**) |

**n が100倍違う点に注意。** 修正前は n=2000 で 1.32秒かかっていたので、
同じ仕事あたりに直すと**2桁以上**速くなっている。

修正前は **single のほうが double より確保が多く(128 対 32)、遅かった。**
C 側の `primitive_add2` をいくら直しても、インタプリタ経路を通っていては
意味が無いということである。

### 6-4 「速くなるはず」で終わらせない

Phase 3 で `null` がインライン化で逆に遅くなった例がある。今回は
**遅くなった項目は無い**:

- double: 修正前後で経路が変わらない(もともと JIT に乗っていた)
- fixnum: `fixnum_add_signed` の高速路は一切触っていない。12..12 ticks
- mixed: double 相当。期待どおり

### 6-5 回帰

| 実行 | 結果 |
|---|---|
| `make test` | **8478 OK / 0 NG**(c-1 前は 8457。+21) |
| `make build`(実機 PE32+) | 成功 |
| `make test-qemu-float-contagion-bench` | **15 passed / 0 failed** |
| `make test-qemu`(256M) | **3834 passed / 0 failed**(c-1 前は 3705。+129) |
| `QEMU_MEM=96M make test-qemu` | **3834 passed / 0 failed** |

JIT の焼き込み監査は `gc-heap=0`
(`#count dis-add kernel=17 immobilized=0 gc-heap=0` /
`#count dis-caller kernel=26 immobilized=1 gc-heap=0`)。
**single-float を即値として焼き込むようにしたあとも、GC ヒープ上のアドレスは
1つも入っていない。** `os_tag_is_heap_ref(0x4)` が偽なので当然ではあるが、
監査で実際に確認した。

`SMALL_HEAP_SIZE` は触っていない(組み込み関数を増やしていないため)。

#### PR #79 のアサーションを2件更新した

`test/lisp/single_float_test.lisp` §10 は

```lisp
;;; single 同士の演算結果は **double** になる。(c) が入ったら single へ変わる。
;;; ここを直すのは次の作業であり、このアサーションはそのとき更新する。
(assert-equal '<double-float> (%%class-name (class-of (+ 1.5f0 1.5f0))))
(assert-equal '<double-float> (%%class-name (class-of (* 2.0f0 2.0f0))))
```

と書いてあった既知の中間状態で、**今回がその「そのとき」である。**
`<single-float>` へ更新し、混在(`(+ 1.5f0 1.5d0)` → double)の確認を足した。

---

## 7. 次の作業: 数学関数 (c-2)

**数学関数は入力の型を保つ。**

| 入力 | 出力 |
|---|---|
| single | single |
| double | double |
| 整数 | **未決**(`*read-default-float-format*` に従う? 要決定) |

対象は §4-1 で c-2 に分類した `sqrt` / `log` / `exp` / `sin` / `cos` / `atan2` /
`float`(FLOAT関数)/ `expt`、およびそれらの上に乗っている Lisp 側の
`asin` / `acos` / `atan` / 双曲線関数。

実装は「内部計算を double で行い、最後に single へ丸める」でよい。
二重丸めは生じるが、リーダの精度仕様(保証しない、`documents/single-float.md`)と
同じ扱いで説明が付く。

### c-2 が持つ意味

**c-2 が入ると `*read-default-float-format*` を `<single-float>` へ戻せる
可能性が出る。**

PR #79 で既定を double にしたのは、`(sqrt 2)` が double を返すのに対して
single リテラルの精度が足りず `%approx=` 系のテスト14件が落ちたからだった
(`documents/single-float.md` §2-3a)。

数学関数が入力の型を保てば `(sqrt 2.0f0)` は single を返す。ただし
**整数入力のときに何を返すか**(`(sqrt 2)` の型)が別途決まらないと解決しない。
`float`(FLOAT関数)を c-2 に含めたのはこのためである。
