#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize LVGL display driver — call once after VESA + LVGL init */
void lv_port_disp_init(void);

#ifdef __cplusplus
}
#endif

#endif