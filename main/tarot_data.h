/** 自动生成: tools/gen_tarot.py —— 大阿尔克纳像素画（30x40 RGB565） */
#pragma once

#include <stdint.h>

#define TAROT_ART_W 30
#define TAROT_ART_H 40
#define TAROT_COUNT 22

typedef struct {
    const char *name;      /* 牌名 */
    const char *upright;   /* 正位释义 */
    const char *reversed;  /* 逆位释义 */
    const uint16_t art[TAROT_ART_W * TAROT_ART_H];
} tarot_card_t;

extern const tarot_card_t tarot_cards[TAROT_COUNT];
extern const uint16_t tarot_card_back[TAROT_ART_W * TAROT_ART_H];
