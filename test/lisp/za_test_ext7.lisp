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
;; これによって初めてJIT対象になる。make-instance/slot-value/set-slot-valueは
;; 拡張7執筆時点ではインタプリタ実行のままだったが、拡張15(引数位置での一般呼び出し
;; ネスト対応)以降はコンパイル対象になった(詳細は後述の該当assert-equal付近の
;; コメント参照)。ただしset-slot-valueはその後M14(#29)でinit_aot.lispへ移動し、
;; za.c JITではなくAOTトランスパイラの対象になったため、現在は%%za-compiled-p が
;; nilを返す(詳細は同assert-equal付近のコメント参照)。

;;; --- 拡張7: quoteシンボルリテラル ---
;;
;; za_classify_operand(leaf operand)はこれまでfixnum即値とparams参照しか認識せず、
;; (quote sym)は一切コンパイル対象にならなかった(該当defun全体がインタプリタへ
;; fallbackしていた)。ILOSの slot-value 等はエラー時に'eval-errorのようなquote
;; シンボルを返すため、この対応が無いとILOSを使うdefunがほぼJITコンパイルされない。

;; init.lispのslot-value/set-slot-valueは、エラー分岐で'eval-errorをquoteしている。
;; 本体の`let`自体はza_compile_letが対応済みだが、そのinit式`(%slot-index slot-name
;; (%%class-slots (%%instance-class instance)) 0)`の引数位置に別の一般呼び出し
;; (%%class-slots/%%instance-class)がネストしており、これが拡張15以前は
;; allow_callゲートで弾かれてコンパイル断念していた実際の原因だった。拡張15で
;; 引数位置の一般呼び出しネストに対応したため、現在はコンパイルされる
;; (下記のisiki-za-test-point-x/y経由の実行結果でも正しいことを確認する)。
(assert-equal t (%%za-compiled-p (function slot-value)))
;; set-slot-valueはM14(#29、setf)でinit.lispのdefunからinit_aot.lispのAOT
;; トランスパイル対象へ移動した(src/lisp/transpile.lisp)。ビルド時にC関数へ直接
;; 変換され、za.cのJIT(machine語へのランタイムコンパイル)を経由しないネイティブ
;; 関数として起動時に登録されるため、%%za-compiled-p(za.cが機械語化したかどうかの
;; 判定)はnilになる——インタプリタへfallbackしているわけではなく、常時コンパイル
;; 済みという点ではza.c JITより上位の状態にある。
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

;; init.lispの`list`自身(`(defun list (&rest items) items)`)も同じ形だったため、
;; 元々は合成テスト関数だけでなく実在のライブラリ関数がJIT対象になる実例として
;; ここで確認していた。M14(#29、list)でinit_aot.lispのAOTトランスパイル対象へ
;; 移動したため、set-slot-valueと同じ理由でza.c JITを経由しない(%%za-compiled-p
;; はnilになる)。&restパラメータの値参照自体のJIT対応は直上のisiki-za-test-rest-*
;; 系(合成テスト関数、init.lispに残置)で引き続き検証できている。
(assert-equal nil (%%za-compiled-p (function list)))

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
;; make-instanceのボディの`let*`自体はza_compile_letが対応済みだが、instance束縛の
;; init式`(%%make-instance-raw class (make-array (length (%%class-slots class))))`は
;; 引数位置に一般呼び出しが3段ネストしており、これも拡張15以前はallow_callゲートで
;; 弾かれてコンパイル断念していた(slot-value/set-slot-valueと同じ理由)。拡張15後は
;; コンパイルされる(下記のisiki-za-test-make-point経由の実行結果でも正しいことを
;; 確認する)。
(assert-equal t (%%za-compiled-p (function make-instance)))

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
