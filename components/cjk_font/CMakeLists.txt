# WenQuanYi 12px CJK bitmap font for u8g2.
#
# This exists as its own component rather than going through u8g2's own
# U8G2_USE_LARGE_FONTS switch, because that switch gates *every* large font in
# u8g2_fonts.c — all 30 wqy sizes and variants, 2.73 MB in total.  That does
# not fit alongside the AirPlay stack in the 3 MB app partition.  Here exactly
# one font is compiled, and only when the display is actually enabled.
#
# The source is u8g2's own prebuilt u8g2_font_wqy12_t_gb2312.c taken from
# tools/font/build/single_font_files/, with the #ifdef U8G2_USE_LARGE_FONTS
# guard replaced by #include "u8g2.h" (U8G2_FONT_SECTION, needed by the
# definition, lives there).  7539 glyphs, 198 KB of .rodata.
#
# u8g2_fonts.c declares the same symbol, but under the same
# U8G2_USE_LARGE_FONTS guard, which is never defined here — so there is no
# duplicate definition and the symbol resolves to this copy.

# Only the OLED path uses this face; the ST7789/LVGL build draws with LVGL
# fonts and would just be carrying 198 KB of dead weight.
if(NOT CONFIG_DISPLAY_ENABLED OR CONFIG_DISPLAY_DRIVER_ST7789)
    idf_component_register(INCLUDE_DIRS ".")
    return()
endif()

# u8g2's headers live in its csrc/ directory, which the component exposes via
# INCLUDE_DIRS — but `REQUIRES u8g2` did not put that path on this component's
# compile line (the cjk_font objects came out with no -I pointing at u8g2 at
# all), so the include directory is added explicitly here instead.  The
# dependency is still declared so the link order carries the u8g2 library.
idf_component_get_property(_u8g2_dir u8g2 COMPONENT_DIR)

idf_component_register(
    SRCS "u8g2_font_wqy12_t_gb2312.c"
    INCLUDE_DIRS "."
    REQUIRES u8g2
)

target_include_directories(${COMPONENT_LIB} PRIVATE "${_u8g2_dir}/csrc")
