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

(defun %for-let-bindings (bindings)
  (if (null bindings)
      nil
      (cons (list (car (car bindings)) (car (cdr (car bindings))))
            (%for-let-bindings (cdr bindings)))))

;; list-exprが指す一時リストからvar群へ順にcar/cdrで取り出してsetqする式を作る。
;; nextの式群は先に(list ...)としてまとめて評価されるため、do構文同様、
;; 各step式は「更新前」の値を参照でき、複数変数の同時更新が成立する。
(defun %for-setqs (bindings list-expr)
  (if (null bindings)
      nil
      (cons `(setq ,(car (car bindings)) (car ,list-expr))
            (%for-setqs (cdr bindings) `(cdr ,list-expr)))))

;; (for ((var1 init1 [step1]) ...) (test result...) body...) を、
;; varをletで束縛したのちtagbody/goによる繰り返しとsetqによる更新に展開する。
;; (旧実装はループ本体ごとにgensymで名前付きdefunを作っていたが、マクロは
;; 評価されるたびに毎回展開され直すため、forの外側にさらにforが入る多重ループでは
;; 1周ごとにgensymシンボルが恒久的に増え続け、symbol table exhaustedを起こしていた。
;; tagbody/goのタグはこのformの中だけで解決される局所的な識別子であり、
;; またgo/tagbody/if/progn/setqはいずれもマクロではなく特殊形式なので、
;; 固定のシンボルを使い回しても新規シンボルは一切生成されない。)
;; tagbodyは自身の最後の式の値を返さず常にnilを返すため、testが成立したときの
;; result部の値は(return-from nil ...)で明示的にblockの脱出値として返す必要がある。
;; block nilで囲むことで、body中の(return-from nil 値)による早期脱出もできる。
(defmacro for (bindings test-and-result &rest body)
  `(let ,(%for-let-bindings bindings)
     (block nil
       (tagbody
        %for-loop
        (if ,(car test-and-result)
            (return-from nil (progn ,@(cdr test-and-result)))
            (progn
              ,@body
              (let ((%for-next-values (list ,@(%for-nexts bindings))))
                ,@(%for-setqs bindings '%for-next-values))
              (go %for-loop)))))))

;; while同様にtagbody/goで展開する。ループを終わらせるにはtestがnilになるか、
;; body中でreturn-fromを使う。
(defmacro while (test &rest body)
  `(block nil
     (tagbody
      %while-loop
      (if ,test
          (progn ,@body (go %while-loop))
          nil))))

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

;; open-input-streamと同じ実装をファイル読み込み用の別名として提供する
;; (ISLisp仕様上はopen-input-fileが正式名で、open-input-streamはより一般的な
;; 入力ストリームを開く関数だが、本実装では9Pファイルopenのみをサポートするため
;; 実質的に同じ動作になる)。
(defun open-input-file (filename)
  (open-input-stream filename))

;;; --- with-open-output-stream / with-open-output-file ---

;; with-open-input-streamの出力版。streamの生成式を評価してnameに束縛し、
;; bodyの実行後は必ずcloseする(unwind-protectでbodyの異常終了時も保証)。
(defmacro with-open-output-stream (binding &rest body)
  `(let ((,(car binding) ,(car (cdr binding))))
     (unwind-protect
         (progn ,@body)
       (close ,(car binding)))))

;; (with-open-output-file (name filename [element-class]) body...) :
;; filenameをopen-output-fileで開き(無ければ新規作成)、nameに束縛してbodyを
;; 評価し、抜けたら必ずcloseする。element-classはwith-open-input-fileと同様の
;; 理由で無視する。
(defmacro with-open-output-file (binding &rest body)
  `(with-open-output-stream (,(car binding) (open-output-file ,(car (cdr binding))))
     ,@body))

;;; --- with-open-io-stream / with-open-io-file ---

;; with-open-output-streamと同型の入出力両用版。
(defmacro with-open-io-stream (binding &rest body)
  `(let ((,(car binding) ,(car (cdr binding))))
     (unwind-protect
         (progn ,@body)
       (close ,(car binding)))))

;; (with-open-io-file (name filename [element-class]) body...) :
;; filenameをopen-io-fileで開き(無ければ新規作成)、nameに束縛してbodyを評価し、
;; 抜けたら必ずcloseする。
(defmacro with-open-io-file (binding &rest body)
  `(with-open-io-stream (,(car binding) (open-io-file ,(car (cdr binding))))
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
        ((eq (car place) 'elt) `(set-elt ,value ,@(cdr place)))
        ((eq (car place) 'slot-value) `(set-slot-value ,@(cdr place) ,value))
        ((eq (car place) 'property) `(set-property ,value ,@(cdr place)))))

;;; --- apply (§9) ---
;;;
;;; %%apply(eval.c側の組み込み関数。fnと評価済みの引数リストの2つを取り、
;;; リストの内容をfnの実引数として展開して呼び出す)を、構文上並べたobj*と
;;; 末尾のlist引数を1本の引数リストに組み立てた上で呼ぶだけのLisp合成関数。

;; objsの最後の要素(list引数)以外を、その手前にconsで連結していく。
(defun %apply-args (objs)
  (if (null (cdr objs))
      (car objs)
      (cons (car objs) (%apply-args (cdr objs)))))

;; fnを、obj*を個別の引数として、末尾のlistの要素をさらに展開した引数列で呼び出す。
(defun apply (fn &rest objs)
  (%%apply fn (%apply-args objs)))

;;; --- mapcar / mapc / mapcan ---
;;;
;;; いずれもfnを評価済みの値として受け取り、関数呼び出しの形はLisp2スコープの
;;; 制約で(f x)のようには書けない(fが変数に束縛された関数値の場合、それは
;;; 関数namespaceではなく変数namespaceにあるため)。そのため、funcall(eval.c側の
;;; 組み込み関数)を介してfnを呼び出す。mapcarのみ複数リストに対応し、
;;; mapc/mapcanは単一のリストのみを受け取る簡略版のままとする。

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

;;; --- create-list / nreverse / maplist / mapl / mapcon (§21.3) ---

(defun %create-list-helper (n elt)
  (if (= n 0)
      nil
      (cons elt (%create-list-helper (- n 1) elt))))

;; 長さnのリストを作る。initial-elementは仕様上省略可(省略時の初期値は
;; implementation defined)で、本実装では省略時はnilを詰める。
(defun create-list (n &rest initial-element)
  (%create-list-helper n (if (null initial-element) nil (car initial-element))))

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

;; maplistと同様にsublistへfnを適用するが、結果はappendで連結する(mapcanの
;; sublist版)。仕様上はnconcによる破壊的な連結だが、既存のmapcanと同様に
;; nconcが未実装のためappendで代用する簡略化になっている。
(defun mapcon (fn list)
  (if (null list)
      nil
      (append (funcall fn list) (mapcon fn (cdr list)))))

;;; --- map-into ---
;;;
;;; destinationとsequences(0個以上)のうち最も短い長さだけ、左から
;;; (function (elt seq1 i) (elt seq2 i) ...)の結果をdestinationのi番目に破壊的に
;;; 書き込み、destinationを返す。sequencesが可変長なので、mapcar等のfuncall経由
;;; ではなく%%apply(eval.c側の組み込み関数、実引数リストをそのまま展開して呼ぶ)
;;; を使う。

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

;;; --- ILOS (最小実装): defclass / make-instance / slot-value / typep / subclassp ---
;;;
;;; 既知の簡略化: スロットの継承は単純な連結(同名オーバーライドやMRO計算は
;;; 行わない)。typepはILOSのクラスインスタンスに対する判定のみで、組み込み型は
;;; 対象外とする。defgeneric/defmethod/call-next-method/next-method-pは後段
;;; (instancepの後)で実装する(単一dispatch・qualifier無しの最小実装)。

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

;; super-nameのリストを、対応する登録済みクラスオブジェクトのリストに変換する。
;; defclass由来(%defclass-supers)と、直後のpredefinedクラスbootstrapの両方から
;; 使う共通の下請け関数(bootstrap側は<object>のようにsupersが本当に空のクラスも
;; 扱うため、空リストへのフォールバックはここでは行わない)
(defun %resolve-supers (super-names)
  (if (null super-names)
      nil
      (cons (%find-class (car super-names)) (%resolve-supers (cdr super-names)))))

;; ISLisp仕様Figure 1のクラス継承木に従い、条件階層以外のpredefinedクラス
;; (<object>とその子)を%%MAKE-BUILTIN-CLASS-RAW(メタクラスは<built-in-class>)で
;; 登録する。supersが未登録だと%find-classがnilを返してしまうため、
;; 親クラスが先に登録済みになる順序で呼ぶ必要がある
(defun %register-builtin-class (name super-names)
  (%register-class name (%%make-builtin-class-raw name (%resolve-supers super-names) nil)))

(%register-builtin-class '<object> nil)
(%register-builtin-class '<basic-array> '(<object>))
(%register-builtin-class '<basic-array*> '(<basic-array>))
(%register-builtin-class '<general-array*> '(<basic-array*>))
(%register-builtin-class '<basic-vector> '(<basic-array>))
(%register-builtin-class '<general-vector> '(<basic-vector>))
(%register-builtin-class '<string> '(<basic-vector>))
(%register-builtin-class '<built-in-class> '(<object>))
(%register-builtin-class '<character> '(<object>))
(%register-builtin-class '<function> '(<object>))
(%register-builtin-class '<generic-function> '(<function>))
(%register-builtin-class '<standard-generic-function> '(<generic-function>))
(%register-builtin-class '<list> '(<object>))
(%register-builtin-class '<cons> '(<list>))
(%register-builtin-class '<symbol> '(<object>))
(%register-builtin-class '<null> '(<list> <symbol>))
(%register-builtin-class '<number> '(<object>))
(%register-builtin-class '<integer> '(<number>))
(%register-builtin-class '<float> '(<number>))
(%register-builtin-class '<standard-class> '(<object>))
(%register-builtin-class '<standard-object> '(<object>))
(%register-builtin-class '<stream> '(<object>))

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

;; supersを指定しないdefclassは仕様上<standard-object>を暗黙に継承する
;; (「A standard class defined with no direct superclasses is guaranteed to
;; be disjoint from all of the classes in the figure, except for the classes
;; named <standard-object> and <object>」)
(defun %defclass-supers (super-names)
  (if (null super-names)
      (list (%find-class '<standard-object>))
      (%resolve-supers super-names)))

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
;; class-designatorはクラス名(シンボル)またはクラスオブジェクト自身。
;; スロットの初期化自体はinitialize-object(総称関数、下記)に委譲する。
;; initialize-objectはmake-instanceより後で定義されるが、この処理系は
;; 関数本体中のシンボル参照を呼び出し時に解決するインタプリタなので、
;; テキスト上の定義順は問題にならない(defclass/%find-classと同じ前提)。
(defun make-instance (class-designator &rest initargs)
  (let* ((class (if (%%classp class-designator) class-designator (%find-class class-designator)))
         (instance (%%make-instance-raw class (make-array (length (%%class-slots class))))))
    (initialize-object instance initargs)
    instance))

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

;;; --- integerp (§19: number class) ---

;; FIXNUM(60bit以内)とbignum(60bit超)のいずれかであれば整数とみなす。
(defun integerp (obj)
  (or (fixnump obj) (bignump obj)))

;;; --- class / the / assure ---
;;;
;;; defgeneric/defmethod(下記の総称関数セクション)がspecializerの解決に
;;; `class`マクロを使うため、このセクションは総称関数セクションより前に
;;; 置く必要がある(macroは定義前に使うと未定義関数呼び出しとして扱われ、
;;; defmethodのspecializer計算がすべて失敗する)。

;; typepが組み込み型も含めて汎用化されたので、そのまま委譲するだけで良い
(defun %assure-typep (obj class-name)
  (typep obj class-name))

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

;;; --- 総称関数(最小実装): defgeneric / defmethod / call-next-method / next-method-p ---
;;;
;;; 複数引数のクラスを見て特定性を判定する複数ディスパッチに対応する。
;;; 既知の簡略化: :before/:after/:aroundなどのmethod-qualifierは実装しない
;;; (primary methodのみ)。メソッドの特定性順序は各引数位置ごとのspecializer
;;; クラス間のsubclassp比較による簡易な挿入ソートで、真のクラス優先順位リスト
;;; (CPL/MRO)計算は行わない(defclassのスロット継承が単純な連結であることと
;;; 同水準の簡略化)。多重継承のダイヤモンド構造では、比較対象の引数位置で
;;; specializer同士がsubclassp関係を持たない場合に順序が不定になりうる。
;;; call-next-method/next-method-pは仕様上`labels`によりレキシカルに
;;; 束縛されるが、本実装では動的変数によるフレームスタックで代替する。

;; *generic-methods*: alist、gf-name -> methods((specializers . fn)*)。
;; specializersは引数位置ごとのクラス(またはnil=無指定)のリスト。
;; *classes*/*handlers*と同じ理由でdefdynamic+%%set-dynamicを使う
(defdynamic *generic-methods* nil)

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

(defun %applicable-methods (name args)
  (%filter-applicable-methods (%find-generic-methods name) args))

(defun %filter-applicable-methods (methods args)
  (if (null methods)
      nil
      (if (%specializers-applicable-p (car (car methods)) args)
          (cons (car methods) (%filter-applicable-methods (cdr methods) args))
          (%filter-applicable-methods (cdr methods) args))))

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

;; *next-methods*: call-next-method/next-method-pが参照する、現在呼び出し中の
;; メソッド呼び出しごとの「残りメソッドリスト+呼び出し引数」のフレームスタック
;; (内側の呼び出しが先頭)。with-handlerの*handlers*と同じ保存/復元パターン
(defdynamic *next-methods* nil)

;; orderedの先頭メソッドをargsで呼び出す。呼び出し中はorderedの残り(cdr)と
;; argsを新しいフレームとして*next-methods*に積み、unwind-protectで必ず復元する。
;; orderedが空(=適用可能なメソッドが無い/次のメソッドが無い)ならエラーとする
;; (仕様: 適用可能なメソッドが無い場合、call-next-methodで次が無い場合、いずれもerror)
(defun %invoke-method-chain (ordered args)
  (if (null ordered)
      (error "no applicable method")
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

;; (call-next-method) → <object>: 元の呼び出し引数のまま次のメソッドを呼ぶ
(defun call-next-method ()
  (let ((frame (car (dynamic *next-methods*))))
    (if (null frame)
        (error "call-next-method: no next method")
        (%invoke-method-chain (car frame) (cdr frame)))))

;; (defgeneric name lambda-list option*)
;; lambda-list/optionsは検証しない(既知の簡略化)。nameを、呼び出し時に
;; %generic-callへdispatchする通常のdefunとして定義するだけ
(defmacro defgeneric (name lambda-list &rest options)
  `(defun ,name (&rest %generic-args) (%generic-call ',name %generic-args)))

;; parameter-profileの各要素から(未評価の)specializer class-nameを取り出す。
;; (var class-name)形式ならclass-name、単純なvarならnil(specializer無し)
(defun %first-param-specializer (param)
  (if (consp param) (car (cdr param)) nil))

(defun %first-param-var (param)
  (if (consp param) (car param) param))

;; lambda-listの各要素から(未評価の)specializer class-nameを、位置に対応する
;; リストとして取り出す
(defun %lambda-list-specializers (lambda-list)
  (if (null lambda-list)
      nil
      (cons (%first-param-specializer (car lambda-list))
            (%lambda-list-specializers (cdr lambda-list)))))

;; lambda-listを、実際にlambdaへ渡せる引数リスト(各要素のspecializerを
;; 取り除いたもの)に変換する
(defun %method-plain-params (lambda-list)
  (if (null lambda-list)
      nil
      (cons (%first-param-var (car lambda-list))
            (%method-plain-params (cdr lambda-list)))))

;; (未評価の)specializer class-nameのリストを、classマクロ(既存、%find-classを
;; ラップ)呼び出し形のリストへ変換する。無指定(nil)の位置はnilのまま
(defun %specializer-forms (names)
  (if (null names)
      nil
      (cons (if (car names) (list 'class (car names)) nil)
            (%specializer-forms (cdr names)))))

;; (defmethod name parameter-profile form*)
;; 各パラメータに(var class-name)形式のspecializerを書ける(複数ディスパッチ)。
;; classマクロをそのまま再利用してspecializerクラスオブジェクトを取得する
(defmacro defmethod (name lambda-list &rest body)
  `(%register-method ',name
     (list ,@(%specializer-forms (%lambda-list-specializers lambda-list)))
     (lambda ,(%method-plain-params lambda-list) ,@body)))

;; (initialize-object instance initialization-arguments) → <object>
;; createがインスタンス生成時に呼ぶ総称関数。システム標準のprimary methodは
;; initargs/initformからスロットを埋める(既存%slot-initial-values/%fill-slotsを
;; そのまま再利用)。ユーザーはクラスを指定したdefmethodでこれをオーバーライド/
;; call-next-methodで拡張できる
(defgeneric initialize-object (instance initargs))

(defmethod initialize-object (instance initargs)
  (%fill-slots (%%instance-slots instance)
               (%slot-initial-values (%%class-slots (%%instance-class instance)) initargs)
               0)
  instance)

;; (class-of obj) → <class>: objが直接属するクラスを返す。ILOSのクラスインスタンス
;; だけでなく、クラスオブジェクト自身(メタクラス判定)や組み込み型の値も対象とする。
;; nilはcar/cdr循環consとしてconsp/symbolp両方にマッチしうる内部表現のため、
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

;;; --- エラー処理とコンディションシステム(§29): signal-condition / with-handler / error / クラス階層 ---
;;;
;;; コンディションはILOSのインスタンスとして表現する(専用のC構造体は増やさない)。
;;; <condition>はISLisp仕様には存在しない実装独自の基底クラス(spec上のルートは
;;; <serious-condition>だが、既存テストが(make-instance '<condition>)を直接使っているため
;;; 後方互換性のために<serious-condition>の親としてそのまま残す)。それ以外のクラス階層は
;;; spec図(tmp/islisp-spec.txt 994-1010行)通り。
;;; 既知のスコープ限定: C側のエラー発生箇所(ゼロ除算・未束縛変数・未定義関数・
;;; ストリームエラー等)を実際にこれらのconditionクラスへ繋ぎ直すことは対象外
;;; (C primitiveから評価器を呼び戻す仕組みが現状無いため)。<floating-point-overflow>/
;;; <floating-point-underflow>は浮動小数点数自体が未実装のため発生源を持たない。

(defclass <condition> ()
  ((%continuable :initform nil)
   (%continue-tag :initform nil)))
(defclass <serious-condition> (<condition>) ())
(defclass <error> (<serious-condition>) ())
(defclass <arithmetic-error> (<error>)
  ((operation :initarg :operation :initform nil)
   (operands :initarg :operands :initform nil)))
(defclass <division-by-zero> (<arithmetic-error>) ())
(defclass <floating-point-overflow> (<arithmetic-error>) ())
(defclass <floating-point-underflow> (<arithmetic-error>) ())
(defclass <control-error> (<error>) ())
(defclass <parse-error> (<error>)
  ((string :initarg :string :initform nil)
   (expected-class :initarg :expected-class :initform nil)))
(defclass <program-error> (<error>) ())
(defclass <domain-error> (<program-error>)
  ((object :initarg :object :initform nil)
   (expected-class :initarg :expected-class :initform nil)))
(defclass <undefined-entity> (<program-error>)
  ((name :initarg :name :initform nil)
   (namespace :initarg :namespace :initform nil)))
(defclass <unbound-variable> (<undefined-entity>) ())
(defclass <undefined-function> (<undefined-entity>) ())
(defclass <simple-error> (<error>)
  ((format-string :initarg :format-string :initform nil)
   (format-arguments :initarg :format-arguments :initform nil)))
(defclass <stream-error> (<error>)
  ((stream :initarg :stream :initform nil)))
(defclass <end-of-stream> (<stream-error>) ())
(defclass <storage-exhausted> (<serious-condition>) ())

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
;; 呼び出し後は*handlers*を元に戻す(unwind-protectでどの脱出経路でも保証する)。
;; continuableな呼び出しはconditionに%continuable/%continue-tagを記録した上でcatchで
;; 包み、continue-conditionからのthrowで指定した値を返して呼び出し元(signal-conditionの
;; 呼び出し元)へ復帰できるようにする。continuableでないconditionでハンドラが
;; (非局所脱出せず)普通に返ってきた場合は、仕様上はエラーだがトップレベルへのabortに
;; フォールバックする。
(defun signal-condition (condition continuable)
  (let ((handlers (dynamic *handlers*)))
    (if (null handlers)
        (if continuable nil (%abort-top-level condition))
        (let ((tag (gensym)))
          (set-slot-value condition '%continuable continuable)
          (set-slot-value condition '%continue-tag tag)
          (catch tag
            (unwind-protect
                (progn
                  (%%set-dynamic '*handlers* (cdr handlers))
                  (let ((result (funcall (car handlers) condition)))
                    (if continuable result (%abort-top-level condition))))
              (%%set-dynamic '*handlers* handlers)))))))

;; (condition-continuable condition) → <object> : signal-conditionが記録した
;; continuable引数(nil/t/継続用文字列)をそのまま返す。
(defun condition-continuable (condition)
  (slot-value condition '%continuable))

;; (continue-condition condition [value]) : conditionを今まさにsignalしている
;; signal-conditionの呼び出しへ、catchタグを介してvalue(既定nil)を返して復帰する。
(defun continue-condition (condition &rest value)
  (throw (slot-value condition '%continue-tag) (if value (car value) nil)))

(defun error (format-string &rest format-arguments)
  (signal-condition
    (make-instance '<simple-error> ':format-string format-string ':format-arguments format-arguments)
    nil))

;; (cerror continue-string error-string obj*) → <object> : spec 6966-6980行の等価定義通り、
;; continue-stringとerror-stringのいずれもobj*でformatする素材として<simple-error>に積み、
;; continuableには「continue-stringをformatした文字列」を渡す(signal-conditionが正常return
;; した場合、あるいはハンドラがcontinue-conditionでvalueを渡した場合、その値がcerrorの
;; 戻り値になる)。
(defun cerror (continue-string error-string &rest objs)
  (signal-condition
    (make-instance '<simple-error> ':format-string error-string ':format-arguments objs)
    (let ((str (create-string-output-stream)))
      (%%apply #'format (cons str (cons continue-string objs)))
      (get-output-stream-string str))))

;;; --- condition accessors (§29.3) ---
;;;
;;; 各アクセサは対象クラスでなければ<domain-error>をsignalする(spec 7080-7081行等、
;;; 各データ表の「An error shall be signaled if X is not a condition of class <X>
;;; (error-id. domain-error)」という要求に対応)。

;; objがclass-nameのインスタンスならそのまま返し、そうでなければ<domain-error>をsignalする。
(defun %check-condition-class (obj class-name)
  (if (typep obj class-name)
      obj
      (signal-condition
        (make-instance '<domain-error> ':object obj ':expected-class (%find-class class-name))
        nil)))

(defun arithmetic-error-operation (condition)
  (slot-value (%check-condition-class condition '<arithmetic-error>) 'operation))

(defun arithmetic-error-operands (condition)
  (slot-value (%check-condition-class condition '<arithmetic-error>) 'operands))

(defun domain-error-object (condition)
  (slot-value (%check-condition-class condition '<domain-error>) 'object))

(defun domain-error-expected-class (condition)
  (slot-value (%check-condition-class condition '<domain-error>) 'expected-class))

(defun parse-error-string (condition)
  (slot-value (%check-condition-class condition '<parse-error>) 'string))

(defun parse-error-expected-class (condition)
  (slot-value (%check-condition-class condition '<parse-error>) 'expected-class))

(defun simple-error-format-string (condition)
  (slot-value (%check-condition-class condition '<simple-error>) 'format-string))

(defun simple-error-format-arguments (condition)
  (slot-value (%check-condition-class condition '<simple-error>) 'format-arguments))

(defun stream-error-stream (condition)
  (slot-value (%check-condition-class condition '<stream-error>) 'stream))

(defun undefined-entity-name (condition)
  (slot-value (%check-condition-class condition '<undefined-entity>) 'name))

(defun undefined-entity-namespace (condition)
  (slot-value (%check-condition-class condition '<undefined-entity>) 'namespace))

;;; --- report-condition (§29.2) ---
;;;
;;; defgeneric/defmethod(既存のinitialize-objectと同じ機構、単一dispatch・
;;; subclasspベースの特定度順ソート)を再利用する。<condition>へのデフォルトメソッドは
;;; クラス名のみ出力し、データを持つクラスには具体的なメッセージを出す特化メソッドを
;;; 追加する。

(defgeneric report-condition (condition stream))

(defmethod report-condition ((condition <condition>) stream)
  (format stream "~A" (%%class-name (class-of condition)))
  condition)

(defmethod report-condition ((condition <simple-error>) stream)
  (%%apply #'format (cons stream (cons (slot-value condition 'format-string)
                                        (slot-value condition 'format-arguments))))
  condition)

(defmethod report-condition ((condition <arithmetic-error>) stream)
  (format stream "arithmetic error: ~A ~A" (slot-value condition 'operation) (slot-value condition 'operands))
  condition)

(defmethod report-condition ((condition <domain-error>) stream)
  (format stream "~A is not of expected class ~A" (slot-value condition 'object) (slot-value condition 'expected-class))
  condition)

(defmethod report-condition ((condition <parse-error>) stream)
  (format stream "cannot parse ~A as ~A" (slot-value condition 'string) (slot-value condition 'expected-class))
  condition)

(defmethod report-condition ((condition <stream-error>) stream)
  (format stream "stream error on ~A" (slot-value condition 'stream))
  condition)

(defmethod report-condition ((condition <undefined-entity>) stream)
  (format stream "undefined ~A: ~A" (slot-value condition 'namespace) (slot-value condition 'name))
  condition)

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

;;; --- symbol property list (§18.2): property / set-property / remove-property ---
;;;
;;; symbolのC側構造体にproperty list用のフィールドが無いため、*classes*/
;;; *generic-methods*と同じく外部のdefdynamicグローバルにentryを持つ。
;;; entryは((symbol . property-name) . value)というconsで、symbol/property-name
;;; はいずれもinternされているためeqで比較できる(*classes*のキー比較と同じ前提)。
;;; 再定義(set-property)は既存entryを取り除いた上で新しいentryを前に積む、
;;; *classes*/*generic-methods*と同じ「shadowパターン」。

(defdynamic *symbol-properties* nil)

;; objがsymbolでなければerror(domain-errorが未実装のため、assure/convertと
;; 同じ簡略化として通常のerrorで代替する)
(defun %check-symbol-arg (obj)
  (if (symbolp obj) obj (error "not a symbol" obj)))

(defun %property-key-eq (entry symbol property-name)
  (if (eq (car (car entry)) symbol)
      (eq (cdr (car entry)) property-name)
      nil))

(defun %find-property-entry (entries symbol property-name)
  (if (null entries)
      nil
      (if (%property-key-eq (car entries) symbol property-name)
          (car entries)
          (%find-property-entry (cdr entries) symbol property-name))))

(defun %remove-property-entry (entries symbol property-name)
  (if (null entries)
      nil
      (if (%property-key-eq (car entries) symbol property-name)
          (%remove-property-entry (cdr entries) symbol property-name)
          (cons (car entries) (%remove-property-entry (cdr entries) symbol property-name)))))

;; (property symbol property-name [obj]) : symbolのproperty-nameという名前の
;; propertyの値。無ければobj(既定nil)を返す
(defun property (symbol property-name &rest default)
  (%check-symbol-arg symbol)
  (%check-symbol-arg property-name)
  (let ((entry (%find-property-entry (dynamic *symbol-properties*) symbol property-name)))
    (if entry
        (cdr entry)
        (if default (car default) nil))))

;; (set-property obj symbol property-name) : symbolにproperty-nameという名前で
;; objを値とするpropertyを設定する(既存なら上書き、無ければ新規作成)。objを返す
(defun set-property (obj symbol property-name)
  (%check-symbol-arg symbol)
  (%check-symbol-arg property-name)
  (%%set-dynamic '*symbol-properties*
    (cons (cons (cons symbol property-name) obj)
          (%remove-property-entry (dynamic *symbol-properties*) symbol property-name)))
  obj)

;; (remove-property symbol property-name) : symbolからproperty-nameという名前の
;; propertyを取り除き、取り除いたpropertyの値(無ければnil)を返す
(defun remove-property (symbol property-name)
  (%check-symbol-arg symbol)
  (%check-symbol-arg property-name)
  (let ((entry (%find-property-entry (dynamic *symbol-properties*) symbol property-name)))
    (%%set-dynamic '*symbol-properties*
      (%remove-property-entry (dynamic *symbol-properties*) symbol property-name))
    (if entry (cdr entry) nil)))

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

;;; --- miscellaneous (§30) ---
(defun identity (obj) obj)

;;; --- number class (§19) float ---
;;; IEEE754 binary64のDBL_MAX/-DBL_MAX相当。reader.cのfloatリテラル構文(§19.2)を
;;; そのまま使って表現する(quotient/exp/log等の未実装超越関数と違い、
;;; 定数の束縛だけなのでCコードの追加は不要)。
(defconstant *most-positive-float* 1.7976931348623157E308)
(defconstant *most-negative-float* -1.7976931348623157E308)

;;; --- number class (§19) quotient/reciprocal/expt/三角・双曲線関数/*pi* ---
;;; sqrt/log/exp/sin/cos/atan2/floor/ceiling/truncate/round/parse-numberは
;;; 生FPU命令が必要なためCプリミティブ(runtime.c)として実装済み。ここではそれらを
;;; 組み合わせて仕様の残り関数をLisp側で合成する。

;; quotientはspec 4342-4362行の通り「両方整数かつ割り切れれば整数、そうでなければ
;; float」という/とは異なる型変換規則を持つ。3引数以上は左から逐次適用する(divisorが
;; 複数ある場合の仕様の定義通り)。
(defun %quotient2 (dividend divisor)
  (if (and (or (fixnump dividend) (bignump dividend))
           (or (fixnump divisor) (bignump divisor))
           (= (mod dividend divisor) 0))
      (div dividend divisor)
      (/ (float dividend) (float divisor))))

(defun quotient (dividend &rest divisors)
  (if divisors
      (%%apply #'quotient (cons (%quotient2 dividend (car divisors)) (cdr divisors)))
      dividend))

(defun reciprocal (x) (quotient 1 x))

;; x1^x2(x2が非負整数)を*による繰り返し二乗法で計算する。x1の型(整数/float)は
;; そのまま結果の型に伝わる(floatp baseなら1.0、それ以外は1を基底値とする)。
;; x1が負でもlogを経由しないため、負の底×整数指数(spec例: (expt -100 2) => 10000、
;; (expt -0.25 -1) => -4.0)を正しく扱える。
(defun %expt-integer (base power)
  (if (= power 0)
      (if (floatp base) 1.0 1)
      (if (= (mod power 2) 0)
          (let ((half (%expt-integer base (quotient power 2))))
            (* half half))
          (* base (%expt-integer base (- power 1))))))

;; x1=0絡みの特殊ケース(spec 4464-4484行)はerrorで処理する。x2が整数(fixnum/bignum)
;; なら底の符号を問わず%expt-integer(負指数はreciprocalで反転)で正確に計算し、
;; それ以外(x2が非整数float)はexp/logの合成とする(この場合のみx1<0はerror)。
(defun expt (x1 x2)
  (cond
    ((= x1 0)
     (cond
       ((and (numberp x2) (< x2 0)) (error "expt: 0 to a negative power ~S" x2))
       ((and (floatp x2) (= x2 0.0)) (error "expt: 0 to a float power of 0.0"))
       (t (if (floatp x2) 0.0 0))))
    ((or (fixnump x2) (bignump x2))
     (if (>= x2 0)
         (%expt-integer x1 x2)
         (reciprocal (%expt-integer x1 (- x2)))))
    ((< x1 0)
     (error "expt: negative base ~S with non-integer power ~S" x1 x2))
    (t (exp (* (float x2) (log (float x1)))))))

(defun tan (x) (/ (sin x) (cos x)))

;; atanの1引数版はspecの参考実装通り(atan2 x 1)
(defun atan (x) (atan2 x 1))

;; asin/acosの定義域チェック。型はnumberpで合っているが値が-1.0〜1.0の範囲外という
;; ユーザー明示指示の3ケース目に対応するdomain-error
(defun %check-unit-range (x)
  (if (and (numberp x) (<= -1.0 x 1.0))
      x
      (signal-condition
        (make-instance '<domain-error> ':object x ':expected-class (%find-class '<number>))
        nil)))

(defun asin (x) (let ((v (%check-unit-range x))) (atan2 v (sqrt (- 1 (* v v))))))
(defun acos (x) (let ((v (%check-unit-range x))) (atan2 (sqrt (- 1 (* v v))) v)))

(defun sinh (x) (/ (- (exp x) (exp (- x))) 2))
(defun cosh (x) (/ (+ (exp x) (exp (- x))) 2))
(defun tanh (x) (/ (sinh x) (cosh x)))

;; |x| >= 1のときlogの引数(1+x または 1-x)が0以下になり、logのdomain-errorが
;; そのまま伝播する(spec 4658-4669行が要求するatanhのdomain-errorを合成の副産物で満たす)
(defun atanh (x) (* 0.5 (- (log (+ 1 x)) (log (- 1 x)))))

;; spec 4505行「*pi* → <float> named constant」の通り、通常の変数参照(*pi*)で
;; 読める必要があるためdefdynamicではなくdefconstantを使う(defdynamicの値は
;; (dynamic name)経由でしか読めず、bareなシンボル参照は未定義変数アクセスになる)
(defconstant *pi* 3.141592653589793)

;;; Environment操作ユーティリティ(documents/environment.md Phase4)。Q4の決定に従い、
;;; make-environment等は特殊形式ではなく通常のLisp関数として実装する。名前(第一引数)は
;;; シンボルとして評価されるため、呼び出し側でquoteする(%%make-environmentが素朴に
;;; ラップしているだけで、defmacro等による名前の暗黙quoteは行わない)。戻り値はdefglobal
;;; (このコードベースにdefparameterは存在しないため)等で呼び出し側が明示的に変数へ
;;; 束縛する運用とする。

;; 生成済みの環境の一覧(list-environments用)。*classes*/*handlers*と同じ理由で
;; defdynamic+%%set-dynamicを使う(setq/os_set_variableは呼び出し先の環境内での
;; 書き込みが呼び出し元から見えないため)。
(defdynamic *environments* nil)

;; parent-envは&restで受け、省略時は現在の環境をparentとする(このコードベースは
;; &optionalに対応していないため、&restで実質的な省略可能引数を表現する)。
(defun make-environment (name &rest parent-env)
  (let ((parent (if parent-env (car parent-env) (%%current-environment))))
    (let ((env (%%make-environment name parent)))
      (%%set-dynamic '*environments* (cons env (dynamic *environments*)))
      env)))

(defun switch-environment (env) (%%set-current-environment env))

;; 追記(実装時の発見、1点目): progn/let等のC実装はいずれも呼び出し元から渡された
;; env引数をそのまま(レキシカルに)子フォームへ伝播するだけで、%%set-current-
;; environmentによるproc->envの書き換えを一切参照しない。したがって当初案(bodyを
;; (progn (%%set-current-environment env) ,@body)へまとめてunwind-protectで復元する)
;; では、bodyはin-environmentが呼ばれた時点のレキシカルenv(マクロ展開の呼び出し元の
;; env)で評価されたままになり、envへ実際には切り替わらない(defunした関数もその
;; 呼び出し元envへ書き込まれてしまう)。%%eval-in-environmentでbodyをformとして
;; envのもとへ明示的に評価することで、実際にenv上でdefun等が行われるようにする。
;;
;; 追記(2点目): 上記の修正だけではdestroy-environmentのQ3ガード(%%current-
;; environment、つまりproc->envを参照する)がbody実行中もenvへ切り替わっていることを
;; 前提にしたテスト(祖先環境破棄の拒否確認)が成立しない。%%eval-in-environmentによる
;; 明示的な評価(実際の副作用の対象)と、%%set-current-environmentによるproc->envの
;; 一時切り替え(destroy-environment等がinspectする「現在の環境」というブックキーピング
;; 情報)は別々の目的を持つため、両方を行う。
(defmacro in-environment (env &rest body)
  (let ((saved (gensym)) (target (gensym)))
    `(let ((,target ,env))
       (let ((,saved (%%current-environment)))
         (unwind-protect
             (progn
               (%%set-current-environment ,target)
               (%%eval-in-environment '(progn ,@body) ,target))
           (%%set-current-environment ,saved))))))

(defun list-environments () (dynamic *environments*))

;; 環境のcons列((name . env-symbol) (variables . alist) (functions . alist)
;; (parent . parent-env) ...)の4番目のスロットがparent。
(defun %environment-parent (env) (cdr (car (cdr (cdr (cdr env))))))

;; candidateがenv自身、またはenvのparentチェーンを遡って見つかるかを確認する。
(defun %environment-ancestor-p (candidate env)
  (cond ((null env) nil)
        ((eq candidate env) t)
        (t (%environment-ancestor-p candidate (%environment-parent env)))))

;; *environments*からenvとeqな要素を1つだけ取り除いた新しいリストを返す。
(defun %environment-remove (env list)
  (if (null list)
      nil
      (if (eq env (car list))
          (cdr list)
          (cons (car list) (%environment-remove env (cdr list))))))

;; Q3: 対象envが現在の環境自身、またはそのparentチェーンを遡って一致する場合は
;; フォールバックせずエラーにする(使用中の環境を破壊すると以降の評価が壊れるため)。
;; 一致しない場合のみ、Phase3のImmobilized Page回収とPhase3.6のリテラルスロット回収を
;; 実行して環境を破棄する。
(defun destroy-environment (env)
  (if (%environment-ancestor-p env (%%current-environment))
      (error "destroy-environment: 使用中の環境またはその祖先は破棄できません ~S" env)
      (progn
        (%%destroy-environment-reclaim env)
        (%%set-dynamic '*environments* (%environment-remove env (dynamic *environments*)))
        t)))
