;; test/lisp/bench_jit_guard.lisp
;;
;; [性能測定] JIT 経路のベンチを測る**前に**、対象が本当に JIT に乗っている
;; ことを確認するゲート(documents/performance-measurement.md「JIT 経路の
;; 基準値」節)。
;;
;; なぜ要るのか:
;;   %%bench-jit-* が何かの理由でインタプリタへ落ちていても、ベンチは動いて
;;   数字を出す。その数字は「JIT を測ったつもりでインタプリタを測った値」で、
;;   名前が jit と書いてあるぶん AOT ベンチが 165 コミット気づかれなかった件
;;   より悪い。**名前は根拠にならない。**
;;
;; 方針:
;;   - 目視ではなく %%za-compiled-p を見る
;;   - T でなければ**測定を始めずに落ちる**(警告して続行にはしない。
;;     数字が出てしまえば誰かが使う)
;;   - カテゴリごとに、そのカテゴリが実際に呼ぶ関数すべてを確認する
;;     (1 つだけ確認して全部を代表させない)
;;
;; 使い方(tools/bench/run_jit_bench.sh が生成する milestone の形):
;;   (bench-jit-guard "%%bench-jit-tailrec"      (function %%bench-jit-tailrec))
;;   (bench-jit-guard "%%bench-jit-tailrec-step" (function %%bench-jit-tailrec-step))
;;   (if (bench-jit-guard-gate)
;;       <測定>
;;     <測定せずに落とす>)
;;
;; 陽性対照は test/lisp/bench_jit_guard_test.lisp にある。
;; **落ちることを見ずに「検査を入れた」と書いてはいけない**(規則 8)。

;; guard が JIT 化を確認できなかった件数。bench-jit-guard-gate が読む
(defglobal *bench-jit-guard-ng* 0)

;; LABEL(関数名の文字列)と FN(関数オブジェクト)を取り、JIT 化されていれば
;; t を返す。されていなければ nil を返し、件数を数えて理由を書き出す。
;; 行は即座に finish-output するので、この後でゲストが止まっても記録は残る
(defun bench-jit-guard (label fn)
  (let ((p (%%za-compiled-p fn)))
    (progn
      (if (eq p t)
          (format *isiki-test-stream* "[JIT-OK] ~A %%za-compiled-p=T~%" label)
        (progn
          (setq *bench-jit-guard-ng* (+ *bench-jit-guard-ng* 1))
          (format *isiki-test-stream*
                  "[JIT-NG] ~A %%za-compiled-p=~S -- JIT に乗っていない。このカテゴリは測定しない~%"
                  label p)))
      (finish-output *isiki-test-stream*)
      (eq p t))))

;; guard が 1 件でも NG なら nil を返し、同時に test-results.txt の failed を
;; 増やして make test-qemu-* 側(" 0 failed" の grep)を落とす。
;; 呼び出し側は戻り値が t のときだけ測定すること
(defun bench-jit-guard-gate ()
  (if (> *bench-jit-guard-ng* 0)
      (progn
        (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
        (format *isiki-test-stream*
                "[NG] bench-jit-guard: ~D 個が JIT に乗っていない。測定値は出していない~%"
                *bench-jit-guard-ng*)
        (finish-output *isiki-test-stream*)
        nil)
    t))

;; ---------------------------------------------------------------------------
;; [性能測定] 1 反復あたりの確保量(byte)。
;;
;; **同じ S 式でも、経路によって展開形が違うことがある。**
;; その差は命令数より確保量にきれいに出る(確保量はこの環境でぶれない計器。規則 9)。
;; documents/performance-measurement.md「for の 2.6 倍の切り分け」A-4。
;;
;; N1 回と N2 回を続けて実行し、確保量の差を (N2 - N1) で割る。
;; 固定費は両方に等しく乗るので引き算で消える(命令数の傾き法と同じ考え方)。
;;
;; **GC が走ったら無効。** GC は確保量を減らすので、傾きが負や 0 に化ける。
;; 走ったら -1 を返す(呼び出し側のアサーションが落ちる)ので、
;; **黙って嘘の 0 を返すことはない。** N は GC が走らない範囲で選ぶこと
;; (実測: %%bench-jit-for は 60,000 反復で約 1.9MB、GC 0 回)
(defun bench-jit-alloc-per-iter (f n1 n2)
  (let ((g0 (%%gc-collect-count)) (a0 (%%heap-used-bytes)))
    (progn
      (funcall f n1)
      (let ((a1 (%%heap-used-bytes)))
        (progn
          (funcall f n2)
          (let ((a2 (%%heap-used-bytes)) (g1 (%%gc-collect-count)))
            (if (= g0 g1)
                (div (- (- a2 a1) (- a1 a0)) (- n2 n1))
              -1)))))))
