#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web_server.h"
#include "wifi_service.h"

static const char* TAG = "WEB_SRV";

extern "C" {

static esp_err_t root_get_handler(httpd_req_t* req)
{
    const char* html = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-S3 WiFi Manager</title>
    <style>
        *{margin:0;padding:0;box-sizing:border-box}
        body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#1a1a2e 0%,#16213e 50%,#0f3460 100%);min-height:100vh;padding:20px;color:#e0e0e0}
        .container{max-width:800px;margin:0 auto}
        .header{text-align:center;margin-bottom:30px;padding:30px 0}
        .header h1{font-size:2.2em;color:#e94560;margin-bottom:8px}
        .header p{color:#8892b0;font-size:1.1em}
        .card{background:rgba(255,255,255,0.05);border-radius:16px;padding:24px;margin-bottom:20px;border:1px solid rgba(255,255,255,0.08);backdrop-filter:blur(10px)}
        .card h2{color:#e94560;margin-bottom:16px;font-size:1.2em;display:flex;align-items:center;gap:8px}
        .card h2 .dot{width:10px;height:10px;border-radius:50%;display:inline-block}
        .dot-online{background:#00ff88;box-shadow:0 0 8px #00ff8866}
        .dot-offline{background:#ff4444;box-shadow:0 0 8px #ff444466}
        .dot-connecting{background:#ffaa00;box-shadow:0 0 8px #ffaa0066;animation:pulse 1s infinite}
        @keyframes pulse{0%,100%{opacity:1}50%{opacity:0.3}}
        .status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin-top:12px}
        .status-item{background:rgba(255,255,255,0.03);padding:14px;border-radius:10px;border:1px solid rgba(255,255,255,0.05)}
        .status-label{font-size:11px;color:#8892b0;text-transform:uppercase;letter-spacing:1px;margin-bottom:6px}
        .status-value{font-size:17px;font-weight:600;color:#e0e0e0;word-break:break-all}
        .rssi-bar{height:4px;background:rgba(255,255,255,0.1);border-radius:2px;margin-top:6px;overflow:hidden}
        .rssi-fill{height:100%;border-radius:2px;transition:width 0.5s,background 0.5s}
        .rssi-good{background:linear-gradient(90deg,#00ff88,#00cc6a)}
        .rssi-ok{background:linear-gradient(90deg,#ffaa00,#ff8800)}
        .rssi-bad{background:linear-gradient(90deg,#ff4444,#cc2222)}
        .mode-toggle{display:flex;gap:10px;margin-bottom:16px}
        .mode-btn{flex:1;padding:12px;border:2px solid rgba(255,255,255,0.1);border-radius:10px;background:rgba(255,255,255,0.03);color:#8892b0;font-size:14px;font-weight:600;cursor:pointer;transition:all 0.3s;text-align:center}
        .mode-btn.active{border-color:#e94560;background:rgba(233,69,96,0.1);color:#e94560}
        .mode-btn:hover{border-color:rgba(233,69,96,0.4)}
        .form-group{margin-bottom:14px}
        label{display:block;margin-bottom:6px;color:#8892b0;font-size:13px;font-weight:500}
        input[type="text"],input[type="password"]{width:100%;padding:12px 15px;background:rgba(255,255,255,0.05);border:1px solid rgba(255,255,255,0.1);border-radius:8px;color:#e0e0e0;font-size:15px;transition:all 0.3s}
        input:focus{outline:none;border-color:#e94560;box-shadow:0 0 0 3px rgba(233,69,96,0.15)}
        .btn{width:100%;padding:13px;border:none;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;transition:all 0.3s;text-transform:uppercase;letter-spacing:0.5px}
        .btn-primary{background:linear-gradient(135deg,#e94560,#c23152);color:white}
        .btn-primary:hover{transform:translateY(-1px);box-shadow:0 6px 20px rgba(233,69,96,0.3)}
        .btn-primary:disabled{opacity:0.5;cursor:not-allowed;transform:none}
        .btn-outline{background:transparent;color:#e94560;border:2px solid #e94560;margin-top:8px}
        .btn-outline:hover{background:rgba(233,69,96,0.1)}
        .btn-danger{background:rgba(255,68,68,0.15);color:#ff6666;margin-top:8px}
        .btn-danger:hover{background:rgba(255,68,68,0.25)}
        .section-ap{display:none}
        .section-ap.show{display:block}
        .section-sta{display:none}
        .section-sta.show{display:block}
        .toast{position:fixed;top:20px;right:20px;padding:14px 20px;border-radius:10px;color:white;font-weight:500;z-index:999;animation:slideIn 0.3s ease;display:none}
        .toast-success{background:rgba(0,255,136,0.15);border:1px solid rgba(0,255,136,0.3);color:#00ff88}
        .toast-error{background:rgba(255,68,68,0.15);border:1px solid rgba(255,68,68,0.3);color:#ff6666}
        @keyframes slideIn{from{transform:translateX(100px);opacity:0}to{transform:translateX(0);opacity:1}}
        .ip-box{background:rgba(233,69,96,0.1);border:1px solid rgba(233,69,96,0.2);padding:10px 15px;border-radius:8px;font-family:monospace;margin-top:8px}
        .badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:12px;font-weight:600}
        .badge-ap{background:rgba(0,200,255,0.15);color:#00c8ff}
        .badge-sta{background:rgba(0,255,136,0.15);color:#00ff88}
        @media(max-width:600px){.header h1{font-size:1.6em}.status-grid{grid-template-columns:1fr 1fr}}
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>ESP32-S3 WiFi Manager</h1>
            <p>USB Network + WiFi AP/STA Configuration</p>
        </div>

        <div class="card">
            <h2><span class="dot dot-offline" id="statusDot"></span> WiFi 状态</h2>
            <div style="margin-bottom:10px"><span class="badge badge-sta" id="modeBadge">STA</span></div>
            <div class="status-grid">
                <div class="status-item"><div class="status-label">连接状态</div><div class="status-value" id="wifiState">--</div></div>
                <div class="status-item"><div class="status-label">WiFi SSID</div><div class="status-value" id="wifiSSID">--</div></div>
                <div class="status-item"><div class="status-label">WiFi IP</div><div class="status-value" id="wifiIP">--</div></div>
                <div class="status-item">
                    <div class="status-label">信号强度</div>
                    <div class="status-value" id="wifiRSSI">--</div>
                    <div class="rssi-bar"><div class="rssi-fill" id="rssiBar" style="width:0%"></div></div>
                </div>
            </div>
            <div class="status-item" style="margin-top:10px" id="apClientsBox">
                <div class="status-label">AP 客户端</div>
                <div class="status-value" id="apClients">0</div>
            </div>
        </div>

        <div class="card">
            <h2>⚙ 工作模式</h2>
            <div class="mode-toggle">
                <button class="mode-btn active" id="btnStaMode" onclick="switchMode('sta')">STA 模式 (客户端)</button>
                <button class="mode-btn" id="btnApMode" onclick="switchMode('ap')">AP 模式 (热点)</button>
            </div>
            <p style="color:#8892b0;font-size:13px;margin-top:8px" id="modeDesc">STA 模式：连接到已有 WiFi 网络，通过 USB 共享上网</p>
        </div>

        <div class="card section-sta show" id="sectionSta">
            <h2>⚙ STA 配置 (连接 WiFi)</h2>
            <form id="wifiForm" onsubmit="return false;">
                <div class="form-group"><label for="ssid">WiFi SSID (网络名称)</label><input type="text" id="ssid" name="ssid" placeholder="输入 WiFi 名称" required></div>
                <div class="form-group"><label for="password">WiFi 密码</label><input type="password" id="password" name="password" placeholder="输入 WiFi 密码 (至少8位)" minlength="8" required></div>
                <button type="submit" class="btn btn-primary" id="connectBtn" onclick="connectWiFi()">连接 WiFi</button>
            </form>
        </div>

        <div class="card section-ap" id="sectionAp">
            <h2>⚙ AP 配置 (热点设置)</h2>
            <form id="apForm" onsubmit="return false;">
                <div class="form-group"><label for="apSsid">AP SSID (热点名称)</label><input type="text" id="apSsid" name="apSsid" required></div>
                <div class="form-group"><label for="apPassword">AP 密码</label><input type="text" id="apPassword" name="apPassword" minlength="8" required></div>
                <button type="submit" class="btn btn-primary" id="apBtn" onclick="startAP()">启动热点</button>
                <button type="button" class="btn btn-outline" id="stopApBtn" onclick="stopAP()" style="display:none">停止热点</button>
            </form>
        </div>

        <div class="card">
            <h2>🔌 USB 网络</h2>
            <p style="color:#8892b0;margin-bottom:8px">通过 USB 线连接电脑，电脑会自动识别为以太网设备并获得 IP</p>
            <div class="ip-box">ESP32 USB 管理地址: <strong style="color:#e94560">http://192.168.5.1</strong></div>
        </div>

        <div class="card">
            <h2>🔧 系统</h2>
            <button class="btn btn-danger" onclick="restartDevice()">重启设备</button>
            <p style="color:#8892b0;margin-top:8px;font-size:12px">运行时间: <span id="uptime">0s</span></p>
        </div>
    </div>
    <div class="toast" id="toast"></div>
    <script>
        var curMode='sta';
        var startTime=Math.floor(Date.now()/1000);
        function showToast(msg,type){var t=document.getElementById('toast');t.textContent=msg;t.className='toast toast-'+type;t.style.display='block';setTimeout(function(){t.style.display='none'},4000)}
        function setActiveMode(m){
            curMode=m;
            var btnSta=document.getElementById('btnStaMode');
            var btnAp=document.getElementById('btnApMode');
            var secSta=document.getElementById('sectionSta');
            var secAp=document.getElementById('sectionAp');
            var desc=document.getElementById('modeDesc');
            var bad=document.getElementById('modeBadge');
            var cliBox=document.getElementById('apClientsBox');
            var stopBtn=document.getElementById('stopApBtn');
            var apBtn=document.getElementById('apBtn');
            if(m==='ap'){
                btnSta.className='mode-btn';
                btnAp.className='mode-btn active';
                secSta.className='card section-sta';
                secAp.className='card section-ap show';
                desc.textContent='AP 模式：ESP32 作为 WiFi 热点，其他设备可连接到此热点';
                bad.className='badge badge-ap';bad.textContent='AP';
                cliBox.style.display='block';
                stopBtn.style.display='block';
                apBtn.textContent='更新热点配置';
            }else{
                btnSta.className='mode-btn active';
                btnAp.className='mode-btn';
                secSta.className='card section-sta show';
                secAp.className='card section-ap';
                desc.textContent='STA 模式：连接到已有 WiFi 网络，通过 USB 共享上网';
                bad.className='badge badge-sta';bad.textContent='STA';
                cliBox.style.display='none';
                stopBtn.style.display='none';
                apBtn.textContent='启动热点';
            }
        }
        function switchMode(m){
            if(m===curMode)return;
            fetch('/api/wifi/mode',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mode='+m}).then(function(r){return r.json()}).then(function(d){
                if(d.success){setActiveMode(m);showToast('已切换到 '+m.toUpperCase()+' 模式','success');updateStatus()}else{showToast('切换失败','error')}
            });
        }
        function updateStatus(){
            fetch('/api/wifi/status').then(function(r){return r.json()}).then(function(d){
                document.getElementById('wifiState').textContent=d.state==='connected'?'已连接':d.state==='connecting'?'连接中...':'未连接';
                document.getElementById('wifiSSID').textContent=(d.mode==='ap'?d.ap_ssid:d.ssid)||'--';
                document.getElementById('wifiIP').textContent=d.ip||'--';
                if(d.rssi){
                    document.getElementById('wifiRSSI').textContent=d.rssi+' dBm';
                    var pct=Math.min(100,Math.max(0,(d.rssi+100)*2));
                    var bar=document.getElementById('rssiBar');
                    bar.style.width=pct+'%';
                    bar.className='rssi-fill '+(pct>60?'rssi-good':pct>30?'rssi-ok':'rssi-bad');
                }else{
                    document.getElementById('wifiRSSI').textContent='--';
                    document.getElementById('rssiBar').style.width='0%';
                }
                var dot=document.getElementById('statusDot');
                dot.className='dot '+(d.state==='connected'?'dot-online':d.state==='connecting'?'dot-connecting':'dot-offline');
                if(d.ssid)document.getElementById('ssid').value=d.ssid;
                if(d.password)document.getElementById('password').value=d.password;
                if(d.ap_ssid)document.getElementById('apSsid').value=d.ap_ssid;
                if(d.ap_password)document.getElementById('apPassword').value=d.ap_password;
                document.getElementById('apClients').textContent=d.ap_clients||'0';
                if(d.mode!==curMode)setActiveMode(d.mode);
            });
        }
        function connectWiFi(){
            var ssid=document.getElementById('ssid').value.trim();
            var pass=document.getElementById('password').value.trim();
            if(!ssid||!pass){showToast('请填写 SSID 和密码','error');return}
            if(pass.length<8){showToast('密码至少8位','error');return}
            var btn=document.getElementById('connectBtn');
            btn.disabled=true;btn.textContent='连接中...';
            var body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass);
            fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){return r.json()}).then(function(d){
                if(d.success){showToast('配置已保存，正在连接...','success');setTimeout(updateStatus,3000);setTimeout(updateStatus,8000)}
                else{showToast('连接失败: '+(d.message||'未知错误'),'error')}
                btn.disabled=false;btn.textContent='连接 WiFi';
            }).catch(function(e){showToast('请求失败','error');btn.disabled=false;btn.textContent='连接 WiFi'});
        }
        function startAP(){
            var ssid=document.getElementById('apSsid').value.trim();
            var pass=document.getElementById('apPassword').value.trim();
            if(!ssid||!pass){showToast('请填写 AP SSID 和密码','error');return}
            if(pass.length<8){showToast('密码至少8位','error');return}
            var btn=document.getElementById('apBtn');
            btn.disabled=true;btn.textContent='启动中...';
            var body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass);
            fetch('/api/wifi/ap/start',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){return r.json()}).then(function(d){
                if(d.success){showToast('AP 热点已启动','success');updateStatus()}
                else{showToast('启动失败','error')}
                btn.disabled=false;btn.textContent='更新热点配置';
            }).catch(function(e){showToast('请求失败','error');btn.disabled=false;btn.textContent='更新热点配置'});
        }
        function stopAP(){
            fetch('/api/wifi/ap/stop',{method:'POST'}).then(function(r){return r.json()}).then(function(d){
                if(d.success){showToast('AP 已停止','success');updateStatus()}
            });
        }
        function restartDevice(){if(!confirm('确定要重启设备吗？'))return;fetch('/api/restart',{method:'POST'}).then(function(){showToast('设备重启中...','success')})}
        function updateUptime(){var s=Math.floor(Date.now()/1000)-startTime;var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),sec=s%60;var str=d>0?d+'d '+h+'h':h>0?h+'h '+m+'m':m>0?m+'m '+sec+'s':sec+'s';document.getElementById('uptime').textContent=str}
        updateStatus();setInterval(updateStatus,5000);setInterval(updateUptime,1000)
    </script>
</body>
</html>
)rawliteral";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

static esp_err_t api_wifi_status_handler(httpd_req_t* req)
{
    char buffer[768];
    wifi_service_get_status_json(buffer, sizeof(buffer));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buffer, strlen(buffer));
    return ESP_OK;
}

static esp_err_t api_wifi_connect_handler(httpd_req_t* req)
{
    char ssid[64] = {0};
    char password[64] = {0};

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0 && ret < (int)sizeof(buf)) {
        buf[ret] = '\0';
        char* ssid_start = strstr(buf, "ssid=");
        if (ssid_start) {
            ssid_start += 5;
            char* end = strchr(ssid_start, '&');
            if (end) { size_t len = end - ssid_start; if (len < sizeof(ssid)) { memcpy(ssid, ssid_start, len); ssid[len] = '\0'; } }
            else { strncpy(ssid, ssid_start, sizeof(ssid) - 1); }
        }
        char* pass_start = strstr(buf, "password=");
        if (pass_start) { pass_start += 9; strncpy(password, pass_start, sizeof(password) - 1); }
        for (int i = (int)strlen(ssid) - 1; i >= 0 && ssid[i] == ' '; i--) ssid[i] = '\0';
        for (int i = (int)strlen(password) - 1; i >= 0 && password[i] == ' '; i--) password[i] = '\0';
        char* amp = strchr(ssid, '&'); if (amp) *amp = '\0';
        amp = strchr(password, '&'); if (amp) *amp = '\0';
    }

    if (strlen(ssid) == 0 || strlen(password) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"SSID or password empty\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    wifi_service_save_config(ssid, password);
    wifi_service_post_connect(ssid, password);
    ESP_LOGI(TAG, "WiFi connect queued: SSID=%s", ssid);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Connecting...\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_wifi_mode_handler(httpd_req_t* req)
{
    char buf[64] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        char* mode_start = strstr(buf, "mode=");
        if (mode_start) {
            mode_start += 5;
            if (strncmp(mode_start, "ap", 2) == 0) {
                wifi_service_post_start_ap(NULL, NULL);
            } else {
                wifi_service_post_stop_ap();
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_ap_start_handler(httpd_req_t* req)
{
    char ssid[64] = {0};
    char password[64] = {0};

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0 && ret < (int)sizeof(buf)) {
        buf[ret] = '\0';
        char* ssid_start = strstr(buf, "ssid=");
        if (ssid_start) {
            ssid_start += 5;
            char* end = strchr(ssid_start, '&');
            if (end) { size_t len = end - ssid_start; if (len < sizeof(ssid)) { memcpy(ssid, ssid_start, len); ssid[len] = '\0'; } }
            else { strncpy(ssid, ssid_start, sizeof(ssid) - 1); }
        }
        char* pass_start = strstr(buf, "password=");
        if (pass_start) { pass_start += 9; strncpy(password, pass_start, sizeof(password) - 1); }
    }

    if (strlen(ssid) > 0 && strlen(password) > 0) {
        wifi_service_post_start_ap(ssid, password);
    } else {
        wifi_service_post_start_ap(NULL, NULL);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_ap_stop_handler(httpd_req_t* req)
{
    wifi_service_post_stop_ap();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_restart_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

} // extern "C"

void web_server_start(WebServer* ws)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;

    httpd_uri_t root_uri       = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
    httpd_uri_t status_uri     = { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = api_wifi_status_handler, .user_ctx = NULL };
    httpd_uri_t connect_uri    = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_handler, .user_ctx = NULL };
    httpd_uri_t mode_uri       = { .uri = "/api/wifi/mode", .method = HTTP_POST, .handler = api_wifi_mode_handler, .user_ctx = NULL };
    httpd_uri_t ap_start_uri   = { .uri = "/api/wifi/ap/start", .method = HTTP_POST, .handler = api_ap_start_handler, .user_ctx = NULL };
    httpd_uri_t ap_stop_uri    = { .uri = "/api/wifi/ap/stop", .method = HTTP_POST, .handler = api_ap_stop_handler, .user_ctx = NULL };
    httpd_uri_t restart_uri    = { .uri = "/api/restart", .method = HTTP_POST, .handler = api_restart_handler, .user_ctx = NULL };

    if (httpd_start(&ws->server, &config) == ESP_OK) {
        httpd_register_uri_handler(ws->server, &root_uri);
        httpd_register_uri_handler(ws->server, &status_uri);
        httpd_register_uri_handler(ws->server, &connect_uri);
        httpd_register_uri_handler(ws->server, &mode_uri);
        httpd_register_uri_handler(ws->server, &ap_start_uri);
        httpd_register_uri_handler(ws->server, &ap_stop_uri);
        httpd_register_uri_handler(ws->server, &restart_uri);
        ESP_LOGI(TAG, "Web server started on port 80");
    }
}

void web_server_stop(WebServer* ws)
{
    if (ws->server) {
        httpd_stop(ws->server);
        ws->server = NULL;
    }
}