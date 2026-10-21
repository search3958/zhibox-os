/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zh_png.h - 最小 PNG デコーダ (zlib inflate 内蔵)
 *
 * 8bit / 非インターレースの PNG (color type 0,2,3,4,6) を
 * RGBA8888 に展開する。libpng / zlib には依存しない。
 */
#ifndef ZHIBOX_ZH_PNG_H
#define ZHIBOX_ZH_PNG_H

#include <stddef.h>
#include <stdint.h>

typedef void *(*zh_png_alloc_fn)(size_t size);

/*
 * data/size : PNG ファイル全体
 * alloc     : 作業用メモリ確保関数 (mmap ラッパなど)
 * rgba      : 成功時に *rgba に RGBA8888 (width*height*4) を返す
 * 戻り値    : 0 = 成功, -1 = 失敗
 */
int zh_png_decode(const uint8_t *data, size_t size, zh_png_alloc_fn alloc,
		  uint8_t **rgba, int *width, int *height);

#endif
