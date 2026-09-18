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
;;
;; [4bit化] **全16値を並べる**。使用中の値だけを試すと、未割当タグを検出器が
;; どう扱うかがテストから見えない。検出器が未割当タグを「ヒープ参照」と誤判定
;; すると、生の即値を焼き込み扱いして偽陽性を出し続ける。
;; 値は documents/tag4-design.md 2.4 の割り当て表と同じ(bit0=0が即値)。

;; GCが追いかけるタグ = 検出されなければならない
(assert-equal 1 (%%za-scan-synth 1))   ; 0x1 TAG_CONS
(assert-equal 1 (%%za-scan-synth 3))   ; 0x3 TAG_SYMBOL
(assert-equal 1 (%%za-scan-synth 5))   ; 0x5 TAG_STRING
(assert-equal 1 (%%za-scan-synth 7))   ; 0x7 TAG_INSTANCE(VECTOR/BIGNUM/FLOATもこれ)
(assert-equal 1 (%%za-scan-synth 15))  ; 0xF TAG_FORWARD(以前は漏れていた)

;; GCが追いかけないタグ = 検出されてはならない(即値の誤検出を防ぐ性質)
(assert-equal 0 (%%za-scan-synth 0))   ; 0x0 TAG_FIXNUM
(assert-equal 0 (%%za-scan-synth 2))   ; 0x2 TAG_CHAR
(assert-equal 0 (%%za-scan-synth 4))   ; 0x4 TAG_SINGLE_FLOAT(予約のみ)
(assert-equal 0 (%%za-scan-synth 14))  ; 0xE TAG_MARKER(MAGIC_*の下位4bit)
(assert-equal 0 (%%za-scan-synth 9))   ; 0x9 TAG_RAW_POINTER(アドレスは持つがGC管理外)

;; 未割当タグ。即値側(bit0=0)もアドレス側(bit0=1)も、値が存在しない今は
;; 「追いかけない」に倒してある(documents/tag4-step2.md 3章)
(assert-equal 0 (%%za-scan-synth 6))   ; 0x6 即値・未割当
(assert-equal 0 (%%za-scan-synth 8))   ; 0x8 即値・未割当
(assert-equal 0 (%%za-scan-synth 10))  ; 0xA 即値・未割当
(assert-equal 0 (%%za-scan-synth 12))  ; 0xC 即値・未割当
(assert-equal 0 (%%za-scan-synth 11))  ; 0xB アドレス・未割当(将来のdouble-float)
(assert-equal 0 (%%za-scan-synth 13))  ; 0xD アドレス・未割当(将来のratio)

;; 陽性対照を流した後も、本物の焼き込みは0のままであること
(assert-equal 0 (%%za-heap-imm-count))
