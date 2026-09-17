/**
 * @file cover.h
 *
 * Converte um JPEG em RGB565 pronto para o LVGL.
 *
 * O recorte circular da capa NAO acontece aqui: e feito por um canal alfa
 * preenchido no boot (ver fill_circle_alpha em main.c). Mascarar os pixels nao
 * serviria, porque a capa gira e cantos opacos varreriam por cima do que esta
 * em volta.
 */

#ifndef COVER_H
#define COVER_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decodifica um JPEG para RGB565.
 *
 * Os buffers vem do chamador de proposito: precisam ser reservados no boot,
 * antes do WiFi subir, enquanto ainda ha um bloco contiguo grande o bastante.
 *
 * @param jpeg     bytes do JPEG (o base64 ja foi desfeito na recepcao)
 * @param jpeg_len tamanho em bytes
 * @param out      destino dos pixels RGB565
 * @param out_cap  capacidade de `out` em bytes
 * @param dsc      preenchido com o descritor para lv_image_set_src()
 * @param scale    divisor aplicado durante a decodificacao: 1, 2, 4 ou 8. O
 *                 JPEG reduz de graca enquanto decodifica, entao a imagem
 *                 grande nunca chega a existir na memoria.
 * @param sat      saturacao em %, 100 mantem como veio
 * @param bright   brilho em %, 100 mantem como veio
 * @return true se decodificou.
 */
bool cover_decode(const uint8_t *jpeg, size_t jpeg_len, uint8_t *out, size_t out_cap,
                  lv_image_dsc_t *dsc, int scale, int sat, int bright);

#ifdef __cplusplus
}
#endif

#endif /*COVER_H*/
