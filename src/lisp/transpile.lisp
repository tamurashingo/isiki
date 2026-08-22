;;;; ホスト(CommonLisp)側で実行するトランスパイラ
;;;;
;;;; M3時点でサポートするのは「引数なし・本体がfixnum/string/symbol/nil/t
;;;; リテラルまたはそれらのquote 1個のdefun」のみ。それ以外の構文
;;;; (macro展開/自由変数捕捉/GC_PROTECT統合等)は後続のマイルストンで拡張する。

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
    ((symbolp expr)
     (error "transpile-expr: 未束縛の変数参照です: ~S" expr))
    (t (error "transpile-expr: 未対応の式です: ~S" expr))))

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
              (format out "    lisp_val_t ~A = cc_car(evaluated_args);~%" (cdr (assoc p scope)))
              (format out "    evaluated_args = cc_cdr(evaluated_args);~%")))
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
