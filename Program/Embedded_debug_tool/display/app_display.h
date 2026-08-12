#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

void app_display_start(void);
void app_display_set_info(const char *ip, int uart1_port, int uart2_port);
void app_display_notify_status(void);
void app_display_notify_sd_ready(void);   /* SD 挂载完成后刷新文件浏览器 */

#endif
