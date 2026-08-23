;;;; トランスパイラでAOTコンパイルする本番関数を置くファイル(M13)。
;;;;
;;;; ここでdefunされた関数は、src/lisp/transpile.lispがJITではなくホスト
;;;; (CommonLisp)側でCコードへ変換し、生成されたsrc/c/lisp_compiled.cとして
;;;; 事前にコンパイル・リンクされる。os_register_aot_init_functions
;;;; (transpile.lispが自動生成するC関数)がos_set_function経由でこれらを
;;;; global_environmentへネイティブ関数として登録するため、init.lisp側からは
;;;; インタプリタで定義された関数と全く同じシンボル名で呼び出せる。
;;;;
;;;; 元はsrc/lisp/init.lispにインタプリタ向けのdefunとして書かれていたが、
;;;; cc_loadによるテキストのパース・シンボルインターン・za.cでのJITコンパイルが
;;;; 不要になった分、Immobilized Spaceの使用量が削減される(このファイルの
;;;; 関数を移動した理由・効果)。
;;;;
;;;; トランスパイラの現状の対応範囲(defunパラメータはシンボルのみ・&rest未対応、
;;;; bodyは単一式のみ)を満たす関数だけをここに置く。

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

;;; --- append / reverse の内部ヘルパー ---

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

(defun %reverse-helper (list acc)
  (if (null list)
      acc
      (%reverse-helper (cdr list) (cons (car list) acc))))

;; listの要素順を反転した新しいリストを返す。
(defun reverse (list)
  (%reverse-helper list nil))

;;; --- maplist / mapl ---

;; mapcarと同様だが、fnには要素そのものではなく後続のsublist(cdrで縮んでいく
;; リスト自身)を適用する。
(defun maplist (fn list)
  (if (null list)
      nil
      (cons (funcall fn list) (maplist fn (cdr list)))))

(defun %mapl-1 (fn list)
  (if (null list)
      nil
      (progn (funcall fn list) (%mapl-1 fn (cdr list)))))

;; maplistと同様にsublistへfnを副作用目的で適用し、list自身を返す(mapcの
;; sublist版)。
(defun mapl (fn list)
  (progn (%mapl-1 fn list) list))

;;; --- list (M14) ---
;;;
;;; 基盤B(&restパラメータ対応)によりAOT対応可能になったため移動。

;; 引数をそのまま並べたリストを返す。&restが評価済みの引数を既にリストとして
;; 束縛するため、bodyはitemsをそのまま返すだけでよい。
(defun list (&rest items)
  items)

;;; --- append (M14) ---
;;;
;;; 基盤B(&restパラメータ対応)によりAOT対応可能になったため移動。呼び出し先の
;;; %append-listsはM13で既にここへ移動・AOT済み。

;; 0個以上のリストを連結して1つのリストにする。
(defun append (&rest lists)
  (%append-lists lists))

;;; --- create-list (M14) ---
;;;
;;; 基盤B(&restパラメータ対応)によりAOT対応可能になったため移動。

(defun %create-list-helper (n elt)
  (if (= n 0)
      nil
      (cons elt (%create-list-helper (- n 1) elt))))

;; 長さnのリストを作る。initial-elementは仕様上省略可(省略時の初期値は
;; implementation defined)で、本実装では省略時はnilを詰める。
(defun create-list (n &rest initial-element)
  (%create-list-helper n (if (null initial-element) nil (car initial-element))))

;;; --- nreverse (M14) ---
;;;
;;; 基盤A(let/let*のマクロ展開)によりAOT対応可能になったため移動。

(defun %nreverse-helper (list prev)
  (if (null list)
      prev
      (let ((next (cdr list)))
        (set-cdr list prev)
        (%nreverse-helper next list))))

;; reverseの破壊的版。listを構成するconsのcdrをset-cdrで書き換えて反転する
;; (新しいconsは確保しない)。
(defun nreverse (list)
  (%nreverse-helper list nil))

;;; --- case-using (M14) ---
;;;
;;; 基盤A(macroexpand-allでのcase-using展開対応)によりAOT対応可能になった
;;; ため移動。case-using自体はマクロ(トランスパイラでは展開されて消える)なので
;;; ここへ移動するのは展開後のコードが実行時に呼ぶこの関数のみ。

;; keylist中のいずれかのkについて(funcall pred key k)が真になるかを判定する。
(defun %case-using-match (pred key keylist)
  (if (null keylist)
      nil
      (if (funcall pred key (car keylist))
          t
          (%case-using-match pred key (cdr keylist)))))

;;; --- setf: slot-value place (M14) ---
;;;
;;; 基盤C(set-car/set-aref/set-elt等のプリミティブ許可リスト追加)によりAOT
;;; 対応可能になったため移動。%%class-slots/%%instance-class/%%instance-slots
;;; はruntime.cのネイティブプリミティブとして既に登録済み。

;; slots(スロット記述子のリスト)の中からslot-nameのインデックス(0起点)を探す
(defun %slot-index (slot-name slots idx)
  (if (null slots)
      nil
      (if (eq slot-name (car (car slots)))
          idx
          (%slot-index slot-name (cdr slots) (+ idx 1)))))

(defun set-slot-value (instance slot-name value)
  (let ((idx (%slot-index slot-name (%%class-slots (%%instance-class instance)) 0)))
    (if (null idx)
        'eval-error
        (set-aref (%%instance-slots instance) idx value))))

;;; --- apply (M14) ---
;;;
;;; 基盤B(&restパラメータ対応)+基盤C(%%apply)によりAOT対応可能になったため移動。

;; objsの最後の要素(list引数)以外を、その手前にconsで連結していく。
(defun %apply-args (objs)
  (if (null (cdr objs))
      (car objs)
      (cons (car objs) (%apply-args (cdr objs)))))

;; fnを、obj*を個別の引数として、末尾のlistの要素をさらに展開した引数列で呼び出す。
(defun apply (fn &rest objs)
  (%%apply fn (%apply-args objs)))

;;; --- mapcar / mapc / mapcan (M14) ---
;;;
;;; 基盤B(&restパラメータ対応)+%%apply(基盤C、既にapplyで追加済み)により
;;; AOT対応可能になったため移動。fnは評価済みの値として受け取り、Lisp2スコープの
;;; 制約でfnを直接呼べないため、funcall/%%apply(いずれもeval.c側の組み込み関数)を
;;; 介して呼び出す。mapcarのみ複数リストに対応し、mapc/mapcanは単一のリストのみを
;;; 受け取る簡略版のままとする。

;; list-of-listsの各要素(サブリスト)のcarを集めたリストを返す。
(defun %lists-car (lists)
  (if (null lists)
      nil
      (cons (car (car lists)) (%lists-car (cdr lists)))))

;; list-of-listsの各要素(サブリスト)のcdrを集めたリストを返す。
(defun %lists-cdr (lists)
  (if (null lists)
      nil
      (cons (cdr (car lists)) (%lists-cdr (cdr lists)))))

;; list-of-listsの中に、空リストになっているものが1つでもあればtを返す。
(defun %lists-some-null (lists)
  (if (null lists)
      nil
      (if (null (car lists))
          t
          (%lists-some-null (cdr lists)))))

;; listsの対応する位置の要素をまとめてfnに渡し(%%apply経由)、結果を集めたリストを
;; 返す。最短のリストが尽きた時点で終了し、他のリストの余った要素は無視する。
(defun %mapcar-lists (fn lists)
  (if (or (null lists) (%lists-some-null lists))
      nil
      (cons (%%apply fn (%lists-car lists))
            (%mapcar-lists fn (%lists-cdr lists)))))

;; listの各要素にfnを適用した結果を集めたリストを返す(1つ以上のリストを受け取れる)。
(defun mapcar (fn &rest lists)
  (%mapcar-lists fn lists))

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

;;; --- mapcon (M14) ---
;;;
;;; appendが基盤B(&rest)によりAOT対応済みになったため移動。maplist/mapcanと
;;; 同様にsublistへfnを適用するが、結果はappendで連結する(mapcanのsublist版)。
;;; 仕様上はnconcによる破壊的な連結だが、既存のmapcanと同様にnconcが未実装の
;;; ためappendで代用する簡略化になっている。
(defun mapcon (fn list)
  (if (null list)
      nil
      (append (funcall fn list) (mapcon fn (cdr list)))))

;;; --- map-into (M14) ---
;;;
;;; 基盤B(&restパラメータ対応)と、*primitive-c-names*へのlength/elt/</>=追加
;;; によりAOT対応可能になったため移動。destinationとsequences(0個以上)のうち
;;; 最も短い長さだけ、左から(function (elt seq1 i) (elt seq2 i) ...)の結果を
;;; destinationのi番目に破壊的に書き込み、destinationを返す。sequencesが可変長
;;; なので、mapcar等のfuncall経由ではなく%%apply(eval.c側の組み込み関数、
;;; 実引数リストをそのまま展開して呼ぶ)を使う。

;; seqsの中で最も短い長さを返す(destinationも含めてこのリストに渡される)。
(defun %map-into-min-length (seqs)
  (if (null (cdr seqs))
      (length (car seqs))
      (let ((rest-min (%map-into-min-length (cdr seqs))))
        (if (< (length (car seqs)) rest-min)
            (length (car seqs))
            rest-min))))

;; sequences群のindex番目の要素を並べたリストを作る(%%applyの実引数リストにする)。
(defun %map-into-args-at (index sequences)
  (if (null sequences)
      nil
      (cons (elt (car sequences) index)
            (%map-into-args-at index (cdr sequences)))))

(defun %map-into-loop (destination function sequences index limit)
  (if (>= index limit)
      destination
      (progn
        (set-elt (%%apply function (%map-into-args-at index sequences))
                 destination index)
        (%map-into-loop destination function sequences (+ index 1) limit))))

(defun map-into (destination function &rest sequences)
  (%map-into-loop destination function sequences 0
                   (%map-into-min-length (cons destination sequences))))

;;; --- ILOSクラスレジストリ (M12基盤B/C, #27) ---
;;;
;;; init.lispからの移動。*classes*自体のdefdynamicと、<object>以下の組み込み
;;; クラスをbootstrap登録する21個の%register-builtin-class呼び出しは、
;;; mainがinit_aot.lisp中のdefun以外のトップレベルフォームを一切読まないため
;;; init.lisp常駐のまま(移動するとAOTコードから見えず単に死んだ定義になる)。
;;; defclassマクロが展開時に呼ぶ%plist-get/%slot-spec-form(s)/
;;; %defclass-supers/%merge-superclass-slotsは、defclass自体がトップレベル
;;; マクロ呼び出しとして常にインタプリタ実行される(mainがdefun以外を無視する
;;; ため)ことをgrepで確認した結果、AOTコードから呼ばれることが無いと判明した
;;; ので、計画に反してここでは移動しない。

;; nameで登録済みクラスを引く。未登録ならnil。
(defun %find-class (name)
  (cdr (assoc name (dynamic *classes*))))

;; *classes*にnameからclassへの対応を追加する。bodyがtranspile-defunの
;; 「単一式のみ」制約を満たすためprognで包む(init.lisp版は2つのトップレベル式)。
(defun %register-class (name class)
  (progn
    (%%set-dynamic '*classes* (cons (cons name class) (dynamic *classes*)))
    class))

;; super-nameのリストを、対応する登録済みクラスオブジェクトのリストに変換する。
(defun %resolve-supers (super-names)
  (if (null super-names)
      nil
      (cons (%find-class (car super-names)) (%resolve-supers (cdr super-names)))))

;; predefinedクラス(<object>とその子)を%%MAKE-BUILTIN-CLASS-RAW(メタクラスは
;; <built-in-class>)で登録する。supersが未登録だと%find-classがnilを返すため、
;; 親クラスが先に登録済みになる順序で呼ぶ必要がある(呼び出し元はinit.lisp常駐)。
(defun %register-builtin-class (name super-names)
  (%register-class name (%%make-builtin-class-raw name (%resolve-supers super-names) nil)))

;;; --- スロットアクセスとインスタンス基礎 (M12 Phase 2, #27) ---
;;;
;;; init.lispからの移動。typep/instancepはclass-of(Phase 3で移動予定、まだ
;;; init.lisp常駐)を呼ぶため、class-ofの移動と同じコミットにまとめる方が筋が
;;; 良いと判断し、計画のPhase 2区分とは異なりここでは移動を見送る(Phase 3で
;;; 対応する)。

;; instanceのslot-nameスロットの値を返す。%slot-index/%%class-slots/
;; %%instance-class/%%instance-slotsは既にM13/M14/本Phaseでwhitelist・移動済み
(defun slot-value (instance slot-name)
  (let ((idx (%slot-index slot-name (%%class-slots (%%instance-class instance)) 0)))
    (if (null idx)
        'eval-error
        (aref (%%instance-slots instance) idx))))

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

;;; --- class-of (M12 基盤D, Phase 3, #27) ---
;;;
;;; init.lispからの移動。condはexpand-cond(*macro-expanders*)によりネストした
;;; ifへ展開されるため、macroexpand-all経由で対応済み(新規のexpand-*追加は
;;; 不要)。typep/instancepはPhase 2では見送っていたが、class-ofが揃ったので
;;; ここで合わせて移動する。

;; FIXNUM(60bit以内)とbignum(60bit超)のいずれかであれば整数とみなす。
(defun integerp (obj)
  (or (fixnump obj) (bignump obj)))

;; objが直接属するクラスを返す。ILOSのクラスインスタンスだけでなく、クラス
;; オブジェクト自身(メタクラス判定)や組み込み型の値も対象とする。nilは
;; car/cdr循環consとしてconsp/symbolp両方にマッチしうる内部表現のため、
;; consp/symbolpより先に判定する必要がある
(defun class-of (obj)
  (cond
    ((%%class-instance-p obj) (%%instance-class obj))
    ((%%standard-classp obj) (%find-class '<standard-class>))
    ((%%builtin-classp obj) (%find-class '<built-in-class>))
    ((null obj) (%find-class '<null>))
    ((consp obj) (%find-class '<cons>))
    ((symbolp obj) (%find-class '<symbol>))
    ((characterp obj) (%find-class '<character>))
    ((stringp obj) (%find-class '<string>))
    ((general-vector-p obj) (%find-class '<general-vector>))
    ((general-array*-p obj) (%find-class '<general-array*>))
    ((floatp obj) (%find-class '<float>))
    ((integerp obj) (%find-class '<integer>))
    ((numberp obj) (%find-class '<number>))
    ((functionp obj) (%find-class '<function>))
    ((streamp obj) (%find-class '<stream>))
    (t (%find-class '<object>))))

;; instanceがclass-designator(クラスオブジェクトまたはクラス名)のインスタンスかどうか。
;; class-ofが組み込み型も含めて汎用化されたので、ILOSインスタンス以外の値でも
;; 正しく判定できる
(defun typep (instance class-designator)
  (subclassp (class-of instance)
             (if (%%classp class-designator) class-designator (%find-class class-designator))))

;; instanceがclass(クラスオブジェクト)のインスタンスかどうか。
;; 仕様上instancepはclassを評価済みのクラスオブジェクトとして受け取り(typepの
;; クラス名designatorとは異なる)、クラスオブジェクトでなければdomain-errorを
;; 発生させるべきだが、<domain-error>が未実装のため既存のtypep(designatorも
;; クラスオブジェクトも受け付ける、緩い実装)にそのまま委譲する既知の簡略化とする。
(defun instancep (instance class)
  (typep instance class))

;;; --- 総称関数ディスパッチ機構(M12 Phase 5, #27) ---
;;;
;;; init.lispからの移動。*generic-methods*/*next-methods*のdefdynamicフォーム
;;; 自体、およびdefgeneric/defmethodマクロとそのマクロ展開時専用ヘルパ
;;; (%first-param-specializer/%first-param-var/%lambda-list-specializers/
;;; %method-plain-params/%specializer-forms)は、Phase4のexpand-class同様、
;;; 展開結果がinit_aot.lisp中の静的テキストとして現れず、defmethod/defgeneric
;;; 呼び出し自体もmainがdefun以外を無視するためAOTから一切見えない
;;; (defclassの%plist-get等と同じ「マクロ展開時専用ヘルパ」パターン)。よって
;;; init.lisp常駐のまま変更しない。
;;;
;;; %invoke-method-chainとcall-next-methodは内部不変条件違反時にerrorを呼ぶが、
;;; errorはPhase9で移動予定でまだAOTから直接呼べない(call-target-c-nameは
;;; *known-function-names*/*primitive-c-names*どちらにも載らない呼び出しを
;;; フォールバック無しでエラーにする)。%%funcall-by-name(新規Cプリミティブ、
;;; os_get_function+os_apply_functionで対象関数を実行時に名前解決して呼ぶ)
;;;経由でerrorを呼ぶことで、Phaseの前後関係に関係なくerrorの実際の
;;; signal-condition経由の挙動を完全に保ったまま解決する。

(defun %find-generic-methods (name)
  (cdr (assoc name (dynamic *generic-methods*))))

;; specializersのリストが位置ごとに等しいかどうか。各要素はnil同士も含めて
;; eqで比較してよい(クラスオブジェクトはdefclassごとに1つの同一オブジェクトと
;; して扱う)
(defun %specializers-equal-p (s1 s2)
  (if (null s1)
      (null s2)
      (if (null s2)
          nil
          (if (eq (car s1) (car s2))
              (%specializers-equal-p (cdr s1) (cdr s2))
              nil))))

(defun %remove-method-with-specializer (specializers methods)
  (if (null methods)
      nil
      (if (%specializers-equal-p specializers (car (car methods)))
          (%remove-method-with-specializer specializers (cdr methods))
          (cons (car methods) (%remove-method-with-specializer specializers (cdr methods))))))

;; 同じgf-name・同じspecializersの既存メソッドを取り除いた上で新しいメソッドを
;; 先頭に積んで登録する(defclass/%register-classと同じ「再定義は前に積んでshadow」
;; パターン)
(defun %register-method (name specializers fn)
  (let* ((existing (%find-generic-methods name))
         (updated (cons (cons specializers fn) (%remove-method-with-specializer specializers existing))))
    (%%set-dynamic '*generic-methods* (cons (cons name updated) (dynamic *generic-methods*)))
    name))

;; specializerがnil(無指定)のメソッドは常に適用可能。それ以外はargのクラス
;; (class-of、組み込み型も含む)がspecializerのサブクラス(自身含む)である場合のみ
;; 適用可能
(defun %method-applicable-p (specializer arg)
  (if (null specializer)
      t
      (subclassp (class-of arg) specializer)))

;; specializersの各要素と、対応する位置のargが全て%method-applicable-pであるか
;; (ANDを取る。specializersがargsより短い場合、残りのargは無指定として扱われる)
(defun %specializers-applicable-p (specializers args)
  (if (null specializers)
      t
      (if (%method-applicable-p (car specializers) (car args))
          (%specializers-applicable-p (cdr specializers) (cdr args))
          nil)))

(defun %filter-applicable-methods (methods args)
  (if (null methods)
      nil
      (if (%specializers-applicable-p (car (car methods)) args)
          (cons (car methods) (%filter-applicable-methods (cdr methods) args))
          (%filter-applicable-methods (cdr methods) args))))

(defun %applicable-methods (name args)
  (%filter-applicable-methods (%find-generic-methods name) args))

;; specializers1がspecializers2より特定的かどうか。先頭の位置から順に見て、
;; その位置のspecializerが完全に一致(eq、両方nilの場合も含む)する場合のみ
;; 次の位置へ進み、一致しない場合はその位置で即決する(specializerがnilの
;; 位置は常に最も非特定的)。CPLは計算しないため、一致しない位置で両方が
;; 非nilかつsubclassp関係を持たない場合はnil(決着つかず)を返す
(defun %specializers-more-specific-p (s1 s2)
  (if (null s1)
      nil
      (if (eq (car s1) (car s2))
          (%specializers-more-specific-p (cdr s1) (cdr s2))
          (if (null (car s2))
              t
              (if (null (car s1))
                  nil
                  (subclassp (car s1) (car s2)))))))

;; m1がm2より特定的かどうか
(defun %more-specific-p (m1 m2)
  (%specializers-more-specific-p (car m1) (car m2)))

(defun %insert-method-by-specificity (m sorted)
  (if (null sorted)
      (list m)
      (if (%more-specific-p m (car sorted))
          (cons m sorted)
          (cons (car sorted) (%insert-method-by-specificity m (cdr sorted))))))

;; 最も特定的なメソッドが先頭になるよう並び替える(挿入ソート)
(defun %order-methods (methods)
  (if (null methods)
      nil
      (%insert-method-by-specificity (car methods) (%order-methods (cdr methods)))))

;; orderedの先頭メソッドをargsで呼び出す。呼び出し中はorderedの残り(cdr)と
;; argsを新しいフレームとして*next-methods*に積み、unwind-protectで必ず復元する。
;; orderedが空(=適用可能なメソッドが無い/次のメソッドが無い)ならエラーとする
;; (仕様: 適用可能なメソッドが無い場合、call-next-methodで次が無い場合、いずれもerror)。
;; errorはPhase9移動までまだAOTから直接呼べないため%%funcall-by-name経由で呼ぶ
(defun %invoke-method-chain (ordered args)
  (if (null ordered)
      (%%funcall-by-name 'error "no applicable method")
      (let ((saved (dynamic *next-methods*)))
        (unwind-protect
            (progn (%%set-dynamic '*next-methods* (cons (cons (cdr ordered) args) saved))
                   (%%apply (cdr (car ordered)) args))
          (%%set-dynamic '*next-methods* saved)))))

(defun %generic-call (name args)
  (%invoke-method-chain (%order-methods (%applicable-methods name args)) args))

;; (next-method-p) → boolean: 現在のメソッドの内側でcall-next-methodが呼べるか
(defun next-method-p ()
  (if (null (dynamic *next-methods*))
      nil
      (if (car (car (dynamic *next-methods*))) t nil)))

;; (call-next-method) → <object>: 元の呼び出し引数のまま次のメソッドを呼ぶ。
;; errorはPhase9移動までまだAOTから直接呼べないため%%funcall-by-name経由で呼ぶ
(defun call-next-method ()
  (let ((frame (car (dynamic *next-methods*))))
    (if (null frame)
        (%%funcall-by-name 'error "call-next-method: no next method")
        (%invoke-method-chain (car frame) (cdr frame)))))

;; (make-instance class-designator :key1 val1 :key2 val2 ...)
;; class-designatorはクラス名(シンボル)またはクラスオブジェクト自身。
;; スロットの初期化自体はinitialize-object(defgeneric経由で動的生成される
;; 総称関数、init.lisp常駐)に委譲するが、initialize-objectという関数名自体は
;; *known-function-names*/*primitive-c-names*どちらにも載らずAOT側から直接
;; 呼び出せない(defgenericの展開結果はインタプリタのロード時に初めて生成される
;; 動的な関数であり、init_aot.lisp中の静的テキストとして存在しないため)。
;; そのため呼び出しを%generic-callの直接呼び出しへ手動で書き換える
;; (計画のdesign insight②、#27)
(defun make-instance (class-designator &rest initargs)
  (let* ((class (if (%%classp class-designator) class-designator (%find-class class-designator)))
         (instance (%%make-instance-raw class (make-array (length (%%class-slots class))))))
    (%generic-call 'initialize-object (list instance initargs))
    instance))

;;; --- 条件システム: トップレベルへのabort (M12 Phase 7, #27) ---
;;;
;;; init.lispからの移動。条件クラス階層(<condition>〜<storage-exhausted>)と
;;; *handlers*のdefdynamic、with-handlerマクロ自体はdefclass/defdynamic/defmacroが
;;; 常にインタプリタ実行される(mainがdefun以外を無視するため)ため移動不要
;;; (計画のPhase7節参照)。移動前に、AOTコード内で発生したreturn-from %top-levelの
;;; 非局所脱出がos_eval_top_level/eval_blockまで正しく伝播するかをQEMU実機で
;;; 確認した(未解決リスク②)。%%test-aot-call-abort-top-levelはその確認用の
;;; 恒久リグレッションテストで、%%funcall-by-name経由でAOTコードから呼ばれた
;;; 関数がreturn-fromの非局所脱出シグナルを返した場合の伝播を検証する
;;; (test/lisp/isiki_test.lisp参照)。

;; トップレベルでcatchされなかったconditionをabortする。os_eval_top_level(C側)が
;; 張っているblock %TOP-LEVELへreturn-fromする。
(defun %abort-top-level (condition)
  (return-from %top-level condition))

(defun %%test-aot-call-abort-top-level (marker)
  (%%funcall-by-name '%abort-top-level marker))
