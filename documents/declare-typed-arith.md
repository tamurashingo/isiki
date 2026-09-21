# `declare` の型を使った `-` `*` `/` の特化

対象: PR(`feature/declare-typed-sub`、base は `feature/compiler-optimization`)

関連: `documents/declare-typed-add.md`(`+` の特化。枠組みはすべてそこ)、
`documents/type-specialized-dispatch.md`、`documents/fixnum-signed-fastpath.md`、
`documents/type-system-survey.md`

---

## 0. この文書の範囲と、分割の判断

指示書は `-` `*` `/` の 3 つを対象にしているが、**演算ごとに PR を分けた。**
この文書は**まず `-` を書き、`*` `/` は後続の PR で追記していく。**

分けた理由は指示書 §1 のとおりで、実装の性質が違うためである。

| 演算 | 実装の性質 | PR |
|---|---|---|
| `-` | `+` とほぼ同じ形。既存のコアをそのまま共有できる | **本 PR** |
| `*` | fixnum の桁溢れ判定を**書き直す必要がある**(§2-3) | 後続 |
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

**採否はこの数字を見て決める話なので、判断を求める。**

---

## 7. 次

- `*` の特化(§2-3 の桁溢れ判定を 128bit の積で書き直す)
- `/` の特化(§2-4。整数どうしの cons 2 個を飛ばす)
- **GENERIC の `*` を直す別 PR**(除算での桁溢れ判定と、負数が bignum 機構へ
  落ちる件。宣言なしのコードも速くなる)
- 比較演算(指示書 §8)
