;; test/lisp/za_test_ext16.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張16
;; (+/-/*・比較演算子・car/cdr/null/atom/eq/consのオペランド位置への複合式の
;; ネスト対応)を検証するテスト。za_test_ext5.lisp〜za_test_ext15.lispと同様に
;; 独立ファイルとして切り出す。za.cのグラマー全体・za_test.lisp(拡張0〜4)
;; については test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結、iz16-プレフィクスで独自にヘルパーを定義する)。
;;
;; 拡張16以前は+/-/*・比較・car/cdr/null/atom/eq/consのオペランドはleaf(局所変数/
;; 固定引数/fixnumリテラル等)限定で、オペランド位置に一般呼び出し・if・入れ子の
;; 算術/比較などを直接書くとza全体がコンパイルを諦めインタプリタへfallbackしていた
;; (test/lisp/za_test.lispのisiki-za-test-bad等、za_test_ext14.lisp/za_test_ext15.lisp
;; の該当コメント参照)。za_compile_operandヘルパー(オペランドをまずleaf分類し、
;; 失敗した場合のみza_compile_exprへ再帰する)の導入によりこの制約が解消された。

;;; --- 1. 算術オペランド位置への算術/比較のネスト ---

(defun iz16-nested-add (a b c) (+ (+ a b) c))
(assert-equal t (%%za-compiled-p (function iz16-nested-add)))
(assert-equal 6 (iz16-nested-add 1 2 3))

(defun iz16-mul-sub (a b c) (- (* a b) c))
(assert-equal t (%%za-compiled-p (function iz16-mul-sub)))
(assert-equal 17 (iz16-mul-sub 5 4 3))

;; まさにmandelbrot.lispの元パターン: 比較の第一オペランド位置に+のネスト。
(defun iz16-gt-add (a b c) (> (+ a b) c))
(assert-equal t (%%za-compiled-p (function iz16-gt-add)))
(assert-equal t (iz16-gt-add 3 4 5))
(assert-equal nil (iz16-gt-add 1 1 5))

;;; --- 2. 算術オペランド位置への一般呼び出しのネスト ---

(defun iz16-fn (a) (+ a 100))
(defun iz16-call-in-add (a b) (+ (iz16-fn a) b))
(assert-equal t (%%za-compiled-p (function iz16-call-in-add)))
(assert-equal 111 (iz16-call-in-add 1 10))

;;; --- 3. 算術オペランド位置へのifのネスト ---
;;
;; test/lisp/za_test.lispのisiki-za-test-badと同型を独立に再確認する。

(defun iz16-if-in-add (x y) (+ x (if y 1 2)))
(assert-equal t (%%za-compiled-p (function iz16-if-in-add)))
(assert-equal 4 (iz16-if-in-add 3 5))
(assert-equal 5 (iz16-if-in-add 3 nil))

;;; --- 4. car/cdr/cons/eq(binary/unary)オペランド位置への算術・呼び出しのネスト ---

(defun iz16-cons-nested (x) (cons (car x) (cdr x)))
(assert-equal t (%%za-compiled-p (function iz16-cons-nested)))
(assert-equal 1 (car (iz16-cons-nested (cons 1 2))))
(assert-equal 2 (cdr (iz16-cons-nested (cons 1 2))))

(defun iz16-eq-add (a b c) (eq (+ a b) c))
(assert-equal t (%%za-compiled-p (function iz16-eq-add)))
(assert-equal t (iz16-eq-add 2 3 5))
(assert-equal nil (iz16-eq-add 2 3 6))

;;; --- 5. 3重以上のネスト(ZA_MAX_ARITH_DEPTHの範囲内) ---

(defun iz16-triple-nested (a b c d) (+ (+ (+ a b) c) d))
(assert-equal t (%%za-compiled-p (function iz16-triple-nested)))
(assert-equal 10 (iz16-triple-nested 1 2 3 4))

;;; --- 6. ZA_MAX_ARITH_DEPTHを超えるネストは安全にfallback ---
;;
;; 2個目以降のオペランド評価はarith_depth+1で行われるため、右側へのネストを
;; 繰り返すとarith_depthが1段ずつ深くなる(za.c内のza_compile_foldのコメント参照)。
;; ZA_MAX_ARITH_DEPTH=4なので、5重右ネストの最内側の+はarith_depth=4で
;; コンパイルを断念し、外側の+も含めてdefun全体がインタプリタへfallbackする
;; (結果自体はインタプリタ経由で正しいことを確認する。iz15-depth-overと同趣旨)。

(defun iz16-depth-over (a b c d e f) (+ a (+ b (+ c (+ d (+ e f))))))
(assert-equal nil (%%za-compiled-p (function iz16-depth-over)))
(assert-equal 21 (iz16-depth-over 1 2 3 4 5 6))

;;; --- 7. NLX(非局所脱出)との組み合わせ ---
;;
;; オペランド評価中の制御転送(return-from)が、fold/binaryのct_patch/cleanup配線を
;; 経由して正しく外側のblockまで伝播することを確認する。

;; 1個目のオペランド(まだ何もlinkしていない、ct_patch0が直接終端へ合流する経路)。
(defun iz16-nlx-in-first-operand (x)
  (block done
    (+ (return-from done 7) x)))
(assert-equal t (%%za-compiled-p (function iz16-nlx-in-first-operand)))
(assert-equal 7 (iz16-nlx-in-first-operand 100))

;; 2個目のオペランド(1個目がすでにlinkされている、cleanupブロックでunlinkしてから
;; 終端へ落ちる経路)。
(defun iz16-nlx-in-second-operand (x)
  (block done
    (+ x (return-from done 42))))
(assert-equal t (%%za-compiled-p (function iz16-nlx-in-second-operand)))
(assert-equal 42 (iz16-nlx-in-second-operand 5))

;; binary(cons)側でも同じcleanup経路を確認する。
(defun iz16-nlx-in-cons-second (x)
  (block done
    (cons x (return-from done 'bailed))))
(assert-equal t (%%za-compiled-p (function iz16-nlx-in-cons-second)))
(assert-equal 'bailed (iz16-nlx-in-cons-second 1))

;;; --- 8. mandelbrot本体相当の統合確認 ---
;;
;; src/lisp/mandelbrot.lispの`(if (> (+ zr2 zi2) 4.0) ...)`と同じ形の簡約版。

(defun iz16-mandel-step (zr zi)
  (let ((zr2 (* zr zr))
        (zi2 (* zi zi)))
    (if (> (+ zr2 zi2) 4.0) 'hit 'continue)))
(assert-equal t (%%za-compiled-p (function iz16-mandel-step)))
(assert-equal 'hit (iz16-mandel-step 2.0 2.0))
(assert-equal 'continue (iz16-mandel-step 0.1 0.1))
