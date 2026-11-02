#!/usr/bin/env python3
"""[二重定義マクロの照合] host/guest 両方の展開ドライバを 1 つのフォーム表から生成する。

**フォームを 2 箇所に書けば、いつか片方だけ直される。**
真実源は tools/macro_parity_forms.sexp だけにし、ここから
  tmp/macro_parity_host.lisp   (SBCL/roswell で transpile.lisp の展開関数を呼ぶ)
  tmp/macro_parity_guest.lisp  (QEMU ゲストで init.lisp の macroexpand-1 を呼ぶ)
を生成する。bench_jit.lisp を bench_aot.lisp から生成するのと同じ方針。
"""
import os
import sys

FORMS = "tools/macro_parity_forms.sexp"


def read_forms():
    out = []
    with open(FORMS, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith(";"):
                continue
            name, form = line.split("\t", 1)
            out.append((name.strip(), form.strip()))
    return out


HOST_HEAD = '''; 自動生成(tools/gen_macro_parity_drivers.py)。編集しない。
(load "src/lisp/transpile.lisp")
(let ((*print-case* :upcase) (*print-pretty* nil) (*print-readably* nil))
  (dolist (e (list
'''
HOST_TAIL = '''    ))
    (let* ((name (first e)) (form (second e))
           (fn (cdr (assoc (intern (string-upcase name)) *macro-expanders*))))
      (format t "~&#HOST ~A :: ~S~%" name
              (if fn (funcall (symbol-function (intern (string-upcase fn))) form)
                  :NO-EXPANDER)))))
'''

GUEST_HEAD = ''';; 自動生成(tools/gen_macro_parity_drivers.py)。編集しない。
(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(defun %macro-parity-dump (label form)
  (progn
    (format *isiki-test-stream* "#GUEST ~A :: ~S~%" label (macroexpand-1 form))
    (finish-output *isiki-test-stream*)))
'''
GUEST_TAIL = '''(isiki-test-report)
(close *isiki-test-stream*)
'''


def main():
    forms = read_forms()
    if not forms:
        sys.exit(f"ERROR: {FORMS} にフォームが 1 件も無い")
    os.makedirs("tmp", exist_ok=True)
    with open("tmp/macro_parity_host.lisp", "w", encoding="utf-8") as f:
        f.write(HOST_HEAD)
        for name, form in forms:
            f.write(f'    (list "{name}" \'{form})\n')
        f.write(HOST_TAIL)
    with open("tmp/macro_parity_guest.lisp", "w", encoding="utf-8") as f:
        f.write(GUEST_HEAD)
        for name, form in forms:
            f.write(f"(%macro-parity-dump \"{name}\" '{form})\n")
        f.write(GUEST_TAIL)
    print(f"生成: tmp/macro_parity_host.lisp / tmp/macro_parity_guest.lisp "
          f"({len(forms)} 件)")


if __name__ == "__main__":
    main()
