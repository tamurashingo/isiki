#ifndef _TYPES_H_
#define _TYPES_H_

typedef unsigned char UINT8;
typedef unsigned short CHAR16;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef short INT16;
typedef int INT32;
typedef long long INT64;


/** Lispが管理するアドレス */
typedef UINT64 lisp_addr_t;
/** LispObjectのアドレス(要untag) */
typedef UINT64 lisp_val_t;

/** shadow stack(GC_PROTECTで保護中のCローカル変数)の1ノード。
 * Cスタック上(呼び出し元関数のローカル変数として)に確保され、
 * var_ptrが指すlisp_val_tをos_gc_collectがルートとしてスキャンする */
typedef struct _gc_rootnode {
    lisp_val_t *var_ptr;
    struct _gc_rootnode *next;
} gc_rootnode;

/** MS ABI(mingw)ターゲット向けにSysV ABIを強制する属性。ネイティブgcc(ISIKIOS_UNIT_TEST)では
 * SysV ABIがデフォルトのため属性が無意味になり-Wattributes警告が出るので、その場合は無にする */
#ifdef ISIKIOS_UNIT_TEST
#define SYSV_ABI
#else
#define SYSV_ABI __attribute__((sysv_abi))
#endif

#endif /* _TYPES_H_ */
