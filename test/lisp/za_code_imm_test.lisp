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

;;; --- 陽性確認: 検出器が実際に発火すること ---
;;
;; 「0件だった」を根拠にする前に、発火することを確認する。
;; %%ZA-SCAN-SYNTH は、指定タグを持つヒープ参照を焼き込んだ合成movabsを作り、
;; za_try_compile_defunと同じ判定経路(os_tag_is_heap_ref + os_addr_region)へ通す。
;;
;; タグごとに確認する。1つ通ったから全部通るとは限らない
;; (かつてCONS/SYMBOL/STRING/INSTANCEだけを列挙しており、TAG_FORWARDが漏れていた)。

;; GCが追いかけるタグ = 検出されなければならない
(assert-equal 1 (%%za-scan-synth 1))   ; TAG_CONS
(assert-equal 1 (%%za-scan-synth 2))   ; TAG_SYMBOL
(assert-equal 1 (%%za-scan-synth 4))   ; TAG_STRING
(assert-equal 1 (%%za-scan-synth 5))   ; TAG_INSTANCE(VECTOR/BIGNUM/FLOATもこれ)
(assert-equal 1 (%%za-scan-synth 6))   ; TAG_FORWARD(以前は漏れていた)

;; GCが追いかけないタグ = 検出されてはならない(fixnum即値の誤検出を防ぐ性質)
(assert-equal 0 (%%za-scan-synth 0))   ; TAG_FIXNUM
(assert-equal 0 (%%za-scan-synth 3))   ; TAG_CHAR
(assert-equal 0 (%%za-scan-synth 7))   ; TAG_RAW_POINTER

;; 陽性対照を流した後も、本物の焼き込みは0のままであること
(assert-equal 0 (%%za-heap-imm-count))
