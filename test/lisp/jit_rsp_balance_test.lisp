;; test/lisp/jit_rsp_balance_test.lisp
;;
;; [回帰] **JIT 生成コードの `sub rsp` と `add rsp` が食い違ったら落ちる。**
;; documents/jit-frame-survey.md §8-5、documents/jit-static-code-check.md。
;;
;; なぜ要るか:
;;   フレームの実寸化(§4。quasiquote/flet をフレーム末尾へ回して関数ごとに
;;   `sub rsp` の量を変える案)では、**プロローグの `sub rsp` の即値を後埋めし、
;;   エピローグの `add rsp` と一致させる**ことになる。出力箇所は
;;   `sub` が 2・`add` が 3(`za.c` の `jit_sub_rsp_imm32` / `jit_add_rsp_imm32`)で、
;;   **`add` の 2 箇所は末尾呼び出しの経路で、本体のコンパイル中に出力される。**
;;
;;   ここが 1 箇所でも食い違うと、**コンパイルエラーにはならない。**
;;   `ret` のときに `rsp` がずれ、呼び出し元の戻り番地を踏んだ上で飛ぶので、
;;   症状は「どこで壊れたか分からないハング/暴走」になる。
;;   **最後の歯止めは、生成コードを静的に読む検査しかない。**
;;
;; 測り方:
;;   **実行して確かめてはいけない。** 食い違ったコードを呼べば、その場で
;;   スタックが壊れてテスト自体が死ぬ(「落ちる」ではなく「ハングする」)。
;;   代わりに**生成コードを命令単位で歩いて、`rsp` を動かす命令の即値を集める。**
;;   決定的で、1 度も実行しない。
;;
;;   命令の長さは `%%DISASM-ITEM`(自前のデコーダ)に数えさせる。
;;   **バイト列を素で走査してはいけない** — `movabs` の 8 byte 即値の中に
;;   `48 81 EC` が現れれば `sub rsp, imm32` と誤認する。
;;
;; 不変条件:
;;   `sub rsp, imm32` と `add rsp, imm32` の即値は**全部同じ**でなければならない。
;;   **個数は一致しない**(`sub` は入口ごと、`add` は出口経路ごとなので、
;;   末尾呼び出しを含む関数では `add` のほうが多い)。
;;   `sub rsp, 0x20` / `add rsp, 0x20`(imm8)は `jit_call_r11` が call の
;;   前後で対にして出す shadow space なので、ここでは数えない。

;;; --- 走査器 ---

;; off の命令が rsp を imm32 で動かすものなら、その即値を返す。違えば -1。
;; kind: 236 = 0xEC(sub rsp, imm32) / 196 = 0xC4(add rsp, imm32)
(defun %jrb-imm32-at (base off kind)
  (if (and (= (%%peek (+ base off)) 72)        ; 0x48 REX.W
           (= (%%peek (+ base off 1)) 129)     ; 0x81 (imm32 形式)
           (= (%%peek (+ base off 2)) kind))
      (+ (%%peek (+ base off 3))
         (* 256 (%%peek (+ base off 4)))
         (* 65536 (%%peek (+ base off 5)))
         (* 16777216 (%%peek (+ base off 6))))
    -1))

;; 生成コード全体を命令単位で歩き、(sub の個数 add の個数 食い違いの個数 即値) を返す。
;; 最初に見つけた即値を正とし、以降それと違うものを食い違いとして数える。
(defun %jrb-audit (f)
  (let ((base (%%disasm-code-base f)) (len (%%disasm-code-len f)))
    (let ((off 0) (nsub 0) (nadd 0) (bad 0) (fsz 0))
      (progn
        (while (< off len)
          (let ((it (%%disasm-item base len off)))
            (if (null it)
                (setq off len)
              (let ((s (%jrb-imm32-at base off 236))
                    (a (%jrb-imm32-at base off 196)))
                (progn
                  (if (> s 0)
                      (progn
                        (setq nsub (+ nsub 1))
                        (if (= fsz 0) (setq fsz s) nil)
                        (if (= s fsz) nil (setq bad (+ bad 1))))
                    nil)
                  (if (> a 0)
                      (progn
                        (setq nadd (+ nadd 1))
                        (if (= fsz 0) (setq fsz a) nil)
                        (if (= a fsz) nil (setq bad (+ bad 1))))
                    nil)
                  (setq off (+ off (car it))))))))
        (list nsub nadd bad fsz)))))

(defun %jrb-nsub (f) (car (%jrb-audit f)))
(defun %jrb-nadd (f) (car (cdr (%jrb-audit f))))
(defun %jrb-bad  (f) (car (cdr (cdr (%jrb-audit f)))))
(defun %jrb-fsz  (f) (car (cdr (cdr (cdr (%jrb-audit f))))))

;; 最初の add rsp, imm32 のオフセット。見つからなければ -1
(defun %jrb-first-add-off (f)
  (let ((base (%%disasm-code-base f)) (len (%%disasm-code-len f)))
    (let ((off 0) (found -1))
      (progn
        (while (and (< found 0) (< off len))
          (let ((it (%%disasm-item base len off)))
            (if (null it)
                (setq off len)
              (progn
                (if (> (%jrb-imm32-at base off 196) 0) (setq found off) nil)
                (setq off (+ off (car it)))))))
        found))))

;;; --- 1. 経路ごとに整合していること ---
;;
;; 機能ごとに出口の形が違う(エピローグ / 末尾呼び出し / NLX / tagbody の go)。
;; どれも即値は 1 つの値(= ZA_FRAME_TOTAL)で揃っていなければならない。

(defun %jrb-leaf (n) (+ n 1))
(defun %jrb-plain (n) (let ((a n)) (+ a 1)))
(defun %jrb-tail (n) (%jrb-leaf n))
(defun %jrb-rec (n) (if (= n 0) 0 (+ n (%jrb-rec (- n 1)))))
(defun %jrb-nlx (n) (block done (if (< n 0) (return-from done 'neg) n)))
(defun %jrb-catch (n) (catch 'tag (if (< n 0) (throw 'tag 'neg) n)))
;; 仮引数への setq は JIT が別の理由で断念するので、局所変数へ受け直す
;; (jit_limits_test.lisp §TAGBODY で踏んだのと同じ罠)
(defun %jrb-tagbody (n) (let ((r n)) (progn (tagbody (if (> r 0) (go jrb-done) nil) (setq r 0) jrb-done) r)))
(defun %jrb-qq (n) `(a ,n b))
(defun %jrb-flet (n) (flet ((f (y) (+ y 1))) (f n)))

(assert-equal t (%%za-compiled-p (function %jrb-leaf)))
(assert-equal t (%%za-compiled-p (function %jrb-plain)))
(assert-equal t (%%za-compiled-p (function %jrb-tail)))
(assert-equal t (%%za-compiled-p (function %jrb-rec)))
(assert-equal t (%%za-compiled-p (function %jrb-nlx)))
(assert-equal t (%%za-compiled-p (function %jrb-catch)))
(assert-equal t (%%za-compiled-p (function %jrb-tagbody)))
(assert-equal t (%%za-compiled-p (function %jrb-qq)))
(assert-equal t (%%za-compiled-p (function %jrb-flet)))

;; 食い違いが 0 であること
(assert-equal 0 (%jrb-bad (function %jrb-leaf)))
(assert-equal 0 (%jrb-bad (function %jrb-plain)))
(assert-equal 0 (%jrb-bad (function %jrb-tail)))
(assert-equal 0 (%jrb-bad (function %jrb-rec)))
(assert-equal 0 (%jrb-bad (function %jrb-nlx)))
(assert-equal 0 (%jrb-bad (function %jrb-catch)))
(assert-equal 0 (%jrb-bad (function %jrb-tagbody)))
(assert-equal 0 (%jrb-bad (function %jrb-qq)))
(assert-equal 0 (%jrb-bad (function %jrb-flet)))

;; 即値は ZA_FRAME_TOTAL = 7480(= 0x1d38)。jit_frame_test.lisp と同じ数字を
;; **別の読み方**(全命令走査 vs プロローグ先頭 64 byte)で確かめている
(assert-equal 7480 (%jrb-fsz (function %jrb-plain)))
(assert-equal 7480 (%jrb-fsz (function %jrb-tail)))

;; 走査が空振りしていないこと(0 件なら「食い違い 0」は無意味)
(assert-equal t (if (> (%jrb-nsub (function %jrb-plain)) 0) t nil))
(assert-equal t (if (> (%jrb-nadd (function %jrb-plain)) 0) t nil))

;;; --- 2. 末尾呼び出しの経路が走査に入っていること(§4-3) ---
;;
;; `sub` は入口ごと(2 箇所)だが、`add` は出口経路ごと。末尾呼び出しを含む関数は
;; **本体の途中にも `add rsp` が出る**ので、add の個数が sub より多くなる。
;; **ここが等しくなったら、末尾呼び出しの経路を走査していない**ということなので落とす。
(assert-equal t (if (> (%jrb-nadd (function %jrb-tail))
                       (%jrb-nsub (function %jrb-tail))) t nil))
;; 末尾呼び出しを含まない関数では add は出口(エピローグ)の分だけ
(assert-equal t (if (= (%jrb-nadd (function %jrb-leaf))
                       (%jrb-nsub (function %jrb-leaf))) t nil))

;;; --- 3. 陽性対照(規則 8) — **末尾呼び出し経路の `add rsp` を 1 byte 壊す** ---
;;
;; 壊したコードは**呼ばない。** 呼べばその場でスタックが壊れて、テストごと死ぬ。
;; 静的に読むだけなので安全に壊して戻せる。
;;
;; %jrb-tail の最初の add rsp, imm32 は**末尾呼び出しの速い経路**のもの
;; (za.c の `jit_add_rsp_imm32(ZA_FRAME_TOTAL)` のうち、エピローグより先に
;; 出力される 2 箇所の 1 つ)。§4-3 が要求する「末尾呼び出しを含む関数」の、
;; まさにその経路を壊す。

(defglobal jrb-tail-base (%%disasm-code-base (function %jrb-tail)))
(defglobal jrb-poke-at (+ jrb-tail-base (+ (%jrb-first-add-off (function %jrb-tail)) 3)))
(defglobal jrb-saved-byte (%%peek jrb-poke-at))

;; 壊す前: 食い違い 0
(assert-equal 0 (%jrb-bad (function %jrb-tail)))
;; 0x38 -> 0x39(フレームが 1 byte 大きいことにする)
(defglobal jrb-ignored-1 (%%poke jrb-poke-at (+ jrb-saved-byte 1)))
;; **壊したら検出されること。** されなければ、この検査は効いていない
(assert-equal t (if (> (%jrb-bad (function %jrb-tail)) 0) t nil))
;; 元に戻す
(defglobal jrb-ignored-2 (%%poke jrb-poke-at jrb-saved-byte))
(assert-equal 0 (%jrb-bad (function %jrb-tail)))
;; 戻したコードがちゃんと動くこと(ここで初めて呼ぶ)
(assert-equal 6 (%jrb-tail 5))
(assert-equal 210 (%jrb-rec 20))
(assert-equal 'neg (%jrb-nlx -1))
(assert-equal 'neg (%jrb-catch -1))
(assert-equal '(a 7 b) (%jrb-qq 7))
(assert-equal 8 (%jrb-flet 7))

(format *isiki-test-stream*
        "#jit-rsp fsz=~D  plain(sub ~D/add ~D)  tail(sub ~D/add ~D)~%"
        (%jrb-fsz (function %jrb-plain))
        (%jrb-nsub (function %jrb-plain)) (%jrb-nadd (function %jrb-plain))
        (%jrb-nsub (function %jrb-tail)) (%jrb-nadd (function %jrb-tail)))
(finish-output *isiki-test-stream*)
