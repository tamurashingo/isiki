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

;;; --- setf ---

;; (setf place value) を place の形に応じて setq/set-car/set-cdr/set-arefに展開する。
;; placeがsymbolならsetq、(car x)ならset-car、(cdr x)ならset-cdr、
;; (aref array i1 i2 ...)ならset-arefに展開する。
(defmacro setf (place value)
  (cond ((symbolp place) `(setq ,place ,value))
        ((eq (car place) 'car) `(set-car ,(car (cdr place)) ,value))
        ((eq (car place) 'cdr) `(set-cdr ,(car (cdr place)) ,value))
        ((eq (car place) 'aref) `(set-aref ,@(cdr place) ,value))))

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
