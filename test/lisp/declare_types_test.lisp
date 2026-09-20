;; test/lisp/declare_types_test.lisp
;;
;; declare による型宣言 (Phase 4a-2) の回帰テスト。documents/declare-types.md
;;
;; **本 Phase では生成コードは変わらない。** 宣言が受け付けられ、コンパイラまで
;; 届いて記録されることだけを見る。型を使った最適化は後続の Phase。
;;
;; declaim と同じく、記録先(za_fn_meta_t)を持つのは JIT コンパイルされた関数
;; だけなので、%%DECLARED-TYPES-OF の確認は実機(make test-qemu)でしかできない。

;;; --- 5-1. 宣言を書いても JIT に乗ること(本作業の主目的) ---
;;
;; declare を body から剥がし忘れると za_is_excluded_special_form か
;; 「DECLARE という関数が無い」で外れる。ここが通れば配管はできている。
(defun dt-plain (x) (+ x 1))
(assert-equal t (%%za-compiled-p (function dt-plain)))
(assert-equal 0 (%%diag-za-bail-at 0))

(defun dt-f (x) (declare (type <fixnum> x)) (+ x 1))
(assert-equal t (%%za-compiled-p (function dt-f)))
(assert-equal 0 (%%diag-za-bail-at 0))            ; 断念していない

;;; --- 5-2. 宣言の有無で挙動が変わらないこと ---
(defun dt-f1 (x) (+ x 1))
(defun dt-f2 (x) (declare (type <fixnum> x)) (+ x 1))
(assert-equal (dt-f1 1) (dt-f2 1))
(assert-equal (dt-f1 100) (dt-f2 100))

;; 宣言が嘘でも正しく動くこと。最適化していないので型ガードはそのまま残っている。
;; **最適化を入れた後はここが変わる**(それが Phase 4 の次段階の意味である)。
(defun dt-f3 (x) (declare (type <fixnum> x)) (+ x 1))
(assert-equal 2.5d0 (dt-f3 1.5d0))
(assert-equal 2.5f0 (dt-f3 1.5f0))
(assert-equal t (integerp (dt-f3 1)))

;;; --- 5-3. 内省 ---
;;
;; %%DECLARED-TYPES-OF は **仮引数の位置順のリスト**を返す。記録先の
;; za_fn_meta_t は Immobilized Space 上で GC のルート走査の対象外のため、
;; 変数名(シンボル)を置けない(documents/declare-types.md §4)。
(defun dt-add-integer (x y) (declare (type <fixnum> x y)) (+ x y))
(assert-equal 2 (%%diag-za-param-decls))          ; コンパイラ本体へ2件届いた
(assert-equal '(<FIXNUM> <FIXNUM>) (%%declared-types-of 'dt-add-integer))
(assert-equal 3 (dt-add-integer 1 2))

;; 関数オブジェクトを直接渡しても引ける(%%OPTIMIZE-OF と同じ)
(assert-equal '(<FIXNUM> <FIXNUM>) (%%declared-types-of (function dt-add-integer)))

;; 宣言が無い関数は nil
(assert-equal nil (%%declared-types-of 'dt-f1))

;; 複数の declare が合成されること
(defun dt-two-decls (x y)
  (declare (type <fixnum> x))
  (declare (type <double-float> y))
  (+ x y))
(assert-equal '(<FIXNUM> <DOUBLE-FLOAT>) (%%declared-types-of 'dt-two-decls))
(assert-equal t (%%za-compiled-p (function dt-two-decls)))
(assert-equal 3.5d0 (dt-two-decls 1 2.5d0))

;; 1つの declare に複数の宣言指定子を書ける
(defun dt-two-specs (x y)
  (declare (type <fixnum> x) (type <single-float> y))
  (+ x y))
(assert-equal '(<FIXNUM> <SINGLE-FLOAT>) (%%declared-types-of 'dt-two-specs))

;; 宣言されていない位置は nil になり、末尾の nil は落ちる
(defun dt-second-only (x y) (declare (type <fixnum> y)) (+ x y))
(assert-equal '(nil <FIXNUM>) (%%declared-types-of 'dt-second-only))
(defun dt-first-only (x y) (declare (type <fixnum> x)) (+ x y))
(assert-equal '(<FIXNUM>) (%%declared-types-of 'dt-first-only))

;;; --- 5-4. 型名 ---
;;
;; 実在のクラスであること。<fixnum> 等は PR #79 で *classes* に登録されている。
(defun dt-t-integer (x) (declare (type <integer> x)) x)
(assert-equal '(<INTEGER>) (%%declared-types-of 'dt-t-integer))
(defun dt-t-number (x) (declare (type <number> x)) x)
(assert-equal '(<NUMBER>) (%%declared-types-of 'dt-t-number))
(defun dt-t-cons (x) (declare (type <cons> x)) x)
(assert-equal '(<CONS>) (%%declared-types-of 'dt-t-cons))
(defun dt-t-string (x) (declare (type <string> x)) x)
(assert-equal '(<STRING>) (%%declared-types-of 'dt-t-string))

;; **別名が働くこと。** <short-float>/<long-float> は <single-float>/<double-float>
;; と同一のクラスオブジェクトに登録された別名であって、サブクラスではない。
;; 利用者が書いた名前ではなく、解決したクラスの**正準名**で符号化しているため、
;; 正準名のほうが記録される(%convert が case でシンボル一致を見ていたために
;; 別名が効かなかった件と同じ轍を踏まない。documents/float-default.md §4-2)。
(defun dt-short (x) (declare (type <short-float> x)) x)
(assert-equal '(<SINGLE-FLOAT>) (%%declared-types-of 'dt-short))
(defun dt-long (x) (declare (type <long-float> x)) x)
(assert-equal '(<DOUBLE-FLOAT>) (%%declared-types-of 'dt-long))

;; 未知の型名は**エラーにせず無視する**。ただし「無視された」ことは分かる。
(defun dt-unknown (x) (declare (type <nonexistent> x)) (+ x 1))
(assert-equal '(<UNKNOWN>) (%%declared-types-of 'dt-unknown))
(assert-equal t (%%za-compiled-p (function dt-unknown)))
(assert-equal 2 (dt-unknown 1))

;; 実在するが符号を持たないクラス(ユーザ定義クラス)は <OTHER> として記録される。
;; 「そもそもクラスが無い」(<UNKNOWN>)と区別できる。
(defclass <dt-user-class> () ())
(defun dt-other (x) (declare (type <dt-user-class> x)) x)
(assert-equal '(<OTHER>) (%%declared-types-of 'dt-other))

;; 宣言指定子が (type ...) でない場合は読み飛ばす(将来 inline 等を足せるように)
(defun dt-unknown-spec (x) (declare (ignore x)) 1)
(assert-equal nil (%%declared-types-of 'dt-unknown-spec))
(assert-equal t (%%za-compiled-p (function dt-unknown-spec)))

;;; --- 5-5. defun の外の declare ---
;;
;; **エラーにならず、黙って無視されること。** これが通らないと、トップレベルで
;; 試した人が最初にエラーを踏む。
(assert-equal nil (declare (type <fixnum> x)))
(assert-equal 2 (let ((x 1))
                  (declare (type <fixnum> x))
                  (+ x 1)))

;;; --- 5-6. defun の中の let ---
;;
;; let は ((lambda (v...) . body) init...) へ展開されるが、let マクロは body を
;; そのまま通すので declare は lambda 本体の先頭に残る。za_compile_let がそこで
;; 剥がして za_local_scope_t へ記録する。
(defun dt-let (n)
  (let ((x 1))
    (declare (type <fixnum> x))
    (+ x n)))
(assert-equal t (%%za-compiled-p (function dt-let)))        ; 宣言を書いても諦めない
(assert-equal 1 (%%diag-za-local-decls))          ; let 束縛変数に1件記録された
(assert-equal 0 (%%diag-za-param-decls))          ; 仮引数側には何も書いていない
(assert-equal 4 (dt-let 3))

;; let の中の宣言は関数オブジェクトには残らない(仮引数の記録とは別物)
(assert-equal nil (%%declared-types-of 'dt-let))

;; 複数束縛・複数宣言
(defun dt-let2 (n)
  (let ((a 1) (b 2))
    (declare (type <fixnum> a) (type <fixnum> b))
    (+ a b n)))
(assert-equal t (%%za-compiled-p (function dt-let2)))
(assert-equal 2 (%%diag-za-local-decls))
(assert-equal 6 (dt-let2 3))

;; 仮引数と let 束縛変数の両方に書ける
(defun dt-both (x)
  (declare (type <fixnum> x))
  (let ((y 1))
    (declare (type <fixnum> y))
    (+ x y)))
(assert-equal t (%%za-compiled-p (function dt-both)))
(assert-equal '(<FIXNUM>) (%%declared-types-of 'dt-both))
(assert-equal 1 (%%diag-za-local-decls))
(assert-equal 3 (dt-both 2))

;; **let* では declare は効かない。** let* は最内で (progn ,@body) へ展開されるため、
;; declare が let 本体の先頭ではなく progn の中に入り、za_compile_let の剥がし対象から
;; 外れる(documents/declare-types.md §5-6)。エラーにはならず、JIT も諦めない。
(defun dt-let-star (n)
  (let* ((a 1) (b (+ a 1)))
    (declare (type <fixnum> a))
    (+ a b n)))
(assert-equal t (%%za-compiled-p (function dt-let-star)))
(assert-equal 0 (%%diag-za-local-decls))          ; 記録されない(既知の制約)
(assert-equal 4 (dt-let-star 1))

;;; --- body 途中の declare(§3-5) ---
;;
;; 宣言としては扱わず、**通常の式として no-op(nil)になる**。エラーにもならず、
;; JIT も諦めない。
(defun dt-mid (x)
  (+ x 1)
  (declare (type <fixnum> x))
  (+ x 2))
(assert-equal t (%%za-compiled-p (function dt-mid)))
(assert-equal nil (%%declared-types-of 'dt-mid))  ; 先頭ではないので記録されない
(assert-equal 3 (dt-mid 1))

;;; --- body 複数式化(PR #71)との関係(§4-4) ---
;;
;; declare を剥がした結果 body が 1 式になる場合も 2 式以上残る場合も動くこと。
(defun dt-body1 (x) (declare (type <fixnum> x)) (+ x 1))
(assert-equal t (%%za-compiled-p (function dt-body1)))
(assert-equal 2 (dt-body1 1))

;; 剥がした結果 body が 2 式残る場合
(defun dt-body2 (x) (declare (type <fixnum> x)) (+ x 1) (+ x 2))
(assert-equal t (%%za-compiled-p (function dt-body2)))
(assert-equal 3 (dt-body2 1))
(assert-equal '(<FIXNUM>) (%%declared-types-of 'dt-body2))

;; **setq を含む関数は declare の有無に関わらず JIT に乗らない。**
;; 宣言を書いたせいでコンパイルされなくなったのではないことを、対照を置いて固定する
;; (これを対照無しで書いたために、一度「declare が JIT を壊した」と読み違えた)。
(defun dt-setq-plain (x) (setq x 1) x)
(defun dt-setq-decl (x) (declare (type <fixnum> x)) (setq x 1) x)
(assert-equal (%%za-compiled-p (function dt-setq-plain))
              (%%za-compiled-p (function dt-setq-decl)))
(assert-equal (dt-setq-plain 99) (dt-setq-decl 99))

;;; --- &rest との組み合わせ ---
;;
;; &rest 変数はリストなので型宣言の対象にしない。固定引数のぶんだけ記録される。
(defun dt-rest (x &rest r) (declare (type <fixnum> x)) (cons x r))
(assert-equal '(<FIXNUM>) (%%declared-types-of 'dt-rest))
(assert-equal '(1 2 3) (dt-rest 1 2 3))

;;; --- 再定義で宣言が消えること ---
(defun dt-redef (x) (declare (type <fixnum> x)) x)
(assert-equal '(<FIXNUM>) (%%declared-types-of 'dt-redef))
(defun dt-redef (x) x)
(assert-equal nil (%%declared-types-of 'dt-redef))
