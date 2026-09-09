;; test/lisp/abi_bench.lisp
;;
;; ABI-M0: documents/abi-redesign.mdが構想する呼び出し規約刷新(consリスト経由の
;; 引数渡しを固定引数レジスタ渡しへ置き換える)の効果を、今後のマイルストン
;; (ABI-M2/M3/M5等)が「変更前後」で比較できるようにするための計測基盤。
;;
;; 「futex回数/N」(過去セッションの一時的な計測手法、Linux上でstrace等を使う
;; 前提でこのリポジトリには存在しない)の代わりに、isiki-os自身が持つ以下の
;; 2つのカウンタをLispから読める形で使う(%%HEAP-USED-BYTESは既存、
;; %%GC-COLLECT-COUNTはABI-M0で新設、runtime.c参照):
;;   - heap-delta / n : 1呼び出しあたりのヒープ確保バイト数(consリスト構築の
;;     有無に直結する、最も細かい粒度の指標)。ただし計測窓の途中でGCが発火すると
;;     ヒープ使用量が減少方向に振れてしまい意味を失うため、gc-delta=0の場合のみ
;;     有効な値として報告する。
;;   - gc-delta : 計測窓中に実際にGCが発火した回数(粗い指標。heap-deltaが
;;     信頼できない大きなnでの参考値)。
;;
;; za.cが生成する機械語の実行を伴うため(za_test.lispと同じ理由により)、
;; このファイルはQEMU実機上でのみ意味を持つ(make test-qemu-milestone
;; MILESTONE=test/lisp/qemu_boot_abi_bench.lisp)。test_framework.lispが
;; 定義するassert-equal等をそのまま使う(boot-entryスクリプトが本ファイルより
;; 先にそれをloadしている前提)。

;; loop-fnをn回呼ぶだけの薄いラッパー(loop-fn自身が自己再帰でn回分のループを
;; 内包する。funcall自体は計測窓内で1回しか発生しないため、動的呼び出しの
;; オーバーヘッドはnで割った際に無視できるほど小さくなる)。
(defun isiki-abi-bench-measure (label loop-fn n)
  (let ((heap-before (%%heap-used-bytes))
        (gc-before (%%gc-collect-count)))
    (funcall loop-fn n)
    (let ((heap-delta (- (%%heap-used-bytes) heap-before))
          (gc-delta (- (%%gc-collect-count) gc-before)))
      (if (= gc-delta 0)
          (format *isiki-test-stream* "[BENCH] ~A: n=~D heap-delta=~D bytes/call=~D gc-delta=0~%"
                  label n heap-delta (div heap-delta n))
          (format *isiki-test-stream*
                  "[BENCH] ~A: n=~D heap-delta=~D (計測窓中にGCが~D回発火したためbytes/callは無効)~%"
                  label n heap-delta gc-delta)))))

;; シナリオ0(baseline): 自己再帰のみで本体に呼び出しを一切挟まないホットループ。
;; za_compile_call(za.c:2669-2830)は自己再帰の引数(n-1、1個)自体もconsリスト化
;; するため、以下の全シナリオに共通して乗る「自己再帰そのものの確保コスト」を
;; 単独で計測し、シナリオ1/2の差分から差し引くための基準値にする。
(defun isiki-abi-bench-loop-baseline (n)
  (if (> n 0)
      (isiki-abi-bench-loop-baseline (- n 1))
      nil))

;; シナリオ1(fast path): (not n) の呼び出しを挟むホットループ。NOTはABI-M1で
;; za_compile_unary経由primitive_null1直接call(非allocating)の高速パスに乗った
;; ため、理論上はbaselineとほぼ同じ確保量になるはず。
(defun isiki-abi-bench-loop-not (n)
  (if (> n 0)
      (progn (not n) (isiki-abi-bench-loop-not (- n 1)))
      nil))

;; シナリオ2(slow path): ユーザー定義1引数関数の呼び出しを挟むホットループ。
;; za_compile_callの一般呼び出し経路は、呼び出し先が固定引数のユーザー定義関数
;; であっても常にconsリストを構築してFunction Cell経由で呼ぶため、
;; ABI-M5(JIT-to-JITユーザー定義関数の静的呼び出し新ABI化)が着手されるまでは
;; baseline+1コール分の追加確保が乗った「現状の基準値」になる。
(defun isiki-abi-bench-identity (x) x)
(defun isiki-abi-bench-loop-usercall (n)
  (if (> n 0)
      (progn (isiki-abi-bench-identity n) (isiki-abi-bench-loop-usercall (- n 1)))
      nil))

;; いずれもza.cでコンパイルされること自体が前提として崩れていないかの回帰確認。
(assert-equal t (%%za-compiled-p (function isiki-abi-bench-loop-baseline)))
(assert-equal t (%%za-compiled-p (function isiki-abi-bench-loop-not)))
(assert-equal t (%%za-compiled-p (function isiki-abi-bench-identity)))
(assert-equal t (%%za-compiled-p (function isiki-abi-bench-loop-usercall)))

;; n=500: GCを誘発しない範囲(GitHub ActionsのKVM無しQEMU/TCGでも現実的な時間で
;; 終わる)でbytes/callのベースラインを記録する。後続マイルストンはこの数値を
;; 変更前後で比較する(改善したかどうかの回帰テストではなく、観測ログとして
;; test-results.txtへ出力するのみ)。
(isiki-abi-bench-measure "baseline" (function isiki-abi-bench-loop-baseline) 500)
(isiki-abi-bench-measure "not(fast-path)" (function isiki-abi-bench-loop-not) 500)
(isiki-abi-bench-measure "usercall(slow-path)" (function isiki-abi-bench-loop-usercall) 500)
(close (open-output-file "/9p/tmp/ckpt-1-abi-bench.txt"))
