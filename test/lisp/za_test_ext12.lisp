;; test/lisp/za_test_ext12.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張12
;; (let-IIFEインライン化のネスト深さ上限をZA_MAX_LET_DEPTH=4から8へ拡張し、
;; クロージャがより深いネストのlet局所変数を正しく捕捉できるようにする対応)
;; を検証するテスト。za_test_ext5.lisp/za_test_ext7.lisp〜za_test_ext11.lispと
;; 同様に独立ファイルとして切り出す。za.cのグラマー全体・za_test.lisp
;; (拡張0〜4)については test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等を
;; そのまま使う(boot-entryスクリプトが本ファイルより先にそれをloadしている
;; 前提)。他のext系ファイルへの依存は無い(自己完結)。
;;
;; za_compile_letのdepth(祖先let数、0始まり)は、N段ネストのうち最内側の
;; N段目でdepth=N-1になる。depth >= ZA_MAX_LET_DEPTHになった時点で
;; その`let`自体のコンパイルが失敗し、defun全体がインタプリタへfallbackする。
;;
;; mandelbrot.lisp投入時にZA_MAX_LET_DEPTHをさらに8から16へ拡張したため
;; (with-standard-outputのdynamic-let2段+for×2段+明示let1段+for1段+
;; let*3段+let*5段=最大深さ13に達することを実測した)、以下3./4.の境界確認も
;; 新上限(16段成功・17段目失敗)に合わせて更新する。

;;; --- 1. 4段ネスト(旧上限ちょうど、回帰確認) ---

(defun isiki-za-test-let-depth-4 ()
  (let ((v1 1))
    (let ((v2 2))
      (let ((v3 3))
        (let ((v4 4))
          (lambda () (+ v1 v2 v3 v4)))))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-depth-4)))
(assert-equal 10 (funcall (isiki-za-test-let-depth-4)))

;;; --- 2. 5段ネスト(旧上限を超え、新上限内) ---

(defun isiki-za-test-let-depth-5 ()
  (let ((v1 1))
    (let ((v2 2))
      (let ((v3 3))
        (let ((v4 4))
          (let ((v5 5))
            (lambda () (+ v1 v2 v3 v4 v5))))))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-depth-5)))
(assert-equal 15 (funcall (isiki-za-test-let-depth-5)))

;;; --- 3. 8段ネスト(旧上限ちょうど、新上限内での回帰確認) ---

(defun isiki-za-test-let-depth-8 ()
  (let ((v1 1))
    (let ((v2 2))
      (let ((v3 3))
        (let ((v4 4))
          (let ((v5 5))
            (let ((v6 6))
              (let ((v7 7))
                (let ((v8 8))
                  (lambda () (+ v1 v2 v3 v4 v5 v6 v7 v8)))))))))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-depth-8)))
(assert-equal 36 (funcall (isiki-za-test-let-depth-8)))

;;; --- 4. 16段ネスト(新上限ちょうど) ---

(defun isiki-za-test-let-depth-16 ()
  (let ((v1 1))
    (let ((v2 2))
      (let ((v3 3))
        (let ((v4 4))
          (let ((v5 5))
            (let ((v6 6))
              (let ((v7 7))
                (let ((v8 8))
                  (let ((v9 9))
                    (let ((v10 10))
                      (let ((v11 11))
                        (let ((v12 12))
                          (let ((v13 13))
                            (let ((v14 14))
                              (let ((v15 15))
                                (let ((v16 16))
                                  (lambda ()
                                    (+ v1 v2 v3 v4 v5 v6 v7 v8
                                       v9 v10 v11 v12 v13 v14 v15 v16)))))))))))))))))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-depth-16)))
(assert-equal 136 (funcall (isiki-za-test-let-depth-16)))

;;; --- 5. 17段ネスト(新上限超過)。コンパイルは失敗しインタプリタへ
;;; fallbackするが、fallback後もインタプリタの通常の環境チェーンにより
;;; 結果自体は正しいことを確認する ---

(defun isiki-za-test-let-depth-17 ()
  (let ((v1 1))
    (let ((v2 2))
      (let ((v3 3))
        (let ((v4 4))
          (let ((v5 5))
            (let ((v6 6))
              (let ((v7 7))
                (let ((v8 8))
                  (let ((v9 9))
                    (let ((v10 10))
                      (let ((v11 11))
                        (let ((v12 12))
                          (let ((v13 13))
                            (let ((v14 14))
                              (let ((v15 15))
                                (let ((v16 16))
                                  (let ((v17 17))
                                    (lambda ()
                                      (+ v1 v2 v3 v4 v5 v6 v7 v8
                                         v9 v10 v11 v12 v13 v14 v15 v16 v17))))))))))))))))))))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-let-depth-17)))
(assert-equal 153 (funcall (isiki-za-test-let-depth-17)))
