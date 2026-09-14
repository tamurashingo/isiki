;;;; test/lisp/za_abi_probe_test.lisp
;;;; [陽性確認] 原則11: JIT生成コードが callee-saved レジスタ(r12)を退避せずに書くと、
;;;; C側の値が壊れることを検出できるか。kind=1 は壊す断片、kind=0 は退避する断片。
;;;; 検出手段そのものが機能することの確認なので、1 と 0 の両方を固定する。
(assert-equal 1 (%%za-diag-clobber-probe 1))   ; 退避なし → 壊れる(検出できる)
(assert-equal 0 (%%za-diag-clobber-probe 0))   ; push/pop で退避 → 壊れない
