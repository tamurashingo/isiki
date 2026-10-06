;; test/lisp/jit_frame_test.lisp
;;
;; [調査/回帰] **JIT 生成コードのスタックフレームが太ったら落ちる。**
;; documents/jit-frame-survey.md §4-4。
;;
;; なぜ要るか:
;;   `ZA_FRAME_TOTAL` は **JIT 関数すべてのフレーム**で、
;;   **JIT 関数の再帰深さの上限をそのまま決めている**
;;   (256KB / 7480 = 35 段。実測は「深さ 33 で返る / 34 で STACK OVERFLOW」)。
;;   **誰も 7480 に気づかなかったのは、フレームの大きさを見ている仕組みが
;;   無かったからである。**
;;   `za.c` のフレーム定数(`ZA_MAX_LET_DEPTH` × `ZA_MAX_LOCALS_PER_LET` など)を
;;   上げるとこの値が増え、**再帰深さが黙って減る。**
;;
;;   `documents/jit.md` に「上げるときは再帰深さの回帰を測ること」と書いたが、
;;   **コメントによる警告は効かない。効くのは落ちる仕組みだけである**
;;   (GC_DEBUG を test 側へ渡し忘れた件と同じ)。
;;
;; 測り方:
;;   **深さを直接探ってはいけない。** 上限を超えると C スタックを溢れさせて
;;   プロセスが止まり、**「落ちる」ではなく「ハングする」**。
;;   代わりに**生成コードのプロローグにある `sub rsp, imm32`
;;   (`48 81 EC` + imm32)の即値を読む**。これは決定的で、ハングしない。
;;   カーネルには手を入れていない(`%%DISASM-CODE-BASE` と `%%PEEK` だけ使う)。

;; 生成コードの先頭 64 byte から最初の sub rsp, imm32 を探して即値を返す。
;; 見つからなければ -1(プロローグの形が変わったということなので、それも落ちる)
(defun %jit-frame-size (f)
  (let ((base (%%disasm-code-base f)) (i 0) (found -1))
    (progn
      (while (and (< found 0) (< i 64))
        (progn
          (if (and (= (%%peek (+ base i)) 72)        ; 0x48
                   (= (%%peek (+ base i 1)) 129)     ; 0x81
                   (= (%%peek (+ base i 2)) 236))    ; 0xEC
              (setq found (+ (%%peek (+ base i 3))
                             (* 256 (%%peek (+ base i 4)))
                             (* 65536 (%%peek (+ base i 5)))
                             (* 16777216 (%%peek (+ base i 6)))))
            nil)
          (setq i (+ i 1))))
      found)))

(defun %jit-frame-probe (n) (let ((a n)) (+ a 1)))
(assert-equal t (%%za-compiled-p (function %jit-frame-probe)))

(defglobal jit-frame-size (%jit-frame-size (function %jit-frame-probe)))
(format *isiki-test-stream* "#jit-frame ZA_FRAME_TOTAL=~D  256KB で ~D 段~%"
        jit-frame-size (div 262144 jit-frame-size))
(finish-output *isiki-test-stream*)

;;; --- 記録値(2026-10-04、f19b434 + PR #118)---
;; ZA_FRAME_TOTAL = 7480 byte。内訳は tools/jit_frame_breakdown.py が出す。
;;   quasiquote 2496 / 一般呼び出し 1568 / let 1536 / flet 768+64 / NLX+未使用 488 / 他
;;
;; **増えるのは落とす。減るのは通す。**
;; 落ちたときにやること:
;;   1. `python3 tools/jit_frame_breakdown.py` で内訳を出し、どの領域が増えたか見る
;;   2. 意図した変更なら、この行の 7480 を新しい値へ**下げる**方向にしか直さないこと。
;;      上げるなら、**再帰深さが減ることを承知した上で**上げ、
;;      documents/jit-frame-survey.md の記録も直す
;;   3. **深さの実測も取り直すこと**(いまは「33 で返る / 34 で溢れる」)
(assert-equal t (if (<= jit-frame-size 7480) t nil))
;; プロローグの形が変わって読めなくなった場合も落とす(-1 が通らないこと)
(assert-equal t (if (> jit-frame-size 0) t nil))

;;; --- フレームの数字が実際の振る舞いと対応していること ---
;; 262144 / 7480 = 35。**上限より十分下の深さ 20 で動くことを確かめる。**
;; 上限ぎりぎり(33)を書くと、呼び出し元が使う C スタックのぶんで溢れる
;; (documents/for-expansion.md で実際に踏んだ)
(defun %jit-frame-rec (n) (if (= n 0) 0 (+ n (%jit-frame-rec (- n 1)))))
(assert-equal t (%%za-compiled-p (function %jit-frame-rec)))
(assert-equal 210 (%jit-frame-rec 20))

;;; --- 1 つの let の束縛数の上限が変わっていないこと ---
;; ZA_MAX_LOCALS_PER_LET = 4。**4 束縛までは JIT に乗り、5 束縛以上は乗らない。**
;; フレームを太らせる代わりにこの上限を上げる変更が入ったら、ここが落ちる
;; (そのときは上の jit-frame-size も一緒に落ちる)
(defun %jit-let4 (n) (let ((a n) (b 1) (c 2) (d 3)) (+ a b c d)))
(defun %jit-let5 (n) (let ((a n) (b 1) (c 2) (d 3) (e 4)) (+ a b c d e)))
(assert-equal t   (%%za-compiled-p (function %jit-let4)))
(assert-equal nil (%%za-compiled-p (function %jit-let5)))
