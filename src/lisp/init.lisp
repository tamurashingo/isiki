;; a simple function to verify (load "src/lisp/init.lisp") works
(defun init-lisp-greeting ()
  "hello from init.lisp")

;;; ここから下は「Lisp側にて実装すべきもの」(documents/isiki-os.md参照)。
;;; C側のquote/if/progn/setq/defun/lambda/defmacro/quasiquote/block/return-from/
;;; unwind-protectのみを土台に、マクロとして実装する。
;;;
;;; 既知の制約: setq(os_set_variable)はcurrent environment自身にしか書き込まず、
;;; かつ関数呼び出し(MAGIC_FUNCTION_INTERPRETED)は呼び出しごとに新しいenvironmentを
;;; 作るため、「外側のletで宣言した変数を、再帰呼び出しの中からsetqで書き換えて
;;; 次のイテレーションに持ち越す」ことはできない。for/whileは、状態を持ち越したい
;;; 変数はループ関数自身の仮引数として明示的に引き渡す(名前付きletスタイルの再帰)
;;; ことで、この制約の範囲内で正しく動くように実装している。

;;; --- let / let* ---

(defun %let-vars (bindings)
  (if (null bindings)
      nil
      (cons (car (car bindings)) (%let-vars (cdr bindings)))))

(defun %let-inits (bindings)
  (if (null bindings)
      nil
      (cons (car (cdr (car bindings))) (%let-inits (cdr bindings)))))

;; (let ((v1 i1) (v2 i2) ...) body...) を、即時実行するlambdaに展開する。
;; lambda呼び出しは必ず新しいenvironmentを作るので、bodyの中でのsetqはこの
;; letの変数だけに(意図通り)閉じて働く。
(defmacro let (bindings &rest body)
  `((lambda ,(%let-vars bindings) ,@body) ,@(%let-inits bindings)))

;; let*は各bindingを1つずつletでネストすることで、後続のinit式から
;; 手前で束縛した変数を参照できる(逐次束縛)ようにする。
(defmacro let* (bindings &rest body)
  (if (null bindings)
      `(progn ,@body)
      `(let (,(car bindings))
         (let* ,(cdr bindings) ,@body))))

;;; --- and / or ---

(defmacro and (&rest forms)
  (if (null forms)
      t
      (if (null (cdr forms))
          (car forms)
          `(if ,(car forms) (and ,@(cdr forms)) nil))))

;; orは各formを2回評価しないよう、gensymで作った一時変数にletで束縛してから調べる。
(defmacro or (&rest forms)
  (if (null forms)
      nil
      (if (null (cdr forms))
          (car forms)
          (let ((temp (gensym)))
            `(let ((,temp ,(car forms)))
               (if ,temp ,temp (or ,@(cdr forms))))))))

;;; --- cond ---

;; 既知の簡略化: (cond (test)) のようにresult式を省略した書き方(test自身の値を
;; 返す)には対応しない。すべてのclauseにresult式を書くことを前提とする。
(defmacro cond (&rest clauses)
  (if (null clauses)
      nil
      `(if ,(car (car clauses))
           (progn ,@(cdr (car clauses)))
           (cond ,@(cdr clauses)))))

;;; --- case / case-using ---
;;;
;;; 既知の簡略化: caseはeqlで照合する仕様だが、この実装ではfixnum/symbol/characterは
;;; タグ付きimmediate値として表現されているため、eq(既存のmemberが使う比較)で
;;; eql相当になる。

;; keyがclauseのkeylist(t以外)のいずれかの要素とeqならそのclauseのbodyを、
;; そうでなければ残りのclausesを調べるif連鎖を組み立てる。tのclauseはdefaultとして扱う。
(defun %case-expand (key clauses)
  (if (null clauses)
      nil
      (if (eq (car (car clauses)) t)
          `(progn ,@(cdr (car clauses)))
          `(if (member ,key ',(car (car clauses)))
               (progn ,@(cdr (car clauses)))
               ,(%case-expand key (cdr clauses))))))

;; (case keyform (keylist form*)* (t form*)?) : keyformを1度だけ評価し、
;; 各clauseのkeylistの要素とeqで照合する(eql相当、上記の既知の簡略化参照)。
(defmacro case (keyform &rest clauses)
  (let ((key (gensym)))
    `(let ((,key ,keyform))
       ,(%case-expand key clauses))))

;; keylist中のいずれかのkについて(funcall pred key k)が真になるかを判定する。
(defun %case-using-match (pred key keylist)
  (if (null keylist)
      nil
      (if (funcall pred key (car keylist))
          t
          (%case-using-match pred key (cdr keylist)))))

;; %case-using-matchの呼び出しをif連鎖として組み立てる版(caseと違い、
;; 述語呼び出しはbody展開時ではなく実行時に行う必要があるためformを生成する)。
(defun %case-using-expand (pred key clauses)
  (if (null clauses)
      nil
      (if (eq (car (car clauses)) t)
          `(progn ,@(cdr (car clauses)))
          `(if (%case-using-match ,pred ,key ',(car (car clauses)))
               (progn ,@(cdr (car clauses)))
               ,(%case-using-expand pred key (cdr clauses))))))

;; (case-using predform keyform (keylist form*)* (t form*)?) : predformとkeyformを
;; それぞれ1度だけ評価し、各clauseのkeylistの要素kについて
;; (funcall predform keyform-value k)が真になるものを探す(引数順はkeyform-valueが先)。
(defmacro case-using (predform keyform &rest clauses)
  (let ((pred (gensym)) (key (gensym)))
    `(let ((,pred ,predform) (,key ,keyform))
       ,(%case-using-expand pred key clauses))))

;;; --- for / while ---

(defun %for-vars (bindings)
  (if (null bindings)
      nil
      (cons (car (car bindings)) (%for-vars (cdr bindings)))))

(defun %for-inits (bindings)
  (if (null bindings)
      nil
      (cons (car (cdr (car bindings))) (%for-inits (cdr bindings)))))

;; step式を省略したbinding (var init) は、次の値も現在値のまま(変化なし)とする。
(defun %for-next (binding)
  (if (null (cdr (cdr binding)))
      (car binding)
      (car (cdr (cdr binding)))))

(defun %for-nexts (bindings)
  (if (null bindings)
      nil
      (cons (%for-next (car bindings)) (%for-nexts (cdr bindings)))))

;; (for ((var1 init1 [step1]) ...) (test result...) body...) を、
;; var群を仮引数として引き渡しながら再帰する名前付きループ関数に展開する。
;; bodyの中のsetqはループ関数自身の仮引数(=var群)に対しては正しく働き、
;; 次のイテレーションの引数(step式)にもその変更が反映される。
;; block nilで囲むことで、body中の(return-from nil 値)による早期脱出もできる。
(defmacro for (bindings test-and-result &rest body)
  (let ((loop-name (gensym)))
    `(block nil
       (defun ,loop-name ,(%for-vars bindings)
         (if ,(car test-and-result)
             (progn ,@(cdr test-and-result))
             (progn
               ,@body
               (,loop-name ,@(%for-nexts bindings)))))
       (,loop-name ,@(%for-inits bindings)))))

;; whileは仮引数を持たないループ関数として展開する(状態を持ち越す変数の宣言が
;; 構文上ないため)。testやbody内でのsetqはループ関数自身のその1回の呼び出しの
;; 中だけで有効で、次のイテレーションには持ち越されない(上記の既知の制約)。
;; ループを終わらせるにはtestがnilになるか、body中でreturn-fromを使う。
(defmacro while (test &rest body)
  (let ((loop-name (gensym)))
    `(block nil
       (defun ,loop-name ()
         (if ,test
             (progn ,@body (,loop-name))
             nil))
       (,loop-name))))

;;; --- with-open-input-stream ---

;; (with-open-input-stream (var stream-form) body...) を、
;; letで束縛してunwind-protectでcloseする形に展開する。
;; bodyが途中でreturn-fromやエラーで抜けても、closeは必ず実行される。
(defmacro with-open-input-stream (binding &rest body)
  `(let ((,(car binding) ,(car (cdr binding))))
     (unwind-protect
         (progn ,@body)
       (close ,(car binding)))))

;;; --- with-open-input-file ---

;; (with-open-input-file (name filename [element-class]) body...) :
;; filenameをopen-input-stream(9P経由のファイルopen)で開き、nameに束縛してbodyを
;; 評価し、抜けたら必ずcloseする(with-open-input-streamそのまま利用)。
;; 既知の制約: element-classは仕様上評価はされるが、既存のOPEN-INPUT-STREAMが
;; 文字ストリーム固定(仕様の既定値<character>のみ相当)のため無視する。
(defmacro with-open-input-file (binding &rest body)
  `(with-open-input-stream (,(car binding) (open-input-stream ,(car (cdr binding))))
     ,@body))

;;; --- setf ---

;; (setf place value) を place の形に応じて setq/set-car/set-cdr/set-arefに展開する。
;; placeがsymbolならsetq、(car x)ならset-car、(cdr x)ならset-cdr、
;; (aref array i1 i2 ...)ならset-arefに展開する。
(defmacro setf (place value)
  (cond ((symbolp place) `(setq ,place ,value))
        ((eq (car place) 'car) `(set-car ,(car (cdr place)) ,value))
        ((eq (car place) 'cdr) `(set-cdr ,(car (cdr place)) ,value))
        ((eq (car place) 'aref) `(set-aref ,@(cdr place) ,value))
        ((eq (car place) 'slot-value) `(set-slot-value ,@(cdr place) ,value))))

;;; --- mapcar / mapc / mapcan ---
;;;
;;; いずれもfnを評価済みの値として受け取り、関数呼び出しの形はLisp2スコープの
;;; 制約で(f x)のようには書けない(fが変数に束縛された関数値の場合、それは
;;; 関数namespaceではなく変数namespaceにあるため)。そのため、funcall(eval.c側の
;;; 組み込み関数)を介してfnを呼び出す。単一のリストのみを受け取る簡略版とする。

;; listの各要素にfnを適用した結果を集めたリストを返す。
(defun mapcar (fn list)
  (if (null list)
      nil
      (cons (funcall fn (car list)) (mapcar fn (cdr list)))))

(defun %mapc-1 (fn list)
  (if (null list)
      nil
      (progn (funcall fn (car list)) (%mapc-1 fn (cdr list)))))

;; listの各要素にfnを副作用目的で適用し、list自身を返す。
(defun mapc (fn list)
  (progn (%mapc-1 fn list) list))

;; listの各要素にfnを適用した結果(リストであることを期待する)をappendで連結して返す。
(defun mapcan (fn list)
  (if (null list)
      nil
      (append (funcall fn (car list)) (mapcan fn (cdr list)))))

;;; --- member / assoc ---
;;;
;;; いずれも要素の比較にeqを使う(ISLispのeql相当。FIXNUMはタグ付きのまま
;;; immediate値として同一性比較できるため、eqで正しく比較できる)。

;; listの中からitemとeqな要素を探し、見つかった要素から始まる部分リストを返す。
;; 見つからなければnil。
(defun member (item list)
  (if (null list)
      nil
      (if (eq item (car list))
          list
          (member item (cdr list)))))

;; alist((key . value)のconsを並べたリスト)からkeyとeqなキーを持つ要素(cons)を探す。
;; 見つからなければnil。
(defun assoc (key alist)
  (if (null alist)
      nil
      (if (eq key (car (car alist)))
          (car alist)
          (assoc key (cdr alist)))))

;;; --- append / reverse ---
;;;
;;; ISLispの仕様上、appendはリストのみを対象とする(文字列・ベクトルは対象外)。
;;; reverseも同様にリストのみを対象とする簡略版とする。

(defun %append2 (list1 list2)
  (if (null list1)
      list2
      (cons (car list1) (%append2 (cdr list1) list2))))

(defun %append-lists (lists)
  (if (null lists)
      nil
      (if (null (cdr lists))
          (car lists)
          (%append2 (car lists) (%append-lists (cdr lists))))))

;; 0個以上のリストを連結して1つのリストにする。
(defun append (&rest lists)
  (%append-lists lists))

(defun %reverse-helper (list acc)
  (if (null list)
      acc
      (%reverse-helper (cdr list) (cons (car list) acc))))

;; listの要素順を反転した新しいリストを返す。
(defun reverse (list)
  (%reverse-helper list nil))

;;; --- list ---

;; 引数をそのまま並べたリストを返す。&restが評価済みの引数を既にリストとして
;; 束縛するため、bodyはitemsをそのまま返すだけでよい。
(defun list (&rest items)
  items)

;;; --- ILOS (最小実装): defclass / make-instance / slot-value / typep / subclassp ---
;;;
;;; 既知の簡略化: defgeneric/defmethod/call-next-methodは実装しない。スロットの
;;; 継承は単純な連結(同名オーバーライドやMRO計算は行わない)。typepはILOSの
;;; クラスインスタンスに対する判定のみで、組み込み型は対象外とする。

;; *classes*はdefun(%register-class)の内側から書き換える必要があるため、
;; defvar+setqではなくdefdynamic+%%set-dynamicを使う。setq(os_set_variable)は
;; current environment自身にしか書き込めず、関数呼び出しは呼び出しごとに新しい
;; environmentを作るため、defvar+setqでは%register-classの中でのsetqが
;; 呼び出し元に見えない(冒頭の既知の制約と同じ理由)。defdynamicはレキシカルな
;; 環境の親子関係と無関係なグローバルとして値を持つため、この制約を受けない。
(defdynamic *classes* nil)

(defun %find-class (name)
  (cdr (assoc name (dynamic *classes*))))

(defun %register-class (name class)
  (%%set-dynamic '*classes* (cons (cons name class) (dynamic *classes*)))
  class)

;; plistからkeyに対応する値を探す。見つからなければdefault
(defun %plist-get (plist key default)
  (if (null plist)
      default
      (if (eq (car plist) key)
          (car (cdr plist))
          (%plist-get (cdr (cdr plist)) key default))))

;; (slot :initarg :key :initform expr) を、初期値をfuncallで取り出せる
;; thunk(引数無しlambda)付きの評価済みフォーム(list 'slot ':key (lambda () expr))
;; に変換する。defclassのマクロ展開の中で使うため、ここではフォームを組み立てるだけ
(defun %slot-spec-form (spec)
  (list 'list (list 'quote (car spec))
              (list 'quote (%plist-get (cdr spec) ':initarg nil))
              (list 'lambda nil (%plist-get (cdr spec) ':initform nil))))

(defun %slot-spec-forms (specs)
  (if (null specs)
      nil
      (cons (%slot-spec-form (car specs)) (%slot-spec-forms (cdr specs)))))

(defun %defclass-supers (super-names)
  (if (null super-names)
      nil
      (cons (%find-class (car super-names)) (%defclass-supers (cdr super-names)))))

(defun %merge-superclass-slots (supers)
  (if (null supers)
      nil
      (append (%%class-slots (car supers)) (%merge-superclass-slots (cdr supers)))))

;; (defclass name (super...) ((slot :initarg :key :initform expr) ...) options...)
;; supers/slot-specsはこの時点では評価しない(マクロなので)。実行時に親クラスを
;; 解決し、スロットを連結してクラスオブジェクトを作り*classes*に登録する
(defmacro defclass (name supers slot-specs &rest options)
  `(let ((%supers (%defclass-supers ',supers)))
     (%register-class ',name
       (%%make-class-raw ',name %supers
         (append (%merge-superclass-slots %supers)
                 (list ,@(%slot-spec-forms slot-specs)))))))

;; slots(スロット記述子のリスト)の中からslot-nameのインデックス(0起点)を探す
(defun %slot-index (slot-name slots idx)
  (if (null slots)
      nil
      (if (eq slot-name (car (car slots)))
          idx
          (%slot-index slot-name (cdr slots) (+ idx 1)))))

(defun slot-value (instance slot-name)
  (let ((idx (%slot-index slot-name (%%class-slots (%%instance-class instance)) 0)))
    (if (null idx)
        'eval-error
        (aref (%%instance-slots instance) idx))))

(defun set-slot-value (instance slot-name value)
  (let ((idx (%slot-index slot-name (%%class-slots (%%instance-class instance)) 0)))
    (if (null idx)
        'eval-error
        (set-aref (%%instance-slots instance) idx value))))

;; 対応するinitargがinitargs(:key1 val1 :key2 val2 ...)に無ければ
;; slot-descriptorのinitform-thunkをfuncallして初期値を得る
(defun %slot-initial-value (slot-descriptor initargs)
  (let ((initarg-key (car (cdr slot-descriptor)))
        (thunk (car (cdr (cdr slot-descriptor)))))
    (if (null initarg-key)
        (funcall thunk)
        (let ((found (member initarg-key initargs)))
          (if found
              (car (cdr found))
              (funcall thunk))))))

(defun %slot-initial-values (slots initargs)
  (if (null slots)
      nil
      (cons (%slot-initial-value (car slots) initargs) (%slot-initial-values (cdr slots) initargs))))

(defun %fill-slots (vec values idx)
  (if (null values)
      vec
      (progn (set-aref vec idx (car values))
             (%fill-slots vec (cdr values) (+ idx 1)))))

;; (make-instance class-designator :key1 val1 :key2 val2 ...)
;; class-designatorはクラス名(シンボル)またはクラスオブジェクト自身
(defun make-instance (class-designator &rest initargs)
  (let* ((class (if (%%classp class-designator) class-designator (%find-class class-designator)))
         (slots (%%class-slots class))
         (values (%slot-initial-values slots initargs)))
    (%%make-instance-raw class (%fill-slots (make-array (length slots)) values 0))))

;; c1がc2自身、またはc2の(推移的な)サブクラスかどうか
(defun subclassp (c1 c2)
  (if (eq c1 c2)
      t
      (%any-subclassp (%%class-supers c1) c2)))

(defun %any-subclassp (classes c2)
  (if (null classes)
      nil
      (if (subclassp (car classes) c2)
          t
          (%any-subclassp (cdr classes) c2))))

;; instanceがclass-designator(クラスオブジェクトまたはクラス名)のインスタンスかどうか
(defun typep (instance class-designator)
  (if (%%class-instance-p instance)
      (subclassp (%%instance-class instance)
                 (if (%%classp class-designator) class-designator (%find-class class-designator)))
      nil))

;; instanceがclass(クラスオブジェクト)のインスタンスかどうか。
;; 仕様上instancepはclassを評価済みのクラスオブジェクトとして受け取り(typepの
;; クラス名designatorとは異なる)、クラスオブジェクトでなければdomain-errorを
;; 発生させるべきだが、<domain-error>が未実装のため既存のtypep(designatorも
;; クラスオブジェクトも受け付ける、緩い実装)にそのまま委譲する既知の簡略化とする。
(defun instancep (instance class)
  (typep instance class))

;;; --- エラー処理とコンディショナルシステム(最小実装): signal-condition / with-handler / error ---
;;;
;;; コンディションはILOSのインスタンスとして表現する(専用のC構造体は増やさない)。
;;; 既知の簡略化: report-condition等の総称関数によるメッセージ整形は実装しない。
;;; format-string/format-argumentsスロットに素材を保持するのみ。

(defclass <condition> () ())
(defclass <error> (<condition>) ())
(defclass <simple-error> (<error>)
  ((format-string :initarg :format-string :initform nil)
   (format-arguments :initarg :format-arguments :initform nil)))

;; *handlers*はwith-handlerの動的スコープの間だけpush/popする、有効なhandler-functionの
;; リスト(内側が先頭)。*classes*と同じ理由でdefdynamic+%%set-dynamicを使う(冒頭の
;; 既知の制約: defvar+setqでは関数呼び出しの内側からのpush/popが呼び出し元に見えない)。
(defdynamic *handlers* nil)

;; handler-formを1度だけ評価し、bodyの動的スコープの間だけ*handlers*の先頭に積む。
;; unwind-protectでbodyがどう脱出しても(非局所脱出でも)必ずpopする。
(defmacro with-handler (handler-form &rest body)
  (let ((saved (gensym)))
    `(let ((,saved (dynamic *handlers*)))
       (%%set-dynamic '*handlers* (cons ,handler-form ,saved))
       (unwind-protect
           (progn ,@body)
         (%%set-dynamic '*handlers* ,saved)))))

;; トップレベルでcatchされなかったconditionをabortする。os_eval_top_level(C側)が
;; 張っているblock %TOP-LEVELへreturn-fromする。
(defun %abort-top-level (condition)
  (return-from %top-level condition))

;; *handlers*の先頭(最も内側)のhandler-functionを、一時的に自分自身を取り除いた状態で
;; 呼び出す(ハンドラ内でのsignal-conditionが次の外側のハンドラに渡るようにするため)。
;; 呼び出し後は*handlers*を元に戻す。continuableでないconditionでハンドラが
;; (非局所脱出せず)普通に返ってきた場合は、仕様上はエラーだがトップレベルへのabortに
;; フォールバックする。
(defun signal-condition (condition continuable)
  (let ((handlers (dynamic *handlers*)))
    (if (null handlers)
        (if continuable nil (%abort-top-level condition))
        (progn
          (%%set-dynamic '*handlers* (cdr handlers))
          (let ((result (funcall (car handlers) condition)))
            (%%set-dynamic '*handlers* handlers)
            (if continuable result (%abort-top-level condition)))))))

(defun error (format-string &rest format-arguments)
  (signal-condition
    (make-instance '<simple-error> ':format-string format-string ':format-arguments format-arguments)
    nil))

;;; --- ignore-errors ---

;; bodyの評価中に<error>系のconditionが発生したらそこで中断してnilを返す。
;; <error>でないconditionはこのスコープでは処理せず、signal-conditionで外側の
;; handlerに伝播させる(with-handlerのテストにある「型が合わなければ外側に渡す」パターン)。
(defmacro ignore-errors (&rest body)
  (let ((block-name (gensym)))
    `(block ,block-name
       (with-handler
           (lambda (c) (if (typep c '<error>) (return-from ,block-name nil) (signal-condition c nil)))
         ,@body))))

;;; --- class / the / assure ---

;; class-nameがtypepでも判定できる組み込み型名なら対応する述語で、それ以外
;; (ILOSのユーザークラス)ならtypepにフォールバックして判定する。
;; 既知の制約: stringp/characterpに相当するprimitiveが存在しないため、
;; <string>/<character>等はここでは対応しない(未対応の型名は常にtypep経由になり、
;; %find-classできないクラス名ならtypepがnilを返すのでassureは必ずエラーになる)。
(defun %assure-typep (obj class-name)
  (case class-name
    ((<integer>) (fixnump obj))
    ((<number>) (numberp obj))
    ((<symbol>) (symbolp obj))
    ((<cons>) (consp obj))
    ((<null>) (null obj))
    (t (typep obj class-name))))

;; (class class-name) : ILOSでdefclassされたクラス名をクラスオブジェクトに変換する。
;; 既知の制約: <integer>等の組み込み型のクラスオブジェクトは未実装のため対象外。
(defmacro class (class-name)
  `(%find-class ',class-name))

;; (the class-name form) : 型を宣言するだけで、実際の型チェックは行わない
;; (spec上、不一致時の動作は未定義なのでno-opで十分)。
(defmacro the (class-name form)
  form)

;; (assure class-name form) : formを評価し、その値がclass-nameの(サブ)クラスの
;; インスタンスでなければerrorを発生させる。一致すれば評価値をそのまま返す。
(defmacro assure (class-name form)
  (let ((v (gensym)))
    `(let ((,v ,form))
       (if (%assure-typep ,v ',class-name)
           ,v
           (error "assure: value is not of the expected type" ,v)))))

;;; --- convert ---
;;;
;;; ISLisp仕様(§17)のconvertの変換表(documents/islisp-v23.pdf、tmp/islisp-spec.txt
;;; 3888-3895行)のうち、既存primitiveだけで組み立てられる変換にスコープを絞る:
;;; symbol->string(symbol-name)、string->symbol(string-to-symbol)、
;;; string->list(string-elt/length)。list->stringは表の該当欄が"–"
;;; (エラーを発生させる、と明記)なので実装しないのは簡略化ではなく仕様通りの挙動。
;;; それ以外の組み合わせ(character<->integer等)はchar-code/code-char等
;;; 未実装のprimitiveが必要なため対象外とし、errorを発生させる。

;; strから(文字idxから文字len-1までの)文字のリストを作る
(defun %string-to-list-from (str idx len)
  (if (= idx len)
      nil
      (cons (string-elt str idx) (%string-to-list-from str (+ idx 1) len))))

(defun %string-to-list (str)
  (%string-to-list-from str 0 (length str)))

(defun %convert (obj class-name)
  (case class-name
    ((<string>) (if (symbolp obj) (symbol-name obj) (error "convert: unsupported conversion to <string>" obj)))
    ((<symbol>) (string-to-symbol obj))
    ((<list>) (%string-to-list obj))
    (t (error "convert: unsupported target class" class-name))))

;; (convert obj class-name) : class-nameは評価しない
(defmacro convert (obj class-name)
  `(%convert ,obj ',class-name))

;;; --- dynamic-let / set-dynamic ---

(defun %dynamic-let-vars (bindings)
  (if (null bindings)
      nil
      (cons (car (car bindings)) (%dynamic-let-vars (cdr bindings)))))

(defun %dynamic-let-inits (bindings)
  (if (null bindings)
      nil
      (cons (car (cdr (car bindings))) (%dynamic-let-inits (cdr bindings)))))

;; list1とlist2(同じ長さ)を(要素1 要素2)のペアのリストに束ねる。
(defun %dynamic-let-zip (list1 list2)
  (if (null list1)
      nil
      (cons (list (car list1) (car list2)) (%dynamic-let-zip (cdr list1) (cdr list2)))))

;; varsとvalues(同じ長さ)から (%%set-dynamic 'var value) のフォーム列を作る。
(defun %dynamic-let-set-forms (vars values)
  (if (null vars)
      nil
      (cons (list '%%set-dynamic (list 'quote (car vars)) (car values))
            (%dynamic-let-set-forms (cdr vars) (cdr values)))))

;; (dynamic-let ((var1 form1) (var2 form2) ...) body...) : 全init-formを(現在の動的束縛の下で)
;; 左から先にすべて評価してから、まとめて各varを動的に再束縛してbodyを評価する。
;; with-handlerと同じ保存/%%set-dynamic/unwind-protect復元パターンを変数の数だけ繰り返す。
;; 既知の制約: 未確立(defdynamic未実行)の変数を束縛するケースは扱わない(既存のdefdynamicの前提と同じ)。
(defmacro dynamic-let (bindings &rest body)
  (let ((vars (%dynamic-let-vars bindings))
        (inits (%dynamic-let-inits bindings)))
    (let ((temps (mapcar (lambda (v) (gensym)) vars))
          (saved (mapcar (lambda (v) (gensym)) vars)))
      `(let (,@(%dynamic-let-zip temps inits))
         (let (,@(%dynamic-let-zip saved (mapcar (lambda (v) (list 'dynamic v)) vars)))
           ,@(%dynamic-let-set-forms vars temps)
           (unwind-protect
               (progn ,@body)
             ,@(%dynamic-let-set-forms vars saved)))))))

;; (set-dynamic form var) : formを評価した値をvarの動的値に設定する(setqと引数順が逆で、
;; varは評価しない)。既存primitive%%SET-DYNAMICをラップするだけ。
(defmacro set-dynamic (form var)
  `(%%set-dynamic ',var ,form))

;;; --- with-standard-input / with-standard-output / with-error-output ---
;;;
;;; 既知のスコープ限定: ここで実装するのは*standard-input*/*standard-output*/
;;; *error-output*という動的変数とそのアクセサ、それらを動的に束縛するマクロだけ。
;;; 既存のREAD/READ-CHAR/WRITE-CHAR等のI/O primitiveは全て明示的にstream引数を
;;; 取る関数として実装済みで、それらを「省略時はこれらの動的変数を見る」ように
;;; 改修することは対象外(documents/isiki-os.mdに注記)。したがって仕様例のような
;;; 引数無しの(read)は動かない。動的束縛そのものが正しく機能することのみ確認する。

(defdynamic *standard-input* nil)
(defdynamic *standard-output* nil)
(defdynamic *error-output* nil)

(defun standard-input () (dynamic *standard-input*))
(defun standard-output () (dynamic *standard-output*))
(defun error-output () (dynamic *error-output*))

;; dynamic-letの単一変数版として展開するだけ
(defmacro with-standard-input (stream-form &rest body)
  `(dynamic-let ((*standard-input* ,stream-form)) ,@body))

(defmacro with-standard-output (stream-form &rest body)
  `(dynamic-let ((*standard-output* ,stream-form)) ,@body))

(defmacro with-error-output (stream-form &rest body)
  `(dynamic-let ((*error-output* ,stream-form)) ,@body))
