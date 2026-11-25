/* TJpgDec configuration for the Cafe Stella reader.
 * Fixed at build time (no Kconfig dependency): RGB565, 512-byte input buffer,
 * output descaling enabled for thumbnails, table clipping for speed. */
#pragma once

#define JD_SZBUF        512
#define JD_FORMAT       1
#define JD_USE_SCALE    1
#define JD_TBLCLIP      1
#define JD_FASTDECODE   1
#define JD_DEFAULT_HUFFMAN 0
