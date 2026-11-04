idf_component_register(
    SRCS "main.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
         "rg/rg_app.c"
         "rg/rg_ui.c"
         "rg/rg_flow.c"
         "rg/rg_input.c"
         "rg/rg_theme.c"
         "rg/rg_content.c"
         "rg/rg_images.c"
         "rg/rg_sound_data.c"
         "classic/cs_shell.c"
         "classic/cs_ui.c"
         "classic/cs_logic.c"
         "classic/cs_net.c"
         "classic/cs_audio.c"
         "classic/cs_call.c"
         "classic/cs_msgs.c"
         "classic/cs_memos.c"
         "classic/cs_mines.c"
         "classic/cs_mines_art.c"
         "classic/cs_bingo.c"
         "classic/cs_astro.c"
         "classic/cs_astro_art.c"
         "classic/cs_assets.c"
    INCLUDE_DIRS "." "rg" "classic"
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_partition esp_timer esp_wifi nvs_flash
             mqtt mbedtls esp_http_client
)

# Release Guidance: LXGW WenKai subsets (SIL OFL 1.1), see assets/fonts/release_guidance/.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/release_guidance/rg_wenkai_note19.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/release_guidance/rg_wenkai_opt21.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/release_guidance/rg_wenkai_long22.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/release_guidance/rg_wenkai_q25.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/release_guidance/rg_wenkai_entry27.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/release_guidance/rg_wenkai_key28.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/release_guidance/rg_wenkai_key30.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/release_guidance/rg_wenkai_small13.c"
)

# Classic shell: Noto Sans CJK SC subsets (SIL OFL 1.1), see assets/fonts/classic/.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/classic/cs_font12.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/classic/cs_font14.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/classic/cs_font18.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/classic/cs_num40.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/classic/cs_num52.c"
)
