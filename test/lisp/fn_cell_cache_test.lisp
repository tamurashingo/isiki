;; test/lisp/fn_cell_cache_test.lisp
;;
;; JITの呼び出し先キャッシュ(za_emit_fn_resolve_cached)が、未定義を表すnilを
;; キャッシュしてしまわないことの回帰テスト。
;;
;; 生成コードはキャッシュスロットに入っている値が0かどうかでキャッシュの有無を
;; 判定する。nilは0ではないタグ付きヒープ値なので、os_get_function_cellが
;; 未定義に対して返したnilをそのままストアすると、以後ずっと「解決済み」と
;; 見なされ、呼び出し先を後から定義しても永久に未定義のままになる。
;;
;; documents/jit.md の「前方参照・再定義がそのまま安全に動きます」が
;; 実際に成立することを、この形で常時確認する。

;; 呼び出し先がまだ存在しない状態で呼び出し元を定義してコンパイルさせる
(defun fcc-early-caller () (fcc-not-yet-defined))
(assert-equal t (%%za-compiled-p (function fcc-early-caller)))

;; 未定義のまま呼ぶ(ここでキャッシュスロットが埋まりうる)。
;; 未定義の関数呼び出しはconditionをsignalせず、シンボルEVAL-ERRORを値として返す
;; (apply_functionがg_sym_eval_errorを返す。runtime.c/eval.c)
;; [P4-3] 未定義関数は <undefined-function> を signal する(spec:1461 / spec:7261-7263)
(assert-error-class '<undefined-function> (fcc-early-caller))

;; あとから定義すれば解決されなければならない
(defun fcc-not-yet-defined () 99)
(assert-equal 99 (fcc-early-caller))

;; 2回目以降も安定して同じ結果になること(今度はキャッシュが効いている)
(assert-equal 99 (fcc-early-caller))

;; 定義済みの呼び出し先を再定義したら追従すること(Function Cell経由の間接呼び出し)
(defun fcc-not-yet-defined () 100)
(assert-equal 100 (fcc-early-caller))

;; 対照: 先に定義してから呼び出し元を定義する順序(従来から動いていた経路)
(defun fcc-callee () 1)
(defun fcc-caller () (fcc-callee))
(assert-equal 1 (fcc-caller))
(defun fcc-callee () 2)
(assert-equal 2 (fcc-caller))

;; 未定義のまま複数回呼んでも、あとから定義すれば解決されること
(defun fcc-early2 () (fcc-later2))
;; [P4-3] 同上。**3回とも同じクラスになること**(未定義の Function Cell を
;; キャッシュしてしまうと2回目以降の挙動が変わるので、回数ぶん見る)
(assert-error-class '<undefined-function> (fcc-early2))
(assert-error-class '<undefined-function> (fcc-early2))
(assert-error-class '<undefined-function> (fcc-early2))
(defun fcc-later2 () 7)
(assert-equal 7 (fcc-early2))
