;; test/lisp/jit_nest_depth_test.lisp
;;
;; za_compile_expr の再帰段数の上限(ZA_MAX_COMPILE_NEST)の回帰テスト。
;;
;; 上限が無かった頃、深くネストした式を defun するとコンパイル中に C スタックを
;; 踏み越え、ガードページ #PF でプロセスが止まっていた(79 段で再現。シリアルには
;; "** STACK OVERFLOW (下端側) **" が出るが、その式は二度と評価されないので
;; 外からは固まったようにしか見えない。documents/known-issue-deep-if-nesting-jit.md)。
;;
;; 上限を超えたら JIT を断念してインタプリタへ落ちる。遅くなるだけで結果は変わらない。
;; ここで見るのは「深くても返ってくること」と「値が正しいこと」の2点。
;;
;; 段数は ZA_MAX_COMPILE_NEST(60)に対する境界。za.c 側の値を変えたらここも直すこと。

(defun jnd-t () 1)

;;; --- 上限の手前: JIT される ---
(defun jnd-59 () (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 0))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))
(assert-equal t (%%za-compiled-p (function jnd-59)))
(assert-equal 1 (jnd-59))

;;; --- 上限ちょうど: 断念してインタプリタへ落ちる。値は変わらない ---
(defun jnd-60 () (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 0)))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))
(assert-equal nil (%%za-compiled-p (function jnd-60)))
(assert-equal 1 (jnd-60))

;;; --- かつてスタックを踏み越えていた深さ。返ってきて、正しい値であること ---
(defun jnd-100 () (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 (if (jnd-t) 1 0)))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))
(assert-equal nil (%%za-compiled-p (function jnd-100)))
(assert-equal 1 (jnd-100))

;;; --- 断念しても次の defun は普通に JIT されること(段数カウンタが戻っている) ---
(defun jnd-after () (if (jnd-t) 7 8))
(assert-equal t (%%za-compiled-p (function jnd-after)))
(assert-equal 7 (jnd-after))
