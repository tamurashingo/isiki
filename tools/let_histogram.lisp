;;;; [調査] let / let* の束縛数(幅)と入れ子段数(深さ)のヒストグラム。
;;;; documents/jit-frame-survey.md §3。
;;;;
;;;; **ソース上の let を数えるのではなく、マクロ展開後の形で数える。**
;;;; JIT が見るのは展開後の IIFE 形 ((lambda (v1 v2...) . body) init...) であり、
;;;; za_compile_let が ZA_MAX_LOCALS_PER_LET(仮引数の個数)と
;;;; ZA_MAX_LET_DEPTH(入れ子段数)で判定する対象もそれである。
;;;;
;;;; 展開は src/lisp/transpile.lisp の macroexpand-all を使うが、
;;;; **expand-for だけ init.lisp 側の形(一時変数を入れ子の let に置く)へ差し替える。**
;;;; AOT 側は 1 つの let に append するので、そのままでは JIT が見る形と幅が違う
;;;; (3 変数の for が「幅 6・深さ 1」ではなく「幅 3・深さ 2」になる)。
;;;;
;;;; 数え方の約束(**undercount する方向に倒してある**):
;;;;   - defmacro のトップレベルフォームは**数えない。** 本体はテンプレートであって
;;;;     コードではなく、中の (let ...) を展開すると実在しない let を数えてしまう
;;;;   - SB-INT:QUASIQUOTE の中は**展開も走査もしない**(同じ理由)。
;;;;     テンプレート内の unquote されたコードに let があれば取りこぼす
;;;;   - 展開中にエラーが出たフォームは件数だけ数えて飛ばす
;;;;
;;;; 出力(TSV、1 行 1 件):
;;;;   IIFE <幅> <深さ> <ファイル> <関数名>
;;;;   SKIP <理由> <ファイル> <関数名>
;;;;   FILE <ファイル> <トップレベルフォーム数>

(load "src/lisp/transpile.lisp")

;;; expand-for を init.lisp 側の形へ差し替える(上のコメント参照)。
;;; 一時変数は固定名のプールから取る点も init.lisp と同じにする。
(defun %%survey-for-temp-names (bindings)
  (loop for i from 1 to (length bindings)
        collect (intern (format nil "%FOR-TMP-~A" i))))

(defun expand-for (form)
  (destructuring-bind (for-kw bindings test-and-result &rest body) form
    (declare (ignore for-kw))
    (let ((temps (%%survey-for-temp-names bindings)))
      `(let ,(%%for-let-bindings bindings)
         (let ,(mapcar (lambda (tv) (list tv nil)) temps)
           (block nil
             (tagbody
              %for-loop
              (if ,(car test-and-result)
                  (return-from nil (progn ,@(cdr test-and-result)))
                  (progn
                    ,@body
                    ,@(%%for-step-setqs bindings temps)
                    ,@(%%for-commit-setqs bindings temps)
                    (go %for-loop))))))))))

(defparameter *qq* (find-symbol "QUASIQUOTE" "SB-INT"))

(defun flat-symbol-list-length (vars)
  "仮引数が「重複のない平坦なシンボルのみ・&rest 無し」なら個数、そうでなければ nil
   (za_local_validate_lambda_vars と同じ判定)。"
  (let ((n 0) (seen '()))
    (loop for cur = vars then (cdr cur)
          while cur
          do (unless (consp cur) (return-from flat-symbol-list-length nil))
             (let ((s (car cur)))
               (unless (and (symbolp s) s (not (string= (symbol-name s) "&REST")))
                 (return-from flat-symbol-list-length nil))
               (when (member s seen) (return-from flat-symbol-list-length nil))
               (push s seen) (incf n)))
    n))

(defparameter *hits* nil)

(defun walk (form depth file name)
  (cond
    ((not (consp form)) nil)
    ((eq (car form) 'quote) nil)
    ((and *qq* (eq (car form) *qq*)) nil)      ; テンプレートの中は見ない
    ;; IIFE: ((lambda (v...) . body) init...)
    ((and (consp (car form)) (eq (caar form) 'lambda))
     (let* ((lam (car form))
            (vars (second lam))
            (w (flat-symbol-list-length vars)))
       (if w
           (progn
             (push (list w (1+ depth) file name) *hits*)
             ;; body は 1 段深く、init は同じ深さ
             (dolist (b (cddr lam)) (walk b (1+ depth) file name))
             (dolist (a (cdr form)) (walk a depth file name)))
           ;; let-IIFE として受け付けられない形(&rest 等)。深さは増やさない
           (progn
             (push (list :invalid (1+ depth) file name) *hits*)
             (dolist (x form) (walk x depth file name))))))
    (t (dolist (x form) (walk x depth file name)))))

(defparameter *files*
  (append (directory "src/lisp/*.lisp") (directory "test/lisp/*.lisp")))

(let ((*print-case* :upcase) (*print-pretty* nil))
  (dolist (path *files*)
    (let ((rel (enough-namestring path (truename "."))) (n 0))
      (handler-case
          (dolist (form (read-all-forms path))
            (incf n)
            (cond
              ((not (consp form)) nil)
              ((eq (car form) 'defmacro)
               (format t "~&SKIP~Cdefmacro~C~A~C~A~%" #\Tab #\Tab rel #\Tab (second form)))
              (t
               (let ((name (if (member (car form) '(defun)) (second form) (car form))))
                 (handler-case
                     (let ((*hits* nil))
                       (walk (macroexpand-all form) 0 rel name)
                       (dolist (h (reverse *hits*))
                         (format t "~&IIFE~C~A~C~A~C~A~C~A~%"
                                 #\Tab (first h) #\Tab (second h) #\Tab
                                 (third h) #\Tab (fourth h))))
                   (error (e) (declare (ignore e))
                     (format t "~&SKIP~Cexpand-error~C~A~C~A~%" #\Tab #\Tab rel #\Tab name)))))))
        (error (e) (declare (ignore e))
          (format t "~&SKIP~Cread-error~C~A~C-~%" #\Tab #\Tab rel #\Tab)))
      (format t "~&FILE~C~A~C~A~%" #\Tab rel #\Tab n))))
