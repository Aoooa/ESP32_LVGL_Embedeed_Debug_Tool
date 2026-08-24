#ifndef READER_FAVCACHE_H
#define READER_FAVCACHE_H

/* reader_favcache —— TXT 阅读收藏的 SD 持久化。
 *
 * 收藏粒度 = 行：每项存{行号, 该行内容快照}（快照尽量多，用于列表预览）。
 * 存 /sdcard/.reader/<basename>.fav，纯文本：每行 "<行号>\t<内容>"。
 * 读回按行号排序；写回覆盖。
 * SD 与 LCD 共享 SPI2 总线：调用须持 esp_lv_adapter 锁（本项目约定）。
 */

#include <stdbool.h>

#define FAV_MAX_ITEMS  64            /* 每 txt 收藏上限 */
#define FAV_CONTENT_MAX 120          /* 内容快照最大字符（含终止符） */

typedef struct {
    int line;                        /* 行号（渲染行，与索引一致） */
    char content[FAV_CONTENT_MAX];   /* 该行内容快照 */
} recent_fav_item_t;                 /* 命名避免与 idf 冲突 */

typedef struct {
    recent_fav_item_t items[FAV_MAX_ITEMS];
    int count;
} reader_fav_list_t;

/* 从 SD 读回收藏（无文件/IO 失败清空列表返回 false；有则填列表返回 true） */
bool reader_fav_load(const char *txt_path, reader_fav_list_t *list);

/* 写收藏到 SD（覆盖）。失败静默。 */
void reader_fav_save(const char *txt_path, const reader_fav_list_t *list);

#endif /* READER_FAVCACHE_H */
