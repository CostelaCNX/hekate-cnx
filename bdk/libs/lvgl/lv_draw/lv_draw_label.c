/**
 * @file lv_draw_label.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_draw_label.h"
#include "lv_draw_line.h"
#include "lv_draw_rbasic.h"
#include "../lv_misc/lv_math.h"

/*********************
 *      DEFINES
 *********************/
#define LABEL_RECOLOR_PAR_LENGTH    6

/**********************
 *      TYPEDEFS
 **********************/
enum {
    CMD_STATE_WAIT,
    CMD_STATE_PAR,
    CMD_STATE_IN,
};
typedef uint8_t cmd_state_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static uint8_t hex_char_to_num(char hex);
static uint8_t ptbr_accent_type(uint32_t letter);
static uint32_t ptbr_base_letter(uint32_t letter);
static void ptbr_draw_accent(const lv_point_t * pos, const lv_area_t * mask, uint8_t letter_w,
                             uint8_t letter_h, uint8_t accent, lv_color_t color, lv_opa_t opa);

/**********************
 *  STATIC VARIABLES
 **********************/


/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Write a text
 * @param coords coordinates of the label
 * @param mask the label will be drawn only in this area
 * @param style pointer to a style
 * @param opa_scale scale down all opacities by the factor
 * @param txt 0 terminated text to write
 * @param flag settings for the text from 'txt_flag_t' enum
 * @param offset text offset in x and y direction (NULL if unused)
 *
 */
void lv_draw_label(const lv_area_t * coords, const lv_area_t * mask, const lv_style_t * style, lv_opa_t opa_scale,
                   const char * txt, lv_txt_flag_t flag, lv_point_t * offset)
{
    const lv_font_t * font = style->text.font;
    lv_coord_t w;
    if((flag & LV_TXT_FLAG_EXPAND) == 0) {
        /*Normally use the label's width as width*/
        w = lv_area_get_width(coords);
    } else {
        /*If EXAPND is enabled then not limit the text's width to the object's width*/
        lv_point_t p;
        lv_txt_get_size(&p, txt, style->text.font, style->text.letter_space, style->text.line_space, LV_COORD_MAX, flag);
        w = p.x;
    }

    lv_coord_t line_height = lv_font_get_height(font) + style->text.line_space;


    /*Init variables for the first line*/
    lv_coord_t line_width = 0;
    lv_point_t pos;
    pos.x = coords->x1;
    pos.y = coords->y1;

    lv_coord_t x_ofs = 0;
    lv_coord_t y_ofs = 0;
    if(offset != NULL) {
        x_ofs = offset->x;
        y_ofs = offset->y;
        pos.y += y_ofs;
    }

    uint32_t line_start = 0;
    uint32_t line_end = lv_txt_get_next_line(txt, font, style->text.letter_space, w, flag);

    /*Go the first visible line*/
    while(pos.y + line_height < mask->y1) {
        /*Go to next line*/
        line_start = line_end;
        line_end += lv_txt_get_next_line(&txt[line_start], font, style->text.letter_space, w, flag);
        pos.y += line_height;

        if(txt[line_start] == '\0') return;
    }

    /*Align to middle*/
    if(flag & LV_TXT_FLAG_CENTER) {
        line_width = lv_txt_get_width(&txt[line_start], line_end - line_start,
                                      font, style->text.letter_space, flag);

        pos.x += (lv_area_get_width(coords) - line_width) / 2;

    }
    /*Align to the right*/
    else if(flag & LV_TXT_FLAG_RIGHT) {
        line_width = lv_txt_get_width(&txt[line_start], line_end - line_start,
                                      font, style->text.letter_space, flag);
        pos.x += lv_area_get_width(coords) - line_width;
    }


    lv_opa_t opa = opa_scale == LV_OPA_COVER ? style->text.opa : (uint16_t)((uint16_t) style->text.opa * opa_scale) >> 8;

    cmd_state_t cmd_state = CMD_STATE_WAIT;
    uint32_t i;
    uint16_t par_start = 0;
    lv_color_t recolor;
    lv_coord_t letter_w;

    /*Real draw need a background color for higher bpp letter*/
#if LV_VDB_SIZE == 0
    lv_rletter_set_background(style->body.main_color);
#endif


    /*Write out all lines*/
    while(txt[line_start] != '\0') {
        if(offset != NULL) {
            pos.x += x_ofs;
        }
        /*Write all letter of a line*/
        cmd_state = CMD_STATE_WAIT;
        i = line_start;
        uint32_t letter;
        while(i < line_end) {
            letter = lv_txt_encoded_next(txt, &i);

            /*Handle the re-color command*/
            if((flag & LV_TXT_FLAG_RECOLOR) != 0) {
                if(letter == (uint32_t)LV_TXT_COLOR_CMD[0]) {
                    if(cmd_state == CMD_STATE_WAIT) { /*Start char*/
                        par_start = i;
                        cmd_state = CMD_STATE_PAR;
                        continue;
                    } else if(cmd_state == CMD_STATE_PAR) { /*Other start char in parameter escaped cmd. char */
                        cmd_state = CMD_STATE_WAIT;
                    } else if(cmd_state == CMD_STATE_IN) { /*Command end */
                        cmd_state = CMD_STATE_WAIT;
                        continue;
                    }
                }

                /*Skip the color parameter and wait the space after it*/
                if(cmd_state == CMD_STATE_PAR) {
                    if(letter == ' ') {
                        /*Get the parameter*/
                        if(i - par_start == LABEL_RECOLOR_PAR_LENGTH + 1) {
                            char buf[LABEL_RECOLOR_PAR_LENGTH + 1];
                            memcpy(buf, &txt[par_start], LABEL_RECOLOR_PAR_LENGTH);
                            buf[LABEL_RECOLOR_PAR_LENGTH] = '\0';
                            int r, g, b;
                            r = (hex_char_to_num(buf[0]) << 4) + hex_char_to_num(buf[1]);
                            g = (hex_char_to_num(buf[2]) << 4) + hex_char_to_num(buf[3]);
                            b = (hex_char_to_num(buf[4]) << 4) + hex_char_to_num(buf[5]);
                            recolor = LV_COLOR_MAKE(r, g, b);
                        } else {
                            recolor.full = style->text.color.full;
                        }
                        cmd_state = CMD_STATE_IN; /*After the parameter the text is in the command*/
                    }
                    continue;
                }
            }

            lv_color_t color = style->text.color;

            if(cmd_state == CMD_STATE_IN) color = recolor;

            uint32_t letter_draw = ptbr_base_letter(letter);
            uint8_t accent = ptbr_accent_type(letter);

            letter_fp(&pos, mask, font, letter_draw, color, opa);
            letter_w = lv_font_get_width(font, letter_draw);
            if(accent) {
                ptbr_draw_accent(&pos, mask, letter_w, lv_font_get_height(font), accent, color, opa);
            }

            if(letter_w > 0){
                pos.x += letter_w + style->text.letter_space;
            }
        }
        /*Go to next line*/
        line_start = line_end;
        line_end += lv_txt_get_next_line(&txt[line_start], font, style->text.letter_space, w, flag);

        pos.x = coords->x1;
        /*Align to middle*/
        if(flag & LV_TXT_FLAG_CENTER) {
            line_width = lv_txt_get_width(&txt[line_start], line_end - line_start,
                                          font, style->text.letter_space, flag);

            pos.x += (lv_area_get_width(coords) - line_width) / 2;

        }
        /*Align to the right*/
        else if(flag & LV_TXT_FLAG_RIGHT) {
            line_width = lv_txt_get_width(&txt[line_start], line_end - line_start,
                                          font, style->text.letter_space, flag);
            pos.x += lv_area_get_width(coords) - line_width;
        }

        /*Go the next line position*/
        pos.y += line_height;

        if(pos.y > mask->y2) return;
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Convert a hexadecimal characters to a number (0..15)
 * @param hex Pointer to a hexadecimal character (0..9, A..F)
 * @return the numerical value of `hex` or 0 on error
 */
static uint8_t hex_char_to_num(char hex)
{
    uint8_t result = 0;

    if(hex >= '0' && hex <= '9') {
        result = hex - '0';
    }
    else {
        if(hex >= 'a') hex -= 'a' - 'A';    /*Convert to upper case*/

        switch(hex) {
            case 'A':
                result = 10;
                break;
            case 'B':
                result = 11;
                break;
            case 'C':
                result = 12;
                break;
            case 'D':
                result = 13;
                break;
            case 'E':
                result = 14;
                break;
            case 'F':
                result = 15;
                break;
            default:
                result = 0;
                break;
        }
    }

    return result;
}

static uint8_t ptbr_accent_type(uint32_t letter)
{
    switch(letter) {
        case 0x00C1: case 0x00C9: case 0x00CD: case 0x00D3: case 0x00DA:
        case 0x00E1: case 0x00E9: case 0x00ED: case 0x00F3: case 0x00FA:
            return 1; /* acute */
        case 0x00C0: case 0x00E0:
            return 2; /* grave */
        case 0x00C2: case 0x00CA: case 0x00D4:
        case 0x00E2: case 0x00EA: case 0x00F4:
            return 3; /* circumflex */
        case 0x00C3: case 0x00D5: case 0x00E3: case 0x00F5:
            return 4; /* tilde */
        case 0x00DC: case 0x00FC:
            return 5; /* diaeresis */
        case 0x00C7: case 0x00E7:
            return 6; /* cedilla */
        default:
            return 0;
    }
}

static uint32_t ptbr_base_letter(uint32_t letter)
{
    switch(letter) {
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3:
            return 'A';
        case 0x00C7:
            return 'C';
        case 0x00C9: case 0x00CA:
            return 'E';
        case 0x00CD:
            return 'I';
        case 0x00D3: case 0x00D4: case 0x00D5:
            return 'O';
        case 0x00DA: case 0x00DC:
            return 'U';
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3:
            return 'a';
        case 0x00E7:
            return 'c';
        case 0x00E9: case 0x00EA:
            return 'e';
        case 0x00ED:
            return 'i';
        case 0x00F3: case 0x00F4: case 0x00F5:
            return 'o';
        case 0x00FA: case 0x00FC:
            return 'u';
        default:
            return letter;
    }
}

static void ptbr_draw_line(lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2,
                           const lv_area_t * mask, lv_style_t * style, lv_opa_t opa)
{
    lv_point_t p1 = { .x = x1, .y = y1 };
    lv_point_t p2 = { .x = x2, .y = y2 };
    lv_draw_line(&p1, &p2, mask, style, opa);
}

static void ptbr_draw_accent(const lv_point_t * pos, const lv_area_t * mask, uint8_t letter_w,
                             uint8_t letter_h, uint8_t accent, lv_color_t color, lv_opa_t opa)
{
    if(letter_w < 3) return;

    lv_style_t style;
    lv_style_copy(&style, &lv_style_plain);
    style.line.color = color;
    style.line.opa = LV_OPA_COVER;
    style.line.width = letter_h >= 24 ? 2 : 1;
    style.line.rounded = 0;

    lv_coord_t cx = pos->x + (letter_w / 2);
    lv_coord_t top = pos->y + (letter_h >= 24 ? 1 : 0);
    lv_coord_t span = letter_w >= 10 ? 4 : 3;

    switch(accent) {
        case 1:
            ptbr_draw_line(cx - 1, top + 3, cx + span / 2, top, mask, &style, opa);
            break;
        case 2:
            ptbr_draw_line(cx - span / 2, top, cx + 1, top + 3, mask, &style, opa);
            break;
        case 3:
            ptbr_draw_line(cx - span, top + 3, cx, top, mask, &style, opa);
            ptbr_draw_line(cx, top, cx + span, top + 3, mask, &style, opa);
            break;
        case 4:
            style.line.width = 1;
            span = letter_w >= 12 ? 5 : 4;
            ptbr_draw_line(cx - span, top + 2, cx - span / 2, top, mask, &style, opa);
            ptbr_draw_line(cx - span / 2, top, cx + span / 2, top + 2, mask, &style, opa);
            ptbr_draw_line(cx + span / 2, top + 2, cx + span, top, mask, &style, opa);
            break;
        case 5:
            ptbr_draw_line(cx - span / 2, top + 1, cx - span / 2, top + 2, mask, &style, opa);
            ptbr_draw_line(cx + span / 2, top + 1, cx + span / 2, top + 2, mask, &style, opa);
            break;
        case 6:
            ptbr_draw_line(cx - 1, pos->y + letter_h - 3, cx + 1, pos->y + letter_h - 1, mask, &style, opa);
            ptbr_draw_line(cx + 1, pos->y + letter_h - 1, cx - 1, pos->y + letter_h + 1, mask, &style, opa);
            break;
    }
}
