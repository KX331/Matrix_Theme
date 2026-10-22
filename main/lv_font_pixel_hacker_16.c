/*******************************************************************************
 * Size: 16 px
 * Bpp: 1
 * Opts: --bpp 1 --size 16 --no-compress --stride 1 --align 1 --font ChillBitmap7x.ttf --symbols 0x4E00-0x9FA5 --range 32-126 --format lvgl -o lv_font_pixel_hacker_16.c
 ******************************************************************************/

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif



#ifndef LV_FONT_PIXEL_HACKER_16
#define LV_FONT_PIXEL_HACKER_16 1
#endif

#if LV_FONT_PIXEL_HACKER_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0xf0, 0xf0,

    /* U+0022 "\"" */
    0xcf, 0x3c, 0xf3,

    /* U+0023 "#" */
    0x33, 0xc, 0xcf, 0xff, 0xff, 0x33, 0xc, 0xcf,
    0xff, 0xff, 0x33, 0xc, 0xc0,

    /* U+0024 "$" */
    0xc, 0x3, 0x3, 0xf0, 0xfc, 0xcc, 0x33, 0x3,
    0xf0, 0xfc, 0xc, 0xc3, 0x3f, 0xf3, 0xfc, 0xc,
    0x3, 0x0,

    /* U+0025 "%" */
    0x30, 0x30, 0xc0, 0xcc, 0xcc, 0x33, 0x30, 0x33,
    0x0, 0xcc, 0x0, 0x33, 0x0, 0xcc, 0xc, 0xcc,
    0x33, 0x33, 0x3, 0xc, 0xc,

    /* U+0026 "&" */
    0x3c, 0x3c, 0xc0, 0xc0, 0x33, 0x33, 0xcc, 0xcc,
    0xf3, 0xf3,

    /* U+0027 "'" */
    0x33, 0xcc,

    /* U+0028 "(" */
    0x33, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x33,

    /* U+0029 ")" */
    0xcc, 0x33, 0x33, 0x33, 0x33, 0x33, 0xcc,

    /* U+002A "*" */
    0x30, 0xcf, 0xff, 0x30, 0xcc, 0xf3,

    /* U+002B "+" */
    0x30, 0xcf, 0xff, 0x30, 0xc0,

    /* U+002C "," */
    0x33, 0xcc,

    /* U+002D "-" */
    0xff, 0xf0,

    /* U+002E "." */
    0xf0,

    /* U+002F "/" */
    0xc, 0x30, 0xc3, 0x30, 0xc3, 0xc, 0xc3, 0xc,
    0x30,

    /* U+0030 "0" */
    0x3c, 0xfc, 0xf3, 0xcf, 0x3c, 0xf3, 0xf3, 0xc0,

    /* U+0031 "1" */
    0x33, 0xff, 0x33, 0x33, 0x33,

    /* U+0032 "2" */
    0xf3, 0xc0, 0xc3, 0x30, 0xcc, 0x30, 0xff, 0xf0,

    /* U+0033 "3" */
    0xf3, 0xc0, 0xc3, 0x30, 0xc0, 0xc3, 0xf3, 0xc0,

    /* U+0034 "4" */
    0x30, 0xcc, 0x30, 0xcf, 0x3f, 0xff, 0xc, 0x30,

    /* U+0035 "5" */
    0xff, 0xfc, 0x30, 0xff, 0xf0, 0xc3, 0xf3, 0xc0,

    /* U+0036 "6" */
    0x30, 0xcc, 0x30, 0xff, 0xfc, 0xf3, 0xf3, 0xc0,

    /* U+0037 "7" */
    0xff, 0xf0, 0xc3, 0x30, 0xc3, 0xc, 0xc3, 0x0,

    /* U+0038 "8" */
    0x3c, 0xfc, 0xf3, 0x30, 0xcc, 0xf3, 0xf3, 0xc0,

    /* U+0039 "9" */
    0x3c, 0xfc, 0xf3, 0xff, 0xf0, 0xc3, 0x30, 0xc0,

    /* U+003A ":" */
    0xf0, 0xf,

    /* U+003B ";" */
    0x33, 0x0, 0x0, 0x33, 0xcc,

    /* U+003C "<" */
    0xc, 0x33, 0xc, 0xc3, 0x3, 0xc, 0xc, 0x30,

    /* U+003D "=" */
    0xff, 0xf0, 0x0, 0xff, 0xf0,

    /* U+003E ">" */
    0xc3, 0x3, 0xc, 0xc, 0x33, 0xc, 0xc3, 0x0,

    /* U+003F "?" */
    0x30, 0xcc, 0xf3, 0xc, 0x33, 0xc, 0x0, 0x3,
    0xc,

    /* U+0040 "@" */
    0x3f, 0xf, 0xcc, 0xcf, 0x33, 0xcf, 0xf3, 0xfc,
    0x3, 0x0, 0x3f, 0xcf, 0xf0,

    /* U+0041 "A" */
    0xc, 0xc, 0x33, 0x33, 0xc3, 0xc3, 0xff, 0xff,
    0xc3, 0xc3,

    /* U+0042 "B" */
    0xfc, 0xfc, 0xc3, 0xc3, 0xfc, 0xfc, 0xc3, 0xc3,
    0xfc, 0xfc,

    /* U+0043 "C" */
    0x3c, 0x3c, 0xc3, 0xc3, 0xc0, 0xc0, 0xc3, 0xc3,
    0x3c, 0x3c,

    /* U+0044 "D" */
    0xfc, 0xfc, 0xc3, 0xc3, 0xc3, 0xc3, 0xcc, 0xcc,
    0xf0, 0xf0,

    /* U+0045 "E" */
    0xff, 0xfc, 0x30, 0xff, 0xfc, 0x30, 0xff, 0xf0,

    /* U+0046 "F" */
    0xff, 0xfc, 0x30, 0xff, 0xfc, 0x30, 0xc3, 0x0,

    /* U+0047 "G" */
    0x3f, 0x3f, 0xc0, 0xc0, 0xcf, 0xcf, 0xc3, 0xc3,
    0x3c, 0x3c,

    /* U+0048 "H" */
    0xc3, 0xc3, 0xc3, 0xc3, 0xff, 0xff, 0xc3, 0xc3,
    0xc3, 0xc3,

    /* U+0049 "I" */
    0xff, 0xff, 0xf0,

    /* U+004A "J" */
    0x3f, 0x3f, 0xc, 0xc, 0xc, 0xc, 0xcc, 0xcc,
    0x30, 0x30,

    /* U+004B "K" */
    0xc3, 0xc3, 0xcc, 0xcc, 0xf0, 0xf0, 0xcc, 0xcc,
    0xc3, 0xc3,

    /* U+004C "L" */
    0xc3, 0xc, 0x30, 0xc3, 0xc, 0x30, 0xff, 0xf0,

    /* U+004D "M" */
    0xc0, 0xf0, 0x3f, 0x3f, 0xcf, 0xcc, 0xf3, 0x3c,
    0xcf, 0x33, 0xc0, 0xf0, 0x30,

    /* U+004E "N" */
    0xc3, 0xc3, 0xf3, 0xf3, 0xcf, 0xcf, 0xc3, 0xc3,
    0xc3, 0xc3,

    /* U+004F "O" */
    0x3c, 0x3c, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3,
    0x3c, 0x3c,

    /* U+0050 "P" */
    0xfc, 0xfc, 0xc3, 0xc3, 0xff, 0xff, 0xc0, 0xc0,
    0xc0, 0xc0,

    /* U+0051 "Q" */
    0x3c, 0x3c, 0xc3, 0xc3, 0xf3, 0xf3, 0xcc, 0xcc,
    0x33, 0x33,

    /* U+0052 "R" */
    0xfc, 0xfc, 0xc3, 0xc3, 0xfc, 0xfc, 0xcc, 0xcc,
    0xc3, 0xc3,

    /* U+0053 "S" */
    0x3f, 0x3f, 0xc0, 0xc0, 0x3c, 0x3c, 0x3, 0x3,
    0xfc, 0xfc,

    /* U+0054 "T" */
    0xff, 0xf3, 0xc, 0x30, 0xc3, 0xc, 0x30, 0xc0,

    /* U+0055 "U" */
    0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3,
    0x3c, 0x3c,

    /* U+0056 "V" */
    0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xcc, 0xcc,
    0x30, 0x30,

    /* U+0057 "W" */
    0xc0, 0xf0, 0x3c, 0xcf, 0x33, 0xcc, 0xf3, 0x33,
    0x30, 0xcc, 0x33, 0xc, 0xc0,

    /* U+0058 "X" */
    0xc3, 0xc3, 0xc3, 0xc3, 0x3c, 0x3c, 0xc3, 0xc3,
    0xc3, 0xc3,

    /* U+0059 "Y" */
    0xc3, 0xc3, 0xc3, 0xc3, 0x3c, 0x3c, 0xc, 0xc,
    0xf0, 0xf0,

    /* U+005A "Z" */
    0xff, 0xf0, 0xc3, 0x30, 0xcc, 0x30, 0xff, 0xf0,

    /* U+005B "[" */
    0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xff,

    /* U+005C "\\" */
    0xc3, 0xc, 0x30, 0x30, 0xc3, 0xc, 0xc, 0x30,
    0xc3,

    /* U+005D "]" */
    0xff, 0x33, 0x33, 0x33, 0x33, 0x33, 0xff,

    /* U+005E "^" */
    0x30, 0xcc, 0xf3,

    /* U+005F "_" */
    0xff, 0xf0,

    /* U+0060 "`" */
    0xcc, 0x33,

    /* U+0061 "a" */
    0x3c, 0xfc, 0xf3, 0xcf, 0x33, 0xcf,

    /* U+0062 "b" */
    0xc3, 0xc, 0x30, 0xf3, 0xcc, 0xf3, 0xcf, 0x3f,
    0x3c,

    /* U+0063 "c" */
    0x3c, 0xfc, 0x30, 0xc3, 0x3, 0xcf,

    /* U+0064 "d" */
    0xc, 0x30, 0xc3, 0x3c, 0xfc, 0xf3, 0xcf, 0x33,
    0xcf,

    /* U+0065 "e" */
    0x3c, 0xfc, 0xf3, 0xf3, 0xc3, 0xcf,

    /* U+0066 "f" */
    0x33, 0xcc, 0xff, 0xcc, 0xcc, 0xcc,

    /* U+0067 "g" */
    0x3c, 0xfc, 0xf3, 0xff, 0xf0, 0xc3, 0xf3, 0xc0,

    /* U+0068 "h" */
    0xc3, 0xc, 0x30, 0xf3, 0xcc, 0xf3, 0xcf, 0x3c,
    0xf3,

    /* U+0069 "i" */
    0xf0, 0xff, 0xff,

    /* U+006A "j" */
    0x33, 0x0, 0x33, 0x33, 0x33, 0x33, 0xcc,

    /* U+006B "k" */
    0xc3, 0xc, 0x30, 0xcf, 0x3f, 0x3c, 0xcf, 0x3c,
    0xf3,

    /* U+006C "l" */
    0xff, 0xff, 0xff,

    /* U+006D "m" */
    0xf3, 0x3c, 0xcc, 0xcf, 0x33, 0xcc, 0xf3, 0x3c,
    0xf, 0x3,

    /* U+006E "n" */
    0xf3, 0xcc, 0xf3, 0xcf, 0x3c, 0xf3,

    /* U+006F "o" */
    0x30, 0xcc, 0xf3, 0xcf, 0x33, 0xc,

    /* U+0070 "p" */
    0xf3, 0xcc, 0xf3, 0xcf, 0x3f, 0x3c, 0xc3, 0x0,

    /* U+0071 "q" */
    0x3c, 0xfc, 0xf3, 0xcf, 0x33, 0xcf, 0xc, 0x30,

    /* U+0072 "r" */
    0x33, 0xcc, 0xcc, 0xcc,

    /* U+0073 "s" */
    0x3c, 0xff, 0x3c, 0x3c, 0xff, 0x3c,

    /* U+0074 "t" */
    0xcc, 0xff, 0xcc, 0xcc, 0x33,

    /* U+0075 "u" */
    0xcf, 0x3c, 0xf3, 0xcf, 0x33, 0xcf,

    /* U+0076 "v" */
    0xcf, 0x3c, 0xf3, 0xf3, 0xcc, 0x30,

    /* U+0077 "w" */
    0xc0, 0xf0, 0x3c, 0xcf, 0x33, 0xff, 0x3f, 0xcc,
    0xc3, 0x30,

    /* U+0078 "x" */
    0xcf, 0x33, 0xc, 0xcf, 0x3c, 0xf3,

    /* U+0079 "y" */
    0xcf, 0x3c, 0xf3, 0x3c, 0xf0, 0xc3, 0xf3, 0xc0,

    /* U+007A "z" */
    0xff, 0xf0, 0xc3, 0x30, 0xcf, 0xff,

    /* U+007B "{" */
    0xc, 0x33, 0xc, 0x30, 0xcc, 0x30, 0x30, 0xc3,
    0xc, 0xc, 0x30,

    /* U+007C "|" */
    0xff, 0xff, 0xff, 0xf0,

    /* U+007D "}" */
    0xc3, 0x3, 0xc, 0x30, 0xc0, 0xc3, 0x30, 0xc3,
    0xc, 0xc3, 0x0,

    /* U+007E "~" */
    0x33, 0x33, 0xcc, 0xcc
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 96, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 128, .box_w = 2, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 4, .adv_w = 128, .box_w = 6, .box_h = 4, .ofs_x = 0, .ofs_y = 8},
    {.bitmap_index = 7, .adv_w = 192, .box_w = 10, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 20, .adv_w = 192, .box_w = 10, .box_h = 14, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 38, .adv_w = 256, .box_w = 14, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 59, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 69, .adv_w = 96, .box_w = 4, .box_h = 4, .ofs_x = 0, .ofs_y = 8},
    {.bitmap_index = 71, .adv_w = 128, .box_w = 4, .box_h = 14, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 78, .adv_w = 128, .box_w = 4, .box_h = 14, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 85, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 91, .adv_w = 128, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 96, .adv_w = 96, .box_w = 4, .box_h = 4, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 98, .adv_w = 128, .box_w = 6, .box_h = 2, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 100, .adv_w = 64, .box_w = 2, .box_h = 2, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 101, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 110, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 118, .adv_w = 96, .box_w = 4, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 123, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 131, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 139, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 147, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 155, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 163, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 171, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 179, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 187, .adv_w = 128, .box_w = 2, .box_h = 8, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 189, .adv_w = 128, .box_w = 4, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 194, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 202, .adv_w = 128, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 207, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 215, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 224, .adv_w = 192, .box_w = 10, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 237, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 247, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 257, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 267, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 277, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 285, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 293, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 303, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 313, .adv_w = 64, .box_w = 2, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 316, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 326, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 336, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 344, .adv_w = 192, .box_w = 10, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 357, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 367, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 377, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 387, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 397, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 407, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 417, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 425, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 435, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 445, .adv_w = 192, .box_w = 10, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 458, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 468, .adv_w = 160, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 478, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 486, .adv_w = 128, .box_w = 4, .box_h = 14, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 493, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 502, .adv_w = 128, .box_w = 4, .box_h = 14, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 509, .adv_w = 128, .box_w = 6, .box_h = 4, .ofs_x = 0, .ofs_y = 8},
    {.bitmap_index = 512, .adv_w = 128, .box_w = 6, .box_h = 2, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 514, .adv_w = 128, .box_w = 4, .box_h = 4, .ofs_x = 2, .ofs_y = 8},
    {.bitmap_index = 516, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 522, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 531, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 537, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 546, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 552, .adv_w = 96, .box_w = 4, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 558, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 566, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 575, .adv_w = 64, .box_w = 2, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 578, .adv_w = 96, .box_w = 4, .box_h = 14, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 585, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 594, .adv_w = 64, .box_w = 2, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 597, .adv_w = 192, .box_w = 10, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 607, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 613, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 619, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 627, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 635, .adv_w = 96, .box_w = 4, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 639, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 645, .adv_w = 96, .box_w = 4, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 650, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 656, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 662, .adv_w = 192, .box_w = 10, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 672, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 678, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 686, .adv_w = 128, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 692, .adv_w = 128, .box_w = 6, .box_h = 14, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 703, .adv_w = 128, .box_w = 2, .box_h = 14, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 707, .adv_w = 128, .box_w = 6, .box_h = 14, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 718, .adv_w = 160, .box_w = 8, .box_h = 4, .ofs_x = 0, .ofs_y = 4}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Map glyph_ids to kern left classes*/
static const uint8_t kern_left_class_mapping[] =
{
    0, 1, 2, 2, 2, 0, 2, 2,
    2, 2, 2, 2, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 3, 3, 2, 3, 3,
    2, 2, 0, 0, 0, 4, 0, 5,
    0, 0, 0, 6, 0, 7, 0, 0,
    0, 0, 0, 0, 4, 8, 0, 0,
    0, 0, 0, 0, 2, 2, 2, 2,
    2, 2, 4, 9, 4, 0, 4, 0,
    4, 4, 0, 0, 4, 0, 4, 4,
    9, 9, 4, 9, 9, 4, 4, 9,
    9, 4, 4, 4, 2, 2, 2, 2
};

/*Map glyph_ids to kern right classes*/
static const uint8_t kern_right_class_mapping[] =
{
    0, 1, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 2, 2, 3, 2, 2,
    2, 2, 4, 0, 5, 0, 0, 0,
    4, 0, 0, 0, 0, 0, 0, 0,
    4, 0, 4, 0, 0, 6, 0, 0,
    7, 0, 0, 0, 2, 2, 2, 2,
    2, 2, 4, 0, 4, 4, 4, 0,
    4, 0, 0, 8, 0, 0, 5, 4,
    4, 5, 4, 4, 4, 0, 5, 5,
    5, 5, 5, 5, 2, 2, 2, 2
};

/*Kern values between classes*/
static const int8_t kern_class_values[] =
{
    0, -32, -32, 0, 0, -32, 0, -32,
    -32, 0, 0, 0, 0, 0, 0, 0,
    -32, 0, 0, 0, 0, -32, 0, 0,
    0, 0, 0, 0, 0, -32, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -32,
    0, 0, 0, -32, 0, 0, 0, -32,
    0, 0, 0, 0, 0, -32, -32, 0,
    0, 0, -32, -32, -32, 0, 0, -32,
    0, 0, 0, 0, 0, -32, 0, -32
};


/*Collect the kern class' data in one place*/
static const lv_font_fmt_txt_kern_classes_t kern_classes =
{
    .class_pair_values   = kern_class_values,
    .left_class_mapping  = kern_left_class_mapping,
    .right_class_mapping = kern_right_class_mapping,
    .left_class_cnt      = 9,
    .right_class_cnt     = 8,
};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = &kern_classes,
    .kern_scale = 16,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 1,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif

};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t lv_font_pixel_hacker_16 = {
#else
lv_font_t lv_font_pixel_hacker_16 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 14,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .static_bitmap = 0,
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if LV_FONT_PIXEL_HACKER_16*/
