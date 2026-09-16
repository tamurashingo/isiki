# 調査: 型システムの現状 (Phase 4 準備)

> 調査日: 2026-09-16 / ブランチ: `feature/type-system-survey`(分岐元 `feature/compiler-optimization` `55213f0`)
> 関連: `documents/inline-builtin.md`(Phase 3)、`documents/jit-immediate-operands.md`(PR #64)、
> `documents/jit-pinned-register.md`(PR #65)、`documents/bench-pinned-nil.md`(PR #66)
>
> **これは調査であり実装ではない。** コードは一切変更していない。

---

## 0. 結論(先に3つ)

### (1) fixnum / bignum の区別は**すでに内部表現として存在する**

`fixnum` はタグ付き即値(下位3bit=`000`、ヒープ確保なし)、`bignum` はヒープ上の
`TAG_INSTANCE`(`MAGIC_BIGNUM`)。境界は **符号ビット1 + マグニチュード60bit**。
昇格・降格はどちらも自動で、`os_make_integer` が結果を見て毎回選び直す。

**したがって Phase 4 は「表現の追加」ではなく「型宣言でその区別をコンパイル時に確定させる」話である。**
作業規模は当初の想定どおりでよい。

### (2) `+` / `-` の fixnum 高速 path と**オーバーフロー検査は JIT にすでに入っている**

`za_emit_arith_call_or_inline`(`src/c/za.c:2486`)が、生のタグ付き値をそのまま
`add` して**符号ビットが立つかどうか(`js`)でオーバーフローを検出**し、
外れたときだけ `primitive_add2` を呼ぶコードを出している。

**Phase 4 が `+` に対してできるのは「型検査 5 命令を消すこと」であって、
「fixnum 加算を作ること」ではない。** 削れる量は下で実測した(§7)。

### (3) 型判定のコストは**判定手段によって 200 倍違う**

| 判定 | 1回あたり | `fixnump` 比 |
|---|---:|---:|
| `(fixnump x)` | **0.33 µs** | 1.0 |
| `(< a b)` | 0.93 µs | 2.8 |
| `(integerp x)` | 2.9 µs | 8.8 |
| `(class-of x)` | 36 µs | **108** |
| `(typep x '<integer>)` | **64 µs** | **191** |

`typep`/`class-of` は **ILOS クラス階層を実際に歩く Lisp 実装**なので、
「型チェックを省く」の対象として考えてよいのは `fixnump` 相当の 0.33 µs のほうである。

---

## 1. 整数の内部表現(3-1)

### 1-1 fixnum: タグ付き即値、**符号マグニチュード表現**

`src/c/runtime.h:7-47`

```c
#define TAG_MASK     0x7ULL
#define TAG_FIXNUM   0x0ULL        /* 即値の fixnum(000) */
#define FIXNUM_SIGN_BIT       0x8000000000000000ULL   /* bit63 = 符号 */
#define FIXNUM_MAGNITUDE_MASK ((1ULL << 60) - 1)      /* bit3〜62 = 絶対値 */
```

```
 63    62                        3   2 1 0
+---+------------------------------+-------+
| S |        magnitude (60bit)     | 0 0 0 |
+---+------------------------------+-------+
```

- `os_make_fixnum(n)` = `n << 3`(`runtime.h:618`、**static inline**。生成コード中に
  1,513 箇所あるためクロス TU 呼び出しをやめたと注記がある)
- `os_fixnum_magnitude(v)` = `(v >> 3) & FIXNUM_MAGNITUDE_MASK`
- `os_fixnum_is_negative(v)` = `(v & FIXNUM_SIGN_BIT) != 0`
- `-0` は存在しない(`os_make_fixnum_signed` がマグニチュード 0 のとき符号を 0 に正規化)

**[重要] 2の補数ではない。** これは Phase 4 の設計に直接効く(§9-4 参照)。

### 1-2 bignum: ヒープ上の `TAG_INSTANCE`

`src/c/runtime.h:69`

```
word0 = MAGIC_BIGNUM (0xB)
word1 = sign (0:非負 / 1:負)   ← 生の int(タグ無し)
word2 = limb 数                 ← 生の int
word3 = limb 配列への生ポインタ ← 基数 2^32、下位32bitのみ使用、limbs[0] が最下位
```

判別は `is_bignum`(`runtime.c`):

```c
return (val & TAG_MASK) == TAG_INSTANCE && ((UINT64 *)(val & ~TAG_MASK))[0] == MAGIC_BIGNUM;
```

**タグ検査 1 回では済まない**(タグ検査 + 1 メモリロード + 比較 = 実測 4〜5 命令、
`primitive_bignump1` は 8 命令)。一方 **fixnum の判定はタグ検査 1 回で済む**。
`primitive_fixnump1` は分岐すら無い 4 命令:

```asm
primitive_fixnump1:
  test $0x7, %cl
  mov  g_sym_t(%rip), %rax
  cmovne nil(%rip), %rax
  ret
```

### 1-3 境界をまたぐときの挙動: **自動昇格・自動降格**

`os_make_integer(sign, limbs, count)`(`runtime.c`)が唯一の合流点:

```c
if (count <= 2) {
    UINT64 magnitude = limbs[0] | (count == 2 ? limbs[1] << 32 : 0);
    if (magnitude <= FIXNUM_MAGNITUDE_MASK) {
        return os_make_fixnum_signed(sign, magnitude);   /* ← 降格。ヒープ確保なし */
    }
}
/* それ以外は limb をコピーして MAGIC_BIGNUM をヒープに確保 */
```

実測(§7 の測定と同じブート内):

```
#INT 2^59: <INTEGER> / 2^60-1: <INTEGER> / 2^60: <INTEGER>
#INT (+ (- (expt 2 60) 1) 1) の型 = <INTEGER> / 値 = 1152921504606846976
#INT (- (expt 2 60) (expt 2 60)) = 0 型=<INTEGER>
```

**ユーザーから見える型名は境界の前後で変わらない**(どちらも `<INTEGER>`)。
昇格・降格は完全に内部事情である。これは ISLisp 準拠として正しい(§4)。

### 1-4 `(+ a b)` が実行時に通る経路

```
(+ a b)
 │
 ├─[JIT]  za_compile_fold(..., primitive_add2)          za.c:3578
 │         └─ za_emit_arith_call_or_inline               za.c:2486
 │             │
 │             ├─ ① 両方が非負 fixnum か?
 │             │     mov  r10, rcx
 │             │     or   r10, rdx
 │             │     movabs r9, 0x8000000000000007   ; TAG_MASK | FIXNUM_SIGN_BIT
 │             │     test r10, r9
 │             │     jne  →低速                      ; タグ不一致 or どちらか負
 │             │
 │             ├─ ② タグ付き値をそのまま加算(下位3bitは両方0のまま)
 │             │     mov  r10, rcx
 │             │     add  r10, rdx
 │             │     js   →低速                      ; bit63 が立つ = 60bit 超え
 │             │     mov  rax, r10                   ; ★ここで完了(ヒープ確保なし)
 │             │
 │             └─ 低速: call primitive_add2
 │
 └─[低速] primitive_add2(a, b)                          runtime.c:5001
            ├─ 同じ条件(両方非負 fixnum かつ和が60bit以内)を C で再判定 → 即返す
            └─ 外れたら os_make_cons ×2 で引数リストを作って primitive_add へ委譲
                 └─ primitive_add                       runtime.c:4918
                     ├─ any_float(args) → 全部 double にして加算、os_make_float
                     ├─ 全部が**非負** fixnum かつ和が60bit以内 → os_make_fixnum
                     └─ それ以外 → **符号マグニチュードの一般パス**
                          LIMB_FRAME / decompose / limb_alloc /
                          mag_add or mag_sub / mag_compare / os_make_integer
```

**[重要] 一般パスに落ちる条件に「負数」が含まれている。**
`(+ -1 1)` は両方 fixnum で結果も fixnum なのに、`decompose` → `limb_alloc` →
`mag_sub` → `os_make_integer` という bignum 用の機構を丸ごと通る。

実測でこの分岐は **1 回あたり 9.7 倍**(0.63 µs → 6.13 µs)の差になっている(§7-4)。

---

## 2. クラスオブジェクトの表現(3-2)

### 2-1 実体は **GC ヒープ上の `TAG_INSTANCE`**。不動領域ではない

`%%make-builtin-class-raw` → `primitive_make_builtin_class_raw`(`runtime.c:7824`):

```c
return os_make_instance(MAGIC_BUILTIN_CLASS, name, supers, slots);
```

`os_make_instance` は `os_alloc_bytes(32)` で **GC ヒープ(From 空間)から確保**する。
`MAGIC_BUILTIN_CLASS` / `MAGIC_STANDARD_CLASS` のインスタンスは word1(name)・
word2(supers)・word3(slots)がすべてタグ付き値で、`gc_scan_instance` が追いかける。

**→ クラスオブジェクトは GC のたびに動く。**

### 2-2 したがって JIT から即値で参照してはならない(PR #64 の規則そのもの)

`documents/jit-immediate-operands.md` の規則「GC ヒープ上のオブジェクトのアドレスを
焼いてはならない」(pitfalls 原則8)に**そのまま該当する**。
クラスオブジェクトは `TAG_INSTANCE` なので `os_tag_is_heap_ref` が真を返し、
`za_try_compile_defun` 末尾の即値監査が検出する側の値である。

Phase 4 で JIT からクラスを参照する必要が出た場合、取れる手は既存の 2 つ:

1. **シンボル名から実行時に再解決する**(`quote` シンボルと同じ手口。
   コード中に `.asciz "…"` を埋めて `os_make_symbol` を呼ぶ)
2. **静的スロット + `os_gc_register_root`**(`g_za_quote_slots` と同じ手口。
   スロットのアドレスは不動なので即値化してよく、値は都度読み直す)

**ただし §9 の見通しでは、そもそもクラスオブジェクトを JIT から参照しない設計を推す。**

### 2-3 登録の仕組みと階層

クラスは **`(dynamic *classes*)` = 名前→クラスオブジェクトの assoc リスト**として持つ。
`%find-class` は `(cdr (assoc name (dynamic *classes*)))` = **線形探索**。

`src/lisp/init_aot.lisp:352-373` に 22 個の predefined クラスの bootstrap 登録があり、
条件クラス群と FS のクラスは後から `defclass` 等で足される。実測で全 **45 個**:

```
#CLS クラス総数=45
<OBJECT>
 ├ <BASIC-ARRAY> ─ <BASIC-ARRAY*> ─ <GENERAL-ARRAY*>
 │              └ <BASIC-VECTOR> ─┬ <GENERAL-VECTOR>
 │                                └ <STRING>
 ├ <BUILT-IN-CLASS>  <CHARACTER>  <STANDARD-CLASS>  <STANDARD-OBJECT>
 ├ <FUNCTION> ─ <GENERIC-FUNCTION> ─ <STANDARD-GENERIC-FUNCTION>
 ├ <LIST> ─┬ <CONS>
 │         └ <NULL>  (親は <LIST> と <SYMBOL> の2つ)
 ├ <SYMBOL>
 ├ <NUMBER> ─┬ <INTEGER>
 │           └ <FLOAT>
 ├ <STREAM>
 ├ <CONDITION> ─ <SERIOUS-CONDITION> ─ <ERROR> ─┬ <ARITHMETIC-ERROR> ─┬ <DIVISION-BY-ZERO>
 │                                              │                     ├ <FLOATING-POINT-OVERFLOW>
 │                                              │                     └ <FLOATING-POINT-UNDERFLOW>
 │                                              ├ <CONTROL-ERROR>  <PARSE-ERROR>  <SIMPLE-ERROR>
 │                                              ├ <PROGRAM-ERROR> ─ <DOMAIN-ERROR>
 │                                              │                 └ <UNDEFINED-ENTITY> ─┬ <UNBOUND-VARIABLE>
 │                                              │                                       └ <UNDEFINED-FUNCTION>
 │                                              ├ <STREAM-ERROR> ─ <END-OF-STREAM>
 │                                              └ <STORAGE-EXHAUSTED>
 └ (FS) <FILE-NODE> <FAT16-FILE-NODE> <FAT32-FILE-NODE> BPB BPB32
```

実測: `<integer>` の supers = `(<NUMBER>)`、`<null>` の supers = `(<LIST> <SYMBOL>)`。

---

## 3. 型判定の実行時コスト(3-3)

### 3-1 実測(1回あたり、ループ空回し分を引いた値)

QEMU(TCG、KVM 無し)、`get-internal-real-time`、tick = 10ms、200,000 回 × 3 往復。
**同一ブート内での比較**なので run-to-run ドリフトの影響を受けない。

| ベンチ | 往復1 | 往復2 | 往復3 | 平均 | nop 差分 | **1回あたり** |
|---|---:|---:|---:|---:|---:|---:|
| `nop`(ループのみ) | 20 | 20 | 20 | 20.0 | — | — |
| `(fixnump i)` | 24 | 28 | 28 | 26.7 | 6.7 | **0.33 µs** |
| `(< i 999999999)` | 40 | 36 | 40 | 38.7 | 18.7 | 0.93 µs |
| `(integerp i)` | 80 | 76 | 80 | 78.7 | 58.7 | 2.93 µs |
| `(class-of i)` | 736 | 748 | 744 | 742.7 | 722.7 | 36.1 µs |
| `(typep i '<integer>)` | 1284 | 1304 | 1304 | 1297.3 | 1277.3 | **63.9 µs** |

### 3-2 なぜこれだけ違うのか

**`fixnump` — タグ検査。クラス階層を一切辿らない。**
`za_compile_unary` が `primitive_fixnump1` への直接 call を出す
(`za.c:3651`)。生成コードは:

```asm
0162  mov rax, [rsp+0x1cb0]      ; x
016a  mov rcx, rax
016d  mov r13, rax
0170  call os_is_control_transfer     ; ← 制御転送チェック(5命令 + call)
0181  mov r11d, 0x0 / cmp rax,r11 / je
0198  mov rax, r13
019b  mov rcx, rax
019e  call primitive_fixnump1          ; ← 本体はここ。4命令
```

**本体 4 命令に対して、呼び出し規約(`sub rsp,0x20` / `call` / `add rsp,0x20`)と
制御転送チェックの call が前に付く。** Phase 3 の `(car x)` で観測した
「オペランドの制御転送チェック 59 byte が支配的」と同じ構図である。

**`typep` — Lisp 実装。クラス階層を実際に歩く。**

```lisp
(defun typep (instance class-designator)
  (subclassp (class-of instance)
             (if (%%classp class-designator) class-designator (%find-class class-designator))))
```

1 回の `typep` が内側で行うこと:

1. `class-of` = **15 分岐の `cond`**。整数は 11 番目の `(integerp obj)` まで落ちる。
   各分岐の述語は Lisp または primitive 呼び出し
2. その先の `(%find-class '<integer>)` = `(cdr (assoc name (dynamic *classes*)))`
   = **45 要素の assoc リストの線形探索**(`<INTEGER>` は末尾側から 29 番目)
3. `%find-class` をもう 1 回(designator 側)
4. `subclassp` → `%any-subclassp` で supers を再帰的に辿る

**dynamic 変数参照 + assoc 線形探索 + クラス階層の再帰**が全部 Lisp で走る。
36〜64 µs という数字はこの構造から出ている。

### 3-3 Phase 4 が「省ける」のはどちらか

**`typep` の 64 µs は Phase 4 の対象ではない。**
`(+ a b)` が実行時に行っている型検査は §1-4 の
`mov / or / movabs / test / jne` **5 命令 19 byte** であって、`typep` ではない。

したがって **Phase 4 で 1 演算あたり削れるのは 5 命令**(+ オーバーフロー検査 `js` を
落とせればさらに 1〜2 命令)。実測ベースの見積りは §9-1 に置く。

---

## 4. ISLisp への準拠度(3-4)

### 4-1 **ISLisp には `<fixnum>` も `<bignum>` も無い**

ISLisp(ISO/IEC 13816)のクラス階層で整数を表すのは `<integer>` ただ 1 つである。
`<integer>` は「整数という数学的な対象」を表す抽象的なクラスであり、
実装の内部表現(固定幅/多倍長)を公開する場所ではない。

**現在の実装はここを正しく守っている。**

```
#CLS 1=<INTEGER> 2^100=<INTEGER> -1=<INTEGER> 1.5=<FLOAT> #\a=<CHARACTER>
     "s"=<STRING> nil=<NULL> '(1)=<CONS> 'a=<SYMBOL>
```

`(class-of 1)` と `(class-of (expt 2 100))` は**どちらも `<INTEGER>`**。
`<FIXNUM>` / `<BIGNUM>` というクラスオブジェクトは 45 個のどこにも存在しない。

### 4-2 `fixnump` / `bignump` は**仕様外の拡張**

ISLisp が規定する数値述語は `numberp` / `integerp` / `floatp` の 3 つ。
`fixnump` / `bignump` は isiki の追加であり、**述語としては存在するがクラスではない**。
これは「内部表現を述語で覗けるが、クラス階層は汚していない」という位置づけで、
準拠の観点では問題にならない。

### 4-3 実装済みのクラス階層と仕様の差

仕様が要求する階層に対し、**45 個は条件クラス群まで含めてほぼ揃っている**。
`<basic-array>` 系・`<function>` 系・`<number>` 系・条件クラス系はすべて登録済み。

条件クラス群は `src/lisp/init.lisp:513-` の `defclass` で登録されるため
`MAGIC_STANDARD_CLASS`(組み込みクラスではなく standard-class)になっている。
また `<CONDITION>` は**仕様に無い実装独自の基底クラス**で、
`(make-instance '<condition>)` を使う既存テストとの後方互換のために
`<serious-condition>` の親として置かれている(`init.lisp:504-506` に明記)。

一方で `class-of` の実装には既知の簡略化がある(コメントに明記されている):

- `instancep` が `typep` に委譲しており、クラスオブジェクト以外を渡したときに
  `<domain-error>` を出さない
- `%merge-superclass-slots` が同名スロットのオーバーライドを行わない

いずれも Phase 4 とは独立の話である。

---

## 5. 型宣言の受け皿(3-5)

### 5-1 `(declaim (type ...))` は**黙って読み飛ばされる**

`src/c/eval.c` `eval_declaim`:

```c
/* optimize以外の指定子(type/ftype等)は黙って読み飛ばす */
if (kind != g_sym_optimize) {
    continue;
}
```

実測で、`(declaim (type <integer> some-var))` はエラーにならず、`optimize` も変化しない:

```
#DCL (declaim (type ...)) 後の optimize = (1 1 1) (エラーにならない)
```

Phase 2 の設計どおりであり、**Phase 4 はここに分岐を足すところから始まる**。

### 5-2 `the` は**マクロで、型を捨てている**

`src/lisp/init.lisp:402`:

```lisp
;; (the class-name form) : 型を宣言するだけで、実際の型チェックは行わない
;; (spec上、不一致時の動作は未定義なのでno-opで十分)。
(defmacro the (class-name form)
  form)
```

**za は式を評価する位置に立つたびに `macroexpand` を回す**ので、
`(the <integer> x)` は za が見る前に `x` になっている。実測:

```
#FN (the <integer> x) compiled=T len=412 imm=10
```

412 byte は単なるパラメータ参照の関数と同じ大きさで、型情報はどこにも残っていない。

**Phase 4 で `the` を使うなら、マクロをやめて za が認識する特殊形式にする必要がある**
(`za_is_excluded_special_form` に入れて eval 側で処理する、ではなく、
`za_compile_expr_inner` の分岐に足す形)。

### 5-3 `declare` は存在しない

`g_sym_declare` も `"DECLARE"` も C 側・Lisp 側のどちらにも無い。Phase 2 で見送ったまま。
`(defun f (a) (declare (type fixnum a)) ...)` を書けるようにするには、

1. `declare` シンボルを足す
2. **defun の body が 2 式以上になる**ため、§6 の「body は 1 式」制約に当たる
   (declare を body から取り除いてから渡す前処理が要る)

の両方が必要になる。**2 番目は見落としやすい。**

### 5-4 environment に型情報を持たせる場合の見通し

現在の declaim は environment の **10 番目のスロット**に `(declaim . fixnum)` の形で
**単一の fixnum** を持っている(`runtime.c:1583` `declaim_slot_of`)。
中身は 1 個の整数にビットを詰めたもの:

| bit | 用途 |
|---|---|
| 0〜5 | `OPTIMIZE_PACK`(speed / safety / space が 2bit ずつ) |
| 8〜39 | `DECLAIM_INLINE_*`(`car`/`cdr`/`null`/`eq` のインライン許可ビット) |

**名前と型の対応はこの形では持てない。** 型は「どの変数が」という情報を伴うため、
ビットフラグではなく **alist(あるいは 11 番目のスロットに `(types . ((a . fixnum) ...))`)**
が必要になる。

environment のスロットは**末尾に足すだけなら既存コードに影響しない**という設計が
明文化されている(`runtime.c:3376` のコメント。`os_get_variable` 等は 4 番目までしか
固定位置参照しない)ので、11 番目を足すこと自体は安全である。

ただし:

- **frame は 8 スロットしか持たない**(`let`/`flet` のたびに作られる hot path なので
  cons を増やさない設計)。`declaim_slot_of` は `os_definition_env` で frame を読み飛ばす。
  **関数引数の型は environment ではなく frame のスコープに属するので、
  この仕組みをそのまま流用できない。**
- declaim は環境スコープで親を継承しない(Phase 2 の決定)。
  型宣言も同じ規則でよいかは別途決める必要がある

---

## 6. JIT が断念する条件(3-6)

`%%DIAG-ZA-BAIL-AT`(`za.c:5104` の `ZA_BAIL_LINE` が記録する断念行)を使って
**実測で一覧化した**。全 48 箇所の計器のうち、実際に踏んだものを構文と対応づける。

### 6-1 **JIT に乗る**構文(実測、すべて `compiled=T`)

| 構文 | code len | 即値 |
|---|---:|---:|
| `let*` | 802 | 22 |
| `cond` | 668 | 24 |
| `case` | 1572 | 50 |
| `while`(= `block` + `tagbody` + `go`) | 1603 | 54 |
| `&rest` | 206 | 5 |
| `format` などの一般呼び出し | 1977 | 60 |
| `apply` | 2103 | 63 |
| `(funcall (lambda (y) y) x)` | 1490 | 49 |
| `((lambda (y) y) x)` | 395 | 11 |
| `catch` / `throw` | 942 | 30 |
| `unwind-protect` | 719 | 20 |
| `(dynamic *v*)` | 463 | 13 |
| `aref` | 1288 | 38 |
| 文字列リテラル | 1289 | 39 |
| quasiquote | 1009 | 31 |
| `flet` 4 束縛 | — | — |
| 引数 16 個 | — | — |

**`tagbody`/`go` は「常に断念する構文」ではない。** `while` は
`block` + `tagbody` + `go` へ展開されるが 1603 byte で JIT に乗っている。

ただし `tagbody` には**自分専用の断念条件**がある(`za.c:5970`〜`6045`):

| 条件 | 上限 / 内容 |
|---|---|
| タグ数 | `ZA_MAX_TAGBODY_TAGS = 16` |
| form 数 | `ZA_MAX_TAGBODY_FORMS = 64` |
| 1 タグあたりの前方 `go` | `ZA_MAX_TAGBODY_GOTOS_PER_TAG = 8` |
| **未解決の前方参照が残る**(`za.c:6013`) | body 中に無いタグへの `go`。**GC 圧の下では、前方参照として登録したタグシンボルが GC で移動して一致しなくなり、ここに落ちる** |

最後の 1 つは `za.c:5932-5945` にコメントで明記されている既知の挙動で、
`for` が `tagbody`/`go` へ展開され `go` が前方参照を作るため、
**GC 圧下では `for` 系だけが落ちる**(AUDIT_STRESS=100 の実測で 4 件)。
PR #66 のベンチで `tagbody`/`go` が JIT に乗らなかったのも
このどれかだと考えられるが、**今回は再現させて確かめていない。**

### 6-2 **JIT に乗らない**構文と、その原因行

| 構文 | bail 行 | 原因 |
|---|---|---|
| **defun の body が 2 式以上** | `za.c:6176` | `cc_cdr(body) != nil` |
| 引数 17 個 | `za.c:6172` | `za_validate_params` → `ZA_MAX_PARAMS = 16` |
| `flet` 5 束縛 | `za.c:5579` → 6334 | `ZA_MAX_FLET_BINDINGS = 4` |
| 算術 5 段ネスト | `za.c:6334` | `ZA_MAX_ARITH_DEPTH = 4` |
| 呼び出し 5 段ネスト | `za.c:6334` | `ZA_MAX_CALL_DEPTH = 4` |
| `let` に 5 束縛 | `za.c:6334` | `ZA_MAX_LOCALS_PER_LET = 4` |

`za.c:6334` は `za_compile_expr` が 0 を返したときの**伝播点**であって起点ではない。
内側の容量上限(`ZA_MAX_ARITH_DEPTH` 等)には `ZA_BAIL_LINE` が入っていないため、
`%%DIAG-ZA-BAIL-AT 0` は 6334 しか返さない。**どの上限に当たったかは実測では区別できない**
(上の表の「原因」欄は、構文を 1 段ずつ減らして境界を確かめた結果である)。

### 6-3 **最大の断念要因は「body が 1 式でないこと」**

これは実測で切り分けた(構文を 1 段ずつ変えて境界を確かめた):

```
#FN2 body 1式                    compiled=T
#FN2 body 2式(どちらも変数参照)  compiled=NIL bail=6176
#FN2 body 3式                    compiled=NIL bail=6176
#FN2 body (progn x x)            compiled=T
```

**`(defun f (x) a b)` は JIT に乗らないが、`(defun f (x) (progn a b))` は乗る。**
`za_try_compile_defun` は body を `progn` で包み直さない。

`src/lisp/*.lisp` の defun 572 個のうち body が 2 式以上なのは **79 個(13.8%)**、
そのうち 76 個は `transpile.lisp`(ホスト側で JIT 対象外)、init.lisp は 3 個だけ。
**したがって既存コードへの影響は小さい**が、
Phase 4 で `(declare ...)` を導入すると **全部の宣言付き defun がここに当たる**。

### 6-4 常に除外される特殊形式

`za_is_excluded_special_form`(`za.c:1854`):

```
quote / defun / lambda / defmacro / function /
defvar / defconstant / defglobal / declaim
```

body の中にこれらが現れる関数はインタプリタに落ちる。
**`declaim` がここに入っている**ため、`(defun f (x) (declaim ...) ...)` は
二重に(§6-3 の 1 式制約と、この除外で)JIT から外れる。

### 6-5 その他の静的上限(`za.c` の `#define`)

| 定数 | 値 | 意味 |
|---|---:|---|
| `ZA_MAX_PARAMS` | 16 | 固定引数の個数 |
| `ZA_MAX_OPERANDS` | 16 | 1 呼び出しの引数個数 |
| `ZA_MAX_LET_DEPTH` | 16 | `let` のネスト |
| `ZA_MAX_LOCALS_PER_LET` | 4 | 1 つの `let` の束縛数 |
| `ZA_MAX_FLET_BINDINGS` | 4 | `flet`/`labels` の束縛数 |
| `ZA_MAX_NLX_DEPTH` | 8 | 非局所脱出のネスト |
| `ZA_MAX_CALL_DEPTH` | 4 | 呼び出しのネスト |
| `ZA_MAX_ARITH_DEPTH` | 4 | 算術のネスト |
| `ZA_MAX_QQ_DEPTH` | 6 | quasiquote のネスト |
| `ZA_MAX_TAGBODY_TAGS` | 16 | `tagbody` のタグ数 |
| `ZA_MAX_TAGBODY_FORMS` | 64 | `tagbody` の form 数 |
| `ZA_MAX_COMPILE_NEST` | 60 | コンパイル時の再帰段数(PR #60) |
| `ZA_MAX_FIXED_ENTRY_PARAMS` | 3 | 固定引数エントリを作る引数数の上限 |

**Phase 4 のベンチを書くときは `ZA_MAX_ARITH_DEPTH = 4` に注意すること。**
`(+ a (+ a (+ a (+ a (+ a a)))))` は JIT に乗らない。

---

## 7. 算術の現状測定(3-7)

**すべて `%%za-compiled-p` = T を確認済み。**

### 7-1 コードサイズと即値

`%%DIAG-ZA-SCAN`(PR #64 で記録ベースになり誤検出ゼロ)。
領域内訳は `%%DIAG-ZA-IMM-REGION`(0=From / 1=To / 2=Immobilized / 4=その他)。

| 式 | code len | 即値 | From | To | Imm | その他(カーネル関数アドレス・定数) |
|---|---:|---:|---:|---:|---:|---:|
| `(+ a b)` | **739** | 20 | 0 | 0 | 0 | 20 |
| `(- a b)` | 742 | 20 | 0 | 0 | 0 | 20 |
| `(* a b)` | 694 | 19 | 0 | 0 | 0 | 19 |
| `(< a b)` | 694 | 19 | 0 | 0 | 0 | 19 |
| `(= a b)` | 694 | 19 | 0 | 0 | 0 | 19 |
| `(/ a b)` | **1428** | 41 | 0 | 0 | **1** | 40 |
| `(+ a b c)` | 1017 | 28 | — | — | — | — |
| `(fixnump x)` | 481 | 13 | — | — | — | — |
| `(integerp x)` | 1148 | 33 | — | — | — | — |
| `(typep x '<integer>)` | 1326 | 39 | — | — | — | — |
| `(class-of x)` | 1148 | 33 | — | — | — | — |
| `(the <integer> x)` | 412 | 10 | — | — | — | — |

**GC ヒープ(From/To)を指す即値は全関数でゼロ**(原則8 の監査が常時通っている)。

`(/ a b)` だけ Immobilized の即値が 1 つある。これは **`/` が特化されておらず一般呼び出しに
なるため、Function Cell のインラインキャッシュスロット**を 1 つ持つからである
(`+ - * < =` は特化経路でカーネル関数を直接呼ぶのでキャッシュを持たない)。

### 7-2 `(+ a b)` の生成コード

739 byte のうち、**加算の高速 path は 40 byte(`0x273`–`0x29a`、5.4%)しかない。**

| 範囲 | 内容 | 長さ |
|---|---|---:|
| `0x000`–`0x0aa` | fixed entry プロローグ(GC link ×4) | 171 |
| `0x0ab`–`0x1ed` | cons entry プロローグ(`cc_car`/`cc_cdr` で a/b を取り出す) | 323 |
| `0x1ee`–`0x267` | a と b の**制御転送チェック 2 回**(`os_is_control_transfer` への real call ×2) | 122 |
| `0x268`–`0x2b0` | **加算本体**(オペランド移動 + 高速 path 40 byte + 低速 call) | **73** |
| `0x2b1`–`0x2e2` | エピローグ | 50 |

加算本体:

```asm
0268  48 89 c2                 mov rdx, rax            ; b
026b  48 8b 8c 24 30 0f 00 00  mov rcx, [rsp+0xf30]    ; a
0273  49 89 ca                 mov r10, rcx
0276  49 09 d2                 or  r10, rdx
0279  49 b9 07 00 00 00 00 00 00 80   movabs r9, 0x8000000000000007   ; ★10 byte
0283  4d 85 ca                 test r10, r9
0286  0f 85 14 00 00 00        jne 0x2a0               ; ← 型が外れたら低速
028c  49 89 ca                 mov r10, rcx
028f  49 01 d2                 add r10, rdx            ; ★タグ付きのまま加算
0292  0f 88 08 00 00 00        js  0x2a0               ; ← オーバーフローなら低速
0298  4c 89 d0                 mov rax, r10
029b  e9 11 00 00 00           jmp 0x2b1
02a0  41 bb 4d 8c 7a 0c        mov r11d, 0xc7a8c4d     ; primitive_add2
02a6  48 83 ec 20 / 41 ff d3 / 48 83 c4 20              ; call
```

**型検査は `mov / or / movabs / test / jne` の 5 命令 19 byte。**
うち `movabs r9, 0x8000000000000007` は値が 2^32 以上なので PR #64 の短縮が効かず
**10 byte のまま**である。

### 7-3 `(< a b)` は**インライン化されていない**

`za_compile_binary` が `primitive_less_than2` への直接 call を出すだけ:

```asm
0268  mov rdx, rax
026b  mov rcx, [rsp+0xf30]
0273  mov r11d, 0xc7a8f54      ; primitive_less_than2
0279  sub rsp,0x20 / call r11 / add rsp,0x20
```

`primitive_less_than2` は `number_compare` を呼び、その中で fixnum×fixnum の
高速 path(符号比較 + マグニチュード比較)に入る。**比較には fixnum インライン化が無い。**

### 7-4 速度(1,000,000 回 × 4 往復、同一ブート内)

ループは `(defun f (n) (let ((i 0) (s ...)) (while (< i n) <被測定> (setq i (+ i 1))) s))` の形。
`nop` はこの `<被測定>` が無いもの。**差分が被測定 1 回分にあたる。**

| ベンチ | 往復1 | 往復2 | 往復3 | 往復4 | 平均 | nop 差分 | 1回あたり |
|---|---:|---:|---:|---:|---:|---:|---:|
| `nop` | 108 | 108 | 112 | 108 | 109.0 | — | — |
| `(setq s (+ s 1))` | 168 | 168 | 188 | 164 | 172.0 | 63.0 | 0.63 µs |
| `(setq s (- s 1))` | 132 | 136 | 136 | 132 | 134.0 | 25.0 | 0.25 µs |
| `(setq s (* 2 3))` | 200 | 192 | 192 | 196 | 195.0 | 86.0 | 0.86 µs |
| **`(setq s (+ -1 1))`** | 740 | 708 | 700 | 740 | **722.0** | **613.0** | **6.13 µs** |
| `(if (fixnump i) …)` | 136 | 136 | 136 | 136 | 136.0 | 27.0 | 0.27 µs |

**`(+ -1 1)` は `(+ s 1)` の 9.7 倍。** 両方 fixnum で結果も fixnum なのに、
負のオペランドがあるだけで `decompose` → `limb_alloc` → `mag_sub` → `os_make_integer` の
bignum 機構を丸ごと通るためである(§1-4)。**これは Phase 4 とは独立に直せる。**

`*` が `+` より遅い(0.86 vs 0.63 µs)のは、`za_emit_arith_call_or_inline` が
`primitive_multiply2` をインライン化していない(オーバーフロー判定に除算が要る)ため。

**[未解明 → 決着済み] `-`(0.25 µs)が `+`(0.63 µs)より速い理由が説明できていない。**
生成コードの命令数は `-` のほうが 1 つ多く(`cmp/jb/mov/sub` vs `mov/add/js`)、
code len も 742 vs 739 でほぼ同じ。レンジは重なっていないので測定誤差ではない。
TCG のブロック分割か分岐方向の違いが効いている可能性があるが、確認していない。

> **【2026-09-16 追記】この観測は再現しなかった。** `documents/fixnum-signed-fastpath.md` §6。
> ここの 2 本は `(+ s 1)` が `s` を 0 から、`(- s 1)` が `s` を 1,000,000,000 から
> 始めており、**被測定以外が揃っていなかった**。初期値を揃えて測り直すと
> `+` のほうが速く、命令数の大小と一致する。同じ `(- s 1)` を別ブートで測ると
> 0.25 → 0.51 µs と 2 倍動いており、ここの値は実行間ドリフトを拾っていた。
> 「同一ブート内」だけでは足りず、**ループの形を完全に揃える**ことが条件になる。

---

## 8. 報告必須項目のまとめ(4-1)

| 項目 | 結論 |
|---|---|
| fixnum / bignum の区別 | **既にある。** fixnum = タグ付き即値(符号1bit + 60bit マグニチュード)、bignum = `MAGIC_BIGNUM` のヒープオブジェクト。昇格・降格とも自動 |
| `(+ a b)` の経路 | JIT が `test`(型ガード)→ `add` → `js`(桁溢れガード)の **2 段のガード**でインライン処理。外れたら `primitive_add2` → `primitive_add` → 符号マグニチュード一般パス |
| クラスオブジェクトの場所 | **GC ヒープ上**(`os_make_instance` → `os_alloc_bytes`)。移動する。JIT から即値で焼いてはならない |
| 型判定のコスト | `fixnump` = 0.33 µs(本体 4 命令 + 呼び出し規約 + 制御転送チェック)、`typep` = 63.9 µs(**191 倍**) |
| JIT が断念する条件 | §6。**最大は「defun の body が 1 式でないこと」**。あとは容量上限(引数 16・let 束縛 4・flet 束縛 4・算術/呼び出しネスト 4)と、`quote`/`defun`/`lambda`/`declaim` 等の除外 |
| 算術の現状測定 | §7。`(+ a b)` = 739 byte / 即値 20 / 加算の高速 path 40 byte / 1 回 0.63 µs |

---

## 9. 設計への示唆(4-2、決定ではない)

### 9-1 Phase 4 は「型宣言の追加」である。「表現の追加」ではない

fixnum / bignum の表現も、JIT の fixnum 高速 path も、オーバーフロー検査も**すでにある**。
Phase 4 が足すのは「宣言を読んで、その高速 path のガードを外す」部分だけである。

**削れる量の見積り(実測ベース):**

`(+ a b)` 1 回 = 0.63 µs のうち、型検査は `mov / or / movabs / test / jne` の 5 命令 19 byte。
加算本体全体が 73 byte / 約 14 命令で、そのうち型検査が 5 命令なので、

- **型検査だけ外す** → 加算部の約 1/3、`(+ a b)` 全体では **0.2 µs 程度**
- **オーバーフロー検査(`js`)も外す** → さらに 1 命令

一方で、同じ `(+ a b)` には `os_is_control_transfer` への **real call が 2 回**入っている
(§7-2、122 byte)。**型検査 5 命令より、この 2 回の call のほうが確実に大きい。**
`a`/`b` のような単純なパラメータ参照は制御転送を返しようがないので、
**そちらを先に削るほうが費用対効果が高い可能性がある**(Phase 3 で `(car x)` の
+89 byte のうち 59 byte が制御転送チェックだった、という観測と同じ構図)。

**したがって Phase 4 は、まず「制御転送チェックを省ける条件」と併せて測ることを勧める。**

### 9-2 fixnum を型システムのどこに置くか → **クラスにはしない**

ISLisp は `<integer>` しか持たず、`(class-of 1)` も `(class-of 2^100)` も `<INTEGER>` を
返す現状が正しい(§4)。`<fixnum>` をクラスとして登録すると `class-of` の戻り値が変わり、
**準拠を崩す**。

**宣言の中でだけ使える名前**にするのがよい。`fixnum` という**シンボル**を
`(declaim (type fixnum x))` / `(the fixnum expr)` の位置でだけ解釈し、
`*classes*` には登録しない。この形なら:

- `class-of` / `typep` / `subclassp` の挙動は一切変わらない
- **JIT がクラスオブジェクト(GC ヒープ上、移動する)を参照する必要が無くなる**
  → §2-2 の原則8 の問題が構造的に発生しない

**これは重要で、「型名をシンボル比較で済ませる」設計にすれば
Phase 4 は原則8 のリスクをまったく負わない。**

### 9-3 型情報を environment にどう持たせるか

§5-4 のとおり、ビットフラグでは足りない。見通し:

- **11 番目のスロット**に `(types . ((a . fixnum) (b . fixnum) ...))` の alist を持たせる。
  末尾追加なので既存の固定位置参照(4 番目まで)には影響しない
- ただし **frame は 8 スロットしか持たない**ので、`let` 束縛変数の型は
  この仕組みに乗らない。**関数引数の型も、defun の compile 時にだけ必要な情報**なので、
  environment ではなく **`za_try_compile_defun` の引数として渡す**ほうが素直かもしれない
- 実際、`optimize` は既に `za_try_compile_defun(params, body, env, owner, optimize)` の
  形で**引数として渡している**(`eval_defun`)。**同じ経路に型情報を足すのが自然**

`declare` を導入する場合は §6-3 の「body は 1 式」制約に必ず当たる。
`eval_defun` 側で `declare` を body から取り除いてから `za_try_compile_defun` へ渡す前処理が要る。

### 9-4 オーバーフロー検査を入れる余地 → **すでにある。ただし表現に依存している**

`js`(bit63)による検出が成立しているのは、**fixnum が符号マグニチュード表現で、
非負のときマグニチュードが bit3〜62 に収まるから**である。

```
両方が非負 fixnum → 下位3bit=000、bit63=0
raw どうしを add → 和が 2^60 を超えると bit63 へ桁上がりする → js で捕まる
```

**2の補数だったらこの手は使えない**(`jo` を使うことになり、タグ付き値のままでは
桁位置がずれる)。逆に言うと、**Phase 4 で表現を変えない限り、この検査はそのまま使える。**

`*` については現状インライン化されていない。`imul` + `jo` は 2 の補数前提の命令なので、
**符号マグニチュード表現のままでは `*` のオーバーフロー検査を 1 命令で書けない**。
`(* a b)` を速くしたいなら、
`mul` の上位 64bit(`rdx`)が 0 かつ結果が `FIXNUM_MAGNITUDE_MASK << 3` 以下、
という 2 段の検査になる。

### 9-5 宣言が嘘だったときの挙動

Phase 3 で `safety` を見送った理由(例外機構が無い)はここにも効く。
現在の JIT は `(car 1)` のような型エラーを `os_car_checked` 経由で
`signal_domain_error_for_class` に落としているので**機構自体はある**が、
`(+ a b)` の高速 path でガードを外した場合、`a` が cons だったら
**タグ付きのアドレスをそのまま加算して意味不明な fixnum を返す**(静かに壊れる)。

pitfalls 原則5(黙って止まる)の観点では、
**`(optimize (safety 3))` のときだけガードを残す**形が自然で、
Phase 2 で `safety` を環境スロットに持たせてある(現在は未使用)ので受け皿は揃っている。

---

## 10. 調査中に気づいた問題(4-3。**修正はしていない**)

### 10-1 `documents/abi-redesign.md` がリポジトリに存在しない

`src/c/za.c` の **6 箇所**(1199 / 1244 / 2473 / 3871 / 4089 / 6264 行)が

```c
 * JIT算術インライン展開(Phase2、documents/abi-redesign.md参照): …
 * fn解決結果キャッシュ(documents/abi-redesign.md「fn解決結果のキャッシュ化」参照): …
```

のように参照しているが、このファイルは**未追跡**(`git status` で `??`)であり、
リポジトリをクローンした人からは読めない。JIT 算術インライン化・関数解決キャッシュ・
ABI 呼び出し規約という中核の設計が、6 箇所から参照されているのに追えない状態になっている。

### 10-2 `(declaim (type ...))` が**黙って**無視される

`eval_declaim` が `optimize` 以外を `continue` で読み飛ばす(§5-1)。
`(declaim (inline ...))` 側は「未知の関数名を黙って無視する」ことをコメントで
**意図として明記している**のに対し、`type` の読み飛ばしにはその説明が無い。
pitfalls 原則6(観測可能性)の観点では、Phase 4 到達まで
「宣言したのに何も起きない」が外から分からない。

### 10-3 `(class class-name)` マクロのコメントが古い

`src/lisp/init.lisp:396`:

```lisp
;; 既知の制約: <integer>等の組み込み型のクラスオブジェクトは未実装のため対象外。
```

実際には `init_aot.lisp:368-370` で `<number>` / `<integer>` / `<float>` が登録済みで、
`(%find-class '<integer>)` は正しくクラスオブジェクトを返す(§2-3 で実測)。
コメントのほうが取り残されている。

### 10-4 負の fixnum が bignum 機構を通る

§1-4 / §7-4。`primitive_add` の高速 path 条件が **「全オペランドが非負 fixnum」**
なので、`(+ -1 1)` のような何でもない式が `limb_alloc` + `mag_sub` + `os_make_integer` を
通り、**9.7 倍遅い**。`primitive_subtract2` 側も同様に「`mag_b <= mag_a`」を要求する。

符号マグニチュード表現のままでも、
「両方 fixnum」+「符号が同じなら加算、違うならマグニチュード比較して減算」を
C の高速 path に足すだけで済むはずで、bignum 機構を通す必要はない。
**Phase 4 とは独立に直せる。**

> **【2026-09-16 追記】改善A として修正済み。** `documents/fixnum-signed-fastpath.md`。
> ヒープ確保 32 byte/回 → 0、`(+ -1 1)` は 5.76 µs → 0.38 µs。
> なお 32 byte の正体は bignum 機構ではなく `primitive_add2` の `os_make_cons` ×2 だった。
> **`*` と `/` には同じ構造が残っている**(同 §5)。

### 10-5 `%%DIAG-ADDR-REGION` は `ISIKIOS_GC_DEBUG` ビルドでしか使えない

`runtime.c:2786` の登録が `#ifdef ISIKIOS_GC_DEBUG` の中にある。
通常ビルドでは `EVAL-ERROR` になるため、
**「このアドレスはどの領域か」を通常ビルドの Lisp から確認する手段が無い**。
`%%DIAG-ZA-IMM-REGION`(JIT の即値に限る)だけが常時使える。
今回はクラスオブジェクトの所在をコード読解で確定させたが、実測では裏を取れていない。

### 10-6 `za.c:6334` が断念の**伝播点**しか記録しない

`ZA_MAX_ARITH_DEPTH` / `ZA_MAX_CALL_DEPTH` / `ZA_MAX_LOCALS_PER_LET` の
どれに当たっても `%%DIAG-ZA-BAIL-AT 0` は 6334 を返す(§6-2)。
`za.c:5093` のコメントが警告しているとおり、これらの `return 0` は
「この特化経路は当てはまらない」という正常な否定と区別がつかないため
計器を一括では入れられない。**容量上限の `return 0` だけは区別して計器化できるはず**で、
Phase 4 のベンチ設計では「なぜ乗らないか」が分からずに時間を使う可能性がある。

---

## 11. 次に決めること(本調査では決めない)

指示書 §6 に対し、調査結果から言えること:

1. **`<fixnum>` はクラスにしない。**宣言の中でだけ使えるシンボルにする(§9-2)
2. **型情報は `za_try_compile_defun` の引数として渡す。**`optimize` が既にその形(§9-3)。
   `declare` を入れるなら「body は 1 式」制約への前処理が必須(§6-3)
3. **型推論の前に、制御転送チェックの削減を測る。**
   `(+ a b)` では型検査 5 命令より `os_is_control_transfer` の call 2 回のほうが大きい(§9-1)
4. **宣言が嘘だったときは `safety` で分ける。**受け皿は Phase 2 で作ってある(§9-5)

加えて、Phase 4 とは独立に着手できるものが 2 つある:

- **負の fixnum が bignum 機構を通る問題**(§10-4)。9.7 倍の差で、直し方も限定的
- **`<` / `>` / `=` に fixnum インライン化が無い**(§7-3)。`+`/`-` と同じ形で書ける
