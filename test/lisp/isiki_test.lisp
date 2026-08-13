;; test/lisp/isiki_test.lisp
;;
;; isiki-os を QEMU 上で起動した REPL から (load "isiki_test.lisp") して実行することを
;; 想定したテストファイル。test/c/script_test.c のネイティブ assert-equal は QEMU 起動後
;; の環境には存在しないため、本ファイル内で自前の assert-equal / assert-float-close を
;; 定義する。
;;
;; 各テストの直前の ;; コメントに、[ISLisp Working Draft 23.0](http://islisp.org/docs/islisp-v23.pdf)
;; 上で対応する "Example:" が記載されているページ番号を記す。
;; 変数名や具体的な数値を(このカーネルの識別子衝突を避ける目的、あるいは単に別の値でも
;; 成立することを示す目的で)変更している場合は「cf.」を付け、仕様の例をそのまま転記した
;; 場合は「p.N」のみを付けている。
;;
;; ただし以下は除外している:
;;   - (implementation-defined) と明記されている例
;;   - このカーネルで未実装の機能を使う例 (documents/isiki-os.md のチェックリストで
;;     [ ] のもの。defclass の :metaclass / :abstractp オプション、mapcan の nconc に
;;     よる正確な破壊的連結、generic-function-p の正しい判定など)
;;   - 無理数・循環小数を結果に持つ浮動小数点演算 (sqrt/log/exp/三角関数等) は、
;;     このカーネルの IEEE754 double 実装が仕様の記載値とビット単位で一致するとは
;;     限らないため、assert-equalではなく許容誤差付きの assert-float-close を使う
;;

(defglobal *isiki-test-stream* (open-output-file "test-results.txt"))
(defglobal *isiki-test-pass* 0)
(defglobal *isiki-test-fail* 0)

(defmacro assert-equal (expected form)
  `(let ((%isiki-expected ,expected) (%isiki-actual ,form))
     (if (equal %isiki-expected %isiki-actual)
         (setq *isiki-test-pass* (+ *isiki-test-pass* 1))
         (progn
           (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
           (format *isiki-test-stream* "[NG] ~S => ~S (expected ~S)~%"
                   ',form %isiki-actual %isiki-expected)))))

(defmacro assert-float-close (expected form)
  `(let ((%isiki-expected ,expected) (%isiki-actual ,form))
     (if (< (abs (- %isiki-expected %isiki-actual)) 1.0e-6)
         (setq *isiki-test-pass* (+ *isiki-test-pass* 1))
         (progn
           (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
           (format *isiki-test-stream* "[NG] ~S => ~S (expected ~~ ~S)~%"
                   ',form %isiki-actual %isiki-expected)))))

(defun isiki-test-report ()
  (format *isiki-test-stream* "~%==== isiki_test.lisp: ~D passed, ~D failed ====~%"
          *isiki-test-pass* *isiki-test-fail*))


;; p.19 (defun copy-cell (x) (cons (car x) (cdr x))) の例。defunの戻り値が関数名
;; シンボルであること(p.28の一般規則)の確認と、定義した関数の動作確認を追加。
(defglobal isiki-test-defun-copy-cell-result (defun copy-cell (x) (cons (car x) (cdr x))))
(assert-equal 'copy-cell isiki-test-defun-copy-cell-result)
(assert-equal '(1 . 2) (copy-cell '(1 . 2)))


;; p.23 (functionp (function car)) => t
(assert-equal t (functionp (function car)))
;; cf. p.23 functionp の説明に基づく非関数の例(仕様は #\a 等、ここでは整数)。
(assert-equal nil (functionp 1))
;; p.23 (funcall (function -) 3) => -3 をそのまま転記
(assert-equal -3 (funcall (function -) 3))
;; p.23 (apply #'- '(4 3)) => 1 をそのまま転記
(assert-equal 1 (apply (function -) '(4 3)))

;; p.24 ((lambda (x y) (+ (* x x) (* y y))) 3 4) => 25 をそのまま転記
(assert-equal 25 ((lambda (x y) (+ (* x x) (* y y))) 3 4))
;; p.24 ((lambda (x y &rest z) z) 3 4 5 6) => (5 6) をそのまま転記
(assert-equal '(5 6) ((lambda (x y &rest z) z) 3 4 5 6))
;; p.24 ((lambda (x y :rest z) z) 3 4 5 6) => (5 6) の :rest 版は、このカーネルが
;; rest引数のマーカーとして &rest のみを認識し :rest を認識しない(未実装)ため省略する。
;; p.24 (funcall (lambda (x y) (- y (* x y))) 7 3) => -18 の演算子・値を変更した版。
(assert-equal 6 (funcall (lambda (x y) (* x y)) 2 3))

;; p.25 (labels ((evenp (n) ...) (oddp (n) ...)) (evenp 88)) => t の例。cf. として
;; evenp/oddp の代わりに階乗を定義した独自の labels 例。
(assert-equal 120
  (labels ((isiki-test-fact (n)
             (if (<= n 1) 1 (* n (isiki-test-fact (- n 1))))))
    (isiki-test-fact 5)))

;; cf. p.25 (flet ((f (x) (+ x 3))) (flet ((f (x) (+ x (f x)))) (f 7))) => 17 の例。
;; 内側の再定義部分を省いた簡略版。
(assert-equal 5
  (flet ((isiki-test-add1 (x) (+ x 1)))
    (isiki-test-add1 4)))

;; p.25 (apply (if (< 1 2) (function max) (function min)) 1 2 (list 3 4)) => 4 をそのまま転記
(assert-equal 4
  (apply (if (< 1 2) (function max) (function min))
         1 2 (list 3 4)))
;; cf. p.25 (defun compose (f g) (lambda (:rest args) (funcall f (apply g args))))
;; (funcall (compose #'sqrt #'*) 12 75) => 30 の例。:rest未対応のため &rest に変更し、
;; sqrt の結果はfloatなので許容誤差付きの比較にする。
(assert-float-close 30.0
  (flet ((isiki-test-compose (f g)
           (lambda (&rest args) (funcall f (apply g args)))))
    (funcall (isiki-test-compose (function sqrt) (function *)) 12 75)))


;; cf. p.26 (defconstant e 2.7182818284590451) の例。識別子・値を変更。
(defconstant isiki-test-limit 100)
(assert-equal 100 isiki-test-limit)


;; cf. p.27 (defglobal today 'wednesday) の例。識別子衝突回避のため isiki-test-x に変更。
(defglobal isiki-test-x 10)
(assert-equal 10 isiki-test-x)
(setq isiki-test-x 20)
(assert-equal 20 isiki-test-x)


;; cf. p.27 (defdynamic *color* 'red) / (dynamic-let ((*color* 'green)) ...) の例。
;; 識別子・値を変更。
(defdynamic *isiki-test-dyn* 1)
(assert-equal 1 (dynamic *isiki-test-dyn*))
(assert-equal 2
  (dynamic-let ((*isiki-test-dyn* 2))
    (dynamic *isiki-test-dyn*)))
(assert-equal 1 (dynamic *isiki-test-dyn*))


;; p.28 (defun caar (x) (car (car x))) => caar をそのまま転記
(defglobal isiki-test-defun-caar-result (defun caar (x) (car (car x))))
(assert-equal 'caar isiki-test-defun-caar-result)
(assert-equal 1 (caar '((1 2) 3)))


;; p.29 eq/eqlの等価性表(PDF画像で物理36ページを直接確認した23行のうち、
;; (implementation-defined)と明記されている3行 (eq 2 2) (eq 100000000 100000000)
;; (eq 10.00000 10.0) を除いた20行をそのまま転記)。
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


;; p.31 (not t)(not '())(not 'nil)(not nil)(not 3)(not (list))(not (list 3))
(assert-equal nil (not t))
(assert-equal t (not '()))
;; p.31 (not 'nil): os_make_symbolが"NIL"という名前を特別扱いし、真のnilセンチネル自身を
;; 返すことで、'nil と '() が同一オブジェクトになるよう修正済み。
(assert-equal t (not 'nil))
(assert-equal t (not nil))
(assert-equal nil (not 3))
(assert-equal t (not (list)))
(assert-equal nil (not (list 3)))


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


;; cf. p.32 and の定義(≡ 't/form/...)に基づく例。仕様の "Example:" の式(=/eql/setq を使う例)は
;; そのまま実行すると副作用があるため、値の組み合わせのみを転記した簡略版。
(assert-equal 4 (and 1 2 4))
(assert-equal nil (and 1 nil 4))
(assert-equal t (and))


;; cf. p.32 or の定義(≡ 'nil/form/...)に基づく例。and と同様の理由で値のみの簡略版。
(assert-equal 1 (or 1 2 4))
(assert-equal 4 (or nil nil 4))
(assert-equal nil (or))


;; p.33 リテラル定数の例。#2A((a b c) (d e f)) はこのカーネルのリーダーが#nA配列
;; リテラル構文を持たないため省略し、残りをそのまま転記。
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


;; cf. p.34 (defglobal x 0) => x / x => 0 / (let ((x 1)) x) => 1 / x => 0 の例。
;; 識別子衝突回避のため isiki-test-var-a に変更。
(defglobal isiki-test-var-a 0)
(assert-equal 0 isiki-test-var-a)
(assert-equal 1 (let ((isiki-test-var-a 1)) isiki-test-var-a))
(assert-equal 0 isiki-test-var-a)

;; cf. p.34 (defglobal x 2) / (+ x 1) => 3 / (setq x 4) => 4 / (+ x 1) => 5 /
;; (let ((x 1)) (setq x 2) x) => 2 / (+ x 1) => 5 の例。識別子衝突回避のため
;; isiki-test-var-b に変更。
(defglobal isiki-test-var-b 2)
(assert-equal 3 (+ isiki-test-var-b 1))
(assert-equal 4 (setq isiki-test-var-b 4))
(assert-equal 5 (+ isiki-test-var-b 1))
(assert-equal 2 (let ((isiki-test-var-b 1)) (setq isiki-test-var-b 2) isiki-test-var-b))
(assert-equal 5 (+ isiki-test-var-b 1))


;; p.35 (setf (car x) 2) => 2 の例。識別子衝突回避のため固有のconsを用意する。
;; 仕様には続けて (defmacro first (spot) `(car ,spot)) / (setf (first x) 2) => 2
;; という、setfがユーザー定義マクロで書かれたplaceを展開してから処理する例もあるが、
;; このカーネルのsetfマクロ(src/lisp/init.lisp)はplaceのcarをcar/cdr/aref/elt/
;; slot-value/propertyのいずれかのシンボルと直接eqで比較するだけでplace自体の
;; マクロ展開を行わず、フォールバック節も無いため、この部分は未実装として省略する。
(defglobal isiki-test-setf-x (cons 1 2))
(assert-equal 2 (setf (car isiki-test-setf-x) 2))
(assert-equal 2 (car isiki-test-setf-x))


;; cf. p.36 (let ((x 2) (y 3)) (* x y)) => 6 の例。識別子・演算子・値を変更。
(assert-equal 3
  (let ((isiki-test-a 1) (isiki-test-b 2))
    (+ isiki-test-a isiki-test-b)))


;; cf. p.37 let* の例。識別子・値を変更。
(assert-equal 7
  (let* ((isiki-test-p 3) (isiki-test-q (+ isiki-test-p 4)))
    isiki-test-q))


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
(assert-equal 'two (case-using (function =) (+ 1.0 1.0) ((1) 'one) ((2) 'two) (t 'more)))
(assert-equal 2 (case-using (function string=) "bar" (("foo") 1) (("bar") 2)))


;; cf. p.41 (progn (setq x 5) (+ x 1)) => 6 の例。識別子衝突回避のため
;; isiki-test-progn-x に変更。
(defglobal isiki-test-progn-x 0)
(assert-equal 6 (progn (setq isiki-test-progn-x 5) (+ isiki-test-progn-x 1)))

;; cf. p.41 (progn (format (standard-output) "4 plus 1 equals ")
;; (format (standard-output) "~D" (+ 4 1))) => nil, prints "4 plus 1 equals 5" の例。
;; standard-outputの代わりにcreate-string-output-streamで出力先を捕捉し、
;; 印字結果も比較できるようにした版。
(assert-equal "4 plus 1 equals 5"
  (let ((isiki-test-progn-s (create-string-output-stream)))
    (progn
      (format isiki-test-progn-s "4 plus 1 equals ")
      (format isiki-test-progn-s "~D" (+ 4 1)))
    (get-output-stream-string isiki-test-progn-s)))


;; p.41 while の例をそのまま転記
(assert-equal '(1 2 3 4 5) (let ((x '()) (i 5)) (while (> i 0) (setq x (cons i x)) (setq i (- i 1))) x))


;; p.42 for の2例をそのまま転記
(assert-equal (vector 0 1 2 3 4)
  (for ((vec (vector 0 0 0 0 0)) (i 0 (+ i 1))) ((= i 5) vec) (setf (elt vec i) i)))
(assert-equal 25
  (let ((x '(1 3 5 7 9))) (for ((x x (cdr x)) (sum 0 (+ sum (car x)))) ((null x) sum))))


;; p.43 (block x (+ 10 (return-from x 6) 22)) => 6 をそのまま転記
(assert-equal 6 (block x (+ 10 (return-from x 6) 22)))

;; p.43 f1/f2 (blockをクロージャ越しにreturn-fromする例)をそのまま転記。識別子衝突
;; 回避のためisiki-test-f1/isiki-test-f2に変更し、defunの戻り値の確認も追加。
(assert-equal 'isiki-test-f1
  (defun isiki-test-f1 ()
    (block b
      (let ((f (lambda () (return-from b 'exit))))
        (isiki-test-f2 f)))))
(assert-equal 'isiki-test-f2
  (defun isiki-test-f2 (g) (funcall g)))
(assert-equal 'exit (isiki-test-f1))

;; cf. p.45 catch/throw の例。識別子衝突回避のためisiki-test-catch-foo/-barに変更。
(assert-equal 'isiki-test-catch-foo
  (defun isiki-test-catch-foo (x) (catch 'block-sum (isiki-test-catch-bar x))))
(assert-equal 'isiki-test-catch-bar
  (defun isiki-test-catch-bar (x)
    (for ((l x (cdr l)) (sum 0 (+ sum (car l))))
        ((null l) sum)
      (cond ((not (numberp (car l))) (throw 'block-sum 0))))))
(assert-equal 10 (isiki-test-catch-foo '(1 2 3 4)))
(assert-equal 0 (isiki-test-catch-foo '(1 2 a 4)))

;; cf. p.46 tagbody/go の with-retry マクロの例。:rest はこのカーネルで未対応のため
;; &rest に変更し、仕様自身が「ISLISPには実在しない仮想の関数」と明記している
;; if-error/sqrtの代わりに、リトライ回数を数えるだけの自己完結した本体に置き換える。
(assert-equal 'isiki-test-with-retry
  (defmacro isiki-test-with-retry (&rest forms)
    (let ((tag (gensym)))
      `(block ,tag
         (tagbody
           ,tag
           (return-from ,tag
             (flet ((retry () (go ,tag)))
               ,@forms)))))))
(assert-equal 3
  (let ((isiki-test-retry-count 0))
    (isiki-test-with-retry
      (setq isiki-test-retry-count (+ isiki-test-retry-count 1))
      (if (< isiki-test-retry-count 3) (retry) isiki-test-retry-count))))

;; p.48-60 (§15 Classes) には defclass/defgeneric/defmethod/next-method-p/
;; call-next-method の構文・意味の説明はあるが、point/point3d のような具体的な
;; "Example:" ブロックは存在しない。以下は §15 の説明に基づき独自に作成したクラス例。
;; ページ順に並べるため、defclass(p.48-50) → defgeneric/defmethod(p.53-59) →
;; typep/subclassp(p.61) の順にする(元のセクション内の記載順を入れ替えている)。

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

;; 期待値の中の nil をクォートリストの要素として書くと、リーダーがトークン"nil"を
;; 真のnil定数とは別のシンボルとしてinternしてしまい、equalが構造不一致と判定する
;; (§8 not の既知の不具合と同じ根本原因)。list呼び出し内で評価させることで、
;; 変数参照解決を経由した真のnil定数を使うよう回避する。
(assert-equal (list 'point-desc nil) (isiki-test-describe (make-instance 'isiki-test-point)))
(assert-equal (list '3d-desc t (list 'point-desc nil)) (isiki-test-describe (make-instance 'isiki-test-point3d)))

;; cf. p.61 typep/subclassp/class-of の説明に基づく独自の例
(assert-equal t (typep (make-instance 'isiki-test-point3d) 'isiki-test-point))
(assert-equal nil (typep (make-instance 'isiki-test-point) 'isiki-test-point3d))
(assert-equal t (subclassp (class-of (make-instance 'isiki-test-point3d))
                            (class-of (make-instance 'isiki-test-point))))


;; cf. p.62 (defmacro caar (x) (list 'car (list 'car x))) => caar の例。p.28で既に
;; caarという名前の関数を定義済みで、このカーネルのdefmacro/defunは同じ名前空間を
;; 共有し再定義を禁止しているため、識別子をisiki-test-caar-macroに変更する。
(assert-equal 'isiki-test-caar-macro
  (defmacro isiki-test-caar-macro (x) (list 'car (list 'car x))))
(assert-equal 1 (isiki-test-caar-macro '((1 2) 3)))

;; p.62-63 quasiquoteの1~4例目をそのまま転記。5・6例目(quasiquote自体をデータとして
;; ネストする例)は、このカーネルでの印字表現の正確な一致が未確認のため省略する。
(assert-equal '(list 3 4) `(list ,(+ 1 2) 4))
(assert-equal '(list name a (quote a)) (let ((name 'a)) `(list name ,name ',name)))
(assert-equal '(a 3 x x x b) `(a ,(+ 1 2) ,@(create-list 3 'x) b))
(assert-equal '((foo 7) . cons) `((foo ,(- 10 3)) ,@(cdr '(c)) . ,(car '(cons))))


;; p.63 (the <integer> 10) => 10 / (the <number> 10) => 10 をそのまま転記。
;; (the <float> 10) は仕様上「the consequences are undefined」と明記されており、
;; 除外方針(実装依存/未定義の例は除外)に合致するため省略する。
(assert-equal 10 (the <integer> 10))
(assert-equal 10 (the <number> 10))

;; p.63 (assure <integer> 10) => 10 / (assure <number> 10) => 10 をそのまま転記
(assert-equal 10 (assure <integer> 10))
(assert-equal 10 (assure <number> 10))


;; cf. p.65 (symbolp 'a) => t / (symbolp "a") => nil の例。(symbolp 3) は同ページの
;; symbolp の説明(obj may be any ISLISP object)に基づく独自の追加ケース。
(assert-equal t (symbolp 'abc))
(assert-equal nil (symbolp "abc"))
(assert-equal nil (symbolp 3))


;; cf. p.67-68 (property 'zeus 'daughter) / (setf (property 'zeus 'daughter) 'athena) /
;; (remove-property 'zeus 'daughter) の例。シンボル名・値を変更。
(assert-equal nil (property 'isiki-test-prop-sym 'isiki-test-prop-key))
(assert-equal 42
  (progn
    (set-property 42 'isiki-test-prop-sym 'isiki-test-prop-key)
    (property 'isiki-test-prop-sym 'isiki-test-prop-key)))
(assert-equal nil
  (progn
    (remove-property 'isiki-test-prop-sym 'isiki-test-prop-key)
    (property 'isiki-test-prop-sym 'isiki-test-prop-key)))


;; p.69 numberp/parse-numberの例をそのまま転記。123.34は浮動小数点の字句解析結果が
;; 完全一致するとは限らないため、assert-equalではなくassert-float-closeを使う
;; (test/lisp/init_test.lisp の %approx= を使った既存の parse-number テストと同様の理由)。
(assert-equal t (numberp 3))
(assert-equal t (numberp -0.3))
(assert-equal nil (numberp '(a b c)))
(assert-equal nil (numberp "17"))
(assert-float-close 123.34 (parse-number "123.34"))
(assert-equal 64206 (parse-number "#XFACE"))

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

;; 元は「§19 numeric functions」(p.71-84)と「§19 trigonometric / exponential
;; functions」(p.73-75)という2つの別セクションだったが、ページ順に並べるため
;; abs(p.73)とexpt(p.74)の間にexp/log(p.73-74)を、sqrt(p.74)とfloat(p.78)の間に
;; sin/cos/atan(p.75)を挿入する形で統合した。

;; cf. p.71 (+ 12 3) => 15 / (- 1 2) => -1 / (* 12 3) => 36 の例。値を変更。
(assert-equal 7 (+ 3 4))
(assert-equal (- 1) (- 3 4))
(assert-equal 12 (* 3 4))
;; cf. p.72 (quotient 10 5) => 2 の例。値を変更。
(assert-equal 2 (quotient 4 2))

;; cf. p.72-73 (max -5 3) => 3 / (min 3 1) => 1 の例。値を変更。
(assert-equal 3 (max 1 3 2))
(assert-equal 1 (min 1 3 2))
;; p.73 (abs -3) => 3 の例に相当(値を変更)。
(assert-equal 5 (abs (- 5)))
(assert-equal 5 (abs 5))

;; cf. p.73 (exp 1) => 2.718... と p.74 log の説明を組み合わせた exp(log 8.0) = 8.0 の例。
(assert-float-close 8.0 (exp (log 8.0)))

;; p.74 (expt 2 3) => 8 をそのまま転記
(assert-equal 8 (expt 2 3))
;; cf. p.74 (sqrt 4) => 2 の例。値を変更(いずれも平方数になるよう選んでいる)。
(assert-equal 9 (sqrt 81))
(assert-equal 4 (sqrt 16))

;; cf. p.75 sin/cos/atan の定義(値そのものは仕様の Example に無いが、0 の場合の
;; 自明な値として追加)。
(assert-float-close 0.0 (sin 0))
(assert-float-close 1.0 (cos 0))
(assert-float-close 0.0 (atan 0))

;; p.78 (float 2) => 2.0 / (float -2.0) => -2.0 をそのまま転記。
;; equal(primitive_equal)はTAG_INSTANCEのfloatをbignum/vectorのようには特殊扱いして
;; いないため、値が同じでも別々に確保されたfloatは常に不一致になる。assert-equalではなく
;; 数値としての近さを見るassert-float-closeで検証する。
(assert-float-close 2.0 (float 2))
(assert-float-close (- 2.0) (float (- 2.0)))
;; p.78 (floatp 2.0) => t / (floatp 2) => nil をそのまま転記
(assert-equal t (floatp 2.0))
(assert-equal nil (floatp 2))

;; p.79 (floor 3.4) => 3 / (floor -3.4) => -4 をそのまま転記
(assert-equal 3 (floor 3.4))
(assert-equal (- 4) (floor (- 3.4)))
;; p.79 (ceiling 3.4) => 4 / (ceiling -3.4) => -3 をそのまま転記
(assert-equal 4 (ceiling 3.4))
(assert-equal (- 3) (ceiling (- 3.4)))
;; p.79 (truncate 3.4) => 3 / (truncate -3.9) => -3 をそのまま転記
(assert-equal 3 (truncate 3.4))
(assert-equal (- 3) (truncate (- 3.9)))
;; p.80 (round 3.5) => 4 / (round 2.5) => 2 / (round -0.5) => 0 をそのまま転記
(assert-equal 4 (round 3.5))
(assert-equal 2 (round 2.5))
(assert-equal 0 (round (- 0.5)))

;; cf. p.80 (integerp 3) => t の例。(integerp 3.0) は同様の nil ケースを値を変えて追加。
(assert-equal t (integerp 3))
(assert-equal nil (integerp 3.0))

;; cf. p.81 div/mod の例(div 12 3 => 4 等)。値を変更。
(assert-equal 2 (div 13 5))
(assert-equal 3 (mod 13 5))
(assert-equal (- 3) (div (- 13) 5))
(assert-equal 2 (mod (- 13) 5))

;; cf. p.81-82 (gcd 12 5) => 1 / (lcm 2 3) => 6 の例。値を変更。
(assert-equal 4 (gcd 12 8))
(assert-equal 24 (lcm 12 8))
;; p.82 (isqrt 1000000000000002000000000000000) => 1000000000000000 をそのまま転記
(assert-equal 1000000000000000 (isqrt 1000000000000002000000000000000))

;; p.83 (characterp #\a) => t / (characterp "a") => nil をそのまま転記。
;; (characterp 'a) は同ページの説明(obj may be any ISLISP object)に基づく追加ケース。
(assert-equal t (characterp #\a))
(assert-equal nil (characterp "a"))
(assert-equal nil (characterp 'a))
;; p.84 (char= #\a #\a) => t / (char= #\a #\b) => nil / (char= #\a #\A) => nil をそのまま転記
(assert-equal t (char= #\a #\a))
(assert-equal nil (char= #\a #\b))
(assert-equal nil (char= #\a #\A))
;; p.84 (char< #\a #\b) => t / (char< #\b #\a) => nil をそのまま転記
(assert-equal t (char< #\a #\b))
(assert-equal nil (char< #\b #\a))
;; p.84 (char<= #\a #\a) => t / (char>= #\b #\a) => t をそのまま転記
(assert-equal t (char<= #\a #\a))
(assert-equal t (char>= #\b #\a))


;; p.85 (consp '(a . b)) => t / (consp '(a b c)) => t / (consp '()) => nil / (consp #(a b)) => nil
;; をそのまま転記
(assert-equal t (consp '(a . b)))
(assert-equal t (consp '(a b c)))
(assert-equal nil (consp '()))
(assert-equal nil (consp #(a b)))

;; p.85 (cons 'a '()) ... (cons '(a b) 'c) をそのまま転記
(assert-equal '(a) (cons 'a '()))
(assert-equal '((a) b c d) (cons '(a) '(b c d)))
(assert-equal '("a" b c) (cons "a" '(b c)))
(assert-equal '(a . 3) (cons 'a 3))
(assert-equal '((a b) . c) (cons '(a b) 'c))

;; p.85-86 (car '((a) b c d)) => (a) / (car '(1 . 2)) => 1 をそのまま転記
(assert-equal '(a) (car '((a) b c d)))
(assert-equal 1 (car '(1 . 2)))
;; p.86 (cdr '((a) b c d)) => (b c d) / (cdr '(1 . 2)) => 2 をそのまま転記
(assert-equal '(b c d) (cdr '((a) b c d)))
(assert-equal 2 (cdr '(1 . 2)))

;; p.86 (setf (car x) 'banana) の例をそのまま転記(x を isiki-test-lst に変更)
(assert-equal '((banana orange) apple banana (banana orange) banana)
  (let ((isiki-test-lst (list 'apple 'orange)))
    (list isiki-test-lst (car isiki-test-lst)
          (setf (car isiki-test-lst) 'banana)
          isiki-test-lst (car isiki-test-lst))))

;; p.86 (setf (cdr x) 'banana) の例をそのまま転記(x を isiki-test-lst2 に変更)
(assert-equal '((apple . banana) (orange) banana (apple . banana) banana)
  (let ((isiki-test-lst2 (list 'apple 'orange)))
    (list isiki-test-lst2 (cdr isiki-test-lst2)
          (setf (cdr isiki-test-lst2) 'banana)
          isiki-test-lst2 (cdr isiki-test-lst2))))

;; p.87 (null '(a b c)) => nil / (null '()) => t / (null (list)) => t をそのまま転記
(assert-equal nil (null '(a b c)))
(assert-equal t (null '()))
(assert-equal t (null (list)))

;; p.87 (listp '(a b c)) => t / (listp '()) => t / (listp '(a . b)) => t /
;; (listp "abc") => nil / (listp #(1 2)) => nil / (listp 'jerome) => nil をそのまま転記
;; (循環リストを使う中間の例は省略)
(assert-equal t (listp '(a b c)))
(assert-equal t (listp '()))
(assert-equal t (listp '(a . b)))
(assert-equal nil (listp "abc"))
(assert-equal nil (listp #(1 2)))
(assert-equal nil (listp 'jerome))

;; p.87-88 (create-list 3 17) => (17 17 17) / (create-list 2 #\a) => (#\a #\a) をそのまま転記
(assert-equal '(17 17 17) (create-list 3 17))
(assert-equal '(#\a #\a) (create-list 2 #\a))

;; p.88 (list 'a (+ 3 4) 'c) => (a 7 c) / (list) => nil をそのまま転記
(assert-equal '(a 7 c) (list 'a (+ 3 4) 'c))
(assert-equal nil (list))

;; p.88 (reverse '(a b c d e)) => (e d c b a) / (reverse '(a)) => (a) / (reverse '()) => ()
;; をそのまま転記
(assert-equal '(e d c b a) (reverse '(a b c d e)))
(assert-equal '(a) (reverse '(a)))
(assert-equal nil (reverse '()))

;; p.88-89 (append '(a b c) '(d e f)) => (a b c d e f) をそのまま転記
(assert-equal '(a b c d e f) (append '(a b c) '(d e f)))

;; p.89 (member 'c '(a b c d e f)) => (c d e f) / (member 'g ...) => nil /
;; (member 'c '(a b c a b c)) => (c a b c) をそのまま転記
(assert-equal '(c d e f) (member 'c '(a b c d e f)))
(assert-equal nil (member 'g '(a b c d e f)))
(assert-equal '(c a b c) (member 'c '(a b c a b c)))

;; p.90 (mapcar #'car '((1 a) (2 b) (3 c))) => (1 2 3) をそのまま転記
(assert-equal '(1 2 3) (mapcar (function car) '((1 a) (2 b) (3 c))))
;; p.90 (mapcar #'abs '(3 -4 2 -5 -6)) => (3 4 2 5 6) をそのまま転記
(assert-equal '(3 4 2 5 6) (mapcar (function abs) '(3 -4 2 -5 -6)))
;; p.90 (mapcar #'cons '(a b c) '(1 2 3)) => ((a . 1) (b . 2) (c . 3)) をそのまま転記
(assert-equal '((a . 1) (b . 2) (c . 3))
  (mapcar (function cons) '(a b c) '(1 2 3)))
;; p.90 (let ((x 0)) (mapc (lambda (v) (setq x (+ x v))) '(3 5)) x) => 8 をそのまま転記
;; (x を isiki-test-sum に変更)
(assert-equal 8 (let ((isiki-test-sum 0))
                  (mapc (lambda (v) (setq isiki-test-sum (+ isiki-test-sum v))) '(3 5))
                  isiki-test-sum))
;; p.90 (maplist (lambda (x) (cons 'foo x)) '(a b c d)) =>
;; ((foo a b c d) (foo b c d) (foo c d) (foo d)) をそのまま転記
(assert-equal '((foo a b c d) (foo b c d) (foo c d) (foo d))
  (maplist (lambda (x) (cons 'foo x)) '(a b c d)))
;; p.90 (let ((k 0)) (mapl (lambda (x) (setq k (+ k (if (member (car x) (cdr x)) 0 1))))
;; '(a b a c d b c)) k) => 4 をそのまま転記(k を isiki-test-k に変更)
(assert-equal 4
  (let ((isiki-test-k 0))
    (mapl (lambda (x) (setq isiki-test-k (+ isiki-test-k (if (member (car x) (cdr x)) 0 1))))
          '(a b a c d b c))
    isiki-test-k))
;; p.90-91 (mapcan (lambda (x) (if (> x 0) (list x))) '(-3 4 0 5 -2 7)) => (4 5 7) をそのまま転記
(assert-equal '(4 5 7) (mapcan (lambda (x) (if (> x 0) (list x))) '(-3 4 0 5 -2 7)))

;; p.91 (assoc 'a '((a . 1) (b . 2))) => (a . 1) / (assoc 'a '((a . 1) (a . 2))) => (a . 1) /
;; (assoc 'c '((a . 1) (b . 2))) => nil をそのまま転記
(assert-equal '(a . 1) (assoc 'a '((a . 1) (b . 2))))
(assert-equal '(a . 1) (assoc 'a '((a . 1) (a . 2))))
(assert-equal nil (assoc 'c '((a . 1) (b . 2))))


;; cf. p.93 (mapcar (lambda (x) (list (basic-array-p x) (basic-array*-p x) (general-array*-p x)))
;; '((a b c) "abc" #(a b c) #1a(a b c) #2a((a) (b) (c)))) の例。このカーネルのリーダーは
;; #nA(...) 配列リテラル構文を持たないため、#1a(a b c) は (make-array 3)(1次元配列。
;; 外延はgeneral-vectorと同じ)、#2a((a) (b) (c)) は (make-array '(3 1))(2次元配列)に
;; それぞれ置き換える。期待値の内側にnilを複数含むリストを組み立てる際、
;; '(...)直書きだと既知の理由(このファイル冒頭の運用ルール参照)で構造比較が
;; 不安定になることを避けるため、listで組み立てる。
(assert-equal
  (list (list nil nil nil) (list t nil nil) (list t nil nil) (list t nil nil) (list t t t))
  (mapcar (lambda (x)
            (list (basic-array-p x) (basic-array*-p x) (general-array*-p x)))
          (list '(a b c) "abc" (vector 'a 'b 'c) (make-array 3) (make-array '(3 1)))))


;; cf. p.95 (array-dimensions (create-array '(2 2) 0)) => (2 2) /
;; (array-dimensions (vector 'a 'b)) => (2) / (array-dimensions "foo") => (3) の例。
;; 仕様のcreate-arrayはこのカーネルのmake-arrayに置き換える。
(assert-equal '(2 2) (array-dimensions (make-array '(2 2))))
(assert-equal '(2) (array-dimensions (vector 'a 'b)))
(assert-equal '(3) (array-dimensions "foo"))

;; cf. p.95 (mapcar (lambda (x) (list (basic-vector-p x) (general-vector-p x))) ...) の例。
;; リスト・文字列・ベクタ・1次元配列(make-array 3、#1a(a b c)相当)の4項目を使う簡略版。
(assert-equal
  (list (list nil nil) (list t nil) (list t t) (list t t))
  (mapcar (lambda (x) (list (basic-vector-p x) (general-vector-p x)))
          (list '(a b c) "abc" (vector 'a 'b 'c) (make-array 3))))


;; cf. p.94 (aref ...) と p.96 (create-vector 3 17) => #(17 17 17) を組み合わせた例。
(assert-equal 0 (aref (create-vector 3 0) 0))
;; cf. p.94 aref と p.96 (vector 'a 'b 'c) => #(a b c) を組み合わせた例。値を変更。
(assert-equal 5 (aref (vector 1 2 5) 2))


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


;; cf. p.100 (elt "abc" 0) => #\a の例。添字を変更(2 => #\c)。
(assert-equal #\c (elt "abc" 2))


;; p.101 subseq の例をそのまま転記
(assert-equal "bcd" (subseq "abcdef" 1 4))
(assert-equal '(b c d) (subseq '(a b c d e f) 1 4))
(assert-equal #(b c d) (subseq (vector 'a 'b 'c 'd 'e 'f) 1 4))

;; p.101-102 map-into の例をそのまま転記(識別子a/b/kは衝突回避のため
;; isiki-test-mi-a/-b/-kに変更)。3つ目の結果はドット対を含むが、このカーネルの
;; リーダーはドット対リテラルの構文をサポートしないため、期待値はconsで組み立てる。
(defglobal isiki-test-mi-a (list 1 2 3 4))
(defglobal isiki-test-mi-b (list 10 10 10 10))
(assert-equal '(11 12 13 14) (map-into isiki-test-mi-a (function +) isiki-test-mi-a isiki-test-mi-b))
(assert-equal '(11 12 13 14) isiki-test-mi-a)
(assert-equal '(10 10 10 10) isiki-test-mi-b)
(defglobal isiki-test-mi-k '(one two three))
(assert-equal
  (list (cons 'one 11) (cons 'two 12) (cons 'three 13) 14)
  (map-into isiki-test-mi-a (function cons) isiki-test-mi-k isiki-test-mi-a))
(assert-equal '(2 4 6 8)
  (let ((isiki-test-mi-x 0))
    (map-into isiki-test-mi-a (lambda () (setq isiki-test-mi-x (+ isiki-test-mi-x 2))))))
(assert-equal '(2 4 6 8) isiki-test-mi-a)


;; cf. p.102-103 streamp/input-stream-p/output-stream-p の例。実ファイルの代わりに
;; with-standard-input/with-standard-outputで文字列ストリームを束縛して確認する版。
(assert-equal t
  (with-standard-input (create-string-input-stream "abc")
    (streamp (standard-input))))
(assert-equal t
  (with-standard-input (create-string-input-stream "abc")
    (input-stream-p (standard-input))))
(assert-equal nil
  (with-standard-input (create-string-input-stream "abc")
    (output-stream-p (standard-input))))
(assert-equal t
  (with-standard-output (create-string-output-stream)
    (output-stream-p (standard-output))))

;; cf. p.103 (read)が動的に束縛されたstandard-inputから読むことを確認する例。
;; このカーネルのreadは引数省略時に暗黙でstandard-inputへフォールバックしないため
;; (read (standard-input)) と明示的に呼ぶ形へ変更するが、with-standard-inputが
;; (standard-input) の返り値を正しく束縛替えすることは変わらず確認できる。
(assert-equal 1
  (with-standard-input (create-string-input-stream "1 2 3")
    (read (standard-input))))


;; cf. p.105 の例。test/c/script_test.c のos_virtio9p_*スタブは常に失敗を返すため、
;; hostビルド(make test)では実ファイルI/Oを伴うこの節は動的に検証できない。
;; 確認済みのプリミティブ署名(src/c/stream_lisp.c)に基づき記述するが、実機
;; (QEMU + 9p)での動作確認が別途必要であることをユーザーへ報告する。
(assert-equal t
  (with-open-output-file (isiki-test-outf "isiki-test-p112.txt")
    (format isiki-test-outf "hello")
    (finish-output isiki-test-outf)
    t))
(assert-equal "hello"
  (with-open-input-file (isiki-test-inf "isiki-test-p112.txt")
    (read-line isiki-test-inf)))


;; p.106 create-string-output-stream / get-output-stream-string の例をそのまま転記
(assert-equal "foo"
  (let ((isiki-test-cos-s (create-string-output-stream)))
    (format isiki-test-cos-s "foo")
    (get-output-stream-string isiki-test-cos-s)))

;; p.107 create-string-input-stream / read の例。1文字目は"hello #(1 2 3) 123 #\A"の
;; 先頭語をreadで4回読み、5回目のeos-value指定版((read str nil "the end")相当)は
;; このカーネルのreadがオプション引数(eos-error-p/eos-value)を受け付けないため省略する。
;; なお文字リテラル#\Aを埋め込むために単一のバックスラッシュを直接文字列内に書く
;; 必要がある(このカーネルのリーダーは文字列中のエスケープシーケンスを扱わないため)。
(defglobal isiki-test-cis-s (create-string-input-stream "hello #(1 2 3) 123 #\A"))
(assert-equal 'hello (read isiki-test-cis-s))
(assert-equal #(1 2 3) (read isiki-test-cis-s))
(assert-equal 123 (read isiki-test-cis-s))
(assert-equal #\A (read isiki-test-cis-s))


;; cf. p.108 read-char の例。3回目の呼び出し(仕様は「エラーがsignalされる」と
;; 明記)は、このカーネルのread-charがEOF時にエラーではなくnilを返す仕様のため、
;; その挙動を確認する形に変更して残す。
(defglobal isiki-test-rc-s (create-string-input-stream "ab"))
(assert-equal #\a (read-char isiki-test-rc-s))
(assert-equal #\b (read-char isiki-test-rc-s))
(assert-equal nil (read-char isiki-test-rc-s))


;; cf. p.109 read-line の例をそのまま転記(入力元を実ファイルから文字列ストリームに
;; 変更)。stream-ready-pは非同期I/Oが無く読み込みが常に同期的にブロックするため、
;; 常にtを返すスタブ実装であることを確認する形に変更する。
(defglobal isiki-test-rl-s (create-string-input-stream "line1
line2"))
(assert-equal "line1" (read-line isiki-test-rl-s))
(assert-equal "line2" (read-line isiki-test-rl-s))
(assert-equal t (stream-ready-p isiki-test-rl-s))


;; p.111 format の主要ディレクティブの例をそのまま転記。~S は文字型引数に対する
;; escaped印字が#\接頭辞を付けない既知の未実装(src/c/print.cのTAG_CHARケースが
;; escapedフラグを無視する)を避けるため、数値引数のみの例に簡略化する。
(assert-equal "The result is 42."
  (let ((isiki-test-fmt-s (create-string-output-stream)))
    (format isiki-test-fmt-s "The result is ~S." 42)
    (get-output-stream-string isiki-test-fmt-s)))
(assert-equal "5"
  (let ((isiki-test-fmt-s (create-string-output-stream)))
    (format isiki-test-fmt-s "~D" 5)
    (get-output-stream-string isiki-test-fmt-s)))
(assert-equal "101"
  (let ((isiki-test-fmt-s (create-string-output-stream)))
    (format isiki-test-fmt-s "~B" 5)
    (get-output-stream-string isiki-test-fmt-s)))
(assert-equal "1F"
  (let ((isiki-test-fmt-s (create-string-output-stream)))
    (format isiki-test-fmt-s "~X" 31)
    (get-output-stream-string isiki-test-fmt-s)))

;; cf. p.111 ~T(タブ)の例。仕様の複数行の所得税表そのままの再現は、複数回の
;; formatにわたる正確な列位置追跡の一致確認が煩雑になるため、最小限の自己完結した
;; 例に簡略化する。
(assert-equal "ab   cd"
  (let ((isiki-test-fmt-t-s (create-string-output-stream)))
    (format isiki-test-fmt-t-s "ab")
    (format isiki-test-fmt-t-s "~5Tcd")
    (get-output-stream-string isiki-test-fmt-t-s)))


;; cf. p.113 の例。p.105と同様の理由(hostビルドでは実ファイルI/Oが常に失敗する
;; スタブのため)で動的に検証できない。確認済みのプリミティブ署名に基づき記述するが、
;; 実機(QEMU + 9p)での動作確認が別途必要であることをユーザーへ報告する。
(assert-equal nil (probe-file "isiki-test-p120-nonexistent.txt"))
(with-open-output-file (isiki-test-p120-f "isiki-test-p120.txt")
  (format isiki-test-p120-f "x"))
(assert-equal t (probe-file "isiki-test-p120.txt"))

;; p.115-117 (§29 Conditions) には with-handler/signal-condition/error/ignore-errors の
;; 構文・意味の説明と、simple-error を自分で create する例(p.117)はあるが、以下のような
;; block/return-from を使う具体的なハンドラ制御の "Example:" ブロックは存在しない。
;; 以下は §29.2 の説明(ハンドラは signal-condition の呼び出し元の外へ制御を移す、
;; ハンドラは自分の受け取った condition に対して signal-condition を再度呼ぶことで
;; 外側のハンドラに委譲できる、continuable な signal-condition はハンドラが通常に
;; returnした値がそのまま結果になる、等)に基づき独自に作成した例。

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
;; 内側のハンドラが型不一致なら自分でsignal-conditionし、外側のハンドラに渡す
(assert-equal 'outer
  (block isiki-test-b2
    (with-handler (lambda (c) (return-from isiki-test-b2 'outer))
      (with-handler (lambda (c) (if (typep c '<simple-error>) (signal-condition c nil) (return-from isiki-test-b2 'inner)))
        (error "boom")))))

;; cf. p.116 signal-condition の continuable 引数の説明に基づく独自の例
;; continuableなsignal-conditionは、ハンドラが脱出せず返した値がそのまま結果になる
(assert-equal 101
  (+ 1
     (with-handler (lambda (c) 100)
       (signal-condition (make-instance '<condition>) t))))


;; cf. p.122 (identity '(a b c)) の例。値を変更。
(assert-equal 5 (identity 5))
(assert-equal 'a (identity 'a))

;;; --- 最終レポート ---

(isiki-test-report)
(close *isiki-test-stream*)
