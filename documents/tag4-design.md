# 4bitタグ体系の設計

調査日: 2026-09-18 / ブランチ: `feature/tag4-design`
派生元: **`feature/jit-tag-constants`（PR #75、2026-09-18時点で未マージ）**。
前提（2章）が PR #75 の成果（`FIXNUM_VALUE_SHIFT` / `CHAR_VALUE_SHIFT` への集約、
JIT のタグ定数の導出化）を織り込んでいるため、main ではなくそちらから派生している。
**PR #75 を先にマージしてください。**

本書は設計の確定が目的で、**実装は含まない**。

---

## 1. 5つの問いへの回答

### 問1: 現行のタグと Lisp の型の対応。ポインタ型は8個に収まるか

**収まる。必要なのは6個で、2個空く。**

ポインタとして独立したタグが要るのは `CONS` / `SYMBOL` / `STRING` / `INSTANCE` /
`RAW_POINTER` / `FORWARD` の**6種**。vector・bignum・float・stream・関数・プロセス・
クラス・非局所脱出は**すべて `TAG_INSTANCE` の下で word0 の `MAGIC_*` によって
区別されており、タグを消費していない**（2.2）。

4bit・bit0=1 のポインタ側スロットは `0x1,3,5,7,9,B,D,F` の8個なので、
6個を割り当てて `0xB` / `0xD` が空く。

### 問2: 提示された割り当て案で不都合が出る箇所

**1件、重大な不都合がある: `0x4` の single-float。**

この処理系の `<float>` は **IEEE754 binary64（double）** で、ヒープ上の
`MAGIC_FLOAT` インスタンスに格納されている（`runtime.c:4470`）。
テストは17桁の有効数字を要求しており（`3.141592653589793`、
`0.16666666666666666`、`1.557407724654902`）、binary32（有効約7桁）へ
変えると**即座に落ちる**。

したがって「`<float>` を即値 single-float にする」ことはできない。
`0x4` は**将来 single-float を別の型として導入するための予約**に留め、
`<float>` は binary64 のままヒープに置く（4.3）。

そのほかは 3.2 の依存表のとおりで、機械的に対応できる。

### 問3: `MAGIC_*` / `TAG_FORWARD` / `GC_DEBUG_TRAP_PATTERN_VALUE` の値をどう決めるか

**`MAGIC_*` の下位4bitを、予約タグ `0xE` に揃える。**
現在は 0x1〜0xF の連番で、新体系では `MAGIC_STANDARD_CLASS`(0xF) が
`TAG_FORWARD`(0xF) と下位4bitで衝突する。`0x0E, 0x1E, 0x2E, …, 0xDE` と振り直せば、
**どの MAGIC も有効なタグとして解釈されない**（0xE は即値側の予約タグ）。
14個でも `0xDE` までで、`MAGIC_MUST_BE_BELOW`(0x1000) に十分収まる。

`GC_DEBUG_TRAP_PATTERN_VALUE` も末尾を `0xE` に揃える
（`0xDEADDEADDEADDEAE`）。ただし `interrupt.c:765` が `& ~0x7` でマスクした
比較をしているので連動が要る（5.3）。

**ただし範囲検査は廃止できない。** `TAG_STRING` の word0 は**生の長さ**という
制御できない整数で、長さが偶然 `0xF` で終われば必ず衝突する。
値の振り直しは「制御できるものを衝突させない」ための改善であって、
範囲検査の代わりにはならない（5.2）。

### 問4: `most-positive-fixnum` を 2^59−1 にしたときの影響

まず前提の訂正が2つある。

1. **`most-positive-fixnum` / `most-negative-fixnum` はこの処理系に存在しない。**
   C にも Lisp にもテストにも定義が無い。実体は `FIXNUM_MAGNITUDE_MASK`
   （`runtime.h:47`、現在 `(1<<60)-1`）である。
2. **現在の fixnum は2の補数ではなく「符号＋絶対値」表現**である。
   マグニチュードが bit3-62 の60bit、符号が bit63（`FIXNUM_SIGN_BIT`）。
   範囲は **−(2^60−1) 〜 +(2^60−1)**で、−0 は存在しない（正規化される）。

そのため指示書が書いている `most-negative-fixnum = −2^59` は、
**2の補数へ変えない限り達成できない**。符号絶対値のままシフトを4にすると
**±(2^59−1)** になる。この選択が実装規模を大きく左右するので、4.1 で
選択肢A（符号絶対値維持）/ 選択肢B（2の補数化）として提示する。
**推奨はA**（JIT の fixnum 高速路がそのまま生きるため）。

影響箇所は 4.2 の表（C側6箇所＋Lispテストのハードコード30箇所）。

### 問5: 実装フェーズをどの順で、どこまでの単位に分けるか

**4段階。** タグ値の変更・シフト量の変更・JIT の untag は**1つのPRにまとめる
必要がある**（分割すると途中で必ず壊れる）。詳細は7章。

---

## 2. Step 1: 現行タグの棚卸し

### 2.1 現行タグ一覧

| 値 | 名前 | 種別 | 対応する Lisp の型 | 主な判定箇所 |
|---|---|---|---|---|
| `0x0` | `TAG_FIXNUM` | 即値 | `<integer>`（fixnum範囲） | `primitive_integerp`、`runtime.c:3014` |
| `0x1` | `TAG_CONS` | ポインタ | `<cons>` / `<null>`(nil) | `primitive_consp1`（`runtime.c:6199`） |
| `0x2` | `TAG_SYMBOL` | ポインタ | `<symbol>` | `primitive_symbolp1`（`runtime.c:6174`） |
| `0x3` | `TAG_CHAR` | 即値 | `<character>` | `primitive_characterp1`（`runtime.c:6402`） |
| `0x4` | `TAG_STRING` | ポインタ | `<string>` | `primitive_stringp1`（`runtime.c:6748`） |
| `0x5` | `TAG_INSTANCE` | ポインタ | 下記すべて | `MAGIC_*` で細分 |
| `0x6` | `TAG_FORWARD` | ポインタ | （GC内部。word0のみ） | `gc_copy_value` |
| `0x7` | `TAG_RAW_POINTER` | 即値扱い | （Lispの型ではない） | `os_tag_is_heap_ref` が素通し |

型述語はすべて `(val & TAG_MASK) == TAG_X` の**単一タグ等値比較**で、
`(tag & 0x3) == 0x1` のような複数タグをまとめる判定は**1箇所も無い**。
これは新体系への移行を素直にする。

### 2.2 タグを持たない型（`TAG_INSTANCE` の下で `MAGIC_*` により区別）

| `MAGIC_*` | 値 | 型 |
|---|---|---|
| `MAGIC_FUNCTION_NATIVE` | 0x1 | 関数（C / JIT / lifted closure） |
| `MAGIC_FUNCTION_INTERPRETED` | 0x2 | 関数（defun / lambda） |
| `MAGIC_PROCESS` | 0x3 | プロセス（PCB） |
| `MAGIC_MACRO` | 0x4 | マクロ |
| `MAGIC_BLOCK_EXIT` | 0x5 | 非局所脱出（block/return-from） |
| `MAGIC_STREAM` | 0x6 | `<stream>` |
| `MAGIC_CLASS_INSTANCE` | 0x8 | ILOS インスタンス |
| `MAGIC_CATCH_EXIT` | 0x9 | 非局所脱出（catch/throw） |
| `MAGIC_GO_EXIT` | 0xA | 非局所脱出（tagbody/go） |
| `MAGIC_BIGNUM` | 0xB | `<integer>`（bignum範囲） |
| `MAGIC_VECTOR` | 0xC | `<general-vector>` / `<general-array*>` |
| `MAGIC_FLOAT` | 0xD | **`<float>`（binary64）** |
| `MAGIC_BUILTIN_CLASS` | 0xE | `<built-in-class>` |
| `MAGIC_STANDARD_CLASS` | 0xF | `<standard-class>` |

**14種がタグを1つも消費していない。** これがポインタ型6個で足りる理由である。

### 2.3 将来追加が見込まれる型

| 型 | 現状 | 新体系での置き場所 |
|---|---|---|
| single-float | **未実装**（痕跡も無い。`grep single.float/binary32/float32` = 0件） | 即値 `0x4` を予約 |
| double-float | 現 `<float>` がこれ。ヒープの `MAGIC_FLOAT` | **即値にできない**（64bit全部が仮数・指数）。`MAGIC_FLOAT` のまま |
| ratio | 未実装。`<float>`/`<integer>` 以外の数値クラスは無い | ポインタ側の空き `0xB` / `0xD`、または `MAGIC_RATIO` |
| `<floating-point-overflow>` 等の条件クラス | `init.lisp:522` に定義済みだが「浮動小数点数自体が未実装のため発生源を持たない」とコメント（**binary64は実装されているので、このコメント自体が古い**） | タグとは無関係 |

**ポインタ側の空き `0xB` / `0xD` は空けておくことを勧める。** double-float と ratio は
どちらも即値にできず、追加するならポインタ型になるためである。
vector や bignum を `MAGIC_*` から昇格させて型判定を速くする案もあるが、
それは最適化であって本フェーズの目的ではない。

### 2.4 確定した新タグ割り当て表

4章の案から**1点だけ変更**した（`0x4` の位置づけ）。

| 値 | bit0 | 種別 | 型 | ペイロード | 備考 |
|---|---|---|---|---|---|
| `0x0` | 0 | 即値 | fixnum | bit4-63 | 表現は 4.1 で決める |
| `0x2` | 0 | 即値 | character | bit32-63（Unicode コードポイント） | 現在は8bit（4.4） |
| `0x4` | 0 | 即値 | **（予約）** single-float | bit32-63（binary32） | **`<float>` は割り当てない**。型自体が未実装 |
| `0x6` `0x8` `0xA` `0xC` | 0 | 即値 | 未割当 | | |
| `0xE` | 0 | 即値 | **予約（poison / MAGIC / 番兵）** | | 3章で用途を確定 |
| `0x1` | 1 | ポインタ | cons | 16byte境界 | nil を含む |
| `0x3` | 1 | ポインタ | symbol | 同上 | |
| `0x5` | 1 | ポインタ | string | 同上 | |
| `0x7` | 1 | ポインタ | instance | 同上 | `MAGIC_*` で14種に細分 |
| `0x9` | 1 | ポインタ | raw pointer | 同上 | GC 非管理 |
| `0xB` `0xD` | 1 | ポインタ | 未割当 | | double-float / ratio 用に空けておく |
| `0xF` | 1 | ポインタ | forward（GC中のみ） | 転送先アドレス | |

`os_tag_is_heap_ref` は新体系では次の形に単純化できる:

```c
/* bit0=1 がポインタ。そのうち RAW_POINTER だけは GC 管理外 */
return (tag & 1) != 0 && tag != TAG_RAW_POINTER;
```

---

## 3. Step 2: タグ値への依存箇所

PR #75 でマクロ経由への集約は済んでいるが、**値そのものへの依存**は別問題である。
洗った結果、依存は**4種類・実質5箇所**しか無かった。

| # | 箇所 | 依存の内容 | 新体系での扱い |
|---|---|---|---|
| 1 | `lisp.c:82` `cc_assoc_eq` | `k & TAG_FIXNUM` — **`TAG_FIXNUM == 0` に依存**し、常に偽（死んだ分岐）。PR #75 報告書 3.4 で記録済み | **修正**。`(k & TAG_MASK) == TAG_FIXNUM` にする。新体系でも `TAG_FIXNUM == 0` は変わらないので挙動は同じだが、意図が読めない式を残さない。あわせて `_Static_assert(TAG_FIXNUM == 0)` を置き、「fixnum のタグが0である」ことに依存する最適化（4.1 の JIT 高速路）の前提を明示する |
| 2 | `runtime.c:516,518` `g_align_val_hist[8][16]` / `g_align_val_first_bad[8]` | **タグを配列添字に使い、サイズが8** | **修正**。`[16]` へ拡張。`_Static_assert(TAG_MASK < ARRAY_SIZE)` を添える |
| 3 | `runtime.c:747` `for (UINT64 tag = 0; tag < 8; tag++)` | 同上のループ上限 | **修正**。`TAG_MASK + 1` から導出する |
| 4 | `runtime.c:753` `if ((UINT64)i != tag)` | 「16byte境界なら下位4bit == タグ値」という前提 | **そのまま**。4bitタグ＋16byte境界でも成立する（PR #74 で境界は保証済み） |
| 5 | `print.c:343` / `runtime.c:685,2050,2367` の `switch (tag)` | ジャンプテーブル。タグ値の連続性に依存はしない（`case` は名前で書かれている） | **そのまま** |
| 6 | `align_tag_name()`（`runtime.c`） | タグ→名前。`switch` による対応で添字ではない | **修正**（新タグ名を追加）。添字ずれの危険は無い |
| 7 | AOT 生成コード（`lisp_compiled.c`） | タグ操作 **0件**（再確認済み） | **なし** |
| 8 | AOT ジェネレータ（`transpile.lisp`） | `TAG_` の出現2件はいずれも**コメント** | **なし** |
| 9 | 型述語（`primitive_consp1` 等） | すべて単一タグの等値比較 | **なし**（マクロ経由で自動追随） |
| 10 | JIT のタグ定数 | PR #75 で `TAG_MASK` から導出済み。T3 は0件 | **なし**（自動追随。`_Static_assert` が範囲を守る） |

**タグの大小比較（`tag < N`）は、上記 #3 の監査ループ1箇所だけ**だった。
型判定に順序を使っている箇所は無い。

---

## 4. Step 3: 即値3種の設計

### 4.1 fixnum — 表現の選択（**要判断**）

現在は**符号＋絶対値**である。

```
bit63      bit62 ............ bit3   bit2-0
 符号      マグニチュード(60bit)      タグ(000)
```

`os_make_fixnum_signed`（`runtime.c:3001`）・`os_fixnum_magnitude`（3014）・
`os_fixnum_is_negative`（`FIXNUM_SIGN_BIT`）がこの形を前提にしている。

#### 選択肢A: 符号絶対値のまま、シフトを4にする（**推奨**）

```
bit63      bit62 ............ bit4   bit3-0
 符号      マグニチュード(59bit)      タグ(0000)
```

- 範囲: **−(2^59−1) 〜 +(2^59−1)** = ±576,460,752,303,423,487
- `FIXNUM_MAGNITUDE_MASK` を `(1<<59)-1` にするだけ
- **JIT の fixnum 高速路がそのまま生きる**。これが決め手である:

  ```c
  jit_movabs_reg(ZA_REG_R9, TAG_MASK | FIXNUM_SIGN_BIT);  // 両方が「タグ0かつ符号0」か
  jit_test_reg_reg(ZA_REG_R10, ZA_REG_R9);
  ...
  jit_add_reg_reg(ZA_REG_R10, ZA_REG_RDX);
  jit_emit_js_rel32_placeholder();   // bit62からの桁上がりがbit63に出る = オーバーフロー
  ```

  マグニチュードが bit4-62 になっても、**桁上がりは変わらず bit63 に出る**ので
  `js` による検出はそのまま成立する。`test` のマスクも `TAG_MASK|FIXNUM_SIGN_BIT`
  から導出しているので自動追随する。**JIT は定数以外1行も変わらない。**
- 指示書の `most-negative-fixnum = −2^59` は**達成できない**（−(2^59−1) になる）

#### 選択肢B: 2の補数へ変える

```
bit63 ............ bit4   bit3-0
  符号付き整数(60bit)      タグ(0000)
```

- 範囲: **−2^59 〜 2^59−1**（指示書どおり）
- −0 の正規化が不要になり、`os_fixnum_is_negative` / `os_make_fixnum_signed` /
  `os_fixnum_magnitude` の3関数と、その**全呼び出し元**が変わる
- **JIT の高速路は書き直し**。`js` によるオーバーフロー検出は使えず、
  `jo`（overflow flag）に変える必要がある。加算の `test` 前提も崩れる
- bignum との相互変換（`os_make_integer` は符号＋limb配列を受け取る）も
  境界の扱いが変わる

#### 比較

| | A: 符号絶対値 | B: 2の補数 |
|---|---|---|
| 範囲 | ±(2^59−1) | −2^59 〜 2^59−1 |
| 指示書の記述と一致 | いいえ | はい |
| C側の変更 | `FIXNUM_MAGNITUDE_MASK` の定義1行 | 3関数＋全呼び出し元 |
| JIT の変更 | **なし**（定数が自動追随） | **高速路の書き直し** |
| bignum 境界 | 既存ロジックのまま | 見直しが要る |

**推奨はA。** 本フェーズの目的はタグ幅の拡張であって数値表現の刷新ではなく、
B を選ぶと変更範囲が数倍になり「タグ拡張が原因の不具合か、表現変更が原因の
不具合か」を切り分けられなくなる。−2^59 の1値が使えないことによる実害も
（`most-negative-fixnum` が未定義である以上）現時点では無い。
B を採るなら**別フェーズ**にすべきである。

### 4.2 fixnum — 影響を受ける箇所

| 箇所 | 内容 | A の場合 |
|---|---|---|
| `runtime.h:47` `FIXNUM_MAGNITUDE_MASK` | `(1<<60)-1` | **`(1<<59)-1` に変更** |
| `runtime.c:3014` `os_fixnum_magnitude` | `(val >> SHIFT) & MASK` | 自動追随 |
| `runtime.c:4386` `os_make_integer` の昇格判定 | `magnitude <= MASK` | 自動追随 |
| `runtime.c:4833,4901` `primitive_add2/subtract2` の高速路 | `sum > MASK` | 自動追随 |
| `runtime.c:5118,5170` 乗算の高速路 | `product > MASK / mag` | 自動追随 |
| `za.c:2436-2460` JIT の fixnum 高速路 | `TAG_MASK\|FIXNUM_SIGN_BIT` と `js` | **自動追随**（4.1） |
| `reader.c:506` 10進リテラルの累積 | `os_make_integer` へ委譲 | 自動追随 |
| `print.c` の bignum 印字 | 同上 | 自動追随 |
| `test/c/runtime_test.c`（14箇所） | **`FIXNUM_MAGNITUDE_MASK` をシンボルで参照** | **自動追随**（値を書いていない） |
| `test/lisp/init_test.lisp`（22箇所） | **`1152921504606846975/6/7` を10進で直書き**、`#x1000000000000000` も | **手で書き換え**（→ `576460752303423487` 等） |
| `test/lisp/za_test.lisp`（7箇所） | 同上 | **手で書き換え** |
| `test/lisp/za_test_ext13.lisp`（1箇所） | 同上 | **手で書き換え** |

**C のテストはシンボル参照なので自動で追随し、Lisp のテストだけが手作業になる（30箇所）。**
この非対称は覚えておく価値がある。将来のために、Lisp 側にも
`most-positive-fixnum` / `most-negative-fixnum` を定数として導入し、
テストがそれを参照する形にすることを勧める（7.3 のテスト設計に含めた）。

### 4.3 single-float — 現状と、即値化に必要な作業

**現状: 存在しない。** `single-float` / `binary32` / `float32` の文字列は
ソース全体で0件。`<float>` は binary64 で、`os_make_float(double)`（`runtime.c:4470`）が
`MAGIC_FLOAT` インスタンスの word1 にビットパターンを入れている。

`<float>` を binary32 の即値にすると、次のテストが即座に落ちる:

```lisp
(assert-float-close 3.141592653589793 *pi*)           ; 16桁
(assert-float-close 0.16666666666666666 (quotient 2 3 4))  ; 17桁
(assert-float-close 1.557407724654902 (tan 1))        ; 16桁
```

binary32 の有効桁は約7桁なので、丸めの時点で一致しない。

**したがって本フェーズでは `0x4` を「予約」に留める。** 実際に single-float を
導入する場合に必要な作業（別フェーズ）:

1. ISLisp の `<float>` をどちらに割り当てるかの決定（仕様上 `<float>` の精度は
   処理系定義。現行の binary64 を維持し、single-float を**別の型**として足すのが自然）
2. リーダ: `1.0s0` のような接尾辞の構文をどうするか
3. プリンタ: binary32 の最短往復表現
4. 演算: 加減乗除・比較・`float` 変換。binary64 との混在規則
5. JIT: **現在 SSE 命令を1つも出力していない**（`jit_emit*` の出力に
   `0x0F 0x6E`(movd) や `0xF3 0x0F` 系が無い）。即値 single-float の演算を
   JIT で速くするなら SSE 命令の出力を新規に追加する必要がある。
   C の `primitive_*` へ委譲するだけなら不要
6. 型述語・`<float>` のクラス階層への組み込み

### 4.4 character — 8bit から Unicode コードポイントへ

**現状は実質8bit**である。`os_make_char(const char c)`（`runtime.c:3220`）が
`char`（x86-64 では**符号付き**）を受け取り、`(lisp_val_t)c << 3 | TAG_CHAR` とする。

このため **0x80 以上のバイトで符号拡張のゴミが上位に残る**:

```
os_make_char((char)0xE3) => 0xFFFFFFFFFFFFFF1B
  タグ(下位3bit) = 3     … 正しい
  (UINT8)(v>>3)  = 0xE3  … 復元はできる
  bit8以上が全部1        … ゴミ
```

`char_compare`（`runtime.c:6410`）が `(UINT8)` でマスクし、印字も `(UINT8)` で
取り出すため**動いてはいる**が、タグ付き値としては汚れている。
`eq` 比較は同じゴミ同士なので一致する。

新体系では **bit32-63 に Unicode コードポイントを置く**ことで、この問題は
構造的に消える（`UINT32` を格納するので符号拡張が起こらない）。

必要な作業:

| 箇所 | 内容 |
|---|---|
| `runtime.c:3220` `os_make_char` | 引数を `UINT32`（コードポイント）に変え、`(UINT64)cp << 32 \| TAG_CHAR` |
| `runtime.c:6410` `char_compare` | `(UINT32)(v >> 32)` で比較 |
| `subprimitive.c:151,162` `%%CHAR-CODE` / `%%CODE-CHAR` | 同上。**`%%CODE-CHAR` に範囲検査が無い**（現在は何を渡しても通る）。`cp <= 0x10FFFF` かつサロゲート領域（0xD800-0xDFFF）でないことを検査し、外れたら `<domain-error>` を返す |
| `print.c:359` / `format.c:67,205` / `stream_lisp.c:211` | `(UINT8)` で取り出している。UTF-8 エンコードして出力するか、当面 ASCII に限るかを決める |
| `runtime.c:7436` `os_make_char((char)bytes[idx])` | 文字列は**バイト列**なので、`elt` が返す「文字」の定義を決める必要がある（バイト単位のままか、UTF-8 デコードするか） |

**注意**: 文字列が UTF-8 バイト列のままなら、`elt` / `length` の意味が
「バイト」か「文字」かという別の設計判断が発生する。
本フェーズでは**タグとペイロードの形だけを決め**、文字列の文字単位化は
別フェーズとするのが安全である。`CHAR_VALUE_SHIFT` を 32 にする変更自体は、
上の5箇所に閉じる。

---

## 5. Step 4: `MAGIC_*` と番兵値の設計

### 5.1 番兵値の一覧と、新体系での解釈

| 値 | 用途 | 現在の下位4bit | 新体系での解釈 |
|---|---|---|---|
| `MAGIC_FUNCTION_NATIVE` 0x1 | word0 | 0x1 | **CONS ポインタ**に見える |
| `MAGIC_FUNCTION_INTERPRETED` 0x2 | word0 | 0x2 | character 即値に見える |
| `MAGIC_PROCESS` 0x3 | word0 | 0x3 | **SYMBOL ポインタ**に見える |
| `MAGIC_MACRO` 0x4 | word0 | 0x4 | single-float 即値に見える |
| `MAGIC_BLOCK_EXIT` 0x5 | word0 | 0x5 | **STRING ポインタ**に見える |
| `MAGIC_STREAM` 0x6 | word0 | 0x6 | 未割当の即値 |
| `MAGIC_CLASS_INSTANCE` 0x8 | word0 | 0x8 | 未割当の即値 |
| `MAGIC_CATCH_EXIT` 0x9 | word0 | 0x9 | **RAW_POINTER** に見える |
| `MAGIC_GO_EXIT` 0xA | word0 | 0xA | 未割当の即値 |
| `MAGIC_BIGNUM` 0xB | word0 | 0xB | **未割当ポインタ** |
| `MAGIC_VECTOR` 0xC | word0 | 0xC | 未割当の即値 |
| `MAGIC_FLOAT` 0xD | word0 | 0xD | **未割当ポインタ** |
| `MAGIC_BUILTIN_CLASS` 0xE | word0 | 0xE | 予約（poison）に見える |
| `MAGIC_STANDARD_CLASS` 0xF | word0 | 0xF | **`TAG_FORWARD` と衝突** |
| `GC_DEBUG_TRAP_PATTERN_VALUE` `0xDEADDEADDEADDEA7` | 塗り潰し | 0x7 | **INSTANCE ポインタ**に見える |
| `MAGIC_MUST_BE_BELOW` 0x1000 | 不変条件の上限 | — | 値の比較のみ。タグとは無関係 |

### 5.2 設計: `MAGIC_*` の下位4bitを `0xE` に揃える

```c
/* 下位4bitを予約タグ TAG_RESERVED(0xE) に固定する。
   こうすると MAGIC 値は「どの有効なタグとしても解釈されない」ビット列になり、
   word0 を値として誤読したときに必ず不正値として弾ける。 */
#define MAGIC_FUNCTION_NATIVE      0x0EULL
#define MAGIC_FUNCTION_INTERPRETED 0x1EULL
#define MAGIC_PROCESS              0x2EULL
...
#define MAGIC_STANDARD_CLASS       0xDEULL
```

- 14個で `0xDE` まで。`MAGIC_MUST_BE_BELOW`(0x1000) に収まる
- `_Static_assert((MAGIC_X & TAG_MASK) == TAG_RESERVED)` を各 MAGIC に付けて、
  新しい MAGIC を足した人が外せないようにする
- `GC_DEBUG_TRAP_PATTERN_VALUE` も `0xDEADDEADDEADDEAE` にする

### 5.3 連動が必要な箇所

`interrupt.c:765` が例外ダンプで塗り潰しパターンを見分けている:

```c
if ((regs[i] & ~(uint64_t)0x7) == (0xDEADDEADDEADDEA6ULL & ~(uint64_t)0x7)) {
```

`~0x7` でタグを落として比較しており、**タグ幅を4bitにすると `~0xF` にする必要がある**。
かつ比較対象の定数も連動する。`TAG_MASK` から導出する形に直すべき箇所である
（PR #75 が za.c に対してやったことの、interrupt.c 版）。

### 5.4 範囲検査は廃止できない

`TAG_STRING` のオブジェクトは **word0 が生の長さ**である（`runtime.c:1262` 付近）。
長さは Lisp プログラムが決める任意の整数なので、**下位4bitを制御できない**。
長さが `…F` で終わる文字列は必ず `TAG_FORWARD` と同じ下位4bitを持つ。

したがって「word0 の下位4bitが `TAG_FORWARD` か」だけでは転送済みを判定できず、
**転送先が To空間の範囲内かという検査が引き続き本質的に必要**である。
5.2 の値の振り直しは「制御できるものを衝突させない」ための改善であって、
範囲検査の代わりにはならない。

代案として「STRING にもヘッダワード（長さ＋型）を持たせて、生の長さを word0 に
置かない」構造変更があるが、本フェーズの範囲外とする。

---

## 6. Step 5: for 間欠バグとの関係

### 6.1 現行3bit体系での衝突の有無

`TAG_FORWARD` = `0x6` と下位3bitが一致する MAGIC は**2つある**。

| 値 | 下位3bit |
|---|---|
| `MAGIC_STREAM` 0x6 | 6 |
| `MAGIC_BUILTIN_CLASS` 0xE | 6 |

加えて `TAG_STRING` の word0（生の長さ）が 6 mod 8 のとき（長さ 6, 14, 22, …）も一致する。

これは `runtime.h:79-89` と `gc_copy_value` のコメントに**既に記録されている既知事項**で、
弾いているのは転送先アドレスの**範囲検査の下限**である。

### 6.2 実害があるか

**無い。偽陽性・偽陰性のどちらも起こりえない。**

- **偽陽性**（本物でないものを転送済みと誤認する）: 誤認するには word0 の値が
  `[g_to_start, g_to_end)` に入る必要がある。MAGIC は最大 `0xF`、
  `MAGIC_MUST_BE_BELOW` が `0x1000` を強制し、さらに `os_heap_init` が
  `heap_base <= 0x1000` なら `os_panic` する（`runtime.c:1075`）。
  実測のヒープ先頭は `0x1B8C000`（約28MB）なので、**5桁以上離れている**。
  STRING の長さについても、長さがヒープ先頭アドレス（数十MB）を超えることは
  ヒープ容量上ありえない。
- **偽陰性**（本物の転送を見逃す）: 転送先は必ず `gc_to_alloc` の返り値なので
  範囲内にある。上限に**動かない `g_to_end`** を使っているため境界でも外れない。
  （上限が `g_to_ptr` だった頃は `fwd_addr == g_to_ptr` で実際に外れており、
  それは修正済み。`gc_copy_value` のコメントに実測付きで記録されている）

### 6.3 for 間欠バグとの関係

**候補を1つ消せる。** 番兵値・`MAGIC_*` と `TAG_FORWARD` の下位ビット衝突は、
現行体系において**症状を生みえない**。したがって for 間欠バグの原因ではない。

PR #72 で「タグ表の誤り」を候補から外したのに続き、本調査で
「番兵値と forwarding pointer の衝突」も外れる。
**タグ表現まわりの候補は、これで一通り潰れたことになる。**

本件で別途の修正 PR は不要である（5.2 の値の振り直しは、
現行のバグ修正ではなく**4bit化に伴う設計**として行う）。

---

## 7. Step 6: 実装フェーズの分割案とテスト設計

### 7.1 分割案

**「1つのPRの中で必ず全テストが通る」形に分割できる。ただし第2段階だけは
まとめて変更する必要がある。**

#### 第1段階: 準備（挙動を変えない）

| 内容 | 検証 |
|---|---|
| `lisp.c:82` の死んだ分岐を `(k & TAG_MASK) == TAG_FIXNUM` に直す | 既存テスト |
| ALIGN_AUDIT のタグ添字配列を `[16]`、ループ上限を `TAG_MASK+1` から導出 | ALIGN_AUDIT の出力が変わらないこと |
| `interrupt.c:765` の `~0x7` を `TAG_MASK` から導出 | 既存テスト |
| Lisp 側に `most-positive-fixnum` / `most-negative-fixnum` を導入し、テストの直書き30箇所をそれ経由へ | **既存テストが同じ結果で通ること**（値はまだ 2^60−1） |
| `MAGIC_*` を下位4bit=0xE へ振り直し＋`_Static_assert` | 既存テスト全部（`MAGIC` は内部値なので外から見えない） |

この段階で**タグ幅はまだ3bit**。全テストが通る。
ロールバック単位は各項目ごと。

#### 第2段階: タグ幅の変更（**分割不可**）

次の4つは**同時でないと動かない**:

1. `TAG_MASK` を `0xF` に、各 `TAG_*` を新しい値に
2. `FIXNUM_VALUE_SHIFT` を 4 に、`FIXNUM_MAGNITUDE_MASK` を `(1<<59)-1` に
3. `CHAR_VALUE_SHIFT` を 32 に、`os_make_char` の引数を `UINT32` に
4. JIT: `TAG_MASK` から自動追随するので**コード変更は不要**（PR #75 の成果）。
   ただし `_Static_assert` が新しい値で成立することの確認が要る

**なぜ分割できないか**: タグ値とシフト量は同じ64bit語を分け合っており、
片方だけ変えると即値とポインタの境界が食い違う。JIT の untag も同じ語を見ている。

| 検証 | 内容 |
|---|---|
| `make test` | fixnum 境界テストは `FIXNUM_MAGNITUDE_MASK` 参照なので自動追随 |
| `make test-qemu`（96M / 256M） | Lisp テストは第1段階で定数経由にしてあるので自動追随 |
| **JIT ダンプの比較** | **一致しない**（タグ値が変わるので当然）。ここでは「差分が出る」ことではなく、**差分が T1 の untag マスクと即値定数だけに閉じている**ことを確認する |
| **変異テスト** | `JIT_IMM8_UNTAG_MASK` の変異で変化するのが9箇所のままであること（PR #75 の数と一致） |
| ALIGN_AUDIT | `violations=0` のままであること。**16byte境界が4bitタグの前提**なので、ここが崩れたら即座に分かる |
| 新規テスト | 7.3 の全項目 |

ロールバックはこの段階まるごと。

#### 第3段階: character の Unicode 化（第2段階と分けられる）

`CHAR_VALUE_SHIFT` を 32 にするところまでは第2段階に含めるが、
**`%%CODE-CHAR` の範囲検査**と**UTF-8 の入出力**は別 PR にできる。
文字列の文字単位化（`elt` / `length` の意味）はさらに別フェーズ。

#### 第4段階: 空きタグの活用（任意）

single-float の導入、double-float / ratio のポインタタグ割り当てなど。
第2段階が済んでいれば独立して進められる。

### 7.2 各段階での検証手段のまとめ

| 手段 | 第1段階 | 第2段階 | 第3段階 |
|---|---|---|---|
| `make test` | 同じ結果 | 同じ結果 | 同じ結果 |
| `make test-qemu` 96M/256M | 同じ結果 | 同じ結果 | 同じ結果 |
| 強制GC（gc-stress 1000） | 同じ結果 | 同じ結果 | 同じ結果 |
| 塗り潰し監査22試験 | 同じ結果 | 同じ結果 | 同じ結果 |
| ALIGN_AUDIT `violations` | 0 | **0**（最重要） | 0 |
| JIT ダンプ | **完全一致** | 差分の内訳を確認 | 完全一致 |
| 変異テスト | 9 / 1 件 | 9 / 1 件 | 9 / 1 件 |

### 7.3 新体系が正しいことを確認するテスト設計

既存テストに無い、新規に要るもの。

#### T-1: 全タグのエンコード→デコード往復

```
すべての TAG_* について:
  - 即値タグ: 代表値をエンコードしてデコードし、元に戻ること
  - ポインタタグ: 16byte境界のアドレスにタグを付けて外し、元に戻ること
```

#### T-2: 型述語の排他性

```
16個のタグ値すべてについて、代表値を作り、
全型述語(consp/symbolp/stringp/characterp/integerp/floatp/...)を通す。
**真になる述語がちょうど1つ**であることを確認する。
```

現行は単一タグ等値比較なので自明に見えるが、
4.4 で character のペイロード位置が変わるため、
`characterp` が「タグだけ見る」ままであることの確認になる。

#### T-3: fixnum の境界値

選択肢A（±(2^59−1)）の場合:

| ケース | 期待 |
|---|---|
| `most-positive-fixnum` = 576460752303423487 | `fixnump` が真 |
| `(+ most-positive-fixnum 1)` | bignum へ昇格 |
| `(- (+ most-positive-fixnum 1) 1)` | fixnum へ降格し、元の値に戻る |
| `most-negative-fixnum` = −576460752303423487 | `fixnump` が真 |
| `(- most-negative-fixnum 1)` | bignum へ昇格 |
| `(* 2 (/ (+ most-positive-fixnum 1) 2))` | 乗算の境界 |
| `(quotient (* k k) k)` where k = most-positive-fixnum | 除算の境界（bignum 経由で戻る） |
| `(isqrt (* k k))` = k | 既存の 914 行のテストと同型 |

**JIT 版も同じケースを通す**こと（`za_test` 系）。JIT の高速路は
C の高速路とは別実装なので、境界で食い違う可能性がある。

#### T-4: 即値とポインタの判定（bit0）

```
16個のタグ値すべてについて、bit0 が
  - 0 なら os_tag_is_heap_ref が偽
  - 1 かつ RAW_POINTER でなければ真
であることを確認する。
```

これは `os_tag_is_heap_ref` の単一の真実源としての性質を守るテストである。

#### T-5: GC を挟んだ即値の保存

```
fixnum / character / (将来の single-float) を大量に作り、
consリストに保持したまま GC を複数回発火させ、値が壊れないこと。
```

即値は `gc_copy_value` が素通しするので原理的に壊れないが、
**新しいタグを「ヒープ参照」と誤判定した場合**に壊れる。
`os_tag_is_heap_ref` の誤りを捕まえるテストになる。

#### T-6: character のペイロード

```
コードポイント 0x00 / 0x7F / 0x80 / 0xFF / 0x100 / 0xD7FF / 0xE000 / 0x10FFFF
について char-code → code-char の往復が値を保つこと。
0x110000 と 0xD800-0xDFFF は <domain-error> になること。
```

**0x80 以上を含めるのが要点**である。現行の符号拡張問題（4.4）が
再発していないことの検査を兼ねる。

---

## 8. 未確定事項と、判断が必要な選択肢

| # | 論点 | 選択肢 | 推奨 |
|---|---|---|---|
| 1 | **fixnum の表現** | A: 符号絶対値のまま ±(2^59−1) / B: 2の補数 −2^59〜2^59−1 | **A**。JIT 高速路が無改造で生きる。B を採るなら別フェーズ（4.1） |
| 2 | **`<float>` の扱い** | a: binary64 のままヒープ、`0x4` は予約 / b: `<float>` を binary32 即値に | **a**。b は17桁のテストが落ちる（4.3） |
| 3 | **character のペイロード幅** | 32bit（bit32-63）/ 21bit で足りる | 32bit。`UINT32` をそのまま置けて符号拡張の問題が消える |
| 4 | **文字列の文字単位化** | 本フェーズでやる / 別フェーズ | **別フェーズ**。`elt`/`length` の意味が変わる大きな判断（4.4） |
| 5 | **空きタグ `0xB`/`0xD`** | 空けておく / vector・bignum を昇格させて型判定を速くする | 空けておく。double-float と ratio はどちらも即値にできない（2.3） |
| 6 | **`MAGIC_*` の振り直し** | 下位4bit=0xE に揃える / 現状の連番のまま | **揃える**。`TAG_FORWARD` との衝突が構造的に消える（5.2） |
| 7 | **`TAG_FORWARD` の値** | `0xF` / 他のポインタ値 | `0xF`。MAGIC を振り直せば衝突しない。STRING の長さとの衝突は値に依らず残る（5.4） |
| 8 | **STRING のヘッダ** | 生の長さのまま / 型付きヘッダを持たせる | 本フェーズの範囲外。範囲検査が本質的に必要な理由なので、将来の検討課題として記録（5.4） |
| 9 | 未割当の即値 `0x6`/`0x8`/`0xA`/`0xC` | 用途未定 | 未定のままでよい。`os_tag_is_heap_ref` が偽を返すことだけ T-4 で保証する |
