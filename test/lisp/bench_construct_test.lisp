;; test/lisp/bench_construct_test.lisp
;;
;; [性能測定] 構文別ベンチマークスイート(documents/performance-measurement.md
;; 「構文別ベンチマークスイート」節)の正当性テスト。
;;
;; ベンチマークの比(AOT版命令数 ÷ C版命令数)に意味があるのは、両者が
;; 「同じ計算」をしている場合に限る。そのため、小さいN(仕事の単位数)で
;; 両実装を実行し、互いに一致すること・期待値と一致することを確認する。
;; 再帰系は深さ1000で区切るため、Nは1000の倍数にする必要がある。

;; Nは1000の倍数(再帰系がN/1000回の繰り返しになるため)
(defglobal bench-n 2000)

;;; --- 1. 単純ループ: nから0までデクリメントするので結果は常に0 ---
(assert-equal 0 (%%bench-c-loop bench-n))
(assert-equal 0 (%%bench-aot-loop bench-n))

;;; --- 2. fixnum算術: 0+1+...+(n-1) = 1999000 ---
(assert-equal 1999000 (%%bench-c-arith bench-n))
(assert-equal 1999000 (%%bench-aot-arith bench-n))

;;; --- 3. 末尾再帰: (1+...+1000)を(n/1000)回 = 500500*2 ---
(assert-equal 1001000 (%%bench-c-tailrec bench-n))
(assert-equal 1001000 (%%bench-aot-tailrec bench-n))

;;; --- 4. 非末尾再帰: 末尾再帰と同じ計算結果になる ---
(assert-equal 1001000 (%%bench-c-nontailrec bench-n))
(assert-equal 1001000 (%%bench-aot-nontailrec bench-n))

;;; --- 5. 条件分岐: 5分岐を巡回し1+2+3+4+5=15を(n/5)回 = 6000 ---
(assert-equal 6000 (%%bench-c-branch bench-n))
(assert-equal 6000 (%%bench-aot-branch bench-n))

;;; --- 6. let束縛: (i+4)の総和 = 1999000 + 4*2000 ---
(assert-equal 2007000 (%%bench-c-let bench-n))
(assert-equal 2007000 (%%bench-aot-let bench-n))

;;; --- 7. cons/リスト操作: (0+...+999)を(n/1000)回 = 499500*2 ---
(assert-equal 999000 (%%bench-c-cons bench-n))
(assert-equal 999000 (%%bench-aot-cons bench-n))

;;; --- 8. イテレーション構文(for): 算術と同じ総和 ---
(assert-equal 1999000 (%%bench-c-for bench-n))
(assert-equal 1999000 (%%bench-aot-for bench-n))

;;; --- 9. ベクタ操作: 書き込んだiをそのまま読み出して加算 = 総和 ---
(assert-equal 1999000 (%%bench-c-vector bench-n))
(assert-equal 1999000 (%%bench-aot-vector bench-n))

;;; --- 10. 関数呼び出し: accを1ずつn回増やす = n ---
(assert-equal 2000 (%%bench-c-funcall bench-n))
(assert-equal 2000 (%%bench-aot-funcall bench-n))
