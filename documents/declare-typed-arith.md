# `declare` の型を使った `-` `*` `/` の特化

対象: PR(`feature/declare-typed-sub`、base は `feature/compiler-optimization`)

関連: `documents/declare-typed-add.md`(`+` の特化。枠組みはすべてそこ)、
`documents/type-specialized-dispatch.md`、`documents/fixnum-signed-fastpath.md`、
`documents/type-system-survey.md`

---

## 0. この文書の範囲と、分割の判断

指示書は `-` `*` `/` の 3 つを対象にしているが、**演算ごとに PR を分けた。**
この文書は `-`(PR #91)から書き始め、**`*` を §8 以降に追記した。`/` は後続の PR で足す。**

分けた理由は指示書 §1 のとおりで、実装の性質が違うためである。

| 演算 | 実装の性質 | PR |
|---|---|---|
| `-` | `+` とほぼ同じ形。既存のコアをそのまま共有できる | PR #91 |
| `*` | fixnum の桁溢れ判定を**書き直す必要がある**(§2-3)。**GENERIC も同時に速くなる** | **本 PR** |
| `/` | ゼロ除算と切り捨て方向の確認が要る | 後続 |

---

## 1. 何をしたか(`-`)

**`+` と同じ形で、`-` を型別の呼び先へ振り分ける。新しい仕組みは作っていない。**

| 型の組 | 行き先 |
|---|---|
| fixnum × fixnum | `primitive_subtract2_fixnum` |
| single × single | `primitive_subtract2_single` |
| **それ以外すべて** | **GENERIC(`primitive_subtract2`)** |

`za_specialized_wrapper` は `+` 専用の早期 return を持っていたので、
**型の組で外側を分け、その中で演算を選ぶ形**に組み替えた。`*` `/` を足すときは
この中へ 1 行ずつ増える。

**単項の `-`(符号反転)は対象外。** `za_compile_fold` は引数が 2 つ未満では
そもそも呼ばれないため、`(- x)` は従来の経路のままである。

---

## 2. 調査(指示書 §4)

### 2-1 どの演算がインライン経路を持つか — **`-` は持つ**

**期待値を立てるために最初に確認した。**

`za_emit_arith_call_or_inline` は、呼び先が `primitive_add2` **または
`primitive_subtract2`** のときだけ、call を出す前に fixnum のインライン
高速路(タグ + 符号ビットの一括判定 → 生の `add` / `sub`)を出す。
`*` `/` はこの分岐に入らず、最初から call だけである。

実機の `disassemble` で裏づけた(PR #90 のシンボル解決で呼び先の名前が出る)。

| 演算 | 呼び先の注釈 | code-len | インライン経路 |
|---|---|---:|---|
| `+` | `primitive_add2` | 735 | **あり** |
| `-` | `primitive_subtract2` | **738** | **あり** |
| `*` | `primitive_multiply2` | **690** | **無い** |
| `/` | `primitive_divide2` | **690** | **無い** |

**690 が「call だけ」の長さである。** `+` `-` はそこに 45〜48 byte 多い。
これがインライン高速路のぶんで、`documents/declare-typed-add.md` §2-1 の
「735 → 690 の 45 byte 減」と同じ差である。

#### そこから立てた期待値

**`-` は `+` と同じく「fixnum は遅くなる / single は速くなる」**と予想した。
特化すると呼び先が `primitive_subtract2` でなくなるので、
`za_emit_arith_call_or_inline` の分岐に入らず、**インライン高速路が丸ごと
消えて call だけになる**からである。

`*` `/` は比較対象が最初から call なので、**特化は純粋な上積みになる**見込み。

**予想は当たった(§4-3)。しかも `+` より大きく遅くなった。**

### 2-2 `_unchecked` への分解は不要だった

`+` では `fixnum_add_signed` をタグ検査つき/なしに分解したが、
**`-` では新しい分解が要らなかった。**

GENERIC の `primitive_subtract2` は、もともと `a-b` を `a+(-b)` に帰着させて
`fixnum_add_signed` を呼んでいる(改善 A)。特化版はその `_unchecked` 版を
呼ぶだけでよい。

```c
/* GENERIC */
if ((b & TAG_MASK) == TAG_FIXNUM && fixnum_add_signed(a, fixnum_negate(b), &diff))
/* 特化 */
if (fixnum_add_signed_unchecked(a, fixnum_negate(b), &diff))
```

GENERIC 側の `(b & TAG_MASK) == TAG_FIXNUM` は `fixnum_negate` が
`os_fixnum_magnitude` を読むための前提確認なので、両方 fixnum と分かっている
特化版では省ける。**`-` 用のコアを書き足してはいない。**

### 2-3 `*` の現在の桁溢れ判定 — **除算を使っている**(指示書 §4-2)

確認した。`primitive_multiply2` の fixnum 高速路はこうなっている。

```c
if ((a & TAG_MASK) == TAG_FIXNUM && (b & TAG_MASK) == TAG_FIXNUM &&
    !os_fixnum_is_negative(a) && !os_fixnum_is_negative(b)) {
    UINT64 mag_a = os_fixnum_magnitude(a);
    UINT64 mag_b = os_fixnum_magnitude(b);
    if (mag_a == 0 || mag_b <= FIXNUM_MAGNITUDE_MASK / mag_a) {   /* ← 除算 */
        return os_make_fixnum(mag_a * mag_b);
    }
}
```

**問題が 2 つある。**

1. **桁溢れ判定に 64bit 除算が入っている。** 128bit の積で判定すれば消せる
2. **両方が非負のときしか通らない。** 負数が 1 つでも絡むと bignum 機構を
   丸ごと通る。これは改善 A 以前の `+` と同じ形で、実測で 9.7 倍遅かった構図
   (`documents/type-system-survey.md` §10-4)

**どちらも型特化とは独立した GENERIC 側の改善であり、直せば宣言なしのコードも
速くなる。** 指示書 §9 のとおり別 PR として提案する。

### 2-4 `/` は整数どうしで必ず cons を 2 個作る

`primitive_divide2` は float が絡まなければ**無条件に** cons を 2 個作って
n 項版 `primitive_divide` へ委譲する(`documents/divide-direct-call.md` §4-2)。

fixnum 特化はここを丸ごと飛ばせるので、**効果は `*` より大きい可能性がある。**

---

## 3. 監査ビルド(指示書 §5-4)

`+` と同じ `ISIKIOS_DECLARE_AUDIT` に乗せた。**通常ビルドの実行時コストはゼロ。**

```bash
make test-qemu-milestone MILESTONE=... EXTRA_CFLAGS=-DISIKIOS_DECLARE_AUDIT
```

**試していない検出器は働かない**ので、4 通りすべて実機で確かめた。

| ビルド | 呼び出し | 結果 |
|---|---|---|
| 監査 | 正しい(`(sub-fixnum 7 3)` / `(sub-single 2.5f0 1.5f0)`) | **通る**(4 と 1.0) |
| 監査 | `(sub-single 1 2)` | **panic**(下記) |
| 監査 | `(sub-fixnum 2.5f0 1.5f0)` | **panic**(下記) |
| 通常 | 同じ嘘 | **止まらない**(`0.0` と `1688849860263936`) |

```
PANIC: declare が嘘をついている
  関数: primitive_subtract2_single
  宣言された型: <single-float>
  実際のタグ: 0x0 (0=fixnum 4=single-float 7=instance)
```

```
PANIC: declare が嘘をついている
  関数: primitive_subtract2_fixnum
  宣言された型: <fixnum>
  実際のタグ: 0x4 (0=fixnum 4=single-float 7=instance)
```

**関数名が出るので、どちらの特化で嘘をついたかが分かる。**

### シリアルは test-qemu-milestone では捕まらない

`make test-qemu-milestone` の QEMU 起動には `-serial` が無く、**panic
メッセージはログに残らない。** 残るのは `test-results.txt` が
`BEFORE-LIE` で途切れることだけである。メッセージを見るには
`-serial stdio` を付けて QEMU を直接起動する必要がある。

---

## 4. 検証

### 4-1 振り分け(§5-2)— **シンボル名で直接確認した**

PR #90 で呼び先に関数名が付くようになったので、**code-len 経由の間接的な判定は
使っていない。** `test/lisp/declare_typed_sub_test.lisp` の `dts-has-callee` が
逆アセンブルの注釈を名前で突き合わせる。

| 宣言 | 呼び先の注釈 | code-len |
|---|---|---:|
| なし | `primitive_subtract2` | 738 |
| **fixnum** | **`primitive_subtract2_fixnum`** | **690** |
| **single-float** | **`primitive_subtract2_single`** | **690** |
| double-float(対照) | `primitive_subtract2` | 738 |
| 混在(fixnum × single) | `primitive_subtract2` | 738 |
| 引数 3 つ | `primitive_subtract2` | — |
| 単項 `-` | (特化を呼ばない) | — |

**取り違えの検出も入れた。** fixnum 版が `primitive_subtract2_single` を
呼んでいないこと、`primitive_add2_fixnum` を呼んでいないことを見ている。
`+` の PR では名前が引けず、これが確認できなかった。

### 4-2 結果が GENERIC と一致すること(§5-1)

`-` は非可換なので、**順序を入れ替えた対**を全部入れた
((- 7 3) と (- 3 7)、(- -3 -7) と (- -7 -3) など)。

- fixnum: 小さい値、2^24 前後、負数の 4 象限、**0 になる場合**
- **桁溢れ**: `(- *most-positive-fixnum* -1)`、`(- *most-negative-fixnum* 1)`、
  `(- *most-positive-fixnum* *most-negative-fixnum*)` → すべて bignum へ昇格し
  GENERIC と一致
- **桁溢れしない境界**: `(- *most-positive-fixnum* 0)`、`(- 0 *most-positive-fixnum*)`
  が bignum にならないこと
- single: `0.0 - 0.0` / `0.0 - -0.0` / `-0.0 - 0.0` / `-0.0 - -0.0` の 4 通り、
  非正規化数、`inf - 1.0`、**`inf - inf`(NaN)**、NaN を左右それぞれに

**0 の符号を個別に見ているのは、fixnum が符号マグニチュード表現だから**である。
`(- 5 5)` や `(- -5 -5)` が `-0` を作っていないことを `~S` の文字列で比べている。

### 4-3 速度(§5-5。n=200,000、3 回の最小値、同一ブート内)

| 型 | 宣言なし | 宣言あり | 差 |
|---|---:|---:|---:|
| **fixnum** | 120 | 148 | **+28(約 23% 遅い)** |
| **single-float** | 152 | 124 | **-28(約 18% 速い)** |
| double-float(対照) | 184 | 192 | +8 |
| bignum(対照) | 264 | 272 | +8 |

**対照群がそろって +8 なので、これが測定の底上げ分である。**
(対照群の 2 つは `bs-gen` とは別の関数オブジェクトで、配置が違う。)
fixnum の +28 と single の -28 は、その水準とは明らかに別である。

#### fixnum が `+` より大きく遅くなった

`+` の +8(3%)に対し、`-` は **+28(23%)**である。

**インライン高速路の効き方が違う**ためと考えている。`+` のインライン路は
「両方非負 fixnum かつ和が桁溢れしない」で成立するが、`-` のインライン路は
そこに「`b <= a`」が加わる代わりに、**成立したときは生の `sub` 1 命令で終わる。**
ベンチの `(- 1000000 3)` はこの条件を満たすので、宣言なし側は call を
1 回も出していない。宣言するとそれが全部 call になる。

**ただし TCG(KVM 無し)では分岐と call の相対コストが実機と違う**ので、
この比率をそのまま実機の値として読まないこと
(`documents/single-float.md` に同じ注意がある)。

**これは意図した帰結であって、失敗ではない**(§2-1 で事前に予想していた)。
とはいえ 23% は `+` の 3% と桁が違うので、§6 に対案の実測を残す。

### 4-4 コードサイズ(§5-6)

**カーネルイメージは +864 byte。** 分岐元(`e048c39`)を別の worktree で
ビルドして並べた(同じ docker イメージ、同じフラグ)。

| | 分岐元 | 本 PR | 差 |
|---|---:|---:|---:|
| `BOOTX64.EFI` | 2,771,535 | **2,772,399** | **+864** |
| `.text` | 0x213940 | 0x213aa0 | **+352** |
| `.rdata` | 0x21d60 | 0x21de0 | **+128** |
| `.data` | 0x2b20 | 0x2b20 | **±0** |

`.text` の +352 が特化関数 2 つの本体、`.rdata` の +128 は **PR #90 の
シンボルテーブルにこの 2 つが載ったぶん**である(名前の文字列 +
アドレスのエントリ)。`.rdata` は PR #90 でシンボルテーブルが
約 108 KB 入って 0x21d60 になっており、**今後 1 演算足すたびに
ここへ百数十 byte ずつ積み増す。**

**Immobilized Space の消費は増えない。** 特化版はカーネル側の C 関数であって
JIT 関数ではないので `za_fn_meta_t` を持たない。利用者の関数 1 つあたりの
メタデータも変わらない。

### 4-5 回帰(§5-7)

| | |
|---|---|
| `make test` | **8498 OK / 0 NG** |
| `make test-qemu` 256M | **4358 passed / 0 failed** |
| `QEMU_MEM=96M make test-qemu` | **4358 passed / 0 failed** |

4289 → 4358 の +69 は `declare_typed_sub_test.lisp` を足したぶんである。

`SMALL_HEAP_SIZE` の窓は動かなかった(組み込み関数としては登録していない。
特化版は JIT からの呼び先でしかなく、Lisp の名前を持たない)。

---

## 5. 測定で踏んだ罠: **bignum の対照群が「嘘の宣言」だった**

最初のベンチで、bignum の対照をこう書いた。

```lisp
(bs-time "bignum decl" (function bs-fix) *bs-big* 3 *bs-n*)   ; bs-fix は <fixnum> 宣言
```

**bignum を `<fixnum>` と宣言した関数に渡すのは「嘘」である。**
`primitive_subtract2_fixnum` はタグを見ずにマグニチュードとしてゴミを読み、
**速く間違った値を返す。**

その結果が出力に出ていた。

```
bignum   nodecl 296
bignum   decl   172      ← 「宣言すると 1.7 倍速い」
```

**対照群のはずが、いちばん派手に「改善」していた。**
これは §3 の監査ビルドが panic で止める条件そのものである。

#### 直し方

特化表に載っていない型を宣言する。`<integer>` は実在するクラスなので
`os_decl_type_code_of` は符号を返すが、`za_specialized_wrapper` が
fixnum / single-float 以外を受け付けないので **GENERIC のまま**である。
しかも bignum を渡しても**嘘にならない。**

```lisp
(defun bs-int (x y) (declare (type <integer> x)) (declare (type <integer> y)) (- x y))
```

嘘でないことを出力で確認している。

```
code-len int=738                                    ← gen と同じ = GENERIC
int-correct=2305843009213693945 gen=2305843009213693945
```

**対照群は「変わらないこと」を見るためにある。派手に動いたら、まず対照群
自身を疑う。** `documents/declare-types.md` §6-2 の「同じ数字が出たら疑う」の
裏返しで、**違いすぎる数字も疑う。**

---

## 6. 対案: インライン高速路を残したまま特化する(**未採用。実測のみ**)

§4-3 の fixnum の 23% 減速は、**特化すると `za_emit_arith_call_or_inline` の
分岐に入らなくなる**ことだけが原因である。呼び先の判定を 2 つ増やすと、
インライン路を残したまま、**フォールバック側だけを特化版にできる。**

```c
int is_add = (wrapper_fn == (void *)primitive_add2 || wrapper_fn == (void *)primitive_add2_fixnum);
int is_sub = (wrapper_fn == (void *)primitive_subtract2 || wrapper_fn == (void *)primitive_subtract2_fixnum);
```

**実際に当ててベンチを取った(同じ n、同じ形)。**

| 型 | 宣言なし | 宣言あり(現状) | 宣言あり(対案) |
|---|---:|---:|---:|
| **fixnum** | 112 | 148 相当(**+28**) | **120(+8)** |
| single-float | 144 | 124 | 124(変わらず) |
| double(対照) | 172 | 172 | 172 |

**fixnum の減速が対照群と同じ +8 の水準まで戻る。**

### それでも本 PR には入れなかった

- 指示書 §1 が「**新しい仕組みを作らない**」としている
- **`+` にも同時に効く**ので、PR #89 で「3% 遅いが可読性と引き換え」と
  記録した判断を、別の PR の外から書き換えることになる
- 特化版に付けた「タグ検査を出さない」という性格が半分戻る
  (インライン路の入口はタグと符号ビットを見る)。
  ただし**監査ビルドは効いたまま**である
  (嘘の型はタグ判定で落ちてフォールバックの call へ行き、そこで panic する)

### 見送りが決定した(PR #91 のレビュー)

**理由は速度ではない。** 「正しい型の引数ではフォールバックに到達しないので、
**特化関数が死んだコードになり、可読性の利点も消える**」ためである。

`-` の 23% は **inline フェーズで解決する**方針になった。
`za_emit_arith_call_or_inline` に手を入れて延命するのではなく、
インライン展開そのものの作業で扱う。

---

## 7. 次

- ~~`*` の特化~~ → **§8(本 PR)**
- `/` の特化(§2-4。整数どうしの cons 2 個を飛ばす)
- 比較演算(指示書 §8)
- `-` の fixnum 23% は **inline フェーズで解決する**(§6)

---

## 8. `*`(本 PR)

対象: PR(`feature/declare-typed-mul`、base は `feature/compiler-optimization`)

### 8-1 この PR は 2 つの変更を含む

**`*` だけは「特化を足す」話で終わらない。**

1. **GENERIC の高速路の改善(改善B)** — 桁溢れ判定から除算を外し、負数に対応させた。
   **宣言を書いていないコードにも効く**
2. **宣言による特化** — `primitive_multiply2_fixnum` / `_single` を足した

`_unchecked` へ分解してコアを共有する以上、**GENERIC の checked 版も新しいコアを
使う。** 1 は 2 の副産物ではなく、**分解した時点で必然的に付いてくる。**

**まとめて 1 つの PR にし、代わりに 3 状態で測った**(§8-5)。指示書は
「GENERIC を先に別 PR にしてもよい」としていたが、分解の形が
`+` `-` と同じである以上**コアを 2 度書くことにはならず**、分ける利点が
「測定の切り分け」だけになる。その切り分けは 3 状態の測定で付く。

### 8-2 桁溢れ判定 — `__int128` を選んだ

```c
static int fixnum_multiply_signed_unchecked(lisp_val_t a, lisp_val_t b, lisp_val_t *out) {
    unsigned __int128 prod =
        (unsigned __int128)os_fixnum_magnitude(a) * (unsigned __int128)os_fixnum_magnitude(b);
    if (prod > (unsigned __int128)FIXNUM_MAGNITUDE_MASK) {
        return 0;
    }
    *out = os_make_fixnum_signed(os_fixnum_is_negative(a) != os_fixnum_is_negative(b),
                                 (UINT64)prod);
    return 1;
}
```

**`__builtin_mul_overflow` ではなく `__int128` にした理由。**

`__builtin_mul_overflow` は「64bit に収まるか」しか見ないので、
**そのあとさらに `FIXNUM_MAGNITUDE_MASK` との比較が要り、判定が 2 段になる。**
`__int128` なら上の 1 行が**そのまま桁溢れの定義**になる。

**生成コードを確認した。** `mul` 1 命令と `cmp`/`sbb` による 128bit 比較で、
**除算も libgcc の呼び出しも無い。**

```
3d9a:  49 f7 e2     mul    %r10          ← u64 × u64 → rdx:rax
3d9d:  49 39 c1     cmp    %rax,%r9
3da6:  49 19 d0     sbb    %rdx,%r8      ← 128bit の大小比較
3daf:  72 18        jb     3dc9          ← 桁溢れなら 0 を返す
```

最終バイナリに `__multi3` / `__udivti3` / `__umodti3` への参照が無いことも
確認した(128bit で libgcc が要るのは**除算・剰余**であって積ではない)。

### 8-3 `*` は `+` `-` より単純だった

`documents/fixnum-signed-fastpath.md` §5-1 の予告どおりである。

**符号は `neg_a ^ neg_b` で決まり、マグニチュードの計算に符号が影響しない。**
`+` の「同符号なら加算、異符号ならマグニチュードを比較して大きいほうから引く」
という場合分けが要らない。

`-0` は `os_make_fixnum_signed` がマグニチュード 0 の符号を落とすので作られない。
gcc は `neg_a != neg_b` を生の値の XOR + `shr $0x3f` に畳んでいた。

### 8-4 n 項版も同じコアへ寄せた

`primitive_multiply` の高速ループも `fixnum_multiply_signed` を使う形にした
(`primitive_add` が `fixnum_add_signed` を使っているのと同じ)。
**`(* -2 3 4)` も bignum 機構を通らなくなる。**

### 8-5 速度 — **3 状態で測った**(指示書 §2)

n=200,000、3 回の最小値、同一ブート内。
**A は分岐元(`705f479`)を別 worktree でビルドし、同じベンチファイルを流した。**

| ケース | **A** 変更前 | **B** 変更後・宣言なし | **C** 変更後・宣言あり | A→B(GENERIC) | B→C(特化) |
|---|---:|---:|---:|---:|---:|
| **fixnum(負を含む)** | **252** | **128** | 132 | **−124(約 2 倍速い)** | +4 |
| fixnum(正のみ) | 128 | 128 | 132 | ±0 | +4 |
| **single-float** | 152 | 148 | **132** | −4 | **−16(約 11% 速い)** |
| double-float(対照) | 176 | 176 | 192 | ±0 | +16 |
| bignum(対照) | 284 | 280 | 300 | −4 | +20 |

雑音の幅は約 7%(指示書 §4-1)。**±8 tick 程度は差として読まない。**

#### 読み取り

**この PR の値は、ほぼ全部が A→B(GENERIC の改善)から出ている。**

- **負数を含む `*` が約 2 倍速くなった。** これは**宣言を書いていないコードにも
  効く。** 改善 A で `+` が 15.2 倍になったのと同じ構図で、
  bignum 機構(decompose → limb_alloc → mag_mul → os_make_integer)を
  丸ごと通らなくなったためである
- **正の fixnum は変わらなかった(±0)。** 除算を消したのに差が出ていない。
  **除算と引き換えに `os_make_fixnum_signed` の呼び出しが 1 回増えている**
  (以前は `os_make_fixnum` のインライン)ので、それで相殺された可能性がある。
  TCG では実機と命令コストの比が違うため、**「除算の除去に効果が無い」とは
  言えない。「この測定では差が出なかった」が正確である**
- **特化(B→C)は fixnum では差を生まなかった(+4 は雑音の幅の中)。**
  GENERIC が省いているのはタグ検査 2 回だけで、call 1 回に対して小さすぎる
- **single では特化が効いた(−16、約 11%)。** `+` `-` と同じ理由で、
  汎用関数の入口で fixnum を試して外し `float_kind_max` で型を判定する処理が
  丸ごと消えるため

#### 期待値との照合(指示書 §4-1)

事前の期待は「**`*` はインライン経路を持たないので、特化は純粋な上積みになる**」
だった。**上積みにはなったが、fixnum では測れる大きさではなかった。**
期待が外れた向きではなく、**大きさの見積りが外れた**。

**「`*` が遅い」の原因はインライン経路の有無ではなく、GENERIC の高速路が
負数を捨てていたことだった。**

### 8-6 code-len では振り分けを判定できない

`-` では GENERIC 738 / 特化 690 と差が出たが、**`*` は GENERIC も特化も 690 で
完全に同じ**である(`*` はもともとインライン経路を持たないため)。

```
code-len gen=690 fix=690 sgl=690 dbl=690 int=690
```

**PR #90 のシンボル解決が無ければ、振り分けを確認する手段が無かった。**
`documents/declare-typed-add.md` §6 で「code-len の差で判定した」と書いた方法は、
`*` には使えない。テストは `primitive_multiply2_fixnum` という**名前**で見ている。

### 8-7 監査ビルド

`-` と同じ `ISIKIOS_DECLARE_AUDIT` に乗せた。**4 通りすべて実機で確かめた。**

| ビルド | 呼び出し | 結果 |
|---|---|---|
| 監査 | 正しい(`(* 3 4)` `(* -3 4)` `(* 1.5f0 2.0f0)`) | **通る** |
| 監査 | `(mul-single 3 4)` | **panic**(`primitive_multiply2_single`) |
| 監査 | `(mul-fixnum 1.5f0 2.0f0)` | **panic**(`primitive_multiply2_fixnum`) |
| 通常 | 同じ嘘 | **止まらない**(下記) |

```
PANIC: declare が嘘をついている
  関数: primitive_multiply2_fixnum
  宣言された型: <fixnum>
  実際のタグ: 0x4 (0=fixnum 4=single-float 7=instance)
```

#### **通常ビルドの嘘が「たまたま正しい答え」を返す**

`-` では嘘の宣言が露骨に壊れた値を返した(`(sub-fixnum 2.5f0 1.5f0)` →
`1688849860263936`)。**`*` はそうならない。**

```
通常ビルド  (mul-fixnum 1.5f0 2.0f0)  ->  3.0f0     ← 正しい答え
通常ビルド  (mul-single 3 4)          ->  0.0       ← 壊れた値
```

single-float のビットをマグニチュードとして読むと 2^58 前後の巨大な値になり、
**その積は必ず桁溢れする。** 桁溢れすると GENERIC の `primitive_multiply` へ
落ちるので、そこで型が正しく判定されて正しい答えが出る。

**嘘をついても動いてしまうので、通常ビルドでは気づけない。**
`*` に関しては**監査ビルドが唯一の検出手段**である。
(`-` のように壊れた値が出れば気づけるが、それは保証されていない。)

### 8-8 検証

- **振り分け**: `primitive_multiply2` / `_fixnum` / `_single` を名前で確認。
  取り違え(single 版を呼んでいない、`+` `-` の特化を呼んでいない)も見ている
- **桁溢れの境界**: **定数から導いた**(直書きしない)。
  `(maxfix-1)/2` を 2 倍すると `maxfix-1` で収まり、`+1` してから 2 倍すると
  **ちょうど 1 だけ超えて** bignum になる。平方根の床 759250124 の 2 乗は収まり、
  その次の 2 乗は溢れる(根の選び方自体も assert している)
- **負数の全組み合わせ**: `(* 3 4)` `(* 3 -4)` `(* -3 4)` `(* -3 -4)`、
  **`-0` を作らないこと**(`(* 0 -1)` と `(* -1 0)` を `~S` の文字列で比較し、
  さらに `(quotient 1 (+ ... 1))` で符号が後段へ漏れないことも見る)
- **負数での桁溢れ**: 負×負 = **正**の bignum、負×正 = **負**の bignum、
  負の境界ちょうどが bignum にならないこと
- **n 項版**: `(* -2 3 4)` `(* -2 3 -4)` `(* -2 0 4)` `(*)` `(* -5)`、
  n 項での桁溢れ
- **対照群 `<integer>`** が嘘でないこと(bignum を渡して GENERIC と一致)

### 8-9 コードサイズ

| | 分岐元(`705f479`) | 本 PR | 差 |
|---|---:|---:|---:|
| `BOOTX64.EFI` | 2,772,399 | **2,774,338** | **+1,939** |
| `.text` | 0x213aa0 | 0x213c40 | **+416** |
| `.rdata` | 0x21de0 | 0x21e60 | **+128** |
| `.data` | 0x2b20 | 0x2b20 | ±0 |

`.text` の +416 は特化関数 2 つと新しいコア、`.rdata` の +128 は
PR #90 のシンボルテーブルにこの 2 つが載ったぶん(`-` のときと同じ)。
EFI 全体の +1,939 がセクションの合計 +544 より大きいのは、
セクション境界のアラインメントによる。

### 8-10 回帰

| | |
|---|---|
| `make test` | **8498 OK / 0 NG** |
| `make test-qemu` 256M | **4449 passed / 0 failed** |
| `QEMU_MEM=96M make test-qemu` | **4449 passed / 0 failed** |

4358 → 4449 の +91 は `declare_typed_mul_test.lisp` を足したぶんである。

`SMALL_HEAP_SIZE` の窓は動かなかった(特化版は組み込み関数として登録していない)。
