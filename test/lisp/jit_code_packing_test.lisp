;; test/lisp/jit_code_packing_test.lisp
;;
;; JIT コードのパッキング(runtime.c の os_imm_code_alloc)の回帰テスト。
;;
;; 以前は za_try_compile_defun がコード長を 4,096 byte へ切り上げて丸ごとページを
;; 専有していたため、200 byte の関数でも 1 ページを使っていた。実測の充填率は 37.9%、
;; defun 1回あたり 4,160 byte で、約 3,900 回で
;; os_panic("immobilized space exhausted") に到達していた
;; (documents/measurement-jit-code-length-distribution.md)。
;;
;; パッキングは環境ごとの bump カーソルで必要な byte 数だけ切り出す。
;; **1ページは必ず1つの環境に属する。** destroy-environment がページ単位で回収し
;; (os_environment_reclaim_pages → os_imm_page_free)、os_imm_page_free は解放した
;; ページの先頭 8 byte を フリーリストの next ポインタで上書きするため、
;; 環境をまたいで同居させると生きているコードが壊れる。
;;
;; %%DIAG-CODE-PACKING は (カーソルから ページ新規 複数ページ 分断回数 分断byte) を返す。

(defun jcp-used () (%%imm-space-used-bytes))
(defun jcp-page-of (fn) (div (%%disasm-code-base fn) 4096))

;;; --- 1. 同一環境の複数の関数が1ページに同居すること ---
;;
;; 旧実装ならこの4関数で4ページ(16,384 byte)を専有していた。
(defglobal *jcp-e* (make-environment 'jcp-env))
(defglobal *jcp-u0* (jcp-used))
(defglobal *jcp-r1* (with-environment *jcp-e* (defun jcp-a () 1)))
(defglobal *jcp-r2* (with-environment *jcp-e* (defun jcp-b () 2)))
(defglobal *jcp-r3* (with-environment *jcp-e* (defun jcp-c () 3)))
(defglobal *jcp-r4* (with-environment *jcp-e* (defun jcp-d () 4)))
(defglobal *jcp-u1* (jcp-used))

;; 4関数ぶんのコードが 2 ページ未満に収まっている(旧実装は 4 ページ = 16,384 byte)
(assert-equal t (< (- *jcp-u1* *jcp-u0*) 8192))

;; 同じ環境なので同じページに載る
(defglobal *jcp-pa* (jcp-page-of (%%eval-in-environment '(function jcp-a) *jcp-e*)))
(assert-equal *jcp-pa* (jcp-page-of (%%eval-in-environment '(function jcp-b) *jcp-e*)))
(assert-equal *jcp-pa* (jcp-page-of (%%eval-in-environment '(function jcp-c) *jcp-e*)))
(assert-equal *jcp-pa* (jcp-page-of (%%eval-in-environment '(function jcp-d) *jcp-e*)))

;; 詰めても実行結果は正しい(パッチした自己参照 movabs が壊れていないこと)
(assert-equal 1 (%%eval-in-environment '(jcp-a) *jcp-e*))
(assert-equal 2 (%%eval-in-environment '(jcp-b) *jcp-e*))
(assert-equal 3 (%%eval-in-environment '(jcp-c) *jcp-e*))
(assert-equal 4 (%%eval-in-environment '(jcp-d) *jcp-e*))

;;; --- 2. 環境が違えば必ず別のページになること(設計の中核) ---
;;
;; ここが破れると、片方の環境を破棄したときに os_imm_page_free が
;; もう片方の生きているコードの先頭 8 byte を next ポインタで上書きする。
(defglobal *jcp-ea* (make-environment 'jcp-env-a))
(defglobal *jcp-eb* (make-environment 'jcp-env-b))
(defglobal *jcp-x1* (with-environment *jcp-ea* (defun jcp-a1 () 11)))
(defglobal *jcp-x2* (with-environment *jcp-eb* (defun jcp-b1 () 21)))
(defglobal *jcp-x3* (with-environment *jcp-ea* (defun jcp-a2 () 12)))
(defglobal *jcp-x4* (with-environment *jcp-eb* (defun jcp-b2 () 22)))

(defglobal *jcp-pga* (jcp-page-of (%%eval-in-environment '(function jcp-a1) *jcp-ea*)))
(defglobal *jcp-pgb* (jcp-page-of (%%eval-in-environment '(function jcp-b1) *jcp-eb*)))
;; 環境 A の2関数は同じページ、環境 B の2関数も同じページ
(assert-equal *jcp-pga* (jcp-page-of (%%eval-in-environment '(function jcp-a2) *jcp-ea*)))
(assert-equal *jcp-pgb* (jcp-page-of (%%eval-in-environment '(function jcp-b2) *jcp-eb*)))
;; そして A と B は別のページ
(assert-equal t (not (= *jcp-pga* *jcp-pgb*)))

(assert-equal 11 (%%eval-in-environment '(jcp-a1) *jcp-ea*))
(assert-equal 22 (%%eval-in-environment '(jcp-b2) *jcp-eb*))

;;; --- 3. 破棄した環境のページがフリーリスト経由で再利用されること ---
(defglobal *jcp-u2* (jcp-used))
(assert-equal t (destroy-environment *jcp-eb*))
;; 回収は bump の巻き戻しではないので高水位は減らない
(assert-equal *jcp-u2* (jcp-used))
;; 破棄後に別の環境へ defun すると、返ったページが再利用されるので
;; **コードページぶんの消費(4,096 byte)が起きない**。
;;
;; ちょうど 0 にはならない。新しい名前の defun は Function Cell(16 byte)と
;; za_fn_meta_t(48 byte)も要り、こちらはスロット用カーソルから取られる
;; (%%imm-space-used-bytes はカーソルの末尾未使用分を差し引くので、
;; ページを跨がなくてもこの 64 byte が見える)。**Function Cell に解放経路が無い**のは
;; 別件で、このブランチの対象外
;; (documents/investigation-flet-cell-leak.md)。
;; したがって「1ページぶん増えていないこと」を見る。
(defglobal *jcp-ec* (make-environment 'jcp-env-c))
(defglobal *jcp-x5* (with-environment *jcp-ec* (defun jcp-c1 () 31)))
(assert-equal 31 (%%eval-in-environment '(jcp-c1) *jcp-ec*))
(assert-equal t (< (- (jcp-used) *jcp-u2*) 4096))

;;; --- 4. 開発フロー: 環境を作って defun して破棄する、を繰り返しても増え続けないこと ---
;;
;; 専用環境で試行錯誤して固まったものだけ親へ反映する、という使い方を想定している。
;; このサイクルで Immobilized Space が単調増加すると、試すたびに減り続けて枯渇する。
;; **このテストが今回の設計変更の主目的。**
(defun jcp-cycle ()
  (let ((e (make-environment 'jcp-tmp)))
    (progn
      (%%eval-in-environment '(defun jcp-t1 () 1) e)
      (%%eval-in-environment '(defun jcp-t2 () 2) e)
      (%%eval-in-environment '(defun jcp-t3 () 3) e)
      (destroy-environment e))))

;; 最初の数回はページを取るので増える。その後は返ったページを使い回すだけになる
(jcp-cycle) (jcp-cycle) (jcp-cycle)
(defglobal *jcp-u3* (jcp-used))
(jcp-cycle) (jcp-cycle) (jcp-cycle) (jcp-cycle) (jcp-cycle)
(jcp-cycle) (jcp-cycle) (jcp-cycle) (jcp-cycle) (jcp-cycle)
(defglobal *jcp-u4* (jcp-used))

;; 10 サイクル = 30 回の defun。**コードページの消費はゼロ**で、
;; 増えるのは Function Cell(16)+ za_fn_meta_t(48)= 64 byte/defun ぶんだけ。
;; 30 × 64 = 1,920 byte で、1 ページにも満たない。
;;
;; パッキング前は 30 × 4,160 = 124,800 byte(30 ページ以上)消費していた。
;; **「1ページも消費しない」で張ることで、コードページが完全に回収・再利用されている
;; ことを、cell/meta の残存(別件)と切り分けて検証できる。**
(assert-equal t (< (- *jcp-u4* *jcp-u3*) 4096))

;;; --- 5. 1ページを超える関数は従来どおり連続確保され、正しく動くこと ---
(defglobal *jcp-p0* (%%diag-code-packing))
;; コールサイトが多く 4,096 byte を超える関数(1 コールサイト約 762 byte)
(defun jcp-big (x)
  (+ (jcp-a1 x) (jcp-a1 x) (jcp-a1 x) (jcp-a1 x) (jcp-a1 x) (jcp-a1 x)
     (jcp-a1 x) (jcp-a1 x) (jcp-a1 x) (jcp-a1 x) (jcp-a1 x) (jcp-a1 x)))
(defglobal *jcp-p1* (%%diag-code-packing))
;; 複数ページ確保のカウンタが増えていること(3番目の要素)
(assert-equal t (>= (elt *jcp-p1* 2) (elt *jcp-p0* 2)))
