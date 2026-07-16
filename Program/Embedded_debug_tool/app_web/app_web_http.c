#include "app_web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "app_web";

httpd_handle_t g_httpd;

static void set_no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
}

static esp_err_t root_handler(httpd_req_t *req)
{
    set_no_cache(req);
    const char *html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\"content=\"width=device-width,initial-scale=1\">"
        "<title>Embedded Debug Tool</title>"
        "<style>body{font-family:-apple-system,system-ui,sans-serif;background:#fff;"
        "color:#111;padding:40px 24px}h1{font-size:18px;font-weight:600;margin-bottom:24px}"
        "a{display:block;padding:14px 16px;border:1px solid #e5e5e5;border-radius:8px;"
        "text-decoration:none;color:#111;margin-bottom:10px;font-size:14px}"
        "a:hover{background:#f9fafb;border-color:#9ca3af}"
        ".sub{color:#9ca3af;font-size:12px;margin-top:4px}</style></head><body>"
        "<h1>Embedded Debug Tool</h1>"
        "<a href=\"/page?uart=0\">UART1<span class=\"sub\">IO2 / IO4 &middot; TCP :8080</span></a>"
        "<a href=\"/page?uart=1\">UART2<span class=\"sub\">IO16 / IO17 &middot; TCP :8081</span></a>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

/* 前向声明 — 在 app_web_ws.c 中实现 */
extern esp_err_t app_ws_handler(httpd_req_t *req);

static esp_err_t page_handler(httpd_req_t *req)
{
    set_no_cache(req);

    char qbuf[16] = {0};
    int idx = 0;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char v[4];
        if (httpd_query_key_value(qbuf, "uart", v, sizeof(v)) == ESP_OK) idx = atoi(v);
    }
    if (idx < 0 || idx > 1) idx = 0;
    uart_bridge_t *br = g_bridges[idx];

    char *page = malloc(5120);
    if (!page) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    int n = snprintf(page, 5120,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\"content=\"width=device-width,initial-scale=1\">"
        "<title>%s</title><style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:-apple-system,system-ui,sans-serif;background:#fff;color:#111;"
        "display:flex;flex-direction:column;height:100vh;padding:12px 16px}"
        "header{display:flex;align-items:center;gap:12px;padding-bottom:10px;"
        "border-bottom:1px solid #e5e5e5}"
        "h1{font-size:15px;font-weight:600;flex:1}"
        ".dot{width:7px;height:7px;border-radius:50%%;background:#ddd}"
        ".dot.on{background:#22c55e}"
        "button{padding:5px 14px;border:1px solid #d1d5d5;background:#fff;color:#374151;"
        "border-radius:6px;font-size:12px;cursor:pointer;font-family:inherit}"
        "button:hover{background:#f9fafb;border-color:#9ca3af}"
        "button.active{background:#f0fdf4;border-color:#22c55e;color:#166534}"
        "#log{flex:1;width:100%%;margin-top:10px;border:1px solid #e5e5e5;border-radius:8px;"
        "padding:10px;font-family:'SF Mono',Menlo,Consolas,monospace;font-size:12px;"
        "line-height:1.5;resize:none;outline:none;background:#fafafa;color:#111}"
        "#log:focus{border-color:#9ca3af}"
        ".info{font-size:11px;color:#9ca3af;text-align:right;padding-top:8px}"
        "#snd{display:flex;gap:6px;align-items:center;padding-top:8px;border-top:1px solid #e5e5e5;margin-top:4px}"
        "#si{flex:1;padding:6px 10px;border:1px solid #d1d5d5;border-radius:6px;font-size:13px;outline:none;font-family:inherit}"
        "#si:focus{border-color:#9ca3af}"
        "#snd label{font-size:12px;color:#374151;display:flex;align-items:center;gap:3px;white-space:nowrap}"
        "#ti{width:65px;padding:5px 6px;border:1px solid #d1d5d5;border-radius:6px;font-size:12px;outline:none;text-align:center}"
        "#sb{padding:6px 16px;border:none;background:#111;color:#fff;border-radius:6px;font-size:12px;cursor:pointer;font-family:inherit}"
        "#sb:hover{background:#333}"
        "</style></head><body>"
        "<header>"
        "<span class=\"dot\" id=\"st\"></span>"
        "<h1>%s</h1>"
        "<button id=\"bp\">暂停</button>"
        "<button id=\"bc\">清空</button>"
        "</header>"
        "<textarea id=\"log\" readonly spellcheck=\"false\"></textarea>"
        "<div class=\"info\" id=\"bi\">RX: 0 &nbsp; TX: 0</div>"
        "<div id=\"snd\">"
        "<input type=\"text\" id=\"si\" placeholder=\"输入数据...\" autocomplete=\"off\">"
        "<label><input type=\"checkbox\" id=\"nl\"> 回车</label>"
        "<label><input type=\"checkbox\" id=\"hx\"> HEX</label>"
        "<input type=\"number\" id=\"ti\" value=\"1000\" min=\"100\" style=\"width:70px\">"
        "<label><input type=\"checkbox\" id=\"te\"> 定时</label>"
        "<button id=\"sb\">发送</button>"
        "</div>"
        "<script>"
        "var U=%d,ws,paused=0,RX=0,TX=0;"
        "function wsSend(m){if(ws&&ws.readyState===1)ws.send(m)}"
        "function updInfo(){document.getElementById('bi').textContent='RX: '+RX+'  TX: '+TX}"
        "function connect(){"
        "ws=new WebSocket('ws://'+location.host+'/ws'+U);"
        "ws.onopen=function(){document.getElementById('st').className='dot on'};"
        "ws.onclose=function(){document.getElementById('st').className='dot';"
        "setTimeout(connect,1000)};"
        "ws.onmessage=function(e){"
        "var m=e.data;"
        "if(m.charAt(0)==='S'){"
        "var p=m.substring(2).split(',');"
        "document.getElementById('te').checked=!!parseInt(p[0]);"
        "document.getElementById('ti').value=p[1];"
        "paused=parseInt(p[2]);"
        "var bp=document.getElementById('bp');"
        "bp.textContent=paused?'继续':'暂停';bp.className=paused?'active':'';"
        "return}"
        "if(paused)return;"
        "var t=document.getElementById('log'),"
        "at=t.scrollTop>=t.scrollHeight-t.clientHeight-10;"
        "t.value+=m;RX+=m.length;"
        "if(at)t.scrollTop=t.scrollHeight;"
        "updInfo()}};"
        "function doSend(){"
        "var d=document.getElementById('si').value;"
        "if(!d)return;"
        "var hx=document.getElementById('hx').checked;"
        "wsSend(hx?'sendh:'+d:'send:'+d);"
        "TX+=hx?Math.ceil(d.length/2):d.length;"
        "updInfo()}"
        "document.getElementById('sb').onclick=doSend;"
        "document.getElementById('si').onkeydown=function(e){if(e.key==='Enter')doSend()};"
        "document.getElementById('nl').onchange=function(){"
        "wsSend('cfg:'+(this.checked?1:0)+','+(document.getElementById('hx').checked?1:0))};"
        "document.getElementById('hx').onchange=function(){"
        "wsSend('cfg:'+(document.getElementById('nl').checked?1:0)+','+(this.checked?1:0))};"
        "document.getElementById('ti').onchange=function(){"
        "wsSend('tconf:'+this.value)};"
        "document.getElementById('te').onchange=function(){"
        "wsSend('timer:'+(this.checked?1:0));"
        "wsSend('tconf:'+document.getElementById('ti').value);"
        "if(this.checked){var d=document.getElementById('si').value;"
        "if(d){var hx=document.getElementById('hx').checked;"
        "wsSend(hx?'sendh:'+d:'send:'+d)}}};"
        "document.getElementById('bc').onclick=function(){"
        "document.getElementById('log').value='';RX=0;TX=0;"
        "updInfo();wsSend('clear')};"
        "document.getElementById('bp').onclick=function(){"
        "paused=!paused;var b=document.getElementById('bp');"
        "if(paused){b.textContent='继续';b.className='active';"
        "wsSend('pause:1')}else{"
        "b.textContent='暂停';b.className='';"
        "wsSend('pause:0')}};"
        "connect();"
        "</script></body></html>",
        br->name, br->name, idx);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, n);
    free(page);
    return ESP_OK;
}

void app_web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 16384;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "starting...");
    ESP_ERROR_CHECK(httpd_start(&g_httpd, &config));

    static const httpd_uri_t uris[] = {
        { .uri = "/",     .method = HTTP_GET, .handler = root_handler },
        { .uri = "/page", .method = HTTP_GET, .handler = page_handler },
        { .uri = "/ws0",  .method = HTTP_GET, .handler = app_ws_handler,
          .is_websocket = true, .handle_ws_control_frames = true },
        { .uri = "/ws1",  .method = HTTP_GET, .handler = app_ws_handler,
          .is_websocket = true, .handle_ws_control_frames = true },
    };
    for (int i = 0; i < 4; i++) {
        httpd_register_uri_handler(g_httpd, &uris[i]);
    }
    ESP_LOGI(TAG, "ready (HTTP + WebSocket)");
}
