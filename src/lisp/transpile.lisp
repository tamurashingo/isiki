;;;; ホスト(CommonLisp)側で実行するトランスパイラ
;;;;
;;;; M9時点でサポートするのは、fixnum/string/symbol/nil/tのリテラルとquote、
;;;; defunパラメータ(クロージャなしのローカル変数)の参照・setq、
;;;; if/progn/and/orを組み合わせた単一の本体式、このファイル内でdefunされた
;;;; 関数同士の自己/相互再帰呼び出し(za.cのような都度のシンボル名解決は行わず、
;;;; AOTでリンクされるC関数を直接呼び出す)、および生成する関数のパラメータを
;;;; GC_PROTECTでshadow stackへ登録するコード生成のみ。let/cond/case等
;;;; (マクロ展開/自由変数捕捉等)は後続のマイルストンで拡張する。
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
(defparameter *output-c-path* "src/c/lisp_compiled.c")

(defparameter *known-function-names* nil
  "現在のトランスパイル対象ファイル内でdefunされている関数名の一覧。mainが
   全defunを読み終えた時点で束縛し、transpile-callが呼び出し先を解決する際に
   参照する(自己/相互再帰が定義順に関係なく解決できるようにするため)")

(defparameter *primitive-c-names*
  ;; 自己再帰の停止条件(カウントダウン等)を書くために最低限必要な算術/比較
  ;; プリミティブのみ対応する。これらはruntime.cのprimitive_*が生成関数と同じ
  ;; ABI(lisp_val_t fn(lisp_val_t args, lisp_val_t env))で既に実装済みのC関数で、
  ;; 呼び出し先アドレスはリンク時に確定するため、defunされた関数と同じ「直接
  ;; 呼び出し」方式で扱える
  '((- . "primitive_subtract")
    (eq . "primitive_eq")))

(defun sanitize-c-ident (name)
  "MEM-REF-64 -> mem_ref_64 (Cの識別子として使える形にする)"
  (remove #\! (substitute #\_ #\- (string-downcase name))))

(defun lisp-name-to-c-name (symbol)
  "&&mem-ref-64 -> lisp_ll_mem_ref_64"
  (let* ((name (symbol-name symbol))
         (prefix-len (cond
                       ((and (>= (length name) 2)
                             (string= (subseq name 0 2) "%%"))
                        2)
                       ((and (plusp (length name))
                             (char= (char name 0) #\%))
                        1)
                       (t 0)))
         (c-prefix (case prefix-len
                     (2 "lisp_ll_")
                     (1 "lisp_")
                     (t "")))
         (body (sanitize-c-ident (subseq name prefix-len))))
    (concatenate 'string c-prefix body)))

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

(declaim (ftype function transpile-quoted))
(declaim (ftype function transpile-if))
(declaim (ftype function transpile-progn))
(declaim (ftype function transpile-setq))
(declaim (ftype function transpile-and))
(declaim (ftype function transpile-or))
(declaim (ftype function transpile-call))
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
   scopeはdefunパラメータ名(シンボル)からCのローカル変数名へのalist"
  (cond
    ((integerp expr)
     (format nil "os_make_fixnum(~AULL)" expr))
    ((stringp expr)
     (format nil "os_make_string(~A)" (c-string-literal expr)))
    ((null expr) "nil")
    ((eq expr t) "g_sym_t")
    ((and (symbolp expr) (assoc expr scope))
     (cdr (assoc expr scope)))
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
    ((and (consp expr) (symbolp (car expr)))
     (transpile-call expr scope))
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
  "(setq var val)。varはscope内の束縛済みローカル変数(defunパラメータ)のみ
   対応する(このマイルストンの対象はクロージャ無しのローカル変数)。Cの代入式は
   代入後の値そのものに評価されるため、追加の処理無くLispのsetqの戻り値規約と一致する"
  (let* ((var (second expr))
         (val (third expr))
         (binding (assoc var scope)))
    (unless binding
      (error "transpile-setq: setqの対象が未束縛のローカル変数です: ~S" var))
    (format nil "(~A = ~A)" (cdr binding) (transpile-expr val scope))))

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
   それ以外のシンボルはos_make_symbolで名前から解決する"
  (cond
    ((symbolp val)
     (cond
       ((null val) "nil")
       ((eq val t) "g_sym_t")
       (t (format nil "os_make_symbol(~A)" (c-string-literal (symbol-name val))))))
    (t (transpile-expr val))))

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
   (&rest等は未対応)、bodyは単一式のみ(fixnum/string/nil/tリテラル・quote・
   パラメータの裸参照)。za.cのネイティブABI(lisp_val_t fn(lisp_val_t
   evaluated_args, lisp_val_t env))に合わせ、パラメータ束縛はza.cと同様に
   evaluated_argsをcc_car/cc_cdrで辿って行う。

   1つのdefunから2つのC関数を生成する: 本体を1手だけ進めるstep関数
   (末尾位置の既知関数呼び出しをトランポリン継続としてreturnする)と、
   ABI互換の公開ラッパー(stepをwhileループで回し切ってlisp_val_tを返す)。
   これにより自己/相互再帰の末尾呼び出しがCの再帰呼び出しにならず、再帰段数に
   関わらずCスタック消費が一定になる(ファイル先頭のコメント参照)"
  (destructuring-bind (defun-kw name params &rest body) form
    (declare (ignore defun-kw))
    (unless (every #'symbolp params)
      (error "transpile-defun: パラメータはシンボルのみ対応です(&restは未対応): ~S" name))
    (unless (= (length body) 1)
      (error "transpile-defun: bodyは単一式のみ対応です: ~S" name))
    (let* ((c-name (lisp-name-to-c-name name))
           (scope (mapcar (lambda (p) (cons p (param-symbol-to-c-name p))) params))
           (tail-stmt (transpile-tail-stmt (first body) scope)))
      (with-output-to-string (out)
        (format out "static tco_result_t ~A__step(lisp_val_t evaluated_args, lisp_val_t env) {~%" c-name)
        (if (null params)
            (format out "    (void)evaluated_args;~%")
            (dolist (p params)
              (let ((c-var (cdr (assoc p scope))))
                (format out "    lisp_val_t ~A = cc_car(evaluated_args);~%" c-var)
                (format out "    evaluated_args = cc_cdr(evaluated_args);~%")
                ;; bodyの評価中のヒープ確保(os_make_string等)がGCを誘発しても、
                ;; パラメータがコピーGCで移動済みの古いアドレスを指したままに
                ;; ならないよう、束縛直後にGC_PROTECTでshadow stackへ登録する
                ;; (eval.cのGC_PROTECT(args)/GC_PROTECT(env)と同じ考え方)
                (format out "    GC_PROTECT(~A);~%" c-var))))
        (format out "    (void)env;~%")
        (format out "    ~A~%" tail-stmt)
        (format out "}~%~%")
        (format out "lisp_val_t ~A(lisp_val_t evaluated_args, lisp_val_t env) {~%" c-name)
        (format out "    tco_result_t __r = ~A__step(evaluated_args, env);~%" c-name)
        ;; __rがトランポリン継続を指す間は、stepをreturn/callで往復するだけの
        ;; フラットなループになる(Cの呼び出しが必ずreturnしてから次のstepが
        ;; 呼ばれるため、GC_PROTECTのshadow stack登録もcleanup属性で各step呼び出し
        ;; ごとに片付き、何回ループしても蓄積しない)
        (format out "    while (__r.is_tail_call) {~%")
        (format out "        __r = __r.fn(__r.args, env);~%")
        (format out "    }~%")
        (format out "    return __r.value;~%")
        (format out "}~%")))))

(defun read-all-forms (path)
  (with-open-file (in path)
    (loop for form = (read in nil :eof)
          until (eq form :eof)
          collect form)))

(defun main ()
  (let* ((forms (read-all-forms *runtime-lisp-path*))
         (defuns (remove-if-not (lambda (form) (and (consp form) (eq (car form) 'defun)))
                                 forms))
         (*known-function-names* (mapcar #'second defuns))
         (prototypes (mapcar #'transpile-prototype defuns))
         (bodies (mapcar #'transpile-defun defuns)))
    (with-open-file (out *output-c-path* :direction :output :if-exists :supersede)
      (format out "#include \"runtime.h\"~%#include \"lisp.h\"~%~%")
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
        (format out "~A~%" b)))))
