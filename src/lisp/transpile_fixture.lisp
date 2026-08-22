;;;; トランスパイラの動作確認用の最小フィクスチャ(M4時点)
;;;; fixnum/string/symbol/nil/tのリテラルとquote、パラメータ付きdefun、
;;;; if/progn/setqをサポートする

(defun %%transpile-fixture-answer ()
  42)

(defun %%transpile-fixture-string ()
  "hello")

(defun %%transpile-fixture-symbol ()
  'foo)

(defun %%transpile-fixture-nil ()
  nil)

(defun %%transpile-fixture-t ()
  t)

(defun %%transpile-fixture-quoted-fixnum ()
  '99)

(defun %%transpile-fixture-identity (x)
  x)

(defun %%transpile-fixture-second-param (x y)
  y)

(defun %%transpile-fixture-if (x)
  (if x 1 2))

(defun %%transpile-fixture-if-no-else (x)
  (if x 42))

(defun %%transpile-fixture-progn (x)
  (progn x 99))

(defun %%transpile-fixture-setq (x)
  (setq x 7))
