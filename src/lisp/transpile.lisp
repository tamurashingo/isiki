;;;; ホスト(CommonLisp)側で実行するトランスパイラ
;;;;
;;;; M2時点でサポートするのは「引数なし・本体が固定fixnumリテラル1個の
;;;; defun」のみ。それ以外の構文(macro展開/自由変数捕捉/GC_PROTECT統合等)は
;;;; 後続のマイルストンで拡張する。

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

(defun transpile-expr (expr)
  "現時点で対応するのは固定fixnumリテラルのみ(61bit以上の値は非対応)"
  (cond
    ((integerp expr)
     (format nil "os_make_fixnum(~AULL)" expr))
    (t (error "transpile-expr: 未対応の式です: ~S" expr))))

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
