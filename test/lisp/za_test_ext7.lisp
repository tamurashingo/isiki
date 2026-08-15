;; test/lisp/za_test_ext7.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張7(quoteシンボル
;; リテラル対応、ILOS/クラス・generic function向け)を検証するテスト。
;; 元は test/lisp/za_test.lisp の一部ではなく新規に追加した機能なので、
;; za_test_ext5.lispと同様に独立ファイルとして切り出す。za.cのグラマー全体・
;; za_test.lisp(拡張0〜4)については test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。
;; za_test.lisp(拡張0〜4)・za_test_ext5.lispへの依存は無い(自己完結)。init.lisp
;; (ILOS実装含む)はboot-entryスクリプトがtest_framework.lispより前にloadしている
;; 前提で、その中で定義されるdefclass/make-instance/slot-value/slot-value等を使う。
;;
;; 追記: quoteシンボル対応と同じ「ILOSをJIT対象に近づける」目的の後続の増分として、
;; &restパラメータの値参照(za_classify_operand/za_emit_operandのis_literal==3)も
;; 本ファイルに追加している。defgenericが生成するdispatch関数
;; `(defun name (&rest %generic-args) (%generic-call 'name %generic-args))`が
;; これによって初めてJIT対象になる。letの直接lambda呼び出しフォールバックは
;; 別課題として今回は対象外(make-instance/slot-value/set-slot-valueは依然
;; インタプリタ実行のまま)。

;;; --- 拡張7: quoteシンボルリテラル ---
;;
;; za_classify_operand(leaf operand)はこれまでfixnum即値とparams参照しか認識せず、
;; (quote sym)は一切コンパイル対象にならなかった(該当defun全体がインタプリタへ
;; fallbackしていた)。ILOSの slot-value 等はエラー時に'eval-errorのようなquote
;; シンボルを返すため、この対応が無いとILOSを使うdefunがほぼJITコンパイルされない。

;; init.lispのslot-value/set-slot-valueは、エラー分岐で'eval-errorをquoteしている
;; ものの、本体が`let`(=即時lambda呼び出し、za_test.lisp 160-166行目の
;; isiki-za-test-let-fallbackと同じ理由で拡張3/4完了後も非対応のまま)を使っているため、
;; quote対応の有無に関わらずコンパイル対象外のまま(結果自体はインタプリタ経由で正しい)。
(assert-equal nil (%%za-compiled-p (function slot-value)))
(assert-equal nil (%%za-compiled-p (function set-slot-value)))

;; quoteシンボルのコアな機構をILOSと無関係に確認する: eqの第2オペランド位置での
;; quoteシンボル比較(za_classify_operandはeqのオペランドにも共有される)。
(defun isiki-za-test-quote-eq (x) (eq x 'isiki-za-test-quote-target))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-eq)))
(assert-equal t (isiki-za-test-quote-eq 'isiki-za-test-quote-target))
(assert-equal nil (isiki-za-test-quote-eq 'isiki-za-test-other-symbol))

;; 一般呼び出しの引数位置でのquoteシンボル(ILOSに近い使い方): (cons 'sym x)。
(defun isiki-za-test-quote-cons (x) (cons 'isiki-za-test-quote-target x))
(assert-equal t (%%za-compiled-p (function isiki-za-test-quote-cons)))
(assert-equal (cons 'isiki-za-test-quote-target 42) (isiki-za-test-quote-cons 42))

;;; --- &rest引数の値参照 ---
;;
;; za_classify_operand/za_emit_operandに&restパラメータそのものへの参照
;; (is_literal==3)を追加した。値は元のevaluated_argsのうち固定引数分を消費した
;; 後に残る部分リストそのもの(新しいリストを作らない、インタプリタのbind_params
;; と同じ挙動)。固定引数が0個(&restのみ)の場合もcdrループが0回になるだけで
;; 正しく動くことを確認する。

(defun isiki-za-test-rest-only (&rest args) args)
(assert-equal t (%%za-compiled-p (function isiki-za-test-rest-only)))
(assert-equal nil (isiki-za-test-rest-only))
(assert-equal (list 1 2 3) (isiki-za-test-rest-only 1 2 3))

;; init.lispの`list`自身(`(defun list (&rest items) items)`)も同じ形なので、
;; 合成テスト関数だけでなく実在のライブラリ関数がJIT対象になる実例として確認する。
(assert-equal t (%%za-compiled-p (function list)))

;; 固定引数+&restの併用。fixed_count分のcdrを済ませた残りがrestに束縛される。
(defun isiki-za-test-rest-mixed (a &rest args) args)
(assert-equal t (%%za-compiled-p (function isiki-za-test-rest-mixed)))
(assert-equal nil (isiki-za-test-rest-mixed 1))
(assert-equal (list 2 3) (isiki-za-test-rest-mixed 1 2 3))

;; rest値そのものをcar/cons/eq等の既存leaf共有経路に渡す(一般呼び出しの引数位置
;; でも使えることの確認)。
(defun isiki-za-test-rest-first (&rest args) (car args))
(assert-equal t (%%za-compiled-p (function isiki-za-test-rest-first)))
(assert-equal 10 (isiki-za-test-rest-first 10 20))
(assert-equal nil (isiki-za-test-rest-first))

;; defgenericが生成するdispatch関数`(defun name (&rest %generic-args) ...)`が
;; 今回の対応でJIT対象になることの確認(initialize-objectはinit.lispがdefgenericで
;; 定義し、defmethodは%register-methodへの登録のみなのでこの関数自体は上書きされない)。
(assert-equal t (%%za-compiled-p (function initialize-object)))

;;; --- ILOSを実際に使う統合テスト ---
;;
;; defclass自体はトップレベルのマクロ展開なのでza.cの対象外(JITはdefun本体のみ)。
;; make-instanceは&rest initargsを持つがボディが`let*`(即時lambda呼び出し、
;; za_test.lisp 160-166行目のisiki-za-test-let-fallbackと同じ理由で拡張3/4完了後も
;; 非対応のまま)を使っているため、上記の&rest対応の有無に関わらずインタプリタ実行
;; のまま(letフォールバックは別課題として今回は対象外)。今回JIT化の到達点になるのは、
;; quoteシンボルのクラス名・initarg・スロット名を使ってmake-instance/slot-valueを
;; 呼び出す「側」の、letを使わないユーザー定義関数がコンパイル対象になること。
(assert-equal nil (%%za-compiled-p (function make-instance)))

(defclass isiki-za-test-point ()
  ((x :initarg :x :initform (lambda () 0))
   (y :initarg :y :initform (lambda () 0))))

(defun isiki-za-test-make-point (px py)
  (make-instance 'isiki-za-test-point ':x px ':y py))

(defun isiki-za-test-point-x (p) (slot-value p 'x))
(defun isiki-za-test-point-y (p) (slot-value p 'y))

(assert-equal t (%%za-compiled-p (function isiki-za-test-make-point)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-point-x)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-point-y)))

(defglobal *isiki-za-test-point* (isiki-za-test-make-point 3 4))
(assert-equal 3 (isiki-za-test-point-x *isiki-za-test-point*))
(assert-equal 4 (isiki-za-test-point-y *isiki-za-test-point*))
