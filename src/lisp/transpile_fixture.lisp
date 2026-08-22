;;;; トランスパイラの動作確認用の最小フィクスチャ(M6時点)
;;;; fixnum/string/symbol/nil/tのリテラルとquote、パラメータ付きdefunをサポートする

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
