;; test/lisp/za_code_imm_test.lisp
;;
;; [GC安全性] documents/pitfalls.md 原則8の回帰テスト。
;;
;; JITが生成した機械語へ「GCで動く領域(From/To空間)を指すアドレス」を焼き込むと、
;; GCの再配置に追随できず、コンパイルは成功したまま実行時に壊れる。このバグクラスを
;; 守る機構は存在しないため、**検出を常時回す**ことでしか再発を防げない。
;;
;; za.cはリテラルを静的スロット(os_gc_register_root済み)経由で参照する設計なので、
;; 焼き込みは常に0でなければならない。za_try_compile_defunがコンパイルのたびに
;; 生成コードを走査して数えている(%%ZA-HEAP-IMM-COUNTは累計)。
;;
;; ここが0でなくなったら、新しく足したコード生成がヒープアドレスを直接
;; 焼き込んでいる。静的スロット経由へ直すこと。

;; ここまでのブート(init.lisp・各テストファイル)で多数の関数がJITコンパイル済み
(assert-equal 0 (%%za-heap-imm-count))

;; labels+letを含む関数を新たにコンパイルしても増えないこと
;; (この形は実際に壊れていた経路。GC跨ぎでローカル解決がグローバル名解決に化けた)
(defun zci-labels-let (x)
  (labels ((inner (y) (let ((a (+ y 1)) (b (+ y 2))) (+ a b))))
    (inner x)))
(assert-equal t (%%za-compiled-p (function zci-labels-let)))
(assert-equal 5 (zci-labels-let 1))
(assert-equal 0 (%%za-heap-imm-count))

;; quoteされたヒープ値を持つ関数(スロット経由で参照されるはず)
(defun zci-quoted (x) (progn x (quote (a b c))))
(assert-equal '(a b c) (zci-quoted 1))
(assert-equal 0 (%%za-heap-imm-count))
