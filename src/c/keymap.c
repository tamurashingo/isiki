#include "keymap.h"

/** Shift キーが押されている間は 1 になる */
static volatile UINT8 key_shift_pressed = 0;

/** Ctrl キーが押されている間は 1 になる。
 *
 * [#216] **これが今まで無かった。** 修飾キーの状態は Shift の 1 種類しか無く、
 * だから制御キーとの組み合わせ(C-a など)はキー割り当てとして作れなかった
 * (logic §9-4)。左右の Ctrl はどちらも 0x1D で届く(右は 0xE0 が前に付くが、
 * 前置きは Break コードと同じ上位ビットで捨てられるため区別しない)。 */
static volatile UINT8 key_ctrl_pressed = 0;

/** スキャンコード→ASCII文字の変換テーブル(Shift未押下時) */
static const char SCANCODE_NORMAL[128] = {
    [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5',
    [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0',
    [0x0C]='-', [0x0D]='=', [0x0E]='\b',
    [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
    [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1A]='[', [0x1B]=']', [0x1C]='\n',
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
    [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
    [0x27]=';', [0x28]='\'', [0x29]='`',
    [0x2B]='\\', [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v',
    [0x30]='b', [0x31]='n', [0x32]='m', [0x33]=',', [0x34]='.', [0x35]='/',
    [0x39]=' ',
};

/** スキャンコード→ASCII文字の変換テーブル(Shift押下時) */
static const char SCANCODE_SHIFT[128] = {
    [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
    [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
    [0x0C]='_', [0x0D]='+', [0x0E]='\b',
    [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T',
    [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O', [0x19]='P',
    [0x1A]='{', [0x1B]='}', [0x1C]='\n',
    [0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
    [0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L',
    [0x27]=':', [0x28]='"', [0x29]='~',
    [0x2B]='|', [0x2C]='Z', [0x2D]='X', [0x2E]='C', [0x2F]='V',
    [0x30]='B', [0x31]='N', [0x32]='M', [0x33]='<', [0x34]='>', [0x35]='?',
    [0x39]=' ',
};

/**
 * 変換表を引く**前に**番号で拾うキーの表。0 はここでは拾わないことを意味する。
 *
 * [#216] 矢印・ESC・Tab・Delete などは変換表(50 個)に載っていない。
 * **前置きが届いていないのが理由ではない**(logic §9-2)ので、
 * ここで番号を見るだけで取れる。Shift+PgUp/PgDn は**これより先**に
 * さかのぼり表示として判定されるため、ここの PgUp/PgDn は
 * 「Shift を押していないとき」だけ届く。
 */
static const UINT8 SCANCODE_SPECIAL[128] = {
    [0x01]=KEY_CODE_ESCAPE,
    [0x0F]=KEY_CODE_TAB,
    [0x47]=KEY_CODE_HOME,
    [0x48]=KEY_CODE_UP,
    [0x49]=KEY_CODE_PAGE_UP,
    [0x4B]=KEY_CODE_LEFT,
    [0x4D]=KEY_CODE_RIGHT,
    [0x4F]=KEY_CODE_END,
    [0x50]=KEY_CODE_DOWN,
    [0x51]=KEY_CODE_PAGE_DOWN,
    [0x52]=KEY_CODE_INSERT,
    [0x53]=KEY_CODE_DELETE,
};

void os_keymap_reset(void) {
    key_shift_pressed = 0;
    key_ctrl_pressed = 0;
}

int os_keymap_shift_pressed(void) {
    return key_shift_pressed != 0;
}

int os_keymap_ctrl_pressed(void) {
    return key_ctrl_pressed != 0;
}

/** 何もしない判定を 1 か所で作る(戻り値の形を書き間違えないため) */
static key_action_t action(key_action_kind_t kind, UINT32 value) {
    key_action_t a;
    a.kind = kind;
    a.value = value;
    return a;
}

key_action_t os_keymap_decode(UINT8 scancode) {
    /* 修飾キーの Make/Break。**Break コードの破棄より先に見る**
       (0xAA/0xB6/0x9D は上位ビットが立っているため) */
    if (scancode == 0x2A || scancode == 0x36) {
        key_shift_pressed = 1;
        return action(KEY_ACTION_MODIFIER, 0);
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        key_shift_pressed = 0;
        return action(KEY_ACTION_MODIFIER, 0);
    }
    if (scancode == 0x1D) {
        key_ctrl_pressed = 1;
        return action(KEY_ACTION_MODIFIER, 0);
    }
    if (scancode == 0x9D) {
        key_ctrl_pressed = 0;
        return action(KEY_ACTION_MODIFIER, 0);
    }
    if (scancode & 0x80) {
        /* その他の Break コードは無視する。**拡張キーの前置き 0xE0 もここで消える。**
           これは #216 でも変えていない(logic §9-2) */
        return action(KEY_ACTION_NONE, 0);
    }

    if (scancode >= 0x3B && scancode <= 0x3E) {
        /* F1〜F4: 表示フォーカスを移す。
           **この判定を下の「任意のキーで最下部へ復帰」より先に置くこと。** 逆にすると
           切り替えのたびに遡り位置がリセットされ、バッファごとの保持と矛盾する
           (もとの interrupt.c のコメントをそのまま引き継いでいる) */
        return action(KEY_ACTION_SWITCH_PROCESS, (UINT32)(scancode - 0x3B));
    }

    /* Shift+PageUp / Shift+PageDown: さかのぼり表示。
       **変換表より前で番号を拾う前例がこれである**(#216 の矢印も同じ形で足した)。
       拡張キーの 0xE0 は上で破棄済みなので、テンキーとナビゲーションキーの
       PgUp/PgDn は区別されない(この用途では問題にならない) */
    if (key_shift_pressed && (scancode == 0x49 || scancode == 0x51)) {
        return action(KEY_ACTION_SCROLLBACK, scancode == 0x49 ? 1 : 0);
    }

    /* [#216] 変換表に載っていないキーを番号で拾う。**ここが新しい分岐である。**
       Shift+PgUp/PgDn の後ろに置いてあるので、さかのぼりの前例を壊さない */
    if (SCANCODE_SPECIAL[scancode] != 0) {
        return action(KEY_ACTION_CHAR, SCANCODE_SPECIAL[scancode]);
    }

    char c = key_shift_pressed ? SCANCODE_SHIFT[scancode] : SCANCODE_NORMAL[scancode];
    if (c == 0) {
        return action(KEY_ACTION_NONE, 0);
    }

    /* [#216] Ctrl + 英字を制御文字(1〜26)にする。
       **Shift の表ではなく素の表の文字から作る**ので、C-A と C-a は同じになる。
       これが無いと「制御キーとの組み合わせ」はキー割り当てとして作れない
       (logic §9-4)。英字以外との組み合わせは素の文字をそのまま通す
       (ASCII の対応が一意でないものを勝手に決めない) */
    if (key_ctrl_pressed) {
        char base = SCANCODE_NORMAL[scancode];
        if (base >= 'a' && base <= 'z') {
            return action(KEY_ACTION_CHAR, (UINT32)(base - 'a' + 1));
        }
    }

    return action(KEY_ACTION_CHAR, (UINT32)(UINT8)c);
}
