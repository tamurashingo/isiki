import re, sys

src = open('test/lisp/isiki_test.lisp').read()

# ---------- lexer helpers ----------
DELIMS = set(' \t\n\r()";\'`,')

def scan_form(text, i):
    """text[i] == '('. Return index just past the matching ')'."""
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == ';':
            while i < n and text[i] != '\n': i += 1
            continue
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == '\\': i += 1
                i += 1
            i += 1
            continue
        if c == '#' and i + 1 < n and text[i+1] == '\\':
            i += 3  # '#', '\', the char itself (may be a delimiter)
            while i < n and text[i] not in DELIMS: i += 1
            continue
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError('unbalanced')

def scan_atom(text, i):
    n = len(text)
    if text[i] == '"':
        i += 1
        while i < n and text[i] != '"':
            if text[i] == '\\': i += 1
            i += 1
        return i + 1
    if text[i] == '#' and i + 1 < n and text[i+1] == '\\':
        i += 3
        while i < n and text[i] not in DELIMS: i += 1
        return i
    while i < n and text[i] not in DELIMS: i += 1
    return i

def scan_elem(text, i):
    """Return end index of one element starting at i (prefixes like ' ` , ,@ #' #( #2A allowed)."""
    n = len(text)
    while i < n and text[i] in "'`,@":
        i += 1
    if text[i] == '#':
        # #'x, #(...), #2A(...), #\c, #b101, #XFACE
        j = i + 1
        if j < n and text[j] == '\\':
            return scan_atom(text, i)
        if j < n and text[j] == "'":
            return scan_elem(text, j + 1)
        while j < n and text[j].isalnum(): j += 1
        if j < n and text[j] == '(':
            return scan_form(text, j)
        return scan_atom(text, i)
    if text[i] == '(':
        return scan_form(text, i)
    return scan_atom(text, i)

def split_elems(form_text):
    """form_text is '( ... )'. Return list of element source strings."""
    inner = form_text[1:-1]
    elems = []
    i, n = 0, len(inner)
    while i < n:
        c = inner[i]
        if c in ' \t\n\r':
            i += 1; continue
        if c == ';':
            while i < n and inner[i] != '\n': i += 1
            continue
        j = scan_elem(inner, i)
        elems.append(inner[i:j])
        i = j
    return elems

# ---------- split file into blocks (comments/blank lines vs forms) ----------
blocks = []  # (kind, text)
i, n = 0, len(src)
while i < n:
    c = src[i]
    if c == '(':
        j = scan_form(src, i)
        blocks.append(('form', src[i:j]))
        i = j
    else:
        j = src.find('\n', i)
        if j < 0: j = n
        blocks.append(('line', src[i:j+1]))
        i = j + 1

DEF_HEADS = ('defun', 'defmacro', 'defglobal', 'defconstant', 'defdynamic', 'defclass', 'defgeneric', 'defmethod')

def head_of(text):
    e = split_elems(text)
    return (e[0].lower() if e else ''), e

out = []
page = None
counters = {}
skipped = []
generated = 0
started = False
stop = False
for kind, text in blocks:
    if kind == 'line':
        if not started:
            if text.startswith(';;; ====='):
                started = True
            else:
                continue
        if 'カーネル固有の回帰テスト' in text:
            stop = True
        if stop:
            continue
        # ページ番号はテスト直前のコメント行の先頭(";; p.N" / ";; cf. p.N")か、
        # セクション見出し(";;; §... (p.N-M)")からだけ取る(コメント本文中の
        # "p.28の一般規則" のような言及は無視する)
        m = re.match(r';;\s*(?:cf\.\s*)?p\.(\d+)', text) or re.match(r';;;.*\(p\.(\d+)', text)
        if m:
            page = int(m.group(1))
        out.append(text)
        continue
    if not started or stop:
        continue
    head, elems = head_of(text)
    if head not in ('assert-equal', 'assert-float-close', 'assert-error', 'assert-output'):
        out.append(text + '\n')
        continue
    if page is None:
        raise SystemExit('no page for ' + text[:60])
    counters[page] = counters.get(page, 0) + 1
    name = 'p-%04d-%04d' % (page, counters[page])
    if head == 'assert-error':
        form = elems[1]
        rest = []
    elif head == 'assert-output':
        form = elems[2]
        rest = elems[3:]
    else:
        form = elems[2]
    fhead, _ = head_of(form) if form.startswith('(') else ('', None)
    if fhead in DEF_HEADS:
        out.append(';; [skip %s] defun 等の定義フォームの戻り値を受け取るテストのため JIT 版では省略'
                   '(定義自体は後続のテストのために実行する)\n' % name)
        out.append(form + '\n')
        skipped.append(name)
        continue
    out.append('(defun %s ()\n  %s)\n' % (name, form.replace('\n', '\n  ') if False else form))
    out.append('(assert-equal t (%%%%za-compiled-p (function %s)))\n' % name)
    if head == 'assert-equal' or head == 'assert-float-close':
        out.append('(%s %s (%s))\n' % (head, elems[1], name))
    elif head == 'assert-error':
        out.append('(assert-error (%s))\n' % name)
    else:
        out.append('(assert-output %s (%s)\n  %s)\n' % (elems[1], name, '\n  '.join(rest)))
    generated += 1

header = ''';; test/lisp/isiki_test_jit.lisp
;;
;; isiki_test.lisp(ISLisp 仕様 v23 の "Example:" 全件)の JIT 版。isiki_test.lisp の各
;; アサーションはトップレベルフォームとしてインタプリタで評価されるため、同じ式を
;; 引数なしの defun の本体に置いて JIT コンパイラ(za.c)にコンパイルさせ、
;;   (1) その関数が実際に JIT コンパイルされたこと((%%za-compiled-p (function p-xxxx-yyyy)))
;;   (2) 呼び出した結果が仕様通りであること
;; の2点を確認する。
;;
;; 関数名は p-xxxx-yyyy(xxxx: 仕様のページ番号 4桁0埋め、yyyy: そのページ内での順番
;; 4桁0埋め)。ページ番号は isiki_test.lisp の各テスト直前のコメント(p.N / cf. p.N)から
;; 取り、順番はスキップしたものも含めて isiki_test.lisp の出現順に振る。
;;
;; defun/defmacro/defglobal 等の定義フォームの戻り値を受け取るテストは JIT 版では
;; スキップする(";; [skip p-xxxx-yyyy]" のコメントを残し、定義そのものは後続のテストの
;; ために実行する)。定義フォームやストリームの準備などアサーション以外のトップレベル
;; フォームは isiki_test.lisp のまま実行する。
;;
;; このファイルは tools/gen_isiki_test_jit.py で isiki_test.lisp から生成する。手で直す
;; のではなく isiki_test.lisp を直して再生成すること。
;;
;; isiki_test.lisp と同じ boot で連続して load されるため、isiki_test.lisp が定義した
;; グローバル(x/today/*color*/str 等)は本ファイルの定義で上書きされる(同じ定義)。

'''
body = re.sub(r'\n{3,}', '\n\n', ''.join(out))
open('test/lisp/isiki_test_jit.lisp', 'w').write(header + body)
print('generated', generated, 'skipped', len(skipped), 'pages', len(counters))
