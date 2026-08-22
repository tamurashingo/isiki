;;;; ホスト(CommonLisp)側で実行するトランスパイラ
;;;;
;;;; M8時点でサポートするのは、fixnum/string/symbol/nil/tのリテラルとquote、
;;;; defunパラメータ(クロージャなしのローカル変数)の参照・setq、
;;;; if/progn/and/orを組み合わせた単一の本体式、および生成する関数のパラメータを
;;;; GC_PROTECTでshadow stackへ登録するコード生成のみ。let/cond/case等
;;;; (マクロ展開/自由変数捕捉等)は後続のマイルストンで拡張する。

(defparameter *runtime-lisp-path* "src/lisp/transpile_fixture.lisp")
(defparameter *output-c-path* "src/c/lisp_compiled.c")

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

(defun transpile-defun (form)
  "(defun name (param*) <本体1式>) に対応する。パラメータはシンボルのみ
   (&rest等は未対応)、bodyは単一式のみ(fixnum/string/nil/tリテラル・quote・
   パラメータの裸参照)。za.cのネイティブABI(lisp_val_t fn(lisp_val_t
   evaluated_args, lisp_val_t env))に合わせ、パラメータ束縛はza.cと同様に
   evaluated_argsをcc_car/cc_cdrで辿って行う"
  (destructuring-bind (defun-kw name params &rest body) form
    (declare (ignore defun-kw))
    (unless (every #'symbolp params)
      (error "transpile-defun: パラメータはシンボルのみ対応です(&restは未対応): ~S" name))
    (unless (= (length body) 1)
      (error "transpile-defun: bodyは単一式のみ対応です: ~S" name))
    (let* ((c-name (lisp-name-to-c-name name))
           (scope (mapcar (lambda (p) (cons p (param-symbol-to-c-name p))) params))
           (body-c (transpile-expr (first body) scope)))
      (with-output-to-string (out)
        (format out "lisp_val_t ~A(lisp_val_t evaluated_args, lisp_val_t env) {~%" c-name)
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
        (format out "    return ~A;~%" body-c)
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
         (bodies (mapcar #'transpile-defun defuns)))
    (with-open-file (out *output-c-path* :direction :output :if-exists :supersede)
      (format out "#include \"runtime.h\"~%#include \"lisp.h\"~%~%")
      (dolist (b bodies)
        (format out "~A~%" b)))))
