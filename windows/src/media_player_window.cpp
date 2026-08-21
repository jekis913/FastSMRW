#include "media_player_window.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <dshow.h>

namespace fastsmui {

namespace {

// Runs a block with COM initialized on this thread, however the thread arrived.
struct ComScope {
    bool owned = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = (hr == S_OK || hr == S_FALSE); // RPC_E_CHANGED_MODE: don't unbalance it
    }
    ~ComScope() {
        if (owned)
            CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

// Walks the audio renderers DirectShow knows about, handing each one's friendly
// name and moniker to `visit`; returning true from `visit` stops the walk.
// Requires COM to already be live on this thread.
template <class Visit> void enum_audio_renderers(Visit visit) {
    ICreateDevEnum* devices = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ICreateDevEnum, reinterpret_cast<void**>(&devices))) ||
        !devices)
        return;
    IEnumMoniker* monikers = nullptr;
    if (devices->CreateClassEnumerator(CLSID_AudioRendererCategory, &monikers, 0) == S_OK &&
        monikers) {
        IMoniker* moniker = nullptr;
        while (monikers->Next(1, &moniker, nullptr) == S_OK) {
            std::wstring name;
            IPropertyBag* bag = nullptr;
            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
                                                 reinterpret_cast<void**>(&bag))) &&
                bag) {
                VARIANT v;
                VariantInit(&v);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR)
                    name = v.bstrVal;
                VariantClear(&v);
                bag->Release();
            }
            const bool stop = !name.empty() && visit(name, moniker);
            moniker->Release();
            if (stop)
                break;
        }
        monikers->Release();
    }
    devices->Release();
}

// Every endpoint shows up twice in this category: once as a DirectSound
// renderer, named "DirectSound: Speakers (...)", and once as a WaveOut one
// named plainly "Speakers (...)". The user should see each device once, under
// the name the rest of Windows calls it, so the prefix comes off for display
// and goes back on when we look the renderer up.
const wchar_t kDirectSoundPrefix[] = L"DirectSound: ";

std::wstring display_name_of(const std::wstring& renderer) {
    const size_t prefix = wcslen(kDirectSoundPrefix);
    return renderer.compare(0, prefix, kDirectSoundPrefix) == 0 ? renderer.substr(prefix)
                                                                : renderer;
}

// The category also holds the two "Default ..." pseudo-devices; the settings
// page offers "System default" itself, so they'd only be confusing duplicates.
bool is_default_pseudo_device(const std::wstring& name) {
    return name.rfind(L"Default ", 0) == 0;
}

} // namespace

std::vector<std::wstring> media_output_devices() {
    ComScope com;
    std::vector<std::wstring> names;
    enum_audio_renderers([&](const std::wstring& renderer, IMoniker*) {
        const std::wstring name = display_name_of(renderer);
        if (!is_default_pseudo_device(name) &&
            std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
        return false; // keep walking
    });
    return names;
}

// One streaming DirectShow graph rendering an HTTP audio URL.
struct MediaPlayback::Impl {
    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    IMediaSeeking* seek = nullptr;
    IBasicAudio* audio = nullptr;
    IMediaEventEx* event = nullptr;
    bool own_com = false;
    int volume = 100;
    std::wstring device; // "" = the system's own output device

    // Put the chosen output device's renderer in the graph before rendering the
    // URL: RenderFile connects to a renderer that's already there in preference
    // to creating one, which is what pins playback to that device.
    bool add_renderer_named(const std::wstring& wanted) {
        bool added = false;
        enum_audio_renderers([&](const std::wstring& name, IMoniker* moniker) {
            if (name != wanted)
                return false;
            IBaseFilter* renderer = nullptr;
            if (SUCCEEDED(moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter,
                                                reinterpret_cast<void**>(&renderer))) &&
                renderer) {
                graph->AddFilter(renderer, L"FastSM Audio Out"); // graph takes its own ref
                renderer->Release();
                added = true;
            }
            return true; // this was the one, however binding went
        });
        return added;
    }

    void add_chosen_renderer() {
        if (device.empty() || !graph)
            return;
        // Prefer the device's DirectSound renderer over its WaveOut twin: WaveOut
        // is a legacy path that can't mix as well and doesn't follow the endpoint
        // as cleanly. Fall back to the plain name if there's no DirectSound entry.
        if (add_renderer_named(kDirectSoundPrefix + device))
            return;
        add_renderer_named(device);
        // Still nothing (the device was unplugged or renamed): leave the graph
        // alone, so RenderFile picks the default renderer rather than not playing.
    }

    void release() {
        if (control)
            control->Stop();
        auto rel = [](auto*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };
        rel(event);
        rel(audio);
        rel(seek);
        rel(control);
        rel(graph);
    }
};

MediaPlayback::MediaPlayback() : impl_(new Impl) {}
MediaPlayback::~MediaPlayback() {
    stop();
    delete impl_;
}

bool MediaPlayback::active() const { return impl_->graph != nullptr; }
int MediaPlayback::volume_pct() const { return impl_->volume; }

bool MediaPlayback::play(const std::wstring& url) {
    stop();
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    impl_->own_com = (hr == S_OK || hr == S_FALSE);
    if (CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder,
                         reinterpret_cast<void**>(&impl_->graph)) != S_OK ||
        !impl_->graph) {
        stop();
        return false;
    }
    impl_->graph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&impl_->control));
    impl_->graph->QueryInterface(IID_IMediaSeeking, reinterpret_cast<void**>(&impl_->seek));
    impl_->graph->QueryInterface(IID_IBasicAudio, reinterpret_cast<void**>(&impl_->audio));
    impl_->graph->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&impl_->event));
    impl_->add_chosen_renderer();
    // RenderFile on an http(s) URL builds a streaming graph (progressive read).
    if (FAILED(impl_->graph->RenderFile(url.c_str(), nullptr)) || !impl_->control) {
        stop();
        return false;
    }
    if (impl_->seek)
        impl_->seek->SetTimeFormat(&TIME_FORMAT_MEDIA_TIME);
    set_volume(impl_->volume); // apply the level we start out at
    if (FAILED(impl_->control->Run())) {
        stop();
        return false;
    }
    return true;
}

void MediaPlayback::stop() {
    impl_->release();
    if (impl_->own_com) {
        CoUninitialize();
        impl_->own_com = false;
    }
}

static LONGLONG to_units(double s) { return static_cast<LONGLONG>(s * 10000000.0); }
static double to_seconds(LONGLONG u) { return static_cast<double>(u) / 10000000.0; }

double MediaPlayback::position() const {
    LONGLONG cur = 0;
    if (impl_->seek)
        impl_->seek->GetCurrentPosition(&cur);
    return to_seconds(cur);
}
double MediaPlayback::duration() const {
    LONGLONG dur = 0;
    if (impl_->seek)
        impl_->seek->GetDuration(&dur);
    return to_seconds(dur);
}
void MediaPlayback::seek(double delta) {
    if (!impl_->seek)
        return;
    const double dur = duration();
    double target = position() + delta;
    target = std::clamp(target, 0.0, dur > 0 ? dur : target);
    LONGLONG pos = to_units(target);
    impl_->seek->SetPositions(&pos, AM_SEEKING_AbsolutePositioning, nullptr,
                              AM_SEEKING_NoPositioning);
}
bool MediaPlayback::toggle_pause() {
    if (!impl_->control)
        return false;
    long st = 0; // OAFILTERSTATE (a LONG): State_Stopped/Paused/Running
    impl_->control->GetState(0, &st);
    if (st == State_Running) {
        impl_->control->Pause();
        return false;
    }
    impl_->control->Run();
    return true;
}
void MediaPlayback::set_volume(int pct) {
    impl_->volume = std::clamp(pct, 0, 100);
    if (!impl_->audio)
        return;
    // put_Volume is in hundredths of a decibel, -10000 (silence) .. 0 (full).
    // Percent means "this fraction of full loudness", so convert through the
    // amplitude ratio — a straight -10000 + pct*100 would put half volume at
    // -50 dB, which is inaudible, and make the settings slider useless.
    const long units =
        impl_->volume <= 0
            ? -10000
            : std::max(-10000L, static_cast<long>(std::lround(
                                    2000.0 * std::log10(impl_->volume / 100.0))));
    impl_->audio->put_Volume(units);
}

int MediaPlayback::adjust_volume(int delta) {
    set_volume(impl_->volume + delta);
    return impl_->volume;
}

void MediaPlayback::set_output_device(std::wstring name) { impl_->device = std::move(name); }
bool MediaPlayback::completed() {
    if (!impl_->event)
        return false;
    long code = 0;
    LONG_PTR p1 = 0, p2 = 0;
    bool done = false;
    while (impl_->event->GetEvent(&code, &p1, &p2, 0) == S_OK) {
        if (code == EC_COMPLETE || code == EC_ERRORABORT)
            done = true;
        impl_->event->FreeEventParams(code, p1, p2);
    }
    return done;
}

namespace {

// Heap state carried by the modeless player window for its whole lifetime.
struct WinState {
    MediaPlayback player;
    std::function<void(const std::wstring&)> speak;
    std::function<void(int)> on_volume; // remember the level the user settled on
};

constexpr wchar_t kClass[] = L"FastSMRWMediaPlayer";

std::wstring fmt_time(double seconds) {
    if (seconds < 0)
        seconds = 0;
    const int total = static_cast<int>(seconds + 0.5);
    wchar_t buf[32];
    wsprintfW(buf, L"%d:%02d", total / 60, total % 60);
    return buf;
}

void say(WinState* s, const std::wstring& msg) {
    if (s && s->speak)
        s->speak(msg);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        SetTimer(hwnd, 1, 500, nullptr); // poll for end-of-stream
        return 0;
    }
    auto* s = reinterpret_cast<WinState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_KEYDOWN:
        if (s) {
            switch (wp) {
            case VK_SPACE:
                say(s, s->player.toggle_pause() ? L"Playing" : L"Paused");
                return 0;
            case VK_LEFT:
                s->player.seek(-5);
                say(s, fmt_time(s->player.position()) + L" of " + fmt_time(s->player.duration()));
                return 0;
            case VK_RIGHT:
                s->player.seek(5);
                say(s, fmt_time(s->player.position()) + L" of " + fmt_time(s->player.duration()));
                return 0;
            case VK_UP:
            case VK_DOWN: {
                const int level = s->player.adjust_volume(wp == VK_UP ? 10 : -10);
                if (s->on_volume)
                    s->on_volume(level); // persist: the next thing you play starts here
                say(s, L"Volume " + std::to_wstring(level) + L" percent");
                return 0;
            }
            case VK_ESCAPE:
                DestroyWindow(hwnd);
                return 0;
            }
        }
        break;
    case WM_TIMER:
        if (s && s->player.completed()) {
            say(s, L"Finished");
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        KillTimer(hwnd, 1);
        delete s; // stops playback + releases COM via ~MediaPlayback
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool show_media_player(HWND parent, HINSTANCE inst, const std::wstring& title,
                       const std::wstring& url, std::function<void(const std::wstring&)> speak,
                       MediaPlayerOptions options) {
    auto* state = new WinState{};
    state->speak = std::move(speak);
    state->on_volume = std::move(options.on_volume);
    state->player.set_output_device(std::move(options.device));
    state->player.set_volume(options.volume);
    if (!state->player.play(url)) {
        delete state;
        return false; // caller falls back to the system player
    }

    static ATOM registered = 0;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc);
    }

    // Keys-only window: no controls, just a caption the screen reader announces
    // ("Playing: <title>"). Escape (or the close box) stops and closes it.
    const int w = 360, h = 90;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, (L"Playing: " + title).c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, parent, nullptr,
                                inst, state);
    if (!hwnd) {
        delete state;
        return false;
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    return true; // modeless; the window owns `state` and cleans up on close
}

} // namespace fastsmui
