/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zh_font.h - 最小 TrueType (sfnt) パーサ / ラスタライザ
 *
 * libfreetype に依存せず、整数演算だけでグリフのカバレッジ (8bit alpha)
 * を作る。テキスト整形や色の合成は呼び出し側 (fb-gradient.c) が行う。
 */
#ifndef ZHIBOX_ZH_FONT_H
#define ZHIBOX_ZH_FONT_H

#include <stddef.h>
#include <stdint.h>

struct zh_font {
	const uint8_t *data;
	uint32_t size;
	uint32_t units_per_em;
	int32_t ascent;
	int32_t descent;
	uint32_t num_glyphs;
	uint32_t num_hmetrics;
	uint32_t cmap_off;
	uint32_t cmap_len;
	int cmap_format;
	uint32_t glyf_off;
	uint32_t glyf_len;
	uint32_t loca_off;
	uint32_t loca_len;
	uint32_t hmtx_off;
	uint32_t hmtx_len;
	int loca_long;
};

/* 成功で 0、非対応 / 壊れている場合は -1 */
int zh_font_init(struct zh_font *font, const void *data, size_t size);

/* 見つからなければ 0 (glyph 0 は .notdef) */
uint32_t zh_font_glyph_index(const struct zh_font *font, uint32_t codepoint);

/* ピクセル単位の送り幅 / アセント */
int zh_font_advance(const struct zh_font *font, uint32_t glyph, int px);
int zh_font_ascent(const struct zh_font *font, int px);

/*
 * グリフを px サイズでラスタライズする。
 *   scratch    : bw*bh バイト以上の作業領域 (128*128 を推奨)
 *   bw, bh     : ビットマップ幅・高さ (空グリフは 0)
 *   bx, by     : ペン位置 (ベースライン) からビットマップ左上へのオフセット
 * 戻り値: 1 = 描画あり, 0 = 空, -1 = 引数不正 / 大きすぎる
 */
int zh_font_render(const struct zh_font *font, uint32_t glyph, int px,
		   uint8_t *scratch, size_t scratch_len,
		   int *bw, int *bh, int *bx, int *by);

#endif
