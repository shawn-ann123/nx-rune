#include "img_viz.hpp"
#include "control_plane.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace
{
int env_integer(const char *name, int fallback)
{
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end == value ? fallback : static_cast<int>(parsed);
}

std::string env_string(const char *name, std::string fallback)
{
    const char *value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : std::move(fallback);
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return text;
}

bool send_all(int fd, const void *data, std::size_t size)
{
    const auto *cursor = static_cast<const unsigned char *>(data);
    while (size > 0)
    {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const ssize_t sent = ::send(fd, cursor, size, flags);
        if (sent <= 0) return false;
        cursor += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool is_private_ipv4(std::uint32_t network_address)
{
    const std::uint32_t address = ntohl(network_address);
    return (address & 0xff000000U) == 0x7f000000U ||
           (address & 0xff000000U) == 0x0a000000U ||
           (address & 0xfff00000U) == 0xac100000U ||
           (address & 0xffff0000U) == 0xc0a80000U;
}

std::string read_asset(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string header_value(const std::string &request, std::string_view name)
{
    const std::string marker = "\r\n" + std::string(name) + ":";
    const std::size_t start = request.find(marker);
    if (start == std::string::npos) return {};
    std::size_t value_start = start + marker.size();
    while (value_start < request.size() && request[value_start] == ' ') ++value_start;
    const std::size_t end = request.find("\r\n", value_start);
    return request.substr(value_start, end == std::string::npos ? std::string::npos : end - value_start);
}

void send_http_response(int fd, int status, std::string_view status_text,
                        std::string_view content_type, const std::string &body)
{
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << status_text << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
             << "X-Content-Type-Options: nosniff\r\n"
             << "X-Frame-Options: SAMEORIGIN\r\n"
             << "Connection: close\r\n\r\n" << body;
    const std::string bytes = response.str();
    send_all(fd, bytes.data(), bytes.size());
}

std::string reason_phrase(int status)
{
    switch (status)
    {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 422: return "Unprocessable Entity";
    default: return "Internal Server Error";
    }
}

std::string json_escape(const std::string &value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::ostringstream escaped;
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (ch < 0x20)
                escaped << "\\u00" << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
            else
                escaped << static_cast<char>(ch);
            break;
        }
    }
    return escaped.str();
}

const char kIndexPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Robomaster Debug MJPEG</title><style>body{margin:0;background:#101114;color:#e8e8e8;font:16px/1.4 sans-serif;}.wrap{max-width:1800px;margin:24px auto;padding:0 16px;}h1{font-size:22px;line-height:1.2;margin:0 0 4px;}.subtitle{margin:0 0 14px;color:#a8adb7;font-size:13px;}.charts{display:none;grid-template-columns:repeat(3,minmax(260px,1fr));gap:12px;margin:0 0 18px;}.chart{background:#17191f;border:1px solid rgba(255,255,255,0.08);border-radius:8px;padding:10px 10px 8px;min-width:0;}.chart-title{font-size:13px;font-weight:700;color:#d7dbe3;margin:0 0 6px;}.chart canvas{width:100%;height:170px;display:block;background:#111318;border-radius:6px;}.legend{display:flex;flex-wrap:wrap;gap:8px 12px;margin-top:7px;font-size:12px;color:#c6cad3;}.legend-item{display:flex;align-items:center;gap:5px;min-width:0;}.swatch{width:10px;height:10px;border-radius:2px;display:inline-block;flex:0 0 auto;}.stage{display:grid;grid-template-columns:minmax(240px,320px) minmax(0,1fr) minmax(240px,320px);gap:18px;align-items:start;}.sidebar{display:flex;flex-direction:column;gap:14px;min-width:0;}.viewer{display:block;min-width:0;}img{width:100%;background:#000;border-radius:8px;display:block;box-shadow:0 10px 28px rgba(0,0,0,0.28);}.panel{display:none;flex-direction:column;gap:4px;padding:10px 12px;border-radius:8px;background:rgba(0,0,0,0.28);backdrop-filter:blur(2px);border:1px solid rgba(255,255,255,0.06);min-height:44px;}.panel.left{align-items:flex-start;text-align:left;}.panel.right{align-items:flex-end;text-align:right;}.line{white-space:pre-wrap;text-shadow:0 1px 2px rgba(0,0,0,0.75);}.status{margin:10px 0 0;color:#a8adb7;font-size:13px;}code{background:#1f2126;padding:2px 6px;border-radius:6px;}@media (max-width: 1200px){.stage{grid-template-columns:1fr;}.sidebar{display:grid;grid-template-columns:1fr 1fr;}.charts{grid-template-columns:1fr;}.panel.right{align-items:flex-start;text-align:left;}}@media (max-width: 720px){.sidebar{grid-template-columns:1fr;}}</style></head><body><div class="wrap"><h1>Debug Stream</h1><p class="subtitle">Backend: <code>browser_mjpeg</code></p><div id="charts" class="charts"></div><div class="stage"><div class="sidebar"><div id="hud-top-left" class="panel left"></div><div id="hud-bottom-left" class="panel left"></div></div><div class="viewer"><img src="/stream" alt="debug stream"></div><div class="sidebar"><div id="hud-top-right" class="panel right"></div><div id="hud-bottom-right" class="panel right"></div></div></div><p id="status" class="status">HUD: connecting</p><script>const panels={top_left:document.getElementById('hud-top-left'),top_right:document.getElementById('hud-top-right'),bottom_left:document.getElementById('hud-bottom-left'),bottom_right:document.getElementById('hud-bottom-right')};const statusEl=document.getElementById('status');const chartsEl=document.getElementById('charts');const series=new Map();const charts=new Map();const windowMs=5000;function clearPanels(){Object.values(panels).forEach(el=>{el.innerHTML='';el.style.display='none';});}function alphaColor(color,alpha){const m=/^#([0-9a-f]{6})$/i.exec(color||'');if(!m)return color||'rgba(255,255,255,'+alpha+')';const n=parseInt(m[1],16);return 'rgba('+((n>>16)&255)+','+((n>>8)&255)+','+(n&255)+','+alpha+')';}function ensureChart(group){let chart=charts.get(group);if(chart)return chart;const wrap=document.createElement('div');wrap.className='chart';const title=document.createElement('div');title.className='chart-title';title.textContent=group;const canvas=document.createElement('canvas');const legend=document.createElement('div');legend.className='legend';wrap.appendChild(title);wrap.appendChild(canvas);wrap.appendChild(legend);chartsEl.appendChild(wrap);chart={wrap,title,canvas,legend};charts.set(group,chart);return chart;}function resizeCanvas(canvas){const rect=canvas.getBoundingClientRect();const dpr=Math.min(window.devicePixelRatio||1,2);const width=Math.max(320,Math.round(rect.width*dpr));const height=Math.max(160,Math.round(rect.height*dpr));if(canvas.width!==width||canvas.height!==height){canvas.width=width;canvas.height=height;}const ctx=canvas.getContext('2d');ctx.setTransform(dpr,0,0,dpr,0,0);return {ctx,w:width/dpr,h:height/dpr};}function appendSamples(samples){if(!Array.isArray(samples))return;for(const sample of samples){if(!sample)continue;const value=Number(sample.value);const t=Number(sample.timestamp_ms);if(!Number.isFinite(value)||!Number.isFinite(t))continue;const id=sample.id||sample.name||'series';let rec=series.get(id);if(!rec){rec={id,group:sample.group||'debug',name:sample.name||id,unit:sample.unit||'',color:sample.color||'#e8e8e8',points:[]};series.set(id,rec);}rec.group=sample.group||rec.group;rec.name=sample.name||rec.name;rec.unit=sample.unit||rec.unit;rec.color=sample.color||rec.color;rec.points.push([t,value]);}drawCharts();}function drawLine(ctx,rec,scale,minT,nowT,stepMode){let started=false;let prevY=0;ctx.beginPath();ctx.strokeStyle=rec.color;ctx.lineWidth=1.8;for(const p of rec.points){if(p[0]<minT||p[0]>nowT)continue;const x=scale.x(p[0]);const y=scale.y(p[1]);if(!started){ctx.moveTo(x,y);started=true;}else if(stepMode){ctx.lineTo(x,prevY);ctx.lineTo(x,y);}else{ctx.lineTo(x,y);}prevY=y;}if(started)ctx.stroke();}function drawOncePulses(ctx,rec,scale,minT,nowT){const pulseW=Math.max(6,scale.plotW*50/windowMs);const yTop=scale.y(1);const yBase=scale.y(0);const height=Math.max(3,yBase-yTop);ctx.fillStyle=alphaColor(rec.color,0.36);ctx.strokeStyle=rec.color;ctx.lineWidth=1;for(const p of rec.points){if(p[0]<minT||p[0]>nowT||p[1]<0.5)continue;const x=scale.x(p[0]);ctx.fillRect(x-pulseW/2,yTop,pulseW,height);ctx.strokeRect(x-pulseW/2,yTop,pulseW,height);}}function drawCharts(){let nowT=0;for(const rec of series.values()){for(const p of rec.points){if(p[0]>nowT)nowT=p[0];}}if(nowT<=0){chartsEl.style.display='none';return;}const minT=nowT-windowMs;const grouped=new Map();for(const rec of series.values()){rec.points=rec.points.filter(p=>p[0]>=minT);if(rec.points.length===0)continue;const group=rec.group||'debug';if(!grouped.has(group))grouped.set(group,[]);grouped.get(group).push(rec);}chartsEl.style.display=grouped.size?'grid':'none';for(const [group,chart] of charts){chart.wrap.style.display=grouped.has(group)?'block':'none';}for(const [group,recs] of grouped){const chart=ensureChart(group);chart.wrap.style.display='block';const g=resizeCanvas(chart.canvas);const ctx=g.ctx;const w=g.w;const h=g.h;ctx.clearRect(0,0,w,h);ctx.fillStyle='#111318';ctx.fillRect(0,0,w,h);const left=46,right=12,top=16,bottom=26;const plotW=Math.max(1,w-left-right);const plotH=Math.max(1,h-top-bottom);const isFire=group.toLowerCase().includes('fire');let min=isFire?-0.15:Infinity;let max=isFire?1.15:-Infinity;if(!isFire){for(const rec of recs){for(const p of rec.points){if(p[0]>=minT&&p[0]<=nowT){min=Math.min(min,p[1]);max=Math.max(max,p[1]);}}}if(!Number.isFinite(min)||!Number.isFinite(max)){min=-1;max=1;}if(Math.abs(max-min)<1e-6){min-=1;max+=1;}const pad=(max-min)*0.12;min-=pad;max+=pad;}const scale={plotW,x:t=>left+(Math.max(minT,Math.min(nowT,t))-minT)/windowMs*plotW,y:v=>top+(max-v)/(max-min)*plotH};ctx.strokeStyle='rgba(255,255,255,0.08)';ctx.fillStyle='rgba(232,232,232,0.58)';ctx.font='11px sans-serif';ctx.textAlign='right';ctx.textBaseline='middle';for(let i=0;i<=4;i++){const y=top+plotH*i/4;ctx.beginPath();ctx.moveTo(left,y);ctx.lineTo(w-right,y);ctx.stroke();const val=max-(max-min)*i/4;ctx.fillText(isFire?val.toFixed(1):val.toFixed(2),left-6,y);}ctx.textAlign='center';ctx.textBaseline='top';ctx.fillText('-5s',left,top+plotH+7);ctx.fillText('now',w-right,top+plotH+7);for(const rec of recs){if(isFire&&rec.id.toLowerCase().includes('once'))drawOncePulses(ctx,rec,scale,minT,nowT);drawLine(ctx,rec,scale,minT,nowT,isFire);}chart.legend.innerHTML='';for(const rec of recs){const item=document.createElement('span');item.className='legend-item';const sw=document.createElement('span');sw.className='swatch';sw.style.background=rec.color;const label=document.createElement('span');const last=rec.points[rec.points.length-1];label.textContent=rec.name+(last?' '+Number(last[1]).toFixed(isFire?0:2)+(rec.unit||''):'');item.appendChild(sw);item.appendChild(label);chart.legend.appendChild(item);}}}function renderHud(payload){clearPanels();if(!payload){statusEl.textContent='HUD: waiting';return;}const sampleCount=Array.isArray(payload.samples)?payload.samples.length:0;if(sampleCount>0)appendSamples(payload.samples);if(!Array.isArray(payload.lines)){statusEl.textContent='HUD: live, samples '+sampleCount;return;}const grouped={top_left:[],top_right:[],bottom_left:[],bottom_right:[]};for(const line of payload.lines){const section=grouped[line.section]?line.section:'top_left';grouped[section].push(line);}for(const [section,items] of Object.entries(grouped)){items.sort((a,b)=>(a.order||0)-(b.order||0));if(items.length===0)continue;const panel=panels[section];panel.style.display='flex';for(const item of items){const div=document.createElement('div');div.className='line';div.textContent=item.text||'';if(item.color)div.style.color=item.color;panel.appendChild(div);}}statusEl.textContent='HUD: live, samples '+sampleCount;}clearPanels();window.addEventListener('resize',()=>drawCharts());const hudEvents=new EventSource('/events');hudEvents.addEventListener('hud',ev=>{try{renderHud(JSON.parse(ev.data));}catch(_){statusEl.textContent='HUD: parse error';}});hudEvents.onopen=()=>{statusEl.textContent='HUD: connected';};hudEvents.onerror=()=>{statusEl.textContent='HUD: reconnecting';};</script></div></body></html>
)HTML";
} // namespace

ImgViz &ImgViz::instance()
{
    static ImgViz instance;
    return instance;
}

ImgViz::~ImgViz()
{
    const State previous = m_state.exchange(State::Stopping);
    if (previous == State::Uninitialized) return;
    m_stream_changed.notify_all();
    stop_browser_server();
    if (m_worker && m_worker->joinable()) m_worker->join();
}

void ImgViz::init(bool enabled)
{
    auto &self = instance();
    State expected = State::Uninitialized;
    if (!self.m_state.compare_exchange_strong(expected, enabled ? State::Enabled : State::Disabled))
        throw std::logic_error("img_viz::init() can only be called once");
    if (!enabled) return;

    const std::string requested = lower(env_string("RP_DEBUG_DISPLAY_BACKEND", "browser_mjpeg"));
    if (requested == "none" || requested == "off")
    {
        self.m_state.store(State::Disabled);
        return;
    }
    self.m_backend = requested == "opencv" || requested == "highgui"
        ? Backend::OpenCv : Backend::BrowserMjpeg;
    self.m_server_port = std::clamp(env_integer("RP_DEBUG_MJPEG_PORT", 8088), 1, 65535);
    self.m_jpeg_quality = std::clamp(env_integer("RP_DEBUG_MJPEG_QUALITY", 75), 30, 100);
    self.m_max_stream_fps = std::clamp(env_integer("RP_DEBUG_MJPEG_FPS", 30), 1, 120);
    self.m_stream_scale =
        std::clamp(env_integer("RP_DEBUG_MJPEG_SCALE_PERCENT", 50), 10, 100) / 100.0;
}

bool ImgViz::enabled()
{
    return instance().m_state.load(std::memory_order_relaxed) == State::Enabled;
}

void ImgViz::enqueue_image_copy(std::string_view window_name, const cv::Mat &image)
{
    if (!image.empty()) enqueue_image_zero_copy(window_name, image.clone());
}

void ImgViz::enqueue_image_zero_copy(std::string_view window_name, const cv::Mat &image)
{
    auto &self = instance();
    if (image.empty() || self.m_state.load(std::memory_order_relaxed) != State::Enabled) return;
    {
        std::lock_guard lock(self.m_mutex);
        auto &window = self.m_windows[std::string(window_name)];
        window.image = image;
        window.is_dirty = true;
    }
    self.ensure_worker();
}

void ImgViz::set_stream_status(std::string text)
{
    auto &self = instance();
    {
        std::lock_guard lock(self.m_status_mutex);
        self.m_stream_status = std::move(text);
    }
    self.m_stream_changed.notify_all();
}

void ImgViz::ensure_worker()
{
    if (m_state.load(std::memory_order_relaxed) != State::Enabled) return;
    std::lock_guard lock(m_mutex);
    if (!m_worker) m_worker = std::make_unique<std::thread>(&ImgViz::visualize, this);
}

void ImgViz::visualize()
{
    if (m_backend == Backend::BrowserMjpeg && !start_browser_server())
    {
        std::cerr << "[img_viz] MJPEG server unavailable; debug stream disabled\n";
        return;
    }

    while (m_state.load(std::memory_order_relaxed) != State::Stopping)
    {
        std::vector<std::pair<std::string, cv::Mat>> frames;
        {
            std::lock_guard lock(m_mutex);
            for (auto &[name, window] : m_windows)
            {
                if (!window.is_dirty) continue;
                frames.emplace_back(name, std::move(window.image));
                window.is_dirty = false;
            }
        }
        for (const auto &[name, image] : frames)
        {
            if (image.empty()) continue;
            if (m_backend == Backend::OpenCv)
            {
                cv::namedWindow(name, cv::WINDOW_NORMAL);
                cv::imshow(name, image);
            }
            else publish_browser_frame(image);
        }
        if (m_backend == Backend::OpenCv) cv::waitKeyEx(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool ImgViz::start_browser_server()
{
    m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_fd < 0) return false;
    int reuse = 1;
    setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(m_server_port));
    if (::bind(m_listen_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(m_listen_fd, 8) != 0)
    {
        ::close(m_listen_fd);
        m_listen_fd = -1;
        return false;
    }
    std::cout << "[img_viz] keypoint debug stream: http://0.0.0.0:" << m_server_port << "/\n";
    m_accept_thread = std::thread(&ImgViz::accept_browser_clients, this);
    return true;
}

void ImgViz::stop_browser_server()
{
    if (m_listen_fd >= 0)
    {
        ::shutdown(m_listen_fd, SHUT_RDWR);
        ::close(m_listen_fd);
        m_listen_fd = -1;
    }
    m_stream_changed.notify_all();
    if (m_accept_thread.joinable()) m_accept_thread.join();
    std::vector<BrowserClient> clients;
    {
        std::lock_guard lock(m_client_threads_mutex);
        clients.swap(m_client_threads);
    }
    for (auto &client : clients) if (client.thread.joinable()) client.thread.join();
}

void ImgViz::reap_finished_browser_clients_locked()
{
    for (auto it = m_client_threads.begin(); it != m_client_threads.end();)
    {
        if (!it->finished->load(std::memory_order_acquire))
        {
            ++it;
            continue;
        }
        if (it->thread.joinable()) it->thread.join();
        it = m_client_threads.erase(it);
    }
}

void ImgViz::run_browser_client(int client_fd, bool private_client,
                                const std::shared_ptr<std::atomic<bool>> &finished)
{
    serve_browser_client(client_fd, private_client);
    finished->store(true, std::memory_order_release);
}

void ImgViz::accept_browser_clients()
{
    while (m_state.load(std::memory_order_relaxed) != State::Stopping)
    {
        {
            std::lock_guard lock(m_client_threads_mutex);
            reap_finished_browser_clients_locked();
        }
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int client_fd = ::accept(m_listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_size);
        if (client_fd < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        timeval timeout{2, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        auto finished = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard lock(m_client_threads_mutex);
        m_client_threads.push_back(
            BrowserClient{std::thread(&ImgViz::run_browser_client, this, client_fd,
                                      is_private_ipv4(peer.sin_addr.s_addr), finished),
                          std::move(finished)});
    }
}

void ImgViz::serve_browser_client(int client_fd, bool private_client)
{
    std::string request;
    std::array<char, 8192> chunk{};
    std::size_t expected_size = 0;
    while (request.size() < 131072)
    {
        const ssize_t received = ::recv(client_fd, chunk.data(), chunk.size(), 0);
        if (received <= 0) break;
        request.append(chunk.data(), static_cast<std::size_t>(received));
        const std::size_t header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) continue;
        if (expected_size == 0)
        {
            const std::string length = header_value(request, "Content-Length");
            expected_size = header_end + 4 + (length.empty() ? 0 : static_cast<std::size_t>(std::stoul(length)));
        }
        if (request.size() >= expected_size) break;
    }
    if (request.empty())
    {
        ::close(client_fd);
        return;
    }
    const std::size_t first_space = request.find(' ');
    const std::size_t second_space = request.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos)
    {
        ::close(client_fd);
        return;
    }
    const std::string method = request.substr(0, first_space);
    std::string path = request.substr(first_space + 1, second_space - first_space - 1);
    const std::size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    const bool wants_stream = method == "GET" && path == "/stream";
    const bool wants_events = method == "GET" && (path == "/events" || path == "/api/v1/events");
    if (wants_events)
    {
        if (!private_client)
        {
            send_http_response(client_fd, 403, "Forbidden", "text/plain; charset=utf-8", "LAN only");
            ::close(client_fd);
            return;
        }
        static constexpr char header[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Connection: keep-alive\r\n\r\n";
        if (!send_all(client_fd, header, sizeof(header) - 1))
        {
            ::close(client_fd);
            return;
        }

        std::uint64_t sequence = 0;
        while (m_state.load(std::memory_order_relaxed) != State::Stopping)
        {
            std::uint64_t frame_id = 0;
            {
                std::lock_guard lock(m_stream_mutex);
                if (m_state.load(std::memory_order_relaxed) == State::Stopping) break;
                frame_id = m_frame_id;
            }
            const std::string payload = ControlPlane::instance().telemetry_json(frame_id);
            const std::string event = "id: " + std::to_string(++sequence) +
                                      "\nevent: telemetry\ndata: " + payload + "\n\n";
            if (!send_all(client_fd, event.data(), event.size())) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::close(client_fd);
        return;
    }

    if (path.rfind("/api/v1/", 0) == 0)
    {
        const std::string origin = header_value(request, "Origin");
        const std::string host = header_value(request, "Host");
        if (!origin.empty() && (host.empty() || origin.find(host) == std::string::npos))
        {
            send_http_response(client_fd, 403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"跨站请求被拒绝\"}");
            ::close(client_fd);
            return;
        }
        const std::size_t body_start = request.find("\r\n\r\n");
        const std::string body = body_start == std::string::npos ? std::string() : request.substr(body_start + 4);
        const std::string session = header_value(request, "X-Control-Session");
        const auto api_response = ControlPlane::instance().handle_http(
            method, path, body, session, private_client);
        const std::string reason = reason_phrase(api_response.status);
        send_http_response(client_fd, api_response.status, reason,
                           api_response.content_type, api_response.body);
        ::close(client_fd);
        return;
    }

    if (!wants_stream)
    {
        std::string asset_name;
        std::string content_type;
        if (method == "GET" && path == "/")
        {
            asset_name = "index.html";
            content_type = "text/html; charset=utf-8";
        }
        else if (method == "GET" && path == "/styles.css")
        {
            asset_name = "styles.css";
            content_type = "text/css; charset=utf-8";
        }
        else if (method == "GET" && path == "/app.js")
        {
            asset_name = "app.js";
            content_type = "text/javascript; charset=utf-8";
        }
        if (asset_name.empty() || !private_client)
        {
            send_http_response(client_fd, private_client ? 404 : 403,
                               private_client ? "Not Found" : "Forbidden",
                               "text/plain; charset=utf-8", private_client ? "Not found" : "LAN only");
        }
        else
        {
            const std::filesystem::path executable = std::filesystem::canonical("/proc/self/exe");
            const std::string body = read_asset(executable.parent_path().parent_path() / "web" / asset_name);
            if (body.empty())
                send_http_response(client_fd, 404, "Not Found", "text/plain; charset=utf-8", "Web asset missing");
            else
                send_http_response(client_fd, 200, "OK", content_type, body);
        }
        ::close(client_fd);
        return;
    }

    static constexpr char header[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Connection: close\r\n\r\n";
    if (!send_all(client_fd, header, sizeof(header) - 1))
    {
        ::close(client_fd);
        return;
    }

    std::uint64_t delivered = 0;
    while (m_state.load(std::memory_order_relaxed) != State::Stopping)
    {
        std::vector<unsigned char> jpeg;
        {
            std::unique_lock lock(m_stream_mutex);
            m_stream_changed.wait_for(lock, std::chrono::seconds(1), [this, delivered] {
                return m_frame_id != delivered ||
                       m_state.load(std::memory_order_relaxed) == State::Stopping;
            });
            if (m_state.load(std::memory_order_relaxed) == State::Stopping) break;
            if (m_frame_id == delivered || m_latest_jpeg.empty()) continue;
            delivered = m_frame_id;
            jpeg = m_latest_jpeg;
        }
        std::ostringstream part_header;
        part_header << "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                    << jpeg.size() << "\r\n\r\n";
        const std::string prefix = part_header.str();
        if (!send_all(client_fd, prefix.data(), prefix.size()) ||
            !send_all(client_fd, jpeg.data(), jpeg.size()) ||
            !send_all(client_fd, "\r\n", 2)) break;
    }
    ::close(client_fd);
}

void ImgViz::publish_browser_frame(const cv::Mat &image)
{
    const auto now = std::chrono::steady_clock::now();
    const auto minimum_interval = std::chrono::milliseconds(1000 / m_max_stream_fps);
    if (m_last_stream_frame.time_since_epoch().count() != 0 &&
        now - m_last_stream_frame < minimum_interval) return;

    cv::Mat scaled = image;
    if (m_stream_scale < 1.0)
    {
        cv::resize(image, scaled, cv::Size(), m_stream_scale, m_stream_scale, cv::INTER_AREA);
    }

    std::vector<unsigned char> jpeg;
    if (!cv::imencode(".jpg", scaled, jpeg, {cv::IMWRITE_JPEG_QUALITY, m_jpeg_quality})) return;
    m_last_stream_frame = now;
    {
        std::lock_guard lock(m_stream_mutex);
        m_latest_jpeg = std::move(jpeg);
        ++m_frame_id;
    }
    m_stream_changed.notify_all();
}
