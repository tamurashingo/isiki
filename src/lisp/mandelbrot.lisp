;; マンデルブロ集合の描画
;;
;; [工夫 2026-11-02] **描画先を引数で受ける mandelbrot-to を分けた。**
;;
;; もとは (mandelbrot) が自分の中で (open-output-stream) を開いていた。
;; 引数無しの open-output-stream は**画面出力ストリーム**(このカーネルの拡張)で、
;; os_stream_write_char は STREAM_OUTPUT_SCREEN をフレームバッファにしか書かない
;; (シリアルへは出ない)。したがって `-display none` の QEMU で走らせると
;; **描いた絵はどこにも残らず、終わって REPL に戻るだけ**になる。
;;
;; 計測や前後比較をするには絵が要る(「速くなった」が「別のものを計算していた」で
;; ないことを確かめるため)。そこで描画先を引数にした版を分け、
;; **(mandelbrot) はそれを画面ストリームで呼ぶだけ**にした。
;;
;; **実装は 1 つだけにしてある**(2 つに分けると片方だけ直される)。
;; もとの版との違いは、文字ごとに (standard-output) を呼び直さなくなったことだけで、
;; 出力は同じである。
(defun mandelbrot-to (out)
  (for ((y -1.0 (+ y 0.05)))
       ((> y 1.0) t)
    (for ((x -2.0 (+ x 0.03)))
         ((> x 0.5) (format-char out #\newline))
      (let ((zr 0.0) (zi 0.0) (i 0) (hit nil))
        (for () ((or (> i 30) hit) nil)
          (let ((zr2 (* zr zr))
                (zi2 (* zi zi)))
            (if (> (+ zr2 zi2) 4.0)
                (setq hit t)
                (progn
                  (setq zi (+ (* 2.0 zr zi) y))
                  (setq zr (+ (- zr2 zi2) x))
                  (setq i (+ i 1))))))
        (format-char out (if hit #\space #\*))))))

;; 画面へ描く(もとの (mandelbrot) と同じ振る舞い)
(defun mandelbrot ()
  (with-standard-output (open-output-stream)
    (mandelbrot-to (standard-output))))
