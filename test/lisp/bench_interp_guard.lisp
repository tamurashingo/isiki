;; test/lisp/bench_interp_guard.lisp
;;
;; [性能測定] 解釈実行の経路のベンチを測る**前に**、対象が本当に
;; インタプリタで走っていることを確認するゲート
;; (documents/performance-measurement.md「解釈実行の経路の基準値」節)。
;;
;; **JIT 側のゲート1 の逆向きである。**
;;   test/lisp/bench_jit_guard.lisp  %%za-compiled-p が **T** であること
;;   ここ                            %%za-compiled-p が **NIL** であること
;;
;; なぜ要るのか:
;;   src/lisp/bench_interp.lisp は defglobal + lambda で包んである。
;;   いま lambda は src/c/eval.c の eval_lambda が make_interpreted_function を
;;   直接呼ぶだけなので必ずインタプリタになるが、**それは実装の現在の性質で
;;   あって、仕様ではない。** lambda がコンパイルされるようになった日に、
;;   この経路の数字は黙って「インタプリタを測ったつもりで JIT を測った値」に
;;   なる。名前は根拠にならない(issue #114 は名前にしか書かれていない事実を
;;   165 コミット見逃した件である)。
;;
;; 方針は JIT 側と同じ:
;;   - 目視ではなく %%za-compiled-p を見る
;;   - NIL でなければ**測定を始めずに落ちる**(警告して続行にはしない。
;;     数字が出てしまえば誰かが使う)
;;   - カテゴリごとに、そのカテゴリが実際に評価する関数すべてを確認する
;;
;; 使い方(tools/bench/run_interp_bench.sh が生成する milestone の形):
;;   (bench-interp-guard "%%bench-interp-loop" %%bench-interp-loop)
;;   (if (bench-interp-guard-gate)
;;       <測定>
;;     <測定せずに落とす>)
;;
;; **引数が関数オブジェクトそのものである点が JIT 側と違う。**
;; bench_interp.lisp は defglobal なので (function X) ではなく変数 X を渡す。
;;
;; 陰性対照と陽性対照は test/lisp/bench_interp_test.lisp にある。
;; **落ちることを見ずに「検査を入れた」と書いてはいけない**(規則 8)。

;; guard がインタプリタであることを確認できなかった件数。
;; bench-interp-guard-gate が読む
(defglobal *bench-interp-guard-ng* 0)

;; LABEL(名前の文字列)と FN(関数オブジェクト)を取り、**JIT 化されていなければ**
;; t を返す。されていたら nil を返し、件数を数えて理由を書き出す。
;; 行は即座に finish-output するので、この後でゲストが止まっても記録は残る
(defun bench-interp-guard (label fn)
  (let ((p (%%za-compiled-p fn)))
    (progn
      (if (null p)
          (format *isiki-test-stream* "[INTERP-OK] ~A %%za-compiled-p=NIL~%" label)
        (progn
          (setq *bench-interp-guard-ng* (+ *bench-interp-guard-ng* 1))
          (format *isiki-test-stream*
                  "[INTERP-NG] ~A %%za-compiled-p=~S -- JIT に乗ってしまっている。このカテゴリは測定しない~%"
                  label p)))
      (finish-output *isiki-test-stream*)
      (null p))))

;; guard が 1 件でも NG なら nil を返し、同時に test-results.txt の failed を
;; 増やして make test-qemu-* 側(" 0 failed" の grep)を落とす。
;; 呼び出し側は戻り値が t のときだけ測定すること
(defun bench-interp-guard-gate ()
  (if (> *bench-interp-guard-ng* 0)
      (progn
        (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
        (format *isiki-test-stream*
                "[NG] bench-interp-guard: ~D 個が JIT に乗っている。測定値は出していない~%"
                *bench-interp-guard-ng*)
        (finish-output *isiki-test-stream*)
        nil)
    t))
