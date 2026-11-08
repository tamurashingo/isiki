;; [計測] mandelbrot を計り、**絵と数値を 9p のファイルへ残す**。
;;
;;   make test-qemu-milestone MILESTONE=tools/mandel_run.lisp
;;
;; ## なぜ専用の走らせ方が要るのか
;;
;; mandelbrot は画面(フレームバッファ)へ描く。**シリアルへは出ない**ので
;; `-display none` では何も残らず、終わって REPL に戻るだけになる。
;; .qemu-test-trigger の仕組みは「このファイルを load してから電源を切る」なので、
;; **ここでファイルへ書いておけば、ホスト側に絵と数値が残る。**
;;
;; ## **このファイルは git に入れない(追跡しない)**
;;
;; 最適化の前後を比べるために `git checkout <古いコミット>` を挟む。
;; **追跡していないファイルは checkout で消えない**ので、
;; 前後の両方で**同一のドライバ**を使える。
;;
;; ## 試験の枠組みを使わない理由
;;
;; 旧 main(PR #78)の test_framework.lisp には isiki-file-end-check が無い。
;; **前後で同じファイルを使う**ために、open-output-file と format だけで書く。
;; test-results.txt の " 0 failed" は make 側の門番が見るので自分で書く。

(load "src/lisp/mandelbrot.lisp")

;; 絵。前後で byte 比較するので、これが**同じ計算をした証拠**になる
(defglobal m-img (open-output-file "/9p/tmp/mandel_image.txt"))

(defglobal m-gc0 (%%gc-collect-count))
(defglobal m-h0 (%%heap-used-bytes))
(defglobal m-t0 (get-internal-real-time))
(defglobal m-r (mandelbrot-to m-img))
(defglobal m-t1 (get-internal-real-time))
(defglobal m-gc1 (%%gc-collect-count))
(defglobal m-h1 (%%heap-used-bytes))

(finish-output m-img)
(close m-img)

;; 数値。**100 tick = 1 秒**(src/c/clock.c の TICKS_PER_SECOND)
(defglobal m-res (open-output-file "/9p/test-results.txt"))
(format m-res "#mandel ticks=~D gc=~D heap-delta=~D result=~S~%"
        (- m-t1 m-t0) (- m-gc1 m-gc0) (- m-h1 m-h0) m-r)
;; **heap-delta は gc が 0 のときだけ意味を持つ**(GC が走ると水位が戻る)。
;; mandelbrot は浮動小数を箱で作るので GC は必ず走る。だから主な量は ticks である
(format m-res "#mandel note heap-delta is meaningful only when gc=0~%")
(format m-res "~%==== isiki tests: 1 passed, 0 failed ====~%")
(finish-output m-res)
(close m-res)
