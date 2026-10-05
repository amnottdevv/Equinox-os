#ifndef TVG_CONFIG_H
#define TVG_CONFIG_H
/* config.h — konfigurasi port ThorVG ke Equinox OS (dibuat tangan,
 * menggantikan config.h yang biasanya digenerate meson).
 * Mode: sw-engine only, TANPA threads, TANPA loaders/savers/gl/wg. */
#define THORVG_VERSION_STRING "1.0.0-eqx"
#define TVG_EXPORT
#define TVG_BUILD
#define THORVG_SW_RASTER_SUPPORT 1
/* sengaja TIDAK didefinisikan:
 * THORVG_THREAD_SUPPORT  (kernel single-thread draw path)
 * THORVG_GL/WG_RASTER_SUPPORT
 * THORVG_*_LOADER_SUPPORT / SAVER (tidak ada file I/O dari tvg)
 * THORVG_PARTIAL_RENDER_SUPPORT (aktifkan saat compositor Task 2) */
#endif
