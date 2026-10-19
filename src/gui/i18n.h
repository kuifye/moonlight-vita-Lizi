#ifndef I18N_H
#define I18N_H

/*
 * i18n.h -- runtime language switch for the custom Moonlight build.
 *
 * config.language: 0 = English, 1 = 简体中文
 *
 * TR(en, zh) expands to a ternary on the global config, so call sites stay
 * readable and there is no lookup table to keep in sync:
 *
 *     MENU_ENTRY(ID, TR("Settings", "设置"));
 *
 * The Chinese strings need glyphs that nerdfont.ttf does not carry; see
 * draw_text_mixed() in guilib.c, which routes code points >= 0x2E80 to a
 * second font (cjk.ttf) so the Nerd Font icons in the English path keep
 * working.
 */

#include "../config.h"

#define TR(en, zh) (config.language ? (zh) : (en))

#endif /* I18N_H */
