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
