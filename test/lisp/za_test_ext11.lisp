;; test/lisp/za_test_ext11.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張11
;; ((function sym)、すなわち#'sym形式のシンボル関数参照)を検証するテスト。
;; za_test_ext5.lisp/za_test_ext7.lisp/za_test_ext8.lisp/za_test_ext9.lisp/
;; za_test_ext10.lispと同様に独立ファイルとして切り出す。za.cのグラマー全体・
;; za_test.lisp(拡張0〜4)については test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結)。
;;
;; 対応範囲は(function sym)のsymがシンボルの場合のみ(v1スコープ)。
;; (function (lambda ...))のような非シンボルは引き続きfallbackする。

;;; --- 1. (function 組み込み関数)をfuncallに渡す ---

(defun isiki-za-test-function-funcall (x)
  (funcall (function car) x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-function-funcall)))
(assert-equal 1 (isiki-za-test-function-funcall '(1 2 3)))

;;; --- 2. (function 組み込み関数)をmapcarに渡す ---

(defun isiki-za-test-function-mapcar (lst)
  (mapcar (function car) lst))
(assert-equal t (%%za-compiled-p (function isiki-za-test-function-mapcar)))
(assert-equal '(1 3) (isiki-za-test-function-mapcar '((1 2) (3 4))))

;;; --- 3. ユーザー定義関数への(function sym)参照 ---

(defun isiki-za-test-function-helper (x)
  (* x 10))

(defun isiki-za-test-function-user-defined (x)
  (funcall (function isiki-za-test-function-helper) x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-function-user-defined)))
(assert-equal 30 (isiki-za-test-function-user-defined 3))

;;; --- 4. (function (lambda ...))(非シンボル)はfallback ---

(defun isiki-za-test-function-nonsymbol-fallback (x)
  (funcall (function (lambda (y) (+ y 1))) x))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-function-nonsymbol-fallback)))
(assert-equal 6 (isiki-za-test-function-nonsymbol-fallback 5))
