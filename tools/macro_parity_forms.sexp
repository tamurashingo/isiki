; [二重定義マクロの照合] host(src/lisp/transpile.lisp の *macro-expanders*)と
; guest(src/lisp/init.lisp の defmacro)を同じフォームで展開して突き合わせる。
;
; **フォームはここだけに書く。** host 用と guest 用のドライバは
; tools/check_macro_parity.sh がこのファイルから生成する
; (2 箇所に書けば片方だけ直される。bench_jit.lisp と同じ方針)。
;
; 1 行 1 件。最初の語がマクロ名(= *macro-expanders* の key)、残りが展開するフォーム。
LET	(let ((a 1) (b 2)) (f a b))
LET*	(let* ((a 1) (b a)) (f a b))
COND	(cond ((p1) (r1)) ((p2) (r2)) (t (r3)))
CASE	(case (k) ((1 2) (r1)) ((3) (r2)) (t (r3)))
CASE-USING	(case-using (pred) (k) ((1) (r1)) (t (r2)))
SETF	(setf (car x) v)
FOR	(for ((i 0 (+ i 1)) (acc 0 (+ acc i))) ((>= i n) acc) (body1) (body2))
WHILE	(while (< i n) (body1) (body2))
WITH-OPEN-INPUT-STREAM	(with-open-input-stream (s (mk)) (b1) (b2))
WITH-OPEN-INPUT-FILE	(with-open-input-file (s "p") (b1))
WITH-OPEN-OUTPUT-STREAM	(with-open-output-stream (s (mk)) (b1))
WITH-OPEN-OUTPUT-FILE	(with-open-output-file (s "p") (b1))
