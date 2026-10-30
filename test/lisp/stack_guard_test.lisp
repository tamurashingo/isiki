;; test/lisp/stack_guard_test.lisp
;;
;; [可用性/回帰] **スタック枯渇がハングではなくエラーになる。** issue #110、
;; documents/stack-guard.md。
;;
;; 直す前の症状:
;;   JIT 関数は 1 段 7,536 byte 使う(ZA_FRAME_TOTAL 7,480 + push rbx/r13 16 +
;;   戻り番地 8 + 呼び出し側のシャドウスペース 32)。プロセスのスタックは 256KB
;;   なので **深さ 33〜34 で溢れる**(境界が範囲なのは呼び出し元のスタック使用量で
;;   1 段動くため)。溢れるとガードページ #PF で止まり、報告は
;;   `-serial stdio` にしか出ないので、**外から見ると QEMU の無反応と区別が付かない。**
;;   **何もしていない深さである。** 素朴な再帰で踏む。
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
;; 2026-10-07 の実測値は **27**(ZA_STACK_SIGNAL_RESERVE = 24,432。za.c の導出式)。
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
;; 2026-10-30: **探索範囲の上端を 40 -> 80 へ広げた。**
;;
;; `sgt-max` は範囲 [lo, hi] の中で成功した最大の d を返すので、
;; **真の深さが hi を超えていると hi をそのまま返す**。つまり
;; 「ちょうど hi」と「hi より深い」を区別できない。
;; フレームを実寸にすると深さは**増える方向**に動く(小さいフレームの関数が多いぶん
;; 同じスタックに多く積める)ので、上端のままだと**上限側の判定が意味を失う**。
;;
;; **上端に張り付いていないことを明示的に見る**(下の 3 つ目の assert)。
;; これがあれば、範囲が足りなくなった日に「黙って上端を返す」のではなく落ちる。
(defglobal sgt-search-hi 80)
(defglobal sgt-max-depth (sgt-max 15 sgt-search-hi))
(format *isiki-test-stream* "#stack-guard max-depth=~D (記録値 26。2026-10-07 は 27、探索上端 ~D)~%"
        sgt-max-depth sgt-search-hi)
(finish-output *isiki-test-stream*)
(assert-equal t (if (>= sgt-max-depth 20) t nil))
(assert-equal t (if (<= sgt-max-depth 34) t nil))
;; **探索が上端で打ち切られていないこと。** ここが落ちたら
;; `sgt-search-hi` を上げてから深さの期待値を取り直すこと(順序を逆にすると、
;; 上端に張り付いた値を「実測値」として記録してしまう)
(assert-equal t (if (< sgt-max-depth sgt-search-hi) t nil))

;; 2026-10-30 の実測は **26**(2026-10-07 の記録は 27)。帯(20〜34)の中なので落ちないが、
;; **1 段動いたこと自体は記録しておく**。PR #94 の取り込みで C 側のコードが変わったので、
;; JIT に入るまでに使う C スタックの量が変わったためと見られる(**推測。未確認**)。
;; フレーム寸法は 7480 のまま不変で、これは jit_frame_test.lisp が別途固定している。
;;
;; **陽性対照(規則 8、2026-10-30 に取得)**: `sgt-search-hi` を 20(真の深さより低い)にすると
;; 検出器は上端の 20 を返し、**上の 2 つの帯の判定(>= 20 / <= 34)は両方とも通ってしまう**。
;; 落ちるのはこの「張り付いていないこと」の判定だけだった。
;; つまりこの判定が無い状態では、**範囲が足りないことを誰も見ていなかった**。

;;; --- 7. 末尾呼び出しは深さを消費しないので、保護は発火しない ---
;;
;; 末尾呼び出しも callee の入口へ `jmp` するので**検査は通る**が、
;; フレームを積まないので残量は減らない。**ここが落ちたら末尾呼び出しが
;; 壊れている**(= 末尾呼び出しがフレームを積むようになった)
(defun sgt-tail (n acc) (if (= n 0) acc (sgt-tail (- n 1) (+ acc n))))
(assert-equal t (%%za-compiled-p (function sgt-tail)))
(assert-equal 500500 (sgt-tail 1000 0))

;;; --- 7b. ハンドラは signal の動的文脈(= 深いところ)で走り、完走する ---
;;
;; ISLisp の `with-handler` のハンドラは**巻き戻す前**に走る。だからハンドラが
;; 使う量も予備領域に入っていなければならない(`ZA_HANDLER_ALLOWANCE` = 4,096)。
;; **現実的な形のハンドラ**(条件のクラス名を引き、リストを組む)が通ることを固定する。
;;
;; **4,096 byte を超えるハンドラは保証していない**(documents/stack-guard.md §5-6)。
;; 超えた場合の挙動は形に依存し、`EVAL-ERROR` での劣化か報告付きの停止になる。
;; **止まる形はテストにできない**(落ちるのではなく機械が止まる)のでここには置かない。
(defun sgt-handler-realistic ()
  (block done
    (with-handler
        (lambda (c)
          (return-from done
            (list 'handled (%%class-name (class-of c))
                  (length (list 1 2 3 4 5 6 7 8 9 10)))))
      (sgt-rec 1000))))
(assert-equal '(handled <storage-exhausted> 10) (sgt-handler-realistic))
;; ハンドラの中で JIT 再帰 1 段も通る
(defun sgt-handler-rec1 ()
  (block done
    (with-handler (lambda (c) (return-from done (list 'h1 (sgt-rec 1)))) (sgt-rec 1000))))
(assert-equal '(h1 1) (sgt-handler-rec1))
;; ここまでで signal の打ち切りに当たっていないこと
(assert-equal 0 (%%diag-signal-overflows))

;;; --- 8. 相互再帰でも発火する(1 つの関数の自己再帰に限らない) ---

(defun sgt-even-ish (n) (if (= n 0) 0 (+ 1 (sgt-odd-ish (- n 1)))))
(defun sgt-odd-ish (n) (if (= n 0) 0 (+ 1 (sgt-even-ish (- n 1)))))
(assert-equal t (%%za-compiled-p (function sgt-even-ish)))
(assert-equal t (%%za-compiled-p (function sgt-odd-ish)))
(assert-equal 10 (sgt-even-ish 10))
(assert-error-class '<storage-exhausted> (sgt-even-ish 1000))
(assert-equal 10 (sgt-even-ish 10))
