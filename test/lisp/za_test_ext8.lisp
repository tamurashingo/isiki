;; test/lisp/za_test_ext8.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張8(let-IIFEインライン化、
;; 通称「案B」)を検証するテスト。za_test_ext5.lisp/za_test_ext7.lispと同様に独立ファイル
;; として切り出す。za.cのグラマー全体・za_test.lisp(拡張0〜4)については
;; test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結)。
;;
;; let-IIFEインライン化は「letを特別扱いする」のではなく、マクロ展開後に
;; `((lambda (v1 v2...) body...) i1 i2...)`という即時呼び出し(IIFE)の形になった
;; call全般を、実際の関数呼び出し手続きを経ずbodyをその場にインライン展開する、
;; という一般的な最適化である。したがってlet/let*/orのようにマクロ展開後この形に
;; 帰着するものすべてに自動的に効く(setqを使うforのみza_is_excluded_special_form
;; による既存の除外で対象外のまま、documents/let.md参照)。

;;; --- 1. 基本のlet-IIFEインライン化 ---

(defun isiki-za-test-let-basic (x)
  (let ((y 1))
    (+ x y)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-basic)))
(assert-equal 11 (isiki-za-test-let-basic 10))

;;; --- 2. シャドーイング(letローカルが外側paramと同名) ---

(defun isiki-za-test-let-shadow-param (x)
  (let ((x (* x 2)))
    (+ x 1)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-shadow-param)))
(assert-equal 21 (isiki-za-test-let-shadow-param 10))

;;; --- 3. ネストしたlet(内側letが外側letと同名変数でシャドーする) ---

(defun isiki-za-test-let-nested-shadow (x)
  (let ((v (+ x 1)))
    (let ((v (* v 10)))
      (+ v x))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-nested-shadow)))
(assert-equal 43 (isiki-za-test-let-nested-shadow 3))

;;; --- 4. let*(ネスト展開の繰り返し適用による逐次束縛) ---

;; za.cの+は現時点でオペランドがleaf(局所変数/固定引数/fixnumリテラル)限定で、
;; ネストした(+ .. (+ ..))自体は非対応(let/prognとは無関係の既存の制約)なので、
;; ここでは(+ a b c)のようにフラットな複数オペランドで検証する。
(defun isiki-za-test-let-star (x)
  (let* ((a (+ x 1))
         (b (+ a 1))
         (c (+ b 1)))
    (+ a b c)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-star)))
(assert-equal 21 (isiki-za-test-let-star 5))

;;; --- 5. ZA_MAX_LET_DEPTH境界(17段ネストで上限超えしfallback) ---
;;
;; mandelbrot.lisp投入時にZA_MAX_LET_DEPTHを8から16へ拡張したため、ここでの
;; 境界確認も17段ネスト(新上限を1段超える)に更新する。境界そのものの網羅的な
;; 検証(4/5/8/16/17段)は test/lisp/za_test_ext12.lisp を参照。
;;
;; 結果自体はインタプリタ経由で正しいことを確認する(インライン化されないだけで
;; defun全体は依然インタプリタで正しく実行される)。

(defun isiki-za-test-let-depth-over (x)
  (let ((a 1))
    (let ((b 2))
      (let ((c 3))
        (let ((d 4))
          (let ((e 5))
            (let ((f 6))
              (let ((g 7))
                (let ((h 8))
                  (let ((i 9))
                    (let ((j 10))
                      (let ((k 11))
                        (let ((l 12))
                          (let ((m 13))
                            (let ((n 14))
                              (let ((o 15))
                                (let ((p 16))
                                  (let ((q 17))
                                    (+ x a b c d e f g h i j k l m n o p q)))))))))))))))))))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-let-depth-over)))
(assert-equal 163 (isiki-za-test-let-depth-over 10))

;;; --- 6. return-fromによる早期脱出(let body内・init式評価中の両方) ---

(defun isiki-za-test-let-return-from-body (x)
  (block done
    (let ((y (+ x 1)))
      (if (eq y 5) (return-from done 'early) nil)
      (+ y 100))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-return-from-body)))
(assert-equal 'early (isiki-za-test-let-return-from-body 4))
(assert-equal 102 (isiki-za-test-let-return-from-body 1))

(defun isiki-za-test-let-return-from-init (x)
  (block done
    (let ((y (if (eq x 0) (return-from done 'init-early) (+ x 1))))
      (+ y 100))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-return-from-init)))
(assert-equal 'init-early (isiki-za-test-let-return-from-init 0))
(assert-equal 106 (isiki-za-test-let-return-from-init 5))

;;; --- 7. letが末尾位置にあり、body最後が自己再帰呼び出しの場合の末尾呼び出し保持 ---
;;
;; 大きなnで深い再帰がスタックオーバーフローしないことで、インライン化後もこの
;; 自己再帰呼び出しがトランポリン経由の末尾呼び出しのまま保たれていることを
;; 間接的に確認する。

(defun isiki-za-test-let-tail-loop (n acc)
  (let ((m (- n 1)))
    (if (eq n 0)
        acc
        (isiki-za-test-let-tail-loop m (+ acc 1)))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-tail-loop)))
(assert-equal 200000 (isiki-za-test-let-tail-loop 200000 0))

;;; --- 8. let-localをキャプチャするlambda(クロージャキャプチャ対策の検証) ---

;; 単純呼び出し
(defun isiki-za-test-let-capture-simple (x)
  (let ((y (+ x 10)))
    (lambda () y)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-capture-simple)))
(assert-equal 15 (funcall (isiki-za-test-let-capture-simple 5)))

;; 呼び出し元へ返して後から呼ぶケース(複数の独立したクロージャを同時に保持しても
;; 互いに影響しないこと)
(defun isiki-za-test-let-capture-later (x)
  (let ((y (* x 2)))
    (lambda (z) (+ y z))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-capture-later)))
(let ((c1 (isiki-za-test-let-capture-later 3))
      (c2 (isiki-za-test-let-capture-later 10)))
  (assert-equal 16 (funcall c1 10))
  (assert-equal 25 (funcall c2 5))
  (assert-equal 16 (funcall c1 10)))

;; ネストしたletの外側/内側両方をキャプチャし、シャドーイングも保つケース
(defun isiki-za-test-let-capture-nested (x)
  (let ((p (+ x 1)) (a 1))
    (let ((q (+ x 2)) (a (+ a 100)))
      (lambda () (list p q a)))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-capture-nested)))
(assert-equal (list 6 7 101) (funcall (isiki-za-test-let-capture-nested 5)))

;;; --- 9. &rest付きlambdaのIIFEは非対応でfallback ---

(defun isiki-za-test-let-rest-fallback (x)
  ((lambda (&rest xs) (car xs)) x (+ x 1)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-let-rest-fallback)))
(assert-equal 10 (isiki-za-test-let-rest-fallback 10))

;;; --- 10. or(setqを使わないため、let対応後は自動的にコンパイル対象になることの回帰確認) ---

(defun isiki-za-test-let-or-regression (x y) (or x y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-let-or-regression)))
(assert-equal 1 (isiki-za-test-let-or-regression 1 2))
(assert-equal 2 (isiki-za-test-let-or-regression nil 2))
