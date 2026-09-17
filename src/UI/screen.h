#ifndef SCREEN_H
#define SCREEN_H

#include "lvgl.h"

#include "../spotify/spotify.h"

/** Monta a tela. Chame uma vez, depois de inicializar o LVGL. */
void screen_create(void);

/**
 * Aplica as informacoes da musica na tela (textos, capa, progresso).
 *
 * Separada da busca de proposito: no ESP32 a requisicao HTTPS roda numa task
 * propria e so o resultado e entregue aqui, com o lock do LVGL em maos.
 */
void screen_apply_info(const spotify_music_info_t *info);

/** Mensagem sobreposta ("Conectando...", "Nada tocando"). NULL esconde. */
void screen_set_status(const char *text);

/**
 * Troca a capa por uma imagem já decodificada.
 *
 * Usada no ESP32, onde a decodificação acontece fora da UI para não segurar a
 * imagem comprimida na memória. `dsc` precisa continuar válido enquanto
 * estiver na tela.
 */
void screen_set_cover(const lv_image_dsc_t *dsc);

/**
 * Troca o fundo borrado por uma imagem já decodificada. A imagem é esticada
 * para a tela inteira — por ser borrada, resolução baixa não aparece.
 */
void screen_set_background(const lv_image_dsc_t *dsc);

#endif
