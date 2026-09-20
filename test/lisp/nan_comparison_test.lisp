;; test/lisp/nan_comparison_test.lisp
;;
;; NaN が絡む比較を IEEE 754 に合わせる(documents/nan-comparison.md)。
;;
;; 以前は `(= nan nan)` が **真** を返していた。number_compare が三値(-1/0/1)で
;; 非順序を表現できず、NaN では `<` も `>` も偽になって **0(等しい)** へ落ちて
;; いたためである(documents/float-default.md §4-1 に既知の問題として記録)。
;;
;; **`/=` だけが NaN に対して真を返す。** これが唯一の例外で間違えやすい。

;;; --- 前提: NaN と無限大が作れること ---
;;; 生成手段は convert_float_test.lisp と同じ(domain-error 未実装のための簡略化。
;;; runtime.c の primitive_divide のコメント参照)
(defglobal *nan-d* (/ 0.0d0 0.0d0))
(defglobal *nan-s* (/ 0.0f0 0.0f0))
(defglobal *inf-d* (/ 1.0d0 0.0d0))
(defglobal *ninf-d* (/ -1.0d0 0.0d0))

(defun nan-print (x)
  (let ((s (create-string-output-stream)))
    (format s "~S" x)
    (get-output-stream-string s)))

(assert-equal "NAN" (nan-print *nan-d*))
(assert-equal "NAN" (nan-print *nan-s*))
(assert-equal "INF" (nan-print *inf-d*))
(assert-equal "-INF" (nan-print *ninf-d*))
;; single の NaN が single のまま作れていること(double へ昇格していない)
(assert-equal '<double-float> (%%class-name (class-of *nan-d*)))
(assert-equal '<single-float> (%%class-name (class-of *nan-s*)))

;;; --- 5-1. 基本。NaN が絡むと /= 以外はすべて偽 ---
(assert-equal nil (= *nan-d* *nan-d*))
(assert-equal nil (= *nan-d* 1.0d0))
(assert-equal nil (= 1.0d0 *nan-d*))          ; 左右を入れ替えても同じ
(assert-equal nil (< *nan-d* 1.0d0))
(assert-equal nil (< 1.0d0 *nan-d*))
(assert-equal nil (> *nan-d* 1.0d0))
(assert-equal nil (> 1.0d0 *nan-d*))
(assert-equal nil (<= *nan-d* 1.0d0))
(assert-equal nil (<= 1.0d0 *nan-d*))
(assert-equal nil (>= *nan-d* 1.0d0))
(assert-equal nil (>= 1.0d0 *nan-d*))

;; **`/=` だけが真。** 「等しくない」は非順序も含む
(assert-equal t (/= *nan-d* *nan-d*))
(assert-equal t (/= *nan-d* 1.0d0))
(assert-equal t (/= 1.0d0 *nan-d*))

;;; --- 5-2. single と double、および混在 ---
(assert-equal nil (= *nan-s* *nan-s*))
(assert-equal nil (= *nan-s* *nan-d*))        ; 混在
(assert-equal nil (= *nan-d* *nan-s*))
(assert-equal nil (= *nan-s* 1.0f0))
(assert-equal nil (= 1.0f0 *nan-s*))
(assert-equal nil (< *nan-s* 1.0f0))
(assert-equal nil (>= *nan-s* 1.0f0))
(assert-equal t (/= *nan-s* *nan-s*))
(assert-equal t (/= *nan-s* *nan-d*))
;; 整数と NaN の混在も同じ
(assert-equal nil (= *nan-d* 1))
(assert-equal nil (< 1 *nan-d*))
(assert-equal t (/= 1 *nan-d*))

;;; --- 5-3. 無限大は順序づけられる(変えていないこと) ---
;;; **NaN と混同しないこと。** 無限大は非順序ではない
(assert-equal t (< 1.0d0 *inf-d*))
(assert-equal t (= *inf-d* *inf-d*))
(assert-equal t (< *ninf-d* *inf-d*))
(assert-equal t (< *ninf-d* 1.0d0))
(assert-equal nil (= *inf-d* *ninf-d*))
(assert-equal t (> *inf-d* 1.0d0))
(assert-equal t (>= *inf-d* *inf-d*))
(assert-equal t (<= *ninf-d* *ninf-d*))
(assert-equal nil (/= *inf-d* *inf-d*))
;; 無限大と NaN の比較は非順序
(assert-equal nil (< *inf-d* *nan-d*))
(assert-equal nil (= *inf-d* *nan-d*))
(assert-equal t (/= *inf-d* *nan-d*))

;;; --- 5-4. 通常の値が壊れていないこと(最も重要な回帰確認) ---
(assert-equal t (= 1.0d0 1.0d0))
(assert-equal t (< 1.0d0 2.0d0))
(assert-equal nil (< 2.0d0 1.0d0))
(assert-equal t (<= 1.0d0 1.0d0))
(assert-equal t (>= 2.0d0 1.0d0))
(assert-equal t (> 2.0d0 1.0d0))
(assert-equal t (/= 1.0d0 2.0d0))
(assert-equal nil (/= 1.0d0 1.0d0))
(assert-equal t (= 1.0f0 1.0f0))
(assert-equal t (< 1.0f0 2.0f0))
;; **整数どうしは一切変わっていないこと**(NaN は存在しない)
(assert-equal t (= 1 1))
(assert-equal t (< 1 2))
(assert-equal nil (< 2 1))
(assert-equal t (>= 2 2))
(assert-equal t (/= 1 2))
(assert-equal nil (/= 1 1))
(assert-equal t (= -1 -1))
(assert-equal t (< -2 -1))
;; bignum も
(assert-equal t (= 100000000000000000000 100000000000000000000))
(assert-equal t (< 100000000000000000000 100000000000000000001))
;; 型が違えば等しくない(PR #80 で固定済み)
(assert-equal nil (= 0.1f0 0.1d0))

;;; --- 5-5. (/= x x) による NaN 判定 ---
;;; **修正後、これが真になるのは x が NaN のときだけ。** 他言語と同じ定石で、
;;; 専用の述語(nan-p 等)を足す必要は無い(documents/nan-comparison.md §3-3)
(assert-equal nil (/= 1.0d0 1.0d0))
(assert-equal nil (/= 0.0d0 0.0d0))
(assert-equal nil (/= *inf-d* *inf-d*))
(assert-equal t (/= *nan-d* *nan-d*))
(assert-equal t (/= *nan-s* *nan-s*))

;;; --- 多引数の /= ---
;;; **この実装の /= は「隣接ペアがすべて等しくない」** であって、CommonLisp の
;;; 「全要素が相異なる」ではない(primitive_num_not_equal のコメント。本作業以前
;;; からの簡略化で、ここでは変えない)。したがって (/= 1 2 1) は真になる。
(assert-equal t (/= 1 2 1))
(assert-equal nil (/= 1 1 2))
;; NaN を混ぜた多引数。隣接ペアのどれかが「等しい」なら偽、それ以外は真
(assert-equal t (/= *nan-d* 1.0d0 2.0d0))
(assert-equal t (/= 1.0d0 *nan-d* 1.0d0))     ; 隣接はどちらも非順序 → 真
(assert-equal nil (/= 1.0d0 1.0d0 *nan-d*))   ; 先頭ペアが等しい → 偽

;;; --- 多引数の = / < も NaN で偽になること ---
(assert-equal nil (= 1.0d0 1.0d0 *nan-d*))
(assert-equal nil (< 1.0d0 2.0d0 *nan-d*))
(assert-equal t (= 1.0d0 1.0d0 1.0d0))
(assert-equal t (< 1.0d0 2.0d0 3.0d0))

;;; --- /= が比較の専用経路に乗ったこと(PR #86) ---
;;; primitive_num_not_equal2 の追加で、/= も < と同じく cons を作らず直接 call
;;; されるようになった。**経路が変わったので、上の NaN の結果が JIT 経由でも
;;; 同じであることを確かめる**(C 側の num_ne とインライン側を一致させる前提)。
(defun nan-ne-jit (a b) (/= a b))
(defun nan-eq-jit (a b) (= a b))
(defun nan-lt-jit (a b) (< a b))
(assert-equal t (%%za-compiled-p (function nan-ne-jit)))
(assert-equal t (%%za-compiled-p (function nan-eq-jit)))
;; JIT 経由でも NaN の規則は同じ
(assert-equal t (nan-ne-jit *nan-d* *nan-d*))
(assert-equal t (nan-ne-jit *nan-s* *nan-s*))
(assert-equal t (nan-ne-jit *nan-d* 1.0d0))
(assert-equal nil (nan-eq-jit *nan-d* *nan-d*))
(assert-equal nil (nan-lt-jit *nan-d* 1.0d0))
;; 通常の値も JIT 経由で一致
(assert-equal nil (nan-ne-jit 1 1))
(assert-equal t (nan-ne-jit 1 2))
(assert-equal nil (nan-ne-jit 1.0d0 1.0f0))   ; 数値として等しい
(assert-equal t (nan-ne-jit 0.1f0 0.1d0))     ; 数値として異なる(PR #80)
(assert-equal t (nan-ne-jit *inf-d* *ninf-d*))
(assert-equal nil (nan-ne-jit *inf-d* *inf-d*))

;;; --- max / min は順序依存になった(§4-3、直さない。記録のみ) ---
;;; num_gt / num_lt が非順序で偽を返すため、**NaN は最良値を更新できない**。
;;; 結果として **引数の順序で答えが変わる**。
;;; IEEE 754-2008 の maxNum(NaN を無視)とも 754-2019 の maximum(NaN を伝播)
;;; とも一致しない。どちらの流儀に寄せるかは将来の選択
;;; (documents/nan-comparison.md §4-3)。
(assert-equal "NAN" (nan-print (max *nan-d* 1.0d0)))    ; 先頭が NaN → NaN が残る
(assert-equal "1.0" (nan-print (max 1.0d0 *nan-d*)))    ; NaN 側が更新できない
(assert-equal "NAN" (nan-print (min *nan-d* 1.0d0)))
(assert-equal "1.0" (nan-print (min 1.0d0 *nan-d*)))
;; NaN が無ければ従来どおり
(assert-equal 2.0d0 (max 1.0d0 2.0d0))
(assert-equal 1.0d0 (min 1.0d0 2.0d0))
(assert-equal 3 (max 1 3 2))
(assert-equal 1 (min 3 1 2))
