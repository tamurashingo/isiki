;;;; ホスト(CommonLisp)側で実行するトランスパイラ
;;;;
;;;; M3時点でサポートするのは「引数なし・本体がfixnum/string/symbol/nil/t
;;;; リテラルまたはそれらのquote 1個のdefun」のみ。それ以外の構文
;;;; (macro展開/自由変数捕捉/GC_PROTECT統合等)は後続のマイルストンで拡張する。

(defparameter *runtime-lisp-path* "src/lisp/transpile_fixture.lisp")
(defparameter *output-c-path* "src/c/lisp_compiled.c")

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
         (body (remove #\! (substitute #\_ #\- (string-downcase (subseq name prefix-len))))))
    (concatenate 'string c-prefix body)))

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

(defun transpile-expr (expr)
  "fixnum/string/nil/tの裸リテラルと、それらに対するquoteおよびシンボルの
   quoteに対応する。GCで移動しうる値(symbol/string)は、za.cのT/quoteシンボル
   リテラル対応と同じく、生ポインタを埋め込まずos_make_symbol/os_make_stringを
   都度呼んで解決する。nilはランタイムがGCで移動しない固定センチネルなので
   externグローバルnilをそのまま参照してよい"
  (cond
    ((integerp expr)
     (format nil "os_make_fixnum(~AULL)" expr))
    ((stringp expr)
     (format nil "os_make_string(~A)" (c-string-literal expr)))
    ((null expr) "nil")
    ((eq expr t) "g_sym_t")
    ((and (consp expr) (eq (car expr) 'quote) (= (length expr) 2))
     (transpile-quoted (second expr)))
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
  "(defun name () <fixnumリテラル>) のみ対応する"
  (destructuring-bind (defun-kw name params &rest body) form
    (declare (ignore defun-kw))
    (unless (null params)
      (error "transpile-defun: 引数付きdefunは未対応です: ~S" name))
    (unless (= (length body) 1)
      (error "transpile-defun: bodyは単一式のみ対応です: ~S" name))
    (let ((c-name (lisp-name-to-c-name name)))
      (format nil "lisp_val_t ~A(lisp_val_t args, lisp_val_t env) {~%    (void)args;~%    (void)env;~%    return ~A;~%}~%"
              c-name (transpile-expr (first body))))))

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
      (format out "#include \"runtime.h\"~%~%")
      (dolist (b bodies)
        (format out "~A~%" b)))))
