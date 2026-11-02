;; test/lisp/convert_float_test.lisp
;;
;; convert による float 形式の変換の回帰テスト。documents/convert-float.md
;;
;; **float の形式を変換する手段は convert だけである。** 型昇格(c-1)は広いほうへ
;; 寄せるので演算では狭められず、(float x) は float をそのまま返す(型保存)。
;;
;; ISLisp の convert は class-name を評価しないので (convert 3 <float>) と裸で書く。
;; クオート付き (convert 3 '<float>) も同じ意味に取る(よくある書き間違いで、
;; そのまま渡すと原因の分かりにくい domain-error になるため)。

;;; --- 1. 基本の組み合わせ ---
(assert-equal '<single-float> (%%class-name (class-of (convert 1.5d0 <single-float>))))
(assert-equal '<double-float> (%%class-name (class-of (convert 1.5f0 <double-float>))))
;;; 恒等
(assert-equal '<single-float> (%%class-name (class-of (convert 1.5f0 <single-float>))))
(assert-equal '<double-float> (%%class-name (class-of (convert 1.5d0 <double-float>))))
;;; 整数から
(assert-equal '<single-float> (%%class-name (class-of (convert 1 <single-float>))))
(assert-equal '<double-float> (%%class-name (class-of (convert 1 <double-float>))))
(assert-equal '<single-float> (%%class-name (class-of (convert -3 <single-float>))))
;;; bignum から。2^100 = 1.27e30 で single の最大値(3.4e38)に収まるので通る
(assert-equal '<single-float> (%%class-name (class-of (convert (expt 2 100) <single-float>))))
(assert-equal '<double-float> (%%class-name (class-of (convert (expt 2 100) <double-float>))))

;;; 値
(assert-equal t (= 1.5f0 (convert 1.5d0 <single-float>)))
(assert-equal t (= 1.5d0 (convert 1.5f0 <double-float>)))
(assert-equal t (= 1.0f0 (convert 1 <single-float>)))
(assert-equal t (= -3.0f0 (convert -3 <single-float>)))
(assert-equal t (= 2.0d0 (convert 2 <double-float>)))

;;; クオート付きでも同じ
(assert-equal '<single-float> (%%class-name (class-of (convert 1.5d0 '<single-float>))))
(assert-equal '<double-float> (%%class-name (class-of (convert 1.5f0 '<double-float>))))
(assert-equal t (= 1.5f0 (convert 1.5d0 '<single-float>)))

;;; --- 2. 値の保存 ---
;;; single → double → single は元に戻る(広げてから狭めるだけなので情報が落ちない)
(assert-equal t (= 1.5f0 (convert (convert 1.5f0 <double-float>) <single-float>)))
(assert-equal t (= 0.1f0 (convert (convert 0.1f0 <double-float>) <single-float>)))
(assert-equal t (= -7.25f0 (convert (convert -7.25f0 <double-float>) <single-float>)))
;;; 型も戻る
(assert-equal '<single-float>
              (%%class-name (class-of (convert (convert 0.1f0 <double-float>) <single-float>))))

;;; double → single → double は**元に戻らない**(単精度へ落とした分の情報が消える)。
;;; これは仕様どおりである
(assert-equal nil (= 0.1d0 (convert (convert 0.1d0 <single-float>) <double-float>)))
;;; ただし単精度の刻みのぶんしか違わない
(assert-equal t (< (abs (- 0.1d0 (convert (convert 0.1d0 <single-float>) <double-float>)))
                   0.000001))
;;; 2進で正確に表せる値なら double → single → double でも戻る
(assert-equal t (= 1.5d0 (convert (convert 1.5d0 <single-float>) <double-float>)))
(assert-equal t (= 0.25d0 (convert (convert 0.25d0 <single-float>) <double-float>)))

;;; --- 3. 範囲外は domain-error ---
(assert-error (convert 1.0d300 <single-float>))
(assert-error (convert -1.0d300 <single-float>))
;;; single の最大値の2倍は double では有限だが single では表せない
(assert-error (convert (* *most-positive-single-float* 2.0d0) <single-float>))
(assert-error (convert (* *most-negative-single-float* 2.0d0) <single-float>))
;;; double の最大値も当然入らない
(assert-error (convert *most-positive-double-float* <single-float>))
;;; 巨大な bignum は double にした時点であふれる。**元が有限なので範囲外扱い**
;;; (入力が最初から無限大だった場合と区別している。documents/convert-float.md §4-3)
(assert-error (convert (expt 2 2000) <single-float>))

;;; --- 3b. 境界ちょうどは通る(オフバイワンの確認) ---
;;; *most-positive-single-float* 自身は single なので恒等になる。
;;; **double 経由で往復させて、狭める側の境界を実際に踏む**
(defglobal *cv-max-single-as-double* (convert *most-positive-single-float* <double-float>))
(defglobal *cv-min-single-as-double* (convert *most-negative-single-float* <double-float>))
(assert-equal '<double-float> (%%class-name (class-of *cv-max-single-as-double*)))
(assert-equal t (= *most-positive-single-float*
                   (convert *cv-max-single-as-double* <single-float>)))
(assert-equal t (= *most-negative-single-float*
                   (convert *cv-min-single-as-double* <single-float>)))
(assert-equal '<single-float>
              (%%class-name (class-of (convert *cv-max-single-as-double* <single-float>))))
;;; 恒等の経路でも通る
(assert-equal t (= *most-positive-single-float*
                   (convert *most-positive-single-float* <single-float>)))

;;; --- 4. アンダーフローは 0 へ丸める。エラーにしない ---
;;; 0 は有限の表現可能な結果であり、丸めとして連続している
;;; (オーバーフローには有限の答えが無いので、そちらだけをエラーにする)
(assert-equal '<single-float> (%%class-name (class-of (convert 1.0d-300 <single-float>))))
(assert-equal t (= 0.0f0 (convert 1.0d-300 <single-float>)))
(assert-equal t (= 0.0f0 (convert -1.0d-300 <single-float>)))
;;; single の非正規化数の範囲に入る値はそのまま残る(0 に潰れない)
(assert-equal t (> (convert 1.0d-40 <single-float>) 0.0f0))

;;; --- 5. 無限大と NaN はそのまま通す ---
;;; single でも表現できる値なのでエラーにしない。
;;; 無限大は (/ 1.0d0 0.0d0) で作れる(domain-error 未実装のための簡略化。
;;; runtime.c の primitive_divide のコメント参照)
(defglobal *cv-inf* (/ 1.0d0 0.0d0))
(defglobal *cv-ninf* (/ -1.0d0 0.0d0))
(defglobal *cv-nan* (/ 0.0d0 0.0d0))

(defun cv-print (x)
  (let ((s (create-string-output-stream)))
    (format s "~S" x)
    (get-output-stream-string s)))

;;; 作れていることの前提確認
(assert-equal "INF" (cv-print *cv-inf*))
(assert-equal "-INF" (cv-print *cv-ninf*))
(assert-equal "NAN" (cv-print *cv-nan*))

(assert-equal '<single-float> (%%class-name (class-of (convert *cv-inf* <single-float>))))
(assert-equal "INF" (cv-print (convert *cv-inf* <single-float>)))
(assert-equal '<single-float> (%%class-name (class-of (convert *cv-ninf* <single-float>))))
(assert-equal "-INF" (cv-print (convert *cv-ninf* <single-float>)))
(assert-equal '<single-float> (%%class-name (class-of (convert *cv-nan* <single-float>))))
(assert-equal "NAN" (cv-print (convert *cv-nan* <single-float>)))
;;; 広げる側も通る
(assert-equal "INF" (cv-print (convert *cv-inf* <double-float>)))

;;; --- 6. 抽象クラスと非数値 ---
;;; <number> は変換表に無いので domain-error(従来どおり)
(assert-error (convert 1.5d0 <number>))
;;; <float> は **ISLisp の変換表にある**ので通る(仕様例 (convert 3 <float>) => 3.0)。
;;; どちらの形式にするかは *read-default-float-format* に従う
(assert-equal t (floatp (convert 1.5d0 <float>)))
(assert-equal '<double-float> (%%class-name (class-of (convert 3 <float>))))
;;; 数値でないものを float 形式へ変換しようとしたら domain-error
(assert-error (convert "abc" <single-float>))
(assert-error (convert 'foo <single-float>))
(assert-error (convert #\a <single-float>))
(assert-error (convert '(1 2) <double-float>))
;;; float → 整数は本作業に含めない(floor/truncate がある)。現状の挙動を固定する
(assert-error (convert 1.5d0 <integer>))

;;; --- 7. 別名(<short-float> / <long-float>) ---
;;; *classes* 上は同一のクラスオブジェクトだが、%convert のディスパッチは
;;; **クラス名シンボルの case** なので、case のキーに並べて明示してある
;;; (documents/convert-float.md §4-1)
(assert-equal '<single-float> (%%class-name (class-of (convert 1.5d0 <short-float>))))
(assert-equal '<double-float> (%%class-name (class-of (convert 1.5f0 <long-float>))))
(assert-equal t (= 1.5f0 (convert 1.5d0 <short-float>)))
(assert-equal t (= 1.5d0 (convert 1.5f0 <long-float>)))
;;; 別名でも範囲外は同じく domain-error
(assert-error (convert 1.0d300 <short-float>))
;;; クラスオブジェクトが同一であることの再確認(PR #79)
(assert-equal t (eq (%find-class '<short-float>) (%find-class '<single-float>)))
(assert-equal t (eq (%find-class '<long-float>) (%find-class '<double-float>)))

;;; --- 8. 既存の convert が壊れていないこと ---
;;; ISLisp 仕様 §17 の変換表。%convert が対応している全組み合わせを通す
(assert-equal #\a (convert #\a <character>))
(assert-equal #\a (convert 97 <character>))
(assert-equal 97 (convert #\a <integer>))
(assert-equal 3 (convert 3 <integer>))
(assert-equal 3 (convert "3" <integer>))
(assert-equal t (= 3.0 (convert 3 <float>)))
(assert-equal t (= 1.5 (convert 1.5 <float>)))
(assert-equal t (= 1.5 (convert "1.5" <float>)))
(assert-equal 'abc (convert "abc" <symbol>))
(assert-equal 'abc (convert 'abc <symbol>))
;; シンボル名は intern 時に大文字化される(os_make_symbol)ので "ABC" になる
(assert-equal "ABC" (convert 'abc <string>))
(assert-equal "abc" (convert "abc" <string>))
(assert-equal "a" (convert #\a <string>))
(assert-equal "3" (convert 3 <string>))
(assert-equal #(#\a #\b #\c) (convert "abc" <general-vector>))
(assert-equal #(1 2) (convert '(1 2) <general-vector>))
(assert-equal '(a b) (convert #(a b) <list>))
(assert-equal '(#\a #\b) (convert "ab" <list>))
(assert-equal '(1 2) (convert '(1 2) <list>))
;;; 変換表に無い組み合わせは domain-error(従来どおり)
(assert-error (convert 'foo <integer>))
(assert-error (convert #\a <general-vector>))
