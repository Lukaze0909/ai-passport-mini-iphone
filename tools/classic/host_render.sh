#!/usr/bin/env bash
# Render the real classic shell UI (main/classic/cs_ui.c + generated fonts and
# images) with desktop LVGL 9.5.0 and write PPM frames. Developer tool only; it
# is not part of tools/validate.sh and proves nothing about the panel or timing.
#   tools/classic/host_render.sh <lvgl-9.5.0-checkout> <work-dir> <out-dir>
# Reuses <work-dir>/liblvgl.a when present (see tools/release_guidance/host_render.sh).
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
lvgl="$(cd -- "$1" && pwd)"
work="$2"
out="$3"
mkdir -p "${work}/obj" "${out}"
if [[ ! -f "${work}/lv_conf.h" ]]; then
    sed -e 's/^#if 0 .*Set this to "1" to enable content.*/#if 1/' \
        -e 's/^#define LV_COLOR_DEPTH .*/#define LV_COLOR_DEPTH 16/' \
        -e 's/^\( *\)#define LV_MEM_SIZE .*/\1#define LV_MEM_SIZE (32 * 1024U)/' \
        "${lvgl}/lv_conf_template.h" > "${work}/lv_conf.h"
fi
defs=(-DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I"${work}" -I"${lvgl}")
if [[ ! -f "${work}/liblvgl.a" ]]; then
    find "${lvgl}/src" -name '*.c' \
        | grep -v -e /thorvg/ -e /drivers/ -e '/libs/[^b]' -e /draw/vg_lite -e /draw/nxp \
              -e /draw/renesas -e /draw/opengles -e /draw/dma2d -e /draw/sdl -e /draw/espressif \
              -e /draw/eve -e /draw/nema \
        | while read -r src; do
            cc -O1 -w "${defs[@]}" -I"${lvgl}/src" -c "${src}" \
                -o "${work}/obj/$(echo "${src}" | cksum | cut -d' ' -f1).o"
        done
    ar rcs "${work}/liblvgl.a" "${work}"/obj/*.o
fi
cc -O1 -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror "${defs[@]}" -I"${repo}/main/classic" \
    "${repo}/tools/classic/host_render.c" \
    "${repo}"/main/classic/{cs_ui,cs_logic,cs_assets,cs_mines_art,cs_astro_art}.c \
    "${repo}"/assets/fonts/classic/cs_*.c \
    "${work}/liblvgl.a" -lm -o "${work}/cs_host_render"
"${work}/cs_host_render" "${out}"
