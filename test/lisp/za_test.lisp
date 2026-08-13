;; test/lisp/za_test.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)がどのdefunをコンパイルし、
;; どれをインタプリタへフォールバックするかを検証するテスト。
;;
;; za.cが出力する機械語は実機ビルド(mingw-gcc, MS x64呼び出し規約, 実行可能メモリ)を
;; 前提としており、ネイティブgccでビルドするユニットテスト(test/c/eval_test.c)では
;; ABIやメモリ実行属性が一致しないため、ユニットテスト側ではJIT自体を常に無効化して
;; いる(za.c参照)。そのため実際のJITコンパイル判定・生成された機械語の実行結果の検証は
;; このファイルをQEMU実機上でloadすることでのみ行う(make test-qemu)。
;;
;; %%za-compiled-p は第一引数の関数がzaで機械語へコンパイルされたかどうかを判定する
;; テスト用の内部組み込み関数(ISLisp仕様外なので%%を付ける、runtime.c参照)。
;; 本ファイルは test/lisp/isiki_test.lisp が定義する assert-equal 等をそのまま使う。

;; (+ x y z) : オペランド3個の+はzaでコンパイルされる
(defun isiki-za-test-add3 (x y z) (+ x y z))
(assert-equal t (%%za-compiled-p (function isiki-za-test-add3)))
(assert-equal 6 (isiki-za-test-add3 1 2 3))

;; &restはあるがbodyから参照しないparamsはzaでコンパイルされる
(defun isiki-za-test-add-fixed (x &rest y) (+ x 1))
(assert-equal t (%%za-compiled-p (function isiki-za-test-add-fixed)))
(assert-equal 11 (isiki-za-test-add-fixed 10 20 30))

;; rest引数名を+のオペランドに使うとzaはコンパイルを諦めインタプリタにフォールバックする
(defun isiki-za-test-bad-rest (x &rest y) (+ x y))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-bad-rest)))

;; xを17個並べてzaのオペランド上限(16個)を超えるとインタプリタにフォールバックする
(defun isiki-za-test-add17 (x)
  (+ x x x x x x x x x x x x x x x x x))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-add17)))
(assert-equal 17 (isiki-za-test-add17 1))

;; (if x 1 2) : else付きifはzaでコンパイルされる
(defun isiki-za-test-pick (x) (if x 1 2))
(assert-equal t (%%za-compiled-p (function isiki-za-test-pick)))
(assert-equal 1 (isiki-za-test-pick 0))
(assert-equal 2 (isiki-za-test-pick nil))

;; (if x 1) : else無しifもzaでコンパイルされる
(defun isiki-za-test-pick-noelse (x) (if x 1))
(assert-equal t (%%za-compiled-p (function isiki-za-test-pick-noelse)))
(assert-equal 1 (isiki-za-test-pick-noelse 5))
(assert-equal nil (isiki-za-test-pick-noelse nil))

;; ifのネストもzaでコンパイルされる
(defun isiki-za-test-pick2 (x y) (if x (if y 1 2) 3))
(assert-equal t (%%za-compiled-p (function isiki-za-test-pick2)))
(assert-equal 1 (isiki-za-test-pick2 1 1))
(assert-equal 2 (isiki-za-test-pick2 1 nil))
(assert-equal 3 (isiki-za-test-pick2 nil 1))

;; ifのthen/elseの中に+を書いてもzaでコンパイルされる
(defun isiki-za-test-addif (x y) (if x (+ x y) 0))
(assert-equal t (%%za-compiled-p (function isiki-za-test-addif)))
(assert-equal 7 (isiki-za-test-addif 3 4))
(assert-equal 0 (isiki-za-test-addif nil 4))

;; +のオペランドの位置に直接ifを書くとzaはコンパイルを諦めインタプリタにフォールバックする
(defun isiki-za-test-bad (x y) (+ x (if y 1 2)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-bad)))
(assert-equal 4 (isiki-za-test-bad 3 5))
(assert-equal 5 (isiki-za-test-bad 3 nil))

;; (- x y) : 2引数の-はzaでコンパイルされる
(defun isiki-za-test-sub (x y) (- x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-sub)))
(assert-equal 7 (isiki-za-test-sub 10 3))

;; (- x) : 単項の-もzaでコンパイルされる
(defun isiki-za-test-neg (x) (- x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-neg)))
(assert-equal (- 5) (isiki-za-test-neg 5))
(assert-equal 0 (isiki-za-test-neg 0))

;; (- x y z) : 3引数の-もzaでコンパイルされる
(defun isiki-za-test-sub3 (x y z) (- x y z))
(assert-equal t (%%za-compiled-p (function isiki-za-test-sub3)))
(assert-equal 5 (isiki-za-test-sub3 10 3 2))

;; (* x y z) : 3引数の*はzaでコンパイルされる
(defun isiki-za-test-mul3 (x y z) (* x y z))
(assert-equal t (%%za-compiled-p (function isiki-za-test-mul3)))
(assert-equal 24 (isiki-za-test-mul3 2 3 4))

;; (* x) : オペランド1個の*はzaのコンパイル対象外でインタプリタにフォールバックする
(defun isiki-za-test-ident (x) (* x))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-ident)))
(assert-equal 9 (isiki-za-test-ident 9))

;; (< x y) : 2引数の<はzaでコンパイルされる
(defun isiki-za-test-lt (x y) (< x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-lt)))
(assert-equal t (isiki-za-test-lt 1 2))
(assert-equal nil (isiki-za-test-lt 2 1))

;; (= x y) : 2引数の=はzaでコンパイルされる
(defun isiki-za-test-numeq (x y) (= x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-numeq)))
(assert-equal t (isiki-za-test-numeq 3 3))
(assert-equal nil (isiki-za-test-numeq 3 4))

;; (< x y z) : 3引数の<はzaのコンパイル対象外でインタプリタにフォールバックする
(defun isiki-za-test-lt3 (x y z) (< x y z))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-lt3)))
(assert-equal t (isiki-za-test-lt3 1 2 3))
(assert-equal nil (isiki-za-test-lt3 1 3 2))

;; ifのthen/elseの中に-や*を書いてもzaでコンパイルされる
(defun isiki-za-test-calc (x y) (if (< x y) (- y x) (* x y)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-calc)))
(assert-equal 2 (isiki-za-test-calc 3 5))
(assert-equal 15 (isiki-za-test-calc 5 3))

;; -のオペランドに直接ifを書くとzaはコンパイルを諦めインタプリタにフォールバックする
(defun isiki-za-test-bad2 (x y) (- x (if y 1 2)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-bad2)))
(assert-equal 9 (isiki-za-test-bad2 10 5))
(assert-equal 8 (isiki-za-test-bad2 10 nil))
