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

;; **この検出器が見ているのは `%jit-frame-probe` の寸法である**(2026-10-30 に明記)。
;; いまは全関数が同じ寸法なので「どの関数か」は意味を持たないが、
;; **フレームを実寸にすると関数ごとに違う値になる**ので、題材が何かが効いてくる。
;;   %jit-frame-probe  … let を使う葉。まれな領域(quasiquote / flet)を使わない = **小さい側**
;;   %jit-frame-qq     … quasiquote を使う = まれな領域を取る **大きい側**
(defun %jit-frame-probe (n) (let ((a n)) (+ a 1)))
(assert-equal t (%%za-compiled-p (function %jit-frame-probe)))
(defun %jit-frame-qq (n) `(a ,n))
(assert-equal t (%%za-compiled-p (function %jit-frame-qq)))

(defglobal jit-frame-size (%jit-frame-size (function %jit-frame-probe)))
(defglobal jit-frame-size-qq (%jit-frame-size (function %jit-frame-qq)))
(format *isiki-test-stream* "#jit-frame probe=~D qq=~D  256KB で ~D 段~%"
        jit-frame-size jit-frame-size-qq (div 262144 jit-frame-size))
(finish-output *isiki-test-stream*)

;; **大きい側が小さい側より小さくなることはない。** これは実寸化の前後どちらでも成り立つ
;; 関係なので、行番号や絶対値と違って**段を越えて生き残る**:
;;   実寸化の前 … 全関数が同じ寸法なので等しい
;;   実寸化の後 … qq を使う関数だけがまれな領域を取るので大きい
(assert-equal t (if (>= jit-frame-size-qq jit-frame-size) t nil))

;;; --- 記録値(2026-10-04、f19b434 + PR #118)---
;; ZA_FRAME_TOTAL = 7480 byte。内訳は tools/jit_frame_breakdown.py が出す。
;;   quasiquote 2496 / 一般呼び出し 1568 / let 1536 / flet 768+64 / NLX+未使用 488 / 他
;;
;; **これは「上限」であって「寸法の期待値」ではない。**
;; 2026-10-30 まで、この数字は `(<= jit-frame-size 7480)` の形で直書きされていた。
;; 全関数が同じ寸法だったので「寸法 = 7480」と「寸法 <= 7480」の区別が要らなかったが、
;; **実寸化すると関数ごとに違う値になる**ので、絶対値は寸法の期待値には使えない。
;; 名前を付けて**上限としてだけ**使う(関数ごとの期待値は下の「関係」で固定する)。
;;
;; **なぜ 7480 なのか**: 実寸化の直前(= いま)の、全関数共通の寸法そのものである。
;; 実寸化は「使う領域だけ確保する」変更なので、**どの関数もこの値を超えない。**
;; つまりこの値は「増えるのは落とす、減るのは通す」の向きを決める基準点である。
(defglobal jit-frame-size-cap 7480)

;; **増えるのは落とす。減るのは通す。**
;; 落ちたときにやること:
;;   1. `python3 tools/jit_frame_breakdown.py` で内訳を出し、どの領域が増えたか見る
;;   2. 意図した変更なら、jit-frame-size-cap を新しい値へ**下げる**方向にしか
;;      直さないこと。上げるなら、**再帰深さが減ることを承知した上で**上げ、
;;      documents/jit-frame-survey.md の記録も直す
;;   3. **深さの実測も取り直すこと**(いまは「33 で返る / 34 で溢れる」)
;;
;; 上限は**大きい側にも掛ける。** qq を使う関数が全関数の最大なので、
;; ここを超えなければどの関数も超えない
(assert-equal t (if (<= jit-frame-size-qq jit-frame-size-cap) t nil))
(assert-equal t (if (<= jit-frame-size jit-frame-size-cap) t nil))
;; プロローグの形が変わって読めなくなった場合も落とす(-1 が通らないこと)
(assert-equal t (if (> jit-frame-size 0) t nil))
(assert-equal t (if (> jit-frame-size-qq 0) t nil))

;;; --- 16 で割った余りが 8 であること ---
;; 入口の rsp は戻り番地で 8 mod 16 で、push rbx / push r13 で元に戻るので、
;; **`sub rsp` の総量が 8 mod 16 のときだけ呼び出し規約の整列が保たれる。**
;; za.c は `_Static_assert((ZA_FRAME_TOTAL % 16) == 8)` でビルド時に見ているが、
;; それは**定数 1 つ**に対する検査である。**実寸化すると寸法は関数ごとに決まるので、
;; ビルド時の検査は「どの関数も 8 mod 16」を保証しない。**
;; ここは出力された機械語の即値そのものを見るので、題材ごとに効く
;; (混在した題材での同じ判定は jit_mixed_frame_test.lisp §2-D)。
;;
;; 落ちたときにやること: 寸法を 16 で割った余りを見て、
;; `tools/jit_frame_breakdown.py` の内訳から**16 の倍数でない領域**を探すこと。
;; **この判定を緩めてはいけない**(緩めると呼び出し規約が静かに壊れる)
(assert-equal 8 (mod jit-frame-size 16))
(assert-equal 8 (mod jit-frame-size-qq 16))

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
