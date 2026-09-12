#!/usr/bin/env python3
"""[GC監査] 例外ダンプのアドレスをPE上の関数名へ逆引きする(tools/bench/locate_rip.sh から使う)。

塗り潰し(ISIKIOS_GC_PAINT)のトラップパターン 0xDEADDEADDEADDEA6 は上位16bitが
0xDEADでcanonicalでないため、デリファレンスするとGP例外(vector=13, error_code=0)
になる。つまりGPの瞬間にトラップがレジスタへ載っていれば、その例外はstale
ポインタの参照そのものである。ただしripが指すのは**読んだ側**(cc_car/cc_cdr等)
であって、保護を怠った呼び出し元ではない。呼び出し元はスタック上の戻りアドレス
として現れるので、スタックダンプの各語も同じ方法で逆引きする。
"""
import re
import sys
import bisect


def load_symbols(dis_path):
    """objdump -d の出力から (開始アドレス, 関数名) の表を作る"""
    syms = []
    pat = re.compile(r'^([0-9a-f]+) <(.+)>:$')
    with open(dis_path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            m = pat.match(ln.rstrip('\n'))
            if m:
                syms.append((int(m.group(1), 16), m.group(2)))
    syms.sort()
    return syms


def lookup(syms, addr):
    keys = [a for a, _ in syms]
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return None
    base, name = syms[i]
    return name, addr - base


def main():
    dis_path = sys.argv[1]
    syms = load_symbols(dis_path)

    # 基準シンボル。例外ダンプは c_cpu_exception_handler を、ゲスト内の
    # %%DIAG-IMAGE-ANCHOR は os_gc_debug_trap_read をアンカーとして報告する
    anchor_name = 'c_cpu_exception_handler'
    if '--anchor-symbol' in sys.argv:
        i = sys.argv.index('--anchor-symbol')
        anchor_name = sys.argv[i + 1]
        del sys.argv[i:i + 2]

    handler_file_addr = None
    for a, n in syms:
        if n == anchor_name:
            handler_file_addr = a
            break
    if handler_file_addr is None:
        sys.exit(f'{anchor_name}が逆アセンブルに見つかりません')

    lo = syms[0][0]
    hi = syms[-1][0] + 0x10000

    if sys.argv[2] == '--serial':
        text = open(sys.argv[3], encoding='utf-8', errors='replace').read()
        m = re.search(r'handler=0x([0-9A-Fa-f]+)', text)
        if not m:
            sys.exit('serialにhandler=の行がありません(古いビルドのダンプ)')
        handler_run = int(m.group(1), 16)
        addrs = []
        rip = re.search(r'rip=0x([0-9A-Fa-f]+)', text)
        if rip:
            addrs.append(('rip', int(rip.group(1), 16)))
        for name, val in re.findall(r'\b(r\d+|r[a-z]{2})=0x([0-9A-Fa-f]+)', text):
            addrs.append((name, int(val, 16)))
        stack = text.split('stack(rsp..):')
        if len(stack) > 1:
            for i, v in enumerate(re.findall(r'0x([0-9A-Fa-f]{16})', stack[1])):
                addrs.append((f'stack[{i}]', int(v, 16)))
    else:
        handler_run = int(sys.argv[3], 16) if sys.argv[2] == '--handler' else None
        handler_run = int(sys.argv[3], 0)
        addrs = [('arg', int(a, 0)) for a in sys.argv[4:]]

    print(f'handler(実行時)=0x{handler_run:x}  handler(ファイル上)=0x{handler_file_addr:x}')
    print()
    seen = set()
    for label, addr in addrs:
        if (addr & ~0x7) == (0xDEADDEADDEADDEA6 & ~0x7):
            print(f'{label:<12} 0x{addr:016x}  <-- 塗り潰しのトラップパターン(stale)')
            continue
        file_addr = handler_file_addr + (addr - handler_run)
        if not (lo <= file_addr < hi):
            continue
        r = lookup(syms, file_addr)
        if r is None:
            continue
        name, off = r
        key = (name, off)
        if key in seen:
            continue
        seen.add(key)
        print(f'{label:<12} 0x{addr:016x}  {name}+0x{off:x}')


if __name__ == '__main__':
    main()
