;; test/lisp/inline_muldiv_test.lisp
;;
;; [#128 / M7-T1] **`*` `/` のインライン展開** の試験。
;; documents/inline-muldiv.md。
;;
;; ## 何が危ないか
;;
;; 1. **タグのずれ。** fixnum は magnitude<<4 なので、タグ付きのまま掛けると
;;    2 回ぶんずれる。片方だけ戻す(`(a>>4) * b`)のが正しい
;; 2. **桁あふれ。** 上位 64bit と bit63 の**両方**を見ないと、
;;    生の 64bit 乗算が巻き上がったときに取り逃がす
;; 3. **ゼロ除算。** 検査を外すと `#DE` で**機械が止まる**(4 分類の分類 3)
;; 4. **負値。** 展開は「両方非負」の守りを通った後だけ。負値は呼び出しへ落ちる
;;
;; 答えはすべて**宣言なしの版と一致すること**で縛る(独立した 2 つの経路)。
;; **絶対値でも固定する**(一致だけだと両方壊れても通る)。

;;; --- 宣言なし(対照群)---
(defun im-mul-gen (x y) (* x y))
(defun im-div-gen (x y) (/ x y))
;;; --- 型宣言のみ(展開されない。従来どおり呼び出し)---
(defun im-mul-decl (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (* x y))
(defun im-div-decl (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (/ x y))
;;; --- 型 + inline 宣言(展開される)---
(defun im-mul (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (inline *)) (* x y))
(defun im-div (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (inline /)) (/ x y))

;;; --- 1. 前提: JIT に乗っている ---
(assert-equal t (%%za-compiled-p (function im-mul)))
(assert-equal t (%%za-compiled-p (function im-div)))
(assert-equal t (%%za-compiled-p (function im-mul-decl)))
(assert-equal t (%%za-compiled-p (function im-div-decl)))

;;; --- 2. **展開されたことを別の計器で見る** ---
;; 型宣言のみの版は呼び出し(movabs + call)、inline 宣言つきは展開なので、
;; **生成コードの長さが違う**。長さが同じなら展開されていない。
;; 比較の特化(#162)ではこの計器が使えなかった(呼び先が変わっても長さが同じ)が、
;; **展開は命令の数そのものが変わる**ので今回は使える
(format *isiki-test-stream* "#im-codelen mul-decl=~D mul-inline=~D div-decl=~D div-inline=~D~%"
        (%%disasm-code-len 'im-mul-decl) (%%disasm-code-len 'im-mul)
        (%%disasm-code-len 'im-div-decl) (%%disasm-code-len 'im-div))
(assert-equal t (if (= (%%disasm-code-len 'im-mul-decl) (%%disasm-code-len 'im-mul)) nil t))
(assert-equal t (if (= (%%disasm-code-len 'im-div-decl) (%%disasm-code-len 'im-div)) nil t))

;;; --- 3. 乗算: 答えが一致する ---
(assert-equal (im-mul-gen 6 7) (im-mul 6 7))
(assert-equal (im-mul-gen 0 5) (im-mul 0 5))
(assert-equal (im-mul-gen 5 0) (im-mul 5 0))
(assert-equal (im-mul-gen 1 1) (im-mul 1 1))
(assert-equal (im-mul-gen 12345 6789) (im-mul 12345 6789))
;; 絶対値
(assert-equal 42 (im-mul 6 7))
(assert-equal 0 (im-mul 0 5))
(assert-equal 0 (im-mul 5 0))
(assert-equal 83810205 (im-mul 12345 6789))

;;; --- 4. 乗算: **負値は呼び出しへ落ちる**(展開は非負だけ)---
(assert-equal (im-mul-gen -6 7) (im-mul -6 7))
(assert-equal (im-mul-gen 6 -7) (im-mul 6 -7))
(assert-equal (im-mul-gen -6 -7) (im-mul -6 -7))
(assert-equal -42 (im-mul -6 7))
(assert-equal -42 (im-mul 6 -7))
(assert-equal 42 (im-mul -6 -7))

;;; --- 5. 乗算: **桁あふれ**。fixnum を超えたら bignum へ昇格する ---
;; 2^59 を超える積。**上位 64bit と bit63 の両方を見ていないと取り逃がす**
(assert-equal (im-mul-gen 1000000000 1000000000) (im-mul 1000000000 1000000000))
(assert-equal (im-mul-gen 123456789 987654321) (im-mul 123456789 987654321))
(assert-equal 1000000000000000000 (im-mul 1000000000 1000000000))
;; **生の 64bit が巻き上がる大きさ**(2^32 x 2^32 = 2^64)
(assert-equal (im-mul-gen 4294967296 4294967296) (im-mul 4294967296 4294967296))
(assert-equal 18446744073709551616 (im-mul 4294967296 4294967296))

;;; --- 6. 除算: 答えが一致する ---
(assert-equal (im-div-gen 42 7) (im-div 42 7))
(assert-equal (im-div-gen 43 7) (im-div 43 7))
(assert-equal (im-div-gen 0 7) (im-div 0 7))
(assert-equal (im-div-gen 1000003 7) (im-div 1000003 7))
;; 絶対値(**切り捨て**)
(assert-equal 6 (im-div 42 7))
(assert-equal 6 (im-div 43 7))
(assert-equal 0 (im-div 0 7))
(assert-equal 0 (im-div 3 7))
(assert-equal 1 (im-div 7 7))

;;; --- 7. 除算: 負値は呼び出しへ落ちる ---
(assert-equal (im-div-gen -42 7) (im-div -42 7))
(assert-equal (im-div-gen 42 -7) (im-div 42 -7))
(assert-equal (im-div-gen -42 -7) (im-div -42 -7))

;;; --- 8. 除算: **ゼロ除算**。検査を外すと #DE で機械が止まる ---
;; 展開の中で 0 を見てフォールバックし、呼び先が signal する
(assert-error (im-div 42 0))
(assert-error (im-div 0 0))
;; 呼び出しのあとも生きている(機械が止まっていない)
(assert-equal 6 (im-div 42 7))

;;; --- 9. 結果のゼロは**正のゼロ**である(-0 は存在しない)---
(assert-equal 0 (im-mul 0 0))
(assert-equal t (= 0 (im-mul 0 0)))
(assert-equal t (= 0 (im-div 0 5)))
