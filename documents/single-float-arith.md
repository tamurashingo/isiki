# single-float 算術の高速化

対象: PR(`feature/single-float-arith`、base は `feature/compiler-optimization`)

関連: `documents/declare-types.md` §7、`documents/single-float.md`、
`documents/nan-comparison.md`(NaN の規則)

---

## 1. 何をしたか

**single-float どうしの算術と比較を、C への call を挟まずに処理する。**

PR #84 の実測で single-float 演算は fixnum の 2 倍以上遅かった(540 対 252 tick)。
single-float はタグ 0x4 の即値で**ヒープ確保がゼロ**なのに、JIT の算術インライン
経路が fixnum 専用だったため、**毎回 C を呼んでいた**。

**案 B(XMM でインライン展開)を採った。** §2 の調査で XMM を安全に扱えると
確認できたためである。

### 対象

| 対象 | 内容 |
|---|---|
| 算術 | `+` `-` `*`(**single どうしのみ**) |
| 比較 | `<` `>` `<=` `>=` `=` `/=`(同上) |

**`/` は対象外。** za に `/` の算術経路そのものが無い(`za_syms_t` に居らず、
一般呼び出しに落ちる)。経路の新設は「既存の形に乗せる」という方針から外れる。

**double-float と混在(single × double)も対象外。** 従来どおり C へ落ちる。
型昇格の規則は C 側が持っているので二重実装しない(PR #80)。

---

## 2. 調査: XMM を使ってよいか

| 項目 | 結果 |
|---|---|
| JIT が出す XMM 命令 | **これまでゼロ**(整数レジスタのみだった) |
| 割り込み・タイマでの保存 | **FXSAVE/FXRSTOR** で x87/xmm0-15/MXCSR すべて(`interrupt.c:395,475`) |
| `spawn` の偽フレーム | **FXSAVE 領域あり**。`init_fpu` の既定状態で初期化(`process.c:332-339`) |
| MXCSR | **`0x1F80`** = 全 SIMD 例外マスク、round-to-nearest、**FTZ/DAZ なし**(`interrupt.c:940`) |
| SSE 有効化 | CR4 の OSFXSR=1 / OSXMMEXCPT=1 |

**MXCSR が既定値で FTZ/DAZ が立っていない**ので、非正規化数で C 経由と
食い違うことは無い(§4 で実測)。

### 守っていること

- **揮発 xmm0/xmm1 だけを使い、call をまたがない。** したがってプロローグ・
  エピローグに保存・復元を足す必要が無い。PR #65(r12 常駐)で踏んだ
  「callee-saved は実行中ずっと不変ではない」問題は生じない
- **タグ付きの Lisp 値を XMM に置かない。** 置くと GC から見えなくなる。
  XMM に入るのは float のビット列だけである
- シフト量とタグは `SINGLE_FLOAT_VALUE_SHIFT` / `TAG_SINGLE_FLOAT`(PR #79)を使う

---

## 3. 実装

### 3-1 算術: 三段構え

`za_emit_arith_call_or_inline` をこうした。

```
[fixnum 高速路]  両方非負 fixnum? → add/sub をインライン   (+ と - のみ。従来どおり)
       ↓ 外れる
[single 高速路]  両方 single?     → XMM でインライン        (+ - *)
       ↓ 外れる
[フォールバック] primitive_*2 へ call                       (混在・double・整数の一般形)
```

`*` は fixnum のインライン経路を持たないので single 判定から始まる。

**fixnum の経路より単純である。** float は範囲を超えても別表現へ昇格せず
無限大になるだけなので、fixnum の `js`(オーバーフロー検出)に相当する検査が要らない。

```asm
; 両方 single と分かった後
mov  r10, rcx / shr r10, 32 / movd xmm0, r10d
mov  r10, rdx / shr r10, 32 / movd xmm1, r10d
addss xmm0, xmm1                  ; subss / mulss
movd r10d, xmm0 / shl r10, 32 / or r10, 4
mov  rax, r10
```

### 3-2 比較: ucomiss + cmov(分岐を使わない)

**`ucomiss` は非順序(NaN)で ZF=PF=CF=1 になる。** これは「より小」「等しい」と
フラグで区別できないため、素朴に `jb` / `je` を使うと NaN で誤る。

| 演算子 | `ucomiss` の順序 | 主 cmov | 補正 |
|---|---|---|---|
| `>` | a, b | `cmova` | 不要(非順序は CF=1 で自然に偽) |
| `>=` | a, b | `cmovae` | 不要(同上) |
| `<` | **b, a** | `cmova` | 不要(**入れ替えで PF を見ずに済む**) |
| `<=` | **b, a** | `cmovae` | 不要(同上) |
| `=` | a, b | `cmove` | **`cmovp` で nil へ戻す**(ZF=1 に吸われるのを防ぐ) |
| `/=` | a, b | `cmove` で nil | **`cmovp` で t へ戻す**(**向きが他と逆**) |

非順序が真になるのは `/=` だけである(IEEE 754)。**C 側の `num_ne`(PR #85)と
一致させている。**

分岐ではなく cmov で組むのは、TCG では分岐の追加が重いためである
(`documents/inline-builtin.md` の実測で、分岐版は呼び出しより 11% 遅かった)。

`g_sym_t` は GC ヒープ上にあり移動するので、**アドレスを movabs して deref する**
(`za_emit_inline_bool_from_flags` と同じ理由)。`movabs` も `mov r64,[r64]` も
フラグを変えないので `ucomiss` の結果は保たれる。

### 3-3 逆アセンブラへの追加(必須だった)

`disassemble_test.lisp` の `disasm-no-undecodable-items` は
「za.c に新しい命令の出力を足したときに落ちて気付ける」設計である。**実際に落ちた**
(デコード失敗 28 個)。

**プレフィックスが未対応だった。** `0x66` と `0xF3` は SSE では**オペコードの一部**
として働く(`66 0F 6E` = movd、`F3 0F 58` = addss)のに、デコーダは REX しか
読んでいなかった。REX より前に来るので先に読む形にした。

| 追加したデコーダ | 命令 |
|---|---|
| `0F 40-4F` | `cmovcc`(setcc と同じ作りで `g_jcc` から条件部分を流用) |
| `0F 2E` / `0F 2F` | `ucomiss` / `comiss` |
| `66 0F 6E` / `66 0F 7E` | `movd xmm,r32` / `movd r32,xmm` |
| `F3 0F 58/59/5C/5E` | `addss` / `mulss` / `subss` / `divss` |
| `C1 /digit` | `shl` / `shr`(group2 テーブルを新設) |

`cmovcc` は既存の `jit_cmove_reg_reg` でも出ていたが、`INLINE_BIT_NULL` /
`INLINE_BIT_EQ` が既定で無効なため `dis-add` には現れず、**今まで露見していなかった**。

---

## 4. 検証

### 4-1 正しさ: C 経由と JIT 経由がビット単位で一致すること

**同じ式をインタプリタ(C 経由)と JIT コンパイル済み関数の両方で評価し、
印字結果を突き合わせた。** `test/lisp/single_float_arith_test.lisp`。

- 通常の値、±0.0(`(* 0.0f0 -1.0f0)` が -0.0 になること)
- 無限大、NaN
- **非正規化数**(MXCSR の FTZ/DAZ が効いていると食い違う)
- 型が保たれること、混在が C へ落ちること
- NaN が絡む比較 6 種(`/=` だけが真)

**非正規化数の実測:**

```
denorm = 1.0f-40  → 9.9999f-41   (0 に潰れていない = DAZ も FTZ も無い)
(+ denorm denorm) → 1.999989f-40 (C 経由と一致)
```

**指数マーカーは `f` を使うこと。** `1.0e38f0` のような書き方は PR #79 以降
曖昧で、意図した値にならない(最初これで非正規化数を作れておらず、
テストとして機能していなかった)。

### 4-2 速度(同一ブート内、n=1,000,000)

| ベンチ | 変更前 | 変更後 |
|---|---:|---:|
| fixnum 加算(対照) | — | 268 tick |
| **single-float 加算** | (PR #84 で 540) | **232 tick** |
| **single-float 比較** | — | **212 tick** |
| double-float 加算(対照) | — | 120 tick / n=200000(確保あり) |

**single-float が fixnum を下回った。** 目標は「fixnum に近づく」だったが、
fixnum の高速路が符号・オーバーフロー検査を持つのに対し、single は
`addss` 一発で済むぶん軽い。

ループ本体の**ヒープ確保はゼロ**(計測窓の 48 byte は `funcall` の引数 cons ぶんの固定分)。

### 4-3 コードサイズ(§6-6)

| 関数 | 変更前 | 変更後 | 増分 |
|---|---:|---:|---:|
| `(+ a b)` | 735 | 818 | **+83** |
| `(- a b)` | 738 | 821 | **+83** |
| `(* a b)` | 690 | 773 | **+83** |
| `(< a b)` | 690 | 779 | **+89** |
| `(= a b)` | 690 | 783 | **+93** |
| `(/= a b)` | 690 | 783 | **+93** |
| `(car x)`(対照) | 487 | 487 | 0 |

**1 演算あたり +83〜93 byte。** Immobilized Space の消費は
480,784 → 484,880 byte(**+4,096 = 1 ページ**、全体の 0.85%)。

`car` が変わっていないことが、増加が算術・比較だけに限られている確認になる。

### 4-4 回帰

| 検証 | 結果 |
|---|---|
| `make test` | 8498 OK / 0 NG |
| `make test-qemu` 256M | **4270 passed / 0 failed** |
| `QEMU_MEM=96M` | **4270 passed / 0 failed** |

---

## 5. 副次的に回避されたこと

C 側は single どうしでも `to_double(a) + to_double(b)` と **double で計算してから
single に丸めている**。`+` `-` `*` は正確な結果が double で表現できるので
`addss` / `subss` / `mulss` と一致するが、**除算だけは二重丸めで結果が変わりうる**
(正確な商を double に丸め、さらに single に丸めるため)。

**`/` が対象外なので、この問題は起きない。** 将来 `/` をインライン化するなら、
`divss` のほうが IEEE 754 的に正しい一方で**現在の C 実装と結果が変わる**ことを
先に確かめる必要がある。

---

## 6. 次に考えられること

**double-float の高速化は算術だけでは効かない。** double はヒープ上の instance で、
演算をインライン化しても**結果の確保が残り**そこが支配的である。タグ 0xB が将来用に
空けてあるが(PR #79)、double を即値にするには 64bit 必要でタグの 4bit が入らない。
NaN boxing などの別の手を検討することになり、規模が大きい。

**`(+ a b)` のプロローグが支配的**という PR #84 の観測は変わらない。
対照の `car` が 487 byte で、算術の 818 byte との差は 331 byte しかない。
**算術そのものより関数の入口のほうが大きい。** 次に大きな効果を狙うならそちらである。

`/` の算術経路を作ることと、fixnum の比較のインライン化(`<` は fixnum でも
call のまま)も残っている。
