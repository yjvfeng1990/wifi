#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web_server.h"
#include "wifi_service.h"
#include "ble_pairing.h"
#include "wifi_now.h"

static const char* TAG = "WEB_SRV";

extern "C" {

static esp_err_t root_get_handler(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, "text/html");
    const char* html = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate">
    <title>ESP32-S3 WiFi Router v4</title>
    <style>
        *{margin:0;padding:0;box-sizing:border-box}
        body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#1a1a2e 0%,#16213e 50%,#0f3460 100%);min-height:100vh;padding:12px;color:#e0e0e0}
        .container{max-width:900px;margin:0 auto}
        .header{text-align:center;margin-bottom:clamp(14px,4vw,30px);padding:clamp(16px,5vw,30px) 0}
        .header h1{font-size:clamp(1.3em,4vw,2.2em);color:#e94560;margin-bottom:6px}
        .header p{color:#8892b0;font-size:clamp(0.85em,2.5vw,1.1em)}
        .card{background:rgba(255,255,255,0.05);border-radius:clamp(10px,3vw,16px);padding:clamp(12px,4vw,24px);margin-bottom:clamp(10px,3vw,20px);border:1px solid rgba(255,255,255,0.08);backdrop-filter:blur(10px)}
        .card h2{color:#e94560;margin-bottom:clamp(10px,3vw,16px);font-size:clamp(1em,3vw,1.2em);display:flex;align-items:center;gap:8px}
        .dot{width:10px;height:10px;border-radius:50%;display:inline-block;flex-shrink:0}
        .dot-online{background:#00ff88;box-shadow:0 0 8px #00ff8866}
        .dot-offline{background:#ff4444;box-shadow:0 0 8px #ff444466}
        .dot-connecting{background:#ffaa00;box-shadow:0 0 8px #ffaa0066;animation:pulse 1s infinite}
        .dot-active{background:#00c8ff;box-shadow:0 0 8px #00c8ff66}
        @keyframes pulse{0%,100%{opacity:1}50%{opacity:0.3}}
        .status-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:clamp(6px,2vw,12px);margin-top:clamp(8px,2vw,12px)}
        .status-item{background:rgba(255,255,255,0.03);padding:clamp(8px,2.5vw,14px);border-radius:clamp(6px,2vw,10px);border:1px solid rgba(255,255,255,0.05)}
        .status-label{font-size:clamp(9px,2vw,11px);color:#8892b0;text-transform:uppercase;letter-spacing:1px;margin-bottom:clamp(3px,1vw,6px)}
        .status-value{font-size:clamp(13px,3.5vw,17px);font-weight:600;color:#e0e0e0;word-break:break-all}
        .rssi-bar{height:3px;background:rgba(255,255,255,0.1);border-radius:2px;margin-top:clamp(3px,1vw,6px);overflow:hidden}
        .rssi-fill{height:100%;border-radius:2px;transition:width 0.5s,background 0.5s}
        .rssi-good{background:linear-gradient(90deg,#00ff88,#00cc6a)}
        .rssi-ok{background:linear-gradient(90deg,#ffaa00,#ff8800)}
        .rssi-bad{background:linear-gradient(90deg,#ff4444,#cc2222)}
        .form-group{margin-bottom:clamp(10px,3vw,14px)}
        label{display:block;margin-bottom:clamp(4px,1.5vw,6px);color:#8892b0;font-size:clamp(11px,2.5vw,13px);font-weight:500}
        input[type="text"],input[type="password"]{width:100%;padding:clamp(9px,2.5vw,12px) clamp(10px,3vw,15px);background:rgba(255,255,255,0.05);border:1px solid rgba(255,255,255,0.1);border-radius:clamp(6px,1.5vw,8px);color:#e0e0e0;font-size:clamp(13px,3vw,15px);transition:all 0.3s;-webkit-text-fill-color:#e0e0e0}
        input:focus{outline:none;border-color:#e94560;box-shadow:0 0 0 3px rgba(233,69,96,0.15)}
        input:-webkit-autofill,input:-webkit-autofill:hover,input:-webkit-autofill:focus,input:-webkit-autofill:active{-webkit-box-shadow:0 0 0 30px #1a1f36 inset !important;-webkit-text-fill-color:#e0e0e0 !important;transition:background-color 5000s ease-in-out 0s}
        .btn{width:100%;padding:clamp(10px,2.5vw,13px);border:none;border-radius:clamp(6px,1.5vw,8px);font-size:clamp(12px,2.5vw,15px);font-weight:600;cursor:pointer;transition:all 0.3s;text-transform:uppercase;letter-spacing:0.5px}
        .btn-primary{background:linear-gradient(135deg,#e94560,#c23152);color:white}
        .btn-primary:hover{transform:translateY(-1px);box-shadow:0 6px 20px rgba(233,69,96,0.3)}
        .btn-primary:disabled{opacity:0.5;cursor:not-allowed;transform:none}
        .btn-outline{background:transparent;color:#e94560;border:2px solid #e94560;margin-top:clamp(4px,1.5vw,8px)}
        .btn-outline:hover{background:rgba(233,69,96,0.1)}
        .btn-ap-start{background:linear-gradient(135deg,#00c8ff,#0090cc);color:white;margin-bottom:clamp(4px,1.5vw,8px)}
        .btn-ap-start:hover{transform:translateY(-1px);box-shadow:0 6px 20px rgba(0,200,255,0.3)}
        .btn-danger{background:rgba(255,68,68,0.15);color:#ff6666;margin-top:clamp(4px,1.5vw,8px)}
        .btn-danger:hover{background:rgba(255,68,68,0.25)}
        .toast{position:fixed;top:10px;right:10px;padding:clamp(10px,2.5vw,14px) clamp(12px,3vw,20px);border-radius:clamp(6px,2vw,10px);color:white;font-weight:500;z-index:999;animation:slideIn 0.3s ease;display:none;font-size:clamp(12px,2.5vw,14px)}
        .toast-success{background:rgba(0,255,136,0.15);border:1px solid rgba(0,255,136,0.3);color:#00ff88}
        .toast-error{background:rgba(255,68,68,0.15);border:1px solid rgba(255,68,68,0.3);color:#ff6666}
        @keyframes slideIn{from{transform:translateX(100px);opacity:0}to{transform:translateX(0);opacity:1}}
        .ip-box{background:rgba(233,69,96,0.1);border:1px solid rgba(233,69,96,0.2);padding:clamp(7px,2vw,10px) clamp(10px,3vw,15px);border-radius:clamp(6px,1.5vw,8px);font-family:monospace;margin-top:clamp(4px,1.5vw,8px);font-size:clamp(12px,2.5vw,14px)}
        .badge{display:inline-block;padding:2px 8px;border-radius:20px;font-size:clamp(9px,2vw,11px);font-weight:600}
        .badge-ap{background:rgba(0,200,255,0.15);color:#00c8ff}
        .badge-sta{background:rgba(0,255,136,0.15);color:#00ff88}
        .badge-apsta{background:rgba(200,100,255,0.15);color:#c864ff}
        .mode-row{display:flex;gap:clamp(4px,1.5vw,8px);align-items:center;margin-bottom:clamp(6px,2vw,10px);flex-wrap:wrap}
        .thr-col{flex:1;min-width:0;text-align:center}
        .thr-title{font-size:clamp(9px,2vw,12px);color:#8892b0;margin-bottom:clamp(4px,1.5vw,10px);text-transform:uppercase;letter-spacing:1px}
        .thr-row{display:flex;justify-content:center;gap:clamp(8px,2.5vw,16px)}
        .thr-dir{font-size:clamp(10px,2vw,13px);margin-bottom:clamp(2px,0.5vw,4px)}
        .thr-val{font-size:clamp(13px,3.5vw,16px);font-weight:600;font-family:monospace;transition:color 0.5s}
        .thr-bar-wrap{height:2px;background:rgba(255,255,255,0.08);border-radius:2px;margin:clamp(4px,1.5vw,8px) 0;overflow:hidden}
        .thr-bar{height:100%;border-radius:2px;transition:width 0.8s ease}
        .thr-rx{background:linear-gradient(90deg,#00ff88,#00cc6a)}
        .thr-tx{background:linear-gradient(90deg,#e94560,#ff6b6b)}
        .thr-sep{width:1px;background:rgba(255,255,255,0.1);margin:0 clamp(4px,1.5vw,8px);flex-shrink:0}
        .thr-grid{display:grid;grid-template-columns:1fr;gap:clamp(8px,2.5vw,16px)}
        .dhcp-table{width:100%;border-collapse:collapse;font-size:clamp(11px,2.5vw,13px)}
        .dhcp-table th{text-align:left;padding:clamp(6px,2vw,10px) clamp(8px,2.5vw,12px);color:#8892b0;font-weight:500;font-size:clamp(9px,2vw,11px);text-transform:uppercase;letter-spacing:1px;border-bottom:1px solid rgba(255,255,255,0.08)}
        .dhcp-table td{padding:clamp(6px,2vw,10px) clamp(8px,2.5vw,12px);border-bottom:1px solid rgba(255,255,255,0.03);font-family:monospace;word-break:break-all}
        .dhcp-table tr:hover{background:rgba(255,255,255,0.02)}
        .dhcp-empty{text-align:center;color:#555;padding:clamp(12px,3vw,20px);font-size:clamp(11px,2.5vw,13px)}
        .dhcp-badge{display:inline-block;padding:2px 6px;border-radius:10px;font-size:clamp(8px,2vw,10px);font-weight:600}
        .dhcp-badge-ap{background:rgba(0,200,255,0.15);color:#00c8ff}
        .dhcp-badge-usb{background:rgba(255,170,0,0.15);color:#ffaa00}
        .scan-btn{background:rgba(0,200,255,0.15);color:#00c8ff;border:1px solid rgba(0,200,255,0.3);padding:clamp(7px,2vw,10px) clamp(12px,3vw,18px);border-radius:clamp(6px,1.5vw,8px);font-size:clamp(11px,2.5vw,13px);font-weight:600;cursor:pointer;transition:all 0.3s;margin-bottom:clamp(8px,2.5vw,12px);display:inline-flex;align-items:center;gap:6px}
        .scan-btn:hover{background:rgba(0,200,255,0.25)}
        .scan-btn:disabled{opacity:0.4;cursor:not-allowed}
        .scan-spinner{display:inline-block;width:clamp(11px,2.5vw,14px);height:clamp(11px,2.5vw,14px);border:2px solid rgba(0,200,255,0.2);border-top:2px solid #00c8ff;border-radius:50%;animation:spin 0.8s linear infinite}
        @keyframes spin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
        .scan-list{max-height:clamp(180px,40vw,260px);overflow-y:auto;margin-top:clamp(6px,1.5vw,8px)}
        .scan-item{display:flex;align-items:center;padding:clamp(7px,2vw,10px) clamp(8px,2.5vw,14px);border-radius:clamp(6px,1.5vw,8px);cursor:pointer;transition:background 0.2s;border:1px solid transparent}
        .scan-item:hover{background:rgba(0,200,255,0.08);border-color:rgba(0,200,255,0.15)}
        .scan-item-icon{width:clamp(26px,6vw,32px);height:clamp(26px,6vw,32px);border-radius:50%;display:flex;align-items:center;justify-content:center;margin-right:clamp(8px,2.5vw,12px);font-size:clamp(11px,2.5vw,14px);flex-shrink:0}
        .scan-item-icon-secure{background:rgba(0,255,136,0.15);color:#00ff88}
        .scan-item-icon-open{background:rgba(255,170,0,0.15);color:#ffaa00}
        .scan-item-info{flex:1;min-width:0}
        .scan-item-ssid{font-weight:500;font-size:clamp(12px,2.5vw,14px);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
        .scan-item-meta{font-size:clamp(9px,2vw,11px);color:#8892b0;margin-top:1px}
        .ble-toggle-btn{display:inline-flex;align-items:center;gap:8px;padding:clamp(8px,2vw,12px) clamp(14px,3vw,20px);border:none;border-radius:clamp(6px,1.5vw,8px);font-size:clamp(11px,2.5vw,13px);font-weight:600;cursor:pointer;transition:all 0.3s;text-transform:uppercase;letter-spacing:0.5px}
        .ble-toggle-on{background:linear-gradient(135deg,#00ff88,#00cc6a);color:#111}
        .ble-toggle-off{background:rgba(255,68,68,0.2);color:#ff6666;border:1px solid rgba(255,68,68,0.3)}
        .ble-toggle-on:hover{box-shadow:0 4px 16px rgba(0,255,136,0.3)}
        .ble-toggle-off:hover{background:rgba(255,68,68,0.3)}
        .mac-box{background:rgba(0,200,255,0.1);border:1px solid rgba(0,200,255,0.2);padding:clamp(6px,1.5vw,10px) clamp(10px,3vw,15px);border-radius:clamp(6px,1.5vw,8px);font-family:monospace;font-size:clamp(12px,2.5vw,14px);color:#00c8ff;word-break:break-all}
        .btn-pair{background:rgba(0,200,255,0.15);color:#00c8ff;border:1px solid rgba(0,200,255,0.3);padding:clamp(4px,1.5vw,6px) clamp(10px,3vw,14px);border-radius:clamp(4px,1vw,6px);font-size:clamp(10px,2vw,12px);font-weight:600;cursor:pointer;transition:all 0.2s;white-space:nowrap}
        .btn-pair:hover{background:rgba(0,200,255,0.25)}
        .btn-mini{background:rgba(255,68,68,0.15);color:#ff6666;border:none;padding:2px 8px;border-radius:4px;font-size:10px;cursor:pointer}
        .btn-mini:hover{background:rgba(255,68,68,0.3)}
        .rssi-small{font-size:clamp(9px,2vw,11px);color:#8892b0}
        @media(min-width:640px){
            .thr-grid{grid-template-columns:repeat(auto-fit,minmax(200px,1fr))}
            .status-grid{grid-template-columns:repeat(auto-fit,minmax(160px,1fr))}
            .scan-list{max-height:260px}
        }
        @media(max-width:600px){.header h1{font-size:1.3em}.status-grid{grid-template-columns:1fr 1fr}.thr-grid{grid-template-columns:1fr}}
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>ESP32-S3 WiFi Manager</h1>
            <p>USB Network + WiFi AP/STA Dual Mode</p>
        </div>

        <div class="card">
            <h2>WiFi State</h2>
            <div class="mode-row">
                <span class="badge badge-sta" id="modeBadge">STA</span>
            </div>
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:16px">
                <div>
                    <h2 style="font-size:0.95em;margin-bottom:10px;color:#00ff88">
                        <span class="dot dot-offline" id="staDot"></span> STA Client
                    </h2>
                    <div class="status-grid" style="grid-template-columns:1fr 1fr">
                        <div class="status-item"><div class="status-label">Status</div><div class="status-value" id="staState">--</div></div>
                        <div class="status-item"><div class="status-label">SSID</div><div class="status-value" id="staSSID">--</div></div>
                        <div class="status-item"><div class="status-label">IP</div><div class="status-value" id="staIP">--</div></div>
                        <div class="status-item">
                            <div class="status-label">RSSI</div>
                            <div class="status-value" id="staRSSI">--</div>
                            <div class="rssi-bar"><div class="rssi-fill" id="rssiBar" style="width:0%"></div></div>
                        </div>
                    </div>
                </div>
                <div>
                    <h2 style="font-size:0.95em;margin-bottom:10px;color:#00c8ff">
                        <span class="dot dot-offline" id="apDot"></span> AP Hotspot
                    </h2>
                    <div class="status-grid" style="grid-template-columns:1fr 1fr">
                        <div class="status-item"><div class="status-label">State</div><div class="status-value" id="apState">Inactive</div></div>
                        <div class="status-item"><div class="status-label">SSID</div><div class="status-value" id="apSSID">--</div></div>
                        <div class="status-item"><div class="status-label">IP</div><div class="status-value">192.168.4.1</div></div>
                        <div class="status-item"><div class="status-label">Clients</div><div class="status-value" id="apClients">0</div></div>
                    </div>
                </div>
            </div>
        </div>

        <div class="card">
            <h2>Real-time Throughput (bytes/s)</h2>
            <div class="thr-grid">
                <div style="background:rgba(0,255,136,0.04);border-radius:12px;padding:16px;border:1px solid rgba(0,255,136,0.1)">
                    <div class="thr-title" style="color:#00ff88">STA (WAN)</div>
                    <div style="display:flex;justify-content:space-around;gap:8px">
                        <div style="text-align:center;flex:1">
                            <div class="thr-dir" style="color:#00ff88">Download</div>
                            <div class="thr-val" id="staRX">0</div>
                            <div class="thr-bar-wrap"><div class="thr-bar thr-rx" id="staRXbar" style="width:0%"></div></div>
                        </div>
                        <div style="text-align:center;flex:1">
                            <div class="thr-dir" style="color:#e94560">Upload</div>
                            <div class="thr-val" id="staTX">0</div>
                            <div class="thr-bar-wrap"><div class="thr-bar thr-tx" id="staTXbar" style="width:0%"></div></div>
                        </div>
                    </div>
                </div>
                <div style="background:rgba(0,200,255,0.04);border-radius:12px;padding:16px;border:1px solid rgba(0,200,255,0.1)">
                    <div class="thr-title" style="color:#00c8ff">AP (LAN)</div>
                    <div style="display:flex;justify-content:space-around;gap:8px">
                        <div style="text-align:center;flex:1">
                            <div class="thr-dir" style="color:#00ff88">Download</div>
                            <div class="thr-val" id="apRX">0</div>
                            <div class="thr-bar-wrap"><div class="thr-bar thr-rx" id="apRXbar" style="width:0%"></div></div>
                        </div>
                        <div style="text-align:center;flex:1">
                            <div class="thr-dir" style="color:#e94560">Upload</div>
                            <div class="thr-val" id="apTX">0</div>
                            <div class="thr-bar-wrap"><div class="thr-bar thr-tx" id="apTXbar" style="width:0%"></div></div>
                        </div>
                    </div>
                </div>
                <div style="background:rgba(255,170,0,0.04);border-radius:12px;padding:16px;border:1px solid rgba(255,170,0,0.1)">
                    <div class="thr-title" style="color:#ffaa00">USB (LAN)</div>
                    <div style="display:flex;justify-content:space-around;gap:8px">
                        <div style="text-align:center;flex:1">
                            <div class="thr-dir" style="color:#00ff88">Download</div>
                            <div class="thr-val" id="usbRX">0</div>
                            <div class="thr-bar-wrap"><div class="thr-bar thr-rx" id="usbRXbar" style="width:0%"></div></div>
                        </div>
                        <div style="text-align:center;flex:1">
                            <div class="thr-dir" style="color:#e94560">Upload</div>
                            <div class="thr-val" id="usbTX">0</div>
                            <div class="thr-bar-wrap"><div class="thr-bar thr-tx" id="usbTXbar" style="width:0%"></div></div>
                        </div>
                    </div>
                </div>
            </div>
        </div>

        <div class="card">
            <h2><span class="dot dot-offline" id="staFormDot"></span> STA — Connect to WiFi</h2>
            <button class="scan-btn" id="scanBtn" onclick="startScan()">Scan Networks</button>
            <div id="scanContainer" style="display:none">
                <div style="display:flex;align-items:center;gap:8px;margin-bottom:10px">
                    <span class="scan-spinner" id="scanSpinner"></span>
                    <span style="font-size:13px;color:#8892b0" id="scanStatus">Scanning...</span>
                </div>
                <div class="scan-list" id="scanList"></div>
            </div>
            <form id="wifiForm" onsubmit="return false;">
                <div class="form-group"><label for="ssid">WiFi SSID</label><input type="text" id="ssid" name="ssid" placeholder="WiFi name" required></div>
                <div class="form-group"><label for="password">WiFi Password</label><input type="password" id="password" name="password" placeholder="Password (min 8 chars)" minlength="8" required></div>
                <button type="submit" class="btn btn-primary" id="connectBtn" onclick="connectWiFi()">Connect</button>
            </form>
        </div>

        <div class="card">
            <h2><span class="dot dot-offline" id="apFormDot"></span> AP — Hotspot Settings</h2>
            <form id="apForm" onsubmit="return false;">
                <div class="form-group"><label for="apSsid">AP SSID</label><input type="text" id="apSsid" name="apSsid" required></div>
                <div class="form-group"><label for="apPassword">AP Password</label><input type="text" id="apPassword" name="apPassword" minlength="8" required></div>
                <button type="submit" class="btn btn-ap-start" id="apBtn" onclick="toggleAP()">Start Hotspot</button>
                <button type="button" class="btn btn-outline" id="stopApBtn" onclick="stopAP()" style="display:none">Stop Hotspot</button>
            </form>
        </div>

        <div class="card">
            <h2>USB Network</h2>
            <p style="color:#8892b0;margin-bottom:8px">Plug USB to computer for automatic Ethernet device recognition</p>
            <div class="ip-box">ESP32 USB Management: <strong style="color:#e94560">http://192.168.5.1</strong></div>
        </div>

        <div class="card">
            <h2>DHCP Clients</h2>
            <div id="dhcpTableContainer">
                <div class="dhcp-empty">No clients connected</div>
            </div>
        </div>

        <div class="card">
            <h2><span class="dot dot-offline" id="bleDot"></span> BLE Pairing</h2>
            <div style="margin-bottom:12px">
                <div class="status-label" style="margin-bottom:4px">ESP-NOW MAC</div>
                <div class="mac-box" id="nowMac">--</div>
            </div>
            <div style="display:flex;gap:8px;align-items:flex-end;flex-wrap:wrap;margin-bottom:12px">
                <div class="form-group" style="flex:1;min-width:120px;margin-bottom:0">
                    <label for="bleName">Device Name</label>
                    <input type="text" id="bleName" placeholder="ESP32-S3-NOW" style="font-size:clamp(12px,2.5vw,14px)">
                </div>
                <div>
                    <button class="ble-toggle-btn ble-toggle-off" id="bleAdvBtn" onclick="toggleAdvertise()">Start BLE</button>
                </div>
            </div>
            <div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">
                <button class="scan-btn" id="bleScanBtn" onclick="startBleScan()">Scan BLE Devices</button>
                <span class="scan-spinner" id="bleScanSpinner" style="display:none"></span>
                <span style="font-size:12px;color:#8892b0" id="bleScanStatus"></span>
            </div>
            <div class="scan-list" id="bleDevList" style="max-height:200px">
                <div class="dhcp-empty">No devices discovered</div>
            </div>
        </div>

        <div class="card">
            <h2>ESP-NOW Peer List</h2>
            <div id="peerTableContainer">
                <div class="dhcp-empty">No peers paired</div>
            </div>
        </div>

        <div class="card">
            <h2>System</h2>
            <button class="btn btn-danger" onclick="restartDevice()">Restart Device</button>
            <p style="color:#8892b0;margin-top:8px;font-size:12px">Uptime: <span id="uptime">0s</span></p>
        </div>
    </div>
    <div class="toast" id="toast"></div>
    <script>
        var startTime=Math.floor(Date.now()/1000);
        var maxBps=1;

        function showToast(msg,type){
            var t=document.getElementById('toast');
            t.textContent=msg;
            t.className='toast toast-'+type;
            t.style.display='block';
            setTimeout(function(){t.style.display='none'},4000);
        }

        function fmtBps(bps){
            if(!bps||bps===0)return'0';
            if(bps>=1048576)return(bps/1048576).toFixed(1)+' M/s';
            if(bps>=1024)return(bps/1024).toFixed(0)+' K/s';
            return bps+' B/s';
        }

        function updThr(id,bps,barId){
            var el=document.getElementById(id);
            var old=parseFloat(el.textContent)||0;
            el.textContent=fmtBps(bps);
            if(bps>maxBps)maxBps=bps;
            var pct=Math.min(100,(bps/Math.max(maxBps,1))*100);
            document.getElementById(barId).style.width=pct+'%';
            if(bps>100000)el.style.color='#ff6b6b';
            else if(bps>10000)el.style.color='#ffaa00';
            else el.style.color='#8892b0';
        }

        function updateUI(d){
            var staState=d.sta_state||'disconnected';
            document.getElementById('staState').textContent=
                staState==='connected'?'Connected':
                staState==='connecting'?'Connecting...':'Disconnected';

            var staDot=document.getElementById('staDot');
            staDot.className='dot '+(staState==='connected'?'dot-online':
                staState==='connecting'?'dot-connecting':'dot-offline');

            document.getElementById('staSSID').textContent=d.sta_ssid||'--';
            document.getElementById('staIP').textContent=d.sta_ip||'--';

            if(d.sta_rssi){
                document.getElementById('staRSSI').textContent=d.sta_rssi+' dBm';
                var pct=Math.min(100,Math.max(0,(d.sta_rssi+100)*2));
                var bar=document.getElementById('rssiBar');
                bar.style.width=pct+'%';
                bar.className='rssi-fill '+(pct>60?'rssi-good':pct>30?'rssi-ok':'rssi-bad');
            }else{
                document.getElementById('staRSSI').textContent='--';
                document.getElementById('rssiBar').style.width='0%';
            }

            var apActive=d.ap_active===true;
            document.getElementById('apState').textContent=apActive?'Active':'Inactive';
            var apDot=document.getElementById('apDot');
            apDot.className='dot '+(apActive?'dot-active':'dot-offline');
            document.getElementById('apSSID').textContent=d.ap_ssid||'--';
            document.getElementById('apClients').textContent=d.ap_clients||'0';

            var apFormDot=document.getElementById('apFormDot');
            apFormDot.className='dot '+(apActive?'dot-active':'dot-offline');

            var stopBtn=document.getElementById('stopApBtn');
            var apBtn=document.getElementById('apBtn');
            if(apActive){
                stopBtn.style.display='block';
                apBtn.textContent='Update Hotspot';
                apBtn.className='btn btn-primary';
            }else{
                stopBtn.style.display='none';
                apBtn.textContent='Start Hotspot';
                apBtn.className='btn btn-ap-start';
            }

            var mode=d.mode||'sta';
            var badge=document.getElementById('modeBadge');
            if(mode==='apsta'){badge.className='badge badge-apsta';badge.textContent='AP+STA';}
            else if(mode==='ap'){badge.className='badge badge-ap';badge.textContent='AP';}
            else{badge.className='badge badge-sta';badge.textContent='STA';}

            updThr('staRX',d.sta_down_bps||0,'staRXbar');
            updThr('staTX',d.sta_up_bps||0,'staTXbar');
            updThr('apRX',d.ap_down_bps||0,'apRXbar');
            updThr('apTX',d.ap_up_bps||0,'apTXbar');
            updThr('usbRX',d.usb_down_bps||0,'usbRXbar');
            updThr('usbTX',d.usb_up_bps||0,'usbTXbar');

            var elSsid=document.getElementById('ssid');
            var elPass=document.getElementById('password');
            if(d.sta_ssid && !elSsid.value)elSsid.value=d.sta_ssid;
            if(d.sta_password && !elPass.value)elPass.value=d.sta_password;
            var elApSsid=document.getElementById('apSsid');
            var elApPass=document.getElementById('apPassword');
            if(d.ap_ssid && !elApSsid.value)elApSsid.value=d.ap_ssid;
            if(d.ap_password && !elApPass.value)elApPass.value=d.ap_password;
        }

        function updateStatus(){
            fetch('/api/wifi/status').then(function(r){return r.json()}).then(updateUI);
        }

        function updateDhcpClients(){
            fetch('/api/dhcp/clients').then(function(r){return r.json()}).then(function(data){
                var container=document.getElementById('dhcpTableContainer');
                if(!data||data.length===0){
                    container.innerHTML='<div class="dhcp-empty">No clients connected</div>';
                    return;
                }
                var html='<table class="dhcp-table"><thead><tr><th>Interface</th><th>MAC Address</th><th>IP Address</th></tr></thead><tbody>';
                for(var i=0;i<data.length;i++){
                    var c=data[i];
                    var iface=c.source==='ap'?'<span class="dhcp-badge dhcp-badge-ap">AP</span>':'<span class="dhcp-badge dhcp-badge-usb">USB</span>';
                    html+='<tr><td>'+iface+'</td><td>'+c.mac+'</td><td>'+c.ip+'</td></tr>';
                }
                html+='</tbody></table>';
                container.innerHTML=html;
            });
        }

        function rssiToStr(r){
            if(r>=-50)return'Excellent';
            if(r>=-65)return'Good';
            if(r>=-75)return'Fair';
            return'Weak';
        }

        var scanTimer=null;
        function startScan(){
            var btn=document.getElementById('scanBtn');
            var container=document.getElementById('scanContainer');
            var statusEl=document.getElementById('scanStatus');
            var spinner=document.getElementById('scanSpinner');
            var list=document.getElementById('scanList');
            if(scanTimer){clearInterval(scanTimer);scanTimer=null;}
            btn.disabled=true;
            btn.textContent='Scanning...';
            container.style.display='block';
            spinner.style.display='inline-block';
            statusEl.textContent='Scanning for WiFi networks...';
            list.innerHTML='';
            function poll(){
                fetch('/api/wifi/scan?_='+Date.now()).then(function(r){return r.json()}).then(function(data){
                    if(data.scanning){
                        return;
                    }
                    clearInterval(scanTimer);
                    scanTimer=null;
                    spinner.style.display='none';
                    btn.disabled=false;
                    btn.textContent='Scan Networks';
                    if(data.error){
                        statusEl.textContent='Scan failed: '+data.error;
                        return;
                    }
                    if(!data.results||data.results.length===0){
                        statusEl.textContent='No networks found';
                        return;
                    }
                    statusEl.textContent=data.results.length+' network(s) found';
                    var html='';
                    for(var i=0;i<data.results.length;i++){
                        var ap=data.results[i];
                        var iconCls=ap.auth==='secure'?'scan-item-icon-secure':'scan-item-icon-open';
                        var icon=ap.auth==='secure'?'&#128274;':'&#128275;';
                        html+='<div class="scan-item" onclick="selectSSID(this.getAttribute(\'data-ssid\'))" data-ssid="'+ap.ssid.replace(/"/g,'&quot;').replace(/&/g,'&amp;')+'">';
                        html+='<div class="scan-item-icon '+iconCls+'">'+icon+'</div>';
                        html+='<div class="scan-item-info">';
                        html+='<div class="scan-item-ssid">'+ap.ssid+'</div>';
                        html+='<div class="scan-item-meta">CH '+ap.channel+' &middot; '+rssiToStr(ap.rssi)+' ('+ap.rssi+' dBm)</div>';
                        html+='</div></div>';
                    }
                    list.innerHTML=html;
                }).catch(function(){
                    clearInterval(scanTimer);scanTimer=null;
                    spinner.style.display='none';
                    btn.disabled=false;
                    btn.textContent='Scan Networks';
                    statusEl.textContent='Scan failed';
                });
            }
            poll();
            scanTimer=setInterval(poll,500);
        }

        function selectSSID(ssid){
            document.getElementById('ssid').value=ssid;
            document.getElementById('scanContainer').style.display='none';
            showToast('Selected: '+ssid,'success');
        }

        function connectWiFi(){
            var ssid=document.getElementById('ssid').value.trim();
            var pass=document.getElementById('password').value.trim();
            if(!ssid||!pass){showToast('Please enter SSID and password','error');return}
            if(pass.length<8){showToast('Password must be at least 8 characters','error');return}
            var btn=document.getElementById('connectBtn');
            btn.disabled=true;btn.textContent='Connecting...';
            var body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass);
            fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){return r.json()}).then(function(d){
                if(d.success){showToast('Config saved, connecting...','success');setTimeout(updateStatus,3000);setTimeout(updateStatus,8000)}
                else{showToast('Connect failed: '+(d.message||'unknown'),'error')}
                btn.disabled=false;btn.textContent='Connect';
            }).catch(function(e){showToast('Request failed','error');btn.disabled=false;btn.textContent='Connect'});
        }

        function toggleAP(){
            var ssid=document.getElementById('apSsid').value.trim();
            var pass=document.getElementById('apPassword').value.trim();
            if(!ssid||!pass){showToast('Please enter AP SSID and password','error');return}
            if(pass.length<8){showToast('Password must be at least 8 characters','error');return}
            var btn=document.getElementById('apBtn');
            btn.disabled=true;btn.textContent='Starting...';
            var body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass);
            fetch('/api/wifi/ap/start',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){return r.json()}).then(function(d){
                if(d.success){showToast('Hotspot started','success');updateStatus()}
                else{showToast('Start failed','error')}
                btn.disabled=false;
            }).catch(function(e){showToast('Request failed','error');btn.disabled=false});
        }

        function stopAP(){
            fetch('/api/wifi/ap/stop',{method:'POST'}).then(function(r){return r.json()}).then(function(d){
                if(d.success){showToast('Hotspot stopped','success');updateStatus()}
            });
        }

        function restartDevice(){
            if(!confirm('Restart device?'))return;
            fetch('/api/restart',{method:'POST'}).then(function(){showToast('Restarting...','success')});
        }

        function updateNowMac(){
            fetch('/api/now/mac').then(function(r){return r.json()}).then(function(d){
                document.getElementById('nowMac').textContent=d.mac||'--';
            });
        }

        function updateBleStatus(){
            fetch('/api/ble/status').then(function(r){return r.json()}).then(function(d){
                var btn=document.getElementById('bleAdvBtn');
                var dot=document.getElementById('bleDot');
                if(d.advertising){
                    btn.textContent='Stop BLE';
                    btn.className='ble-toggle-btn ble-toggle-on';
                    dot.className='dot dot-online';
                }else{
                    btn.textContent='Start BLE';
                    btn.className='ble-toggle-btn ble-toggle-off';
                    dot.className='dot dot-offline';
                }
                if(d.scanning){
                    document.getElementById('bleScanSpinner').style.display='inline-block';
                }
            });
        }

        function toggleAdvertise(){
            fetch('/api/ble/status').then(function(r){return r.json()}).then(function(d){
                var name=document.getElementById('bleName').value.trim();
                var enable=d.advertising?'0':'1';
                fetch('/api/ble/advertise',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'enable='+enable+'&name='+encodeURIComponent(name)}).then(function(r){return r.json()}).then(function(){
                    updateBleStatus();
                    showToast(d.advertising?'BLE stopped':'BLE advertising started','success');
                });
            });
        }

        var bleScanTimer=null;
        function startBleScan(){
            var btn=document.getElementById('bleScanBtn');
            if(bleScanTimer){clearInterval(bleScanTimer);bleScanTimer=null;}
            btn.disabled=true;
            btn.textContent='Scanning...';
            document.getElementById('bleScanSpinner').style.display='inline-block';
            document.getElementById('bleScanStatus').textContent='Scanning for BLE devices...';
            document.getElementById('bleDevList').innerHTML='<div class="dhcp-empty">Scanning...</div>';
            fetch('/api/ble/scan',{method:'POST'});
            function poll(){
                fetch('/api/ble/devices?_='+Date.now()).then(function(r){return r.json()}).then(function(data){
                    var html='';
                    if(data.devices&&data.devices.length>0){
                        for(var i=0;i<data.devices.length;i++){
                            var d=data.devices[i];
                            html+='<div class="scan-item">';
                            html+='<div class="scan-item-icon scan-item-icon-secure">&#128206;</div>';
                            html+='<div class="scan-item-info">';
                            html+='<div class="scan-item-ssid">'+d.name+'</div>';
                            html+='<div class="scan-item-meta">NOW: '+d.now_mac+' &middot; RSSI: '+d.rssi+' dBm</div>';
                            html+='</div>';
                            html+='<button class="btn-pair" onclick="pairDevice('+d.index+')">Pair</button>';
                            html+='</div>';
                        }
                    }else if(!data.scanning){
                        html='<div class="dhcp-empty">No devices discovered</div>';
                    }
                    if(html)document.getElementById('bleDevList').innerHTML=html;
                    if(!data.scanning){
                        clearInterval(bleScanTimer);bleScanTimer=null;
                        btn.disabled=false;btn.textContent='Scan BLE Devices';
                        document.getElementById('bleScanSpinner').style.display='none';
                        document.getElementById('bleScanStatus').textContent=data.devices&&data.devices.length>0?data.devices.length+' device(s) found':'No devices found';
                        updateNowPeers();
                    }
                });
            }
            poll();
            bleScanTimer=setInterval(poll,800);
        }

        function pairDevice(index){
            fetch('/api/ble/pair',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'index='+index}).then(function(r){return r.json()}).then(function(){
                showToast('Device paired','success');
                setTimeout(updateNowPeers,500);
            });
        }

        function updateNowPeers(){
            fetch('/api/now/peers').then(function(r){return r.json()}).then(function(data){
                var container=document.getElementById('peerTableContainer');
                if(!data||data.length===0){
                    container.innerHTML='<div class="dhcp-empty">No peers paired</div>';
                    return;
                }
                var html='<table class="dhcp-table"><thead><tr><th>Name</th><th>ESP-NOW MAC</th><th>Channel</th><th></th></tr></thead><tbody>';
                for(var i=0;i<data.length;i++){
                    var p=data[i];
                    html+='<tr><td>'+p.name+'</td><td>'+p.mac+'</td><td>'+p.channel+'</td>';
                    html+='<td><button class="btn-mini" onclick="removePeer(\''+p.mac+'\')">Remove</button></td></tr>';
                }
                html+='</tbody></table>';
                container.innerHTML=html;
            });
        }

        function removePeer(mac){
            fetch('/api/now/peer/remove',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mac='+encodeURIComponent(mac)}).then(function(r){return r.json()}).then(function(){
                showToast('Peer removed','success');
                updateNowPeers();
            });
        }

        function updateUptime(){
            var s=Math.floor(Date.now()/1000)-startTime;
            var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),sec=s%60;
            var str=d>0?d+'d '+h+'h':h>0?h+'h '+m+'m':m>0?m+'m '+sec+'s':sec+'s';
            document.getElementById('uptime').textContent=str;
        }

        updateStatus();
        setInterval(updateStatus,2000);
        updateDhcpClients();
        setInterval(updateDhcpClients,5000);
        updateNowMac();
        updateBleStatus();
        setInterval(updateBleStatus,2000);
        updateNowPeers();
        setInterval(updateNowPeers,3000);
        setInterval(updateUptime,1000);
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
    char buffer[1024];
    wifi_service_get_status_json(buffer, sizeof(buffer));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buffer, strlen(buffer));
    return ESP_OK;
}

static bool parse_urlencoded(const char* buf, int len,
                              char* ssid_out, size_t ssid_size,
                              char* pass_out, size_t pass_size)
{
    bool found = false;

    const char* ssid_start = strstr(buf, "ssid=");
    if (ssid_start) {
        ssid_start += 5;
        const char* end = strchr(ssid_start, '&');
        if (end) {
            size_t copy_len = (size_t)(end - ssid_start);
            if (copy_len >= ssid_size) copy_len = ssid_size - 1;
            memcpy(ssid_out, ssid_start, copy_len);
            ssid_out[copy_len] = '\0';
        } else {
            strncpy(ssid_out, ssid_start, ssid_size - 1);
        }
        found = true;
    }

    const char* pass_start = strstr(buf, "password=");
    if (pass_start) {
        pass_start += 9;
        strncpy(pass_out, pass_start, pass_size - 1);
    }

    return found;
}

static esp_err_t api_wifi_connect_handler(httpd_req_t* req)
{
    char ssid[64]     = {0};
    char password[64] = {0};

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0 && ret < (int)sizeof(buf)) {
        buf[ret] = '\0';
        parse_urlencoded(buf, ret, ssid, sizeof(ssid), password, sizeof(password));
    }

    if (strlen(ssid) == 0 || strlen(password) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"message\":\"SSID or password empty\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    wifi_service_save_config(ssid, password);
    wifi_service_post_connect(ssid, password);
    ESP_LOGI(TAG, "WiFi connect queued: SSID=%s", ssid);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Connecting...\"}",
                    HTTPD_RESP_USE_STRLEN);
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
            if (strncmp(mode_start, "apsta", 5) == 0) {
                wifi_service_post_set_mode(WIFI_OP_MODE_APSTA);
            } else if (strncmp(mode_start, "ap", 2) == 0) {
                wifi_service_post_set_mode(WIFI_OP_MODE_AP);
            } else {
                wifi_service_post_set_mode(WIFI_OP_MODE_STA);
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_ap_start_handler(httpd_req_t* req)
{
    char ssid[64]     = {0};
    char password[64] = {0};

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0 && ret < (int)sizeof(buf)) {
        buf[ret] = '\0';
        parse_urlencoded(buf, ret, ssid, sizeof(ssid), password, sizeof(password));
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

static esp_err_t api_dhcp_clients_handler(httpd_req_t* req)
{
    char buffer[768];
    wifi_service_get_dhcp_clients_json(buffer, sizeof(buffer));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buffer, strlen(buffer));
    return ESP_OK;
}

static void json_escape_ssid(const uint8_t* src, char* dst, int dst_size)
{
    int w = 0;
    int end = dst_size - 7;
    for (int j = 0; j < 32 && w < end && src[j]; j++) {
        unsigned char c = src[j];
        if (c == '"')       { dst[w++] = '\\'; dst[w++] = '"'; }
        else if (c == '\\') { dst[w++] = '\\'; dst[w++] = '\\'; }
        else if (c == '\b') { dst[w++] = '\\'; dst[w++] = 'b'; }
        else if (c == '\f') { dst[w++] = '\\'; dst[w++] = 'f'; }
        else if (c == '\n') { dst[w++] = '\\'; dst[w++] = 'n'; }
        else if (c == '\r') { dst[w++] = '\\'; dst[w++] = 'r'; }
        else if (c == '\t') { dst[w++] = '\\'; dst[w++] = 't'; }
        else if (c < 0x20)  {
            w += snprintf(dst + w, dst_size - w, "\\u%04x", c);
        }
        else { dst[w++] = (char)c; }
    }
    dst[w] = '\0';
}

static char s_scan_json[4096] = {0};
static bool s_scan_ready = false;
static bool s_scan_running = false;

static void scan_task(void* arg)
{
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        snprintf(s_scan_json, sizeof(s_scan_json),
                 "{\"error\":\"scan failed: %d\",\"results\":[]}", ret);
        s_scan_ready = true;
        s_scan_running = false;
        ESP_LOGE(TAG, "Scan failed: %d", ret);
        vTaskDelete(NULL);
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        snprintf(s_scan_json, sizeof(s_scan_json), "{\"results\":[]}");
        s_scan_ready = true;
        s_scan_running = false;
        vTaskDelete(NULL);
        return;
    }

    wifi_ap_record_t* ap_records = (wifi_ap_record_t*)malloc(
        ap_num * sizeof(wifi_ap_record_t));
    if (!ap_records) {
        snprintf(s_scan_json, sizeof(s_scan_json), "{\"results\":[]}");
        s_scan_ready = true;
        s_scan_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_wifi_scan_get_ap_records(&ap_num, ap_records);

    int pos = snprintf(s_scan_json, sizeof(s_scan_json), "{\"results\":[");
    char escaped[96];
    for (int i = 0; i < (int)ap_num && pos < (int)sizeof(s_scan_json) - 150; i++) {
        json_escape_ssid(ap_records[i].ssid, escaped, sizeof(escaped));

        int rssi = ap_records[i].rssi;
        int auth = (int)ap_records[i].authmode;
        const char* auth_str = "open";
        if (auth == WIFI_AUTH_WPA2_PSK || auth == WIFI_AUTH_WPA3_PSK ||
            auth == WIFI_AUTH_WPA2_WPA3_PSK)
            auth_str = "secure";
        else if (auth != WIFI_AUTH_OPEN)
            auth_str = "wep";

        pos += snprintf(s_scan_json + pos, sizeof(s_scan_json) - pos,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"auth\":\"%s\"}",
                        i > 0 ? "," : "",
                        escaped, rssi,
                        ap_records[i].primary, auth_str);
    }
    snprintf(s_scan_json + pos, sizeof(s_scan_json) - pos, "]}");
    free(ap_records);

    ESP_LOGI(TAG, "Scan done: %d APs, JSON: %d bytes", ap_num, (int)strlen(s_scan_json));
    s_scan_ready = true;
    s_scan_running = false;
    vTaskDelete(NULL);
}

static esp_err_t api_wifi_scan_handler(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, "application/json");

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        httpd_resp_send(req, "{\"error\":\"STA not active\",\"results\":[]}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (s_scan_ready) {
        httpd_resp_send(req, s_scan_json, strlen(s_scan_json));
        s_scan_ready = false;
        return ESP_OK;
    }

    if (!s_scan_running) {
        esp_wifi_scan_stop();
        s_scan_running = true;
        s_scan_ready = false;
        xTaskCreate(scan_task, "scan", 4096, NULL, 5, NULL);
    }

    httpd_resp_send(req, "{\"scanning\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_ble_status_handler(httpd_req_t* req)
{
    ble_pair_state_t st = ble_pairing_get_state();
    bool adv = ble_pairing_is_advertising();
    bool scn = ble_pairing_is_scanning();
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"state\":%d,\"advertising\":%s,\"scanning\":%s}",
             (int)st, adv ? "true" : "false", scn ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

static esp_err_t api_ble_advertise_handler(httpd_req_t* req)
{
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        char* en = strstr(buf, "enable=");
        if (en) {
            en += 7;
            if (en[0] == '1' || en[0] == 't') {
                char* name = strstr(buf, "name=");
                const char* dev_name = NULL;
                if (name) {
                    name += 5;
                    char* amp = strchr(name, '&');
                    if (amp) *amp = '\0';
                    dev_name = name;
                }
                ble_pairing_start_advertise(dev_name);
            } else {
                ble_pairing_stop_advertise();
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_ble_scan_handler(httpd_req_t* req)
{
    ble_pairing_start_scan(10);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_ble_devices_handler(httpd_req_t* req)
{
    ble_discovered_device_t devices[BLE_MAX_DISCOVERED];
    int count = ble_pairing_get_discovered(devices, BLE_MAX_DISCOVERED);

    char buf[2048];
    int pos = snprintf(buf, sizeof(buf),
                       "{\"scanning\":%s,\"devices\":[",
                       ble_pairing_is_scanning() ? "true" : "false");
    for (int i = 0; i < count; i++) {
        char bmac[18], nmac[18];
        snprintf(bmac, sizeof(bmac), MACSTR, MAC2STR(devices[i].mac));
        snprintf(nmac, sizeof(nmac), MACSTR, MAC2STR(devices[i].now_mac));
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"index\":%d,\"ble_mac\":\"%s\","
                        "\"now_mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d}",
                        i > 0 ? "," : "",
                        i, bmac, nmac, devices[i].name, devices[i].rssi);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

static esp_err_t api_ble_pair_handler(httpd_req_t* req)
{
    char buf[64] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        char* idx_str = strstr(buf, "index=");
        if (idx_str) {
            int idx = atoi(idx_str + 6);
            ble_pairing_pair_with_device(idx);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_now_peers_handler(httpd_req_t* req)
{
    char buffer[2048];
    wifi_now_get_peers_json(buffer, sizeof(buffer));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buffer, strlen(buffer));
    return ESP_OK;
}

static esp_err_t api_now_peer_remove_handler(httpd_req_t* req)
{
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        char* mac_str = strstr(buf, "mac=");
        if (mac_str) {
            mac_str += 4;
            uint8_t mac[6];
            int vals[6];
            if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
                       &vals[0], &vals[1], &vals[2],
                       &vals[3], &vals[4], &vals[5]) == 6) {
                for (int i = 0; i < 6; i++) mac[i] = (uint8_t)vals[i];
                wifi_now_remove_peer(mac);
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_now_mac_handler(httpd_req_t* req)
{
    uint8_t* mac = ble_pairing_get_own_now_mac();
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"mac\":\"" MACSTR "\"}", MAC2STR(mac));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

} // extern "C"

void web_server_start(WebServer* ws)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_uri_handlers = 24;
    config.stack_size       = 16384;

    httpd_uri_t root_uri     = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
    httpd_uri_t status_uri   = { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = api_wifi_status_handler, .user_ctx = NULL };
    httpd_uri_t connect_uri  = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_handler, .user_ctx = NULL };
    httpd_uri_t mode_uri     = { .uri = "/api/wifi/mode", .method = HTTP_POST, .handler = api_wifi_mode_handler, .user_ctx = NULL };
    httpd_uri_t ap_start_uri = { .uri = "/api/wifi/ap/start", .method = HTTP_POST, .handler = api_ap_start_handler, .user_ctx = NULL };
    httpd_uri_t ap_stop_uri  = { .uri = "/api/wifi/ap/stop", .method = HTTP_POST, .handler = api_ap_stop_handler, .user_ctx = NULL };
    httpd_uri_t restart_uri  = { .uri = "/api/restart", .method = HTTP_POST, .handler = api_restart_handler, .user_ctx = NULL };
    httpd_uri_t dhcp_uri     = { .uri = "/api/dhcp/clients", .method = HTTP_GET, .handler = api_dhcp_clients_handler, .user_ctx = NULL };
    httpd_uri_t scan_uri     = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_handler, .user_ctx = NULL };
    httpd_uri_t ble_status_uri = { .uri = "/api/ble/status", .method = HTTP_GET, .handler = api_ble_status_handler, .user_ctx = NULL };
    httpd_uri_t ble_adv_uri    = { .uri = "/api/ble/advertise", .method = HTTP_POST, .handler = api_ble_advertise_handler, .user_ctx = NULL };
    httpd_uri_t ble_scan_uri   = { .uri = "/api/ble/scan", .method = HTTP_POST, .handler = api_ble_scan_handler, .user_ctx = NULL };
    httpd_uri_t ble_dev_uri    = { .uri = "/api/ble/devices", .method = HTTP_GET, .handler = api_ble_devices_handler, .user_ctx = NULL };
    httpd_uri_t ble_pair_uri   = { .uri = "/api/ble/pair", .method = HTTP_POST, .handler = api_ble_pair_handler, .user_ctx = NULL };
    httpd_uri_t now_peers_uri  = { .uri = "/api/now/peers", .method = HTTP_GET, .handler = api_now_peers_handler, .user_ctx = NULL };
    httpd_uri_t now_rm_uri     = { .uri = "/api/now/peer/remove", .method = HTTP_POST, .handler = api_now_peer_remove_handler, .user_ctx = NULL };
    httpd_uri_t now_mac_uri    = { .uri = "/api/now/mac", .method = HTTP_GET, .handler = api_now_mac_handler, .user_ctx = NULL };

    if (httpd_start(&ws->server, &config) == ESP_OK) {
        httpd_register_uri_handler(ws->server, &root_uri);
        httpd_register_uri_handler(ws->server, &status_uri);
        httpd_register_uri_handler(ws->server, &connect_uri);
        httpd_register_uri_handler(ws->server, &mode_uri);
        httpd_register_uri_handler(ws->server, &ap_start_uri);
        httpd_register_uri_handler(ws->server, &ap_stop_uri);
        httpd_register_uri_handler(ws->server, &restart_uri);
        httpd_register_uri_handler(ws->server, &dhcp_uri);
        httpd_register_uri_handler(ws->server, &scan_uri);
        httpd_register_uri_handler(ws->server, &ble_status_uri);
        httpd_register_uri_handler(ws->server, &ble_adv_uri);
        httpd_register_uri_handler(ws->server, &ble_scan_uri);
        httpd_register_uri_handler(ws->server, &ble_dev_uri);
        httpd_register_uri_handler(ws->server, &ble_pair_uri);
        httpd_register_uri_handler(ws->server, &now_peers_uri);
        httpd_register_uri_handler(ws->server, &now_rm_uri);
        httpd_register_uri_handler(ws->server, &now_mac_uri);
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