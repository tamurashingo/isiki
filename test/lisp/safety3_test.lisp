;; test/lisp/safety3_test.lisp
;;
;; [#148] **safety 3 で宣言した型を検証する。** documents/safety3-verify.md。
;;
;; ## 何が新しいか
;;
;; 既存の監査(`ISIKIOS_DECLARE_AUDIT`)は**別ビルド**で、失敗すると **os_panic** する。
;; panic は freestanding でハンドラが無いので、**外から見ると QEMU の無反応と
;; 区別が付かない**(#110 で族として扱った「結果が無い」と同じ形。
;; 実際この試験を書く前に、監査ビルドの陽性対照で**シリアルを捕まえないと
;; 対照が成立しない**ことを踏んでいる)。
;;
;; **safety 3 は本番ビルドのまま関数単位で効き、signal する。**
;; だから「嘘を捕まえたあとも試験が続けられる」——それがこのファイルの形である。
;;
;; ## 一方向の定義は崩していない
;;
;; #112 の結論は「safety は検査を**外してよい**という許可であって、
;; 外れることの保証ではない」。safety 3 はその許可を**与えない**うえに、
;; **食い違いを報告する**段である。

;;; --- 1. safety 3 + 正しい宣言 → ふつうに動く ---
(defun s3-ok (x y)
  (declare (optimize (safety 3))) (declare (type <fixnum> x)) (declare (type <fixnum> y))
  (+ x y))
(assert-equal t (%%za-compiled-p (function s3-ok)))
(assert-equal 7 (s3-ok 3 4))
(assert-equal 0 (s3-ok 0 0))
(assert-equal -1 (s3-ok 2 -3))

;;; --- 2. safety 3 + **嘘の宣言** → signal される(**止まらない**)---
;; fixnum と宣言して float を渡す
(defun s3-lie (x y)
  (declare (optimize (safety 3))) (declare (type <fixnum> x)) (declare (type <fixnum> y))
  (+ x y))
(assert-equal t (%%za-compiled-p (function s3-lie)))
(assert-error (s3-lie 1.5 2))
(assert-error (s3-lie 2 1.5))
(assert-error (s3-lie 1.5 2.5))
;; 文字列でも捕まる
(assert-error (s3-lie "a" 2))
;; **<domain-error> である**
(assert-error-class '<domain-error> (s3-lie 1.5 2))

;;; --- 3. **捕まえたあとも生きている**(ここが panic との違い)---
;; panic なら機械が止まるので、この行は実行されない
(assert-equal 7 (s3-ok 3 4))
(assert-equal 7 (s3-lie 3 4))

;;; --- 4. 比較でも効く(#165 で呼び出しの出し方を 1 箇所に寄せたため)---
(defun s3-lt (x y)
  (declare (optimize (safety 3))) (declare (type <fixnum> x)) (declare (type <fixnum> y))
  (< x y))
(assert-equal t (s3-lt 3 5))
(assert-equal nil (s3-lt 5 3))
(assert-error-class '<domain-error> (s3-lt 1.5 2))

;;; --- 5. single の宣言でも効く ---
(defun s3-sgl (x y)
  (declare (optimize (safety 3)))
  (declare (type <single-float> x)) (declare (type <single-float> y))
  (+ x y))
(assert-equal 4.0f0 (s3-sgl 1.5f0 2.5f0))
;; fixnum を渡すと捕まる
(assert-error-class '<domain-error> (s3-sgl 1 2.5f0))
(assert-error-class '<domain-error> (s3-sgl 1.5f0 2))

;;; --- 6. **対照群: safety 3 でなければ検査は出ない** ---
;; 既定(safety 1/2)では宣言を信じる。嘘をついても signal されない
;; (**それが #112 の一方向の定義である**: 外してよいという許可)。
;; ここで signal されるようになったら、既定の経路に検査が漏れている
(defun s3-default (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y))
  (+ x y))
(assert-equal t (%%za-compiled-p (function s3-default)))
(assert-equal 7 (s3-default 3 4))
;; **float を渡しても signal されない**(宣言を信じて計算する)。
;; 答えが何になるかは宣言した人の責任なので、ここでは「signal されない」ことだけ見る
(defglobal s3-default-lie (s3-default 1.5 2))
(assert-equal t (if (= s3-default-lie s3-default-lie) t t))

;;; --- 7. safety 0(検査を外す)と並べて、3 段が別物であることを見る ---
(defun s3-zero (x y)
  (declare (optimize (safety 0))) (declare (type <fixnum> x)) (declare (type <fixnum> y))
  (+ x y))
(assert-equal 7 (s3-zero 3 4))
;; safety 0 / 既定 / safety 3 で**生成コードが違う**(段が効いている)
(format *isiki-test-stream* "#s3-codelen zero=~D default=~D three=~D~%"
        (%%disasm-code-len 's3-zero) (%%disasm-code-len 's3-default)
        (%%disasm-code-len 's3-ok))
(assert-equal t (if (= (%%disasm-code-len 's3-default) (%%disasm-code-len 's3-ok)) nil t))
