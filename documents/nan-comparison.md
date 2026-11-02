# NaN 比較を IEEE 754 に合わせる

対象: PR(`feature/nan-comparison`、base は `feature/compiler-optimization`)

関連: `documents/float-default.md` §4-1(既知の問題として記録されていたもの)

---

## 1. 何を直したか

`(= nan nan)` が **真** を返していた。IEEE 754 では NaN はどの値とも等しくなく、
**自分自身とも等しくない**。

```lisp
(= nan nan)    ; 旧: T   → 新: NIL
(< nan 1.0d0)  ; 旧: NIL → 新: NIL(変化なし)
(/= nan nan)   ; 旧: NIL → 新: T    ★ここだけ真
```

ISLisp 仕様にも CommonLisp 規格にも NaN の比較についての記述は無いが、
C / Rust / Python / JavaScript いずれも IEEE 754 に従って偽を返す。それに合わせた。

---

## 2. 原因: 三値では非順序を表現できない

旧 `number_compare` は `-1 / 0 / 1` の三値を返していた。float 経路はこうだった。

```c
return da < db ? -1 : (da > db ? 1 : 0);
```

**NaN では `<` も `>` も偽になるので、0(等しい)へ落ちる。**
IEEE 754 の比較には第四の状態 **非順序(unordered)** があり、三値では表せない。

---

## 3. 直し方: 旧名を消して、漏れをコンパイルエラーにする

**第四の状態を足すだけでは足りない。** 呼び出し側が

```c
if (number_compare(a, b) >= 0) { ... }
```

のように戻り値を直接比べていると、`UNORDERED` が「大きい」として通ってしまう。
**書き換え漏れが静かに間違った結果になる**という、最も避けたい形である。

そこで関数名を `number_compare` → `number_compare4` に変え、**旧名を消した**。
呼び出し側は必ず述語を通す。

```c
#define NUM_CMP_LESS      (-1)
#define NUM_CMP_EQUAL     0
#define NUM_CMP_GREATER   1
#define NUM_CMP_UNORDERED 2

static int num_lt(a, b) { return number_compare4(a, b) == NUM_CMP_LESS; }
static int num_gt(a, b) { return number_compare4(a, b) == NUM_CMP_GREATER; }
static int num_eq(a, b) { return number_compare4(a, b) == NUM_CMP_EQUAL; }
static int num_le(a, b) { int c = ...; return c == NUM_CMP_LESS  || c == NUM_CMP_EQUAL; }
static int num_ge(a, b) { int c = ...; return c == NUM_CMP_GREATER || c == NUM_CMP_EQUAL; }
/* /= は「等しくない」なので **非順序も含む** */
static int num_ne(a, b) { return number_compare4(a, b) != NUM_CMP_EQUAL; }
```

**ありえない状態を表現できなくする。** PR #83 で float の既定を単一の定数に
まとめたのと同じ原理である。

### 非順序の判定

```c
if (da < db)  { return NUM_CMP_LESS; }
if (da > db)  { return NUM_CMP_GREATER; }
if (da == db) { return NUM_CMP_EQUAL; }
return NUM_CMP_UNORDERED;   /* 三つとも偽になるのは NaN が絡むときだけ */
```

**無限大はここへ来ない。** 無限大は順序づけられるので `<` `>` `==` のどれかが真になる。

---

## 4. 呼び出し側(全 13 箇所)

| 呼び出し元 | 旧 | 新 | 非順序のとき |
|---|---|---|---|
| `primitive_less_than`(n項) | `>= 0` | `!num_lt` | 偽 |
| `primitive_less_than2` | `< 0` | `num_lt` | 偽 |
| `primitive_greater_than`(n項) | `<= 0` | `!num_gt` | 偽 |
| `primitive_greater_than2` | `> 0` | `num_gt` | 偽 |
| `primitive_num_equal`(n項) | `!= 0` | `!num_eq` | 偽 |
| `primitive_num_equal2` | `== 0` | `num_eq` | 偽 |
| **`primitive_num_not_equal`**(n項) | `== 0` | **`!num_ne`** | **真** |
| `primitive_greater_equal`(n項) | `< 0` | `!num_ge` | 偽 |
| `primitive_greater_equal2` | `>= 0` | `num_ge` | 偽 |
| `primitive_less_equal`(n項) | `> 0` | `!num_le` | 偽 |
| `primitive_less_equal2` | `<= 0` | `num_le` | 偽 |
| `primitive_max` | `> 0` | `num_gt` | 更新しない(§4-3) |
| `primitive_min` | `< 0` | `num_lt` | 更新しない(§4-3) |

### 4-2 `/=` は `=` の否定ではなく、独立実装だった

`primitive_num_not_equal` は `number_compare(...) == 0` で判定していた。
**非順序が 0 として返ると「等しい」に吸収され、`/=` が偽になる。**
ここは踏んでいた。`num_ne` を `!= NUM_CMP_EQUAL` と定義したことで、
非順序が「等しくない」に含まれて真になる。

### `/=` だけが比較の高速経路に乗っていない(実測)

**`primitive_num_not_equal2`(二項版)が存在しない。** 比較演算子で二項版が
欠けているのは `/=` だけである。

```
primitive_num_equal2 (=) / less_than2 (<) / greater_than2 (>)
less_equal2 (<=) / greater_equal2 (>=)     … すべて存在
primitive_num_not_equal2 (/=)              … **存在しない**
```

`za_compile_binary` は「`rcx=a, rdx=b` で直接 call できる二項版」を要求するため、
`/=` は専用経路を使えず `za_syms_t` にも登録されていない(`za.c` に `"/="` という
文字列は一度も現れない)。結果として **一般呼び出しへ落ちる**。

**関数そのものは JIT に乗る。** 乗らないのは `/=` の部分だけである。
実測(96M、n=500):

| | code-len | bytes/call |
|---|---:|---:|
| `<`(二項版あり) | 690 | **0** |
| `/=`(二項版なし) | 1425 | **32** |

`<` は cons を作らず直接 call するので確保ゼロ。`/=` は n-ary の
`primitive_num_not_equal` を呼ぶため **1 回あたり cons 2 個ぶん 32 byte を確保**し、
コードサイズも 2 倍になる。PR #80 で「比較は cons を作らず直接委譲する」と
改善した経路に、**`/=` だけ乗り損ねている**。

本作業では直さない(NaN の意味論とは別の話であり、二項版の新設は独立した変更)。
直すなら `primitive_num_not_equal2` を足して `za_syms_t` へ登録する。

**意味は「隣接ペアがすべて等しくない」**であって、CommonLisp の
「全要素が相異なる」ではない(`primitive_num_not_equal` のコメント。
本作業以前からの簡略化で、ここでは変えていない)。

```lisp
(/= 1 2 1)   ; => T   (CommonLisp なら NIL)
```

### 4-3 `max` / `min` は**順序依存**になった(直さない。記録のみ)

`num_gt` / `num_lt` が非順序で偽を返すため、**NaN は最良値を更新できない**。
結果として **引数の順序で答えが変わる**。

```lisp
(max nan 1.0d0)   ; => NAN    先頭が NaN。以降 num_gt が偽なので更新されない
(max 1.0d0 nan)   ; => 1.0d0  NaN 側が更新できないので無視される
```

**これは IEEE のどちらの流儀とも一致しない。**

| 規格 | NaN の扱い |
|---|---|
| IEEE 754-2008 `maxNum` | NaN を無視して非 NaN を返す |
| IEEE 754-2019 `maximum` | NaN を伝播する |
| **本実装** | **引数の順序で変わる** |

将来直すなら**どちらの流儀に寄せるかという選択**がある。本作業では
現在の挙動をテストで固定するにとどめた(`test/lisp/nan_comparison_test.lisp`)。

### 4-4 NaN と無限大の作り方

`convert_float_test.lisp` と同じ手段が使える(domain-error 未実装のための簡略化。
`primitive_divide` のコメント参照)。

```lisp
(/ 0.0d0 0.0d0)    ; => NAN  (double)
(/ 0.0f0 0.0f0)    ; => NAN  (single のまま。double へ昇格しない)
(/ 1.0d0 0.0d0)    ; => INF
(/ -1.0d0 0.0d0)   ; => -INF
```

single / double の両方で作れる。

### 4-5 既存テストへの影響

`(= nan nan)` が真であることに依存しているテストは**無かった**。
`convert_float_test.lisp` は印字(`~S` の結果が `"NAN"`)で判定しており、
比較には依存していない。そのため既存テストの修正は不要だった。

---

## 5. 副次的な効果: `(/= x x)` が NaN 判定の定石になる

修正後、**`(/= x x)` が真になるのは x が NaN のときだけ**である。

```lisp
(/= 1.0d0 1.0d0)     ; => NIL
(/= *inf-d* *inf-d*) ; => NIL
(/= nan nan)         ; => T
```

他言語と同じ定石なので、**専用の述語(`nan-p` 等)を足す必要は無い**。
`documents/float-default.md` §4-1 が「直すときは NaN 判定の述語も一緒に用意するのが筋」
と書いていたが、`/=` を正しくすればそれで足りる。

---

## 6. 検証

| 検証 | 結果 |
|---|---|
| `make test` | 8498 OK / 0 NG |
| `make test-qemu` 256M | 4201 passed / 0 failed |
| `QEMU_MEM=96M` | 4201 passed / 0 failed |

`test/lisp/nan_comparison_test.lisp` で固定した範囲:

- NaN が絡む `=` `<` `>` `<=` `>=` がすべて偽(**左右を入れ替えた両方向**)
- `/=` だけが真
- single / double / 混在、整数との混在
- **無限大は順序づけられる**(NaN と混同していないこと)
- 通常の値・整数・bignum・負数が一切変わっていないこと
- 多引数の `=` `<` `/=`
- `max` / `min` の順序依存(§4-3)

---

## 7. 次の作業への引き継ぎ

`feature/single-float-arith` で比較を `ucomiss` でインライン化する。
**C 側が正しくなったので、インライン側はそれに一致させればよい。**

`ucomiss` のフラグ:

| 状態 | ZF | PF | CF |
|---|---|---|---|
| 非順序 | 1 | 1 | 1 |
| より大 | 0 | 0 | 0 |
| より小 | 0 | 0 | 1 |
| 等しい | 1 | 0 | 0 |

**非順序は「より小」とも「等しい」ともフラグで区別できない。**
`jb` で `<` を出すと NaN で真になり誤る。`je` で `=` も同じ。

- `<` `<=` は**オペランドを入れ替えて** `ja` / `jae` を使えば PF を見ずに正しくなる
- `=` は入れ替えでは解決せず、**PF を明示的に見る**必要がある(`setnp` と `sete` の論理積)
- **`/=` は比較の専用経路に乗っていない**(§4-2。二項版が無いため一般呼び出しに
  落ちる)ので、`ucomiss` 化の対象は `=` `<` `>` `<=` `>=` の 5 つになる。
  `/=` も速くしたいなら `primitive_num_not_equal2` の新設が先で、それは別作業である
