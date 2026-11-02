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

---

## 8. AOT 経路は `declare` / `declaim` を効かせない(2026-10-30 の判断)

§5-4 の追記である。§5-4 は「`transpile-defun` の body が単一式なので書けない」と
いう 1 点だけを書いていたが、**書ける位置を全部試して挙動を確定させた**ので、
その結果と判断をここに残す。**結論は「効かせない」。**

**この節で実装は変えていない。** 触ったのはこの文書だけである。

### 8-1 調べた範囲と方法

対象は次の 3 つ。

| 経路 | 入口 | 宣言の扱い |
|---|---|---|
| AOT | `src/lisp/transpile.lisp`(ホスト SBCL 上で動く) | **無し**(本節) |
| JIT | `src/c/za.c` | 読む(§1〜§4) |
| インタプリタ | `src/c/eval.c` | **剥がして JIT に渡すだけ**(§8-5) |

AOT の入力ファイルは `transpile.lisp` の `*aot-lisp-path*` / `*utility-lisp-path*` /
`*fs-lisp-paths*` / `*bench-lisp-path*` / `*runtime-lisp-path*`(= `init_aot.lisp`、
`utility.lisp`、`device.lisp`、`ide.lisp`、`partition.lisp`、`mount.lisp`、
`file-node.lisp`、`fat16.lisp`、`fat32.lisp`、`file-cmd.lisp`、`bench_aot.lisp`、
`test/lisp/transpile_fixture.lisp`)の 12 ファイル。

方法は 2 つ。

- **grep**(下の各節に実際のコマンドと結果を置く)
- **ホスト上で変換関数を直接呼ぶ probe。**
  `ros run --load src/lisp/transpile.lisp --load <probe.lisp> --quit` で
  `transpile-defun` / `transpile-toplevel-form` / `macroexpand-all` を
  `handler-case` 付きで呼び、返り値かエラーメッセージを出した。
  **`main` を呼んでいないので生成物(`lisp_compiled.c`)は 1 byte も書かれない**
  (実行後に `src/c/lisp_compiled.c` が「無い」ままであることと
  `git status --porcelain` が空であることで確認した)。
- **`make transpile` / docker ビルド / QEMU は実行していない。**
  並行作業と競合するため。したがって「生成物そのものを見た」確認は
  この節には**1 つも無い**(§8-9 に手順だけ置く)。

### 8-2 位置ごとの挙動(probe の実測。2026-10-30)

| 書いた位置 | 何が起きるか | メッセージ |
|---|---|---|
| `(defun f (x) (declare (type <fixnum> x)) x)` | **エラーで止まる** | `transpile-defun: bodyは単一式のみ対応です: F` |
| `(defun f (x) (progn (declare ...) x))` | **エラーで止まる** | `transpile-call: 未対応の呼び出し先です: DECLARE` |
| `(defun f (x) (let ((y 1)) (declare ...) (+ y x)))` | **エラーで止まる** | `transpile-call: 未対応の呼び出し先です: DECLARE` |
| `(defun f (x) (the <fixnum> x))` | **エラーで止まる** | `transpile-call: 未対応の呼び出し先です: THE` |
| トップレベル `(declaim (optimize (speed 3)))` | **エラーで止まる** | `transpile-call: 未対応の呼び出し先です: DECLAIM` |
| トップレベル `(declare (type <fixnum> x))` | **エラーで止まる** | `transpile-call: 未対応の呼び出し先です: DECLARE` |
| 同じものを `test/lisp/transpile_fixture.lisp` に書いた場合 | **黙って捨てられる** | (無し) |

3 行目(`let`)が止まるのは、`let` が即時 lambda 呼び出しへ展開されて body が
`progn` になるためで、2 行目と同じ経路である。つまり **AOT 入力には
`declare` を書ける位置が 1 つも無い。** §5-4 の「`defun` の body に書くと止まる」
より強い。

**例外が 1 つある。** `main`(`transpile.lisp:2337`)は fixture からは
`(remove-if-not #'toplevel-defun-p ...)` で **`defun` だけを集める**。
他の入力ファイルには `aot-toplevel-forms` / `utility-toplevel-forms` /
`fs-toplevel-forms` / `bench-toplevel-forms` という受け皿があるのに、
**fixture にはそれに相当する変数が無い。** そのため fixture のトップレベルに
置いた `declaim` は、エラーにもならず生成物にも出ず、**黙って消える**。
これは `declaim` に限った話ではなく、fixture のトップレベル非 `defun` フォーム
全般の性質である(fixture は現状 `defun` しか持っていないので踏まれていない)。

エラーは `main-or-die`(`transpile.lisp:2329`)が掴んで exit 1 にするので、
**`make` は赤になる**(`transpile.lisp:2316` 付近のコメント [P3] が理由)。

### 8-3 なぜ止まるのか(実体)

3 段ある。

1. **本体 1 式制約。** `transpile.lisp:2206-2207`。

   ```lisp
   (unless (= (length body) 1)
     (error "transpile-defun: bodyは単一式のみ対応です: ~S" name))
   ```

   `declare` を書くと式が 2 つになるのでここで止まる。

2. **`macroexpand-all` は宣言を展開しない。**
   これは**ホスト CL の `macroexpand-all` ではなく、`transpile.lisp:515` の自前の
   再帰コードウォーカー**である(`init.lisp` の `defmacro let` 等をホスト SBCL へ
   そのまま load できないため、マクロ本体を通常関数として移植し alist で引く方式。
   `transpile.lisp:505-514` のコメント)。引く表は `*macro-expanders*`
   (`transpile.lisp:284`)の 14 個だけで、`declare` / `declaim` / `the` は
   **載っていない**。probe で `(macroexpand-all '(declare (type <fixnum> x)))` が
   `(DECLARE (TYPE <FIXNUM> X))` を、`(macroexpand-all '(the <fixnum> x))` が
   `(THE <FIXNUM> X)` をそのまま返すことを確認した。

   **`init.lisp:465` の `(defmacro the (class-name form) form)` は AOT には効かない。**
   あれはインタプリタが読む定義で、トランスパイラは `init.lisp` を入力にしていない。
   「インタプリタでは `the` が no-op として素通りするのに、AOT ではビルドが止まる」
   という食い違いになる。

3. **dispatch に分岐が無い。** `transpile-expr`(`transpile.lisp:1003-1101`)の
   `cond` に `declare` / `declaim` / `the` の節は無く、
   `((and (consp expr) (symbolp (car expr))) (transpile-call expr scope))` に落ちる。
   `transpile-call` は `(t ...)` 枝の `let` で先に
   `call-target-c-name`(`transpile.lisp:537`)を呼ぶので、引数側(`(type ...)` /
   `(optimize ...)`)より**オペレータ名のほうが先にエラーになる**。
   だからメッセージに出るのは `DECLARE` / `DECLAIM` / `THE` である。

   ```sh
   $ grep -cE "'(declare|declaim|the|proclaim|optimize)\b" src/lisp/transpile.lisp
   0
   ```

   (= 宣言オペレータを**シンボルとして見ている箇所が 1 つも無い**。
   `transpile.lisp` 内に 30 箇所ある `(declare (ignore ...))` と
   `transpile.lisp:979-1000` の `(declaim (ftype function ...))` 22 本は、
   **ホスト SBCL 向け**である。前者は未使用引数の警告抑制、後者は
   相互再帰する変換関数の前方宣言で、どちらも `ros run --load` で
   `transpile.lisp` 自身がコンパイルされるときにだけ意味を持つ。
   **AOT の入力として読まれるものではない。**)

### 8-4 算術・比較の呼び先表 — 型特化版は AOT から 1 度も呼ばれていない

呼び先の表は **2 つだけ**で、`transpile.lisp` 内の他の場所に C 関数名の
文字列リテラルは無い(`grep -n '"primitive_\|"cc_' src/lisp/transpile.lisp` の
ヒットは全部この 2 表の中。600 行以降のヒットは `cc_cdr`(box 読み出し)1 件のみ)。

| 表 | 行 | 役割 | 算術・比較のエントリ |
|---|---|---|---|
| `*primitive-c-names*` | 101-282 | n 項 / 可変長。cons リストを組んで渡す | `+` `-` `*` `<` `>` `>=` `<=` `=` → `primitive_add` / `_subtract` / `_multiply` / `_less_than` / `_greater_than` / `_greater_equal` / `_less_equal` / `_num_equal`。他に `div` `mod` |
| `*primitive-fixed-arity-c-names*` | 549-598 | 実引数個数が一致したときだけ使う固定引数版 | `+` `-` `=` `<` `>` `>=` の **2 引数版のみ** → `primitive_add2` / `_subtract2` / `_num_equal2` / `_less_than2` / `_greater_than2` / `_greater_equal2` |

- **どちらの表も型には触っていない。** 載っているのは GENERIC 版だけである。
- **`*` と `<=` には 2 引数版のエントリが無い。** 2 引数で書いても n 項版
  (cons リスト構築あり)へ落ちる。意図かどうかは**不明**(表のコメントは
  「個数が一致しない呼び出しはフォールバックする」とだけ書いており、
  `*` を載せなかった理由は書かれていない)。
- **`/` はどちらの表にも無い。** AOT 入力から `/` は呼べない。
- 末尾位置も同じ表を使う。`transpile-tail-call`(`transpile.lisp:2039`)が
  扱うのは `*known-function-names*` に載る **ユーザ定義 `defun` だけ**で、
  プリミティブは `transpile-expr` → `transpile-call` に落ちる。
  **3 つめの表は無い。**

C 側に揃っている型特化版は **8 本**。

| | fixnum | single-float |
|---|---|---|
| `+` | `primitive_add2_fixnum`(`runtime.h:2040`) | `primitive_add2_single`(`:2046`) |
| `-` | `primitive_subtract2_fixnum`(`:2070`) | `primitive_subtract2_single`(`:2076`) |
| `*` | `primitive_multiply2_fixnum`(`:2139`) | `primitive_multiply2_single`(`:2145`) |
| `/` | `primitive_divide2_fixnum`(`:2240`) | `primitive_divide2_single`(`:2246`) |

- **double-float の特化版は無い。比較の特化版も 1 本も無い**(宣言しても
  `<` `>` `>=` `=` は GENERIC のまま)。
- 選ぶのは `za_specialized_wrapper`(`za.c:3116`)**だけ**で、ここは
  JIT の `za_compile_fold` からしか呼ばれない。混在(fixnum × single)は
  GENERIC へ落とす。
- **AOT 側には特化版の名前が 1 つも無い。**

  ```sh
  $ grep -c 'primitive_add2_fixnum\|primitive_subtract2_fixnum\|primitive_multiply2_fixnum\|primitive_divide2_fixnum\|primitive_add2_single\|primitive_subtract2_single\|primitive_multiply2_single\|primitive_divide2_single' src/lisp/transpile.lisp
  0
  ```

**選ぶ余地はあるか。** 形の上ではある(2 表に特化版の列を足し、宣言を読んで
切り替える)。ただしそれは**宣言を読む仕組みを `transpile.lisp` に新設する**
作業であって、既存の表を差し替えるだけでは済まない。§8-6 の 1 番目の理由。

### 8-5 インタプリタ経路との違い(1 行)

> **インタプリタは `declare` を「剥がして JIT へ渡す」だけで自分では使わない。
> AOT は剥がす仕組みすら持たないので、書くとビルドが止まる。**

内訳:

- `eval_defun`(`eval.c:480-494`)が `os_env_optimize` →
  `os_scan_declaration_inline` → `os_scan_declarations` の順に読み、
  **body から先頭の連続する `declare` を剥がし**、残りと `declared_types` を
  `za_try_compile_defun` へ渡す(`eval.c:483` のコメント「宣言を剥がす前に読むこと」)。
- 作用対象の無い位置の `declare` は `eval.c:1226-1234` で **`nil` に潰す**
  (エラーにしない)。`za.c:4286` も同じく `nil`。どちらも
  「`za_is_excluded_special_form` に入れてはいけない」と明記されている(§4-4)。
- `declared_types` の**読み手は `za.c` だけ**(`za.c:7019` / `:7434` / `:7440`。
  JIT 無効ビルドのスタブ `za.c:7521-7523` は `(void)declared_types;` で捨てる)。
  `runtime.c:1799` の `%%DECLARED-TYPES-OF` は内省用で、最適化には使われない。
- したがって**宣言は JIT 専用の入力**であり、**インタプリタ実行そのものの意味は
  `declare` の有無で変わらない**。

AOT 入力と `init.lisp` に現在ある宣言の件数:

```sh
$ grep -cE '\((declare|declaim|the) ' <AOT 入力 12 ファイル> src/lisp/init.lisp
全ファイル 0
```

**宣言は AOT 入力にも `init.lisp` にも 1 件も書かれていない**(2026-10-30 時点)。
宣言が使われているのは `test/lisp/declare_types_test.lisp` /
`declaim_test.lisp` / `declare_types_bench.lisp` などテスト・ベンチ側だけで、
これらは**インタプリタ経路で読まれる**(AOT の入力リストに入っていない)。

### 8-6 結論: **効かせない**

理由を 3 つ。重いほうから。

1. **行き先が無い(事実)。** 型情報の消費者は `za.c` だけで、AOT は `za.c` を
   1 行も通らない。3 経路が共有しているのは C ランタイムだけである。
   AOT で効かせるには「宣言を読む」「表を特化版へ切り替える」という
   **`transpile.lisp` 側の別実装**が必要で、JIT 側の成果が横に流れてくるのではない。
2. **対照群が壊れる(事実。§8-8)。** AOT の生成物が変わると、
   `aot/*` ベンチ行を「±0 の対照群」として使っている既存の記録の前提が崩れる。
3. **見込みが薄い(推測。確認していない)。** §7 の実測で、JIT 側で
   `(declare (type <fixnum> x y))` が省くのは「x / y の型ガード 118 byte」である。
   AOT は**もともと `primitive_add2` を直接 call していて、JIT のような
   インライン加算経路を持たない**(§8-4)。つまり AOT で特化版へ切り替えても
   変わるのは call 先 1 つで、JIT で効いた 118 byte に相当する削減対象が無い。
   **これは生成コードを読んだ上での推測であって、測っていない。**
   測るには `make transpile` と QEMU が必要で、本作業では実行していない。

### 8-7 `declare` を剥がすか: **剥がさない。入れないと決めた**

**「できない」のではない。** `transpile-defun` の 1 式制約を外さずに
「body 先頭の `declare` だけ読み捨てる」実装は、`length` を見る前に
`(car form)` が `DECLARE` のフォームを落とすだけなので数行で書ける。
**それをやらないと決めた。**

理由:

1. **いまは黙って食い違わない。** 書いた瞬間に止まり、メッセージに関数名が出る
   (§8-2)。剥がして無視する実装を入れると「AOT では宣言が効いていない」ことが
   **沈黙する**。PR #81 の AOT float リテラルが静かにずれていた件(§5-4)と
   同じ形の罠を、自分で作ることになる。
2. **「黙って捨てる」経路を増やす方向である。** §8-2 で見つかった唯一の
   黙殺経路(fixture のトップレベルフォーム)は、増やすのではなく
   いずれ塞ぐべきものである。
3. **剥がす動機が実在していない。** 効かせないと決めたのだから、剥がす理由は
   「同じソースを AOT とインタプリタの両方に食わせたい」だけになる。
   いま AOT 入力と `init.lisp` は別ファイルで、宣言の件数は**両方 0 件**(§8-5)。
   共有したいソースが存在しない。

**したがって「AOT 入力に `declare` / `declaim` / `the` を書いてはいけない」という
制約をそのまま残す。** 将来、宣言を含む同一ソースを両経路に食わせる必要が
出たときは、**剥がす実装と「剥がした(=効いていない)ことを言う診断」を
同時に**入れること。片方だけ入れると 1 番目の罠を踏む。

### 8-8 効かせた場合に何が起きるか — 対照群が使えなくなる

**AOT の生成物 `src/c/lisp_compiled.c` のテキストが変わる。**
これは測定精度の問題ではなく、**対照群の問題**である(規則 5 / R-control)。

- `documents/stack-guard.md` §4 / §5-1 は「AOT は `za.c` のプロローグを通らないので
  **対照群になる**」「`aot/*` 全 10 行は **±0**」を前提に効果を示している。
- `documents/for-expansion.md` §5-1 も `aot/for` を対照群に置いている。
- 宣言を AOT で効かせると、宣言を書いた関数の**呼び先が変わって命令列が動く**。
  以後「`aot/*` が動いたのはノイズか効果か」を切り分ける手段が無くなる。

さらに `for-expansion.md` §5-3 の反省(「1 命令も動かない」は N=1,000,000 の
ベンチでは**分解能が無く検証できない**。規則 10 / R-resolution)を踏まえると、
**AOT 側は「時間が動かない」ではなく「生成テキストが同一」で固定しておくほうが
強い。** テキストの `diff` は分解能が 1 byte なので、規則 10 の問題が起きない。

この結論により、**「この PR で AOT の生成物は 1 文字も変わらない」を、
後続の最適化作業でそのまま対照群として使い続けられる。**
JIT 側の作業(型特化・インライン展開)は定義上 AOT に触らないので、
`lisp_compiled.c` の `diff` が空であることを**毎回確かめるべき不変条件**にできる。

### 8-9 「1 文字も変わらない」の確認方法

**生成物はリポジトリに無い。** `.gitignore` の 8-9 行目に
`src/c/lisp_compiled.c` と `test/c/lisp_compiled_fixture.c` が入っており、
`git ls-files` も空、作業ツリーにも存在しない(2026-10-30 に確認)。
したがって diff を取るには生成が必要である。

```sh
make transpile
cp src/c/lisp_compiled.c /tmp/base_lisp_compiled.c
# (変更を入れる)
make transpile
diff -u /tmp/base_lisp_compiled.c src/c/lisp_compiled.c   # 空であること
```

**生成せずに静的に言えること**(今回確認したのはこちらだけ):

- 特化版 8 本の名前が `transpile.lisp` に 1 つも無い(§8-4 の `grep -c` → `0`)。
  ゆえに生成物にも出得ない。
- 宣言オペレータをシンボルとして見ている箇所が 1 つも無い
  (§8-3 の `grep -cE` → `0`)。ゆえに宣言由来の出力コードは存在し得ない。
- そもそも AOT 入力に宣言が 1 件も書かれていない(§8-5 の `grep -cE` → 全部 `0`)。

**未確認(不明)のまま残すもの:**

- 生成物 `src/c/lisp_compiled.c` を実際に見て
  「宣言由来の文字列が 0 件」を確かめること。
  **本作業では `make transpile` を実行していないので未実施。**
- §8-6 の 3 番目(AOT で特化版へ切り替えても見込みが薄い)の**実測**。
  読んだだけで測っていない。
- `*primitive-fixed-arity-c-names*` に `*` と `<=` の 2 引数版が無い理由。
  コメントに書かれていない。
