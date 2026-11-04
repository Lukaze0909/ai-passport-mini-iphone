#!/usr/bin/env bash
# Render the real Release Guidance UI (main/rg/rg_ui.c + generated fonts/images)
# with desktop LVGL 9.5.0 and write PPM frames. Developer tool only; it is not
# part of tools/validate.sh and proves nothing about the panel or timing.
#   tools/release_guidance/host_render.sh <lvgl-9.5.0-checkout> <work-dir> <out-dir>
# Needs a C compiler; converting PPM to PNG is left to the caller.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
lvgl="$(cd -- "$1" && pwd)"
work="$2"
out="$3"
mkdir -p "${work}/obj" "${work}/stubs" "${out}"

sed -e 's/^#if 0 .*Set this to "1" to enable content.*/#if 1/' \
    -e 's/^#define LV_COLOR_DEPTH .*/#define LV_COLOR_DEPTH 16/' \
    -e 's/^\( *\)#define LV_MEM_SIZE .*/\1#define LV_MEM_SIZE (32 * 1024U)/' \
    "${lvgl}/lv_conf_template.h" > "${work}/lv_conf.h"
cat > "${work}/stubs/esp_log.h" <<'EOF'
#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
EOF
printf '#pragma once\n#define CONFIG_RG_FX_ENABLE 1\n#define CONFIG_RG_RAIN_DROPS 16\n#define CONFIG_RG_LEAVES 4\n#define CONFIG_RG_PAGE_FADE_MS 0\n' \
    > "${work}/stubs/sdkconfig.h"

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
cc -O1 -std=c11 -Wall -Wextra -Werror "${defs[@]}" -I"${work}/stubs" -I"${repo}/main/rg" \
    "${repo}/tools/release_guidance/host_render.c" \
    "${repo}"/main/rg/{rg_ui,rg_flow,rg_theme,rg_content,rg_images}.c \
    "${repo}"/assets/fonts/release_guidance/rg_wenkai_*.c \
    "${work}/liblvgl.a" -lm -o "${work}/host_render"
"${work}/host_render" "${out}"
