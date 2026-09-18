# `os_make_char` への符号付き `char` の受け渡しをコンパイル時に検出する

## 0. 基準点

| | |
|---|---|
| ブランチ | `feature/tag4-step2`（PR #78） |
| **着手時点の先頭** | **`1f83fccd0d78f7d1928aeed264b4da90531e2ac7`**（`1f83fcc`） |

本フェーズの「変更前と同一」はすべてこの commit との比較である（main ではない）。

前提: PR #78 §9-6。`os_make_char` の引数を `UINT32` にしたので符号拡張そのものは
解消したが、呼び出し側が `char` を渡すと**暗黙変換で符号拡張し、コンパイラは
既定では警告しない**。

---

## 1. Step 0 の確認結果

### 1-1. 呼び出し箇所と渡している式の型

`os_make_char` の呼び出しは**製品コード8箇所・テスト約40箇所**。
製品コードの内訳は以下のとおりで、**符号付きの型を渡している箇所は無い**
（PR #78 で4箇所を `UINT8` 経由に直した結果）。

| 箇所 | 渡している式 | 型 |
|---|---|---|
| `reader.c:653` | `(UINT8)buf[0]` | `UINT8` |
| `reader.c:656` / `659` / `662` | `' '` / `'\n'` / `'\t'` | **`int`**（Cの文字リテラル） |
| `stream_lisp.c:205` / `411` | `(UINT8)ch` | `UINT8` |
| `runtime.c:7387` / `7461` | `bytes[idx]`（`bytes` は `UINT8*`） | `UINT8` |

テスト側は文字リテラル（`int`）と `0x41` / `0x80u` / `0x10FFFFu` 等の整数定数のみ。

### 1-2. 関数ポインタとしての使用

**無い。** `os_make_char` が `(` を伴わずに現れるのは、コメント3箇所だけである
（`runtime_test.c:199` / `1814`、`za.c:936`）。したがってマクロ化しても壊れる
呼び出しは存在しない。

### 1-3. AOT ジェネレータ

**出力している。** `transpile.lisp:993` が

```lisp
(format nil "os_make_char(~A)" (c-char-literal expr))
```

を出す。`c-char-literal`（`transpile.lisp:357`）は `'.'` のような
**Cの文字リテラル**を作る。生成物 `lisp_compiled.c` に現れるのは実際に
`os_make_char('.')` と `os_make_char('~')` の2種だけである。

**Cの文字リテラルは `char` ではなく `int` 型**なので、`_Generic` の
`default` に落ちて通る。生成コードの変更は不要。

### 1-4. JIT

**JIT は `os_make_char` を呼ばない。** `za.c:936-943` は、リーダが
パース時に構築済みの `TAG_CHAR` 値をそのまま `movabs` の即値として
埋め込むだけである（ヒープ確保を伴わない純粋なビットパックなので
GCで動かない、という理由）。

```c
if ((form & TAG_MASK) == TAG_CHAR) {
    out->is_literal = 1;
    out->literal = form;   /* reader.c が os_make_char で作った値そのもの */
    return 1;
}
```

つまり JIT が埋め込む character も、**元をたどれば reader.c の
`os_make_char` が作っている**。C側のガードはここにも効くので、
本フェーズで JIT 側に対処すべきものは無い。

---

## 2. 採用した案と理由

**案A（`_Generic` で符号付きの型を弾く）を採った。**

```c
lisp_val_t os_make_char_from_code(const UINT32 code);

__attribute__((error("os_make_char: pass an unsigned code point, not a signed char. Cast via UINT8 (e.g. os_make_char((UINT8)ch)) or use UINT32.")))
lisp_val_t os_make_char_rejects_signed_char(int);

#define os_make_char(code) _Generic((code), \
    char:        os_make_char_rejects_signed_char, \
    signed char: os_make_char_rejects_signed_char, \
    default:     os_make_char_from_code)(code)
```

理由:

- **呼び出し側の変更が0箇所で済む。** 1-1 のとおり既に全箇所が通る型なので、
  案B（構造体で包む）のように呼び出しを書き換える必要がない。
  AOT ジェネレータにも手を入れずに済む（1-3）
- **`_Static_assert` を各所に置く既存の書き方となじむ。** このコードベースは
  「不変条件はコンパイル時に落とす」方針で、`runtime.h` に25本以上の
  `_Static_assert` がある。同じ場所・同じ考え方で完結する
- 案C（`-Wsign-conversion` を範囲限定で有効化）は、`#pragma` を置いた
  ファイル全体の既存コードに警告が出るうえ、**警告であってエラーではない**。
  「弾く」という目的に対して緩い

### `__attribute__((error(...)))` はこの環境で効いた

指示書のとおり最初に確認した。**両方のツールチェーンで効く。**

| ツールチェーン | 条件 | 結果 |
|---|---|---|
| `x86_64-w64-mingw32-gcc`（実機ビルド） | `-O1` | **エラーになる** |
| `gcc`（`make test` のホストビルド） | 最適化指定なし（`-O0` 相当） | **エラーになる** |

`-O0` でも効くのは、この attribute が「最適化で消えずに残った呼び出し」を
エラーにする仕掛けで、最適化しなければ呼び出しは必ず残るためである。
したがって**リンクエラーに落とす代替案（宣言だけして定義しない）は不要**だった。

メッセージは ASCII で書いた（PR #74 で確認したとおり、GCC は非 ASCII を
8進エスケープで出力して読めなくなる）。

### `char` が符号なしの環境について

指示書の方針どおり、**符号の有無にかかわらず `char` と `signed char` の両方を弾く**。
`char` が符号なしの環境では弾く必要がないが、環境によって通ったり弾かれたりする
ほうが厄介なためである。通したい場合は `(UINT8)` を噛ませる。

---

## 3. `int` の扱いについての判断

**`int` は弾かない。**

理由は2つある。

1. **Cの文字リテラル（`'A'`）が `int` である。** 弾くと `reader.c` の3箇所と
   AOT 生成コードが全滅する（1-1 / 1-3）。文字リテラルを書けなくするのは
   API として明らかに行き過ぎである
2. **`getchar()` 系の戻り値をそのまま渡す書き方を壊さない**（指示書 Step 2 の
   懸念そのもの）。この種の関数は EOF を負値で返すため戻り値が `int` であり、
   呼び出し側が EOF を弾いたうえで渡す、という形が普通である

残る穴は「負の `int` を渡す誤用」である。これは弾けない。ただし

- `char` を渡す誤用（**暗黙変換で起きるので気付きにくい**）は弾けている
- 負の `int` を渡すのは**明示的に負値を書くか、負になりうる計算結果を
  渡すか**であり、`char` の暗黙変換より目に付きやすい

ので、現時点では釣り合っていると判断した。実行時検査を入れれば塞げるが、
それは挙動の変更であり本フェーズの対象外（指示書 2章）。

---

## 4. ネガティブテストの結果

`runtime.h` を実際に include する一時ファイルで、型ごとにコンパイルを試した
（`x86_64-w64-mingw32-gcc -O1`）。**この確認用ファイルはコミットしていない。**

| 渡した型 | 期待 | 結果 |
|---|---|---|
| `char` | 弾く | **弾かれた** |
| `signed char` | 弾く | **弾かれた** |
| `const char` | 弾く | **弾かれた**（`_Generic` は上位の修飾子を落とすため `char` に一致する） |
| `unsigned char` | 通す | 通った |
| `UINT8` | 通す | 通った |
| `UINT32` | 通す | 通った |
| `UINT64` | 通す | 通った |
| `int` | 通す | 通った（3章の判断どおり） |
| 文字リテラル `'A'` | 通す | 通った |
| `(UINT8)c`（`c` は `char`） | 通す | 通った |

弾かれたときのメッセージ（ASCII、そのまま読める）:

```
error: call to 'os_make_char_rejects_signed_char' declared with attribute error:
os_make_char: pass an unsigned code point, not a signed char.
Cast via UINT8 (e.g. os_make_char((UINT8)ch)) or use UINT32.
```

何をすべきか（`(UINT8)` を噛ませる）が文面に入っている。

---

## 5. 挙動が変わっていないことの検証

基準は `1f83fcc`。

### 5-1. テスト

| 項目 | `1f83fcc` | 本フェーズ後 |
|---|---|---|
| `make test` | 8218 OK / 0 NG | **8218 OK / 0 NG** |
| `make test-qemu` 256M | 3283 passed, 0 failed | **3283 passed, 0 failed** |
| `make test-qemu` 96M | 3283 passed, 0 failed | **3283 passed, 0 failed** |

### 5-2. ALIGN_AUDIT

| | 256M | 96M |
|---|---|---|
| `violations` | **0** | **0** |
| VIOLATION 行 | **0** | **0** |
| gc-count | 14（基準と同じ） | 73（基準と同じ） |
| `gc-live peak` | 5,623,504（基準と同じ） | 5,623,504（基準と同じ） |

タグ別ヒストグラムも基準と同一（CONS→1 / SYMBOL→3 / STRING→5 /
INSTANCE→7 / RAWPTR→9、いずれも misaligned=0、first-bad=0）。

### 5-3. JIT の出力

`JIT_DUMP=1` で基準（`1f83fcc`）と本フェーズ後をそれぞれ 256M で起動し、
関数ごとの `size` / `maskedbytes` / `hash` を突き合わせた。

| | `1f83fcc` | 本フェーズ後 |
|---|---|---|
| コンパイルされた関数 | 1038 | 1038 |
| **size / maskedbytes / hash の3列** | **全1038関数で完全一致** | |
| 合計バイト数 | 1,655,605 | 1,655,605 |

**ハッシュまで含めて完全一致**である。JIT は `os_make_char` を呼ばない（1-4）
ので当然の結果だが、確認した。

### 5-4. 生成コードの差はシンボル名だけ

`os_make_char` の実体を `os_make_char_from_code` へ改名したので、
その影響だけに収まっているかを確認した。`os_make_char` を含む3ファイルを
基準と本フェーズ後でそれぞれコンパイルし、逆アセンブルを突き合わせた。

| オブジェクト | 逆アセンブルの差分行数 | 内容 |
|---|---|---|
| `runtime.o` | **2** | 関数ラベルが `<os_make_char>` → `<os_make_char_from_code>` になっただけ。命令列は同一 |
| `reader.o` | **0** | — |
| `stream_lisp.o` | **0** | — |

シンボル表の差も改名だけである。

| オブジェクト | `1f83fcc` | 本フェーズ後 |
|---|---|---|
| `runtime.o` | `T os_make_char` | `T os_make_char_from_code` |
| `reader.o` | `U os_make_char` | `U os_make_char_from_code` |
| `stream_lisp.o` | `U os_make_char` | `U os_make_char_from_code` |

`reader.o` / `stream_lisp.o` の命令列が1バイトも変わっていないことから、
**マクロ展開後の呼び出しは元の直接呼び出しと同じコードになっている**
（`_Generic` はコンパイル時に解決され、実行時のコストは無い）。

### 5-5. 塗り潰し監査

`AUDIT_STRESS=1000`（実用上限）で22試験。素の実行ではGCを跨いだアサーションが
0件で実質検査にならないことが PR #78 で判明しているため、stress で回す。

| | 基準(`1f83fcc`、PR #78 で実測) | 本フェーズ後 |
|---|---|---|
| OK / FAIL | 22 / 0 | **22 / 0** |
| GCを跨いだアサーション | 479 / 1938（24.7%） | **479 / 1938（24.7%）** |
| trap回数 | 全試験0 | **全試験0** |
| trap箇所 | 全試験0 | **全試験0** |

件数・カバレッジ・trap の有無まで基準と一致した。

---

## 6. Step 3: 同種の危険がある関数の洗い出し

**本フェーズでは修正しない。** 件数と危険度のみ記録する。

### 6-1. 最も件数が多い形: `>> FIXNUM_VALUE_SHIFT` の生シフト

`os_fixnum_magnitude` は符号bitをマスクで落とす。

```c
UINT64 os_fixnum_magnitude(lisp_val_t val) {
    return (val >> FIXNUM_VALUE_SHIFT) & FIXNUM_MAGNITUDE_MASK;   /* 符号bitが落ちる */
}
```

一方、**生のシフトだけで値を取り出している箇所が35箇所ある**。

| ファイル | 件数 |
|---|---|
| `src/c/subprimitive.c` | 17 |
| `src/c/runtime.c` | 15 |
| `src/c/ide_subprimitive.c` | 3 |

`lisp_val_t` は `UINT64`（`types.h:23`）なので `>>` は論理シフトである。
fixnum は符号＋絶対値で**符号が bit63** にあるため、負の fixnum を生シフトすると
**符号bitが値の最上位へ流れ込み、巨大な正の数になる**。

例: Lisp の `-5` は `0x8000000000000050`。`>> 4` で `0x0800000000000005`
= 576,460,752,303,423,493。

**これは本段階（4bit化）で生まれたものではない。** 3bitのころも
`>> 3` で `0x1000000000000005` になっており、形も危険度も同じである。

### 6-2. 危険度の内訳

| 危険度 | 内容 | 代表箇所 |
|---|---|---|
| **中** | **確保サイズ**に流れ、境界検査が無い。負の長さが巨大な確保要求になる | `primitive_create_string`(`runtime.c:7353`)、`primitive_create_vector`(`7076`)、`primitive_make_array`(`7162`/`7165`) |
| 低 | **添字**に流れるが、直後に `idx >= len` / `idx >= dim` で弾かれる。エラーメッセージが「範囲外」になるだけで、メモリ安全性は保たれる | `primitive_elt`(`7494`)、`primitive_set_elt`(`7568`)、`array_offset`(`7199`)、`char_index`/`string_index` の `start`(`6597`/`6624`) |
| 低 | 診断・デバッグ用の組み込み（`%%DIAG-*`）。外から負値を渡せるが影響範囲が計測値に閉じる | `cc_diag_*` 13関数 |

**中の3件について補足**: `make-array` は `total *= dims[i]` で乗算するため、
複数次元に負値を混ぜると**積が UINT64 で巻き戻り**、小さい領域を確保したまま
ヘッダには巨大な次元が入る。その後 `array_offset` の `idx >= dim` は
巨大な `dim` に対して通ってしまうので、**確保外への書き込みになりうる**。
`create-string` / `create-vector` は巻き戻りが起きないので、
巨大な確保要求 → OOM という制御された失敗に落ちると見込まれる。

**この見込みは実行で確認していない**（負値を渡すと `os_panic` がユニットテスト
ビルドで無限ループする既知の性質があり、確認自体に手当てが要るため）。
別フェーズで実際に走らせて確かめること。

### 6-3. 符号なし引数を取る構築関数

`os_make_char` と同じ「暗黙変換で符号拡張する」形のものは他に無い。
符号なし引数を取る構築関数は以下だが、いずれも**引数が文字コードのような
小さい値ではなく、呼び出し側が符号付きを渡す動機が無い**。

| 関数 | 引数 | 評価 |
|---|---|---|
| `os_make_fixnum(UINT64)` | マグニチュード | 呼び出し153箇所。減算を渡す唯一の箇所（`runtime.c:4877`）は直前に `mag_b <= mag_a` の検査があり負にならない |
| `os_make_instance(UINT64 magic, ...)` | MAGIC と生ワード | 値は定数か生ポインタ |
| `os_make_vector_raw(UINT64 count, ...)` | 要素数 | 6-2 の中に含まれる |
| `os_make_array_from_nested_list(UINT64 rank, ...)` | 次元数 | 呼び出し2箇所、いずれも定数 |
| `os_alloc_raw(UINT64 n)` / `os_alloc_bytes(UINT64 n)` | バイト数 | 6-2 の中に含まれる |
| `os_make_integer(int sign, UINT64 *limbs, UINT64 count)` | limb数 | 符号は別引数で明示的 |

### 6-4. 別フェーズにするかの判断材料

- 6-2 の「中」3件は**入力が Lisp プログラム由来**（`(create-string -5)` と書ける）
  なので、ユーザが踏める。優先度は高め
- 直し方は2通り: (a) 各箇所で `os_fixnum_is_negative` を見てエラーにする、
  (b) 「fixnum から長さを取り出す」専用のヘルパを作って35箇所を通す。
  (b) のほうが本フェーズの `os_make_char` と同じ「1箇所に集約して誤用を
  構造的に防ぐ」形になる
- **挙動が変わる**（今はエラーにならない入力がエラーになる）ので、
  本フェーズのような「挙動を変えない」枠では扱えない
