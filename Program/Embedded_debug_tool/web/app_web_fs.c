/* app_web_fs.c —— SD 文件管理 Web API（见 app_web_fs.h） */

#include "app_web_fs.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web_fs";

#define FS_CHUNK     2048
#define FS_MAX_ITEMS 512      /* 单目录列表上限（防止超大目录爆内存） */
#define FS_PATH_MAX  240

/* ── 传输状态（临界区保护；httpd 任务写，LVGL 任务读） ── */
static portMUX_TYPE s_stat_mux = portMUX_INITIALIZER_UNLOCKED;
static app_web_fs_status_t s_stat;

static void fs_stat_set_begin(int upload, const char *basename, uint32_t total)
{
    portENTER_CRITICAL(&s_stat_mux);
    s_stat.upload = upload;
    s_stat.total = total;
    s_stat.done = 0;
    size_t i = 0;
    for (; basename[i] && i < sizeof(s_stat.name) - 1; i++) {
        s_stat.name[i] = basename[i];
    }
    s_stat.name[i] = '\0';
    s_stat.busy = 1;
    portEXIT_CRITICAL(&s_stat_mux);
}

static void fs_stat_progress(uint32_t done)
{
    portENTER_CRITICAL(&s_stat_mux);
    s_stat.done = done;
    portEXIT_CRITICAL(&s_stat_mux);
}

static void fs_stat_end(void)
{
    portENTER_CRITICAL(&s_stat_mux);
    s_stat.busy = 0;
    portEXIT_CRITICAL(&s_stat_mux);
}

esp_err_t app_web_fs_get_status(app_web_fs_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_stat_mux);
    *out = s_stat;
    portEXIT_CRITICAL(&s_stat_mux);
    return ESP_OK;
}

/* ── 工具 ── */

static int fs_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void fs_urldecode(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *s = in; *s && o + 1 < outsz; s++) {
        if (*s == '%') {
            int hi = fs_hex(s[1]), lo = fs_hex(s[2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)(hi * 16 + lo);
                s += 2;
                continue;
            }
        } else if (*s == '+') {
            out[o++] = ' ';
            continue;
        }
        out[o++] = *s;
    }
    out[o] = '\0';
}

/* 路径校验：必须挂载根内，且不含 ".." 路径段（防越界读写） */
static bool fs_path_ok(const char *p)
{
    size_t n = strlen(p);
    if (n == 0 || n >= FS_PATH_MAX) return false;
    if (strncmp(p, "/sdcard", 7) != 0) return false;
    if (p[7] != '/' && p[7] != '\0') return false;
    const char *s = p;
    while ((s = strstr(s, "..")) != NULL) {
        if ((s == p || s[-1] == '/') && (s[2] == '\0' || s[2] == '/')) return false;
        s += 2;
    }
    return true;
}

/* 查询参数 → 解码到 out；失败返回 false */
static bool fs_query(httpd_req_t *req, const char *key, char *out, size_t outsz)
{
    char q[320];   /* 深层路径的 p= 可能较长，查询串缓冲给足 */
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    char v[FS_PATH_MAX];
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return false;
    fs_urldecode(v, out, outsz);
    return true;
}

/* 路径 basename（下载头用） */
static const char *fs_basename(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* 文件名 JSON 字符串转义（引号/反斜杠/控制字符；UTF-8 非 ASCII 原样透传） */
static void fs_json_str(const char *s, char *out, size_t outsz)
{
    size_t o = 1;
    out[0] = '"';
    for (; *s && o + 8 < outsz; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    if (o + 2 < outsz) {
        out[o++] = '"';
        out[o] = '\0';
    } else {
        out[outsz - 1] = '\0';
    }
}

static esp_err_t fs_send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, strlen(body));
}

/* ── 各 handler ── */

static esp_err_t fs_list_handler(httpd_req_t *req)
{
    char path[FS_PATH_MAX] = "/sdcard";
    char v[FS_PATH_MAX];
    if (fs_query(req, "p", v, sizeof(v))) {
        if (!fs_path_ok(v)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        snprintf(path, sizeof(path), "%s", v);
    }

    char *buf = heap_caps_malloc(24 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    size_t len = 0;
    bool closed = false;   /* 缓冲截断时已提前闭合 */
    buf[len++] = '[';

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *ent;
        int n = 0;
        while (n < FS_MAX_ITEMS && (ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;   /* 隐藏项（含 .reader 元数据目录） */
            char fp[FS_PATH_MAX + 64];
            snprintf(fp, sizeof(fp), "%s/%s", path, ent->d_name);
            struct stat st;
            bool isdir = (ent->d_type == DT_DIR);
            long sz = 0;
            time_t mtime = 0;
            if (stat(fp, &st) == 0) {
                isdir = S_ISDIR(st.st_mode);
                sz = (long)st.st_size;
                mtime = st.st_mtime;
            }
            char esc[160];
            fs_json_str(ent->d_name, esc, sizeof(esc));
            if (n++ > 0) buf[len++] = ',';
            int a = snprintf(buf + len, 24 * 1024 - len, "{\"n\":%s,\"d\":%d,\"s\":%ld,\"m\":%lld}",
                             esc, isdir ? 1 : 0, sz, (long long)mtime);
            if (a <= 0 || (size_t)a >= 24 * 1024 - len) {
                /* 放不下当前条目：先闭合数组（保留已写条目，防 JSON 截断） */
                if (len + 2 < 24 * 1024) {
                    buf[len++] = ']';
                    buf[len] = '\0';
                    closed = true;
                }
                break;
            }
            len += (size_t)a;
        }
        closedir(d);
    }
    esp_lv_adapter_unlock();

    if (!closed && len + 1 < 24 * 1024) {
        buf[len++] = ']';
        buf[len] = '\0';
    } else if (!closed) {
        buf[24 * 1024 - 1] = '\0';
    }
    esp_err_t ret = fs_send_json(req, buf);
    heap_caps_free(buf);
    return ret;
}

static esp_err_t fs_download_handler(httpd_req_t *req)
{
    char v[FS_PATH_MAX];
    if (!fs_query(req, "p", v, sizeof(v)) || !fs_path_ok(v)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    FILE *f = fopen(v, "rb");
    if (!f) {
        esp_lv_adapter_unlock();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    /* 文件总长（进度显示） */
    struct stat st;
    uint32_t total = 0;
    if (stat(v, &st) == 0 && st.st_size > 0) total = (uint32_t)st.st_size;
    esp_lv_adapter_unlock();

    httpd_resp_set_type(req, "application/octet-stream");
    /* 下载文件名（去特殊字符，仅做展示用） */
    char fn[64], cd[96];
    const char *base = fs_basename(v);
    size_t i = 0;
    for (; base[i] && i < sizeof(fn) - 1; i++) {
        fn[i] = ((unsigned char)base[i] == '"' || (unsigned char)base[i] == ';') ? '_' : base[i];
    }
    fn[i] = '\0';
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", fn);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);

    /* 块级锁：SD 读一块（2KB）即释放，网络发送不持锁——
     * LVGL 界面每块之间有刷新机会，大文件不冻结 UI */
    fs_stat_set_begin(0, fn, total);
    char buf[FS_CHUNK];
    uint32_t done = 0;
    for (;;) {
        if (esp_lv_adapter_lock(-1) != ESP_OK) break;
        size_t r = fread(buf, 1, sizeof(buf), f);
        esp_lv_adapter_unlock();
        if (r == 0) break;
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) break;
        done += (uint32_t)r;
        fs_stat_progress(done);
    }
    /* fclose 也是 SD 操作，须持锁执行 */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        fclose(f);
        esp_lv_adapter_unlock();
    }
    fs_stat_end();
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t fs_upload_handler(httpd_req_t *req)
{
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST only");
    }
    char v[FS_PATH_MAX];
    if (!fs_query(req, "p", v, sizeof(v)) || !fs_path_ok(v)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    FILE *f = fopen(v, "wb");
    if (!f) {
        esp_lv_adapter_unlock();
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "open failed");
    }
    esp_lv_adapter_unlock();

    /* 块级锁：网络收（recv）不持锁，SD 写一块（2KB）即释放——
     * LVGL 界面每块之间有刷新机会，大文件不冻结 UI */
    fs_stat_set_begin(1, fs_basename(v), req->content_len > 0 ? (uint32_t)req->content_len : 0);
    int remaining = req->content_len;
    char buf[FS_CHUNK];
    bool ok = true;
    uint32_t done = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > FS_CHUNK ? FS_CHUNK : remaining);
        if (r <= 0) { ok = false; break; }
        if (esp_lv_adapter_lock(-1) != ESP_OK) { ok = false; break; }
        size_t w = fwrite(buf, 1, (size_t)r, f);
        esp_lv_adapter_unlock();
        if (w != (size_t)r) { ok = false; break; }
        remaining -= r;
        done += (uint32_t)r;
        fs_stat_progress(done);
    }
    /* fclose 也是 SD 操作，须持锁执行 */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        fclose(f);
        esp_lv_adapter_unlock();
    }
    fs_stat_end();
    return fs_send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t fs_mkdir_handler(httpd_req_t *req)
{
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST only");
    }
    char v[FS_PATH_MAX];
    if (!fs_query(req, "p", v, sizeof(v)) || !fs_path_ok(v)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    int r = mkdir(v, 0755);
    esp_lv_adapter_unlock();
    return fs_send_json(req, r == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t fs_delete_handler(httpd_req_t *req)
{
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST only");
    }
    char v[FS_PATH_MAX];
    if (!fs_query(req, "p", v, sizeof(v)) || !fs_path_ok(v)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    int r = remove(v);                 /* 文件 */
    if (r != 0) r = rmdir(v);          /* 空目录 */
    esp_lv_adapter_unlock();
    return fs_send_json(req, r == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t fs_rename_handler(httpd_req_t *req)
{
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST only");
    }
    char oldp[FS_PATH_MAX], newp[FS_PATH_MAX];
    if (!fs_query(req, "p", oldp, sizeof(oldp)) || !fs_query(req, "q", newp, sizeof(newp)) ||
        !fs_path_ok(oldp) || !fs_path_ok(newp)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    int r = rename(oldp, newp);
    esp_lv_adapter_unlock();
    return fs_send_json(req, r == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* ── 前端（单页） ── */

static const char FS_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\"content=\"width=device-width,initial-scale=1\">"
    "<title>SD Files</title><style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#0d0d0d;color:#e5e7eb;"
    "max-width:720px;margin:0 auto;padding:12px 14px}"
    "header{display:flex;align-items:center;gap:10px;padding-bottom:10px;border-bottom:1px solid #2a2a2a}"
    "h1{font-size:16px;font-weight:600;flex:1;color:#fff}"
    "button,input{font-family:inherit;font-size:13px}"
    "button{padding:5px 12px;background:#1f2937;color:#e5e7eb;border:1px solid #374151;"
    "border-radius:6px;cursor:pointer}"
    "button:hover{background:#374151}"
    "#path{flex:1;padding:5px 8px;background:#111;border:1px solid #2a2a2a;border-radius:6px;color:#e5e7eb}"
    "table{width:100%;border-collapse:collapse;margin-top:10px}"
    "td,th{padding:7px 8px;border-bottom:1px solid #1f2937;font-size:13px;text-align:left}"
    "td.n{color:#7dd3fc;cursor:pointer}"
    "td.n.dir{color:#fbbf24;font-weight:600}"
    "td.s,th.s{width:60px;color:#9ca3af;text-align:right}"
    "td.a{width:150px;text-align:right}"
    "td.a a,td.a button{display:inline-block;margin-left:6px}"
    "a{color:#7dd3fc;text-decoration:none}"
    "#bar{display:flex;gap:8px;margin-top:10px;align-items:center;flex-wrap:wrap}"
    "#bar input{flex:1;min-width:120px;padding:5px 8px;background:#111;border:1px solid #2a2a2a;"
    "border-radius:6px;color:#e5e7eb}"
    "#msg{font-size:12px;color:#fbbf24;min-height:16px;margin-top:8px}"
    "</style></head><body>"
    "<header><h1>SD Files</h1><button id=\"up\">\u25B2</button></header>"
    "<div id=\"bar\">"
    "<button id=\"mkdir\">+Folder</button>"
    "<input id=\"nd\" placeholder=\"new folder name\">"
    "<input type=\"file\" id=\"fu\">"
    "<button id=\"fub\">Upload</button>"
    "</div>"
    "<div id=\"msg\"></div>"
    "<table><thead><tr><th>Name</th><th class=\"s\">Size</th><th class=\"a\">Actions</th></tr></thead>"
    "<tbody id=\"tb\"></tbody></table>"
    "<script>"
    "var path='/sdcard';"
    "function msg(t){document.getElementById('msg').textContent=t||''}"
    "function enc(s){return encodeURIComponent(s)}"
    "function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')"
    ".replace(/\"/g,'&quot;')}"
    "async function api(u,opt){var r=await fetch(u,opt);if(!r.ok)throw new Error(r.status);return r}"
    "function up(){path=path.replace(/\\/[^\\/]*$/,'');if(path.indexOf('/sdcard')!==0)path='/sdcard';load()}"
    "async function load(){"
    "msg('loading...');"
    "try{var r=await api('/fs/list?p='+enc(path));var a=await r.json();"
    "var tb=document.getElementById('tb');tb.innerHTML='';"
    "a.sort(function(x,y){return x.d===y.d?(x.n<y.n?-1:1):x.d?-1:1});"
    "a.forEach(function(it){"
    "var tr=document.createElement('tr');"
    "var name=esc(it.n);"
    "var icon=it.d?'':'';"
    "var td1=document.createElement('td');td1.className='n'+(it.d?' dir':'');"
    "td1.textContent=it.n;"
    "if(it.d)td1.onclick=function(){path+='/'+it.n;load()};"
    "var td2=document.createElement('td');td2.className='s';td2.textContent=it.d?'':fmt(it.s);"
    "var td3=document.createElement('td');td3.className='a';"
    "if(!it.d){"
    "var a1=document.createElement('a');a1.textContent='dl';"
    "a1.href='/fs/download?p='+enc(path+'/'+it.n);"
    "var b1=document.createElement('button');b1.textContent='del';"
    "b1.onclick=function(){if(confirm('Delete '+it.n+'?'))go('/fs/delete?p='+enc(path+'/'+it.n))};"
    "var b2=document.createElement('button');b2.textContent='mv';"
    "b2.onclick=function(){var nn=prompt('New name',it.n);if(nn)go('/fs/rename?p='+"
    "enc(path+'/'+it.n)+'&q='+enc(path+'/'+nn))};"
    "td3.appendChild(a1);td3.appendChild(b1);td3.appendChild(b2);"
    "}else{var b3=document.createElement('button');b3.textContent='del';"
    "b3.onclick=function(){if(confirm('Delete folder '+it.n+'?'))go('/fs/delete?p='+enc(path+'/'+it.n))};"
    "td3.appendChild(b3);}"
    "tr.appendChild(td1);tr.appendChild(td2);tr.appendChild(td3);tb.appendChild(tr);"
    "});"
    "document.getElementById('path').value=path;msg('');"
    "}catch(e){msg('list failed: '+e)}"
    "}"
    "function fmt(n){if(n>=1048576)return (n/1048576).toFixed(1)+'M';"
    "if(n>=1024)return (n/1024).toFixed(1)+'K';return n}"
    "async function go(u){try{var r=await api(u,{method:'POST'});var j=await r.json();"
    "if(j.ok)msg('ok');else msg('failed');load()}catch(e){msg('err '+e)}}"
    "document.getElementById('up').onclick=up;"
    "document.getElementById('path').onkeydown=function(e){"
    "if(e.key==='Enter'){var p=this.value.trim();if(p.indexOf('/sdcard')===0&&p.length>0)path=p;load()}};"
    "document.getElementById('mkdir').onclick=function(){"
    "var n=document.getElementById('nd').value.trim();"
    "if(!n){msg('folder name?');return}"
    "go('/fs/mkdir?p='+enc(path+'/'+n));document.getElementById('nd').value=''};"
    "document.getElementById('fub').onclick=function(){"
    "var f=document.getElementById('fu').files[0];"
    "if(!f){msg('pick a file');return}"
    "msg('uploading '+f.name+'...');"
    "fetch('/fs/upload?p='+enc(path+'/'+f.name),{method:'POST',body:f}).then(function(r){return r.json()})"
    ".then(function(j){msg(j.ok?'uploaded':'failed');load()}).catch(function(e){msg('err '+e)})};"
    "document.getElementById('path').value=path;load();"
    "</script></body></html>";

static esp_err_t fs_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, FS_HTML, sizeof(FS_HTML) - 1);
}

/* ── 开关：注册/注销 /fs* 路由（默认关闭，由 WebFS App 启用） ── */

static const httpd_uri_t s_fs_uris[] = {
    { .uri = "/fs",          .method = HTTP_GET,  .handler = fs_page_handler },
    { .uri = "/fs/list",     .method = HTTP_GET,  .handler = fs_list_handler },
    { .uri = "/fs/download", .method = HTTP_GET,  .handler = fs_download_handler },
    { .uri = "/fs/upload",   .method = HTTP_POST, .handler = fs_upload_handler },
    { .uri = "/fs/mkdir",    .method = HTTP_POST, .handler = fs_mkdir_handler },
    { .uri = "/fs/delete",   .method = HTTP_POST, .handler = fs_delete_handler },
    { .uri = "/fs/rename",   .method = HTTP_POST, .handler = fs_rename_handler },
};
#define FS_URI_COUNT (sizeof(s_fs_uris) / sizeof(s_fs_uris[0]))

httpd_handle_t g_web_fs_httpd;   /* 由 app_web_http 启动时注入 */
static bool s_fs_enabled;

esp_err_t app_web_fs_start(void)
{
    if (s_fs_enabled || !g_web_fs_httpd) return s_fs_enabled ? ESP_OK : ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < FS_URI_COUNT; i++) {
        esp_err_t r = httpd_register_uri_handler(g_web_fs_httpd, &s_fs_uris[i]);
        if (r != ESP_OK && r != ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGW(TAG, "register %s failed: %s", s_fs_uris[i].uri, esp_err_to_name(r));
            ret = r;
        }
    }
    if (ret == ESP_OK) s_fs_enabled = true;
    ESP_LOGI(TAG, "fs %s", s_fs_enabled ? "enabled" : "enable failed");
    return ret;
}

void app_web_fs_stop(void)
{
    if (!s_fs_enabled) return;
    for (size_t i = 0; i < FS_URI_COUNT; i++) {
        httpd_unregister_uri_handler(g_web_fs_httpd, s_fs_uris[i].uri, s_fs_uris[i].method);
    }
    s_fs_enabled = false;
    ESP_LOGI(TAG, "fs disabled");
}

bool app_web_fs_enabled(void)
{
    return s_fs_enabled && g_web_fs_httpd != NULL;
}