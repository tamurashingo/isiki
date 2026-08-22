;;;; トランスパイラの動作確認用の最小フィクスチャ(M3時点)
;;;; fixnum/string/symbol/nil/tのリテラルとquoteのみサポートする

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
