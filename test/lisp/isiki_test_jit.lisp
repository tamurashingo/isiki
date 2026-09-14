;; test/lisp/isiki_test_jit.lisp
;;
;; isiki_test.lisp(ISLisp 仕様 v23 の "Example:" 全件)の JIT 版。isiki_test.lisp の各
;; アサーションはトップレベルフォームとしてインタプリタで評価されるため、同じ式を
;; 引数なしの defun の本体に置いて JIT コンパイラ(za.c)にコンパイルさせ、
;;   (1) その関数が実際に JIT コンパイルされたこと((%%za-compiled-p (function p-xxxx-yyyy)))
;;   (2) 呼び出した結果が仕様通りであること
;; の2点を確認する。
;;
;; 関数名は p-xxxx-yyyy(xxxx: 仕様のページ番号 4桁0埋め、yyyy: そのページ内での順番
;; 4桁0埋め)。ページ番号は isiki_test.lisp の各テスト直前のコメント(p.N / cf. p.N)から
;; 取り、順番はスキップしたものも含めて isiki_test.lisp の出現順に振る。
;;
;; defun/defmacro/defglobal 等の定義フォームの戻り値を受け取るテストは JIT 版では
;; スキップする(";; [skip p-xxxx-yyyy]" のコメントを残し、定義そのものは後続のテストの
;; ために実行する)。定義フォームやストリームの準備などアサーション以外のトップレベル
;; フォームは isiki_test.lisp のまま実行する。
;;
;; このファイルは tools/gen_isiki_test_jit.py で isiki_test.lisp から生成する。手で直す
;; のではなく isiki_test.lisp を直して再生成すること。
;;
;; isiki_test.lisp と同じ boot で連続して load されるため、isiki_test.lisp が定義した
;; グローバル(x/today/*color*/str 等)は本ファイルの定義で上書きされる(同じ定義)。

;;; ===========================================================================
;;; §1-§12 導入・定義・評価 (p.7-28)
;;; ===========================================================================

;; p.7 (+ 3 4) => 7 (メタ記号 ⇒ の説明に使われている例)
(defun p-0007-0001 ()
  (+ 3 4))
(assert-equal t (%%za-compiled-p (function p-0007-0001)))
(assert-equal 7 (p-0007-0001))

;; p.19 (defun copy-cell (x) (cons (car x) (cdr x))) の例。defunの戻り値が関数名
;; シンボルであること(p.28の一般規則)の確認と、定義した関数の動作確認を追加。
(defglobal isiki-test-defun-copy-cell-result (defun copy-cell (x) (cons (car x) (cdr x))))

(defun p-0019-0001 ()
  isiki-test-defun-copy-cell-result)
(assert-equal t (%%za-compiled-p (function p-0019-0001)))
(assert-equal 'copy-cell (p-0019-0001))

(defun p-0019-0002 ()
  (copy-cell '(1 . 2)))
(assert-equal t (%%za-compiled-p (function p-0019-0002)))
(assert-equal '(1 . 2) (p-0019-0002))

;; p.22 ((lambda (x) (+ x x)) 4) => 8 をそのまま転記
(defun p-0022-0001 ()
  ((lambda (x) (+ x x)) 4))
(assert-equal t (%%za-compiled-p (function p-0022-0001)))
(assert-equal 8 (p-0022-0001))

;; p.23 (functionp (function car)) => t
(defun p-0023-0001 ()
  (functionp (function car)))
(assert-equal t (%%za-compiled-p (function p-0023-0001)))
(assert-equal t (p-0023-0001))

;; cf. p.23 functionp の説明に基づく非関数の例(仕様は #\a 等、ここでは整数)。
(defun p-0023-0002 ()
  (functionp 1))
(assert-equal t (%%za-compiled-p (function p-0023-0002)))
(assert-equal nil (p-0023-0002))

;; p.23 (funcall (function -) 3) => -3 をそのまま転記
(defun p-0023-0003 ()
  (funcall (function -) 3))
(assert-equal t (%%za-compiled-p (function p-0023-0003)))
(assert-equal -3 (p-0023-0003))

;; p.23 (apply #'- '(4 3)) => 1 をそのまま転記
(defun p-0023-0004 ()
  (apply #'- '(4 3)))
(assert-equal t (%%za-compiled-p (function p-0023-0004)))
(assert-equal 1 (p-0023-0004))

;; p.24 lambda の4例。このカーネルは可変長引数のマーカーとして &rest のみを受け付け
;; :rest は対応しない(方針として対応不要)ため、3例目の :rest 版は &rest に置き換える(cf.)。
(defun p-0024-0001 ()
  ((lambda (x y) (+ (* x x) (* y y))) 3 4))
(assert-equal t (%%za-compiled-p (function p-0024-0001)))
(assert-equal 25 (p-0024-0001))

(defun p-0024-0002 ()
  ((lambda (x y &rest z) z) 3 4 5 6))
(assert-equal t (%%za-compiled-p (function p-0024-0002)))
(assert-equal '(5 6) (p-0024-0002))

(defun p-0024-0003 ()
  ((lambda (x y &rest z) z) 3 4 5 6))
(assert-equal t (%%za-compiled-p (function p-0024-0003)))
(assert-equal '(5 6) (p-0024-0003))

(defun p-0024-0004 ()
  (funcall (lambda (x y) (- y (* x y))) 7 3))
(assert-equal t (%%za-compiled-p (function p-0024-0004)))
(assert-equal -18 (p-0024-0004))

;; p.25 labels の例をそのまま転記
(defun p-0025-0001 ()
  (labels ((evenp (n)
             (if (= n 0)
                 t
                 (oddp (- n 1))))
           (oddp (n)
             (if (= n 0)
                 nil
                 (evenp (- n 1)))))
    (evenp 88)))
(assert-equal t (%%za-compiled-p (function p-0025-0001)))
(assert-equal t (p-0025-0001))

;; p.25 flet の例をそのまま転記(内側のfの本体中のfは外側のfを指す)
(defun p-0025-0002 ()
  (flet ((f (x) (+ x 3)))
    (flet ((f (x) (+ x (f x))))
      (f 7))))
(assert-equal t (%%za-compiled-p (function p-0025-0002)))
(assert-equal 17 (p-0025-0002))

;; p.25 (apply (if (< 1 2) (function max) (function min)) 1 2 (list 3 4)) => 4 をそのまま転記
(defun p-0025-0003 ()
  (apply (if (< 1 2) (function max) (function min))
         1 2 (list 3 4)))
(assert-equal t (%%za-compiled-p (function p-0025-0003)))
(assert-equal 4 (p-0025-0003))

;; cf. p.25 compose の例。:rest は &rest に置き換え、sqrt の結果はfloatなので許容誤差付きの比較にする。
;; [skip p-0025-0004] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun compose (f g)
    (lambda (&rest args)
      (funcall f (apply g args))))

(defun p-0025-0005 ()
  (funcall (compose (function sqrt) (function *)) 12 75))
(assert-equal t (%%za-compiled-p (function p-0025-0005)))
(assert-float-close 30.0 (p-0025-0005))

;; p.25 (let ((x '(1 2 3))) (funcall (cond ((listp x) (function car)) (t (lambda (x) (cons x 1)))) x)) => 1
(defun p-0025-0006 ()
  (let ((x '(1 2 3)))
    (funcall (cond ((listp x) (function car))
                   (t (lambda (x) (cons x 1))))
             x)))
(assert-equal t (%%za-compiled-p (function p-0025-0006)))
(assert-equal 1 (p-0025-0006))

;; p.26 defconstant の例をそのまま転記
;; [skip p-0026-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defconstant e 2.7182818284590451)

(defun p-0026-0002 ()
  e)
(assert-equal t (%%za-compiled-p (function p-0026-0002)))
(assert-equal 2.7182818284590451 (p-0026-0002))

;; [skip p-0026-0003] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun f () e)

(defun p-0026-0004 ()
  (f))
(assert-equal t (%%za-compiled-p (function p-0026-0004)))
(assert-equal 2.7182818284590451 (p-0026-0004))

;; p.27 defglobal の例をそのまま転記
;; [skip p-0027-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal today 'wednesday)

(defun p-0027-0002 ()
  today)
(assert-equal t (%%za-compiled-p (function p-0027-0002)))
(assert-equal 'wednesday (p-0027-0002))

;; [skip p-0027-0003] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun what-is-today () today)

(defun p-0027-0004 ()
  (what-is-today))
(assert-equal t (%%za-compiled-p (function p-0027-0004)))
(assert-equal 'wednesday (p-0027-0004))

(defun p-0027-0005 ()
  (let ((what-is-today 'thursday)) (what-is-today)))
(assert-equal t (%%za-compiled-p (function p-0027-0005)))
(assert-equal 'wednesday (p-0027-0005))

(defun p-0027-0006 ()
  (let ((today 'thursday)) (what-is-today)))
(assert-equal t (%%za-compiled-p (function p-0027-0006)))
(assert-equal 'wednesday (p-0027-0006))

;; p.27 defdynamic の例をそのまま転記
;; [skip p-0027-0007] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defdynamic *color* 'red)

(defun p-0027-0008 ()
  (dynamic *color*))
(assert-equal t (%%za-compiled-p (function p-0027-0008)))
(assert-equal 'red (p-0027-0008))

;; [skip p-0027-0009] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun what-color () (dynamic *color*))

(defun p-0027-0010 ()
  (what-color))
(assert-equal t (%%za-compiled-p (function p-0027-0010)))
(assert-equal 'red (p-0027-0010))

(defun p-0027-0011 ()
  (dynamic-let ((*color* 'green)) (what-color)))
(assert-equal t (%%za-compiled-p (function p-0027-0011)))
(assert-equal 'green (p-0027-0011))

;; dynamic-let を抜けたら元の値に戻る(§10.3 の説明)
(defun p-0027-0012 ()
  (what-color))
(assert-equal t (%%za-compiled-p (function p-0027-0012)))
(assert-equal 'red (p-0027-0012))

;; p.28 (defun caar (x) (car (car x))) => caar をそのまま転記
(defglobal isiki-test-defun-caar-result (defun caar (x) (car (car x))))

(defun p-0028-0001 ()
  isiki-test-defun-caar-result)
(assert-equal t (%%za-compiled-p (function p-0028-0001)))
(assert-equal 'caar (p-0028-0001))

(defun p-0028-0002 ()
  (caar '((1 2) 3)))
(assert-equal t (%%za-compiled-p (function p-0028-0002)))
(assert-equal 1 (p-0028-0002))

;;; ===========================================================================
;;; §13 Predicates (p.29-32)
;;; ===========================================================================

;; p.29-30 eq/eql の等価性表。(implementation-defined)と明記されている行を除いた
;; 全行をそのまま転記。
(defun p-0029-0001 ()
  (eql () ()))
(assert-equal t (%%za-compiled-p (function p-0029-0001)))
(assert-equal t (p-0029-0001))

(defun p-0029-0002 ()
  (eq () ()))
(assert-equal t (%%za-compiled-p (function p-0029-0002)))
(assert-equal t (p-0029-0002))

(defun p-0029-0003 ()
  (eql '() '()))
(assert-equal t (%%za-compiled-p (function p-0029-0003)))
(assert-equal t (p-0029-0003))

(defun p-0029-0004 ()
  (eq '() '()))
(assert-equal t (%%za-compiled-p (function p-0029-0004)))
(assert-equal t (p-0029-0004))

(defun p-0029-0005 ()
  (eql 'a 'a))
(assert-equal t (%%za-compiled-p (function p-0029-0005)))
(assert-equal t (p-0029-0005))

(defun p-0029-0006 ()
  (eq 'a 'a))
(assert-equal t (%%za-compiled-p (function p-0029-0006)))
(assert-equal t (p-0029-0006))

(defun p-0029-0007 ()
  (eql 'a 'A))
(assert-equal t (%%za-compiled-p (function p-0029-0007)))
(assert-equal t (p-0029-0007))

(defun p-0029-0008 ()
  (eq 'a 'A))
(assert-equal t (%%za-compiled-p (function p-0029-0008)))
(assert-equal t (p-0029-0008))

(defun p-0029-0009 ()
  (eql 'a 'b))
(assert-equal t (%%za-compiled-p (function p-0029-0009)))
(assert-equal nil (p-0029-0009))

(defun p-0029-0010 ()
  (eq 'a 'b))
(assert-equal t (%%za-compiled-p (function p-0029-0010)))
(assert-equal nil (p-0029-0010))

(defun p-0029-0011 ()
  (eql 'f 'nil))
(assert-equal t (%%za-compiled-p (function p-0029-0011)))
(assert-equal nil (p-0029-0011))

(defun p-0029-0012 ()
  (eq 'f 'nil))
(assert-equal t (%%za-compiled-p (function p-0029-0012)))
(assert-equal nil (p-0029-0012))

(defun p-0029-0013 ()
  (eql 2 2))
(assert-equal t (%%za-compiled-p (function p-0029-0013)))
(assert-equal t (p-0029-0013))

(defun p-0029-0014 ()
  (eql 2 2.0))
(assert-equal t (%%za-compiled-p (function p-0029-0014)))
(assert-equal nil (p-0029-0014))

(defun p-0029-0015 ()
  (eq 2 2.0))
(assert-equal t (%%za-compiled-p (function p-0029-0015)))
(assert-equal nil (p-0029-0015))

(defun p-0029-0016 ()
  (eql 100000000 100000000))
(assert-equal t (%%za-compiled-p (function p-0029-0016)))
(assert-equal t (p-0029-0016))

(defun p-0029-0017 ()
  (eql 10.00000 10.0))
(assert-equal t (%%za-compiled-p (function p-0029-0017)))
(assert-equal t (p-0029-0017))

(defun p-0029-0018 ()
  (eql (cons 1 2) (cons 1 2)))
(assert-equal t (%%za-compiled-p (function p-0029-0018)))
(assert-equal nil (p-0029-0018))

(defun p-0029-0019 ()
  (eq (cons 1 2) (cons 1 2)))
(assert-equal t (%%za-compiled-p (function p-0029-0019)))
(assert-equal nil (p-0029-0019))

(defun p-0029-0020 ()
  (let ((x '(a))) (eql x x)))
(assert-equal t (%%za-compiled-p (function p-0029-0020)))
(assert-equal t (p-0029-0020))

(defun p-0029-0021 ()
  (let ((x '(a))) (eq x x)))
(assert-equal t (%%za-compiled-p (function p-0029-0021)))
(assert-equal t (p-0029-0021))

(defun p-0029-0022 ()
  (let ((p (lambda (x) x))) (eql p p)))
(assert-equal t (%%za-compiled-p (function p-0029-0022)))
(assert-equal t (p-0029-0022))

(defun p-0029-0023 ()
  (let ((p (lambda (x) x))) (eq p p)))
(assert-equal t (%%za-compiled-p (function p-0029-0023)))
(assert-equal t (p-0029-0023))

(defun p-0029-0024 ()
  (let ((x "a")) (eql x x)))
(assert-equal t (%%za-compiled-p (function p-0029-0024)))
(assert-equal t (p-0029-0024))

(defun p-0029-0025 ()
  (let ((x "a")) (eq x x)))
(assert-equal t (%%za-compiled-p (function p-0029-0025)))
(assert-equal t (p-0029-0025))

(defun p-0029-0026 ()
  (let ((x "")) (eql x x)))
(assert-equal t (%%za-compiled-p (function p-0029-0026)))
(assert-equal t (p-0029-0026))

(defun p-0029-0027 ()
  (let ((x "")) (eq x x)))
(assert-equal t (%%za-compiled-p (function p-0029-0027)))
(assert-equal t (p-0029-0027))

(defun p-0029-0028 ()
  (eql #\a #\A))
(assert-equal t (%%za-compiled-p (function p-0029-0028)))
(assert-equal nil (p-0029-0028))

(defun p-0029-0029 ()
  (eq #\a #\A))
(assert-equal t (%%za-compiled-p (function p-0029-0029)))
(assert-equal nil (p-0029-0029))

(defun p-0029-0030 ()
  (eql #\a #\a))
(assert-equal t (%%za-compiled-p (function p-0029-0030)))
(assert-equal t (p-0029-0030))

(defun p-0029-0031 ()
  (eql #\space #\Space))
(assert-equal t (%%za-compiled-p (function p-0029-0031)))
(assert-equal t (p-0029-0031))

(defun p-0029-0032 ()
  (eql #\space #\space))
(assert-equal t (%%za-compiled-p (function p-0029-0032)))
(assert-equal t (p-0029-0032))

;; p.31 (equal 'a 'a) ... (equal "a" "A") をそのまま転記
(defun p-0031-0001 ()
  (equal 'a 'a))
(assert-equal t (%%za-compiled-p (function p-0031-0001)))
(assert-equal t (p-0031-0001))

(defun p-0031-0002 ()
  (equal 2 2))
(assert-equal t (%%za-compiled-p (function p-0031-0002)))
(assert-equal t (p-0031-0002))

(defun p-0031-0003 ()
  (equal 2 2.0))
(assert-equal t (%%za-compiled-p (function p-0031-0003)))
(assert-equal nil (p-0031-0003))

(defun p-0031-0004 ()
  (equal '(a) '(a)))
(assert-equal t (%%za-compiled-p (function p-0031-0004)))
(assert-equal t (p-0031-0004))

(defun p-0031-0005 ()
  (equal '(a (b) c) '(a (b) c)))
(assert-equal t (%%za-compiled-p (function p-0031-0005)))
(assert-equal t (p-0031-0005))

(defun p-0031-0006 ()
  (equal (cons 1 2) (cons 1 2)))
(assert-equal t (%%za-compiled-p (function p-0031-0006)))
(assert-equal t (p-0031-0006))

(defun p-0031-0007 ()
  (equal '(a) (list 'a)))
(assert-equal t (%%za-compiled-p (function p-0031-0007)))
(assert-equal t (p-0031-0007))

(defun p-0031-0008 ()
  (equal "abc" "abc"))
(assert-equal t (%%za-compiled-p (function p-0031-0008)))
(assert-equal t (p-0031-0008))

(defun p-0031-0009 ()
  (equal (vector 'a) (vector 'a)))
(assert-equal t (%%za-compiled-p (function p-0031-0009)))
(assert-equal t (p-0031-0009))

(defun p-0031-0010 ()
  (equal #(a b) #(a b)))
(assert-equal t (%%za-compiled-p (function p-0031-0010)))
(assert-equal t (p-0031-0010))

(defun p-0031-0011 ()
  (equal #(a b) #(a c)))
(assert-equal t (%%za-compiled-p (function p-0031-0011)))
(assert-equal nil (p-0031-0011))

(defun p-0031-0012 ()
  (equal "a" "A"))
(assert-equal t (%%za-compiled-p (function p-0031-0012)))
(assert-equal nil (p-0031-0012))

;; p.31 (not t)(not '())(not 'nil)(not nil)(not 3)(not (list))(not (list 3)) をそのまま転記
(defun p-0031-0013 ()
  (not t))
(assert-equal t (%%za-compiled-p (function p-0031-0013)))
(assert-equal nil (p-0031-0013))

(defun p-0031-0014 ()
  (not '()))
(assert-equal t (%%za-compiled-p (function p-0031-0014)))
(assert-equal t (p-0031-0014))

(defun p-0031-0015 ()
  (not 'nil))
(assert-equal t (%%za-compiled-p (function p-0031-0015)))
(assert-equal t (p-0031-0015))

(defun p-0031-0016 ()
  (not nil))
(assert-equal t (%%za-compiled-p (function p-0031-0016)))
(assert-equal t (p-0031-0016))

(defun p-0031-0017 ()
  (not 3))
(assert-equal t (%%za-compiled-p (function p-0031-0017)))
(assert-equal nil (p-0031-0017))

(defun p-0031-0018 ()
  (not (list)))
(assert-equal t (%%za-compiled-p (function p-0031-0018)))
(assert-equal t (p-0031-0018))

(defun p-0031-0019 ()
  (not (list 3)))
(assert-equal t (%%za-compiled-p (function p-0031-0019)))
(assert-equal nil (p-0031-0019))

;; p.32 and の例をそのまま転記
(defun p-0032-0001 ()
  (and (= 2 2) (> 2 1)))
(assert-equal t (%%za-compiled-p (function p-0032-0001)))
(assert-equal t (p-0032-0001))

(defun p-0032-0002 ()
  (and (= 2 2) (< 2 1)))
(assert-equal t (%%za-compiled-p (function p-0032-0002)))
(assert-equal nil (p-0032-0002))

(defun p-0032-0003 ()
  (and (eql 'a 'a) (not (> 1 2))))
(assert-equal t (%%za-compiled-p (function p-0032-0003)))
(assert-equal t (p-0032-0003))

(defun p-0032-0004 ()
  (let ((x 'a)) (and x (setq x 'b))))
(assert-equal t (%%za-compiled-p (function p-0032-0004)))
(assert-equal 'b (p-0032-0004))

(defun p-0032-0005 ()
  (let ((x nil)) (and x (setq x 'b))))
(assert-equal t (%%za-compiled-p (function p-0032-0005)))
(assert-equal nil (p-0032-0005))

(defun p-0032-0006 ()
  (let ((time 10))
    (if (and (< time 24) (> time 12))
        (- time 12) time)))
(assert-equal t (%%za-compiled-p (function p-0032-0006)))
(assert-equal 10 (p-0032-0006))

(defun p-0032-0007 ()
  (let ((time 18))
    (if (and (< time 24) (> time 12))
        (- time 12) time)))
(assert-equal t (%%za-compiled-p (function p-0032-0007)))
(assert-equal 6 (p-0032-0007))

;; cf. p.32 and の定義(≡ 't/form/...)に基づく追加例(引数なし)。
(defun p-0032-0008 ()
  (and))
(assert-equal t (%%za-compiled-p (function p-0032-0008)))
(assert-equal t (p-0032-0008))

;; p.32 or の例をそのまま転記
(defun p-0032-0009 ()
  (or (= 2 2) (> 2 1)))
(assert-equal t (%%za-compiled-p (function p-0032-0009)))
(assert-equal t (p-0032-0009))

(defun p-0032-0010 ()
  (or (= 2 2) (< 2 1)))
(assert-equal t (%%za-compiled-p (function p-0032-0010)))
(assert-equal t (p-0032-0010))

(defun p-0032-0011 ()
  (let ((x 'a)) (or x (setq x 'b))))
(assert-equal t (%%za-compiled-p (function p-0032-0011)))
(assert-equal 'a (p-0032-0011))

(defun p-0032-0012 ()
  (let ((x nil)) (or x (setq x 'b))))
(assert-equal t (%%za-compiled-p (function p-0032-0012)))
(assert-equal 'b (p-0032-0012))

;; cf. p.32 or の定義(≡ 'nil/form/...)に基づく追加例(引数なし)。
(defun p-0032-0013 ()
  (or))
(assert-equal t (%%za-compiled-p (function p-0032-0013)))
(assert-equal nil (p-0032-0013))

;;; ===========================================================================
;;; §14 Control structure (p.33-47)
;;; ===========================================================================

;; p.33 リテラル定数の例。#2A((a b c) (d e f)) はリーダー構文依存のため
;; isiki_test_syntax.lisp 側で転記し、残りをここで転記する。
(defun p-0033-0001 ()
  #\a)
(assert-equal t (%%za-compiled-p (function p-0033-0001)))
(assert-equal #\a (p-0033-0001))

(defun p-0033-0002 ()
  145932)
(assert-equal t (%%za-compiled-p (function p-0033-0002)))
(assert-equal 145932 (p-0033-0002))

(defun p-0033-0003 ()
  "abc")
(assert-equal t (%%za-compiled-p (function p-0033-0003)))
(assert-equal "abc" (p-0033-0003))

(defun p-0033-0004 ()
  #(a b c))
(assert-equal t (%%za-compiled-p (function p-0033-0004)))
(assert-equal #(a b c) (p-0033-0004))

;; p.33 quote の例をそのまま転記。
(defun p-0033-0005 ()
  (quote a))
(assert-equal t (%%za-compiled-p (function p-0033-0005)))
(assert-equal 'a (p-0033-0005))

(defun p-0033-0006 ()
  (quote #(a b c)))
(assert-equal t (%%za-compiled-p (function p-0033-0006)))
(assert-equal #(a b c) (p-0033-0006))

(defun p-0033-0007 ()
  (quote (+ 1 2)))
(assert-equal t (%%za-compiled-p (function p-0033-0007)))
(assert-equal '(+ 1 2) (p-0033-0007))

(defun p-0033-0008 ()
  (quote ()))
(assert-equal t (%%za-compiled-p (function p-0033-0008)))
(assert-equal nil (p-0033-0008))

(defun p-0033-0009 ()
  'a)
(assert-equal t (%%za-compiled-p (function p-0033-0009)))
(assert-equal 'a (p-0033-0009))

(defun p-0033-0010 ()
  '#(a b c))
(assert-equal t (%%za-compiled-p (function p-0033-0010)))
(assert-equal #(a b c) (p-0033-0010))

(defun p-0033-0011 ()
  '(car l))
(assert-equal t (%%za-compiled-p (function p-0033-0011)))
(assert-equal '(car l) (p-0033-0011))

(defun p-0033-0012 ()
  '(+ 1 2))
(assert-equal t (%%za-compiled-p (function p-0033-0012)))
(assert-equal '(+ 1 2) (p-0033-0012))

(defun p-0033-0013 ()
  '(quote a))
(assert-equal t (%%za-compiled-p (function p-0033-0013)))
(assert-equal '(quote a) (p-0033-0013))

(defun p-0033-0014 ()
  ''a)
(assert-equal t (%%za-compiled-p (function p-0033-0014)))
(assert-equal '(quote a) (p-0033-0014))

(defun p-0033-0015 ()
  (car ''a))
(assert-equal t (%%za-compiled-p (function p-0033-0015)))
(assert-equal 'quote (p-0033-0015))

;; p.34 (defglobal x 0) => x / x => 0 / (let ((x 1)) x) => 1 / x => 0 をそのまま転記
;; [skip p-0034-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal x 0)

(defun p-0034-0002 ()
  x)
(assert-equal t (%%za-compiled-p (function p-0034-0002)))
(assert-equal 0 (p-0034-0002))

(defun p-0034-0003 ()
  (let ((x 1)) x))
(assert-equal t (%%za-compiled-p (function p-0034-0003)))
(assert-equal 1 (p-0034-0003))

(defun p-0034-0004 ()
  x)
(assert-equal t (%%za-compiled-p (function p-0034-0004)))
(assert-equal 0 (p-0034-0004))

;; p.34 setq の例をそのまま転記。(defglobal x 2) はp.34の (defglobal x 0) の再定義
;; [skip p-0034-0005] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal x 2)

(defun p-0034-0006 ()
  (+ x 1))
(assert-equal t (%%za-compiled-p (function p-0034-0006)))
(assert-equal 3 (p-0034-0006))

(defun p-0034-0007 ()
  (setq x 4))
(assert-equal t (%%za-compiled-p (function p-0034-0007)))
(assert-equal 4 (p-0034-0007))

(defun p-0034-0008 ()
  (+ x 1))
(assert-equal t (%%za-compiled-p (function p-0034-0008)))
(assert-equal 5 (p-0034-0008))

(defun p-0034-0009 ()
  (let ((x 1)) (setq x 2) x))
(assert-equal t (%%za-compiled-p (function p-0034-0009)))
(assert-equal 2 (p-0034-0009))

(defun p-0034-0010 ()
  (+ x 1))
(assert-equal t (%%za-compiled-p (function p-0034-0010)))
(assert-equal 5 (p-0034-0010))

;; p.35 setf の例をそのまま転記。x は p.34 で定義したグローバル変数だが、仕様の
;; 「In the cons x」に合わせてここでは cons を束縛し直す。2つ目は setf が
;; ユーザー定義マクロで書かれた place を展開してから処理する例。
(setq x (cons 1 2))

(defun p-0035-0001 ()
  (setf (car x) 2))
(assert-equal t (%%za-compiled-p (function p-0035-0001)))
(assert-equal 2 (p-0035-0001))

(defun p-0035-0002 ()
  (car x))
(assert-equal t (%%za-compiled-p (function p-0035-0002)))
(assert-equal 2 (p-0035-0002))

(setq x (cons 1 2))

;; [skip p-0035-0003] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defmacro first (spot) `(car ,spot))

(defun p-0035-0004 ()
  (setf (first x) 2))
(assert-equal t (%%za-compiled-p (function p-0035-0004)))
(assert-equal 2 (p-0035-0004))

(defun p-0035-0005 ()
  (car x))
(assert-equal t (%%za-compiled-p (function p-0035-0005)))
(assert-equal 2 (p-0035-0005))

;; p.36 let の3例をそのまま転記
(defun p-0036-0001 ()
  (let ((x 2) (y 3))
    (* x y)))
(assert-equal t (%%za-compiled-p (function p-0036-0001)))
(assert-equal 6 (p-0036-0001))

(defun p-0036-0002 ()
  (let ((x 2) (y 3))
    (let ((x 7)
          (z (+ x y)))
      (* z x))))
(assert-equal t (%%za-compiled-p (function p-0036-0002)))
(assert-equal 35 (p-0036-0002))

(defun p-0036-0003 ()
  (let ((x 1) (y 2))
    (let ((x y) (y x))
      (list x y))))
(assert-equal t (%%za-compiled-p (function p-0036-0003)))
(assert-equal '(2 1) (p-0036-0003))

;; p.37 let* の2例をそのまま転記
(defun p-0037-0001 ()
  (let ((x 2) (y 3))
    (let* ((x 7)
           (z (+ x y)))
      (* z x))))
(assert-equal t (%%za-compiled-p (function p-0037-0001)))
(assert-equal 70 (p-0037-0001))

(defun p-0037-0002 ()
  (let ((x 1) (y 2))
    (let* ((x y) (y x))
      (list x y))))
(assert-equal t (%%za-compiled-p (function p-0037-0002)))
(assert-equal '(2 2) (p-0037-0002))

;; p.38 dynamic-let の例をそのまま転記(y は defdynamic されていない動的変数)
;; [skip p-0038-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun foo (x)
    (dynamic-let ((y x))
      (bar 1)))

;; [skip p-0038-0002] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun bar (x)
    (+ x (dynamic y)))

(defun p-0038-0003 ()
  (foo 2))
(assert-equal t (%%za-compiled-p (function p-0038-0003)))
(assert-equal 3 (p-0038-0003))

;; p.38 (if (> 3 2) 'yes 'no) => yes / (if (> 2 3) 'yes 'no) => no /
;; (if (> 2 3) 'yes) => nil をそのまま転記
(defun p-0038-0004 ()
  (if (> 3 2) 'yes 'no))
(assert-equal t (%%za-compiled-p (function p-0038-0004)))
(assert-equal 'yes (p-0038-0004))

(defun p-0038-0005 ()
  (if (> 2 3) 'yes 'no))
(assert-equal t (%%za-compiled-p (function p-0038-0005)))
(assert-equal 'no (p-0038-0005))

(defun p-0038-0006 ()
  (if (> 2 3) 'yes))
(assert-equal t (%%za-compiled-p (function p-0038-0006)))
(assert-equal nil (p-0038-0006))

;; p.39 (if (> 3 2) (- 3 2) (+ 3 2)) => 1 /
;; (let ((x 7)) (if (< x 0) x (- x))) => -7 をそのまま転記
(defun p-0039-0001 ()
  (if (> 3 2) (- 3 2) (+ 3 2)))
(assert-equal t (%%za-compiled-p (function p-0039-0001)))
(assert-equal 1 (p-0039-0001))

(defun p-0039-0002 ()
  (let ((x 7)) (if (< x 0) x (- x))))
(assert-equal t (%%za-compiled-p (function p-0039-0002)))
(assert-equal -7 (p-0039-0002))

;; p.39 cond の3例をそのまま転記
(defun p-0039-0003 ()
  (cond ((> 3 2) 'greater) ((< 3 2) 'less)))
(assert-equal t (%%za-compiled-p (function p-0039-0003)))
(assert-equal 'greater (p-0039-0003))

(defun p-0039-0004 ()
  (cond ((> 3 3) 'greater) ((< 3 3) 'less)))
(assert-equal t (%%za-compiled-p (function p-0039-0004)))
(assert-equal nil (p-0039-0004))

(defun p-0039-0005 ()
  (cond ((> 3 3) 'greater) ((< 3 3) 'less) (t 'equal)))
(assert-equal t (%%za-compiled-p (function p-0039-0005)))
(assert-equal 'equal (p-0039-0005))

;; p.40 case/case-using の6例をそのまま転記
(defun p-0040-0001 ()
  (case (* 2 3) ((2 3 5 7) 'prime) ((4 6 8 9) 'composite)))
(assert-equal t (%%za-compiled-p (function p-0040-0001)))
(assert-equal 'composite (p-0040-0001))

(defun p-0040-0002 ()
  (case (car '(c d)) ((a) 'a) ((b) 'b)))
(assert-equal t (%%za-compiled-p (function p-0040-0002)))
(assert-equal nil (p-0040-0002))

(defun p-0040-0003 ()
  (case (car '(c d)) ((a e i o u) 'vowel) ((y) 'semivowel) (t 'consonant)))
(assert-equal t (%%za-compiled-p (function p-0040-0003)))
(assert-equal 'consonant (p-0040-0003))

(defun p-0040-0004 ()
  (let ((char #\u)) (case char ((#\a #\e #\o #\u #\i) 'vowels) (t 'consonants))))
(assert-equal t (%%za-compiled-p (function p-0040-0004)))
(assert-equal 'vowels (p-0040-0004))

(defun p-0040-0005 ()
  (case-using #'= (+ 1.0 1.0) ((1) 'one) ((2) 'two) (t 'more)))
(assert-equal t (%%za-compiled-p (function p-0040-0005)))
(assert-equal 'two (p-0040-0005))

(defun p-0040-0006 ()
  (case-using #'string= "bar" (("foo") 1) (("bar") 2)))
(assert-equal t (%%za-compiled-p (function p-0040-0006)))
(assert-equal 2 (p-0040-0006))

;; p.41 progn の例をそのまま転記。(defglobal x 0) は p.34 の x の再定義。
;; [skip p-0041-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal x 0)

(defun p-0041-0002 ()
  (progn (setq x 5) (+ x 1)))
(assert-equal t (%%za-compiled-p (function p-0041-0002)))
(assert-equal 6 (p-0041-0002))

;; p.41 (progn (format (standard-output) "4 plus 1 equals ") (format (standard-output) "~D" (+ 4 1)))
;; => nil, prints "4 plus 1 equals 5" をそのまま転記。standard-output を文字列ストリームに
;; 束縛して印字結果も検証する。
(defun p-0041-0003 ()
  (progn
    (format (standard-output) "4 plus 1 equals ")
    (format (standard-output) "~D" (+ 4 1))))
(assert-equal t (%%za-compiled-p (function p-0041-0003)))
(assert-output (isiki-test-progn-result isiki-test-progn-output) (p-0041-0003)
  (assert-equal nil isiki-test-progn-result)
  (assert-equal "4 plus 1 equals 5" isiki-test-progn-output))

;; p.41 while の例をそのまま転記
(defun p-0041-0004 ()
  (let ((x '()) (i 5)) (while (> i 0) (setq x (cons i x)) (setq i (- i 1))) x))
(assert-equal t (%%za-compiled-p (function p-0041-0004)))
(assert-equal '(1 2 3 4 5) (p-0041-0004))

;; p.42 for の2例をそのまま転記
(defun p-0042-0001 ()
  (for ((vec (vector 0 0 0 0 0)) (i 0 (+ i 1))) ((= i 5) vec) (setf (elt vec i) i)))
(assert-equal t (%%za-compiled-p (function p-0042-0001)))
(assert-equal #(0 1 2 3 4) (p-0042-0001))

(defun p-0042-0002 ()
  (let ((x '(1 3 5 7 9))) (for ((x x (cdr x)) (sum 0 (+ sum (car x)))) ((null x) sum))))
(assert-equal t (%%za-compiled-p (function p-0042-0002)))
(assert-equal 25 (p-0042-0002))

;; p.43 (block x (+ 10 (return-from x 6) 22)) => 6 をそのまま転記
(defun p-0043-0001 ()
  (block x (+ 10 (return-from x 6) 22)))
(assert-equal t (%%za-compiled-p (function p-0043-0001)))
(assert-equal 6 (p-0043-0001))

;; p.43 f1/f2 (blockをクロージャ越しにreturn-fromする例)をそのまま転記。
;; [skip p-0043-0002] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun f1 ()
    (block b
      (let ((f (lambda () (return-from b 'exit))))
        (f2 f))))

;; [skip p-0043-0003] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun f2 (g) (funcall g))

(defun p-0043-0004 ()
  (f1))
(assert-equal t (%%za-compiled-p (function p-0043-0004)))
(assert-equal 'exit (p-0043-0004))

;; p.43 (block sum-block (for ...)) => 0 をそのまま転記
(defun p-0043-0005 ()
  (block sum-block
    (for ((x '(1 a 2 3) (cdr x))
          (sum 0 (+ sum (car x))))
        ((null x) sum)
      (cond ((not (numberp (car x))) (return-from sum-block 0))))))
(assert-equal t (%%za-compiled-p (function p-0043-0005)))
(assert-equal 0 (p-0043-0005))

;; cf. p.43-44 bar (blockの動的extentを抜けた後のreturn-fromがエラーになる例)。
;; p.38 で bar を別の定義で使っているため isiki-test-bl-bar に変更。
;; [skip p-0038-0007] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-bl-bar (x y)
    (let ((foo #'car))
      (let ((result
              (block bl
                (setq foo (lambda () (return-from bl 'first-exit)))
                (if x (return-from bl 'second-exit) 'third-exit))))
        (if y (funcall foo) nil)
        result)))

(defun p-0038-0008 ()
  (isiki-test-bl-bar t nil))
(assert-equal t (%%za-compiled-p (function p-0038-0008)))
(assert-equal 'second-exit (p-0038-0008))

(defun p-0038-0009 ()
  (isiki-test-bl-bar nil nil))
(assert-equal t (%%za-compiled-p (function p-0038-0009)))
(assert-equal 'third-exit (p-0038-0009))

(defun p-0038-0010 ()
  (isiki-test-bl-bar nil t))
(assert-equal t (%%za-compiled-p (function p-0038-0010)))
(assert-error (p-0038-0010))

(defun p-0038-0011 ()
  (isiki-test-bl-bar t t))
(assert-equal t (%%za-compiled-p (function p-0038-0011)))
(assert-error (p-0038-0011))

;; cf. p.45 catch/throw の例。p.38 の foo/bar と区別するため isiki-test-catch-foo/-bar に変更。
;; [skip p-0045-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-catch-foo (x) (catch 'block-sum (isiki-test-catch-bar x)))

;; [skip p-0045-0002] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-catch-bar (x)
    (for ((l x (cdr l)) (sum 0 (+ sum (car l))))
        ((null l) sum)
      (cond ((not (numberp (car l))) (throw 'block-sum 0)))))

(defun p-0045-0003 ()
  (isiki-test-catch-foo '(1 2 3 4)))
(assert-equal t (%%za-compiled-p (function p-0045-0003)))
(assert-equal 10 (p-0045-0003))

(defun p-0045-0004 ()
  (isiki-test-catch-foo '(1 2 a 4)))
(assert-equal t (%%za-compiled-p (function p-0045-0004)))
(assert-equal 0 (p-0045-0004))

;; cf. p.46 tagbody/go の with-retry マクロの例。:rest は &rest に置き換える。仕様の使用例は
;; 「ISLISPには実在しない仮想の関数」if-error を使うため、その部分だけ
;; リトライ回数を数える自己完結した本体に置き換える。
;; [skip p-0046-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defmacro with-retry (&rest forms)
    (let ((tag (gensym)))
      `(block ,tag
         (tagbody
           ,tag
           (return-from ,tag
             (flet ((retry () (go ,tag)))
               ,@forms))))))

(defun p-0046-0002 ()
  (let ((isiki-test-retry-count 0))
    (with-retry
      (setq isiki-test-retry-count (+ isiki-test-retry-count 1))
      (if (< isiki-test-retry-count 3) (retry) isiki-test-retry-count))))
(assert-equal t (%%za-compiled-p (function p-0046-0002)))
(assert-equal 3 (p-0046-0002))

;; cf. p.46-47 unwind-protect の例(1つ目: catch/throw と property の後始末)。
;; foo/bar は isiki-test-up-foo/-bar に変更。
;; [skip p-0046-0003] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-up-foo (x)
    (catch 'duplicates
      (unwind-protect (isiki-test-up-bar x)
        (for ((l x (cdr l)))
            ((null l) 'unused)
          (remove-property (car l) 'label)))))

;; [skip p-0046-0004] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-up-bar (l)
    (cond ((and (symbolp l) (property l 'label))
           (throw 'duplicates 'found))
          ((symbolp l) (setf (property l 'label) t))
          ((isiki-test-up-bar (car l)) (isiki-test-up-bar (cdr l)))
          (t nil)))

(defun p-0046-0005 ()
  (isiki-test-up-foo '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0046-0005)))
(assert-equal t (p-0046-0005))

(defun p-0046-0006 ()
  (property 'a 'label))
(assert-equal t (%%za-compiled-p (function p-0046-0006)))
(assert-equal nil (p-0046-0006))

(defun p-0046-0007 ()
  (isiki-test-up-foo '(a b a c)))
(assert-equal t (%%za-compiled-p (function p-0046-0007)))
(assert-equal 'found (p-0046-0007))

(defun p-0046-0008 ()
  (property 'a 'label))
(assert-equal t (%%za-compiled-p (function p-0046-0008)))
(assert-equal nil (p-0046-0008))

;; この例の bar は (symbolp nil) が t のため末尾の nil にも label を付け、cleanup は
;; リストの要素(a b c)しか外さないので nil の label が残る。同じ例を続けて実行する
;; (isiki_test_jit.lisp)と 2 回目の '(a b c) で found になってしまうため、ここで外す
(remove-property nil 'label)

;; cf. p.47 unwind-protect の例(2つ目: cleanup中に別のblockへreturn-fromするとエラー)。
;; test/test2/test3/test4 は isiki-test-up-test/-test2/-test3/-test4 に変更。
;; [skip p-0047-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-up-test ()
    (catch 'outer (isiki-test-up-test2)))

;; [skip p-0047-0002] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-up-test2 ()
    (block inner
      (isiki-test-up-test3 (lambda ()
                             (return-from inner 7)))))

;; [skip p-0047-0003] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-up-test3 (fun)
    (unwind-protect (isiki-test-up-test4) (funcall fun)))

;; [skip p-0047-0004] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-up-test4 ()
    (throw 'outer 6))

(defun p-0047-0005 ()
  (isiki-test-up-test))
(assert-equal t (%%za-compiled-p (function p-0047-0005)))
(assert-error (p-0047-0005))

;;; ===========================================================================
;;; §15 Objects (p.48-61)
;;; ===========================================================================

;; p.48-60 (§15 Classes) には defclass/defgeneric/defmethod/next-method-p/
;; call-next-method の構文・意味の説明はあるが、具体的な "Example:" ブロックは存在しない。
;; 以下は §15 の説明に基づき独自に作成したクラス例。

(defclass isiki-test-point () ((x :initarg :x :initform 0) (y :initarg :y :initform 0)))

(defclass isiki-test-point3d (isiki-test-point) ((z :initarg :z :initform 0)))

;; cf. p.48-50 (defclass ... :initarg ... :initform ...) の説明に基づく独自の例
(defun p-0048-0001 ()
  (slot-value (make-instance 'isiki-test-point ':x 1 ':y 2) 'x))
(assert-equal t (%%za-compiled-p (function p-0048-0001)))
(assert-equal 1 (p-0048-0001))

(defun p-0048-0002 ()
  (slot-value (make-instance 'isiki-test-point) 'x))
(assert-equal t (%%za-compiled-p (function p-0048-0002)))
(assert-equal 0 (p-0048-0002))

(defun p-0048-0003 ()
  (slot-value (make-instance 'isiki-test-point3d ':x 1 ':y 2 ':z 3) 'z))
(assert-equal t (%%za-compiled-p (function p-0048-0003)))
(assert-equal 3 (p-0048-0003))

;; cf. p.53-59 defgeneric/defmethod/next-method-p/call-next-method の説明に基づく独自の例
(defgeneric isiki-test-describe (obj))

(defmethod isiki-test-describe (obj) (list 'point-desc (next-method-p)))

(defmethod isiki-test-describe ((obj isiki-test-point3d))
  (list '3d-desc (next-method-p) (call-next-method)))

(defun p-0053-0001 ()
  (isiki-test-describe (make-instance 'isiki-test-point)))
(assert-equal t (%%za-compiled-p (function p-0053-0001)))
(assert-equal '(point-desc nil) (p-0053-0001))

(defun p-0053-0002 ()
  (isiki-test-describe (make-instance 'isiki-test-point3d)))
(assert-equal t (%%za-compiled-p (function p-0053-0002)))
(assert-equal '(3d-desc t (point-desc nil)) (p-0053-0002))

;; M12(#27)のQEMU実機限定regression: call-next-methodで次のメソッドが無い場合の
;; エラー通知は、AOT化されたcall-next-methodのlambdaクロージャ(自由変数0個で
;; 生成される)から%%funcall-by-name経由でまだインタプリタ実行のerrorを呼ぶ経路を
;; 通る。かつてこの経路の捕捉環境にnilを渡していたため、error本体からの
;; signal-condition/make-instance呼び出しがos_get_functionでglobal_environmentへ
;; 到達できず、条件を正しくsignalする代わりにg_sym_eval_errorが素通りしていた
;; (ignore-errorsで捕捉できない生の値のリーク)。host側のgcc(aarch64)ビルドでは
;; 再現しなかったが実機QEMU(x86_64/mingw)では必ず再現したため、実機スモークテスト
;; でのみ検出できるregressionだった
(defgeneric isiki-test-no-next-method (obj))

(defmethod isiki-test-no-next-method (obj) (call-next-method))

(defun p-0053-0003 ()
  (ignore-errors (isiki-test-no-next-method 42)))
(assert-equal t (%%za-compiled-p (function p-0053-0003)))
(assert-equal nil (p-0053-0003))

;; cf. p.61 typep/subclassp/class-of の説明に基づく独自の例
(defun p-0061-0001 ()
  (typep (make-instance 'isiki-test-point3d) 'isiki-test-point))
(assert-equal t (%%za-compiled-p (function p-0061-0001)))
(assert-equal t (p-0061-0001))

(defun p-0061-0002 ()
  (typep (make-instance 'isiki-test-point) 'isiki-test-point3d))
(assert-equal t (%%za-compiled-p (function p-0061-0002)))
(assert-equal nil (p-0061-0002))

(defun p-0061-0003 ()
  (subclassp (class-of (make-instance 'isiki-test-point3d))
                            (class-of (make-instance 'isiki-test-point))))
(assert-equal t (%%za-compiled-p (function p-0061-0003)))
(assert-equal t (p-0061-0003))

;;; ===========================================================================
;;; §16 Macros (p.62-63)
;;; ===========================================================================

;; cf. p.62 (defmacro caar (x) (list 'car (list 'car x))) => caar の例。p.28で既に
;; caar という名前の関数を定義済みなので、識別子を isiki-test-caar-macro に変更する。
;; [skip p-0062-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defmacro isiki-test-caar-macro (x) (list 'car (list 'car x)))

(defun p-0062-0002 ()
  (isiki-test-caar-macro '((1 2) 3)))
(assert-equal t (%%za-compiled-p (function p-0062-0002)))
(assert-equal 1 (p-0062-0002))

;; p.63 quasiquote の6例をそのまま転記(5・6例目は quasiquote 自体をデータとして
;; ネストする例。期待値もリーダーが読んだ quasiquote/unquote 構造として比較する)
(defun p-0063-0001 ()
  `(list ,(+ 1 2) 4))
(assert-equal t (%%za-compiled-p (function p-0063-0001)))
(assert-equal '(list 3 4) (p-0063-0001))

(defun p-0063-0002 ()
  (let ((name 'a)) `(list name ,name ',name)))
(assert-equal t (%%za-compiled-p (function p-0063-0002)))
(assert-equal '(list name a (quote a)) (p-0063-0002))

(defun p-0063-0003 ()
  `(a ,(+ 1 2) ,@(create-list 3 'x) b))
(assert-equal t (%%za-compiled-p (function p-0063-0003)))
(assert-equal '(a 3 x x x b) (p-0063-0003))

(defun p-0063-0004 ()
  `((foo ,(- 10 3)) ,@(cdr '(c)) . ,(car '(cons))))
(assert-equal t (%%za-compiled-p (function p-0063-0004)))
(assert-equal '((foo 7) . cons) (p-0063-0004))

(defun p-0063-0005 ()
  `(a `(b ,(+ 1 2) ,(foo ,(+ 1 3) d) e) f))
(assert-equal t (%%za-compiled-p (function p-0063-0005)))
(assert-equal '(a `(b ,(+ 1 2) ,(foo 4 d) e) f) (p-0063-0005))

(defun p-0063-0006 ()
  (let ((name1 'x)
        (name2 'y))
    `(a `(b ,,name1 ,',name2 d) e)))
(assert-equal t (%%za-compiled-p (function p-0063-0006)))
(assert-equal '(a `(b ,x ,'y d) e) (p-0063-0006))

;;; ===========================================================================
;;; §17 Declarations and coercions (p.63-65)
;;; ===========================================================================

;; p.63 (the <integer> 10) => 10 / (the <number> 10) => 10 をそのまま転記。
;; (the <float> 10) は仕様上「the consequences are undefined」と明記されているため省略する。
(defun p-0063-0007 ()
  (the <integer> 10))
(assert-equal t (%%za-compiled-p (function p-0063-0007)))
(assert-equal 10 (p-0063-0007))

(defun p-0063-0008 ()
  (the <number> 10))
(assert-equal t (%%za-compiled-p (function p-0063-0008)))
(assert-equal 10 (p-0063-0008))

;; p.63 assure の3例をそのまま転記
(defun p-0063-0009 ()
  (assure <integer> 10))
(assert-equal t (%%za-compiled-p (function p-0063-0009)))
(assert-equal 10 (p-0063-0009))

(defun p-0063-0010 ()
  (assure <number> 10))
(assert-equal t (%%za-compiled-p (function p-0063-0010)))
(assert-equal 10 (p-0063-0010))

(defun p-0063-0011 ()
  (assure <float> 10))
(assert-equal t (%%za-compiled-p (function p-0063-0011)))
(assert-error (p-0063-0011))

;; p.65 convert の3例をそのまま転記
(defun p-0065-0001 ()
  (convert 3 <float>))
(assert-equal t (%%za-compiled-p (function p-0065-0001)))
(assert-equal 3.0 (p-0065-0001))

(defun p-0065-0002 ()
  (convert "abc" <general-vector>))
(assert-equal t (%%za-compiled-p (function p-0065-0002)))
(assert-equal #(#\a #\b #\c) (p-0065-0002))

(defun p-0065-0003 ()
  (convert #(a b) <list>))
(assert-equal t (%%za-compiled-p (function p-0065-0003)))
(assert-equal '(a b) (p-0065-0003))

;;; ===========================================================================
;;; §18 Symbol class (p.65-68)
;;; ===========================================================================

;; p.65 symbolp の10例をそのまま転記
(defun p-0065-0004 ()
  (symbolp 'a))
(assert-equal t (%%za-compiled-p (function p-0065-0004)))
(assert-equal t (p-0065-0004))

(defun p-0065-0005 ()
  (symbolp "a"))
(assert-equal t (%%za-compiled-p (function p-0065-0005)))
(assert-equal nil (p-0065-0005))

(defun p-0065-0006 ()
  (symbolp #\a))
(assert-equal t (%%za-compiled-p (function p-0065-0006)))
(assert-equal nil (p-0065-0006))

(defun p-0065-0007 ()
  (symbolp 't))
(assert-equal t (%%za-compiled-p (function p-0065-0007)))
(assert-equal t (p-0065-0007))

(defun p-0065-0008 ()
  (symbolp t))
(assert-equal t (%%za-compiled-p (function p-0065-0008)))
(assert-equal t (p-0065-0008))

(defun p-0065-0009 ()
  (symbolp 'nil))
(assert-equal t (%%za-compiled-p (function p-0065-0009)))
(assert-equal t (p-0065-0009))

(defun p-0065-0010 ()
  (symbolp nil))
(assert-equal t (%%za-compiled-p (function p-0065-0010)))
(assert-equal t (p-0065-0010))

(defun p-0065-0011 ()
  (symbolp '()))
(assert-equal t (%%za-compiled-p (function p-0065-0011)))
(assert-equal t (p-0065-0011))

(defun p-0065-0012 ()
  (symbolp '*pi*))
(assert-equal t (%%za-compiled-p (function p-0065-0012)))
(assert-equal t (p-0065-0012))

(defun p-0065-0013 ()
  (symbolp *pi*))
(assert-equal t (%%za-compiled-p (function p-0065-0013)))
(assert-equal nil (p-0065-0013))

;; p.67-68 property / set-property / remove-property の例をそのまま転記。
;; 仕様の記載順(property の例が先)では athena が未設定なので、まず setf 版で設定し、
;; property → set-property → remove-property の順に確認する。
(defun p-0067-0001 ()
  (setf (property 'zeus 'daughter) 'athena))
(assert-equal t (%%za-compiled-p (function p-0067-0001)))
(assert-equal 'athena (p-0067-0001))

(defun p-0067-0002 ()
  (property 'zeus 'daughter))
(assert-equal t (%%za-compiled-p (function p-0067-0002)))
(assert-equal 'athena (p-0067-0002))

(defun p-0067-0003 ()
  (set-property 'athena 'zeus 'daughter))
(assert-equal t (%%za-compiled-p (function p-0067-0003)))
(assert-equal 'athena (p-0067-0003))

(defun p-0067-0004 ()
  (remove-property 'zeus 'daughter))
(assert-equal t (%%za-compiled-p (function p-0067-0004)))
(assert-equal 'athena (p-0067-0004))

;; cf. p.67 property の説明(無ければ obj(既定nil)を返す)に基づく追加例
(defun p-0067-0005 ()
  (property 'zeus 'daughter))
(assert-equal t (%%za-compiled-p (function p-0067-0005)))
(assert-equal nil (p-0067-0005))

(defun p-0067-0006 ()
  (property 'zeus 'daughter 'unknown))
(assert-equal t (%%za-compiled-p (function p-0067-0006)))
(assert-equal 'unknown (p-0067-0006))

;; p.68 gensym の例(defmacro twice)をそのまま転記
;; [skip p-0068-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defmacro twice (x)
    (let ((v (gensym)))
      `(let ((,v ,x)) (+ ,v ,v))))

(defun p-0068-0002 ()
  (twice 5))
(assert-equal t (%%za-compiled-p (function p-0068-0002)))
(assert-equal 10 (p-0068-0002))

;;; ===========================================================================
;;; §19 Number class (p.69-82)
;;; ===========================================================================

;; p.69 numberp の例をそのまま転記
(defun p-0069-0001 ()
  (numberp 3))
(assert-equal t (%%za-compiled-p (function p-0069-0001)))
(assert-equal t (p-0069-0001))

(defun p-0069-0002 ()
  (numberp -0.3))
(assert-equal t (%%za-compiled-p (function p-0069-0002)))
(assert-equal t (p-0069-0002))

(defun p-0069-0003 ()
  (numberp '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0069-0003)))
(assert-equal nil (p-0069-0003))

(defun p-0069-0004 ()
  (numberp "17"))
(assert-equal t (%%za-compiled-p (function p-0069-0004)))
(assert-equal nil (p-0069-0004))

;; p.69 parse-number の例をそのまま転記。123.34 は浮動小数点の字句解析結果が
;; 完全一致するとは限らないため assert-float-close を使う。
(defun p-0069-0005 ()
  (parse-number "123.34"))
(assert-equal t (%%za-compiled-p (function p-0069-0005)))
(assert-float-close 123.34 (p-0069-0005))

(defun p-0069-0006 ()
  (parse-number "#XFACE"))
(assert-equal t (%%za-compiled-p (function p-0069-0006)))
(assert-equal 64206 (p-0069-0006))

(defun p-0069-0007 ()
  (parse-number "-37."))
(assert-equal t (%%za-compiled-p (function p-0069-0007)))
(assert-error (p-0069-0007))

(defun p-0069-0008 ()
  (parse-number "-.5"))
(assert-equal t (%%za-compiled-p (function p-0069-0008)))
(assert-error (p-0069-0008))

;; p.69-70 = / /= の例をそのまま転記
(defun p-0069-0009 ()
  (= 3 4))
(assert-equal t (%%za-compiled-p (function p-0069-0009)))
(assert-equal nil (p-0069-0009))

(defun p-0069-0010 ()
  (= 3 3.0))
(assert-equal t (%%za-compiled-p (function p-0069-0010)))
(assert-equal t (p-0069-0010))

(defun p-0069-0011 ()
  (= (parse-number "134.54") 134.54))
(assert-equal t (%%za-compiled-p (function p-0069-0011)))
(assert-equal t (p-0069-0011))

(defun p-0069-0012 ()
  (= 0.0 -0.0))
(assert-equal t (%%za-compiled-p (function p-0069-0012)))
(assert-equal t (p-0069-0012))

(defun p-0069-0013 ()
  (/= 3 4))
(assert-equal t (%%za-compiled-p (function p-0069-0013)))
(assert-equal t (p-0069-0013))

(defun p-0069-0014 ()
  (/= 3 3.0))
(assert-equal t (%%za-compiled-p (function p-0069-0014)))
(assert-equal nil (p-0069-0014))

(defun p-0069-0015 ()
  (/= (parse-number "134.54") 134.54))
(assert-equal t (%%za-compiled-p (function p-0069-0015)))
(assert-equal nil (p-0069-0015))

;; p.70 >= / <= / > / < の例をそのまま転記
(defun p-0070-0001 ()
  (> 2 2))
(assert-equal t (%%za-compiled-p (function p-0070-0001)))
(assert-equal nil (p-0070-0001))

(defun p-0070-0002 ()
  (> 2.0 2))
(assert-equal t (%%za-compiled-p (function p-0070-0002)))
(assert-equal nil (p-0070-0002))

(defun p-0070-0003 ()
  (> 2 -10))
(assert-equal t (%%za-compiled-p (function p-0070-0003)))
(assert-equal t (p-0070-0003))

(defun p-0070-0004 ()
  (> 100 3))
(assert-equal t (%%za-compiled-p (function p-0070-0004)))
(assert-equal t (p-0070-0004))

(defun p-0070-0005 ()
  (< 2 2))
(assert-equal t (%%za-compiled-p (function p-0070-0005)))
(assert-equal nil (p-0070-0005))

(defun p-0070-0006 ()
  (< 1 2))
(assert-equal t (%%za-compiled-p (function p-0070-0006)))
(assert-equal t (p-0070-0006))

(defun p-0070-0007 ()
  (>= 2 2))
(assert-equal t (%%za-compiled-p (function p-0070-0007)))
(assert-equal t (p-0070-0007))

(defun p-0070-0008 ()
  (>= 2.0 2))
(assert-equal t (%%za-compiled-p (function p-0070-0008)))
(assert-equal t (p-0070-0008))

(defun p-0070-0009 ()
  (>= -1 2))
(assert-equal t (%%za-compiled-p (function p-0070-0009)))
(assert-equal nil (p-0070-0009))

(defun p-0070-0010 ()
  (<= -1 2))
(assert-equal t (%%za-compiled-p (function p-0070-0010)))
(assert-equal t (p-0070-0010))

(defun p-0070-0011 ()
  (<= 2 -1))
(assert-equal t (%%za-compiled-p (function p-0070-0011)))
(assert-equal nil (p-0070-0011))

;; p.71 + / * の例をそのまま転記
(defun p-0071-0001 ()
  (+ 12 3))
(assert-equal t (%%za-compiled-p (function p-0071-0001)))
(assert-equal 15 (p-0071-0001))

(defun p-0071-0002 ()
  (+ 1 2 3))
(assert-equal t (%%za-compiled-p (function p-0071-0002)))
(assert-equal 6 (p-0071-0002))

(defun p-0071-0003 ()
  (+ 12 3.0))
(assert-equal t (%%za-compiled-p (function p-0071-0003)))
(assert-equal 15.0 (p-0071-0003))

(defun p-0071-0004 ()
  (+ 4 0.0))
(assert-equal t (%%za-compiled-p (function p-0071-0004)))
(assert-equal 4.0 (p-0071-0004))

(defun p-0071-0005 ()
  (+))
(assert-equal t (%%za-compiled-p (function p-0071-0005)))
(assert-equal 0 (p-0071-0005))

(defun p-0071-0006 ()
  (* 12 3))
(assert-equal t (%%za-compiled-p (function p-0071-0006)))
(assert-equal 36 (p-0071-0006))

(defun p-0071-0007 ()
  (* 12 3.0))
(assert-equal t (%%za-compiled-p (function p-0071-0007)))
(assert-equal 36.0 (p-0071-0007))

(defun p-0071-0008 ()
  (* 4.0 0))
(assert-equal t (%%za-compiled-p (function p-0071-0008)))
(assert-equal 0.0 (p-0071-0008))

(defun p-0071-0009 ()
  (* 2 3 4))
(assert-equal t (%%za-compiled-p (function p-0071-0009)))
(assert-equal 24 (p-0071-0009))

(defun p-0071-0010 ()
  (*))
(assert-equal t (%%za-compiled-p (function p-0071-0010)))
(assert-equal 1 (p-0071-0010))

;; p.71 - (単項) の例をそのまま転記
(defun p-0071-0011 ()
  (- 1))
(assert-equal t (%%za-compiled-p (function p-0071-0011)))
(assert-equal -1 (p-0071-0011))

(defun p-0071-0012 ()
  (- -4.0))
(assert-equal t (%%za-compiled-p (function p-0071-0012)))
(assert-equal 4.0 (p-0071-0012))

(defun p-0071-0013 ()
  (- 4.0))
(assert-equal t (%%za-compiled-p (function p-0071-0013)))
(assert-equal -4.0 (p-0071-0013))

(defun p-0071-0014 ()
  (eql (- 0.0) -0.0))
(assert-equal t (%%za-compiled-p (function p-0071-0014)))
(assert-equal t (p-0071-0014))

(defun p-0071-0015 ()
  (eql (- -0.0) 0.0))
(assert-equal t (%%za-compiled-p (function p-0071-0015)))
(assert-equal t (p-0071-0015))

;; p.71 - (多項) の例をそのまま転記。(- 2.3 -3.0) => 5.3 は2進で正確でないため許容誤差付き。
(defun p-0071-0016 ()
  (- 1 2))
(assert-equal t (%%za-compiled-p (function p-0071-0016)))
(assert-equal -1 (p-0071-0016))

(defun p-0071-0017 ()
  (- 92 43))
(assert-equal t (%%za-compiled-p (function p-0071-0017)))
(assert-equal 49 (p-0071-0017))

(defun p-0071-0018 ()
  (- 2.3 -3.0))
(assert-equal t (%%za-compiled-p (function p-0071-0018)))
(assert-float-close 5.3 (p-0071-0018))

(defun p-0071-0019 ()
  (- 0.0 0.0))
(assert-equal t (%%za-compiled-p (function p-0071-0019)))
(assert-equal 0.0 (p-0071-0019))

(defun p-0071-0020 ()
  (- 3 4 5))
(assert-equal t (%%za-compiled-p (function p-0071-0020)))
(assert-equal -6 (p-0071-0020))

;; p.72 reciprocal / quotient の例をそのまま転記
(defun p-0072-0001 ()
  (reciprocal 2))
(assert-equal t (%%za-compiled-p (function p-0072-0001)))
(assert-equal 0.5 (p-0072-0001))

(defun p-0072-0002 ()
  (quotient 10 5))
(assert-equal t (%%za-compiled-p (function p-0072-0002)))
(assert-equal 2 (p-0072-0002))

(defun p-0072-0003 ()
  (quotient 1 2))
(assert-equal t (%%za-compiled-p (function p-0072-0003)))
(assert-equal 0.5 (p-0072-0003))

(defun p-0072-0004 ()
  (quotient 2 -0.5))
(assert-equal t (%%za-compiled-p (function p-0072-0004)))
(assert-equal -4.0 (p-0072-0004))

(defun p-0072-0005 ()
  (quotient 0 0.0))
(assert-equal t (%%za-compiled-p (function p-0072-0005)))
(assert-error (p-0072-0005))

(defun p-0072-0006 ()
  (quotient 2 3 4))
(assert-equal t (%%za-compiled-p (function p-0072-0006)))
(assert-float-close 0.16666666666666666 (p-0072-0006))

;; p.72 max / min の例をそのまま転記((max 2 2.0)/(min 2 2.0) は implementation-defined のため省略)
(defun p-0072-0007 ()
  (max -5 3))
(assert-equal t (%%za-compiled-p (function p-0072-0007)))
(assert-equal 3 (p-0072-0007))

(defun p-0072-0008 ()
  (max 2.0 3))
(assert-equal t (%%za-compiled-p (function p-0072-0008)))
(assert-equal 3 (p-0072-0008))

(defun p-0072-0009 ()
  (max 1 5 2 4 3))
(assert-equal t (%%za-compiled-p (function p-0072-0009)))
(assert-equal 5 (p-0072-0009))

(defun p-0072-0010 ()
  (min 3 1))
(assert-equal t (%%za-compiled-p (function p-0072-0010)))
(assert-equal 1 (p-0072-0010))

(defun p-0072-0011 ()
  (min 1 2.0))
(assert-equal t (%%za-compiled-p (function p-0072-0011)))
(assert-equal 1 (p-0072-0011))

(defun p-0072-0012 ()
  (min 1 5 2 4 3))
(assert-equal t (%%za-compiled-p (function p-0072-0012)))
(assert-equal 1 (p-0072-0012))

;; p.73 abs の例をそのまま転記
(defun p-0073-0001 ()
  (abs -3))
(assert-equal t (%%za-compiled-p (function p-0073-0001)))
(assert-equal 3 (p-0073-0001))

(defun p-0073-0002 ()
  (abs 2.0))
(assert-equal t (%%za-compiled-p (function p-0073-0002)))
(assert-equal 2.0 (p-0073-0002))

(defun p-0073-0003 ()
  (abs -0.0))
(assert-equal t (%%za-compiled-p (function p-0073-0003)))
(assert-equal 0.0 (p-0073-0003))

;; p.73 exp の例をそのまま転記((exp 0) は implementation-defined で 1 or 1.0 のため、値だけ確認)
(defun p-0073-0004 ()
  (exp 1))
(assert-equal t (%%za-compiled-p (function p-0073-0004)))
(assert-float-close 2.718281828459045 (p-0073-0004))

(defun p-0073-0005 ()
  (exp 2))
(assert-equal t (%%za-compiled-p (function p-0073-0005)))
(assert-float-close 7.38905609893065 (p-0073-0005))

(defun p-0073-0006 ()
  (exp 1.23))
(assert-equal t (%%za-compiled-p (function p-0073-0006)))
(assert-float-close 3.4212295362896734 (p-0073-0006))

(defun p-0073-0007 ()
  (exp 0))
(assert-equal t (%%za-compiled-p (function p-0073-0007)))
(assert-float-close 1.0 (p-0073-0007))

;; p.73-74 log の例をそのまま転記((log 1) は implementation-defined で 0 or 0.0 のため、値だけ確認)
(defun p-0073-0008 ()
  (log 2.718281828459045))
(assert-equal t (%%za-compiled-p (function p-0073-0008)))
(assert-float-close 1.0 (p-0073-0008))

(defun p-0073-0009 ()
  (log 10))
(assert-equal t (%%za-compiled-p (function p-0073-0009)))
(assert-float-close 2.302585092994046 (p-0073-0009))

(defun p-0073-0010 ()
  (log 1))
(assert-equal t (%%za-compiled-p (function p-0073-0010)))
(assert-float-close 0.0 (p-0073-0010))

;; p.74 expt の例をそのまま転記。(expt x 0) 等の x を含む一般則は具体値(x=5, 5.0)で確認する。
(defun p-0074-0001 ()
  (expt 2 3))
(assert-equal t (%%za-compiled-p (function p-0074-0001)))
(assert-equal 8 (p-0074-0001))

(defun p-0074-0002 ()
  (expt -100 2))
(assert-equal t (%%za-compiled-p (function p-0074-0002)))
(assert-equal 10000 (p-0074-0002))

(defun p-0074-0003 ()
  (expt 4 -2))
(assert-equal t (%%za-compiled-p (function p-0074-0003)))
(assert-equal 0.0625 (p-0074-0003))

(defun p-0074-0004 ()
  (expt 0.5 2))
(assert-equal t (%%za-compiled-p (function p-0074-0004)))
(assert-equal 0.25 (p-0074-0004))

(defun p-0074-0005 ()
  (expt 5 0))
(assert-equal t (%%za-compiled-p (function p-0074-0005)))
(assert-equal 1 (p-0074-0005))

(defun p-0074-0006 ()
  (expt 5.0 0))
(assert-equal t (%%za-compiled-p (function p-0074-0006)))
(assert-equal 1.0 (p-0074-0006))

(defun p-0074-0007 ()
  (expt -0.25 -1))
(assert-equal t (%%za-compiled-p (function p-0074-0007)))
(assert-equal -4.0 (p-0074-0007))

;; (expt 100 0.5) => 10.0 は exp/log 経由の計算では最終ビットが揺れうるので許容誤差付き
(defun p-0074-0008 ()
  (expt 100 0.5))
(assert-equal t (%%za-compiled-p (function p-0074-0008)))
(assert-float-close 10.0 (p-0074-0008))

(defun p-0074-0009 ()
  (expt 100 -1.5))
(assert-equal t (%%za-compiled-p (function p-0074-0009)))
(assert-float-close 0.001 (p-0074-0009))

(defun p-0074-0010 ()
  (expt 5.0 0.0))
(assert-equal t (%%za-compiled-p (function p-0074-0010)))
(assert-equal 1.0 (p-0074-0010))

(defun p-0074-0011 ()
  (expt 0.0 0.0))
(assert-equal t (%%za-compiled-p (function p-0074-0011)))
(assert-error (p-0074-0011))

;; p.74 sqrt の例をそのまま転記
(defun p-0074-0012 ()
  (sqrt 4))
(assert-equal t (%%za-compiled-p (function p-0074-0012)))
(assert-equal 2 (p-0074-0012))

(defun p-0074-0013 ()
  (sqrt 2))
(assert-equal t (%%za-compiled-p (function p-0074-0013)))
(assert-float-close 1.4142135623730951 (p-0074-0013))

(defun p-0074-0014 ()
  (sqrt -1))
(assert-equal t (%%za-compiled-p (function p-0074-0014)))
(assert-error (p-0074-0014))

;; p.74 *pi* => 3.141592653589793 をそのまま転記
(defun p-0074-0015 ()
  *pi*)
(assert-equal t (%%za-compiled-p (function p-0074-0015)))
(assert-float-close 3.141592653589793 (p-0074-0015))

;; p.75 sin/cos/tan の例をそのまま転記(仕様注記の通り精度の揺れは許容する)
(defun p-0075-0001 ()
  (sin 1))
(assert-equal t (%%za-compiled-p (function p-0075-0001)))
(assert-float-close 0.8414709848078965 (p-0075-0001))

(defun p-0075-0002 ()
  (sin 0))
(assert-equal t (%%za-compiled-p (function p-0075-0002)))
(assert-float-close 0.0 (p-0075-0002))

(defun p-0075-0003 ()
  (sin 0.001))
(assert-equal t (%%za-compiled-p (function p-0075-0003)))
(assert-float-close (parse-number "9.999998333333417E-4") (p-0075-0003))

(defun p-0075-0004 ()
  (cos 1))
(assert-equal t (%%za-compiled-p (function p-0075-0004)))
(assert-float-close 0.5403023058681398 (p-0075-0004))

(defun p-0075-0005 ()
  (cos 0))
(assert-equal t (%%za-compiled-p (function p-0075-0005)))
(assert-float-close 1.0 (p-0075-0005))

(defun p-0075-0006 ()
  (cos 0.001))
(assert-equal t (%%za-compiled-p (function p-0075-0006)))
(assert-float-close 0.9999995000000417 (p-0075-0006))

(defun p-0075-0007 ()
  (tan 1))
(assert-equal t (%%za-compiled-p (function p-0075-0007)))
(assert-float-close 1.557407724654902 (p-0075-0007))

(defun p-0075-0008 ()
  (tan 0))
(assert-equal t (%%za-compiled-p (function p-0075-0008)))
(assert-float-close 0.0 (p-0075-0008))

(defun p-0075-0009 ()
  (tan 0.001))
(assert-equal t (%%za-compiled-p (function p-0075-0009)))
(assert-float-close 0.0010000003333334668 (p-0075-0009))

;; p.76 atan2 の例をそのまま転記。asin/acos は仕様通り定義し、atan は組み込み関数と
;; 衝突するため isiki-test-atan に変更(cf.)。
(defun p-0076-0001 ()
  (atan2 0 3.0))
(assert-equal t (%%za-compiled-p (function p-0076-0001)))
(assert-float-close 0.0 (p-0076-0001))

(defun p-0076-0002 ()
  (atan2 1 1))
(assert-equal t (%%za-compiled-p (function p-0076-0002)))
(assert-float-close 0.7853981633974483 (p-0076-0002))

(defun p-0076-0003 ()
  (atan2 1.0 -0.3))
(assert-equal t (%%za-compiled-p (function p-0076-0003)))
(assert-float-close 1.8622531212727635 (p-0076-0003))

(defun p-0076-0004 ()
  (atan2 0.0 -0.5))
(assert-equal t (%%za-compiled-p (function p-0076-0004)))
(assert-float-close 3.141592653589793 (p-0076-0004))

(defun p-0076-0005 ()
  (atan2 -1 -1))
(assert-equal t (%%za-compiled-p (function p-0076-0005)))
(assert-float-close -2.356194490192345 (p-0076-0005))

(defun p-0076-0006 ()
  (atan2 -1.0 0.3))
(assert-equal t (%%za-compiled-p (function p-0076-0006)))
(assert-float-close -1.2793396 (p-0076-0006))

(defun p-0076-0007 ()
  (atan2 0.0 0.5))
(assert-equal t (%%za-compiled-p (function p-0076-0007)))
(assert-equal 0.0 (p-0076-0007))

;; [skip p-0076-0008] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun asin (x) (atan2 x (sqrt (- 1 (expt x 2)))))

;; [skip p-0076-0009] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun acos (x) (atan2 (sqrt (- 1 (expt x 2))) x))

;; [skip p-0076-0010] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun isiki-test-atan (x) (atan2 x 1))

;; cf. 上で定義した asin/acos/isiki-test-atan が組み込みの atan と整合することの確認
(defun p-0076-0011 ()
  (isiki-test-atan 1))
(assert-equal t (%%za-compiled-p (function p-0076-0011)))
(assert-float-close (atan 1) (p-0076-0011))

(defun p-0076-0012 ()
  (asin 0.5))
(assert-equal t (%%za-compiled-p (function p-0076-0012)))
(assert-float-close 0.5235987755982989 (p-0076-0012))

(defun p-0076-0013 ()
  (acos 0.5))
(assert-equal t (%%za-compiled-p (function p-0076-0013)))
(assert-float-close 1.0471975511965979 (p-0076-0013))

;; p.77 sinh/cosh/tanh の例をそのまま転記
(defun p-0077-0001 ()
  (sinh 1))
(assert-equal t (%%za-compiled-p (function p-0077-0001)))
(assert-float-close 1.1752011936438014 (p-0077-0001))

(defun p-0077-0002 ()
  (sinh 0))
(assert-equal t (%%za-compiled-p (function p-0077-0002)))
(assert-float-close 0.0 (p-0077-0002))

(defun p-0077-0003 ()
  (sinh 0.001))
(assert-equal t (%%za-compiled-p (function p-0077-0003)))
(assert-float-close 0.001000000166666675 (p-0077-0003))

(defun p-0077-0004 ()
  (cosh 1))
(assert-equal t (%%za-compiled-p (function p-0077-0004)))
(assert-float-close 1.5430806348152437 (p-0077-0004))

(defun p-0077-0005 ()
  (cosh 0))
(assert-equal t (%%za-compiled-p (function p-0077-0005)))
(assert-float-close 1.0 (p-0077-0005))

(defun p-0077-0006 ()
  (cosh 0.001))
(assert-equal t (%%za-compiled-p (function p-0077-0006)))
(assert-float-close 1.0000005000000416 (p-0077-0006))

(defun p-0077-0007 ()
  (tanh 1))
(assert-equal t (%%za-compiled-p (function p-0077-0007)))
(assert-float-close 0.7615941559557649 (p-0077-0007))

(defun p-0077-0008 ()
  (tanh 0))
(assert-equal t (%%za-compiled-p (function p-0077-0008)))
(assert-float-close 0.0 (p-0077-0008))

(defun p-0077-0009 ()
  (tanh 0.001))
(assert-equal t (%%za-compiled-p (function p-0077-0009)))
(assert-float-close (parse-number "9.999996666668002E-4") (p-0077-0009))

;; p.77 atanh の例をそのまま転記。asinh/acosh は仕様通り定義する。
(defun p-0077-0010 ()
  (atanh 0.5))
(assert-equal t (%%za-compiled-p (function p-0077-0010)))
(assert-float-close 0.5493061443340549 (p-0077-0010))

(defun p-0077-0011 ()
  (atanh 0))
(assert-equal t (%%za-compiled-p (function p-0077-0011)))
(assert-float-close 0.0 (p-0077-0011))

(defun p-0077-0012 ()
  (atanh 0.001))
(assert-equal t (%%za-compiled-p (function p-0077-0012)))
(assert-float-close 0.0010000003333335335 (p-0077-0012))

;; [skip p-0077-0013] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun asinh (x) (atanh (quotient x (sqrt (+ 1 (expt x 2))))))

;; [skip p-0077-0014] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defun acosh (x) (atanh (quotient (sqrt (* (- x 1) (+ x 1))) x)))

;; cf. 上で定義した asinh/acosh の動作確認(sinh/cosh の逆関数になっていること)
(defun p-0077-0015 ()
  (asinh (sinh 1)))
(assert-equal t (%%za-compiled-p (function p-0077-0015)))
(assert-float-close 1.0 (p-0077-0015))

(defun p-0077-0016 ()
  (acosh (cosh 1)))
(assert-equal t (%%za-compiled-p (function p-0077-0016)))
(assert-float-close 1.0 (p-0077-0016))

;; p.78 floatp の例をそのまま転記
(defun p-0078-0001 ()
  (floatp "2.4"))
(assert-equal t (%%za-compiled-p (function p-0078-0001)))
(assert-equal nil (p-0078-0001))

(defun p-0078-0002 ()
  (floatp 2))
(assert-equal t (%%za-compiled-p (function p-0078-0002)))
(assert-equal nil (p-0078-0002))

(defun p-0078-0003 ()
  (floatp 2.0))
(assert-equal t (%%za-compiled-p (function p-0078-0003)))
(assert-equal t (p-0078-0003))

;; p.78 float の例をそのまま転記。bignum の例は絶対誤差では比較できないので比で確認する。
(defun p-0078-0004 ()
  (float 0))
(assert-equal t (%%za-compiled-p (function p-0078-0004)))
(assert-equal 0.0 (p-0078-0004))

(defun p-0078-0005 ()
  (float 2))
(assert-equal t (%%za-compiled-p (function p-0078-0005)))
(assert-equal 2.0 (p-0078-0005))

(defun p-0078-0006 ()
  (float -2.0))
(assert-equal t (%%za-compiled-p (function p-0078-0006)))
(assert-equal -2.0 (p-0078-0006))

(defun p-0078-0007 ()
  (quotient (float 123456789123456789123456789) (parse-number "1.2345678912345679E26")))
(assert-equal t (%%za-compiled-p (function p-0078-0007)))
(assert-float-close 1.0 (p-0078-0007))

;; p.78-79 floor の例をそのまま転記
(defun p-0078-0008 ()
  (floor 3.0))
(assert-equal t (%%za-compiled-p (function p-0078-0008)))
(assert-equal 3 (p-0078-0008))

(defun p-0078-0009 ()
  (floor 3.4))
(assert-equal t (%%za-compiled-p (function p-0078-0009)))
(assert-equal 3 (p-0078-0009))

(defun p-0078-0010 ()
  (floor 3.9))
(assert-equal t (%%za-compiled-p (function p-0078-0010)))
(assert-equal 3 (p-0078-0010))

(defun p-0078-0011 ()
  (floor -3.9))
(assert-equal t (%%za-compiled-p (function p-0078-0011)))
(assert-equal -4 (p-0078-0011))

(defun p-0078-0012 ()
  (floor -3.4))
(assert-equal t (%%za-compiled-p (function p-0078-0012)))
(assert-equal -4 (p-0078-0012))

(defun p-0078-0013 ()
  (floor -3.0))
(assert-equal t (%%za-compiled-p (function p-0078-0013)))
(assert-equal -3 (p-0078-0013))

;; p.79 ceiling の例をそのまま転記
(defun p-0079-0001 ()
  (ceiling 3.0))
(assert-equal t (%%za-compiled-p (function p-0079-0001)))
(assert-equal 3 (p-0079-0001))

(defun p-0079-0002 ()
  (ceiling 3.4))
(assert-equal t (%%za-compiled-p (function p-0079-0002)))
(assert-equal 4 (p-0079-0002))

(defun p-0079-0003 ()
  (ceiling 3.9))
(assert-equal t (%%za-compiled-p (function p-0079-0003)))
(assert-equal 4 (p-0079-0003))

(defun p-0079-0004 ()
  (ceiling -3.9))
(assert-equal t (%%za-compiled-p (function p-0079-0004)))
(assert-equal -3 (p-0079-0004))

(defun p-0079-0005 ()
  (ceiling -3.4))
(assert-equal t (%%za-compiled-p (function p-0079-0005)))
(assert-equal -3 (p-0079-0005))

(defun p-0079-0006 ()
  (ceiling -3.0))
(assert-equal t (%%za-compiled-p (function p-0079-0006)))
(assert-equal -3 (p-0079-0006))

;; p.79 truncate の例をそのまま転記
(defun p-0079-0007 ()
  (truncate 3.0))
(assert-equal t (%%za-compiled-p (function p-0079-0007)))
(assert-equal 3 (p-0079-0007))

(defun p-0079-0008 ()
  (truncate 3.4))
(assert-equal t (%%za-compiled-p (function p-0079-0008)))
(assert-equal 3 (p-0079-0008))

(defun p-0079-0009 ()
  (truncate 3.9))
(assert-equal t (%%za-compiled-p (function p-0079-0009)))
(assert-equal 3 (p-0079-0009))

(defun p-0079-0010 ()
  (truncate -3.4))
(assert-equal t (%%za-compiled-p (function p-0079-0010)))
(assert-equal -3 (p-0079-0010))

(defun p-0079-0011 ()
  (truncate -3.9))
(assert-equal t (%%za-compiled-p (function p-0079-0011)))
(assert-equal -3 (p-0079-0011))

(defun p-0079-0012 ()
  (truncate -3.0))
(assert-equal t (%%za-compiled-p (function p-0079-0012)))
(assert-equal -3 (p-0079-0012))

;; p.79-80 round の例をそのまま転記
(defun p-0079-0013 ()
  (round 3.0))
(assert-equal t (%%za-compiled-p (function p-0079-0013)))
(assert-equal 3 (p-0079-0013))

(defun p-0079-0014 ()
  (round 3.4))
(assert-equal t (%%za-compiled-p (function p-0079-0014)))
(assert-equal 3 (p-0079-0014))

(defun p-0079-0015 ()
  (round -3.4))
(assert-equal t (%%za-compiled-p (function p-0079-0015)))
(assert-equal -3 (p-0079-0015))

(defun p-0079-0016 ()
  (round 3.6))
(assert-equal t (%%za-compiled-p (function p-0079-0016)))
(assert-equal 4 (p-0079-0016))

(defun p-0079-0017 ()
  (round -3.6))
(assert-equal t (%%za-compiled-p (function p-0079-0017)))
(assert-equal -4 (p-0079-0017))

(defun p-0079-0018 ()
  (round 3.5))
(assert-equal t (%%za-compiled-p (function p-0079-0018)))
(assert-equal 4 (p-0079-0018))

(defun p-0079-0019 ()
  (round -3.5))
(assert-equal t (%%za-compiled-p (function p-0079-0019)))
(assert-equal -4 (p-0079-0019))

(defun p-0079-0020 ()
  (round 2.5))
(assert-equal t (%%za-compiled-p (function p-0079-0020)))
(assert-equal 2 (p-0079-0020))

(defun p-0079-0021 ()
  (round -0.5))
(assert-equal t (%%za-compiled-p (function p-0079-0021)))
(assert-equal 0 (p-0079-0021))

;; p.80 integerp の例をそのまま転記
(defun p-0080-0001 ()
  (integerp 3))
(assert-equal t (%%za-compiled-p (function p-0080-0001)))
(assert-equal t (p-0080-0001))

(defun p-0080-0002 ()
  (integerp 3.4))
(assert-equal t (%%za-compiled-p (function p-0080-0002)))
(assert-equal nil (p-0080-0002))

(defun p-0080-0003 ()
  (integerp "4"))
(assert-equal t (%%za-compiled-p (function p-0080-0003)))
(assert-equal nil (p-0080-0003))

(defun p-0080-0004 ()
  (integerp '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0080-0004)))
(assert-equal nil (p-0080-0004))

;; p.81 div / mod の例をそのまま転記
(defun p-0081-0001 ()
  (div 12 3))
(assert-equal t (%%za-compiled-p (function p-0081-0001)))
(assert-equal 4 (p-0081-0001))

(defun p-0081-0002 ()
  (div 14 3))
(assert-equal t (%%za-compiled-p (function p-0081-0002)))
(assert-equal 4 (p-0081-0002))

(defun p-0081-0003 ()
  (div -12 3))
(assert-equal t (%%za-compiled-p (function p-0081-0003)))
(assert-equal -4 (p-0081-0003))

(defun p-0081-0004 ()
  (div -14 3))
(assert-equal t (%%za-compiled-p (function p-0081-0004)))
(assert-equal -5 (p-0081-0004))

(defun p-0081-0005 ()
  (div 12 -3))
(assert-equal t (%%za-compiled-p (function p-0081-0005)))
(assert-equal -4 (p-0081-0005))

(defun p-0081-0006 ()
  (div 14 -3))
(assert-equal t (%%za-compiled-p (function p-0081-0006)))
(assert-equal -5 (p-0081-0006))

(defun p-0081-0007 ()
  (div -12 -3))
(assert-equal t (%%za-compiled-p (function p-0081-0007)))
(assert-equal 4 (p-0081-0007))

(defun p-0081-0008 ()
  (div -14 -3))
(assert-equal t (%%za-compiled-p (function p-0081-0008)))
(assert-equal 4 (p-0081-0008))

(defun p-0081-0009 ()
  (mod 12 3))
(assert-equal t (%%za-compiled-p (function p-0081-0009)))
(assert-equal 0 (p-0081-0009))

(defun p-0081-0010 ()
  (mod 7 247))
(assert-equal t (%%za-compiled-p (function p-0081-0010)))
(assert-equal 7 (p-0081-0010))

(defun p-0081-0011 ()
  (mod 247 7))
(assert-equal t (%%za-compiled-p (function p-0081-0011)))
(assert-equal 2 (p-0081-0011))

(defun p-0081-0012 ()
  (mod 14 3))
(assert-equal t (%%za-compiled-p (function p-0081-0012)))
(assert-equal 2 (p-0081-0012))

(defun p-0081-0013 ()
  (mod -12 3))
(assert-equal t (%%za-compiled-p (function p-0081-0013)))
(assert-equal 0 (p-0081-0013))

(defun p-0081-0014 ()
  (mod -14 3))
(assert-equal t (%%za-compiled-p (function p-0081-0014)))
(assert-equal 1 (p-0081-0014))

(defun p-0081-0015 ()
  (mod 12 -3))
(assert-equal t (%%za-compiled-p (function p-0081-0015)))
(assert-equal 0 (p-0081-0015))

(defun p-0081-0016 ()
  (mod 14 -3))
(assert-equal t (%%za-compiled-p (function p-0081-0016)))
(assert-equal -1 (p-0081-0016))

(defun p-0081-0017 ()
  (mod -12 -3))
(assert-equal t (%%za-compiled-p (function p-0081-0017)))
(assert-equal 0 (p-0081-0017))

(defun p-0081-0018 ()
  (mod -14 -3))
(assert-equal t (%%za-compiled-p (function p-0081-0018)))
(assert-equal -2 (p-0081-0018))

;; p.81-82 gcd / lcm の例をそのまま転記
(defun p-0081-0019 ()
  (gcd 12 5))
(assert-equal t (%%za-compiled-p (function p-0081-0019)))
(assert-equal 1 (p-0081-0019))

(defun p-0081-0020 ()
  (gcd 15 24))
(assert-equal t (%%za-compiled-p (function p-0081-0020)))
(assert-equal 3 (p-0081-0020))

(defun p-0081-0021 ()
  (gcd -15 24))
(assert-equal t (%%za-compiled-p (function p-0081-0021)))
(assert-equal 3 (p-0081-0021))

(defun p-0081-0022 ()
  (gcd 15 -24))
(assert-equal t (%%za-compiled-p (function p-0081-0022)))
(assert-equal 3 (p-0081-0022))

(defun p-0081-0023 ()
  (gcd -15 -24))
(assert-equal t (%%za-compiled-p (function p-0081-0023)))
(assert-equal 3 (p-0081-0023))

(defun p-0081-0024 ()
  (gcd 0 -4))
(assert-equal t (%%za-compiled-p (function p-0081-0024)))
(assert-equal 4 (p-0081-0024))

(defun p-0081-0025 ()
  (gcd 0 0))
(assert-equal t (%%za-compiled-p (function p-0081-0025)))
(assert-equal 0 (p-0081-0025))

(defun p-0081-0026 ()
  (lcm 2 3))
(assert-equal t (%%za-compiled-p (function p-0081-0026)))
(assert-equal 6 (p-0081-0026))

(defun p-0081-0027 ()
  (lcm 15 24))
(assert-equal t (%%za-compiled-p (function p-0081-0027)))
(assert-equal 120 (p-0081-0027))

(defun p-0081-0028 ()
  (lcm 15 -24))
(assert-equal t (%%za-compiled-p (function p-0081-0028)))
(assert-equal 120 (p-0081-0028))

(defun p-0081-0029 ()
  (lcm -15 24))
(assert-equal t (%%za-compiled-p (function p-0081-0029)))
(assert-equal 120 (p-0081-0029))

(defun p-0081-0030 ()
  (lcm -15 -24))
(assert-equal t (%%za-compiled-p (function p-0081-0030)))
(assert-equal 120 (p-0081-0030))

(defun p-0081-0031 ()
  (lcm 0 -4))
(assert-equal t (%%za-compiled-p (function p-0081-0031)))
(assert-equal 0 (p-0081-0031))

(defun p-0081-0032 ()
  (lcm 0 0))
(assert-equal t (%%za-compiled-p (function p-0081-0032)))
(assert-equal 0 (p-0081-0032))

;; p.82 isqrt の例をそのまま転記
(defun p-0082-0001 ()
  (isqrt 49))
(assert-equal t (%%za-compiled-p (function p-0082-0001)))
(assert-equal 7 (p-0082-0001))

(defun p-0082-0002 ()
  (isqrt 63))
(assert-equal t (%%za-compiled-p (function p-0082-0002)))
(assert-equal 7 (p-0082-0002))

(defun p-0082-0003 ()
  (isqrt 1000000000000002000000000000000))
(assert-equal t (%%za-compiled-p (function p-0082-0003)))
(assert-equal 1000000000000000 (p-0082-0003))

;;; ===========================================================================
;;; §20 Character class (p.83-84)
;;; ===========================================================================

;; p.83 characterp の例をそのまま転記
(defun p-0083-0001 ()
  (characterp #\a))
(assert-equal t (%%za-compiled-p (function p-0083-0001)))
(assert-equal t (p-0083-0001))

(defun p-0083-0002 ()
  (characterp "a"))
(assert-equal t (%%za-compiled-p (function p-0083-0002)))
(assert-equal nil (p-0083-0002))

(defun p-0083-0003 ()
  (characterp 'a))
(assert-equal t (%%za-compiled-p (function p-0083-0003)))
(assert-equal nil (p-0083-0003))

;; p.84 文字比較の例をそのまま転記((char< #\a #\A) / (char< #\* #\a) / (char<= #\a #\A) は
;; implementation-defined のため省略)
(defun p-0084-0001 ()
  (char= #\a #\a))
(assert-equal t (%%za-compiled-p (function p-0084-0001)))
(assert-equal t (p-0084-0001))

(defun p-0084-0002 ()
  (char= #\a #\b))
(assert-equal t (%%za-compiled-p (function p-0084-0002)))
(assert-equal nil (p-0084-0002))

(defun p-0084-0003 ()
  (char= #\a #\A))
(assert-equal t (%%za-compiled-p (function p-0084-0003)))
(assert-equal nil (p-0084-0003))

(defun p-0084-0004 ()
  (char/= #\a #\a))
(assert-equal t (%%za-compiled-p (function p-0084-0004)))
(assert-equal nil (p-0084-0004))

(defun p-0084-0005 ()
  (char< #\a #\a))
(assert-equal t (%%za-compiled-p (function p-0084-0005)))
(assert-equal nil (p-0084-0005))

(defun p-0084-0006 ()
  (char< #\a #\b))
(assert-equal t (%%za-compiled-p (function p-0084-0006)))
(assert-equal t (p-0084-0006))

(defun p-0084-0007 ()
  (char< #\b #\a))
(assert-equal t (%%za-compiled-p (function p-0084-0007)))
(assert-equal nil (p-0084-0007))

(defun p-0084-0008 ()
  (char> #\b #\a))
(assert-equal t (%%za-compiled-p (function p-0084-0008)))
(assert-equal t (p-0084-0008))

(defun p-0084-0009 ()
  (char<= #\a #\a))
(assert-equal t (%%za-compiled-p (function p-0084-0009)))
(assert-equal t (p-0084-0009))

(defun p-0084-0010 ()
  (char>= #\b #\a))
(assert-equal t (%%za-compiled-p (function p-0084-0010)))
(assert-equal t (p-0084-0010))

(defun p-0084-0011 ()
  (char>= #\a #\a))
(assert-equal t (%%za-compiled-p (function p-0084-0011)))
(assert-equal t (p-0084-0011))

;;; ===========================================================================
;;; §21 List class (p.85-91)
;;; ===========================================================================

;; p.85 consp の例をそのまま転記
(defun p-0085-0001 ()
  (consp '(a . b)))
(assert-equal t (%%za-compiled-p (function p-0085-0001)))
(assert-equal t (p-0085-0001))

(defun p-0085-0002 ()
  (consp '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0085-0002)))
(assert-equal t (p-0085-0002))

(defun p-0085-0003 ()
  (consp '()))
(assert-equal t (%%za-compiled-p (function p-0085-0003)))
(assert-equal nil (p-0085-0003))

(defun p-0085-0004 ()
  (consp #(a b)))
(assert-equal t (%%za-compiled-p (function p-0085-0004)))
(assert-equal nil (p-0085-0004))

;; p.85 cons の例をそのまま転記
(defun p-0085-0005 ()
  (cons 'a '()))
(assert-equal t (%%za-compiled-p (function p-0085-0005)))
(assert-equal '(a) (p-0085-0005))

(defun p-0085-0006 ()
  (cons '(a) '(b c d)))
(assert-equal t (%%za-compiled-p (function p-0085-0006)))
(assert-equal '((a) b c d) (p-0085-0006))

(defun p-0085-0007 ()
  (cons "a" '(b c)))
(assert-equal t (%%za-compiled-p (function p-0085-0007)))
(assert-equal '("a" b c) (p-0085-0007))

(defun p-0085-0008 ()
  (cons 'a 3))
(assert-equal t (%%za-compiled-p (function p-0085-0008)))
(assert-equal '(a . 3) (p-0085-0008))

(defun p-0085-0009 ()
  (cons '(a b) 'c))
(assert-equal t (%%za-compiled-p (function p-0085-0009)))
(assert-equal '((a b) . c) (p-0085-0009))

;; p.85-86 car の例をそのまま転記((car '()) はエラー)
(defun p-0085-0010 ()
  (car '()))
(assert-equal t (%%za-compiled-p (function p-0085-0010)))
(assert-error (p-0085-0010))

(defun p-0085-0011 ()
  (car '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0085-0011)))
(assert-equal 'a (p-0085-0011))

(defun p-0085-0012 ()
  (car '((a) b c d)))
(assert-equal t (%%za-compiled-p (function p-0085-0012)))
(assert-equal '(a) (p-0085-0012))

(defun p-0085-0013 ()
  (car '(1 . 2)))
(assert-equal t (%%za-compiled-p (function p-0085-0013)))
(assert-equal 1 (p-0085-0013))

;; p.86 cdr の例をそのまま転記((cdr '()) はエラー)
(defun p-0086-0001 ()
  (cdr '()))
(assert-equal t (%%za-compiled-p (function p-0086-0001)))
(assert-error (p-0086-0001))

(defun p-0086-0002 ()
  (cdr '((a) b c d)))
(assert-equal t (%%za-compiled-p (function p-0086-0002)))
(assert-equal '(b c d) (p-0086-0002))

(defun p-0086-0003 ()
  (cdr '(1 . 2)))
(assert-equal t (%%za-compiled-p (function p-0086-0003)))
(assert-equal 2 (p-0086-0003))

;; p.86 (setf (car x) 'banana) の例をそのまま転記
(defun p-0086-0004 ()
  (let ((x (list 'apple 'orange)))
    (list x (car x)
          (setf (car x) 'banana)
          x (car x))))
(assert-equal t (%%za-compiled-p (function p-0086-0004)))
(assert-equal '((banana orange) apple banana (banana orange) banana) (p-0086-0004))

;; p.86 (setf (cdr x) 'banana) の例をそのまま転記
(defun p-0086-0005 ()
  (let ((x (list 'apple 'orange)))
    (list x (cdr x)
          (setf (cdr x) 'banana)
          x (cdr x))))
(assert-equal t (%%za-compiled-p (function p-0086-0005)))
(assert-equal '((apple . banana) (orange) banana (apple . banana) banana) (p-0086-0005))

;; p.87 null の例をそのまま転記
(defun p-0087-0001 ()
  (null '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0087-0001)))
(assert-equal nil (p-0087-0001))

(defun p-0087-0002 ()
  (null '()))
(assert-equal t (%%za-compiled-p (function p-0087-0002)))
(assert-equal t (p-0087-0002))

(defun p-0087-0003 ()
  (null (list)))
(assert-equal t (%%za-compiled-p (function p-0087-0003)))
(assert-equal t (p-0087-0003))

;; p.87 listp の例をそのまま転記(循環リストの例を含む)
(defun p-0087-0004 ()
  (listp '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0087-0004)))
(assert-equal t (p-0087-0004))

(defun p-0087-0005 ()
  (listp '()))
(assert-equal t (%%za-compiled-p (function p-0087-0005)))
(assert-equal t (p-0087-0005))

(defun p-0087-0006 ()
  (listp '(a . b)))
(assert-equal t (%%za-compiled-p (function p-0087-0006)))
(assert-equal t (p-0087-0006))

(defun p-0087-0007 ()
  (let ((x (list 'a)))
    (setf (cdr x) x)
    (listp x)))
(assert-equal t (%%za-compiled-p (function p-0087-0007)))
(assert-equal t (p-0087-0007))

(defun p-0087-0008 ()
  (listp "abc"))
(assert-equal t (%%za-compiled-p (function p-0087-0008)))
(assert-equal nil (p-0087-0008))

(defun p-0087-0009 ()
  (listp #(1 2)))
(assert-equal t (%%za-compiled-p (function p-0087-0009)))
(assert-equal nil (p-0087-0009))

(defun p-0087-0010 ()
  (listp 'jerome))
(assert-equal t (%%za-compiled-p (function p-0087-0010)))
(assert-equal nil (p-0087-0010))

;; p.88 create-list の例をそのまま転記
(defun p-0088-0001 ()
  (create-list 3 17))
(assert-equal t (%%za-compiled-p (function p-0088-0001)))
(assert-equal '(17 17 17) (p-0088-0001))

(defun p-0088-0002 ()
  (create-list 2 #\a))
(assert-equal t (%%za-compiled-p (function p-0088-0002)))
(assert-equal '(#\a #\a) (p-0088-0002))

;; p.88 list の例をそのまま転記
(defun p-0088-0003 ()
  (list 'a (+ 3 4) 'c))
(assert-equal t (%%za-compiled-p (function p-0088-0003)))
(assert-equal '(a 7 c) (p-0088-0003))

(defun p-0088-0004 ()
  (list))
(assert-equal t (%%za-compiled-p (function p-0088-0004)))
(assert-equal nil (p-0088-0004))

;; p.88 reverse の例をそのまま転記(nreverse の例は implementation-defined のため省略)
(defun p-0088-0005 ()
  (reverse '(a b c d e)))
(assert-equal t (%%za-compiled-p (function p-0088-0005)))
(assert-equal '(e d c b a) (p-0088-0005))

(defun p-0088-0006 ()
  (reverse '(a)))
(assert-equal t (%%za-compiled-p (function p-0088-0006)))
(assert-equal '(a) (p-0088-0006))

(defun p-0088-0007 ()
  (reverse '()))
(assert-equal t (%%za-compiled-p (function p-0088-0007)))
(assert-equal '() (p-0088-0007))

;; p.89 (append '(a b c) '(d e f)) => (a b c d e f) をそのまま転記
(defun p-0089-0001 ()
  (append '(a b c) '(d e f)))
(assert-equal t (%%za-compiled-p (function p-0089-0001)))
(assert-equal '(a b c d e f) (p-0089-0001))

;; p.89 member の例をそのまま転記
(defun p-0089-0002 ()
  (member 'c '(a b c d e f)))
(assert-equal t (%%za-compiled-p (function p-0089-0002)))
(assert-equal '(c d e f) (p-0089-0002))

(defun p-0089-0003 ()
  (member 'g '(a b c d e f)))
(assert-equal t (%%za-compiled-p (function p-0089-0003)))
(assert-equal nil (p-0089-0003))

(defun p-0089-0004 ()
  (member 'c '(a b c a b c)))
(assert-equal t (%%za-compiled-p (function p-0089-0004)))
(assert-equal '(c a b c) (p-0089-0004))

;; p.90-91 mapcar/mapc/maplist/mapl/mapcan/mapcon の例をそのまま転記
(defun p-0090-0001 ()
  (mapcar #'car '((1 a) (2 b) (3 c))))
(assert-equal t (%%za-compiled-p (function p-0090-0001)))
(assert-equal '(1 2 3) (p-0090-0001))

(defun p-0090-0002 ()
  (mapcar #'abs '(3 -4 2 -5 -6)))
(assert-equal t (%%za-compiled-p (function p-0090-0002)))
(assert-equal '(3 4 2 5 6) (p-0090-0002))

(defun p-0090-0003 ()
  (mapcar #'cons '(a b c) '(1 2 3)))
(assert-equal t (%%za-compiled-p (function p-0090-0003)))
(assert-equal '((a . 1) (b . 2) (c . 3)) (p-0090-0003))

(defun p-0090-0004 ()
  (let ((x 0)) (mapc (lambda (v) (setq x (+ x v))) '(3 5)) x))
(assert-equal t (%%za-compiled-p (function p-0090-0004)))
(assert-equal 8 (p-0090-0004))

(defun p-0090-0005 ()
  (maplist #'append '(1 2 3 4) '(1 2) '(1 2 3)))
(assert-equal t (%%za-compiled-p (function p-0090-0005)))
(assert-equal '((1 2 3 4 1 2 1 2 3) (2 3 4 2 2 3)) (p-0090-0005))

(defun p-0090-0006 ()
  (maplist (lambda (x) (cons 'foo x)) '(a b c d)))
(assert-equal t (%%za-compiled-p (function p-0090-0006)))
(assert-equal '((foo a b c d) (foo b c d) (foo c d) (foo d)) (p-0090-0006))

(defun p-0090-0007 ()
  (maplist (lambda (x) (if (member (car x) (cdr x)) 0 1))
           '(a b a c d b c)))
(assert-equal t (%%za-compiled-p (function p-0090-0007)))
(assert-equal '(0 0 1 0 1 1 1) (p-0090-0007))

(defun p-0090-0008 ()
  (let ((k 0))
    (mapl (lambda (x)
            (setq k (+ k (if (member (car x) (cdr x)) 0 1))))
          '(a b a c d b c))
    k))
(assert-equal t (%%za-compiled-p (function p-0090-0008)))
(assert-equal 4 (p-0090-0008))

(defun p-0090-0009 ()
  (mapcan (lambda (x) (if (> x 0) (list x))) '(-3 4 0 5 -2 7)))
(assert-equal t (%%za-compiled-p (function p-0090-0009)))
(assert-equal '(4 5 7) (p-0090-0009))

(defun p-0090-0010 ()
  (mapcon (lambda (x) (if (member (car x) (cdr x)) (list (car x))))
          '(a b a c d b c b c)))
(assert-equal t (%%za-compiled-p (function p-0090-0010)))
(assert-equal '(a b c b c) (p-0090-0010))

(defun p-0090-0011 ()
  (mapcon #'list '(1 2 3 4)))
(assert-equal t (%%za-compiled-p (function p-0090-0011)))
(assert-equal '((1 2 3 4) (2 3 4) (3 4) (4)) (p-0090-0011))

;; p.91 assoc の例をそのまま転記
(defun p-0091-0001 ()
  (assoc 'a '((a . 1) (b . 2))))
(assert-equal t (%%za-compiled-p (function p-0091-0001)))
(assert-equal '(a . 1) (p-0091-0001))

(defun p-0091-0002 ()
  (assoc 'a '((a . 1) (a . 2))))
(assert-equal t (%%za-compiled-p (function p-0091-0002)))
(assert-equal '(a . 1) (p-0091-0002))

(defun p-0091-0003 ()
  (assoc 'c '((a . 1) (b . 2))))
(assert-equal t (%%za-compiled-p (function p-0091-0003)))
(assert-equal nil (p-0091-0003))

;;; ===========================================================================
;;; §22 Arrays (p.93-95)
;;; ===========================================================================

;; p.93 basic-array-p/basic-array*-p/general-array*-p の例。#1a(a b c) / #2a((a) (b) (c))
;; はリーダー構文依存のため、isiki_test_syntax.lisp にそのまま転記し、ここでは
;; 同じ外延を create-array で組み立てた版(cf.)を置く。
(defun p-0093-0001 ()
  (mapcar (lambda (x)
            (list (basic-array-p x)
                  (basic-array*-p x)
                  (general-array*-p x)))
          (list '(a b c)
                "abc"
                #(a b c)
                (create-array '(3) 'a)
                (create-array '(3 1) 'a))))
(assert-equal t (%%za-compiled-p (function p-0093-0001)))
(assert-equal '((nil nil nil) (t nil nil) (t nil nil) (t nil nil) (t t t)) (p-0093-0001))

;; p.93 create-array の例。1つ目の期待値 #2a(...) はリーダー構文依存のため
;; isiki_test_syntax.lisp 側で比較し、ここでは次元と要素で確認する(cf.)。
(defun p-0093-0002 ()
  (array-dimensions (create-array '(2 3) 0.0)))
(assert-equal t (%%za-compiled-p (function p-0093-0002)))
(assert-equal '(2 3) (p-0093-0002))

(defun p-0093-0003 ()
  (aref (create-array '(2 3) 0.0) 1 2))
(assert-equal t (%%za-compiled-p (function p-0093-0003)))
(assert-equal 0.0 (p-0093-0003))

(defun p-0093-0004 ()
  (create-array '(2) 0.0))
(assert-equal t (%%za-compiled-p (function p-0093-0004)))
(assert-equal #(0.0 0.0) (p-0093-0004))

;; p.94 aref / set-aref の例をそのまま転記(array1 の印字表現の比較は
;; isiki_test_syntax.lisp 側)
;; [skip p-0094-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal array1 (create-array '(3 3 3) 0))

(defun p-0094-0002 ()
  (aref array1 0 1 2))
(assert-equal t (%%za-compiled-p (function p-0094-0002)))
(assert-equal 0 (p-0094-0002))

(defun p-0094-0003 ()
  (setf (aref array1 0 1 2) 3.14))
(assert-equal t (%%za-compiled-p (function p-0094-0003)))
(assert-equal 3.14 (p-0094-0003))

(defun p-0094-0004 ()
  (aref array1 0 1 2))
(assert-equal t (%%za-compiled-p (function p-0094-0004)))
(assert-equal 3.14 (p-0094-0004))

(defun p-0094-0005 ()
  (aref (create-array '(8 8) 6) 1 1))
(assert-equal t (%%za-compiled-p (function p-0094-0005)))
(assert-equal 6 (p-0094-0005))

(defun p-0094-0006 ()
  (aref (create-array '() 19)))
(assert-equal t (%%za-compiled-p (function p-0094-0006)))
(assert-equal 19 (p-0094-0006))

(defun p-0094-0007 ()
  (setf (aref array1 0 1 2) 3.15))
(assert-equal t (%%za-compiled-p (function p-0094-0007)))
(assert-equal 3.15 (p-0094-0007))

(defun p-0094-0008 ()
  (set-aref 51.3 array1 0 1 2))
(assert-equal t (%%za-compiled-p (function p-0094-0008)))
(assert-equal 51.3 (p-0094-0008))

(defun p-0094-0009 ()
  (aref array1 0 1 2))
(assert-equal t (%%za-compiled-p (function p-0094-0009)))
(assert-equal 51.3 (p-0094-0009))

;; p.95 array-dimensions の例をそのまま転記
(defun p-0095-0001 ()
  (array-dimensions (create-array '(2 2) 0)))
(assert-equal t (%%za-compiled-p (function p-0095-0001)))
(assert-equal '(2 2) (p-0095-0001))

(defun p-0095-0002 ()
  (array-dimensions (vector 'a 'b)))
(assert-equal t (%%za-compiled-p (function p-0095-0002)))
(assert-equal '(2) (p-0095-0002))

(defun p-0095-0003 ()
  (array-dimensions "foo"))
(assert-equal t (%%za-compiled-p (function p-0095-0003)))
(assert-equal '(3) (p-0095-0003))

;;; ===========================================================================
;;; §23 Vectors (p.95-96)
;;; ===========================================================================

;; p.95 basic-vector-p/general-vector-p の例。#1a/#2a の項目はリーダー構文依存のため
;; isiki_test_syntax.lisp にそのまま転記し、ここでは create-array で組み立てた版(cf.)。
(defun p-0095-0004 ()
  (mapcar (lambda (x)
            (list (basic-vector-p x)
                  (general-vector-p x)))
          (list '(a b c)
                "abc"
                #(a b c)
                (create-array '(3) 'a)
                (create-array '(3 1) 'a))))
(assert-equal t (%%za-compiled-p (function p-0095-0004)))
(assert-equal '((nil nil) (t nil) (t t) (t t) (nil nil)) (p-0095-0004))

;; p.96 create-vector の例をそのまま転記
(defun p-0096-0001 ()
  (create-vector 3 17))
(assert-equal t (%%za-compiled-p (function p-0096-0001)))
(assert-equal #(17 17 17) (p-0096-0001))

(defun p-0096-0002 ()
  (create-vector 2 #\a))
(assert-equal t (%%za-compiled-p (function p-0096-0002)))
(assert-equal #(#\a #\a) (p-0096-0002))

;; p.96 vector の例をそのまま転記
(defun p-0096-0003 ()
  (vector 'a 'b 'c))
(assert-equal t (%%za-compiled-p (function p-0096-0003)))
(assert-equal #(a b c) (p-0096-0003))

;; (vector) => #() の #() リテラルとの比較は isiki_test_syntax.lisp 側。ここでは長さ0の
;; general-vector が返ることを確認する(cf.)
(defun p-0096-0004 ()
  (length (vector)))
(assert-equal t (%%za-compiled-p (function p-0096-0004)))
(assert-equal 0 (p-0096-0004))

(defun p-0096-0005 ()
  (general-vector-p (vector)))
(assert-equal t (%%za-compiled-p (function p-0096-0005)))
(assert-equal t (p-0096-0005))

;;; ===========================================================================
;;; §24 String class (p.97-99)
;;; ===========================================================================

;; p.97 (stringp "abc") => t / (stringp 'abc) => nil をそのまま転記
(defun p-0097-0001 ()
  (stringp "abc"))
(assert-equal t (%%za-compiled-p (function p-0097-0001)))
(assert-equal t (p-0097-0001))

(defun p-0097-0002 ()
  (stringp 'abc))
(assert-equal t (%%za-compiled-p (function p-0097-0002)))
(assert-equal nil (p-0097-0002))

;; p.97 (create-string 3 #\a) => "aaa" / (create-string 0 #\a) => "" をそのまま転記
(defun p-0097-0003 ()
  (create-string 3 #\a))
(assert-equal t (%%za-compiled-p (function p-0097-0003)))
(assert-equal "aaa" (p-0097-0003))

(defun p-0097-0004 ()
  (create-string 0 #\a))
(assert-equal t (%%za-compiled-p (function p-0097-0004)))
(assert-equal "" (p-0097-0004))

;; p.98 string比較関数の例をそのまま転記
(defun p-0098-0001 ()
  (if (string= "abcd" "abcd") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0001)))
(assert-equal t (p-0098-0001))

(defun p-0098-0002 ()
  (if (string= "abcd" "wxyz") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0002)))
(assert-equal nil (p-0098-0002))

(defun p-0098-0003 ()
  (if (string= "abcd" "abcde") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0003)))
(assert-equal nil (p-0098-0003))

(defun p-0098-0004 ()
  (if (string= "abcde" "abcd") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0004)))
(assert-equal nil (p-0098-0004))

(defun p-0098-0005 ()
  (if (string/= "abcd" "wxyz") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0005)))
(assert-equal t (p-0098-0005))

(defun p-0098-0006 ()
  (if (string< "abcd" "abcd") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0006)))
(assert-equal nil (p-0098-0006))

(defun p-0098-0007 ()
  (if (string< "abcd" "wxyz") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0007)))
(assert-equal t (p-0098-0007))

(defun p-0098-0008 ()
  (if (string< "abcd" "abcde") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0008)))
(assert-equal t (p-0098-0008))

(defun p-0098-0009 ()
  (if (string< "abcde" "abcd") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0009)))
(assert-equal nil (p-0098-0009))

(defun p-0098-0010 ()
  (if (string<= "abcd" "abcd") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0010)))
(assert-equal t (p-0098-0010))

(defun p-0098-0011 ()
  (if (string<= "abcd" "wxyz") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0011)))
(assert-equal t (p-0098-0011))

(defun p-0098-0012 ()
  (if (string<= "abcd" "abcde") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0012)))
(assert-equal t (p-0098-0012))

(defun p-0098-0013 ()
  (if (string<= "abcde" "abcd") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0013)))
(assert-equal nil (p-0098-0013))

(defun p-0098-0014 ()
  (if (string> "abcd" "wxyz") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0014)))
(assert-equal nil (p-0098-0014))

(defun p-0098-0015 ()
  (if (string>= "abcd" "abcd") t nil))
(assert-equal t (%%za-compiled-p (function p-0098-0015)))
(assert-equal t (p-0098-0015))

;; p.98 char-index の例をそのまま転記
(defun p-0098-0016 ()
  (char-index #\b "abcab"))
(assert-equal t (%%za-compiled-p (function p-0098-0016)))
(assert-equal 1 (p-0098-0016))

(defun p-0098-0017 ()
  (char-index #\B "abcab"))
(assert-equal t (%%za-compiled-p (function p-0098-0017)))
(assert-equal nil (p-0098-0017))

(defun p-0098-0018 ()
  (char-index #\b "abcab" 2))
(assert-equal t (%%za-compiled-p (function p-0098-0018)))
(assert-equal 4 (p-0098-0018))

(defun p-0098-0019 ()
  (char-index #\d "abcab"))
(assert-equal t (%%za-compiled-p (function p-0098-0019)))
(assert-equal nil (p-0098-0019))

(defun p-0098-0020 ()
  (char-index #\a "abcab" 4))
(assert-equal t (%%za-compiled-p (function p-0098-0020)))
(assert-equal nil (p-0098-0020))

;; p.99 string-index の例をそのまま転記
(defun p-0099-0001 ()
  (string-index "foo" "foobar"))
(assert-equal t (%%za-compiled-p (function p-0099-0001)))
(assert-equal 0 (p-0099-0001))

(defun p-0099-0002 ()
  (string-index "bar" "foobar"))
(assert-equal t (%%za-compiled-p (function p-0099-0002)))
(assert-equal 3 (p-0099-0002))

(defun p-0099-0003 ()
  (string-index "FOO" "foobar"))
(assert-equal t (%%za-compiled-p (function p-0099-0003)))
(assert-equal nil (p-0099-0003))

(defun p-0099-0004 ()
  (string-index "foo" "foobar" 1))
(assert-equal t (%%za-compiled-p (function p-0099-0004)))
(assert-equal nil (p-0099-0004))

(defun p-0099-0005 ()
  (string-index "bar" "foobar" 1))
(assert-equal t (%%za-compiled-p (function p-0099-0005)))
(assert-equal 3 (p-0099-0005))

(defun p-0099-0006 ()
  (string-index "foo" ""))
(assert-equal t (%%za-compiled-p (function p-0099-0006)))
(assert-equal nil (p-0099-0006))

(defun p-0099-0007 ()
  (string-index "" "foo"))
(assert-equal t (%%za-compiled-p (function p-0099-0007)))
(assert-equal 0 (p-0099-0007))

;; p.99 string-append の例をそのまま転記
(defun p-0099-0008 ()
  (string-append "abc" "def"))
(assert-equal t (%%za-compiled-p (function p-0099-0008)))
(assert-equal "abcdef" (p-0099-0008))

(defun p-0099-0009 ()
  (string-append "abc" "abc"))
(assert-equal t (%%za-compiled-p (function p-0099-0009)))
(assert-equal "abcabc" (p-0099-0009))

(defun p-0099-0010 ()
  (string-append "abc" ""))
(assert-equal t (%%za-compiled-p (function p-0099-0010)))
(assert-equal "abc" (p-0099-0010))

(defun p-0099-0011 ()
  (string-append "" "abc"))
(assert-equal t (%%za-compiled-p (function p-0099-0011)))
(assert-equal "abc" (p-0099-0011))

(defun p-0099-0012 ()
  (string-append "abc" "" "def"))
(assert-equal t (%%za-compiled-p (function p-0099-0012)))
(assert-equal "abcdef" (p-0099-0012))

;;; ===========================================================================
;;; §25 Sequence functions (p.100-102)
;;; ===========================================================================

;; p.100 length の例をそのまま転記
(defun p-0100-0001 ()
  (length '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0100-0001)))
(assert-equal 3 (p-0100-0001))

(defun p-0100-0002 ()
  (length '(a (b) (c d e))))
(assert-equal t (%%za-compiled-p (function p-0100-0002)))
(assert-equal 3 (p-0100-0002))

(defun p-0100-0003 ()
  (length '()))
(assert-equal t (%%za-compiled-p (function p-0100-0003)))
(assert-equal 0 (p-0100-0003))

(defun p-0100-0004 ()
  (length (vector 'a 'b 'c)))
(assert-equal t (%%za-compiled-p (function p-0100-0004)))
(assert-equal 3 (p-0100-0004))

;; p.100 elt の例をそのまま転記
(defun p-0100-0005 ()
  (elt '(a b c) 2))
(assert-equal t (%%za-compiled-p (function p-0100-0005)))
(assert-equal 'c (p-0100-0005))

(defun p-0100-0006 ()
  (elt (vector 'a 'b 'c) 1))
(assert-equal t (%%za-compiled-p (function p-0100-0006)))
(assert-equal 'b (p-0100-0006))

(defun p-0100-0007 ()
  (elt "abc" 0))
(assert-equal t (%%za-compiled-p (function p-0100-0007)))
(assert-equal #\a (p-0100-0007))

;; p.100 (setf (elt string 2) #\O) の例をそのまま転記(仕様の最後の行の x は
;; string の誤記とみなす)
(defun p-0100-0008 ()
  (let ((string (create-string 5 #\x)))
    (setf (elt string 2) #\O)
    string))
(assert-equal t (%%za-compiled-p (function p-0100-0008)))
(assert-equal "xxOxx" (p-0100-0008))

;; p.101 subseq の例をそのまま転記
(defun p-0101-0001 ()
  (subseq "abcdef" 1 4))
(assert-equal t (%%za-compiled-p (function p-0101-0001)))
(assert-equal "bcd" (p-0101-0001))

(defun p-0101-0002 ()
  (subseq '(a b c d e f) 1 4))
(assert-equal t (%%za-compiled-p (function p-0101-0002)))
(assert-equal '(b c d) (p-0101-0002))

(defun p-0101-0003 ()
  (subseq (vector 'a 'b 'c 'd 'e 'f) 1 4))
(assert-equal t (%%za-compiled-p (function p-0101-0003)))
(assert-equal #(b c d) (p-0101-0003))

;; p.101-102 map-into の例をそのまま転記(仕様の setq a/b/k は未定義変数への setq
;; なので、先に defglobal で変数を用意してから仕様通り setq する)
(defglobal a nil)

(defglobal b nil)

(defglobal k nil)

(defun p-0101-0004 ()
  (setq a (list 1 2 3 4)))
(assert-equal t (%%za-compiled-p (function p-0101-0004)))
(assert-equal '(1 2 3 4) (p-0101-0004))

(defun p-0101-0005 ()
  (setq b (list 10 10 10 10)))
(assert-equal t (%%za-compiled-p (function p-0101-0005)))
(assert-equal '(10 10 10 10) (p-0101-0005))

(defun p-0101-0006 ()
  (map-into a #'+ a b))
(assert-equal t (%%za-compiled-p (function p-0101-0006)))
(assert-equal '(11 12 13 14) (p-0101-0006))

(defun p-0101-0007 ()
  a)
(assert-equal t (%%za-compiled-p (function p-0101-0007)))
(assert-equal '(11 12 13 14) (p-0101-0007))

(defun p-0101-0008 ()
  b)
(assert-equal t (%%za-compiled-p (function p-0101-0008)))
(assert-equal '(10 10 10 10) (p-0101-0008))

(defun p-0101-0009 ()
  (setq k '(one two three)))
(assert-equal t (%%za-compiled-p (function p-0101-0009)))
(assert-equal '(one two three) (p-0101-0009))

(defun p-0101-0010 ()
  (map-into a #'cons k a))
(assert-equal t (%%za-compiled-p (function p-0101-0010)))
(assert-equal '((one . 11) (two . 12) (three . 13) 14) (p-0101-0010))

(defun p-0101-0011 ()
  (let ((x 0))
    (map-into a
              (lambda () (setq x (+ x 2))))))
(assert-equal t (%%za-compiled-p (function p-0101-0011)))
(assert-equal '(2 4 6 8) (p-0101-0011))

(defun p-0101-0012 ()
  a)
(assert-equal t (%%za-compiled-p (function p-0101-0012)))
(assert-equal '(2 4 6 8) (p-0101-0012))

;;; ===========================================================================
;;; §26 Stream class (p.102-106)
;;; ===========================================================================

;; p.102 streamp の例をそのまま転記
(defun p-0102-0001 ()
  (streamp (standard-input)))
(assert-equal t (%%za-compiled-p (function p-0102-0001)))
(assert-equal t (p-0102-0001))

(defun p-0102-0002 ()
  (streamp '()))
(assert-equal t (%%za-compiled-p (function p-0102-0002)))
(assert-equal nil (p-0102-0002))

;; p.102 input-stream-p の例をそのまま転記
(defun p-0102-0003 ()
  (input-stream-p (standard-input)))
(assert-equal t (%%za-compiled-p (function p-0102-0003)))
(assert-equal t (p-0102-0003))

(defun p-0102-0004 ()
  (input-stream-p (standard-output)))
(assert-equal t (%%za-compiled-p (function p-0102-0004)))
(assert-equal nil (p-0102-0004))

(defun p-0102-0005 ()
  (input-stream-p '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0102-0005)))
(assert-equal nil (p-0102-0005))

;; p.103 output-stream-p の例をそのまま転記
(defun p-0103-0001 ()
  (output-stream-p (standard-output)))
(assert-equal t (%%za-compiled-p (function p-0103-0001)))
(assert-equal t (p-0103-0001))

(defun p-0103-0002 ()
  (output-stream-p (standard-input)))
(assert-equal t (%%za-compiled-p (function p-0103-0002)))
(assert-equal nil (p-0103-0002))

(defun p-0103-0003 ()
  (output-stream-p "hello"))
(assert-equal t (%%za-compiled-p (function p-0103-0003)))
(assert-equal nil (p-0103-0003))

;; p.103 with-standard-input の例をそのまま転記((read) は引数省略で standard-input から読む)
(defun p-0103-0004 ()
  (with-standard-input (create-string-input-stream "this is a string")
    (list (read) (read))))
(assert-equal t (%%za-compiled-p (function p-0103-0004)))
(assert-equal '(this is) (p-0103-0004))

;; p.104 (open-input-file "example.lsp" 8) => implementation-defined は省略。
;; p.104 with-open-output-file / with-open-input-file の例をそのまま転記
;; (ファイル名は /9p/tmp/ 以下に変更)
(defun p-0104-0001 ()
  (with-open-output-file (outstream "/9p/tmp/isiki-test-example.dat")
    (format outstream "hello")))
(assert-equal t (%%za-compiled-p (function p-0104-0001)))
(assert-equal nil (p-0104-0001))

(defun p-0104-0002 ()
  (with-open-input-file (instream "/9p/tmp/isiki-test-example.dat")
    (read instream)))
(assert-equal t (%%za-compiled-p (function p-0104-0002)))
(assert-equal 'hello (p-0104-0002))

;; p.105 open-input-file / close の例をそのまま転記(close の戻り値は
;; implementation-defined なので、2回 close してもエラーにならないことだけ確認する)
;; [skip p-0105-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal input-str (open-input-file "/9p/tmp/isiki-test-example.dat"))

(defun p-0105-0002 ()
  (progn (close input-str) t))
(assert-equal t (%%za-compiled-p (function p-0105-0002)))
(assert-equal t (p-0105-0002))

(defun p-0105-0003 ()
  (progn (close input-str) t))
(assert-equal t (%%za-compiled-p (function p-0105-0003)))
(assert-equal t (p-0105-0003))

;; p.105 open-output-file / finish-output の例をそのまま転記
;; [skip p-0105-0004] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal output-str (open-output-file "/9p/tmp/isiki-test-data.lsp"))

(defun p-0105-0005 ()
  (finish-output output-str))
(assert-equal t (%%za-compiled-p (function p-0105-0005)))
(assert-equal nil (p-0105-0005))

(close output-str)

;; p.106 create-string-input-stream の例をそのまま転記
(defun p-0106-0001 ()
  (let ((str (create-string-input-stream "this is a string")))
    (list (read str) (read str) (read str))))
(assert-equal t (%%za-compiled-p (function p-0106-0001)))
(assert-equal '(this is a) (p-0106-0001))

;; p.106 create-string-output-stream / get-output-stream-string の2例をそのまま転記
(defun p-0106-0002 ()
  (let ((str (create-string-output-stream)))
    (format str "hello")
    (format str "world")
    (get-output-stream-string str)))
(assert-equal t (%%za-compiled-p (function p-0106-0002)))
(assert-equal "helloworld" (p-0106-0002))

(defun p-0106-0003 ()
  (let ((out-str (create-string-output-stream)))
    (format out-str "This is a string")
    (let ((part1 (get-output-stream-string out-str)))
      (format out-str "right!")
      (list part1 (get-output-stream-string out-str)))))
(assert-equal t (%%za-compiled-p (function p-0106-0003)))
(assert-equal '("This is a string" "right!") (p-0106-0003))

;;; ===========================================================================
;;; §27 Input and output (p.107-112)
;;; ===========================================================================

;; p.107 read の例をそのまま転記。文字列リテラル中の "#\\A" は §24 の規則
;; (バックスラッシュはバックスラッシュでエスケープする)により文字列としては #\A になる。
;; [skip p-0107-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal str (create-string-input-stream "hello #(1 2 3) 123 #\\A"))

(defun p-0107-0002 ()
  (read str))
(assert-equal t (%%za-compiled-p (function p-0107-0002)))
(assert-equal 'hello (p-0107-0002))

(defun p-0107-0003 ()
  (read str))
(assert-equal t (%%za-compiled-p (function p-0107-0003)))
(assert-equal #(1 2 3) (p-0107-0003))

(defun p-0107-0004 ()
  (read str))
(assert-equal t (%%za-compiled-p (function p-0107-0004)))
(assert-equal 123 (p-0107-0004))

(defun p-0107-0005 ()
  (read str))
(assert-equal t (%%za-compiled-p (function p-0107-0005)))
(assert-equal #\A (p-0107-0005))

(defun p-0107-0006 ()
  (read str nil "the end"))
(assert-equal t (%%za-compiled-p (function p-0107-0006)))
(assert-equal "the end" (p-0107-0006))

;; p.108 read-char の例をそのまま転記(ストリーム終端でエラー)
;; [skip p-0108-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal str (create-string-input-stream "hi"))

(defun p-0108-0002 ()
  (read-char str))
(assert-equal t (%%za-compiled-p (function p-0108-0002)))
(assert-equal #\h (p-0108-0002))

(defun p-0108-0003 ()
  (read-char str))
(assert-equal t (%%za-compiled-p (function p-0108-0003)))
(assert-equal #\i (p-0108-0003))

(defun p-0108-0004 ()
  (read-char str))
(assert-equal t (%%za-compiled-p (function p-0108-0004)))
(assert-error (p-0108-0004))

;; p.108 preview-char の例をそのまま転記
(defun p-0108-0005 ()
  (let ((s (create-string-input-stream "foo")))
    (list (preview-char s) (read-char s) (read-char s))))
(assert-equal t (%%za-compiled-p (function p-0108-0005)))
(assert-equal '(#\f #\f #\o) (p-0108-0005))

;; p.108-109 read-line の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更)
(defun p-0108-0006 ()
  (with-open-output-file (out "/9p/tmp/isiki-test-newfile")
    (format out "This is an example")
    (format out "~%")
    (format out "look at the output file")))
(assert-equal t (%%za-compiled-p (function p-0108-0006)))
(assert-equal nil (p-0108-0006))

;; [skip p-0108-0007] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal str (open-input-file "/9p/tmp/isiki-test-newfile"))

(defun p-0108-0008 ()
  (read-line str))
(assert-equal t (%%za-compiled-p (function p-0108-0008)))
(assert-equal "This is an example" (p-0108-0008))

(defun p-0108-0009 ()
  (read-line str))
(assert-equal t (%%za-compiled-p (function p-0108-0009)))
(assert-equal "look at the output file" (p-0108-0009))

(close str)

;; p.109 stream-ready-p の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更)
(defun p-0109-0001 ()
  (with-open-output-file (out "/9p/tmp/isiki-test-testfile.dat")
    (format out "This is an example")))
(assert-equal t (%%za-compiled-p (function p-0109-0001)))
(assert-equal nil (p-0109-0001))

(defun p-0109-0002 ()
  (with-open-input-file (in "/9p/tmp/isiki-test-testfile.dat")
    (stream-ready-p in)))
(assert-equal t (%%za-compiled-p (function p-0109-0002)))
(assert-equal t (p-0109-0002))

;; p.111 format の例をそのまま転記。output-stream は文字列出力ストリームに束縛し、
;; 戻り値(nil)と出力を assert-output で検証する。
(defun p-0111-0001 ()
  (format (standard-output) "No result"))
(assert-equal t (%%za-compiled-p (function p-0111-0001)))
(assert-output (r o) (p-0111-0001)
  (assert-equal nil r)
  (assert-equal "No result" o))

(defun p-0111-0002 ()
  (format (standard-output) "The result is ~A and nothing else." "meningitis"))
(assert-equal t (%%za-compiled-p (function p-0111-0002)))
(assert-output (r o) (p-0111-0002)
  (assert-equal nil r)
  (assert-equal "The result is meningitis and nothing else." o))

(defun p-0111-0003 ()
  (format (standard-output) "The result i~C" #\s))
(assert-equal t (%%za-compiled-p (function p-0111-0003)))
(assert-output (r o) (p-0111-0003)
  (assert-equal nil r)
  (assert-equal "The result is" o))

(defun p-0111-0004 ()
  (format (standard-output) "The results are ~S and ~S." 1 #\a))
(assert-equal t (%%za-compiled-p (function p-0111-0004)))
(assert-output (r o) (p-0111-0004)
  (assert-equal nil r)
  (assert-equal "The results are 1 and #\\a." o))

(defun p-0111-0005 ()
  (format (standard-output) "Binary code ~B" 150))
(assert-equal t (%%za-compiled-p (function p-0111-0005)))
(assert-output (r o) (p-0111-0005)
  (assert-equal nil r)
  (assert-equal "Binary code 10010110" o))

(defun p-0111-0006 ()
  (format (standard-output) "permission ~O" 493))
(assert-equal t (%%za-compiled-p (function p-0111-0006)))
(assert-output (r o) (p-0111-0006)
  (assert-equal nil r)
  (assert-equal "permission 755" o))

(defun p-0111-0007 ()
  (format (standard-output) "You ~X ~X" 2989 64206))
(assert-equal t (%%za-compiled-p (function p-0111-0007)))
(assert-output (r o) (p-0111-0007)
  (assert-equal nil r)
  (assert-equal "You BAD FACE" o))

(defun p-0111-0008 ()
  (progn
    (format (standard-output) "~&Name ~10Tincome ~20Ttax~%")
    (format (standard-output) "~A ~10T~D ~20T~D" "Grummy" 23000 7500)))
(assert-equal t (%%za-compiled-p (function p-0111-0008)))
(assert-output (r o) (p-0111-0008)
  (assert-equal nil r)
  (assert-equal "Name      income    tax
Grummy    23000     7500" o))

(defun p-0111-0009 ()
  (format (standard-output) "This will be split into~%two lines."))
(assert-equal t (%%za-compiled-p (function p-0111-0009)))
(assert-output (r o) (p-0111-0009)
  (assert-equal nil r)
  (assert-equal "This will be split into
two lines." o))

(defun p-0111-0010 ()
  (format (standard-output) "This is a tilde: ~~"))
(assert-equal t (%%za-compiled-p (function p-0111-0010)))
(assert-output (r o) (p-0111-0010)
  (assert-equal nil r)
  (assert-equal "This is a tilde: ~" o))

;; p.112 read-byte の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更。
;; 8ビットのバイトコードが格納される前提なので、hello の各文字の ASCII コードになる)
;; [skip p-0112-0001] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal byte-example (open-output-stream "/9p/tmp/isiki-test-byte-ex"))

(defun p-0112-0002 ()
  (format byte-example "hello"))
(assert-equal t (%%za-compiled-p (function p-0112-0002)))
(assert-equal nil (p-0112-0002))

(close byte-example)

(setq byte-example (open-input-stream "/9p/tmp/isiki-test-byte-ex" 8))

(defun p-0112-0003 ()
  (read-byte byte-example))
(assert-equal t (%%za-compiled-p (function p-0112-0003)))
(assert-equal 104 (p-0112-0003))

(defun p-0112-0004 ()
  (read-byte byte-example))
(assert-equal t (%%za-compiled-p (function p-0112-0004)))
(assert-equal 101 (p-0112-0004))

(defun p-0112-0005 ()
  (read-byte byte-example))
(assert-equal t (%%za-compiled-p (function p-0112-0005)))
(assert-equal 108 (p-0112-0005))

(defun p-0112-0006 ()
  (read-byte byte-example))
(assert-equal t (%%za-compiled-p (function p-0112-0006)))
(assert-equal 108 (p-0112-0006))

(defun p-0112-0007 ()
  (read-byte byte-example))
(assert-equal t (%%za-compiled-p (function p-0112-0007)))
(assert-equal 111 (p-0112-0007))

(close byte-example)

;; p.112 write-byte の例をそのまま転記(close の戻り値は implementation-defined)。
;; 書いたバイトを read-byte で読み戻して確認する。
(let ((out-str (open-output-stream "/9p/tmp/isiki-test-byte-example" 8)))
  (write-byte #b101 out-str)
  (close out-str))

(defun p-0112-0008 ()
  (let ((in-str (open-input-stream "/9p/tmp/isiki-test-byte-example" 8)))
    (let ((v (read-byte in-str)))
      (close in-str)
      v)))
(assert-equal t (%%za-compiled-p (function p-0112-0008)))
(assert-equal 5 (p-0112-0008))

;;; ===========================================================================
;;; §28 Files (p.113-114)
;;; ===========================================================================

;; p.113 probe-file の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更。テストの
;; 再実行で残っているかもしれないので、事前に別名の存在しないファイルで確認する)
(defun p-0113-0001 ()
  (probe-file "/9p/tmp/isiki-test-notexist-never-created.lsp"))
(assert-equal t (%%za-compiled-p (function p-0113-0001)))
(assert-equal nil (p-0113-0001))

;; [skip p-0113-0002] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal new-file (open-output-file "/9p/tmp/isiki-test-notexist.lsp"))

(close new-file)

(defun p-0113-0003 ()
  (probe-file "/9p/tmp/isiki-test-notexist.lsp"))
(assert-equal t (%%za-compiled-p (function p-0113-0003)))
(assert-equal t (p-0113-0003))

;; p.113 file-position の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更)
;; [skip p-0113-0004] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略(定義自体は後続のテストのために実行する)
(defglobal example (open-output-file "/9p/tmp/isiki-test-example.lsp"))

(defun p-0113-0005 ()
  (format example "hello"))
(assert-equal t (%%za-compiled-p (function p-0113-0005)))
(assert-equal nil (p-0113-0005))

(close example)

(setq example (open-input-stream "/9p/tmp/isiki-test-example.lsp" 8))

(defun p-0113-0006 ()
  (file-position example))
(assert-equal t (%%za-compiled-p (function p-0113-0006)))
(assert-equal 0 (p-0113-0006))

(defun p-0113-0007 ()
  (read-byte example))
(assert-equal t (%%za-compiled-p (function p-0113-0007)))
(assert-equal 104 (p-0113-0007))

(defun p-0113-0008 ()
  (file-position example))
(assert-equal t (%%za-compiled-p (function p-0113-0008)))
(assert-equal 1 (p-0113-0008))

;; p.114 (set-file-position example 4) => 4 をそのまま転記(続けて位置4の 'o' が読めること)
(defun p-0114-0001 ()
  (set-file-position example 4))
(assert-equal t (%%za-compiled-p (function p-0114-0001)))
(assert-equal 4 (p-0114-0001))

(defun p-0114-0002 ()
  (read-byte example))
(assert-equal t (%%za-compiled-p (function p-0114-0002)))
(assert-equal 111 (p-0114-0002))

(close example)

;; p.114 file-length の例。file27.dat は仕様に中身が無いので25バイトのファイルを作って
;; 確認する(バイトサイズ2の例は「Implementations are not required to support」のため省略)
(with-open-output-file (out "/9p/tmp/isiki-test-file27.dat")
  (format out "0123456789012345678901234"))

(defun p-0114-0003 ()
  (file-length "/9p/tmp/isiki-test-file27.dat" 8))
(assert-equal t (%%za-compiled-p (function p-0114-0003)))
(assert-equal 25 (p-0114-0003))

;;; ===========================================================================
;;; §29 Condition system (p.115-122)
;;; ===========================================================================

;; p.117 signal-condition の例をそのまま転記。ハンドラで捕捉して、<simple-error> の
;; インスタンスが format-string / format-arguments を持って届くことを確認する。
(defun p-0117-0001 ()
  (block isiki-test-p117
    (with-handler
        (lambda (c)
          (return-from isiki-test-p117
            (list (if (typep c '<simple-error>) '<simple-error> 'other)
                  (simple-error-format-string c)
                  (simple-error-format-arguments c))))
      (signal-condition (create (class <simple-error>)
                                'format-string "A ~A problem occurred."
                                'format-arguments '(bad))
                        nil))))
(assert-equal t (%%za-compiled-p (function p-0117-0001)))
(assert-equal '(<simple-error> "A ~A problem occurred." (bad)) (p-0117-0001))

;; p.115-117 (§29 Conditions) には上記以外の "Example:" ブロックは存在しない。
;; 以下は §29.2 の説明に基づき独自に作成した例。

;; cf. p.116-117 with-handler/error の説明に基づく独自の例
(defun p-0116-0001 ()
  (block isiki-test-b1
    (with-handler (lambda (c) (return-from isiki-test-b1 'caught))
      (error "boom"))))
(assert-equal t (%%za-compiled-p (function p-0116-0001)))
(assert-equal 'caught (p-0116-0001))

;; p.117 (ignore-errors form*) の説明に基づく独自の例
(defun p-0117-0002 ()
  (ignore-errors (error "boom")))
(assert-equal t (%%za-compiled-p (function p-0117-0002)))
(assert-equal nil (p-0117-0002))

(defun p-0117-0003 ()
  (ignore-errors 5))
(assert-equal t (%%za-compiled-p (function p-0117-0003)))
(assert-equal 5 (p-0117-0003))

;; cf. p.115 「ハンドラは受け取ったconditionに対しsignal-conditionを呼ぶことで
;; 外側のハンドラに委譲できる」という説明に基づく独自の例
(defun p-0115-0001 ()
  (block isiki-test-b2
    (with-handler (lambda (c) (return-from isiki-test-b2 'outer))
      (with-handler (lambda (c) (if (typep c '<simple-error>) (signal-condition c nil) (return-from isiki-test-b2 'inner)))
        (error "boom")))))
(assert-equal t (%%za-compiled-p (function p-0115-0001)))
(assert-equal 'outer (p-0115-0001))

;; cf. p.116 signal-condition の continuable 引数の説明に基づく独自の例
(defun p-0116-0002 ()
  (+ 1
     (with-handler (lambda (c) 100)
       (signal-condition (make-instance '<condition>) t))))
(assert-equal t (%%za-compiled-p (function p-0116-0002)))
(assert-equal 101 (p-0116-0002))

;;; ===========================================================================
;;; §30 Miscellaneous (p.122)
;;; ===========================================================================

;; p.122 (identity '(a b c)) => (a b c) をそのまま転記
(defun p-0122-0001 ()
  (identity '(a b c)))
(assert-equal t (%%za-compiled-p (function p-0122-0001)))
(assert-equal '(a b c) (p-0122-0001))

;; p.122 (get-universal-time) => 2901312000 の例。値は実行時刻に依存するので、
;; 1900年起点の秒数として正の整数が返ることを確認する。
(defun p-0122-0002 ()
  (integerp (get-universal-time)))
(assert-equal t (%%za-compiled-p (function p-0122-0002)))
(assert-equal t (p-0122-0002))

(defun p-0122-0003 ()
  (> (get-universal-time) 0))
(assert-equal t (%%za-compiled-p (function p-0122-0003)))
(assert-equal t (p-0122-0003))

;;; ===========================================================================
