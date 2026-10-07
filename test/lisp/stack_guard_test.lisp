;; test/lisp/stack_guard_test.lisp
;;
;; [可用性/回帰] **スタック枯渇がハングではなくエラーになる。** issue #110、
;; documents/stack-guard.md。
;;
;; 直す前の症状:
;;   JIT 関数は 1 段 7,536 byte 使う(ZA_FRAME_TOTAL 7,480 + push rbx/r13 16 +
;;   戻り番地 8 + 呼び出し側のシャドウスペース 32)。プロセスのスタックは 256KB
;;   なので **深さ 34 で溢れる。** 溢れるとガードページ #PF で止まり、報告は
;;   `-serial stdio` にしか出ないので、**外から見ると QEMU の無反応と区別が付かない。**
;;   **深さ 34 は何もしていない深さである。** 素朴な再帰で踏む。
;;
;; 直したあと:
;;   プロローグの先頭(`sub rsp` より前)で残量を見て、足りなければ
;;   `<storage-exhausted>` を signal する。
;;
;; **このファイル自体が本作業の副産物である。**
;; PR #120 の時点では「深さを直接探るテストは書けない」と記録していた
;; (探ると溢れてハングし、「落ちる」にならない)。**signal が入ると
;; ハングしなくなるので、深さを直接探れる。**
;;
;; 注意: `<storage-exhausted>` は `<serious-condition>` の直接の子であって
;; **`<error>` の子ではない。** だから `ignore-errors` では捕まらない
;; (ISLisp のクラス階層どおり)。`with-handler` を使う。

;;; --- 1. 浅い呼び出しは何も変わらない ---

(defun sgt-leaf (n) (+ n 1))
(assert-equal t (%%za-compiled-p (function sgt-leaf)))
(assert-equal 6 (sgt-leaf 5))

(defun sgt-rec (n) (if (= n 0) 0 (+ n (sgt-rec (- n 1)))))
(assert-equal t (%%za-compiled-p (function sgt-rec)))

;; 深さ 20 は従来どおり成功する。
;; **上限ぎりぎりを書かないこと** — 呼び出し元が使う C スタックのぶんで
;; 結果が変わる(PR #120 / documents/for-expansion.md で実際に踏んだ)
(assert-equal 210 (sgt-rec 20))

;;; --- 2. 深さ 1000 は <storage-exhausted> になる(ハングしない) ---

(assert-error-class '<storage-exhausted> (sgt-rec 1000))

;;; --- 3. signal したあとも REPL が生きている ---

(assert-equal 3 (+ 1 2))
(assert-equal 210 (sgt-rec 20))

;;; --- 4. 1 回だけではない。何度でも枯渇して戻れる ---

(assert-error-class '<storage-exhausted> (sgt-rec 1000))
(assert-error-class '<storage-exhausted> (sgt-rec 500))
(assert-equal 210 (sgt-rec 20))

;;; --- 5. signal の深さ打ち切り(SIGNAL_MAX_DEPTH 16)に当たっていないこと ---
;;
;; 当たると、**signal しようとして打ち切られ、何も起きない**
;; (os_signal_condition が g_sym_eval_error を返す)。
;; 上で 3 回枯渇させたあとで 0 であること = 1 回も打ち切られていない
(assert-equal 0 (%%diag-signal-overflows))

;;; --- 6. 最大深さを直接探れる(本作業の副産物) ---
;;
;; **ハングしないので、深さを 1 つずつ上げて限界を求められる。**
;; 2026-10-07 の実測値は **27**(ZA_STACK_SIGNAL_RESERVE = 24,576)。
;;
;; 帯で見るのは、呼び出し元のスタック使用量で 1〜2 段動くため。
;;   **下限 20**: 予備領域を増やしすぎると深さが削られる。ここで気づく
;;   **上限 34**: 保護が効かなくなった / フレームが縮んだ場合に気づく
;; **どちらに外れても落ちる**(規則 9 の双方向)。
(defun sgt-try (d)
  (block done
    (with-handler (lambda (c) (return-from done -1))
      (progn (sgt-rec d) d))))
(defun sgt-max (lo hi)
  (let ((d lo) (best -1))
    (progn (while (<= d hi)
             (progn (if (> (sgt-try d) 0) (setq best d) nil) (setq d (+ d 1))))
           best)))
(defglobal sgt-max-depth (sgt-max 15 40))
(format *isiki-test-stream* "#stack-guard max-depth=~D (記録値 27)~%" sgt-max-depth)
(finish-output *isiki-test-stream*)
(assert-equal t (if (>= sgt-max-depth 20) t nil))
(assert-equal t (if (<= sgt-max-depth 34) t nil))

;;; --- 7. 末尾呼び出しは深さを消費しないので、保護は発火しない ---
;;
;; 末尾呼び出しも callee の入口へ `jmp` するので**検査は通る**が、
;; フレームを積まないので残量は減らない。**ここが落ちたら末尾呼び出しが
;; 壊れている**(= 末尾呼び出しがフレームを積むようになった)
(defun sgt-tail (n acc) (if (= n 0) acc (sgt-tail (- n 1) (+ acc n))))
(assert-equal t (%%za-compiled-p (function sgt-tail)))
(assert-equal 500500 (sgt-tail 1000 0))

;;; --- 8. 相互再帰でも発火する(1 つの関数の自己再帰に限らない) ---

(defun sgt-even-ish (n) (if (= n 0) 0 (+ 1 (sgt-odd-ish (- n 1)))))
(defun sgt-odd-ish (n) (if (= n 0) 0 (+ 1 (sgt-even-ish (- n 1)))))
(assert-equal t (%%za-compiled-p (function sgt-even-ish)))
(assert-equal t (%%za-compiled-p (function sgt-odd-ish)))
(assert-equal 10 (sgt-even-ish 10))
(assert-error-class '<storage-exhausted> (sgt-even-ish 1000))
(assert-equal 10 (sgt-even-ish 10))
