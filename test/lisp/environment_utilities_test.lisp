;; test/lisp/environment_utilities_test.lisp
;;
;; Phase4(documents/environment.md、Environment操作ユーティリティ)の動作確認。
;; make-environment/switch-environment/with-environment/list-environments/
;; destroy-environmentの一連の流れ(一時環境作成→シャドウイング→実行確認→破棄→
;; 元定義への復帰)、および使用中の環境自身・祖先環境の破棄を試みてエラーになる
;; ことを確認する(Q3)。
;;
;; 本ファイルはtest/lisp/test_framework.lispが定義するassert-equal等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。
;; defparameterはこのコードベースに存在しないため、代わりにdefglobal(常に評価して
;; current environmentへ書き込む特殊形式)を使う。

;;; --- 1. 一時環境の作成、シャドウイング、破棄、元定義への復帰 ---

(defun eutil-shadow-fn (x) (+ x 1))
(assert-equal 6 (eutil-shadow-fn 5))

(defglobal eutil-temp-env (make-environment 'eutil-temp))

;; list-environmentsに登録されている(list-environmentsはenvそのものではなく
;; 環境名symbolの一覧を返すため、名前で確認する)
(assert-equal t (if (member 'eutil-temp (list-environments)) t nil))

;; 一時環境へ切り替えて同名関数を再定義すると、その環境内でシャドウイングされる
(with-environment eutil-temp-env
  (defun eutil-shadow-fn (x) (+ x 100))
  (assert-equal 105 (eutil-shadow-fn 5)))

;; with-environmentを抜けたら元の環境の定義に戻っている(shadowingはeutil-temp-envの
;; functionsスロットにのみ書き込まれ、このトップレベルフォーム自体はeutil-temp-envの
;; 外側の環境で評価されるため、そもそも影響を受けていない)
(assert-equal 6 (eutil-shadow-fn 5))

;; 破棄すると成功しt、list-environmentsからも消える
(assert-equal t (destroy-environment eutil-temp-env))
(assert-equal nil (if (member 'eutil-temp (list-environments)) t nil))

;;; --- 2. 使用中の環境自身の破棄はエラーになり、拒否される(フォールバックしない) ---

(assert-equal nil (ignore-errors (destroy-environment (%%current-environment))))

;; 拒否された後も現在の環境は壊れておらず、通常の評価が継続できる
(assert-equal 6 (eutil-shadow-fn 5))
(assert-equal 7 (eutil-shadow-fn 6))

;;; --- 3. 祖先環境の破棄もエラーになる(現在の環境が子環境の場合) ---

(defglobal eutil-env-a (make-environment 'eutil-a))
(defglobal eutil-env-b (make-environment 'eutil-b eutil-env-a))

(with-environment eutil-env-b
  (assert-equal nil (ignore-errors (destroy-environment eutil-env-a)))
  ;; 拒否された後もeutil-env-a自身は生きており、子環境(現在の環境)からの評価も継続できる
  (assert-equal 6 (eutil-shadow-fn 5)))

;; with-environmentを抜けた後、a/bともまだlist-environmentsに残っている(拒否されたため破棄されていない)
(assert-equal t (if (member 'eutil-a (list-environments)) t nil))
(assert-equal t (if (member 'eutil-b (list-environments)) t nil))

;; 子(葉)から先に破棄すれば、どちらも使用中でも祖先でもないため正常に破棄できる
(assert-equal t (destroy-environment eutil-env-b))
(assert-equal t (destroy-environment eutil-env-a))
(assert-equal nil (if (member 'eutil-a (list-environments)) t nil))
(assert-equal nil (if (member 'eutil-b (list-environments)) t nil))

;;; --- 4. switch-environment: env自体(cons)、名前symbol、名前string(いずれも登録済み) ---

;; 元の環境に戻すためのcons自体を保存しておく(名前でのlookupはこの後の確認対象
;; なので、戻り先の判定には使わない)
(defglobal eutil-sw-saved-env (%%current-environment))
(defglobal eutil-sw-env (make-environment 'eutil-sw))

;; consを渡した場合: 環境そのものとしてそのままセットし、戻り値は環境名symbol
(assert-equal 'eutil-sw (switch-environment eutil-sw-env))
(assert-equal t (eq eutil-sw-env (%%current-environment)))
(switch-environment eutil-sw-saved-env)

;; symbolを渡した場合: list-environmentsに表示される名前として検索してセットする
(assert-equal 'eutil-sw (switch-environment 'eutil-sw))
(assert-equal t (eq eutil-sw-env (%%current-environment)))
(switch-environment eutil-sw-saved-env)

;; stringを渡した場合: symbolに変換した上で同様に検索してセットする
(assert-equal 'eutil-sw (switch-environment "eutil-sw"))
(assert-equal t (eq eutil-sw-env (%%current-environment)))
(switch-environment eutil-sw-saved-env)

;; 未登録の名前を渡した場合はエラーになる(フォールバックしない)
(assert-equal nil (ignore-errors (switch-environment 'eutil-no-such-env)))

(destroy-environment eutil-sw-env)
