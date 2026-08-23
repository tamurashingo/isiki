;;;; ホスト(CommonLisp)側で実行するトランスパイラ
;;;;
;;;; M9までにサポートしたのは、fixnum/string/symbol/nil/tのリテラルとquote、
;;;; defunパラメータ(クロージャなしのローカル変数)の参照・setq、
;;;; if/progn/and/orを組み合わせた単一の本体式、このファイル内でdefunされた
;;;; 関数同士の自己/相互再帰呼び出し(za.cのような都度のシンボル名解決は行わず、
;;;; AOTでリンクされるC関数を直接呼び出す)、および生成する関数のパラメータを
;;;; GC_PROTECTでshadow stackへ登録するコード生成。
;;;;
;;;; M10ではlambdaによる第一級(エスケープ可能)クロージャを追加する。lambda式は
;;;; トップレベルのC関数(defunと同じ__step/公開ラッパーの2関数構成)へリフトし、
;;;; 自由変数(パラメータでも他のdefun/プリミティブ名でもない裸の変数参照)だけを
;;;; 含む最小限の環境(os_make_environment)に捕捉する。捕捉方式はza.cの拡張4と
;;;; 同じSBCL方式の変数単位box昇格: setqされ、かつ何らかのネストしたlambdaに
;;;; 捕捉される変数だけをbox((val . 実値)のcons、os_setcdrで書き換え、cc_cdrで
;;;; 読み出す)として扱い、それ以外は値コピーで捕捉する。boxは複数のクロージャが
;;;; 同一のcons自体を共有することで、どちらから書き換えても他方から見える
;;;; (za_emit_build_capture_envと同じ考え方)。専用のshadow-stack配列は新設せず、
;;;; 既存のGC_PROTECTベースの仕組みで捕捉環境自体を保護する。let/cond/case等は
;;;; 後続のマイルストンで拡張する。
;;;;
;;;; 末尾位置の既知関数呼び出しは、64KB(ガード無し)のプロセススタックを
;;;; 実測した結果、素朴なC再帰では約410段で隣接プロセスのスタックを破壊する
;;;; ことが分かったため、トランポリン方式で定数スタックに変換する(ユーザー
;;;; 確認済み: 自己再帰だけでなく相互再帰も含む一般的な末尾呼び出しに対応)。
;;;; 各defunは「1手だけ進めるstep関数(tco_result_tを返す)」と「stepをループで
;;;; 回し切る公開ラッパー(ABI互換のlisp_val_tを返す)」の2つのC関数に分解する。
;;;; 末尾位置の既知関数呼び出しはstep関数を実際には呼ばず、呼び出し先の関数
;;;; ポインタと引数だけをtco_result_tに詰めてreturnする。ラッパーのwhileループが
;;;; それを受け取って次のstepを呼ぶため、Cの呼び出しは常にreturnしてから次の
;;;; 呼び出しが起きる「フラットな」形になり、再帰段数に関わらずCスタック消費は
;;;; 一定になる(GC_PROTECTのshadow stack登録もcleanup属性でC関数のスコープに
;;;; 束縛されているため、ループが何回回ってもリークしない)。プリミティブ呼び出し
;;;; は再帰しないstep関数を持たないため、末尾位置でも即時評価して確定値を返す。

(defparameter *runtime-lisp-path* "src/lisp/transpile_fixture.lisp")
(defparameter *aot-lisp-path* "src/lisp/init_aot.lisp"
  "init.lispから移動した、AOTトランスパイル対象の本番関数を置くファイル(M13)。
   transpile_fixture.lispと違い、ここでdefunされた関数はos_register_aot_init_functions
   経由でglobal_environmentへ登録され、init.lisp側からもインタプリタで定義された
   関数と同じシンボル名で呼び出せる")
(defparameter *output-c-path* "src/c/lisp_compiled.c")

(defparameter *known-function-names* nil
  "現在のトランスパイル対象ファイル内でdefunされている関数名の一覧。mainが
   全defunを読み終えた時点で束縛し、transpile-callが呼び出し先を解決する際に
   参照する(自己/相互再帰が定義順に関係なく解決できるようにするため)")

(defparameter *primitive-c-names*
  ;; 自己再帰の停止条件(カウントダウン等)を書くために最低限必要な算術/比較
  ;; プリミティブと、M10で導入するfuncall(lambdaが生成するクロージャ値を
  ;; 呼び出す唯一の手段)に対応する。これらはruntime.c/eval.cのprimitive_*が
  ;; 生成関数と同じABI(lisp_val_t fn(lisp_val_t args, lisp_val_t env))で
  ;; 既に実装済みのC関数で、呼び出し先アドレスはリンク時に確定するため、
  ;; defunされた関数と同じ「直接呼び出し」方式で扱える。primitive_funcallは
  ;; apply_function経由で汎用的にディスパッチするため、呼び出し先が
  ;; os_make_lifted_closureで作ったクロージャであってもこの1エントリで
  ;; そのまま対応できる(eval.c参照)
  '((- . "primitive_subtract")
    (eq . "primitive_eq")
    (funcall . "primitive_funcall")
    ;; M13: init.lispから移動するリスト操作関数(member/assoc/%append2/...)が
    ;; 使うcar/cdr/cons/nullに対応する。runtime.cのprimitive_car等はos_bootstrap内で
    ;; global_environmentへ既に登録済みのコア関数で、生成関数と同じABIを持つため
    ;; defunされた関数と同じ「直接呼び出し」方式で扱える
    (null . "primitive_null")
    (car . "primitive_car")
    (cdr . "primitive_cdr")
    (cons . "primitive_cons")
    ;; M14: create-list(%create-list-helper)が停止条件に使う数値等価比較
    (= . "primitive_num_equal")
    ;; M14: nreverse(%nreverse-helper)が破壊的な反転に使う
    (set-cdr . "primitive_set_cdr")
    ;; M14: setfの展開先(set-car/set-aref/set-elt)と、slot-value/set-slot-value
    ;; (%slot-index)が使う+・%%class-slots・%%instance-class・%%instance-slots
    (set-car . "primitive_set_car")
    (set-aref . "primitive_set_aref")
    (set-elt . "primitive_set_elt")
    (+ . "primitive_add")
    (%%class-slots . "primitive_class_slots")
    (%%instance-class . "primitive_instance_class")
    (%%instance-slots . "primitive_instance_slots")
    ;; M14: apply(&restの実引数リストを展開してfnを呼ぶ、eval.c側の組み込み関数)
    (%%apply . "primitive_apply")))

(defun sanitize-c-ident (name)
  "MEM-REF-64 -> mem_ref_64 (Cの識別子として使える形にする)"
  (remove #\! (substitute #\_ #\- (string-downcase name))))

(defun lisp-name-to-c-name (symbol)
  "MEMBER -> lisp_ll_member / %%mem-ref-64 -> lisp_ll_mem_ref_64。
   Lisp名の先頭に%系のマーカーが無いM13以降のような通常のトップレベル関数名
   (member/assoc/reverse等)も、必ずlisp_ll_を付けてCのグローバル名前空間へ
   出す(接頭辞無しのままだとlibc等の同名識別子と衝突しうるため。過去は
   全ファイルのdefunが%%transpile-fixture-接頭辞付きだったため露見しなかった)"
  (let* ((name (symbol-name symbol))
         (prefix-len (cond
                       ((and (>= (length name) 2)
                             (string= (subseq name 0 2) "%%"))
                        2)
                       ((and (plusp (length name))
                             (char= (char name 0) #\%))
                        1)
                       (t 0)))
         (body (sanitize-c-ident (subseq name prefix-len))))
    (concatenate 'string "lisp_ll_" body)))

(defun param-symbol-to-c-name (symbol)
  "defunパラメータ名 -> Cのローカル変数名。プレフィックスは付けない"
  (sanitize-c-ident (symbol-name symbol)))

(defun c-string-literal (s)
  "CommonLisp文字列からCの文字列リテラル(ダブルクオート込み)を作る。
   \\と\"のみエスケープする(現時点のfixtureはASCII識別子文字列のみのため十分)"
  (with-output-to-string (out)
    (write-char #\" out)
    (loop for ch across s
          do (when (or (char= ch #\\) (char= ch #\"))
               (write-char #\\ out))
             (write-char ch out))
    (write-char #\" out)))

;;; M10: lambdaの自由変数解析。let等の追加束縛フォームが無い現時点では、
;;; 式木の中で新たに変数を束縛できるのはlambda自身のパラメータだけなので、
;;; BOUND(既に束縛済みのシンボルのリスト)を引数で辿るだけでスコープを追跡できる。
;;; quoteはリテラルデータなので中身を式として解釈しない。呼び出し形式
;;; (name arg*)/if/progn/setq/and/or はいずれも(car expr)がシンボルで
;;; (cdr expr)が処理すべき部分式のリストという共通の形をしているため、
;;; lambda/quote以外は特別扱いせず(cdr expr)を再帰的に処理するだけでよい
;;; (carのシンボル自身は呼び出し先名やsetqの対象決定など別の場所で解決するため、
;;; ここでは変数参照として扱わない=自然にプリミティブ名/defun名を除外できる)

(defun free-variables (expr bound)
  "EXPR中の裸シンボル参照のうち、BOUND(シンボルのリスト)に含まれないものを
   重複なく集めて返す。lambda式に出会った場合はそのパラメータをBOUNDに加えて
   本体を再帰的に走査するため、ネストしたlambdaがさらに内側でしか使わない
   変数は自然に除外され、外側からの捕捉が必要な自由変数だけが伝播する"
  (cond
    ((integerp expr) nil)
    ((stringp expr) nil)
    ((null expr) nil)
    ((eq expr t) nil)
    ((symbolp expr) (if (member expr bound) nil (list expr)))
    ((and (consp expr) (eq (car expr) 'quote)) nil)
    ((and (consp expr) (eq (car expr) 'dynamic) (= (length expr) 2)) nil)
    ((and (consp expr) (eq (car expr) 'defdynamic) (= (length expr) 3))
     (free-variables (third expr) bound))
    ((and (consp expr) (eq (car expr) 'lambda) (= (length expr) 3))
     (free-variables (third expr) (append (second expr) bound)))
    ((consp expr)
     (reduce #'union (mapcar (lambda (e) (free-variables e bound)) (cdr expr))
             :initial-value nil))
    (t nil)))

(defun setq-targets (expr bound)
  "EXPR中でsetqの対象になっている変数のうち、BOUNDに含まれないものを重複なく
   集めて返す(free-variablesと同じBOUND伝播規則でlambdaのネストを辿るため、
   ネストしたlambdaの内部で行われるsetqも見つかる)。この関数はboxに昇格すべき
   パラメータを判定するために、あるパラメータが自分の本体のどこか(直接でも
   ネストしたlambda経由でも)でsetqされるかどうかを調べる目的で使う"
  (cond
    ((atom expr) nil)
    ((eq (car expr) 'quote) nil)
    ((and (eq (car expr) 'dynamic) (= (length expr) 2)) nil)
    ((and (eq (car expr) 'defdynamic) (= (length expr) 3))
     (setq-targets (third expr) bound))
    ((and (eq (car expr) 'lambda) (= (length expr) 3))
     (setq-targets (third expr) (append (second expr) bound)))
    ((and (eq (car expr) 'setq) (= (length expr) 3))
     (union (if (member (second expr) bound) nil (list (second expr)))
            (setq-targets (third expr) bound)))
    (t (reduce #'union (mapcar (lambda (e) (setq-targets e bound)) (cdr expr))
               :initial-value nil))))

(defun find-lambda-forms (expr)
  "EXPR中に直接現れるlambda式を集める。見つけたlambdaの内部(そのbody)までは
   辿らない: ネストしたlambdaが外側の変数を必要とする場合、それはfree-variables
   がそのネストしたlambdaを直接囲むlambdaの自由変数として再帰的に伝播させるため、
   ここで別途掘り進める必要が無い(captured-params参照)"
  (cond
    ((atom expr) nil)
    ((eq (car expr) 'quote) nil)
    ((and (eq (car expr) 'lambda) (= (length expr) 3)) (list expr))
    (t (reduce #'append (mapcar #'find-lambda-forms (cdr expr)) :initial-value nil))))

(defun captured-params (body params)
  "BODY中のどこかにネストして現れるlambdaが自由変数として必要とする変数のうち、
   PARAMS(この関数自身のパラメータ)に含まれるものを集めて返す(=このパラメータは
   何らかのネストしたlambdaに捕捉される、という判定)。find-lambda-formsで見つけた
   直接ネストのlambdaそれぞれについて、そのlambda自身のパラメータだけをBOUNDとした
   free-variablesを取ることで、より深くネストしたlambdaの要求も(free-variables自身の
   lambda特別扱いにより)自動的に伝播して含まれる"
  (intersection params
                (reduce #'union
                        (mapcar (lambda (l) (free-variables (third l) (second l)))
                                (find-lambda-forms body))
                        :initial-value nil)))

(defun boxed-params (body params)
  "PARAMSのうち、za.cの拡張4と同じ基準(setqされ、かつエスケープするlambdaに
   捕捉される変数だけをbox化する)でboxへ昇格すべきものを返す"
  (intersection (setq-targets body nil) (captured-params body params)))

;;; M14: マクロ展開。init.lispのlet/cond/case/for/while/setf/with-open-*は全て
;;; defmacroだが、init.lispには(defpackage/in-package)によるパッケージ分離が無いため、
;;; これらをそのままホストSBCLへ(defmacro let ...)としてloadするとCL:LET等の
;;; 特殊形式/標準マクロと名前衝突し"The special operator LET can't be redefined
;;; as a macro."になる(M5/issue #20で検証済み・documents/transpiler.md参照)。
;;; そのため、マクロ名と衝突しない通常の関数(expand-let等)としてマクロ本体を
;;; ポートし、以下のmacroexpand-allという自前の再帰コードウォーカーから
;;; *macro-expanders*(マクロ名->展開関数のalist)経由で呼び出す方式を採る。
;;; gensymはホスト側SBCL標準のものを使う(展開時にのみ使う一意シンボルなので、
;;; ランタイム側のgensymと意味的に完全に等価)。

(defun macroexpand-all (form)
  "FORM中に現れるマクロ呼び出し(*macro-expanders*に載っているもの)を再帰的に
   展開して、コアフォーム(if/progn/setq/and/or/lambda/dynamic/defdynamic+呼び出し)
   だけになるまで畳み込む。quoteは不透過データなので中身を展開しない。lambdaは
   パラメータリストに触れずbodyのみ展開する。dynamicは第2引数(未評価のシンボル
   リテラル)に触れず、defdynamicは第3引数(値を計算する式)のみ展開する(いずれも
   free-variables/setq-targetsの特別扱いと同じ理由)。マクロ呼び出しが見つかった
   場合は展開関数を1回呼んだ後、展開結果を再度macroexpand-allに通す(let*が
   letへ展開するなど、展開結果がさらにマクロ呼び出しを含みうるため)"
  (cond
    ((atom form) form)
    ((eq (car form) 'quote) form)
    ((and (eq (car form) 'dynamic) (= (length form) 2)) form)
    ((and (eq (car form) 'defdynamic) (= (length form) 3))
     (list (first form) (second form) (macroexpand-all (third form))))
    ((and (eq (car form) 'lambda) (= (length form) 3))
     (list (first form) (second form) (macroexpand-all (third form))))
    ((and (symbolp (car form)) (assoc (car form) *macro-expanders*))
     (macroexpand-all (funcall (cdr (assoc (car form) *macro-expanders*)) form)))
    (t (cons (macroexpand-all (car form)) (mapcar #'macroexpand-all (cdr form))))))

;;; let/let*: init.lispのdefmacro let/let*(%let-vars/%let-inits)と同じ展開規則。
;;; bodyは&restで複数式を許すが、transpile-lambda/transpile-defunは単一の本体式
;;; のみ対応するため、init.lispの`,@body`をそのまま展開先に置く代わりに
;;; (progn ,@body)で1式に畳み込む(prognは既存サポート済みで、複数式のうち
;;; 最後の式の値を残す評価順もletのbody評価順と一致する)。

(defun %%let-vars (bindings)
  (if (null bindings) nil (cons (car (car bindings)) (%%let-vars (cdr bindings)))))

(defun %%let-inits (bindings)
  (if (null bindings) nil (cons (car (cdr (car bindings))) (%%let-inits (cdr bindings)))))

(defun expand-let (form)
  (destructuring-bind (let-kw bindings &rest body) form
    (declare (ignore let-kw))
    `((lambda ,(%%let-vars bindings) (progn ,@body)) ,@(%%let-inits bindings))))

(defun expand-let* (form)
  (destructuring-bind (let*-kw bindings &rest body) form
    (declare (ignore let*-kw))
    (if (null bindings)
        `(progn ,@body)
        `(let (,(car bindings))
           (let* ,(cdr bindings) ,@body)))))

;;; cond: init.lispのdefmacro cond(%%case-expandとは別物)と同じ展開規則。
;;; 1段展開すると結果に(cond ,@(cdr clauses))というcond自身の再帰呼び出しが
;;; 残るが、macroexpand-allが展開結果を再度macroexpand-allに通す(let*と同じ
;;; 理由)ため、clausesが尽きるまで自動的に繰り返し展開される

(defun expand-cond (form)
  (let ((clauses (cdr form)))
    (if (null clauses)
        nil
        `(if ,(car (car clauses))
             (progn ,@(cdr (car clauses)))
             (cond ,@(cdr clauses))))))

;;; case: init.lispのdefmacro case(%case-expand)と同じ展開規則。t節以外の
;;; clauseはkeylistをquoteしたまま(member key '(...))へ、t節は素通しでprognへ
;;; 展開するif連鎖。keylistのquoteはtranspile-quotedがリスト(cons)対応する
;;; 必要があるため、このコミットでtranspile-quotedへcons分岐を追加する。

(defun %%case-expand (key clauses)
  (if (null clauses)
      nil
      (if (eq (car (car clauses)) t)
          `(progn ,@(cdr (car clauses)))
          `(if (member ,key ',(car (car clauses)))
               (progn ,@(cdr (car clauses)))
               ,(%%case-expand key (cdr clauses))))))

(defun expand-case (form)
  (destructuring-bind (case-kw keyform &rest clauses) form
    (declare (ignore case-kw))
    (let ((key (gensym)))
      `(let ((,key ,keyform))
         ,(%%case-expand key clauses)))))

;;; case-using: init.lispのdefmacro case-using(%case-using-expand)と同じ展開
;;; 規則。実行時の述語呼び出しは%case-using-match(init_aot.lispへ移動する既知
;;; 関数)へ委譲するため、caseと違いmemberを直接埋め込むのではなく関数呼び出しを
;;; 生成する。

(defun %%case-using-expand (pred key clauses)
  (if (null clauses)
      nil
      (if (eq (car (car clauses)) t)
          `(progn ,@(cdr (car clauses)))
          `(if (%case-using-match ,pred ,key ',(car (car clauses)))
               (progn ,@(cdr (car clauses)))
               ,(%%case-using-expand pred key (cdr clauses))))))

(defun expand-case-using (form)
  (destructuring-bind (case-using-kw predform keyform &rest clauses) form
    (declare (ignore case-using-kw))
    (let ((pred (gensym)) (key (gensym)))
      `(let ((,pred ,predform) (,key ,keyform))
         ,(%%case-using-expand pred key clauses)))))

;;; setf: init.lispのdefmacro setfと同じ展開規則。placeの形に応じてsetq/
;;; set-car/set-cdr/set-aref/set-elt/set-slot-value/set-propertyへ展開する。
;;; このうちset-property(%check-symbol-arg経由でerrorを呼ぶ)はILOSの
;;; condition system(M12)が無いと呼び出し先を解決できずtranspile時に
;;; エラーとなるため、(setf (property ...) ...)は現時点では未対応のまま
;;; (呼び出し先解決時にcall-target-c-nameが明示的なエラーで検出する)。
;;; car/cdr/aref/elt/slot-valueの5形式は本コミットで対応する。

(defun expand-setf (form)
  (destructuring-bind (setf-kw place value) form
    (declare (ignore setf-kw))
    (cond ((symbolp place) `(setq ,place ,value))
          ((eq (car place) 'car) `(set-car ,(car (cdr place)) ,value))
          ((eq (car place) 'cdr) `(set-cdr ,(car (cdr place)) ,value))
          ((eq (car place) 'aref) `(set-aref ,@(cdr place) ,value))
          ((eq (car place) 'elt) `(set-elt ,value ,@(cdr place)))
          ((eq (car place) 'slot-value) `(set-slot-value ,@(cdr place) ,value))
          ((eq (car place) 'property) `(set-property ,value ,@(cdr place))))))

(defparameter *macro-expanders*
  (list (cons 'let #'expand-let)
        (cons 'let* #'expand-let*)
        (cons 'cond #'expand-cond)
        (cons 'case #'expand-case)
        (cons 'case-using #'expand-case-using)
        (cons 'setf #'expand-setf))
  "マクロ名(シンボル)から展開関数への alist。展開関数は元のフォーム全体
   (car=マクロ名を含む)を受け取り、展開後のフォームを返す。macroexpand-allが
   これを見てディスパッチする。各オペレータの実装コミットでここへ追加していく")

(declaim (ftype function transpile-quoted))
(declaim (ftype function transpile-if))
(declaim (ftype function transpile-progn))
(declaim (ftype function transpile-setq))
(declaim (ftype function transpile-and))
(declaim (ftype function transpile-or))
(declaim (ftype function transpile-dynamic-read))
(declaim (ftype function transpile-defdynamic))
(declaim (ftype function transpile-call))
(declaim (ftype function transpile-lambda))
(declaim (ftype function transpile-tail-stmt))
(declaim (ftype function transpile-tail-and))
(declaim (ftype function transpile-tail-or))
(declaim (ftype function transpile-tail-call))

(defun transpile-expr (expr &optional scope)
  "fixnum/string/nil/tの裸リテラルと、それらに対するquote、シンボルのquote、
   および束縛済みパラメータの参照(scope内のシンボル)に対応する。GCで移動しうる
   値(symbol/string)は、za.cのT/quoteシンボルリテラル対応と同じく、生ポインタを
   埋め込まずos_make_symbol/os_make_stringを都度呼んで解決する。nilはランタイムが
   GCで移動しない固定センチネルなのでexternグローバルnilをそのまま参照してよい。
   scopeはシンボルからCのローカル変数の情報(c-name . boxed-p)へのalist。
  boxed-pがnon-nilの変数は、値そのものではなくbox((捨て値 . 実値)のcons)を
  c-nameが指すため、読み出しはcc_cdrを経由する(M10のbox化パラメータ/自由変数、
  transpile-defun/transpile-lambda参照)"
  (cond
    ((integerp expr)
     (format nil "os_make_fixnum(~AULL)" expr))
    ((stringp expr)
     (format nil "os_make_string(~A)" (c-string-literal expr)))
    ((null expr) "nil")
    ((eq expr t) "g_sym_t")
    ((and (symbolp expr) (assoc expr scope))
     (let ((binding (cdr (assoc expr scope))))
       (if (cdr binding)
           (format nil "cc_cdr(~A)" (car binding))
           (car binding))))
    ((and (consp expr) (eq (car expr) 'quote) (= (length expr) 2))
     (transpile-quoted (second expr)))
    ((and (consp expr) (eq (car expr) 'if))
     (transpile-if expr scope))
    ((and (consp expr) (eq (car expr) 'progn))
     (transpile-progn expr scope))
    ((and (consp expr) (eq (car expr) 'setq) (= (length expr) 3))
     (transpile-setq expr scope))
    ((and (consp expr) (eq (car expr) 'and))
     (transpile-and (cdr expr) scope))
    ((and (consp expr) (eq (car expr) 'or))
     (transpile-or (cdr expr) scope))
    ((and (consp expr) (eq (car expr) 'lambda) (= (length expr) 3))
     (transpile-lambda expr scope))
    ((and (consp expr) (eq (car expr) 'dynamic) (= (length expr) 2))
     (transpile-dynamic-read expr))
    ((and (consp expr) (eq (car expr) 'defdynamic) (= (length expr) 3))
     (transpile-defdynamic expr scope))
    ((and (consp expr) (symbolp (car expr)))
     (transpile-call expr scope))
    ((and (consp expr) (consp (car expr)) (eq (car (car expr)) 'lambda) (= (length (car expr)) 3))
     ;; let/let*の展開((lambda (vars) body) inits)のような、演算子位置に直接
     ;; lambda式が来る即時呼び出し形式。funcallプリミティブ(primitive_funcall、
     ;; fnを第一引数、残りを実引数として受け取りapply_functionへ渡す)への
     ;; 呼び出しへ書き換えることで、既存のtranspile-call/transpile-lambdaを
     ;; そのまま再利用できる
     (transpile-call (cons 'funcall expr) scope))
    ((symbolp expr)
     (error "transpile-expr: 未束縛の変数参照です: ~S" expr))
    (t (error "transpile-expr: 未対応の式です: ~S" expr))))

(defun transpile-if (expr scope)
  "(if test then [else])。真偽値は「nil以外はすべて真」の規約に従い、
   testをnilと比較した結果でCの三項演算子に分岐する(za.cの拡張0と同じ設計)。
   elseを省略した場合はnilがデフォルト(destructuring-bindの&optionalが
   nilにデフォルトし、transpile-exprがnil式を\"nil\"に変換するのでそのまま流れる)"
  (destructuring-bind (if-kw test then &optional else) expr
    (declare (ignore if-kw))
    (format nil "((~A) != nil ? (~A) : (~A))"
            (transpile-expr test scope)
            (transpile-expr then scope)
            (transpile-expr else scope))))

(defun transpile-progn (expr scope)
  "(progn form*)。Cのコンマ演算子で左から順に評価し、最後の式の値を残す
   (副作用の順序もコンマ演算子の評価順保証によりLispの逐次評価と一致する)。
   formが1つも無い場合はnilを返す"
  (let ((forms (cdr expr)))
    (if (null forms)
        "nil"
        (format nil "(~{~A~^, ~})" (mapcar (lambda (f) (transpile-expr f scope)) forms)))))

(defun transpile-setq (expr scope)
  "(setq var val)。varはscope内の束縛済みローカル変数(defun/lambdaパラメータ、
   またはlambdaが捕捉した自由変数)のみ対応する。box化されていない変数への
   代入は、Cの代入式が代入後の値そのものに評価される性質を使い、追加の処理
   無くLispのsetqの戻り値規約と一致する。box化された変数(za.cの拡張4と同じ
   基準でsetqされエスケープするlambdaに捕捉されたもの)への代入はos_setcdrで
   box(cons)のcdrを直接書き換える(このboxを共有する他のクロージャや外側の
   スコープからも変更後の値が見える)。os_setcdrはval自身を返す規約なので、
   ここでもCの代入式と同じく戻り値の扱いを変える必要がない"
  (let* ((var (second expr))
         (val (third expr))
         (binding (assoc var scope)))
    (unless binding
      (error "transpile-setq: setqの対象が未束縛のローカル変数です: ~S" var))
    (let ((c-name (car (cdr binding)))
          (boxed-p (cdr (cdr binding))))
      (if boxed-p
          (format nil "os_setcdr(~A, ~A)" c-name (transpile-expr val scope))
          (format nil "(~A = ~A)" c-name (transpile-expr val scope))))))

(defun transpile-and (forms scope)
  "(and form*)。init.lispのdefmacro andが展開する(if a (and b...) nil)の
   ネストと同じ形をCの三項演算子で直接生成する(=transpile-ifと同じパターン)。
   formが1つも無い場合はt、1つだけの場合はその式自身をそのまま返す
   (末尾のif展開に頼らずここで打ち切ることで、不要なネストを避ける)"
  (cond
    ((null forms) "g_sym_t")
    ((null (cdr forms)) (transpile-expr (car forms) scope))
    (t (format nil "((~A) != nil ? (~A) : nil)"
               (transpile-expr (car forms) scope)
               (transpile-and (cdr forms) scope)))))

(defparameter *or-temp-counter* 0)

(defun transpile-or (forms scope)
  "(or form*)。init.lispのdefmacro orは(let ((temp a)) (if temp temp (or ...)))
   に展開し、これはaを二重評価しないための一時変数である。このマイルストンでは
   letが未対応(M10でlambda lifting/closuresが入るまで見送り)なため、GNU Cの
   文(ステートメント)式({ ...; expr; })でCレベルの一時変数を導入し、同じ
   単一評価の性質を実現する(一時変数名はネストしたブロックスコープにより
   衝突しないが、可読性と将来の-Wshadow対策のため呼び出しごとに一意な名前を
   振る)。formが1つも無い場合はnil、1つだけの場合はその式自身をそのまま返す"
  (cond
    ((null forms) "nil")
    ((null (cdr forms)) (transpile-expr (car forms) scope))
    (t (let ((temp (format nil "__or_tmp_~A" (incf *or-temp-counter*))))
         (format nil "({ lisp_val_t ~A = (~A); ~A != nil ? ~A : (~A); })"
                 temp
                 (transpile-expr (car forms) scope)
                 temp
                 temp
                 (transpile-or (cdr forms) scope))))))

(defun transpile-quoted (val)
  "(quote val)のval側。fixnum/string/nil/tはtranspile-exprと同じ扱いで、
   それ以外のシンボルはos_make_symbolで名前から解決する。consは要素ごとに
   再帰的にtranspile-quotedしたものをos_make_consで組み立てる(caseのkeylist
   '(1 2 3)等、リテラルなリストのquote対応。transpile-cons-chainと同じ
   「引数を評価してから呼び出す」C評価順のため、途中でGCが起きても未保護の
   中間値が上位32bit破壊等に晒される窓は無い)"
  (cond
    ((symbolp val)
     (cond
       ((null val) "nil")
       ((eq val t) "g_sym_t")
       (t (format nil "os_make_symbol(~A)" (c-string-literal (symbol-name val))))))
    ((consp val)
     (format nil "os_make_cons(~A, ~A)" (transpile-quoted (car val)) (transpile-quoted (cdr val))))
    (t (transpile-expr val))))

(defun transpile-dynamic-read (expr)
  "(dynamic name)。nameは未評価のシンボルリテラルとして扱い(quoteと同様)、
   os_get_dynamicで動的変数の現在値を取得する式を生成する"
  (format nil "os_get_dynamic(~A)" (transpile-quoted (second expr))))

(defparameter *defdynamic-temp-counter* 0)

(defun transpile-defdynamic (expr scope)
  "(defdynamic name value-form)。value-formをscope内で評価し、GCで移動しうる
   その結果を一旦Cローカル変数へGC_PROTECTしてから、name(未評価のシンボル
   リテラル)をos_make_symbolで解決してos_set_dynamicへ渡す。eval_defdynamicと
   同じくnameそのものを式全体の値として返す(za.cの拡張6と同じ意味論)。
   トランスパイラの対応範囲は非局所脱出(block/return-from/catch/throw)を
   まだ含まないため、eval_defdynamicにあるis_control_transferチェックは
   ここでは省略している"
  (let ((temp (format nil "__defdynamic_val_~A" (incf *defdynamic-temp-counter*)))
        (name-c (transpile-quoted (second expr))))
    (format nil "({ lisp_val_t ~A = ~A; GC_PROTECT(~A); os_set_dynamic(~A, ~A); ~A; })"
            temp (transpile-expr (third expr) scope) temp name-c temp name-c)))

(defparameter *lambda-name-counter* 0)
(defparameter *lifted-lambda-decls* nil
  "現在transpile-defunがdefun本体を走査している間に見つかったlambda式を
   トップレベルのC関数(__step/公開ラッパー)へリフトした結果の文字列を蓄積する
   リスト。transpile-lambdaが自身の生成物をpushする。ネストしたlambdaほど先に
   push されるため、出力時はreverseして定義順(内側から外側)に並べる(Cは
   使用箇所より前に定義または宣言されている必要があるため)。transpile-defunが
   1つのdefunを処理する間だけletで新しい束縛を張る")
(defparameter *closure-temp-counter* 0)

(defun emit-param-binding-stmt (c-var boxed-p)
  "step関数の先頭で、パラメータ1つをevaluated_argsから読み出しCローカル変数
   c-varへ束縛するC文を作る。boxed-pがnon-nilの場合(za.cの拡張4と同じ基準で
   setqされエスケープするlambdaに捕捉されるパラメータ)は、値そのものではなく
   (捨て値 . 実値)のconsをc-varへ束縛する。このconsが以後の全参照(このパラメータ
   自身への読み書きと、ネストしたlambdaが捕捉する際に共有するオブジェクト)で
   共有される「box」そのものになる"
  (if boxed-p
      (format nil "lisp_val_t ~A = os_make_cons(nil, cc_car(evaluated_args)); evaluated_args = cc_cdr(evaluated_args); GC_PROTECT(~A);"
              c-var c-var)
      (format nil "lisp_val_t ~A = cc_car(evaluated_args); evaluated_args = cc_cdr(evaluated_args); GC_PROTECT(~A);"
              c-var c-var)))

;;; M14 基盤B: &restパラメータ。list/append/create-list/apply/mapcar/map-into等が
;;; 使う。呼び出し側(transpile-call/transpile-tail-call)はパラメータの個数に
;;; 関わらず常に実引数を1つのconsチェーン(evaluated_args)として渡すだけなので、
;;; 変更が必要なのは呼び出される側(param-scope-and-preamble)だけでよい:
;;; 固定パラメータをcc_car/cc_cdrで1つずつ剥がした後に残るevaluated_argsは、
;;; 既にそのまま&restパラメータが指すべきリストそのものになっている

(defun split-rest-param (params)
  "PARAMS(defun/lambdaのパラメータリスト)を(values fixed-params rest-param)に
   分割する。&restシンボルが無ければrest-paramはnil。&rest name(nameは
   シンボル1つ)の形式のみ対応する"
  (let ((pos (position '&rest params)))
    (if (null pos)
        (values params nil)
        (let ((rest-tail (nthcdr (1+ pos) params)))
          (unless (and (= (length rest-tail) 1) (symbolp (car rest-tail)))
            (error "split-rest-param: &restの後はパラメータ名1つのみ対応です: ~S" params))
          (values (subseq params 0 pos) (car rest-tail))))))

(defun emit-rest-param-binding-stmt (c-var boxed-p)
  "step関数の先頭で、&restパラメータへ残りのevaluated_args全部をそのまま
   束縛するC文を作る(cc_car/cc_cdrで1つずつ剥がす通常パラメータとは異なり、
   この時点のevaluated_args自体が既に欲しいリストそのものになっている)。
   boxed-pの意味・box化の形はemit-param-binding-stmtと同じ"
  (if boxed-p
      (format nil "lisp_val_t ~A = os_make_cons(nil, evaluated_args); GC_PROTECT(~A);"
              c-var c-var)
      (format nil "lisp_val_t ~A = evaluated_args; GC_PROTECT(~A);"
              c-var c-var)))

(defun emit-capture-fetch-stmt (c-var symbol-name-string)
  "リフトしたlambdaのstep関数の先頭で、自由変数1つを捕捉環境(envパラメータ、
   apply_function/za_ensure_trampolineが定義時の捕捉環境へ差し替え済み)から
   symbol-name-stringという名前のシンボルで検索し、Cローカル変数c-varへ束縛
   するC文を作る。この変数がboxか値コピーかは、捕捉元の外側スコープでの
   boxed-pがそのまま伝播する(呼び出し元のtranspile-lambda参照)ため、ここでは
   os_get_variableが返した値をそのままc-varへ入れるだけでよい"
  (format nil "lisp_val_t ~A = os_get_variable(os_make_symbol(~A), env); GC_PROTECT(~A);"
          c-var (c-string-literal symbol-name-string) c-var))

(defun param-scope-and-preamble (params body)
  "PARAMSとBODY(このパラメータ群だけをスコープに持つLisp式)から、za.cの拡張4と
   同じSBCL方式の変数単位box昇格の判定を行い、(values scope preamble-stmts)を
   返す。scopeはこの関数自身のパラメータ分のalist(シンボル -> (c-name . boxed-p))、
   preamble-stmtsはstep関数の先頭でevaluated_argsから読み出すC文のリスト
   (固定パラメータの並び順、末尾に&restパラメータがあればさらにその後)。
   defun/lambdaのどちらのパラメータ束縛にも共通して使う。&restパラメータ
   (基盤B)はsplit-rest-paramで固定パラメータと切り分け、box昇格判定
   (boxed-params)には固定パラメータと同じ1つの変数として扱わせる"
  (multiple-value-bind (fixed-params rest-param) (split-rest-param params)
    (let* ((all-params (if rest-param (append fixed-params (list rest-param)) fixed-params))
           (boxed (boxed-params body all-params)))
      (values
       (mapcar (lambda (p) (cons p (cons (param-symbol-to-c-name p) (and (member p boxed) t))))
               all-params)
       (append
        (mapcar (lambda (p) (emit-param-binding-stmt (param-symbol-to-c-name p) (and (member p boxed) t)))
                fixed-params)
        (when rest-param
          (list (emit-rest-param-binding-stmt (param-symbol-to-c-name rest-param) (and (member rest-param boxed) t)))))))))

(defun emit-function-body (c-name params preamble-stmts body-form scope)
  "defun/lambdaのどちらにも共通のstep関数+公開ラッパーのC関数定義を組み立てる。
   PARAMSはevaluated_argsを消費するパラメータの並び(空ならevaluated_args自体が
   未使用になるため(void)キャストで警告を抑止する)、PREAMBLE-STMTSはstep関数の
   先頭で実行するC文(パラメータ束縛+lambdaの場合は自由変数の捕捉環境からの
   読み出し)、BODY-FORMは末尾位置として処理する本体式、SCOPEはPREAMBLE-STMTSが
   束縛した全変数(パラメータ+自由変数)を含むalist"
  (let ((tail-stmt (transpile-tail-stmt body-form scope)))
    (with-output-to-string (out)
      (format out "static tco_result_t ~A__step(lisp_val_t evaluated_args, lisp_val_t env) {~%" c-name)
      (when (null params)
        (format out "    (void)evaluated_args;~%"))
      (dolist (stmt preamble-stmts)
        (format out "    ~A~%" stmt))
      (format out "    (void)env;~%")
      (format out "    ~A~%" tail-stmt)
      (format out "}~%~%")
      (format out "lisp_val_t ~A(lisp_val_t evaluated_args, lisp_val_t env) {~%" c-name)
      (format out "    tco_result_t __r = ~A__step(evaluated_args, env);~%" c-name)
      (format out "    while (__r.is_tail_call) {~%")
      (format out "        __r = __r.fn(__r.args, env);~%")
      (format out "    }~%")
      (format out "    return __r.value;~%")
      (format out "}~%"))))

(defun emit-closure-creation (c-name free-vars outer-scope)
  "リフトしたlambda本体(c-name)を、自由変数だけを含む最小限の捕捉環境と共に
   os_make_lifted_closureでラップするC式を作る。自由変数が1つも無ければ環境の
   確保自体が不要なので直接nilを渡す(このマイルストンにはletが無いため、
   パラメータを一切参照しないlambdaは頻出しうる)。自由変数がある場合は、
   os_make_environment(親を持たない、この捕捉専用の環境)を作りGC_PROTECTしたのち、
   各自由変数についてOUTER-SCOPE(このlambda式が出現した時点の外側のscope)での
   現在の値(box化されていればboxそのもの、そうでなければ値そのもの)を
   os_env_add_binding_pairで(sym . 値)ペアとして連結する。boxそのものを共有
   することで、複数のクロージャが同じboxを捕捉した場合に一方の書き換えが
   他方からも見える(za.cの拡張4と同じ設計)。シンボル/consの確保がGCを
   誘発しても既存のOUTER-SCOPEの変数は呼び出し元でGC_PROTECT済みなので安全"
  (if (null free-vars)
      (format nil "os_make_lifted_closure((lisp_addr_t)(void *)~A, nil)" c-name)
      (let ((env-temp (format nil "__closure_env_~A" (incf *closure-temp-counter*))))
        (format nil "({ lisp_val_t ~A = os_make_environment(os_make_symbol(~A), nil); GC_PROTECT(~A); ~{~A~}os_make_lifted_closure((lisp_addr_t)(void *)~A, ~A); })"
                env-temp
                (c-string-literal c-name)
                env-temp
                (mapcar (lambda (v)
                          (let* ((sym-temp (format nil "__closure_sym_~A" (incf *closure-temp-counter*)))
                                 (pair-temp (format nil "__closure_pair_~A" (incf *closure-temp-counter*)))
                                 (outer-c-name (car (cdr (assoc v outer-scope)))))
                            (format nil "lisp_val_t ~A = os_make_symbol(~A); GC_PROTECT(~A); lisp_val_t ~A = os_make_cons(~A, ~A); GC_PROTECT(~A); os_env_add_binding_pair(~A, ~A); "
                                    sym-temp (c-string-literal (symbol-name v)) sym-temp
                                    pair-temp sym-temp outer-c-name
                                    pair-temp
                                    pair-temp env-temp)))
                        free-vars)
                c-name
                env-temp))))

(defun transpile-lambda (expr scope)
  "(lambda (param*) <本体1式>)。M10で導入する第一級(エスケープ可能)クロージャ。
   自由変数(パラメータでも他のdefun名/プリミティブ名でもない裸の変数参照)を
   free-variablesで解析し、外側のSCOPEで解決できなければ未束縛変数として
   エラーにする(transpile-exprの規約と同じ)。本体はdefunと同じ__step/公開
   ラッパーの2関数構成でトップレベルのC関数へリフトし(*lifted-lambda-decls*へ
   push)、使用箇所にはリフト済み関数と捕捉環境をos_make_lifted_closureで
   ラップする式(emit-closure-creation)を返す。パラメータ自身のbox昇格判定は
   defunと同じparam-scope-and-preambleを使い、捕捉した自由変数のbox/値コピーの
   区別は捕捉元(outer scope)のboxed-pをそのまま継承する(boxはこのlambdaが
   新たに決めるものではなく、そのbox化された変数を最初に持つdefun/lambdaが
   一度だけ決める性質だから)"
  (destructuring-bind (lambda-kw params raw-body) expr
    (declare (ignore lambda-kw))
    (unless (every #'symbolp params)
      (error "transpile-lambda: パラメータはシンボルのみ対応です: ~S" expr))
    (let* ((c-name (format nil "__lisp_lambda_~A" (incf *lambda-name-counter*)))
           (body (macroexpand-all raw-body))
           (free-vars (free-variables body params)))
      (dolist (v free-vars)
        (unless (assoc v scope)
          (error "transpile-lambda: 自由変数が外側のスコープで未束縛です: ~S" v)))
      (multiple-value-bind (own-scope own-preamble) (param-scope-and-preamble params body)
        (let* ((capture-scope
                 (mapcar (lambda (v)
                           (let* ((binding (cdr (assoc v scope)))
                                  (outer-boxed-p (cdr binding)))
                             (cons v (cons (format nil "__captured_~A" (param-symbol-to-c-name v))
                                           outer-boxed-p))))
                         free-vars))
               (capture-preamble
                 (mapcar (lambda (v)
                           (emit-capture-fetch-stmt (car (cdr (assoc v capture-scope))) (symbol-name v)))
                         free-vars))
               (body-scope (append own-scope capture-scope))
               (fn-text (emit-function-body c-name params (append own-preamble capture-preamble) body body-scope))
               (closure-expr (emit-closure-creation c-name free-vars scope)))
          (push fn-text *lifted-lambda-decls*)
          closure-expr)))))

(defun call-target-c-name (name)
  "呼び出し先シンボル名からC関数名を解決する。このファイル内でdefunされた
   関数名(*known-function-names*、mainがdefun走査後に束縛する)を優先し、
   次に算術/比較プリミティブのホワイトリスト(*primitive-c-names*)を見る。
   za.cのようなランタイム上のシンボル->Function-Cell解決は行わず、AOTで
   リンクされるC関数を名前で直接呼び出す(呼び出し先アドレスはリンク時に確定
   するため、定義順に関わらず相互再帰も解決できる)"
  (cond
    ((member name *known-function-names*) (lisp-name-to-c-name name))
    ((assoc name *primitive-c-names*) (cdr (assoc name *primitive-c-names*)))
    (t (error "transpile-call: 未対応の呼び出し先です: ~S" name))))

(defun transpile-cons-chain (temps)
  "Cの一時変数名のリストから、末尾がnilのconsチェーンを組み立てるC式を作る
   (evaluated_argsとして呼び出し先に渡す引数リストの構築)"
  (if (null temps)
      "nil"
      (format nil "os_make_cons(~A, ~A)" (car temps) (transpile-cons-chain (cdr temps)))))

(defparameter *call-temp-counter* 0)

(defun transpile-call (expr scope)
  "(name arg*)。nameはdefunされた関数名またはプリミティブのホワイトリストに
   限る(call-target-c-name参照)。各引数はeval.cのeval_argsと同じ考え方で、
   1つずつ評価してすぐGC_PROTECTしてから次の引数を評価する(引数式自体の評価が
   GCを誘発しても、既に評価済みの前の引数がコピーGCで移動済みの古いアドレスを
   指したままにならないようにするため)。引数が無い場合は一時変数もconsチェーンも
   不要なため、直接nilを渡す単純な呼び出し式にする"
  (let* ((name (car expr))
         (args (cdr expr))
         (c-name (call-target-c-name name)))
    (if (null args)
        (format nil "~A(nil, env)" c-name)
        (let ((temps (mapcar (lambda (arg)
                                (declare (ignore arg))
                                (format nil "__call_arg_~A" (incf *call-temp-counter*)))
                              args)))
          (format nil "({ ~{~A~} ~A(~A, env); })"
                  (mapcar (lambda (temp arg)
                            (format nil "lisp_val_t ~A = (~A); GC_PROTECT(~A); "
                                    temp (transpile-expr arg scope) temp))
                          temps args)
                  c-name
                  (transpile-cons-chain temps))))))

(defun tail-return-final (c-expr)
  "末尾位置で、既に確定したC式c-exprの値をそのままtco_result_tとしてreturnする
   Cの文を作る(トランポリンを継続させず、この時点で呼び出し元のwhileループを
   終了させる)"
  (format nil "return (tco_result_t){.is_tail_call = 0, .value = (~A)};" c-expr))

(defun transpile-tail-call (expr scope)
  "末尾位置の(name arg*)で、nameが*known-function-names*に含まれる(=この
   ファイル内でdefunされた)場合にのみ呼ばれる。transpile-callと同様にGC-safeな
   引数一時変数を組み立てるが、実際にはstep関数を呼ばずtco_result_tへ関数
   ポインタと引数consチェーンを詰めてreturnする。呼び出し元のCフレームはここで
   returnして消えるため、何段トランポリンが続いてもCスタックは伸びない"
  (let* ((name (car expr))
         (args (cdr expr))
         (c-name (lisp-name-to-c-name name)))
    (if (null args)
        (format nil "return (tco_result_t){.is_tail_call = 1, .fn = ~A__step, .args = nil};" c-name)
        (let ((temps (mapcar (lambda (arg)
                                (declare (ignore arg))
                                (format nil "__call_arg_~A" (incf *call-temp-counter*)))
                              args)))
          (format nil "{ ~{~A~} return (tco_result_t){.is_tail_call = 1, .fn = ~A__step, .args = ~A}; }"
                  (mapcar (lambda (temp arg)
                            (format nil "lisp_val_t ~A = (~A); GC_PROTECT(~A); "
                                    temp (transpile-expr arg scope) temp))
                          temps args)
                  c-name
                  (transpile-cons-chain temps))))))

(defun transpile-tail-and (forms scope)
  "transpile-andの末尾位置版。最後の式だけが本当の末尾位置(そこに到達した場合の
   値がand全体の値になる)で、それより前の式は真偽判定のためだけに通常評価する
   (nilで短絡した場合の戻り値は常にnilそのものなので、確定値としてreturnする)"
  (cond
    ((null forms) (tail-return-final "g_sym_t"))
    ((null (cdr forms)) (transpile-tail-stmt (car forms) scope))
    (t (format nil "if ((~A) != nil) { ~A } else { ~A }"
               (transpile-expr (car forms) scope)
               (transpile-tail-and (cdr forms) scope)
               (tail-return-final "nil")))))

(defun transpile-tail-or (forms scope)
  "transpile-orの末尾位置版。二重評価を避けるための一時変数はtranspile-orと同じ
   考え方だが、値を返すのがCの式ではなく文になるため、GNUの文(ステートメント)式
   ではなく普通のブロック内if文で構成する。最後の式だけが本当の末尾位置になる"
  (cond
    ((null forms) (tail-return-final "nil"))
    ((null (cdr forms)) (transpile-tail-stmt (car forms) scope))
    (t (let ((temp (format nil "__or_tmp_~A" (incf *or-temp-counter*))))
         (format nil "{ lisp_val_t ~A = (~A); if (~A != nil) { ~A } else { ~A } }"
                 temp
                 (transpile-expr (car forms) scope)
                 temp
                 (tail-return-final temp)
                 (transpile-tail-or (cdr forms) scope))))))

(defun transpile-tail-stmt (expr scope)
  "式exprが末尾位置にあるときの、tco_result_tをreturnするCの文を作る。
   if/progn/and/orは末尾位置を最後の分岐/式へ伝播し、それ以外(リテラル・quote・
   パラメータ参照・setq・プリミティブ呼び出し・未知の呼び出し)はtranspile-expr
   にそのまま委譲して確定値としてreturnする。*known-function-names*に載っている
   関数への呼び出しだけが、transpile-tail-call経由でトランポリン継続になる
   (quoteとsetqは(consp expr)かつ(symbolp (car expr))を満たすが呼び出し先の
   関数名ではないため、known-function-namesに入り得ずここでは自然に除外される)"
  (cond
    ((and (consp expr) (eq (car expr) 'if))
     (destructuring-bind (if-kw test then &optional else) expr
       (declare (ignore if-kw))
       (format nil "if ((~A) != nil) { ~A } else { ~A }"
               (transpile-expr test scope)
               (transpile-tail-stmt then scope)
               (transpile-tail-stmt else scope))))
    ((and (consp expr) (eq (car expr) 'progn))
     (let ((forms (cdr expr)))
       (if (null forms)
           (tail-return-final "nil")
           (format nil "~{(void)(~A); ~}~A"
                   (mapcar (lambda (f) (transpile-expr f scope)) (butlast forms))
                   (transpile-tail-stmt (car (last forms)) scope)))))
    ((and (consp expr) (eq (car expr) 'and))
     (transpile-tail-and (cdr expr) scope))
    ((and (consp expr) (eq (car expr) 'or))
     (transpile-tail-or (cdr expr) scope))
    ((and (consp expr) (symbolp (car expr)) (member (car expr) *known-function-names*))
     (transpile-tail-call expr scope))
    (t (tail-return-final (transpile-expr expr scope)))))

(defun transpile-prototype (form)
  "(defun name (param*) body) から、mainがbody生成前に出力する前方宣言を作る。
   相互再帰時、defunの並び順に関わらずどちらのC関数からも他方を呼べるようにする
   (za.cのシンボル解決に頼らない代わりに、AOTのC前方宣言で解決する)。__step版は
   ファイル内実装詳細(トランポリンのジャンプ先)なのでstaticにする"
  (let ((c-name (lisp-name-to-c-name (second form))))
    (format nil "static tco_result_t ~A__step(lisp_val_t evaluated_args, lisp_val_t env);~%lisp_val_t ~A(lisp_val_t evaluated_args, lisp_val_t env);"
            c-name c-name)))

(defun transpile-defun (form)
  "(defun name (param*) <本体1式>) に対応する。パラメータはシンボルのみ
   (末尾の&rest nameのみ許容、基盤B/param-scope-and-preamble参照)、
   bodyは単一式のみ(fixnum/string/nil/tリテラル・quote・
   パラメータの裸参照・M10のlambda等)。za.cのネイティブABI(lisp_val_t fn(
   lisp_val_t evaluated_args, lisp_val_t env))に合わせ、パラメータ束縛はza.cと
   同様にevaluated_argsをcc_car/cc_cdrで辿って行う(param-scope-and-preamble/
   emit-function-body参照。setqされ、かつ本体中のlambdaに捕捉されるパラメータは
   box化される=M10でdefunパラメータもlambdaと同じbox昇格の対象になる)。

   1つのdefunから2つのC関数を生成する: 本体を1手だけ進めるstep関数
   (末尾位置の既知関数呼び出しをトランポリン継続としてreturnする)と、
   ABI互換の公開ラッパー(stepをwhileループで回し切ってlisp_val_tを返す)。
   これにより自己/相互再帰の末尾呼び出しがCの再帰呼び出しにならず、再帰段数に
   関わらずCスタック消費が一定になる(ファイル先頭のコメント参照)。

   本体の中にlambdaが直接またはネストして現れる場合、それらはこのdefunより先に
   トップレベルのC関数として出力する必要があるため、*lifted-lambda-decls*を
   このdefun専用にletで束縛し、transpile-lambdaが積んだ結果を(内側から外側の
   順で)このdefun自身のC関数定義の前に連結する"
  (destructuring-bind (defun-kw name params &rest body) form
    (declare (ignore defun-kw))
    (unless (every #'symbolp params)
      (error "transpile-defun: パラメータはシンボルのみ対応です: ~S" name))
    (unless (= (length body) 1)
      (error "transpile-defun: bodyは単一式のみ対応です: ~S" name))
    (let* ((c-name (lisp-name-to-c-name name))
           (*lifted-lambda-decls* nil)
           (expanded-body (macroexpand-all (first body))))
      (multiple-value-bind (scope preamble) (param-scope-and-preamble params expanded-body)
        (let ((fn-text (emit-function-body c-name params preamble expanded-body scope)))
          (format nil "~{~A~%~}~A" (reverse *lifted-lambda-decls*) fn-text))))))

(defun read-all-forms (path)
  (with-open-file (in path)
    (loop for form = (read in nil :eof)
          until (eq form :eof)
          collect form)))

(defun emit-aot-registration (aot-defuns)
  "init_aot.lisp由来のdefun群それぞれについて、os_set_function/
   os_make_native_functionでglobal_environmentへ登録するC関数
   os_register_aot_init_functionsを生成する(M13)。init.lispから該当defunの
   テキストを取り除いた後も、インタプリタ側から同じシンボル名でこれらの
   AOTコンパイル済み関数を呼び出せるようにするための配線"
  (format nil "void os_register_aot_init_functions(void) {~%~{    os_set_function(os_make_symbol(\"~A\"), os_make_native_function((lisp_addr_t)(void *)~A), global_environment);~%~}}~%"
          (mapcan (lambda (form)
                    (list (symbol-name (second form)) (lisp-name-to-c-name (second form))))
                  aot-defuns)))

(defun main ()
  (let* ((fixture-defuns (remove-if-not (lambda (form) (and (consp form) (eq (car form) 'defun)))
                                         (read-all-forms *runtime-lisp-path*)))
         (aot-defuns (remove-if-not (lambda (form) (and (consp form) (eq (car form) 'defun)))
                                     (read-all-forms *aot-lisp-path*)))
         (defuns (append fixture-defuns aot-defuns))
         (*known-function-names* (mapcar #'second defuns))
         (prototypes (mapcar #'transpile-prototype defuns))
         (bodies (mapcar #'transpile-defun defuns))
         (registration (emit-aot-registration aot-defuns)))
    (with-open-file (out *output-c-path* :direction :output :if-exists :supersede)
      ;; funcall(primitive_funcall)はeval.hで宣言されているため、runtime.h/lisp.hだけでは
      ;; 暗黙のint宣言(実体はlisp_val_t=64bitを返すため上位32bitが失われ得る)になってしまう
      (format out "#include \"runtime.h\"~%#include \"lisp.h\"~%#include \"eval.h\"~%~%")
      ;; 末尾呼び出しのトランポリン継続を表す型。is_tail_call=0ならvalueが確定値、
      ;; 1ならfn/argsが「次にこのstep関数をこの引数で呼ぶ」ことを表す(実際の呼び出し
      ;; は各defunの公開ラッパーのwhileループが行う。ファイル先頭のコメント参照)
      (format out "typedef struct tco_result tco_result_t;~%")
      (format out "typedef tco_result_t (*step_fn_t)(lisp_val_t, lisp_val_t);~%")
      (format out "struct tco_result {~%    int is_tail_call;~%    lisp_val_t value;~%    step_fn_t fn;~%    lisp_val_t args;~%};~%~%")
      (dolist (p prototypes)
        (format out "~A~%" p))
      (format out "~%")
      (dolist (b bodies)
        (format out "~A~%" b))
      (format out "~%~A" registration))))
