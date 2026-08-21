;; test/lisp/za_test_ext9.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張9(setqによるlet-localの
;; 再代入、および前提となるblock nil/return-from nilの修正)を検証するテスト。
;; za_test_ext5.lisp/za_test_ext7.lisp/za_test_ext8.lispと同様に独立ファイルとして
;; 切り出す。za.cのグラマー全体・za_test.lisp(拡張0〜4)については
;; test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結)。
;;
;; setqの対応範囲はlet-localのみ(v1スコープ)。外側defunの固定引数・dynamic変数への
;; 再代入は書き込み可能なスロットが無いため引き続きfallbackする(defglobal変数・
;; 未束縛シンボルへのsetqは拡張19(za_test_ext19.lisp参照)で別途対応済み)。
;; また、setq対象のlet-localを、setqより前に生成されたクロージャがキャプチャしている
;; 場合、クロージャ側は生成時点の値をコピーするだけなのでインタプリタの参照共有
;; セマンティクスと食い違う可能性があるため、同一defun内にsetqとエスケープするlambda
;; (値の位置に現れる裸のlambda)の両方が存在する場合は安全側に倒して全体をfallbackさせる
;; (documents/let.md参照)。

;;; --- 0. block/return-from with name=nil (for/whileの前提となる別修正の検証) ---

(defun isiki-za-test-block-nil-basic (x)
  (block nil (+ x 1)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-block-nil-basic)))
(assert-equal 11 (isiki-za-test-block-nil-basic 10))

(defun isiki-za-test-return-from-nil-basic (x)
  (block nil
    (if (< x 0) (return-from nil 'negative) nil)
    (+ x 1)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-return-from-nil-basic)))
(assert-equal 'negative (isiki-za-test-return-from-nil-basic -1))
(assert-equal 6 (isiki-za-test-return-from-nil-basic 5))

;;; --- 1. let-localへの基本のsetq ---

(defun isiki-za-test-setq-basic (x)
  (let ((y 1))
    (setq y (+ x y))
    y))
(assert-equal t (%%za-compiled-p (function isiki-za-test-setq-basic)))
(assert-equal 11 (isiki-za-test-setq-basic 10))

(defun isiki-za-test-setq-return-value (x)
  (let ((y 1))
    (setq y (+ x y))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-setq-return-value)))
(assert-equal 11 (isiki-za-test-setq-return-value 10))

;;; --- 2. tagbody/goループ内でのsetq(forが必要とする形の直接検証) ---

(defun isiki-za-test-setq-tagbody-loop (n)
  (let ((i 0) (sum 0))
    (block done
      (tagbody
       loop
       (if (eq i n)
           (return-from done sum)
           (progn
             (setq sum (+ sum i))
             (setq i (+ i 1))
             (go loop)))))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-setq-tagbody-loop)))
(assert-equal 45 (isiki-za-test-setq-tagbody-loop 10))

;;; --- 3. forマクロそのものがJITコンパイル対象になること(本命の実利) ---

(defun isiki-za-test-setq-for-loop (n)
  (let ((sum 0))
    (for ((i 0 (+ i 1))) ((eq i n) sum)
      (setq sum (+ sum i)))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-setq-for-loop)))
(assert-equal 10 (isiki-za-test-setq-for-loop 5))

;;; --- 4. 外側defunの固定引数へのsetqは対象外でfallback ---

(defun isiki-za-test-setq-param-fallback (x)
  (setq x (+ x 1))
  x)
(assert-equal nil (%%za-compiled-p (function isiki-za-test-setq-param-fallback)))
(assert-equal 11 (isiki-za-test-setq-param-fallback 10))

;;; --- 5. setq + エスケープするlambdaのキャプチャ(box昇格の検証) ---
;;
;; yはsetqされ、かつエスケープするlambda fに捕捉されるためbox昇格の対象(ZA_VAR_BOXED)
;; になる。以前はこの組み合わせを関数全体レベルの粗い安全網でインタプリタへ
;; フォールバックさせていたが、変数単位box昇格(documents/let.mdの「最大のリスク」節
;; 参照、za_test.lispの「拡張7」も参照)によりzaでコンパイルされ、かつ正しい結果を
;; 返すようになった。

(defun isiki-za-test-setq-capture-escape (x)
  (let ((y x))
    (let ((f (lambda () y)))
      (setq y (+ y 100))
      (funcall f))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-setq-capture-escape)))
(assert-equal 105 (isiki-za-test-setq-capture-escape 5))

;;; --- 6. シャドーイングされた内側let-localへのsetq(外側同名変数は不変) ---

(defun isiki-za-test-setq-shadow (x)
  (let ((v x))
    (let ((v (* v 10)))
      (setq v (+ v 1)))
    v))
(assert-equal t (%%za-compiled-p (function isiki-za-test-setq-shadow)))
(assert-equal 3 (isiki-za-test-setq-shadow 3))

;;; --- 7. setqの値式中の早期脱出(return-from) ---

(defun isiki-za-test-setq-early-exit (x)
  (block done
    (let ((y 1))
      (setq y (if (eq x 0) (return-from done 'early) (+ y 100)))
      y)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-setq-early-exit)))
(assert-equal 'early (isiki-za-test-setq-early-exit 0))
(assert-equal 101 (isiki-za-test-setq-early-exit 5))
