# `declare` による型宣言の受け付け (Phase 4a-2)

対象: PR #84(`feature/declare-types`、base は `feature/compiler-optimization`)

関連: `documents/type-system-survey.md` §5-3 / §6-3 / §6-4、
`documents/declaim-design.md`(Phase 2)、`documents/inline-builtin.md`(Phase 3)

---

## 1. 何をしたか

`(declare (type <fixnum> x y))` を受け付け、型情報をコンパイル時に記録する
ところまで作った。

```lisp
(defun add-integer (x y)
  (declare (type <fixnum> x y))
  (+ x y))

(%%declared-types-of 'add-integer)   ; => (<FIXNUM> <FIXNUM>)
```

**生成コードは 1 バイトも変わらない。** 型を使った最適化は後続の Phase である。
Phase 2 の `declaim` と同じで、受け取って記録し、コンパイラへ引き渡すところまで。

### 実行時の表現には触れていない

- frame のスロットは増えていない(`let` の hot path は変わらない)
- 環境のスロットも増えていない
- 仮引数の型は `za_fn_meta_t`(Immobilized Space)へ、`let` 束縛変数の型は
  `za_local_scope_t`(C スタック上のコンパイル時構造)へ置く

---

## 2. 調査結果

### 2-1 `let` は対象にできる(同じ PR に含めた)

指示書 §4-1 の懸念は「`let` がマクロで lambda 適用へ展開されるので、
`eval_defun` で剥がす経路に乗らない」ことだった。**展開後も declare は残る。**

```lisp
(defmacro let (bindings &rest body)
  `((lambda ,(%let-vars bindings) ,@body) ,@(%let-inits bindings)))
```

`body` を `,@body` でそのまま通すので、`(let ((x 1)) (declare ...) (+ x 1))` は

```lisp
((lambda (x) (declare (type <fixnum> x)) (+ x 1)) 1)
```

になり、**declare は lambda 本体の先頭にそのまま現れる**。したがって
「展開前に剥がす」必要は無く、`za_compile_let` が lambda 本体を取り出した
直後に剥がせばよい。

| 確認項目 | 結果 |
|---|---|
| `let` マクロ展開後の declare の位置 | lambda 本体の先頭。そのまま残る |
| 展開前/展開後のどちらで剥がすか | **展開後**(`za_compile_let`)。マクロには手を入れていない |
| `za_compile_let` の body の扱い | `lambda_body` を1つのローカルで持ち回るので、そこを差し替えれば解析ループと本体コンパイルループの両方が剥がした後を見る |
| `let*` | **効かない。** §5-6 を参照 |
| `lambda` / `flet` / `labels` | 本体は常にインタプリタ実行(JIT 再帰しない)。declare は no-op として素通りする。§5-2 参照 |

### 2-2 コンパイル時構造のデータ量: **増加 0 byte**

`za_local_var_t` に型のフィールドを素朴に足すと 16 → 24 byte に太り、
`za_local_scope_t` が 1 段あたり 32 byte 増える。`kind` を enum(4 byte)から
`UINT16` へ落としてから 1 byte を足すことで **16 byte のまま**にした。

| 構造 | 変更前 | 素朴に足した場合 | 採用した形 |
|---|---:|---:|---:|
| `za_local_var_t` | 16 | 24 | **16** |
| `za_local_scope_t` | 80 | 112 | **80** |
| `ZA_MAX_LET_DEPTH`(16)段ぶん | 1280 | 1792 | **1280** |

`za_local_scope_t` は `za_compile_let` の C スタック上のローカルで、ネストの
段数ぶんだけ積み上がる。`ZA_MAX_COMPILE_NEST = 60` は式の再帰段数であって
`let` の段数ではないので、積み上がる量を決めるのは `ZA_MAX_LET_DEPTH = 16` の
ほうである。**増加が 0 なので、どちらの上限にも影響しない。**

`sizeof(za_local_var_t) == 16` は `_Static_assert` で固定した。後から誰かが
フィールドを足したらコンパイルが止まる。

仮引数の型を記録する `za_fn_meta_t` は 48 → 64 byte になる
(`os_imm_slot_alloc` が `OS_HEAP_ALIGN = 16` へ切り上げるため、
6 フィールド 48 byte はちょうど 48、8 フィールド 64 byte は 64)。
**7 フィールド(56 byte)でも切り上げ後は 64 byte** なので、1 ワードに詰めても
2 ワード使っても実消費は変わらない。詰める必要が無いので 2 ワード使い、
1 変数 8bit の素直な符号にした。増分は **JIT コンパイルされた関数 1 個あたり
16 byte**。

### 2-3 `declaim` 側は変えていない

Phase 2 の `eval_declaim` は `optimize` 以外の指定子を `continue` で読み飛ばす。
**本作業ではそこに手を入れていない**(グローバル変数の型宣言は Phase 4c で、
「後から `setq` で型が変わる」という別の問題を含む)。

ただし**型名の解決経路は共有できる形にしてある**。`os_decl_type_code_of` は
`declare` 専用ではなく、クラス名シンボルを受け取って型符号を返すだけの関数で、
Phase 4c の `declaim` 側からそのまま呼べる。二重実装にはならない。

### 2-4 body 複数式化(PR #71)との関係

declare を剥がした結果 body が 1 式になる場合も 2 式以上残る場合も動く。
PR #71 で `za_try_compile_defun` が body 2 式以上を `(progn . body)` で包むように
なっているため、剥がした結果に関わらず同じ経路を通る。

```lisp
(defun f (x) (declare (type <fixnum> x)) (+ x 1))        ; 剥がすと 1 式
(defun g (x) (declare (type <fixnum> x)) (setq x 1) x)   ; 剥がすと 2 式
```

どちらも `%%za-compiled-p` が T になることをテストで固定した。

---

## 3. 仕様

### 3-1 構文

> **【この決定は上書きされた (inline-arith Phase 2)】**
> 「`type` キーワード付きの形**のみ**」は、**もう成り立っていない。**
> `(declare (inline +))` / `(declare (notinline +))` が**効くようになった**
> (`documents/inline-arith.md` §7)。
> 下の「`type` 以外は黙って読み飛ばす」も、`inline` / `notinline` については
> 当てはまらない。**それ以外の指定子(`ignore` 等)は読み飛ばしのまま。**
>
> 当時の「後から `inline` 等を足しやすい」という見立ては当たっていた。
> **足したので、記述を上書きする。**

**`type` キーワード付きの形のみ。**(← `inline` / `notinline` が追加された)

```lisp
(declare (type <fixnum> x y))
```

CommonLisp の `(declare (fixnum x y))` のような型名直書きは採らない。
型名と他の宣言指定子を構文で区別できるほうが、後から `inline` 等を足しやすい。

`type` 以外の宣言指定子(`(declare (ignore x))` など)は**黙って読み飛ばす**。
Phase 3 の `inline` と同じ方針である。

### 3-2 書ける位置

**body の先頭のみ。** 先頭に連続する `declare` はすべて読み取って合成する。

```lisp
(defun f (x y)
  (declare (type <fixnum> x))
  (declare (type <double-float> y))   ; 複数可。合成される
  (+ x y))
```

body の途中に現れた `declare` は**宣言として扱わない**。エラーにもならず、
**通常の式として no-op(nil)** になる。JIT も諦めない。

### 3-3 コンパイルされない文脈では無視される

**`declare` はコンパイル時にしか効かない。** JIT は `defun` をトリガーに動くため、
`defun` の外に書かれた `declare` には作用する対象が無い。

```lisp
;; トップレベル。インタプリタで実行されるので declare は素通りする(nil を返す)
(let ((x 1))
  (declare (type <fixnum> x))
  (+ x 1))                            ; => 2

;; こう書けば declare が効く(defun 全体がコンパイルされる)
(defun f (x)
  (declare (type <fixnum> x))
  (let ((y 1))
    (declare (type <fixnum> y))
    (+ x y)))
```

**これは「放っておけば成立する」ものではない。** 何も手当てしないと、
インタプリタが `(declare ...)` を式として評価しようとして未定義の関数エラーに
なる。`eval` 側に no-op の特殊形式として足してある。

**利用者が最初に戸惑うのはここである。** トップレベルで試すと何も起きない。

### 3-4 型名は実在のクラス

`<fixnum>` などは `*classes*` に登録された実在のクラスである(PR #79)。
解決は `%find-class` と**同じ `*classes*` を同じ順序で引く**
(`decl_find_class`、後述の §4-2)。別の名前解決表は作っていない。

**別名が働く。** `<short-float>` / `<long-float>` は `<single-float>` /
`<double-float>` と**同一のクラスオブジェクト**に 2 つ目の名前で登録された別名で
あって、サブクラスではない(`init_aot.lisp`)。符号化は**利用者が書いた名前では
なく、解決したクラスオブジェクトの正準名**(クラスの word1)を見るので、
別名は自動的に同じ符号になる。

```lisp
(defun f (x) (declare (type <short-float> x)) x)
(%%declared-types-of 'f)     ; => (<SINGLE-FLOAT>)
```

`%convert` が利用者の書いた名前を `case` で突き合わせていたために別名が効かな
かった件(`documents/float-default.md` §4-2)と同じ轍は踏んでいない。

### 3-5 未知の型名は無視する。ただし無視されたことは分かる

```lisp
(defun f (x) (declare (type <nonexistent> x)) (+ x 1))   ; エラーにならない
(%%declared-types-of 'f)     ; => (<UNKNOWN>)
```

Phase 2 で `(declaim (type ...))` が**黙って**読み飛ばされていたのが
`documents/type-system-survey.md` §10-2 で問題として挙がっている。
本実装では「無視された」ことを内省関数から読めるようにした。

2 種類を区別する。

| 記録される値 | 意味 |
|---|---|
| `<UNKNOWN>` | クラスとして解決できなかった(そんなクラス名は無い) |
| `<OTHER>` | 実在するクラスだが、本実装が符号を持たない(ユーザ定義クラス等) |
| `nil` | その位置には宣言が無い |

警告を出す手段(`warn` 相当)は無いため、内省関数で分かる形を採った。

---

## 4. 記録先の設計

### 4-1 なぜ「変数名 → 型」の alist を持たないのか

指示書 §3-6 は A 案(関数オブジェクトに保持する)を推しており、その代償として
「リストなので GC ルートの考慮が要る」と書かれている。

**記録先はどちらも GC のルート走査の対象外である。**

- `za_fn_meta_t` は Immobilized Space 上(GC は辿らない)
- `za_local_scope_t` は C スタック上のコンパイル時構造

クラスオブジェクトもシンボルも `gc_copy_value` が追いかける対象なので、生の
`lisp_val_t` を置くと**最初の GC でそのまま stale になる**。ルート登録の仕組み
(`os_gc_register_root` + リテラルスロット)を持ち込めば格納自体はできるが、
内省のためだけに GC の面倒を増やすことになる。

そこで **「解決済みクラスの正準名に対応する小さな整数」へ畳んでから持つ**。

```c
typedef struct { UINT64 w[2]; } os_decl_types_t;   /* 16変数 × 8bit */
```

`lisp_val_t` を 1 つも含まないので、GC と完全に無関係になり、値渡しで持ち回れる。
A 案の利点(永続する・関数を指定して引ける)はそのまま得られる。

**代償は 2 つ。**

1. **変数名が残らない。** `%%declared-types-of` は**仮引数の位置順のリスト**を
   返す。名前が要る場合は仮引数リストと突き合わせる。
2. **符号を持つクラスが有限。** 組み込みクラス 26 個(`g_decl_type_names`)に
   限られ、それ以外は `<OTHER>` になる。本 Phase は最適化しないため実害は無く、
   最適化に使える型(数値の塔)はすべて符号を持っている。

### 4-2 なぜ Lisp の `%find-class` を呼ばないのか

`os_resolve_class`(既存、`osApplyFunction` で Lisp の `%find-class` を呼ぶ)は
**確保を伴う**。型宣言の解決は `za_compile_let` からも呼ばれるので、
コンパイル中に GC を誘発してはいけない(`documents/pitfalls.md` 原則 4:
コンパイル中の GC は手元の AST を stale にする)。

そこで `decl_find_class` は `*classes*` をその場で走査する
(`%find-class` の `(cdr (assoc name (dynamic *classes*)))` と同じ)。
**確保を一切行わない。** PR #83 で印字経路の `os_make_symbol` を
`find_interned_symbol` へ置き換えたのと同じ理由である。

**別の表は作っていない。** 引くのは `%find-class` と同じ `*classes*` であり、
`g_decl_type_names` が受け取るのは**既に解決済みのクラスの正準名**である。

### 4-3 `declare` を特殊形式にした理由(マクロにしない)

`the` はマクロとして実装されており、**`za` が見る前に型情報が消えている**
(`documents/type-system-survey.md` §5-2)。`za` は式を評価する位置に立つたびに
`macroexpand` を回すためである。

`declare` を同じ形で作ると、`eval_defun` で剥がす前にマクロ展開が走った場合に
同じことが起きる。**`eval` と `za` がそれぞれ認識する特殊形式にしてある。**

### 4-4 `za_is_excluded_special_form` に入れてはいけない

`declaim` はそこに入っているため、`(defun f (x) (declaim ...) ...)` は JIT から
外れる(`documents/type-system-survey.md` §6-4)。

`declare` を同じ扱いにすると「**宣言を書いた結果コンパイルされなくなる**」という
本末転倒になる。`za_compile_expr` に分岐を足し、**nil を返す式として潰している**。

これにより、剥がしそこねた位置(body 途中、対象にしていない束縛形式の中)に
`declare` が残っていても JIT は諦めない。

| 経路 | `declare` の扱い |
|---|---|
| `eval_defun` の body 先頭 | 剥がして型を符号化 → `za_try_compile_defun` へ渡す |
| `za_compile_let` の let 本体先頭 | 剥がして `za_local_scope_t` へ記録 |
| それ以外の位置(`za`) | nil を返す式へ潰す |
| それ以外の位置(インタプリタ) | `eval` の特殊形式として nil を返す |

---

## 5. 既知の制約

### 5-1 内省はコンパイルされた関数に限る

`%%declared-types-of` は `za_fn_meta_t` を読むので、**インタプリタ実行に落ちた
関数は nil を返す**。`%%OPTIMIZE-OF` / `%%INLINE-OF` と同じ性質である。

**ユニットテストのビルド(`make test`)では JIT が常に断念する**ため、
この関数の確認は `make test-qemu` 側でしか行えない。

### 5-2 `lambda` / `flet` / `labels` の本体は対象外

これらの本体は常にインタプリタ実行で、JIT は再帰しない。`declare` は no-op と
して素通りする。**エラーにはならず、JIT も諦めない**(その関数全体が JIT 対象
であれば、`flet` を含んでいても今までどおりコンパイルされる)。

### 5-3 `let` 束縛変数の型は関数オブジェクトに残らない

`za_local_scope_t` はコンパイルが終われば消える C スタック上の構造なので、
`%%declared-types-of` では引けない。配管が通っていることを外から確かめる
ために、**直近のコンパイル試行で記録した件数**を返す診断を足した。

```lisp
(defun f (n) (let ((x 1)) (declare (type <fixnum> x)) (+ x n)))
(%%diag-za-local-decls)      ; => 1
```

### 5-4 AOT トランスパイラ(`src/lisp/transpile.lisp`)は declare を扱わない

AOT 対象のファイル(`init_aot.lisp` 等)の `defun` に `declare` を書くと、
`transpile-defun` の「bodyは単一式のみ対応です」で**ビルドが止まる**。

黙って食い違うわけではない(PR #81 で AOT の float リテラルが静かにずれていた
のとは形が違う)ので、当面はこのままにした。AOT は C を生成する経路で JIT を
通らないため、型情報の行き先がそもそも無い。

### 5-5 `let*` では declare が効かない

`let*` は最内で `(progn ,@body)` へ展開される。

```lisp
(let* ((a 1) (b 2)) (declare (type <fixnum> a)) body)
;; → (let ((a 1)) (let ((b 2)) (progn (declare ...) body)))
```

declare が `let` 本体の先頭ではなく **`progn` の中**に入るため、`za_compile_let` の
剥がし対象から外れる。エラーにはならず、JIT も諦めない(通常の式として nil に潰れる)
が、**型は記録されない**。

`let` の body を剥がす位置を「先頭の progn の中まで見る」ようにすれば対応できるが、
`(progn (declare ...) ...)` を宣言として扱うかどうかは `declare` を書ける位置
(§3-2「body の先頭のみ」)の定義を広げる話になるため、本作業では踏み込まなかった。

### 5-6 同じ変数を 2 回宣言したら後勝ち

```lisp
(declare (type <fixnum> x))
(declare (type <double-float> x))   ; こちらが残る
```

CommonLisp では未定義。単純に上書きしている。

---

### 5-7 実装中に踏んだ無限ループ(記録)

**`nil` は `g_nil_cell | TAG_CONS`、つまり TAG_CONS タグを持つ Lisp 値である**
(`runtime.c` 冒頭のコメント)。リストの終端判定に

```c
for (lisp_val_t v = list; (v & TAG_MASK) == TAG_CONS; v = cc_cdr(v))   /* だめ */
```

と書くと、**nil もこの条件を満たす**ため `cc_cdr(nil) == nil` が返り続けて抜けられない。
`decl_apply_spec` の変数走査でこれを踏み、`(declare (type <fixnum> x))` を含む
defun を 1 つ評価した時点で実機がハングした。

**`make test` は 8489 件すべて通っていた。** ユニットテストに declare を踏む経路が
1 つも無く、このループに一度も入っていなかったためである。
`make test-qemu` では「ハング」ではなく「JIT が効かず全部インタプリタになったような
極端な遅さ」に見え、2 時間走っても終わらなかった。

再発防止として `test/c/runtime_test.c` に
`test_scan_declarations_terminates_on_nil()` を追加した。declare の走査は
`eval_defun` 側(インタプリタ経路)なので、**JIT が常に断念するユニットテストの
ビルドでも踏める**。

終端は必ず `v != nil` を先に見ること。既存コードが一貫して
`for (cur = args; cur != nil; cur = cc_cdr(cur))` と書いているのはこのためである。

---

## 6. 追加した組み込み

| 名前 | 内容 |
|---|---|
| `%%DECLARED-TYPES-OF` | 仮引数の宣言型を位置順のリストで返す。インタプリタ実行の関数は nil |
| `%%DIAG-ZA-PARAM-DECLS` | 直近のコンパイル試行で**コンパイラが受け取った**仮引数の宣言の件数 |
| `%%DIAG-ZA-LOCAL-DECLS` | 直近のコンパイル試行で `let` 束縛変数に記録した宣言の件数 |

`%%DECLARED-TYPES-OF` が読むのは**コンパイル後に meta へ書き戻した結果**なので、
「コンパイラ本体へ届いていたか」とは別物である(コンパイルに失敗した関数では
両者が食い違う)。前者を `%%DIAG-ZA-PARAM-DECLS` で分けて確認できるようにした。

組み込みが 3 つ増えたので `SMALL_HEAP_SIZE` の窓が動く可能性があったが、
**85KB のままで `runtime_test` は通った**(ハングしない)。

---

## 6-2. 性能

### 結論: declare 自体のコストは測定限界以下。生成コードは 1 byte も変わらない

`test/lisp/declare_types_bench.lisp` で、**コンパイル時と実行時を分けて**測った
(`make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_declare_bench.lisp`)。

**生成コードは宣言の有無で完全に同一である。** `disassemble` の出力を 1 行ずつ
突き合わせて確認した(下記)。したがって実行時間に差が出る理由は原理的に無い。

```
add-fixnum  : (declare (type <fixnum> x y)) → code-len=735  declared=(<FIXNUM> <FIXNUM>)
add-number  : 宣言なし                       → code-len=735  declared=NIL
diff: 完全一致(配置アドレス以外に 1 byte の差も無し)
```

**実行時間**(n=1,000,000。3回測って最小値。1 tick = 10ms)

| 型 | 宣言あり − なし | **対照(なし − なし)** |
|---|---:|---:|
| `<fixnum>` | +36 | **+44** |
| `<single-float>` | +4 | **-20** |
| `<double-float>` | +4 | **+4** |

**対照群**は「宣言の違いが一切無い、本体が同一で名前だけ違う 2 関数」の差である。
生成コードが同じでも Immobilized Space 上の配置が違えばこれだけ揺れる。
**対照の差のほうが大きいので、宣言による実行時コストは検出できていない。**
対照群を置かずに測った回は fixnum が +16% に見え、危うく「実行時に効く」と
読み違えるところだった。

**コンパイル時間**(n=1000)は 2 回測って符号が反転した
(1 回目は全型 +4、2 回目は -4 / -16 / -12 / -24)。
**この分解能では検出できない。**

### テストスイート全体は +18% 遅くなっている。原因は未解明

`make test-qemu`(256M・進捗出力なし)は再現性のある差がある。

| 構成 | tests | `#elapsed` |
|---|---:|---:|
| 変更前 | 4032 | 18388 / 18488 / 18820 |
| 本作業 | 4095 | 21752 / 21772 |

declare のテスト 63 件自体は 52 tick(0.5秒)しか使っていないので、
**+3332 tick ≒ 33秒 = +18%** は実装由来である。しかし:

- declare の処理そのものは、上記のとおりコンパイル時も実行時も**検出限界以下**
- 生成コードは 1 byte も変わらない
- `za_compile_let` の declare 走査は無効化しても変化しない(無罪)
- `za_fn_meta_t` を 48 byte へ戻すと 19348 tick になった(1 回のみ)が、
  **Immobilized Space の実消費増は 21KB / 1.3MB = 1.6%(325 → 330 ページ)**
  にすぎず、+5 ページで 24 秒という因果は成り立たない

**したがって「meta の肥大が原因」という説明は取り下げる。** 残る候補は
「構造体サイズを変えると、それを触る全関数の機械語が変わり、コード配置と
命令キャッシュの当たり方が変わる」というビルド差だが、**これも実証していない。**
96M では +8% にとどまる。

コンパイル時間が多少増えるのは許容する、という判断でこのまま入れた。

### 測定の作法(踏んだ罠)

- **壁時計は ±10% ぶれる。** 同じ内容の 2 回が 220s と 241s になった。
  ゲスト内の tick(`#elapsed`)で比べること。
- **対照群を置くこと。** 生成コードが同一でも配置で数 % 揺れる。
  「宣言あり vs なし」だけ見ると、その揺れを宣言のせいにしてしまう。
- **進捗出力(`#at`)そのものの影響は無い**(41 行では 0〜4秒)。
  ただし監査モードは全アサーション 4095 行を出すので、そちらでは効く。
  `*isiki-test-progress*` で止められる。

---

## 7. 次の段階

型情報がコンパイラへ届くようになった。次は 2 方向ある。

**型を使った最適化。** ただし `(+ a b)` の現状を実測で押さえておくこと。
`(defun add-number (x y) (+ x y))` を disassemble すると、`+` の本体は 191 byte で
**内訳は次のとおりだった**(全体 735 byte のうち)。

| 範囲 | byte | 内容 |
|---|---:|---|
| `0x1EE`-`0x224` | 55 | x の TAG_INSTANCE 判定 + bignum 判定 call |
| `0x225`-`0x263` | 63 | y の同じ判定 |
| `0x264`-`0x287` | 36 | `or` + `test 0x800000000000000f`(タグ4bit と 符号bit63) |
| `0x288`-`0x29B` | 20 | **インライン加算 `add r10, rdx`** + オーバーフロー検出 `js` |
| `0x29C`-`0x2AC` | 17 | `primitive_add2` への call |

**fixnum どうしなら call は起きない。** `add r10, rdx` 一発で完結し、
`primitive_add2` は**フォールバック経路でのみ**呼ばれる。

したがって `(declare (type <fixnum> x y))` で省けるのは **118 byte の型ガード**
(上の表の 1〜2 行目)である。

**省けないものが 2 つある。**

- **オーバーフロー検査。** fixnum どうしでも和が範囲を超えれば bignum になる。
  省くと静かに壊れる。
- **bit63 の検査。** この処理系の fixnum は 2 の補数ではなく**符号+絶対値**
  (`documents/fixnum-signed-fastpath.md`)なので、負の数が混ざると単純な `add` が
  使えない。型が fixnum と確定していてもここは残る。タグ側(下位 4bit)だけが省ける。

**float 側のほうが伸びしろが大きい。** single-float / double-float は上の `test` に
引っかかるため、**毎回 `primitive_add2` を call している**(インライン経路が
fixnum 専用のため)。ベンチでも single は fixnum の 2 倍以上かかっている。

```
run <fixnum>       n=1000000  ticks=252
run <single-float> n=1000000  ticks=540
```

single-float はタグ 0x4 の即値でヒープ確保が無いのにこの差で、call と引数 cons
構築のぶんである。`primitive_add_single_float` のような**型別の呼び先**を用意するか、
single のインライン加算(bits 32-63 を取り出して加算)を出せば、fixnum の型ガード
削減より効果が大きいと見込まれる。SBCL が `generic-+` と型確定後の特化版を
分けているのと同じ構造である。

`test/lisp/declare_types_bench.lisp` が fixnum / single-float / double-float の
**現状の基準値**になっている。最適化を入れたらここに差が出るはずである。

**Phase 4b(`the`)と 4c(`declaim` によるグローバル変数の型宣言)。**
4c はグローバル変数が後から `setq` で書き換わるため、宣言した型を保ち続ける
保証が無いという別の問題を含む。`os_decl_type_code_of` は `declare` 専用では
ないので、4c から同じものを呼べる。
