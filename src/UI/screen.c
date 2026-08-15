#include <lvgl/lvgl.h>
#include "screen.h"

#define ALBUM_IMG_PATH_64 "A:src/UI/images/ab67616d000048515124ed45a94033830b320500.jpg"

typedef struct
{
    int id_music;
    char *ALBUM_IMG_PATH_300;
    char *BLURRY_BACKGROUND;
    char *music_name;
    char *artists;
    int music_progress;
    int music_duration;
} Screen_itens;

Screen_itens itens = {
    .id_music = 1,
    .ALBUM_IMG_PATH_300 = "A:src/UI/images/nirvana.png",
    .BLURRY_BACKGROUND = "A:src/UI/images/blurryBackground.png",
    .music_name = "About a Girl",
    .artists = "Nirvana",
    .music_duration = 30, 
    .music_progress = 0};

typedef struct
{
    lv_obj_t *scr;
    lv_obj_t *music_label;
    lv_obj_t *artists_label;
    lv_obj_t *img_create;
    lv_obj_t *bg_create;
    lv_obj_t *slider_create;
} Screen_layout;

static Screen_layout layout;

static void apply_track(void)
{
    lv_image_set_src(layout.img_create, itens.ALBUM_IMG_PATH_300);
    lv_obj_update_layout(layout.img_create);
    lv_image_set_pivot(layout.img_create,
                       lv_obj_get_width(layout.img_create) / 2,
                       lv_obj_get_height(layout.img_create) / 2);

    lv_image_set_src(layout.bg_create, itens.BLURRY_BACKGROUND);

    lv_label_set_text(layout.music_label, itens.music_name);
    lv_label_set_text(layout.artists_label, itens.artists);

    lv_obj_align_to(layout.music_label, layout.img_create, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 15);
    lv_obj_align_to(layout.artists_label, layout.music_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_slider_set_range(layout.slider_create, 0, itens.music_duration);
}

static void apply_progress(void)
{
    lv_slider_set_value(layout.slider_create, itens.music_progress, LV_ANIM_OFF);
}

static void load_song(int id)
{
    if (id == 2)
    {
        itens.id_music = 2;
        itens.ALBUM_IMG_PATH_300 = "A:src/UI/images/jigsaw.png";
        itens.BLURRY_BACKGROUND = "A:src/UI/images/jigsawBg.png";
        itens.music_name = "Jigsaw Falling Into Place";
        itens.artists = "Radiohead";
        itens.music_duration = 30;
        itens.music_progress = 0;
        return;
    }

    itens.id_music = 1;
    itens.ALBUM_IMG_PATH_300 = "A:src/UI/images/nirvana.png";
    itens.BLURRY_BACKGROUND = "A:src/UI/images/blurryBackground.png";
    itens.music_name = "About a Girl";
    itens.artists = "Nirvana";
    itens.music_duration = 30;
    itens.music_progress = 0;
}

static void update(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    itens.music_progress += 2;

    if (itens.music_progress >= itens.music_duration)
    {
        load_song(itens.id_music == 1 ? 2 : 1);
        apply_track(); 
    }

    apply_progress(); 
}

static void rotate_image(lv_timer_t *timer)
{
    lv_obj_t *img = lv_timer_get_user_data(timer);

    static int16_t angle = 0;
    angle += 10;
    if (angle >= 3600)
    {
        angle = 0;
    }

    lv_image_set_rotation(img, angle);
}

void screen_create(void)
{
    layout.scr = lv_screen_active();
    layout.music_label = lv_label_create(layout.scr);
    layout.artists_label = lv_label_create(layout.scr);
    layout.img_create = lv_image_create(layout.scr);
    layout.bg_create = lv_image_create(layout.scr);
    layout.slider_create = lv_slider_create(layout.scr);

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x202020), LV_PART_MAIN);

    lv_image_set_src(layout.bg_create, itens.BLURRY_BACKGROUND);
    lv_obj_set_size(layout.bg_create, LV_PCT(100), LV_PCT(100));    
    lv_image_set_inner_align(layout.bg_create, LV_IMAGE_ALIGN_STRETCH); 
    lv_obj_align(layout.bg_create, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_move_background(layout.bg_create);

    lv_image_set_src(layout.img_create, itens.ALBUM_IMG_PATH_300);
    lv_obj_align(layout.img_create, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_update_layout(layout.img_create);
    lv_image_set_pivot(layout.img_create, lv_obj_get_width(layout.img_create) / 2, lv_obj_get_height(layout.img_create) / 2);
    lv_timer_create(rotate_image, 50, layout.img_create);

    lv_obj_set_style_text_color(layout.music_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_color(layout.artists_label, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(layout.music_label, &lv_font_montserrat_20, LV_PART_MAIN);

    lv_obj_set_width(layout.music_label, lv_obj_get_width(layout.img_create));
    lv_obj_set_width(layout.artists_label, lv_obj_get_width(layout.img_create));

    lv_label_set_text(layout.music_label, itens.music_name);
    lv_label_set_long_mode(layout.music_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text(layout.artists_label, itens.artists);
    lv_label_set_long_mode(layout.artists_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    lv_obj_align_to(layout.music_label, layout.img_create, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 15);
    lv_obj_align_to(layout.artists_label, layout.music_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_obj_set_size(layout.slider_create, lv_pct(90), 3);
    lv_slider_set_range(layout.slider_create, 0, itens.music_duration);
    lv_slider_set_value(layout.slider_create, itens.music_progress, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(layout.slider_create, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(layout.slider_create, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(layout.slider_create, lv_color_hex(0xFFFFFF), LV_PART_KNOB);

    lv_obj_update_layout(layout.scr);
    int32_t prog_y = lv_obj_get_y(layout.artists_label) + lv_obj_get_height(layout.artists_label) + 15;
    lv_obj_align(layout.slider_create, LV_ALIGN_TOP_MID, 0, prog_y);

    lv_timer_create(update, 2000, NULL);
}
