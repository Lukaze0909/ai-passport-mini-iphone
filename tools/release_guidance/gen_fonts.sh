#!/usr/bin/env bash
# Regenerate the Release Guidance LXGW WenKai subsets from the character inventories
# written by gen_content.py. Requires Node.js (npx) and the licensed TTF:
#   LXGW_WENKAI_TTF=/path/to/LXGWWenKai-Regular.ttf tools/release_guidance/gen_fonts.sh
# Converter: lv_font_conv 1.5.3, 4 bpp, uncompressed, LVGL 9 format.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
ttf="${LXGW_WENKAI_TTF:?set LXGW_WENKAI_TTF to LXGWWenKai-Regular.ttf}"
out="${repo_root}/assets/fonts/release_guidance"
conv="npx --yes lv_font_conv@1.5.3"

for spec in note19:19 opt21:21 long22:22 q25:25 entry27:27 key28:28 key30:30 small13:13; do
    name="${spec%%:*}"
    px="${spec##*:}"
    chars="$(tr -d '\n' < "${out}/rg_wenkai_${name}.chars")"
    ${conv} --font "${ttf}" --symbols "${chars}" \
        --size "${px}" --bpp 4 --format lvgl --no-compress \
        --lv-font-name "rg_wenkai_${name}" --lv-include lvgl.h \
        --output "${out}/rg_wenkai_${name}.c"
    # Keep machine-specific paths out of the recorded converter options.
    perl -pi -e 's{(--font )\S*/}{$1}; s{(--output )\S+}{$1assets/fonts/release_guidance/rg_wenkai_'"${name}"'.c}' \
        "${out}/rg_wenkai_${name}.c"
done
