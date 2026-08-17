# isiki-os JITコンパイラ(za.c)の制約まとめ

このドキュメントは、`src/c/za.c` が「実際にどこまでコンパイルできるか」を、
実装コードそのものに 基づいて整理したものです。

## 前提

- 非対応構文に出会うと、`defun`単位でコンパイルが**丸ごと**失敗し、黙って
  インタプリタ実行(`MAGIC_FUNCTION_INTERPRETED`)にフォールバックします
  (`eval_defun`、`eval.c`)。部分的にJIT化してあとはインタプリタで補う、という
  混在は存在しません。結果自体はインタプリタ経由でも常に正しく、フォールバックは
  「速くならない」だけで「間違った答えが出る」ことはありません。
- したがって以下の「できないパターン」はすべて「そのdefun全体がインタプリタ実行に
  なる」という意味です。JIT化できたかどうかは`(print #'関数名)`等で表示される
  `#<FUNCTION-COMPILED ...>`(JIT成功)/`#<FUNCTION-INTERPRETED ...>`
  (フォールバック)の違いで確認できます。

## コンパイルできるパターン

### 算術・比較・基本データ操作

```lisp
(defun sum3 (a b c) (+ a b c))          ; ZA_MAX_OPERANDS(16)まで項を増やせる
(defun avg (a b) (/ (+ a b) 2))         ; 算術のネストもOK(後述の深さ制限内)
(defun cmp (a b) (if (< a b) 'less 'ge))
(defun second-elt (lst) (car (cdr lst)))
(defun push-front (x lst) (cons x lst))
```
オペランド位置には即値・パラメータ参照だけでなく、`if`・一般呼び出し・入れ子の
算術/比較式も置けます。結果はインタプリタと同じC関数(`primitive_add`等)を呼ぶ
だけなので、bignum化・float化を含め結果の正しさはインタプリタと完全に一致します。

### `if` / `progn` / `setq`(let-localのみ)

```lisp
(defun abs2 (x)
  (if (< x 0)
      (- 0 x)
      x))

(defun counter-step (n)
  (let ((acc 0))
    (setq acc (+ acc n))   ; let-localへのsetqはOK
    acc))
```

### 関数呼び出し(自己再帰・相互再帰・他関数呼び出し)

```lisp
(defun fact (n)
  (if (= n 0) 1 (* n (fact (- n 1)))))       ; 自己再帰

(defun is-even (n) (if (= n 0) t (is-odd (- n 1))))   ; 相互再帰
(defun is-odd (n) (if (= n 0) nil (is-even (- n 1))))
```
呼び出し先は毎回名前で再解決するため、前方参照・再定義がそのまま安全に動きます。
末尾呼び出しは共有トランポリンへの`jmp`で実際にスタックを消費しない形で
最適化されています(`fact`は末尾呼び出しではないためスタックを消費しますが、
以下は末尾呼び出しとして定数スタックで動きます):

```lisp
(defun fact-tail (n acc)
  (if (= n 0) acc (fact-tail (- n 1) (* n acc))))    ; 末尾呼び出し
```

### `let` / `let*`(多段ネスト対応)

```lisp
(defun distance-sq (x1 y1 x2 y2)
  (let* ((dx (- x2 x1))
         (dy (- y2 y1)))
    (+ (* dx dx) (* dy dy))))

(defun nested-let-example (a)
  (let ((b (+ a 1)))
    (let ((c (+ b 1)))
      (let ((d (+ c 1)))
        (+ a b c d)))))            ; 多段ネストもコンパイル対象(16段まで)
```
マクロ展開後の`((lambda (v...) . body) i...)`という即時呼び出し(IIFE)形を、
実際の関数呼び出しを経由せずbodyへインライン展開する専用対応
(`za_compile_let`)があります。内側の`let`は外側の変数を正しくシャドーイング
します。

### 引数位置の`lambda`(クロージャ)

```lisp
(defun apply-twice (f x) (funcall f (funcall f x)))

(defun make-adder (n) (lambda (x) (+ x n)))   ; クロージャを返す

(defun with-callback (x)
  (apply-twice (lambda (y) (* y 2)) x))       ; 引数位置に直接lambdaを書ける
```

### `(function sym)` / `&rest`

```lisp
(defun call-it (x) (funcall (function +) x 1))   ; #'+ 相当

(defun my-list (&rest args) args)                ; rest引数の値をそのまま参照できる
(defun count-args (&rest args) (length args))
```

### quote(シンボル・リスト・ヒープ値すべて対応)

```lisp
(defun tag-of (x) (if (eq x 'foo) 'is-foo 'other))
(defun default-plist () '(a 1 b 2))    ; リストquoteもGC安全なスロット参照で対応
```

### 非局所脱出

```lisp
(defun find-first-negative (lst)
  (block found
    (dolist (x lst)                 ; dolistはマクロ展開でtagbody/go相当になる
      (if (< x 0) (return-from found x)))
    nil))

(defun safe-divide (a b)
  (catch 'div-error
    (if (= b 0) (throw 'div-error 'error) (/ a b))))

(defun with-cleanup (x)
  (unwind-protect
      (process x)
    (cleanup x)))                   ; JIT↔インタプリタ境界を越えて正しく伝播
```

### 動的変数

```lisp
(defdynamic *counter* 0)
(defun bump-counter () (dynamic *counter*))
```

### ILOSの関数本体(`slot-value`等)

```lisp
(defclass point () (x y))

(defun point-x (p) (slot-value p 'x))     ; slot-value自体の本体もJIT対象
(defun make-point (x y) (make-instance 'point 'x x 'y y))
```
`defclass`/`defgeneric`/`defmethod`自体はトップレベルのマクロ展開なのでJIT対象
外ですが、それらが呼ぶ`slot-value`/`set-slot-value`/`make-instance`/
`initialize-object`という**関数の本体**は現在JITコンパイル対象になっています。

## コンパイルできないパターン(常にフォールバック)

### 動的head呼び出し・非対応の特殊形式

```lisp
(defmacro my-when (test &rest body)
  `(if ,test (progn ,@body)))       ; quasiquoteはza未対応。展開後の関数がquasiquoteを
                                     ; 直接使う場合はフォールバックする

(defglobal *table* (make-hash-table))  ; defglobal自体は常にフォールバック要因

(flet ((sq (x) (* x x)))            ; flet/labelsはza未対応
  (sq 5))
```

### let-local以外への`setq`

```lisp
(defun bad-setq (x)
  (setq x (+ x 1))    ; 固定引数xへのsetqは非対応→フォールバック
  x)

(defdynamic *g* 0)
(defun bad-setq-dynamic ()
  (setq *g* (+ (dynamic *g*) 1))    ; 動的変数へのsetqも非対応→フォールバック
  (dynamic *g*))
```

### ILOSの複数ディスパッチ

```lisp
(defgeneric combine (a b))
(defmethod combine ((a shape) (b shape)) ...)  ; 複数ディスパッチ自体はinit.lisp側
                                                ; (ISLisp処理系)が単一ディスパッチの
                                                ; ままのため、JIT固有の制約ではない
```

## できる場合でもこうなるとできない境界(エッジケース)

すべて**固定サイズのスタックスロット**をコンパイル時に静的に割り付ける設計に
起因する、静的なネスト深さ/個数の上限です。実行時の再帰呼び出し回数(自己再帰・
相互再帰)そのものには影響しません(通常の`call`/`return`で処理され無制限)。
超えると、実行前のコンパイル時点で丸ごとフォールバックが決まります。

### `let`の多段ネスト(16段、1let内4変数まで)

```lisp
;; OK: 16段以内
(let ((a1 1)) (let ((a2 2)) (let ((a3 3)) ... )))

;; NG: 17段目に達すると全体がフォールバック
(let ((a1 1)) (let ((a2 2)) ... (let ((a17 17)) (+ a1 a17)) ...))

;; NG: 1つのletで5変数以上を同時に束縛
(let ((a 1) (b 2) (c 3) (d 4) (e 5))   ; ZA_MAX_LOCALS_PER_LET(4)を超える
  (+ a b c d e))
```

### 呼び出し引数位置の式ネスト(4段まで)

```lisp
;; OK: 4段
(defun ok-nest (x) (f (g (h (i x)))))

;; NG: 5段目でフォールバック
(defun bad-nest (x) (f (g (h (i (j x))))))
```

### 算術/比較式のネスト(4段まで、呼び出しネストとは別カウンタ)

```lisp
;; OK
(defun ok-arith (a b c d) (+ (- (* a b) c) d))

;; NG: 5段以上のネストでフォールバック
(defun bad-arith (a b c d e)
  (+ (- (* (+ a (- b c)) d) e) 1))
```

### 比較演算子はちょうど2項のみ

```lisp
(defun bad-lt (a b c) (< a b c))   ; 3項の<は非対応→フォールバック
(defun bad-lt2 (a) (< a))          ; 1項も非対応
```

### `go`は関数境界・同一`tagbody`境界を越えられない

```lisp
(defun loop-sum (n)
  (let ((i 0) (acc 0))
    (tagbody
     top
       (if (= i n) (go done))
       (setq acc (+ acc i))
       (setq i (+ i 1))
       (go top)               ; OK: 同一tagbody内のラベルへのgo
     done)
    acc))

;; NG(コンセプト例): catch/unwind-protectのスパニングを跨いでラベルへ戻るgoは
;; 静的な深さ不一致としてコンパイルを断念する
(defun bad-go ()
  (tagbody
   top
     (catch 'x
       (go top))))              ; catchの奥からtagbodyラベルへ戻るgoは非対応
```
一方`block`/`return-from`・`catch`/`throw`は制御転送値の伝播方式なので、
関数境界を越えても正しく動きます(`go`だけが特別に静的ジャンプへ解決される
点に注意)。

### `setq`とエスケープする`lambda`の併用

```lisp
;; NG: let-localへのsetqと、外側変数を捕捉して関数外へ返るlambdaが同居 → 全体フォールバック
(defun bad-combo ()
  (let ((acc 0))
    (setq acc (+ acc 1))
    (lambda () acc)))       ; accを捕捉して関数外へ返すクロージャ

;; OK: setqだけなら問題ない
(defun ok-setq-only ()
  (let ((acc 0))
    (setq acc (+ acc 1))
    acc))

;; OK: 捕捉lambdaだけ(setqが無い)なら問題ない
(defun ok-lambda-only ()
  (let ((acc 0))
    (lambda () acc)))
```
理由はクロージャキャプチャがlet-localの値を捕捉時に1回だけコピーする実装のため、
コピー後に`setq`で書き換えてもクロージャ側には反映されない食い違いが起きるのを
安全側に倒して避けているためです。

### 固定引数の個数(16個まで)

```lisp
(defun many-args (a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 a16) a1)  ; OK
;; 17個目以降を追加するとZA_MAX_PARAMSを超えフォールバック
```

## まとめ表: 静的サイズ上限

| 制限 | 定数 | 値 |
|---|---|---|
| 固定引数の個数 | `ZA_MAX_PARAMS` | 16 |
| 算術/呼び出しの引数・項数 | `ZA_MAX_OPERANDS` | 16 |
| letのネスト段数 | `ZA_MAX_LET_DEPTH` | 16 |
| 1let内の同時束縛変数数 | `ZA_MAX_LOCALS_PER_LET` | 4 |
| 呼び出し引数位置の式ネスト段数 | `ZA_MAX_CALL_DEPTH` | 4 |
| 算術/比較式のネスト段数 | `ZA_MAX_ARITH_DEPTH` | 4 |
| 非局所脱出のスパニング段数 | `ZA_MAX_NLX_DEPTH` | 4 |
| lambdaスロット数(1関数あたり) | `ZA_MAX_LAMBDA_SLOTS` | 32 |
| シンボルquoteスロット数 | `ZA_MAX_QUOTE_SLOTS` | 32 |
| 裸float/bignumリテラルスロット数 | `ZA_MAX_NUMBER_SLOTS` | 32 |
| tagbodyラベル数 | `ZA_MAX_TAGBODY_TAGS` | 16 |
| 1ラベルあたりgoの個数 | `ZA_MAX_TAGBODY_GOTOS_PER_TAG` | 8 |
| JITコード全体のバッファサイズ | `JIT_CODE_SIZE` | 512KB |

## 参考: `za_is_excluded_special_form`に残るシンボル

`quote`/`defun`/`lambda`/`defmacro`/`quasiquote`/`function`/`flet`/`labels`/
`defvar`/`defconstant`/`defglobal`。このうち`quote`/`function`/`lambda`は
一般呼び出しとして誤解釈しないための安全網として残っているだけで、実際には
専用の分岐(`za_classify_operand`等)で構文的に対応済みです。実質的に今も
専用コンパイル処理が無く常にフォールバックするのは`quasiquote`/`flet`/`labels`/
`defvar`/`defconstant`/`defglobal`の6つです。
