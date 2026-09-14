;; test/lisp/isiki_test.lisp
;;
;; isiki-os を QEMU 上で起動し、boot-entryスクリプト(test/lisp/qemu_boot_test.lisp)から
;; load して実行するテストファイル。
;;
;; [ISLisp Working Draft 23.0](http://islisp.org/docs/islisp-v23.pdf) に "Example:" として
;; 記載されている例を **すべて** テストにすることを目的とする。各テストの直前の
;; ;; コメントに、仕様上で対応する "Example:" が記載されているページ番号を記す。
;; 変数名や具体的な数値を(このカーネルの識別子衝突を避ける目的、あるいは単に別の値でも
;; 成立することを示す目的で)変更している場合は「cf.」を付け、仕様の例をそのまま転記した
;; 場合は「p.N」のみを付けている。
;;
;; 方針:
;;   - 仕様の例は、未実装の機能を使うものであっても **省略せず** 転記する。失敗したものは
;;     未実装またはバグとして扱い、カーネル側を直す(テストを弱めて通すことはしない)。
;;   - 例外は次の2種類だけ:
;;       (a) 仕様自身が (implementation-defined) / undefined と明記している例
;;       (b) #2A((a b c) (d e f)) のような **リーダー構文** に依存する例。リーダーが構文を
;;           知らないと load 自体が中断して後続のテストが全部消えるため、
;;           test/lisp/isiki_test_syntax.lisp に分離している(内容は同じ方針で全転記)
;;   - "an error shall be signaled" の例は assert-error(test_framework.lisp)で
;;     「conditionがsignalされること」を検証する
;;   - 無理数・循環小数を結果に持つ浮動小数点演算(sqrt/log/exp/三角関数等)は、仕様の
;;     記載値とビット単位で一致するとは限らないため、assert-equalではなく許容誤差付きの
;;     assert-float-close で比較する。2.0 や 0.25 のように2進で正確に表現できる値は
;;     assert-equal で(型を含めて)比較する
;;   - 仕様の例が同じ識別子(foo/bar/x等)を別の定義で使い回している箇所は、例同士が
;;     干渉しないよう isiki-test- 接頭辞で区別する(cf.)。
;;   - 実ファイルを使う例は /9p/tmp/ 以下(ホストの tmp/、.gitignore 済み)に書く
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal / assert-float-close /
;; assert-error / assert-output をそのまま使う(boot-entryスクリプトが本ファイルより先に
;; それをloadしている前提)。


;;; ===========================================================================
;;; §1-§12 導入・定義・評価 (p.7-28)
;;; ===========================================================================

;; p.7 (+ 3 4) => 7 (メタ記号 ⇒ の説明に使われている例)
(assert-equal 7 (+ 3 4))


;; p.19 (defun copy-cell (x) (cons (car x) (cdr x))) の例。defunの戻り値が関数名
;; シンボルであること(p.28の一般規則)の確認と、定義した関数の動作確認を追加。
(defglobal isiki-test-defun-copy-cell-result (defun copy-cell (x) (cons (car x) (cdr x))))
(assert-equal 'copy-cell isiki-test-defun-copy-cell-result)
(assert-equal '(1 . 2) (copy-cell '(1 . 2)))


;; p.22 ((lambda (x) (+ x x)) 4) => 8 をそのまま転記
(assert-equal 8 ((lambda (x) (+ x x)) 4))


;; p.23 (functionp (function car)) => t
(assert-equal t (functionp (function car)))
;; cf. p.23 functionp の説明に基づく非関数の例(仕様は #\a 等、ここでは整数)。
(assert-equal nil (functionp 1))
;; p.23 (funcall (function -) 3) => -3 をそのまま転記
(assert-equal -3 (funcall (function -) 3))
;; p.23 (apply #'- '(4 3)) => 1 をそのまま転記
(assert-equal 1 (apply #'- '(4 3)))

;; p.24 lambda の4例。このカーネルは可変長引数のマーカーとして &rest のみを受け付け
;; :rest は対応しない(方針として対応不要)ため、3例目の :rest 版は &rest に置き換える(cf.)。
(assert-equal 25 ((lambda (x y) (+ (* x x) (* y y))) 3 4))
(assert-equal '(5 6) ((lambda (x y &rest z) z) 3 4 5 6))
(assert-equal '(5 6) ((lambda (x y &rest z) z) 3 4 5 6))
(assert-equal -18 (funcall (lambda (x y) (- y (* x y))) 7 3))

;; p.25 labels の例をそのまま転記
(assert-equal t
  (labels ((evenp (n)
             (if (= n 0)
                 t
                 (oddp (- n 1))))
           (oddp (n)
             (if (= n 0)
                 nil
                 (evenp (- n 1)))))
    (evenp 88)))

;; p.25 flet の例をそのまま転記(内側のfの本体中のfは外側のfを指す)
(assert-equal 17
  (flet ((f (x) (+ x 3)))
    (flet ((f (x) (+ x (f x))))
      (f 7))))

;; p.25 (apply (if (< 1 2) (function max) (function min)) 1 2 (list 3 4)) => 4 をそのまま転記
(assert-equal 4
  (apply (if (< 1 2) (function max) (function min))
         1 2 (list 3 4)))
;; cf. p.25 compose の例。:rest は &rest に置き換え、sqrt の結果はfloatなので許容誤差付きの比較にする。
(assert-equal 'compose
  (defun compose (f g)
    (lambda (&rest args)
      (funcall f (apply g args)))))
(assert-float-close 30.0 (funcall (compose (function sqrt) (function *)) 12 75))

;; p.25 (let ((x '(1 2 3))) (funcall (cond ((listp x) (function car)) (t (lambda (x) (cons x 1)))) x)) => 1
(assert-equal 1
  (let ((x '(1 2 3)))
    (funcall (cond ((listp x) (function car))
                   (t (lambda (x) (cons x 1))))
             x)))


;; p.26 defconstant の例をそのまま転記
(assert-equal 'e (defconstant e 2.7182818284590451))
(assert-equal 2.7182818284590451 e)
(assert-equal 'f (defun f () e))
(assert-equal 2.7182818284590451 (f))


;; p.27 defglobal の例をそのまま転記
(assert-equal 'today (defglobal today 'wednesday))
(assert-equal 'wednesday today)
(assert-equal 'what-is-today (defun what-is-today () today))
(assert-equal 'wednesday (what-is-today))
(assert-equal 'wednesday (let ((what-is-today 'thursday)) (what-is-today)))
(assert-equal 'wednesday (let ((today 'thursday)) (what-is-today)))


;; p.27 defdynamic の例をそのまま転記
(assert-equal '*color* (defdynamic *color* 'red))
(assert-equal 'red (dynamic *color*))
(assert-equal 'what-color (defun what-color () (dynamic *color*)))
(assert-equal 'red (what-color))
(assert-equal 'green (dynamic-let ((*color* 'green)) (what-color)))
;; dynamic-let を抜けたら元の値に戻る(§10.3 の説明)
(assert-equal 'red (what-color))


;; p.28 (defun caar (x) (car (car x))) => caar をそのまま転記
(defglobal isiki-test-defun-caar-result (defun caar (x) (car (car x))))
(assert-equal 'caar isiki-test-defun-caar-result)
(assert-equal 1 (caar '((1 2) 3)))


;;; ===========================================================================
;;; §13 Predicates (p.29-32)
;;; ===========================================================================

;; p.29-30 eq/eql の等価性表。(implementation-defined)と明記されている行を除いた
;; 全行をそのまま転記。
(assert-equal t (eql () ()))
(assert-equal t (eq () ()))
(assert-equal t (eql '() '()))
(assert-equal t (eq '() '()))
(assert-equal t (eql 'a 'a))
(assert-equal t (eq 'a 'a))
(assert-equal t (eql 'a 'A))
(assert-equal t (eq 'a 'A))
(assert-equal nil (eql 'a 'b))
(assert-equal nil (eq 'a 'b))
(assert-equal nil (eql 'f 'nil))
(assert-equal nil (eq 'f 'nil))
(assert-equal t (eql 2 2))
(assert-equal nil (eql 2 2.0))
(assert-equal nil (eq 2 2.0))
(assert-equal t (eql 100000000 100000000))
(assert-equal t (eql 10.00000 10.0))
(assert-equal nil (eql (cons 1 2) (cons 1 2)))
(assert-equal nil (eq (cons 1 2) (cons 1 2)))
(assert-equal t (let ((x '(a))) (eql x x)))
(assert-equal t (let ((x '(a))) (eq x x)))
(assert-equal t (let ((p (lambda (x) x))) (eql p p)))
(assert-equal t (let ((p (lambda (x) x))) (eq p p)))
(assert-equal t (let ((x "a")) (eql x x)))
(assert-equal t (let ((x "a")) (eq x x)))
(assert-equal t (let ((x "")) (eql x x)))
(assert-equal t (let ((x "")) (eq x x)))
(assert-equal nil (eql #\a #\A))
(assert-equal nil (eq #\a #\A))
(assert-equal t (eql #\a #\a))
(assert-equal t (eql #\space #\Space))
(assert-equal t (eql #\space #\space))


;; p.31 (equal 'a 'a) ... (equal "a" "A") をそのまま転記
(assert-equal t (equal 'a 'a))
(assert-equal t (equal 2 2))
(assert-equal nil (equal 2 2.0))
(assert-equal t (equal '(a) '(a)))
(assert-equal t (equal '(a (b) c) '(a (b) c)))
(assert-equal t (equal (cons 1 2) (cons 1 2)))
(assert-equal t (equal '(a) (list 'a)))
(assert-equal t (equal "abc" "abc"))
(assert-equal t (equal (vector 'a) (vector 'a)))
(assert-equal t (equal #(a b) #(a b)))
(assert-equal nil (equal #(a b) #(a c)))
(assert-equal nil (equal "a" "A"))


;; p.31 (not t)(not '())(not 'nil)(not nil)(not 3)(not (list))(not (list 3)) をそのまま転記
(assert-equal nil (not t))
(assert-equal t (not '()))
(assert-equal t (not 'nil))
(assert-equal t (not nil))
(assert-equal nil (not 3))
(assert-equal t (not (list)))
(assert-equal nil (not (list 3)))


;; p.32 and の例をそのまま転記
(assert-equal t (and (= 2 2) (> 2 1)))
(assert-equal nil (and (= 2 2) (< 2 1)))
(assert-equal t (and (eql 'a 'a) (not (> 1 2))))
(assert-equal 'b (let ((x 'a)) (and x (setq x 'b))))
(assert-equal nil (let ((x nil)) (and x (setq x 'b))))
(assert-equal 10
  (let ((time 10))
    (if (and (< time 24) (> time 12))
        (- time 12) time)))
(assert-equal 6
  (let ((time 18))
    (if (and (< time 24) (> time 12))
        (- time 12) time)))
;; cf. p.32 and の定義(≡ 't/form/...)に基づく追加例(引数なし)。
(assert-equal t (and))


;; p.32 or の例をそのまま転記
(assert-equal t (or (= 2 2) (> 2 1)))
(assert-equal t (or (= 2 2) (< 2 1)))
(assert-equal 'a (let ((x 'a)) (or x (setq x 'b))))
(assert-equal 'b (let ((x nil)) (or x (setq x 'b))))
;; cf. p.32 or の定義(≡ 'nil/form/...)に基づく追加例(引数なし)。
(assert-equal nil (or))


;;; ===========================================================================
;;; §14 Control structure (p.33-47)
;;; ===========================================================================

;; p.33 リテラル定数の例。#2A((a b c) (d e f)) はリーダー構文依存のため
;; isiki_test_syntax.lisp 側で転記し、残りをここで転記する。
(assert-equal #\a #\a)
(assert-equal 145932 145932)
(assert-equal "abc" "abc")
(assert-equal #(a b c) #(a b c))


;; p.33 quote の例をそのまま転記。
(assert-equal 'a (quote a))
(assert-equal #(a b c) (quote #(a b c)))
(assert-equal '(+ 1 2) (quote (+ 1 2)))
(assert-equal nil (quote ()))
(assert-equal 'a 'a)
(assert-equal #(a b c) '#(a b c))
(assert-equal '(car l) '(car l))
(assert-equal '(+ 1 2) '(+ 1 2))
(assert-equal '(quote a) '(quote a))
(assert-equal '(quote a) ''a)
(assert-equal 'quote (car ''a))


;; p.34 (defglobal x 0) => x / x => 0 / (let ((x 1)) x) => 1 / x => 0 をそのまま転記
(assert-equal 'x (defglobal x 0))
(assert-equal 0 x)
(assert-equal 1 (let ((x 1)) x))
(assert-equal 0 x)

;; p.34 setq の例をそのまま転記。(defglobal x 2) はp.34の (defglobal x 0) の再定義
(assert-equal 'x (defglobal x 2))
(assert-equal 3 (+ x 1))
(assert-equal 4 (setq x 4))
(assert-equal 5 (+ x 1))
(assert-equal 2 (let ((x 1)) (setq x 2) x))
(assert-equal 5 (+ x 1))


;; p.35 setf の例をそのまま転記。x は p.34 で定義したグローバル変数だが、仕様の
;; 「In the cons x」に合わせてここでは cons を束縛し直す。2つ目は setf が
;; ユーザー定義マクロで書かれた place を展開してから処理する例。
(setq x (cons 1 2))
(assert-equal 2 (setf (car x) 2))
(assert-equal 2 (car x))
(setq x (cons 1 2))
(assert-equal 'first (defmacro first (spot) `(car ,spot)))
(assert-equal 2 (setf (first x) 2))
(assert-equal 2 (car x))


;; p.36 let の3例をそのまま転記
(assert-equal 6
  (let ((x 2) (y 3))
    (* x y)))
(assert-equal 35
  (let ((x 2) (y 3))
    (let ((x 7)
          (z (+ x y)))
      (* z x))))
(assert-equal '(2 1)
  (let ((x 1) (y 2))
    (let ((x y) (y x))
      (list x y))))


;; p.37 let* の2例をそのまま転記
(assert-equal 70
  (let ((x 2) (y 3))
    (let* ((x 7)
           (z (+ x y)))
      (* z x))))
(assert-equal '(2 2)
  (let ((x 1) (y 2))
    (let* ((x y) (y x))
      (list x y))))


;; p.38 dynamic-let の例をそのまま転記(y は defdynamic されていない動的変数)
(assert-equal 'foo
  (defun foo (x)
    (dynamic-let ((y x))
      (bar 1))))
(assert-equal 'bar
  (defun bar (x)
    (+ x (dynamic y))))
(assert-equal 3 (foo 2))


;; p.38 (if (> 3 2) 'yes 'no) => yes / (if (> 2 3) 'yes 'no) => no /
;; (if (> 2 3) 'yes) => nil をそのまま転記
(assert-equal 'yes (if (> 3 2) 'yes 'no))
(assert-equal 'no (if (> 2 3) 'yes 'no))
(assert-equal nil (if (> 2 3) 'yes))

;; p.39 (if (> 3 2) (- 3 2) (+ 3 2)) => 1 /
;; (let ((x 7)) (if (< x 0) x (- x))) => -7 をそのまま転記
(assert-equal 1 (if (> 3 2) (- 3 2) (+ 3 2)))
(assert-equal -7 (let ((x 7)) (if (< x 0) x (- x))))

;; p.39 cond の3例をそのまま転記
(assert-equal 'greater (cond ((> 3 2) 'greater) ((< 3 2) 'less)))
(assert-equal nil (cond ((> 3 3) 'greater) ((< 3 3) 'less)))
(assert-equal 'equal (cond ((> 3 3) 'greater) ((< 3 3) 'less) (t 'equal)))


;; p.40 case/case-using の6例をそのまま転記
(assert-equal 'composite (case (* 2 3) ((2 3 5 7) 'prime) ((4 6 8 9) 'composite)))
(assert-equal nil (case (car '(c d)) ((a) 'a) ((b) 'b)))
(assert-equal 'consonant (case (car '(c d)) ((a e i o u) 'vowel) ((y) 'semivowel) (t 'consonant)))
(assert-equal 'vowels (let ((char #\u)) (case char ((#\a #\e #\o #\u #\i) 'vowels) (t 'consonants))))
(assert-equal 'two (case-using #'= (+ 1.0 1.0) ((1) 'one) ((2) 'two) (t 'more)))
(assert-equal 2 (case-using #'string= "bar" (("foo") 1) (("bar") 2)))


;; p.41 progn の例をそのまま転記。(defglobal x 0) は p.34 の x の再定義。
(assert-equal 'x (defglobal x 0))
(assert-equal 6 (progn (setq x 5) (+ x 1)))
;; p.41 (progn (format (standard-output) "4 plus 1 equals ") (format (standard-output) "~D" (+ 4 1)))
;; => nil, prints "4 plus 1 equals 5" をそのまま転記。standard-output を文字列ストリームに
;; 束縛して印字結果も検証する。
(assert-output (isiki-test-progn-result isiki-test-progn-output)
  (progn
    (format (standard-output) "4 plus 1 equals ")
    (format (standard-output) "~D" (+ 4 1)))
  (assert-equal nil isiki-test-progn-result)
  (assert-equal "4 plus 1 equals 5" isiki-test-progn-output))


;; p.41 while の例をそのまま転記
(assert-equal '(1 2 3 4 5) (let ((x '()) (i 5)) (while (> i 0) (setq x (cons i x)) (setq i (- i 1))) x))


;; p.42 for の2例をそのまま転記
(assert-equal #(0 1 2 3 4)
  (for ((vec (vector 0 0 0 0 0)) (i 0 (+ i 1))) ((= i 5) vec) (setf (elt vec i) i)))
(assert-equal 25
  (let ((x '(1 3 5 7 9))) (for ((x x (cdr x)) (sum 0 (+ sum (car x)))) ((null x) sum))))


;; p.43 (block x (+ 10 (return-from x 6) 22)) => 6 をそのまま転記
(assert-equal 6 (block x (+ 10 (return-from x 6) 22)))

;; p.43 f1/f2 (blockをクロージャ越しにreturn-fromする例)をそのまま転記。
(assert-equal 'f1
  (defun f1 ()
    (block b
      (let ((f (lambda () (return-from b 'exit))))
        (f2 f)))))
(assert-equal 'f2
  (defun f2 (g) (funcall g)))
(assert-equal 'exit (f1))

;; p.43 (block sum-block (for ...)) => 0 をそのまま転記
(assert-equal 0
  (block sum-block
    (for ((x '(1 a 2 3) (cdr x))
          (sum 0 (+ sum (car x))))
        ((null x) sum)
      (cond ((not (numberp (car x))) (return-from sum-block 0))))))

;; cf. p.43-44 bar (blockの動的extentを抜けた後のreturn-fromがエラーになる例)。
;; p.38 で bar を別の定義で使っているため isiki-test-bl-bar に変更。
(assert-equal 'isiki-test-bl-bar
  (defun isiki-test-bl-bar (x y)
    (let ((foo #'car))
      (let ((result
              (block bl
                (setq foo (lambda () (return-from bl 'first-exit)))
                (if x (return-from bl 'second-exit) 'third-exit))))
        (if y (funcall foo) nil)
        result))))
(assert-equal 'second-exit (isiki-test-bl-bar t nil))
(assert-equal 'third-exit (isiki-test-bl-bar nil nil))
(assert-error (isiki-test-bl-bar nil t))
(assert-error (isiki-test-bl-bar t t))

;; cf. p.45 catch/throw の例。p.38 の foo/bar と区別するため isiki-test-catch-foo/-bar に変更。
(assert-equal 'isiki-test-catch-foo
  (defun isiki-test-catch-foo (x) (catch 'block-sum (isiki-test-catch-bar x))))
(assert-equal 'isiki-test-catch-bar
  (defun isiki-test-catch-bar (x)
    (for ((l x (cdr l)) (sum 0 (+ sum (car l))))
        ((null l) sum)
      (cond ((not (numberp (car l))) (throw 'block-sum 0))))))
(assert-equal 10 (isiki-test-catch-foo '(1 2 3 4)))
(assert-equal 0 (isiki-test-catch-foo '(1 2 a 4)))

;; cf. p.46 tagbody/go の with-retry マクロの例。:rest は &rest に置き換える。仕様の使用例は
;; 「ISLISPには実在しない仮想の関数」if-error を使うため、その部分だけ
;; リトライ回数を数える自己完結した本体に置き換える。
(assert-equal 'with-retry
  (defmacro with-retry (&rest forms)
    (let ((tag (gensym)))
      `(block ,tag
         (tagbody
           ,tag
           (return-from ,tag
             (flet ((retry () (go ,tag)))
               ,@forms)))))))
(assert-equal 3
  (let ((isiki-test-retry-count 0))
    (with-retry
      (setq isiki-test-retry-count (+ isiki-test-retry-count 1))
      (if (< isiki-test-retry-count 3) (retry) isiki-test-retry-count))))

;; cf. p.46-47 unwind-protect の例(1つ目: catch/throw と property の後始末)。
;; foo/bar は isiki-test-up-foo/-bar に変更。
(assert-equal 'isiki-test-up-foo
  (defun isiki-test-up-foo (x)
    (catch 'duplicates
      (unwind-protect (isiki-test-up-bar x)
        (for ((l x (cdr l)))
            ((null l) 'unused)
          (remove-property (car l) 'label))))))
(assert-equal 'isiki-test-up-bar
  (defun isiki-test-up-bar (l)
    (cond ((and (symbolp l) (property l 'label))
           (throw 'duplicates 'found))
          ((symbolp l) (setf (property l 'label) t))
          ((isiki-test-up-bar (car l)) (isiki-test-up-bar (cdr l)))
          (t nil))))
(assert-equal t (isiki-test-up-foo '(a b c)))
(assert-equal nil (property 'a 'label))
(assert-equal 'found (isiki-test-up-foo '(a b a c)))
(assert-equal nil (property 'a 'label))
;; この例の bar は (symbolp nil) が t のため末尾の nil にも label を付け、cleanup は
;; リストの要素(a b c)しか外さないので nil の label が残る。同じ例を続けて実行する
;; (isiki_test_jit.lisp)と 2 回目の '(a b c) で found になってしまうため、ここで外す
(remove-property nil 'label)

;; cf. p.47 unwind-protect の例(2つ目: cleanup中に別のblockへreturn-fromするとエラー)。
;; test/test2/test3/test4 は isiki-test-up-test/-test2/-test3/-test4 に変更。
(assert-equal 'isiki-test-up-test
  (defun isiki-test-up-test ()
    (catch 'outer (isiki-test-up-test2))))
(assert-equal 'isiki-test-up-test2
  (defun isiki-test-up-test2 ()
    (block inner
      (isiki-test-up-test3 (lambda ()
                             (return-from inner 7))))))
(assert-equal 'isiki-test-up-test3
  (defun isiki-test-up-test3 (fun)
    (unwind-protect (isiki-test-up-test4) (funcall fun))))
(assert-equal 'isiki-test-up-test4
  (defun isiki-test-up-test4 ()
    (throw 'outer 6)))
(assert-error (isiki-test-up-test))


;;; ===========================================================================
;;; §15 Objects (p.48-61)
;;; ===========================================================================

;; p.48-60 (§15 Classes) には defclass/defgeneric/defmethod/next-method-p/
;; call-next-method の構文・意味の説明はあるが、具体的な "Example:" ブロックは存在しない。
;; 以下は §15 の説明に基づき独自に作成したクラス例。

(defclass isiki-test-point () ((x :initarg :x :initform 0) (y :initarg :y :initform 0)))
(defclass isiki-test-point3d (isiki-test-point) ((z :initarg :z :initform 0)))

;; cf. p.48-50 (defclass ... :initarg ... :initform ...) の説明に基づく独自の例
(assert-equal 1 (slot-value (make-instance 'isiki-test-point ':x 1 ':y 2) 'x))
(assert-equal 0 (slot-value (make-instance 'isiki-test-point) 'x))
(assert-equal 3 (slot-value (make-instance 'isiki-test-point3d ':x 1 ':y 2 ':z 3) 'z))

;; cf. p.53-59 defgeneric/defmethod/next-method-p/call-next-method の説明に基づく独自の例
(defgeneric isiki-test-describe (obj))
(defmethod isiki-test-describe (obj) (list 'point-desc (next-method-p)))
(defmethod isiki-test-describe ((obj isiki-test-point3d))
  (list '3d-desc (next-method-p) (call-next-method)))

(assert-equal '(point-desc nil) (isiki-test-describe (make-instance 'isiki-test-point)))
(assert-equal '(3d-desc t (point-desc nil)) (isiki-test-describe (make-instance 'isiki-test-point3d)))

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
(assert-equal nil (ignore-errors (isiki-test-no-next-method 42)))

;; cf. p.61 typep/subclassp/class-of の説明に基づく独自の例
(assert-equal t (typep (make-instance 'isiki-test-point3d) 'isiki-test-point))
(assert-equal nil (typep (make-instance 'isiki-test-point) 'isiki-test-point3d))
(assert-equal t (subclassp (class-of (make-instance 'isiki-test-point3d))
                            (class-of (make-instance 'isiki-test-point))))


;;; ===========================================================================
;;; §16 Macros (p.62-63)
;;; ===========================================================================

;; cf. p.62 (defmacro caar (x) (list 'car (list 'car x))) => caar の例。p.28で既に
;; caar という名前の関数を定義済みなので、識別子を isiki-test-caar-macro に変更する。
(assert-equal 'isiki-test-caar-macro
  (defmacro isiki-test-caar-macro (x) (list 'car (list 'car x))))
(assert-equal 1 (isiki-test-caar-macro '((1 2) 3)))

;; p.63 quasiquote の6例をそのまま転記(5・6例目は quasiquote 自体をデータとして
;; ネストする例。期待値もリーダーが読んだ quasiquote/unquote 構造として比較する)
(assert-equal '(list 3 4) `(list ,(+ 1 2) 4))
(assert-equal '(list name a (quote a)) (let ((name 'a)) `(list name ,name ',name)))
(assert-equal '(a 3 x x x b) `(a ,(+ 1 2) ,@(create-list 3 'x) b))
(assert-equal '((foo 7) . cons) `((foo ,(- 10 3)) ,@(cdr '(c)) . ,(car '(cons))))
(assert-equal '(a `(b ,(+ 1 2) ,(foo 4 d) e) f)
  `(a `(b ,(+ 1 2) ,(foo ,(+ 1 3) d) e) f))
(assert-equal '(a `(b ,x ,'y d) e)
  (let ((name1 'x)
        (name2 'y))
    `(a `(b ,,name1 ,',name2 d) e)))


;;; ===========================================================================
;;; §17 Declarations and coercions (p.63-65)
;;; ===========================================================================

;; p.63 (the <integer> 10) => 10 / (the <number> 10) => 10 をそのまま転記。
;; (the <float> 10) は仕様上「the consequences are undefined」と明記されているため省略する。
(assert-equal 10 (the <integer> 10))
(assert-equal 10 (the <number> 10))

;; p.63 assure の3例をそのまま転記
(assert-equal 10 (assure <integer> 10))
(assert-equal 10 (assure <number> 10))
(assert-error (assure <float> 10))

;; p.65 convert の3例をそのまま転記
(assert-equal 3.0 (convert 3 <float>))
(assert-equal #(#\a #\b #\c) (convert "abc" <general-vector>))
(assert-equal '(a b) (convert #(a b) <list>))


;;; ===========================================================================
;;; §18 Symbol class (p.65-68)
;;; ===========================================================================

;; p.65 symbolp の10例をそのまま転記
(assert-equal t (symbolp 'a))
(assert-equal nil (symbolp "a"))
(assert-equal nil (symbolp #\a))
(assert-equal t (symbolp 't))
(assert-equal t (symbolp t))
(assert-equal t (symbolp 'nil))
(assert-equal t (symbolp nil))
(assert-equal t (symbolp '()))
(assert-equal t (symbolp '*pi*))
(assert-equal nil (symbolp *pi*))


;; p.67-68 property / set-property / remove-property の例をそのまま転記。
;; 仕様の記載順(property の例が先)では athena が未設定なので、まず setf 版で設定し、
;; property → set-property → remove-property の順に確認する。
(assert-equal 'athena (setf (property 'zeus 'daughter) 'athena))
(assert-equal 'athena (property 'zeus 'daughter))
(assert-equal 'athena (set-property 'athena 'zeus 'daughter))
(assert-equal 'athena (remove-property 'zeus 'daughter))
;; cf. p.67 property の説明(無ければ obj(既定nil)を返す)に基づく追加例
(assert-equal nil (property 'zeus 'daughter))
(assert-equal 'unknown (property 'zeus 'daughter 'unknown))


;; p.68 gensym の例(defmacro twice)をそのまま転記
(assert-equal 'twice
  (defmacro twice (x)
    (let ((v (gensym)))
      `(let ((,v ,x)) (+ ,v ,v)))))
(assert-equal 10 (twice 5))


;;; ===========================================================================
;;; §19 Number class (p.69-82)
;;; ===========================================================================

;; p.69 numberp の例をそのまま転記
(assert-equal t (numberp 3))
(assert-equal t (numberp -0.3))
(assert-equal nil (numberp '(a b c)))
(assert-equal nil (numberp "17"))

;; p.69 parse-number の例をそのまま転記。123.34 は浮動小数点の字句解析結果が
;; 完全一致するとは限らないため assert-float-close を使う。
(assert-float-close 123.34 (parse-number "123.34"))
(assert-equal 64206 (parse-number "#XFACE"))
(assert-error (parse-number "-37."))
(assert-error (parse-number "-.5"))

;; p.69-70 = / /= の例をそのまま転記
(assert-equal nil (= 3 4))
(assert-equal t (= 3 3.0))
(assert-equal t (= (parse-number "134.54") 134.54))
(assert-equal t (= 0.0 -0.0))
(assert-equal t (/= 3 4))
(assert-equal nil (/= 3 3.0))
(assert-equal nil (/= (parse-number "134.54") 134.54))

;; p.70 >= / <= / > / < の例をそのまま転記
(assert-equal nil (> 2 2))
(assert-equal nil (> 2.0 2))
(assert-equal t (> 2 -10))
(assert-equal t (> 100 3))
(assert-equal nil (< 2 2))
(assert-equal t (< 1 2))
(assert-equal t (>= 2 2))
(assert-equal t (>= 2.0 2))
(assert-equal nil (>= -1 2))
(assert-equal t (<= -1 2))
(assert-equal nil (<= 2 -1))

;; p.71 + / * の例をそのまま転記
(assert-equal 15 (+ 12 3))
(assert-equal 6 (+ 1 2 3))
(assert-equal 15.0 (+ 12 3.0))
(assert-equal 4.0 (+ 4 0.0))
(assert-equal 0 (+))
(assert-equal 36 (* 12 3))
(assert-equal 36.0 (* 12 3.0))
(assert-equal 0.0 (* 4.0 0))
(assert-equal 24 (* 2 3 4))
(assert-equal 1 (*))

;; p.71 - (単項) の例をそのまま転記
(assert-equal -1 (- 1))
(assert-equal 4.0 (- -4.0))
(assert-equal -4.0 (- 4.0))
(assert-equal t (eql (- 0.0) -0.0))
(assert-equal t (eql (- -0.0) 0.0))

;; p.71 - (多項) の例をそのまま転記。(- 2.3 -3.0) => 5.3 は2進で正確でないため許容誤差付き。
(assert-equal -1 (- 1 2))
(assert-equal 49 (- 92 43))
(assert-float-close 5.3 (- 2.3 -3.0))
(assert-equal 0.0 (- 0.0 0.0))
(assert-equal -6 (- 3 4 5))

;; p.72 reciprocal / quotient の例をそのまま転記
(assert-equal 0.5 (reciprocal 2))
(assert-equal 2 (quotient 10 5))
(assert-equal 0.5 (quotient 1 2))
(assert-equal -4.0 (quotient 2 -0.5))
(assert-error (quotient 0 0.0))
(assert-float-close 0.16666666666666666 (quotient 2 3 4))

;; p.72 max / min の例をそのまま転記((max 2 2.0)/(min 2 2.0) は implementation-defined のため省略)
(assert-equal 3 (max -5 3))
(assert-equal 3 (max 2.0 3))
(assert-equal 5 (max 1 5 2 4 3))
(assert-equal 1 (min 3 1))
(assert-equal 1 (min 1 2.0))
(assert-equal 1 (min 1 5 2 4 3))

;; p.73 abs の例をそのまま転記
(assert-equal 3 (abs -3))
(assert-equal 2.0 (abs 2.0))
(assert-equal 0.0 (abs -0.0))

;; p.73 exp の例をそのまま転記((exp 0) は implementation-defined で 1 or 1.0 のため、値だけ確認)
(assert-float-close 2.718281828459045 (exp 1))
(assert-float-close 7.38905609893065 (exp 2))
(assert-float-close 3.4212295362896734 (exp 1.23))
(assert-float-close 1.0 (exp 0))

;; p.73-74 log の例をそのまま転記((log 1) は implementation-defined で 0 or 0.0 のため、値だけ確認)
(assert-float-close 1.0 (log 2.718281828459045))
(assert-float-close 2.302585092994046 (log 10))
(assert-float-close 0.0 (log 1))

;; p.74 expt の例をそのまま転記。(expt x 0) 等の x を含む一般則は具体値(x=5, 5.0)で確認する。
(assert-equal 8 (expt 2 3))
(assert-equal 10000 (expt -100 2))
(assert-equal 0.0625 (expt 4 -2))
(assert-equal 0.25 (expt 0.5 2))
(assert-equal 1 (expt 5 0))
(assert-equal 1.0 (expt 5.0 0))
(assert-equal -4.0 (expt -0.25 -1))
;; (expt 100 0.5) => 10.0 は exp/log 経由の計算では最終ビットが揺れうるので許容誤差付き
(assert-float-close 10.0 (expt 100 0.5))
(assert-float-close 0.001 (expt 100 -1.5))
(assert-equal 1.0 (expt 5.0 0.0))
(assert-error (expt 0.0 0.0))

;; p.74 sqrt の例をそのまま転記
(assert-equal 2 (sqrt 4))
(assert-float-close 1.4142135623730951 (sqrt 2))
(assert-error (sqrt -1))

;; p.74 *pi* => 3.141592653589793 をそのまま転記
(assert-float-close 3.141592653589793 *pi*)

;; p.75 sin/cos/tan の例をそのまま転記(仕様注記の通り精度の揺れは許容する)
(assert-float-close 0.8414709848078965 (sin 1))
(assert-float-close 0.0 (sin 0))
(assert-float-close (parse-number "9.999998333333417E-4") (sin 0.001))
(assert-float-close 0.5403023058681398 (cos 1))
(assert-float-close 1.0 (cos 0))
(assert-float-close 0.9999995000000417 (cos 0.001))
(assert-float-close 1.557407724654902 (tan 1))
(assert-float-close 0.0 (tan 0))
(assert-float-close 0.0010000003333334668 (tan 0.001))

;; p.76 atan2 の例をそのまま転記。asin/acos は仕様通り定義し、atan は組み込み関数と
;; 衝突するため isiki-test-atan に変更(cf.)。
(assert-float-close 0.0 (atan2 0 3.0))
(assert-float-close 0.7853981633974483 (atan2 1 1))
(assert-float-close 1.8622531212727635 (atan2 1.0 -0.3))
(assert-float-close 3.141592653589793 (atan2 0.0 -0.5))
(assert-float-close -2.356194490192345 (atan2 -1 -1))
(assert-float-close -1.2793396 (atan2 -1.0 0.3))
(assert-equal 0.0 (atan2 0.0 0.5))
(assert-equal 'asin (defun asin (x) (atan2 x (sqrt (- 1 (expt x 2))))))
(assert-equal 'acos (defun acos (x) (atan2 (sqrt (- 1 (expt x 2))) x)))
(assert-equal 'isiki-test-atan (defun isiki-test-atan (x) (atan2 x 1)))
;; cf. 上で定義した asin/acos/isiki-test-atan が組み込みの atan と整合することの確認
(assert-float-close (atan 1) (isiki-test-atan 1))
(assert-float-close 0.5235987755982989 (asin 0.5))
(assert-float-close 1.0471975511965979 (acos 0.5))

;; p.77 sinh/cosh/tanh の例をそのまま転記
(assert-float-close 1.1752011936438014 (sinh 1))
(assert-float-close 0.0 (sinh 0))
(assert-float-close 0.001000000166666675 (sinh 0.001))
(assert-float-close 1.5430806348152437 (cosh 1))
(assert-float-close 1.0 (cosh 0))
(assert-float-close 1.0000005000000416 (cosh 0.001))
(assert-float-close 0.7615941559557649 (tanh 1))
(assert-float-close 0.0 (tanh 0))
(assert-float-close (parse-number "9.999996666668002E-4") (tanh 0.001))

;; p.77 atanh の例をそのまま転記。asinh/acosh は仕様通り定義する。
(assert-float-close 0.5493061443340549 (atanh 0.5))
(assert-float-close 0.0 (atanh 0))
(assert-float-close 0.0010000003333335335 (atanh 0.001))
(assert-equal 'asinh (defun asinh (x) (atanh (quotient x (sqrt (+ 1 (expt x 2)))))))
(assert-equal 'acosh (defun acosh (x) (atanh (quotient (sqrt (* (- x 1) (+ x 1))) x))))
;; cf. 上で定義した asinh/acosh の動作確認(sinh/cosh の逆関数になっていること)
(assert-float-close 1.0 (asinh (sinh 1)))
(assert-float-close 1.0 (acosh (cosh 1)))

;; p.78 floatp の例をそのまま転記
(assert-equal nil (floatp "2.4"))
(assert-equal nil (floatp 2))
(assert-equal t (floatp 2.0))

;; p.78 float の例をそのまま転記。bignum の例は絶対誤差では比較できないので比で確認する。
(assert-equal 0.0 (float 0))
(assert-equal 2.0 (float 2))
(assert-equal -2.0 (float -2.0))
(assert-float-close 1.0 (quotient (float 123456789123456789123456789) (parse-number "1.2345678912345679E26")))

;; p.78-79 floor の例をそのまま転記
(assert-equal 3 (floor 3.0))
(assert-equal 3 (floor 3.4))
(assert-equal 3 (floor 3.9))
(assert-equal -4 (floor -3.9))
(assert-equal -4 (floor -3.4))
(assert-equal -3 (floor -3.0))

;; p.79 ceiling の例をそのまま転記
(assert-equal 3 (ceiling 3.0))
(assert-equal 4 (ceiling 3.4))
(assert-equal 4 (ceiling 3.9))
(assert-equal -3 (ceiling -3.9))
(assert-equal -3 (ceiling -3.4))
(assert-equal -3 (ceiling -3.0))

;; p.79 truncate の例をそのまま転記
(assert-equal 3 (truncate 3.0))
(assert-equal 3 (truncate 3.4))
(assert-equal 3 (truncate 3.9))
(assert-equal -3 (truncate -3.4))
(assert-equal -3 (truncate -3.9))
(assert-equal -3 (truncate -3.0))

;; p.79-80 round の例をそのまま転記
(assert-equal 3 (round 3.0))
(assert-equal 3 (round 3.4))
(assert-equal -3 (round -3.4))
(assert-equal 4 (round 3.6))
(assert-equal -4 (round -3.6))
(assert-equal 4 (round 3.5))
(assert-equal -4 (round -3.5))
(assert-equal 2 (round 2.5))
(assert-equal 0 (round -0.5))

;; p.80 integerp の例をそのまま転記
(assert-equal t (integerp 3))
(assert-equal nil (integerp 3.4))
(assert-equal nil (integerp "4"))
(assert-equal nil (integerp '(a b c)))

;; p.81 div / mod の例をそのまま転記
(assert-equal 4 (div 12 3))
(assert-equal 4 (div 14 3))
(assert-equal -4 (div -12 3))
(assert-equal -5 (div -14 3))
(assert-equal -4 (div 12 -3))
(assert-equal -5 (div 14 -3))
(assert-equal 4 (div -12 -3))
(assert-equal 4 (div -14 -3))
(assert-equal 0 (mod 12 3))
(assert-equal 7 (mod 7 247))
(assert-equal 2 (mod 247 7))
(assert-equal 2 (mod 14 3))
(assert-equal 0 (mod -12 3))
(assert-equal 1 (mod -14 3))
(assert-equal 0 (mod 12 -3))
(assert-equal -1 (mod 14 -3))
(assert-equal 0 (mod -12 -3))
(assert-equal -2 (mod -14 -3))

;; p.81-82 gcd / lcm の例をそのまま転記
(assert-equal 1 (gcd 12 5))
(assert-equal 3 (gcd 15 24))
(assert-equal 3 (gcd -15 24))
(assert-equal 3 (gcd 15 -24))
(assert-equal 3 (gcd -15 -24))
(assert-equal 4 (gcd 0 -4))
(assert-equal 0 (gcd 0 0))
(assert-equal 6 (lcm 2 3))
(assert-equal 120 (lcm 15 24))
(assert-equal 120 (lcm 15 -24))
(assert-equal 120 (lcm -15 24))
(assert-equal 120 (lcm -15 -24))
(assert-equal 0 (lcm 0 -4))
(assert-equal 0 (lcm 0 0))

;; p.82 isqrt の例をそのまま転記
(assert-equal 7 (isqrt 49))
(assert-equal 7 (isqrt 63))
(assert-equal 1000000000000000 (isqrt 1000000000000002000000000000000))


;;; ===========================================================================
;;; §20 Character class (p.83-84)
;;; ===========================================================================

;; p.83 characterp の例をそのまま転記
(assert-equal t (characterp #\a))
(assert-equal nil (characterp "a"))
(assert-equal nil (characterp 'a))

;; p.84 文字比較の例をそのまま転記((char< #\a #\A) / (char< #\* #\a) / (char<= #\a #\A) は
;; implementation-defined のため省略)
(assert-equal t (char= #\a #\a))
(assert-equal nil (char= #\a #\b))
(assert-equal nil (char= #\a #\A))
(assert-equal nil (char/= #\a #\a))
(assert-equal nil (char< #\a #\a))
(assert-equal t (char< #\a #\b))
(assert-equal nil (char< #\b #\a))
(assert-equal t (char> #\b #\a))
(assert-equal t (char<= #\a #\a))
(assert-equal t (char>= #\b #\a))
(assert-equal t (char>= #\a #\a))


;;; ===========================================================================
;;; §21 List class (p.85-91)
;;; ===========================================================================

;; p.85 consp の例をそのまま転記
(assert-equal t (consp '(a . b)))
(assert-equal t (consp '(a b c)))
(assert-equal nil (consp '()))
(assert-equal nil (consp #(a b)))

;; p.85 cons の例をそのまま転記
(assert-equal '(a) (cons 'a '()))
(assert-equal '((a) b c d) (cons '(a) '(b c d)))
(assert-equal '("a" b c) (cons "a" '(b c)))
(assert-equal '(a . 3) (cons 'a 3))
(assert-equal '((a b) . c) (cons '(a b) 'c))

;; p.85-86 car の例をそのまま転記((car '()) はエラー)
(assert-error (car '()))
(assert-equal 'a (car '(a b c)))
(assert-equal '(a) (car '((a) b c d)))
(assert-equal 1 (car '(1 . 2)))
;; p.86 cdr の例をそのまま転記((cdr '()) はエラー)
(assert-error (cdr '()))
(assert-equal '(b c d) (cdr '((a) b c d)))
(assert-equal 2 (cdr '(1 . 2)))

;; p.86 (setf (car x) 'banana) の例をそのまま転記
(assert-equal '((banana orange) apple banana (banana orange) banana)
  (let ((x (list 'apple 'orange)))
    (list x (car x)
          (setf (car x) 'banana)
          x (car x))))

;; p.86 (setf (cdr x) 'banana) の例をそのまま転記
(assert-equal '((apple . banana) (orange) banana (apple . banana) banana)
  (let ((x (list 'apple 'orange)))
    (list x (cdr x)
          (setf (cdr x) 'banana)
          x (cdr x))))

;; p.87 null の例をそのまま転記
(assert-equal nil (null '(a b c)))
(assert-equal t (null '()))
(assert-equal t (null (list)))

;; p.87 listp の例をそのまま転記(循環リストの例を含む)
(assert-equal t (listp '(a b c)))
(assert-equal t (listp '()))
(assert-equal t (listp '(a . b)))
(assert-equal t
  (let ((x (list 'a)))
    (setf (cdr x) x)
    (listp x)))
(assert-equal nil (listp "abc"))
(assert-equal nil (listp #(1 2)))
(assert-equal nil (listp 'jerome))

;; p.88 create-list の例をそのまま転記
(assert-equal '(17 17 17) (create-list 3 17))
(assert-equal '(#\a #\a) (create-list 2 #\a))

;; p.88 list の例をそのまま転記
(assert-equal '(a 7 c) (list 'a (+ 3 4) 'c))
(assert-equal nil (list))

;; p.88 reverse の例をそのまま転記(nreverse の例は implementation-defined のため省略)
(assert-equal '(e d c b a) (reverse '(a b c d e)))
(assert-equal '(a) (reverse '(a)))
(assert-equal '() (reverse '()))

;; p.89 (append '(a b c) '(d e f)) => (a b c d e f) をそのまま転記
(assert-equal '(a b c d e f) (append '(a b c) '(d e f)))

;; p.89 member の例をそのまま転記
(assert-equal '(c d e f) (member 'c '(a b c d e f)))
(assert-equal nil (member 'g '(a b c d e f)))
(assert-equal '(c a b c) (member 'c '(a b c a b c)))

;; p.90-91 mapcar/mapc/maplist/mapl/mapcan/mapcon の例をそのまま転記
(assert-equal '(1 2 3) (mapcar #'car '((1 a) (2 b) (3 c))))
(assert-equal '(3 4 2 5 6) (mapcar #'abs '(3 -4 2 -5 -6)))
(assert-equal '((a . 1) (b . 2) (c . 3)) (mapcar #'cons '(a b c) '(1 2 3)))
(assert-equal 8 (let ((x 0)) (mapc (lambda (v) (setq x (+ x v))) '(3 5)) x))
(assert-equal '((1 2 3 4 1 2 1 2 3) (2 3 4 2 2 3))
  (maplist #'append '(1 2 3 4) '(1 2) '(1 2 3)))
(assert-equal '((foo a b c d) (foo b c d) (foo c d) (foo d))
  (maplist (lambda (x) (cons 'foo x)) '(a b c d)))
(assert-equal '(0 0 1 0 1 1 1)
  (maplist (lambda (x) (if (member (car x) (cdr x)) 0 1))
           '(a b a c d b c)))
(assert-equal 4
  (let ((k 0))
    (mapl (lambda (x)
            (setq k (+ k (if (member (car x) (cdr x)) 0 1))))
          '(a b a c d b c))
    k))
(assert-equal '(4 5 7) (mapcan (lambda (x) (if (> x 0) (list x))) '(-3 4 0 5 -2 7)))
(assert-equal '(a b c b c)
  (mapcon (lambda (x) (if (member (car x) (cdr x)) (list (car x))))
          '(a b a c d b c b c)))
(assert-equal '((1 2 3 4) (2 3 4) (3 4) (4)) (mapcon #'list '(1 2 3 4)))

;; p.91 assoc の例をそのまま転記
(assert-equal '(a . 1) (assoc 'a '((a . 1) (b . 2))))
(assert-equal '(a . 1) (assoc 'a '((a . 1) (a . 2))))
(assert-equal nil (assoc 'c '((a . 1) (b . 2))))


;;; ===========================================================================
;;; §22 Arrays (p.93-95)
;;; ===========================================================================

;; p.93 basic-array-p/basic-array*-p/general-array*-p の例。#1a(a b c) / #2a((a) (b) (c))
;; はリーダー構文依存のため、isiki_test_syntax.lisp にそのまま転記し、ここでは
;; 同じ外延を create-array で組み立てた版(cf.)を置く。
(assert-equal '((nil nil nil) (t nil nil) (t nil nil) (t nil nil) (t t t))
  (mapcar (lambda (x)
            (list (basic-array-p x)
                  (basic-array*-p x)
                  (general-array*-p x)))
          (list '(a b c)
                "abc"
                #(a b c)
                (create-array '(3) 'a)
                (create-array '(3 1) 'a))))

;; p.93 create-array の例。1つ目の期待値 #2a(...) はリーダー構文依存のため
;; isiki_test_syntax.lisp 側で比較し、ここでは次元と要素で確認する(cf.)。
(assert-equal '(2 3) (array-dimensions (create-array '(2 3) 0.0)))
(assert-equal 0.0 (aref (create-array '(2 3) 0.0) 1 2))
(assert-equal #(0.0 0.0) (create-array '(2) 0.0))

;; p.94 aref / set-aref の例をそのまま転記(array1 の印字表現の比較は
;; isiki_test_syntax.lisp 側)
(assert-equal 'array1 (defglobal array1 (create-array '(3 3 3) 0)))
(assert-equal 0 (aref array1 0 1 2))
(assert-equal 3.14 (setf (aref array1 0 1 2) 3.14))
(assert-equal 3.14 (aref array1 0 1 2))
(assert-equal 6 (aref (create-array '(8 8) 6) 1 1))
(assert-equal 19 (aref (create-array '() 19)))
(assert-equal 3.15 (setf (aref array1 0 1 2) 3.15))
(assert-equal 51.3 (set-aref 51.3 array1 0 1 2))
(assert-equal 51.3 (aref array1 0 1 2))

;; p.95 array-dimensions の例をそのまま転記
(assert-equal '(2 2) (array-dimensions (create-array '(2 2) 0)))
(assert-equal '(2) (array-dimensions (vector 'a 'b)))
(assert-equal '(3) (array-dimensions "foo"))


;;; ===========================================================================
;;; §23 Vectors (p.95-96)
;;; ===========================================================================

;; p.95 basic-vector-p/general-vector-p の例。#1a/#2a の項目はリーダー構文依存のため
;; isiki_test_syntax.lisp にそのまま転記し、ここでは create-array で組み立てた版(cf.)。
(assert-equal '((nil nil) (t nil) (t t) (t t) (nil nil))
  (mapcar (lambda (x)
            (list (basic-vector-p x)
                  (general-vector-p x)))
          (list '(a b c)
                "abc"
                #(a b c)
                (create-array '(3) 'a)
                (create-array '(3 1) 'a))))

;; p.96 create-vector の例をそのまま転記
(assert-equal #(17 17 17) (create-vector 3 17))
(assert-equal #(#\a #\a) (create-vector 2 #\a))

;; p.96 vector の例をそのまま転記
(assert-equal #(a b c) (vector 'a 'b 'c))
;; (vector) => #() の #() リテラルとの比較は isiki_test_syntax.lisp 側。ここでは長さ0の
;; general-vector が返ることを確認する(cf.)
(assert-equal 0 (length (vector)))
(assert-equal t (general-vector-p (vector)))


;;; ===========================================================================
;;; §24 String class (p.97-99)
;;; ===========================================================================

;; p.97 (stringp "abc") => t / (stringp 'abc) => nil をそのまま転記
(assert-equal t (stringp "abc"))
(assert-equal nil (stringp 'abc))

;; p.97 (create-string 3 #\a) => "aaa" / (create-string 0 #\a) => "" をそのまま転記
(assert-equal "aaa" (create-string 3 #\a))
(assert-equal "" (create-string 0 #\a))

;; p.98 string比較関数の例をそのまま転記
(assert-equal t (if (string= "abcd" "abcd") t nil))
(assert-equal nil (if (string= "abcd" "wxyz") t nil))
(assert-equal nil (if (string= "abcd" "abcde") t nil))
(assert-equal nil (if (string= "abcde" "abcd") t nil))
(assert-equal t (if (string/= "abcd" "wxyz") t nil))
(assert-equal nil (if (string< "abcd" "abcd") t nil))
(assert-equal t (if (string< "abcd" "wxyz") t nil))
(assert-equal t (if (string< "abcd" "abcde") t nil))
(assert-equal nil (if (string< "abcde" "abcd") t nil))
(assert-equal t (if (string<= "abcd" "abcd") t nil))
(assert-equal t (if (string<= "abcd" "wxyz") t nil))
(assert-equal t (if (string<= "abcd" "abcde") t nil))
(assert-equal nil (if (string<= "abcde" "abcd") t nil))
(assert-equal nil (if (string> "abcd" "wxyz") t nil))
(assert-equal t (if (string>= "abcd" "abcd") t nil))

;; p.98 char-index の例をそのまま転記
(assert-equal 1 (char-index #\b "abcab"))
(assert-equal nil (char-index #\B "abcab"))
(assert-equal 4 (char-index #\b "abcab" 2))
(assert-equal nil (char-index #\d "abcab"))
(assert-equal nil (char-index #\a "abcab" 4))

;; p.99 string-index の例をそのまま転記
(assert-equal 0 (string-index "foo" "foobar"))
(assert-equal 3 (string-index "bar" "foobar"))
(assert-equal nil (string-index "FOO" "foobar"))
(assert-equal nil (string-index "foo" "foobar" 1))
(assert-equal 3 (string-index "bar" "foobar" 1))
(assert-equal nil (string-index "foo" ""))
(assert-equal 0 (string-index "" "foo"))

;; p.99 string-append の例をそのまま転記
(assert-equal "abcdef" (string-append "abc" "def"))
(assert-equal "abcabc" (string-append "abc" "abc"))
(assert-equal "abc" (string-append "abc" ""))
(assert-equal "abc" (string-append "" "abc"))
(assert-equal "abcdef" (string-append "abc" "" "def"))


;;; ===========================================================================
;;; §25 Sequence functions (p.100-102)
;;; ===========================================================================

;; p.100 length の例をそのまま転記
(assert-equal 3 (length '(a b c)))
(assert-equal 3 (length '(a (b) (c d e))))
(assert-equal 0 (length '()))
(assert-equal 3 (length (vector 'a 'b 'c)))

;; p.100 elt の例をそのまま転記
(assert-equal 'c (elt '(a b c) 2))
(assert-equal 'b (elt (vector 'a 'b 'c) 1))
(assert-equal #\a (elt "abc" 0))

;; p.100 (setf (elt string 2) #\O) の例をそのまま転記(仕様の最後の行の x は
;; string の誤記とみなす)
(assert-equal "xxOxx"
  (let ((string (create-string 5 #\x)))
    (setf (elt string 2) #\O)
    string))

;; p.101 subseq の例をそのまま転記
(assert-equal "bcd" (subseq "abcdef" 1 4))
(assert-equal '(b c d) (subseq '(a b c d e f) 1 4))
(assert-equal #(b c d) (subseq (vector 'a 'b 'c 'd 'e 'f) 1 4))

;; p.101-102 map-into の例をそのまま転記(仕様の setq a/b/k は未定義変数への setq
;; なので、先に defglobal で変数を用意してから仕様通り setq する)
(defglobal a nil)
(defglobal b nil)
(defglobal k nil)
(assert-equal '(1 2 3 4) (setq a (list 1 2 3 4)))
(assert-equal '(10 10 10 10) (setq b (list 10 10 10 10)))
(assert-equal '(11 12 13 14) (map-into a #'+ a b))
(assert-equal '(11 12 13 14) a)
(assert-equal '(10 10 10 10) b)
(assert-equal '(one two three) (setq k '(one two three)))
(assert-equal '((one . 11) (two . 12) (three . 13) 14) (map-into a #'cons k a))
(assert-equal '(2 4 6 8)
  (let ((x 0))
    (map-into a
              (lambda () (setq x (+ x 2))))))
(assert-equal '(2 4 6 8) a)


;;; ===========================================================================
;;; §26 Stream class (p.102-106)
;;; ===========================================================================

;; p.102 streamp の例をそのまま転記
(assert-equal t (streamp (standard-input)))
(assert-equal nil (streamp '()))

;; p.102 input-stream-p の例をそのまま転記
(assert-equal t (input-stream-p (standard-input)))
(assert-equal nil (input-stream-p (standard-output)))
(assert-equal nil (input-stream-p '(a b c)))

;; p.103 output-stream-p の例をそのまま転記
(assert-equal t (output-stream-p (standard-output)))
(assert-equal nil (output-stream-p (standard-input)))
(assert-equal nil (output-stream-p "hello"))

;; p.103 with-standard-input の例をそのまま転記((read) は引数省略で standard-input から読む)
(assert-equal '(this is)
  (with-standard-input (create-string-input-stream "this is a string")
    (list (read) (read))))

;; p.104 (open-input-file "example.lsp" 8) => implementation-defined は省略。
;; p.104 with-open-output-file / with-open-input-file の例をそのまま転記
;; (ファイル名は /9p/tmp/ 以下に変更)
(assert-equal nil
  (with-open-output-file (outstream "/9p/tmp/isiki-test-example.dat")
    (format outstream "hello")))
(assert-equal 'hello
  (with-open-input-file (instream "/9p/tmp/isiki-test-example.dat")
    (read instream)))

;; p.105 open-input-file / close の例をそのまま転記(close の戻り値は
;; implementation-defined なので、2回 close してもエラーにならないことだけ確認する)
(assert-equal 'input-str (defglobal input-str (open-input-file "/9p/tmp/isiki-test-example.dat")))
(assert-equal t (progn (close input-str) t))
(assert-equal t (progn (close input-str) t))

;; p.105 open-output-file / finish-output の例をそのまま転記
(assert-equal 'output-str (defglobal output-str (open-output-file "/9p/tmp/isiki-test-data.lsp")))
(assert-equal nil (finish-output output-str))
(close output-str)

;; p.106 create-string-input-stream の例をそのまま転記
(assert-equal '(this is a)
  (let ((str (create-string-input-stream "this is a string")))
    (list (read str) (read str) (read str))))

;; p.106 create-string-output-stream / get-output-stream-string の2例をそのまま転記
(assert-equal "helloworld"
  (let ((str (create-string-output-stream)))
    (format str "hello")
    (format str "world")
    (get-output-stream-string str)))
(assert-equal '("This is a string" "right!")
  (let ((out-str (create-string-output-stream)))
    (format out-str "This is a string")
    (let ((part1 (get-output-stream-string out-str)))
      (format out-str "right!")
      (list part1 (get-output-stream-string out-str)))))


;;; ===========================================================================
;;; §27 Input and output (p.107-112)
;;; ===========================================================================

;; p.107 read の例をそのまま転記。文字列リテラル中の "#\\A" は §24 の規則
;; (バックスラッシュはバックスラッシュでエスケープする)により文字列としては #\A になる。
(assert-equal 'str (defglobal str (create-string-input-stream "hello #(1 2 3) 123 #\\A")))
(assert-equal 'hello (read str))
(assert-equal #(1 2 3) (read str))
(assert-equal 123 (read str))
(assert-equal #\A (read str))
(assert-equal "the end" (read str nil "the end"))

;; p.108 read-char の例をそのまま転記(ストリーム終端でエラー)
(assert-equal 'str (defglobal str (create-string-input-stream "hi")))
(assert-equal #\h (read-char str))
(assert-equal #\i (read-char str))
(assert-error (read-char str))

;; p.108 preview-char の例をそのまま転記
(assert-equal '(#\f #\f #\o)
  (let ((s (create-string-input-stream "foo")))
    (list (preview-char s) (read-char s) (read-char s))))

;; p.108-109 read-line の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更)
(assert-equal nil
  (with-open-output-file (out "/9p/tmp/isiki-test-newfile")
    (format out "This is an example")
    (format out "~%")
    (format out "look at the output file")))
(assert-equal 'str (defglobal str (open-input-file "/9p/tmp/isiki-test-newfile")))
(assert-equal "This is an example" (read-line str))
(assert-equal "look at the output file" (read-line str))
(close str)

;; p.109 stream-ready-p の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更)
(assert-equal nil
  (with-open-output-file (out "/9p/tmp/isiki-test-testfile.dat")
    (format out "This is an example")))
(assert-equal t
  (with-open-input-file (in "/9p/tmp/isiki-test-testfile.dat")
    (stream-ready-p in)))

;; p.111 format の例をそのまま転記。output-stream は文字列出力ストリームに束縛し、
;; 戻り値(nil)と出力を assert-output で検証する。
(assert-output (r o) (format (standard-output) "No result")
  (assert-equal nil r)
  (assert-equal "No result" o))
(assert-output (r o) (format (standard-output) "The result is ~A and nothing else." "meningitis")
  (assert-equal nil r)
  (assert-equal "The result is meningitis and nothing else." o))
(assert-output (r o) (format (standard-output) "The result i~C" #\s)
  (assert-equal nil r)
  (assert-equal "The result is" o))
(assert-output (r o) (format (standard-output) "The results are ~S and ~S." 1 #\a)
  (assert-equal nil r)
  (assert-equal "The results are 1 and #\\a." o))
(assert-output (r o) (format (standard-output) "Binary code ~B" 150)
  (assert-equal nil r)
  (assert-equal "Binary code 10010110" o))
(assert-output (r o) (format (standard-output) "permission ~O" 493)
  (assert-equal nil r)
  (assert-equal "permission 755" o))
(assert-output (r o) (format (standard-output) "You ~X ~X" 2989 64206)
  (assert-equal nil r)
  (assert-equal "You BAD FACE" o))
(assert-output (r o)
  (progn
    (format (standard-output) "~&Name ~10Tincome ~20Ttax~%")
    (format (standard-output) "~A ~10T~D ~20T~D" "Grummy" 23000 7500))
  (assert-equal nil r)
  (assert-equal "Name      income    tax
Grummy    23000     7500" o))
(assert-output (r o) (format (standard-output) "This will be split into~%two lines.")
  (assert-equal nil r)
  (assert-equal "This will be split into
two lines." o))
(assert-output (r o) (format (standard-output) "This is a tilde: ~~")
  (assert-equal nil r)
  (assert-equal "This is a tilde: ~" o))

;; p.112 read-byte の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更。
;; 8ビットのバイトコードが格納される前提なので、hello の各文字の ASCII コードになる)
(assert-equal 'byte-example (defglobal byte-example (open-output-stream "/9p/tmp/isiki-test-byte-ex")))
(assert-equal nil (format byte-example "hello"))
(close byte-example)
(setq byte-example (open-input-stream "/9p/tmp/isiki-test-byte-ex" 8))
(assert-equal 104 (read-byte byte-example))
(assert-equal 101 (read-byte byte-example))
(assert-equal 108 (read-byte byte-example))
(assert-equal 108 (read-byte byte-example))
(assert-equal 111 (read-byte byte-example))
(close byte-example)

;; p.112 write-byte の例をそのまま転記(close の戻り値は implementation-defined)。
;; 書いたバイトを read-byte で読み戻して確認する。
(let ((out-str (open-output-stream "/9p/tmp/isiki-test-byte-example" 8)))
  (write-byte #b101 out-str)
  (close out-str))
(assert-equal 5
  (let ((in-str (open-input-stream "/9p/tmp/isiki-test-byte-example" 8)))
    (let ((v (read-byte in-str)))
      (close in-str)
      v)))


;;; ===========================================================================
;;; §28 Files (p.113-114)
;;; ===========================================================================

;; p.113 probe-file の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更。テストの
;; 再実行で残っているかもしれないので、事前に別名の存在しないファイルで確認する)
(assert-equal nil (probe-file "/9p/tmp/isiki-test-notexist-never-created.lsp"))
(assert-equal 'new-file (defglobal new-file (open-output-file "/9p/tmp/isiki-test-notexist.lsp")))
(close new-file)
(assert-equal t (probe-file "/9p/tmp/isiki-test-notexist.lsp"))

;; p.113 file-position の例をそのまま転記(ファイル名は /9p/tmp/ 以下に変更)
(assert-equal 'example (defglobal example (open-output-file "/9p/tmp/isiki-test-example.lsp")))
(assert-equal nil (format example "hello"))
(close example)
(setq example (open-input-stream "/9p/tmp/isiki-test-example.lsp" 8))
(assert-equal 0 (file-position example))
(assert-equal 104 (read-byte example))
(assert-equal 1 (file-position example))

;; p.114 (set-file-position example 4) => 4 をそのまま転記(続けて位置4の 'o' が読めること)
(assert-equal 4 (set-file-position example 4))
(assert-equal 111 (read-byte example))
(close example)

;; p.114 file-length の例。file27.dat は仕様に中身が無いので25バイトのファイルを作って
;; 確認する(バイトサイズ2の例は「Implementations are not required to support」のため省略)
(with-open-output-file (out "/9p/tmp/isiki-test-file27.dat")
  (format out "0123456789012345678901234"))
(assert-equal 25 (file-length "/9p/tmp/isiki-test-file27.dat" 8))


;;; ===========================================================================
;;; §29 Condition system (p.115-122)
;;; ===========================================================================

;; p.117 signal-condition の例をそのまま転記。ハンドラで捕捉して、<simple-error> の
;; インスタンスが format-string / format-arguments を持って届くことを確認する。
(assert-equal '(<simple-error> "A ~A problem occurred." (bad))
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

;; p.115-117 (§29 Conditions) には上記以外の "Example:" ブロックは存在しない。
;; 以下は §29.2 の説明に基づき独自に作成した例。

;; cf. p.116-117 with-handler/error の説明に基づく独自の例
(assert-equal 'caught
  (block isiki-test-b1
    (with-handler (lambda (c) (return-from isiki-test-b1 'caught))
      (error "boom"))))

;; p.117 (ignore-errors form*) の説明に基づく独自の例
(assert-equal nil (ignore-errors (error "boom")))
(assert-equal 5 (ignore-errors 5))

;; cf. p.115 「ハンドラは受け取ったconditionに対しsignal-conditionを呼ぶことで
;; 外側のハンドラに委譲できる」という説明に基づく独自の例
(assert-equal 'outer
  (block isiki-test-b2
    (with-handler (lambda (c) (return-from isiki-test-b2 'outer))
      (with-handler (lambda (c) (if (typep c '<simple-error>) (signal-condition c nil) (return-from isiki-test-b2 'inner)))
        (error "boom")))))

;; cf. p.116 signal-condition の continuable 引数の説明に基づく独自の例
(assert-equal 101
  (+ 1
     (with-handler (lambda (c) 100)
       (signal-condition (make-instance '<condition>) t))))


;;; ===========================================================================
;;; §30 Miscellaneous (p.122)
;;; ===========================================================================

;; p.122 (identity '(a b c)) => (a b c) をそのまま転記
(assert-equal '(a b c) (identity '(a b c)))

;; p.122 (get-universal-time) => 2901312000 の例。値は実行時刻に依存するので、
;; 1900年起点の秒数として正の整数が返ることを確認する。
(assert-equal t (integerp (get-universal-time)))
(assert-equal t (> (get-universal-time) 0))


;;; ===========================================================================
;;; カーネル固有の回帰テスト
;;; ===========================================================================

;; M12 Phase7(#27)の恒久リグレッションテスト: %abort-top-levelがAOT化された後も、
;; return-from %top-levelの非局所脱出がos_eval_top_level(このトップレベルフォーム
;; 自身を包むblock %top-level)まで正しく伝播することを確認する(未解決リスク②)。
;; %%test-aot-call-abort-top-levelは%%funcall-by-name経由でAOTコード内から呼ばれた
;; 場合の伝播を、直後の(%abort-top-level ...)は通常のトップレベル関数呼び出しから
;; AOTのreturn-fromが素通りするかを確認する。いずれも単独のトップレベルフォームなので
;; block %top-levelは毎回新規に張られ、後続のフォームには影響しない。続く
;; (assert-equal t t)まで正常に到達することが、伝播が壊れていない証拠になる
(%%test-aot-call-abort-top-level 'isiki-test-phase7-abort-marker-1)
(%abort-top-level 'isiki-test-phase7-abort-marker-2)
(assert-equal t t)
