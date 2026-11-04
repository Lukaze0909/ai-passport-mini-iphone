<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

| Files | Format and use | Source and license |
| --- | --- | --- |
| [`fonts/release_guidance/`](fonts/release_guidance/meta.json) `rg_wenkai_*.c` | Eight LVGL 9 bitmap subsets (13–30 px, 4 bpp, uncompressed, Flash const) for the Release Guidance app; each `.chars` file is the exact glyph inventory generated from `tools/release_guidance/rg_spec.py`. Regenerate with `tools/release_guidance/gen_fonts.sh` (lv_font_conv 1.5.3). | Derived from LXGW WenKai Regular, SIL OFL 1.1 ([`LICENSE.txt`](fonts/release_guidance/LICENSE.txt)). |

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

| Files | Format and use | Source and license |
| --- | --- | --- |
| `music/release_guidance/snd_{A_droplet,B_bell,C_marimba}.wav` | 24 kHz, 16-bit mono PCM reference phrases (three taps, then the two-note confirm). `tools/release_guidance/gen_assets.py` slices one tap and the confirm phrase from each and embeds them at 0.6 gain in `main/rg/rg_sound_data.c` (about 98 KB Flash). 24 kHz keeps the bell overtones below Nyquist. | Synthesized for this project by the classic-shell fork authors, 2026-10-31. |
