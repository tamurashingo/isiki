;; test/lisp/za_test_ext13.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張13
;; (裸の[quoteしていない]float/bignumリテラルをオペランドとして分類できるようにする対応、
;; および比較演算子>/<=/>=を既存の</=と同じ経路で追加する対応)を検証するテスト。
;; za_test_ext5.lisp/za_test_ext7.lisp〜za_test_ext12.lispと同様に独立ファイルとして
;; 切り出す。za.cのグラマー全体・za_test.lisp(拡張0〜4)については
;; test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結)。
;;
;; 演算そのもの(+/-/*)のコード生成は無変更で、ランタイム側(primitive_add2等)が
;; 元々bignum/floatに対応済みだったため実行結果は正しい。今回za.c側で追加したのは
;; 「裸のfloat/bignumリテラルをza_classify_operandが分類できる」ことと
;; 「>/<=/>=をza_compile_binary経由でディスパッチする」ことの2点のみ。

;;; --- 1. 裸のfloatリテラルを+のオペランドに使う ---

;; equalはfloat同士の値比較に対応していない(TAG_INSTANCEケースはMAGIC_BIGNUM/
;; MAGIC_VECTORのみでMAGIC_FLOATは未実装)ため、新たに確保されたfloatオブジェクトは
;; 値が同じでもassert-equalでは真にならない。isiki_test.lispの既存の慣習
;; (float (float 2)等)に合わせてassert-float-closeを使う。

(defun isiki-za-test-num-float-literal-add (x)
  (+ x 1.5))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-float-literal-add)))
(assert-float-close 3.5 (isiki-za-test-num-float-literal-add 2))

;;; --- 2. 裸のbignumリテラルを+のオペランドに使う(fixnum範囲を超える整数リテラル) ---

(defun isiki-za-test-num-bignum-literal-add (x)
  (+ x 100000000000000000000))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-bignum-literal-add)))
(assert-equal 100000000000000000001 (isiki-za-test-num-bignum-literal-add 1))

;;; --- 3. fixnum同士の演算が実行時にbignumへ昇格するケース(リテラル自体はfixnum範囲内) ---
;;
;; 2^30 * 2^30 = 2^60 はFIXNUM_MAGNITUDE_MASK(60bit、最大2^60-1)を超えるため
;; 自動的にbignumへ昇格する。オペランドはparam経由(既存の分類ルート)であり、
;; ここではza_compile_fold自体は無変更のまま実行結果が正しいことのみを確認する。

(defun isiki-za-test-num-fixnum-overflow-mul (x y)
  (* x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-fixnum-overflow-mul)))
(assert-equal 1152921504606846976 (isiki-za-test-num-fixnum-overflow-mul 1073741824 1073741824))

;;; --- 4. floatリテラルをifのtest位置で使う(裸リテラルのleaf classify確認) ---
;;
;; このLispではnil以外はすべて真なので、0.0も真になる(Cのゼロ真偽とは異なる)。

(defun isiki-za-test-num-float-if-zero ()
  (if 0.0 'nonzero-truthy 'unreachable))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-float-if-zero)))
(assert-equal 'nonzero-truthy (isiki-za-test-num-float-if-zero))

(defun isiki-za-test-num-float-if-nonzero ()
  (if 3.14 'truthy 'unreachable))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-float-if-nonzero)))
(assert-equal 'truthy (isiki-za-test-num-float-if-nonzero))

;;; --- 5. > の基本動作(境界値含む) ---

(defun isiki-za-test-num-gt (x y) (> x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-gt)))
(assert-equal t (isiki-za-test-num-gt 5 3))
(assert-equal nil (isiki-za-test-num-gt 3 5))
(assert-equal nil (isiki-za-test-num-gt 3 3))

;;; --- 6. <= の基本動作(境界値含む) ---

(defun isiki-za-test-num-le (x y) (<= x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-le)))
(assert-equal t (isiki-za-test-num-le 3 5))
(assert-equal t (isiki-za-test-num-le 3 3))
(assert-equal nil (isiki-za-test-num-le 5 3))

;;; --- 7. >= の基本動作(境界値含む) ---

(defun isiki-za-test-num-ge (x y) (>= x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-ge)))
(assert-equal t (isiki-za-test-num-ge 5 3))
(assert-equal t (isiki-za-test-num-ge 3 3))
(assert-equal nil (isiki-za-test-num-ge 3 5))

;;; --- 8. 比較演算子とfloat/bignumリテラルの組み合わせ ---

(defun isiki-za-test-num-gt-float-literal (x) (> x 3.14))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-gt-float-literal)))
(assert-equal t (isiki-za-test-num-gt-float-literal 4.0))
(assert-equal nil (isiki-za-test-num-gt-float-literal 2.0))

(defun isiki-za-test-num-le-bignum-literal (x) (<= x 100000000000000000000))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-le-bignum-literal)))
(assert-equal t (isiki-za-test-num-le-bignum-literal 1))
(assert-equal nil (isiki-za-test-num-le-bignum-literal 200000000000000000000))

;;; --- 9. > をifのtest位置で使う(allow_call=0ゲートを迂回して通ることの確認) ---
;;
;; <と全く同じ経路(allow_callゲートより前のヘッドシンボル判定)を通るため、
;; 引数位置での一般呼び出し不可制約とは無関係にコンパイルされる。

(defun isiki-za-test-num-gt-if-test (x y)
  (if (> x y) 'x-bigger 'y-bigger-or-equal))
(assert-equal t (%%za-compiled-p (function isiki-za-test-num-gt-if-test)))
(assert-equal 'x-bigger (isiki-za-test-num-gt-if-test 5 3))
(assert-equal 'y-bigger-or-equal (isiki-za-test-num-gt-if-test 3 5))
(assert-equal 'y-bigger-or-equal (isiki-za-test-num-gt-if-test 3 3))
