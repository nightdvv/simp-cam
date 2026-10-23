#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"SimpCamMainWindow";
constexpr UINT kFrameReadyMessage = WM_APP + 1;
constexpr int kCameraComboId = 1001;
constexpr int kRefreshButtonId = 1002;

struct Camera {
    std::wstring name;
    std::wstring symbolicLink;
};

struct Frame {
    UINT32 width = 0;
    UINT32 height = 0;
    bool topDown = true;
    std::vector<BYTE> pixels;
};

class CameraCapture {
public:
    CameraCapture(HWND window, std::wstring symbolicLink)
        : window_(window), symbolicLink_(std::move(symbolicLink)) {}

    ~CameraCapture() { Stop(); }

    void Start() {
        Stop();
        stopping_.store(false);
        worker_ = std::thread(&CameraCapture::Run, this);
    }

    void Stop() {
        stopping_.store(true);
        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        {
            std::lock_guard<std::mutex> lock(readerMutex_);
            reader = reader_;
        }
        if (reader) reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lock(readerMutex_);
        reader_.Reset();
    }

    bool CopyLatestFrame(Frame& destination) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (frame_.pixels.empty()) return false;
        destination = frame_;
        return true;
    }

    void AcknowledgeFrameMessage() { notificationPending_.store(false); }

    std::wstring Error() const {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return error_;
    }

private:
    void SetError(std::wstring message) {
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            error_ = std::move(message);
        }
        NotifyWindow();
    }

    void NotifyWindow() {
        if (!stopping_.load() && !notificationPending_.exchange(true))
            PostMessageW(window_, kFrameReadyMessage, 0, 0);
    }

    void Run() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitializeCom = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            SetError(DescribeError(L"COM", hr));
            return;
        }

        IMFAttributes* attributesRaw = nullptr;
        IMFAttributes* readerAttributesRaw = nullptr;
        IMFMediaSource* sourceRaw = nullptr;
        IMFSourceReader* readerRaw = nullptr;
        const wchar_t* failedStage = L"kamera hazırlığı";
        hr = MFCreateAttributes(&attributesRaw, 2);
        if (SUCCEEDED(hr)) hr = attributesRaw->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(hr)) hr = attributesRaw->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symbolicLink_.c_str());
        failedStage = L"kamerayı açma";
        if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attributesRaw, &sourceRaw);
        if (attributesRaw) attributesRaw->Release();

        if (SUCCEEDED(hr)) {
            failedStage = L"görüntü okuyucuyu başlatma";
            hr = MFCreateAttributes(&readerAttributesRaw, 2);
            if (SUCCEEDED(hr)) hr = readerAttributesRaw->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(sourceRaw, readerAttributesRaw, &readerRaw);
        }
        if (readerAttributesRaw) readerAttributesRaw->Release();

        if (SUCCEEDED(hr)) {
            failedStage = L"RGB görüntü biçimini ayarlama";
            IMFMediaType* mediaTypeRaw = nullptr;
            hr = MFCreateMediaType(&mediaTypeRaw);
            if (SUCCEEDED(hr)) hr = mediaTypeRaw->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = mediaTypeRaw->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            if (SUCCEEDED(hr)) hr = readerRaw->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaTypeRaw);
            if (mediaTypeRaw) mediaTypeRaw->Release();
        }

        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lock(readerMutex_);
            reader_.Attach(readerRaw);
        } else if (readerRaw) {
            readerRaw->Release();
        }
        if (FAILED(hr)) {
            SetError(DescribeError(failedStage, hr));
            if (readerRaw) readerRaw->Release();
            if (sourceRaw) {
                sourceRaw->Shutdown();
                sourceRaw->Release();
            }
            if (uninitializeCom) CoUninitialize();
            return;
        }

        while (!stopping_.load()) {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sampleRaw = nullptr;
            Microsoft::WRL::ComPtr<IMFSourceReader> reader;
            {
                std::lock_guard<std::mutex> lock(readerMutex_);
                reader = reader_;
            }
            if (!reader) break;
            hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp, &sampleRaw);
            if (FAILED(hr)) {
                if (!stopping_.load()) SetError(DescribeError(L"kare okuma", hr));
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!sampleRaw) continue;

            IMFMediaBuffer* bufferRaw = nullptr;
            hr = sampleRaw->ConvertToContiguousBuffer(&bufferRaw);
            sampleRaw->Release();
            if (FAILED(hr)) continue;

            BYTE* bytes = nullptr;
            DWORD maxLength = 0, currentLength = 0;
            hr = bufferRaw->Lock(&bytes, &maxLength, &currentLength);
            if (SUCCEEDED(hr)) {
                UINT32 width = 0, height = 0;
                LONG stride = 0;
                IMFMediaType* currentTypeRaw = nullptr;
                reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentTypeRaw);
                if (currentTypeRaw) {
                    MFGetAttributeSize(currentTypeRaw, MF_MT_FRAME_SIZE, &width, &height);
                    UINT32 strideValue = 0;
                    if (SUCCEEDED(currentTypeRaw->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideValue)))
                        stride = static_cast<LONG>(strideValue);
                    currentTypeRaw->Release();
                }
                if (stride == 0) stride = static_cast<LONG>(width * 4);
                const size_t expectedBytes = static_cast<size_t>(width) * height * 4;
                if (width && height && currentLength >= expectedBytes && !stopping_.load()) {
                    std::lock_guard<std::mutex> lock(frameMutex_);
                    frame_.width = width;
                    frame_.height = height;
                    frame_.topDown = stride > 0;
                    frame_.pixels.assign(bytes, bytes + expectedBytes);
                }
                bufferRaw->Unlock();
                if (width && height) NotifyWindow();
            }
            bufferRaw->Release();
        }

        {
            std::lock_guard<std::mutex> lock(readerMutex_);
            reader_.Reset();
        }
        if (sourceRaw) {
            sourceRaw->Shutdown();
            sourceRaw->Release();
        }
        if (uninitializeCom) CoUninitialize();
    }

    static std::wstring DescribeError(const wchar_t* stage, HRESULT hr) {
        wchar_t code[16]{};
        swprintf_s(code, L"0x%08X", static_cast<unsigned int>(hr));
        return std::wstring(L"Kamera ") + stage + L" hatası: " + code;
    }

    HWND window_;
    std::wstring symbolicLink_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> notificationPending_{false};
    std::thread worker_;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    std::mutex readerMutex_;
    std::mutex frameMutex_;
    Frame frame_;
    mutable std::mutex errorMutex_;
    std::wstring error_;
};

std::vector<Camera> EnumerateCameras() {
    std::vector<Camera> cameras;
    IMFAttributes* attributes = nullptr;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFCreateAttributes(&attributes, 1))) return cameras;
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(MFEnumDeviceSources(attributes, &devices, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* name = nullptr;
            WCHAR* link = nullptr;
            UINT32 nameLength = 0, linkLength = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLength)) && name &&
                SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &linkLength)) && link) {
                cameras.push_back({name, link});
            }
            CoTaskMemFree(name);
            CoTaskMemFree(link);
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
    }
    attributes->Release();
    return cameras;
}

struct AppState {
    HWND window = nullptr;
    HWND cameraCombo = nullptr;
    HWND statusLabel = nullptr;
    std::vector<Camera> cameras;
    std::unique_ptr<CameraCapture> capture;
    Frame displayedFrame;
    bool hasFrame = false;
    HDC previewBufferDc = nullptr;
    HBITMAP previewBufferBitmap = nullptr;
    HBITMAP previewBufferPreviousBitmap = nullptr;
    int previewBufferWidth = 0;
    int previewBufferHeight = 0;
};

AppState g_app;

void SetStatus(const std::wstring& text) {
    if (g_app.statusLabel) SetWindowTextW(g_app.statusLabel, text.c_str());
}

void StartSelectedCamera() {
    const int index = static_cast<int>(SendMessageW(g_app.cameraCombo, CB_GETCURSEL, 0, 0));
    if (g_app.capture) {
        g_app.capture->Stop();
        g_app.capture.reset();
    }
    g_app.hasFrame = false;
    InvalidateRect(g_app.window, nullptr, FALSE);
    if (index < 0 || index >= static_cast<int>(g_app.cameras.size())) {
        SetStatus(L"Kamera bulunamadı.");
        return;
    }
    SetStatus(L"Kamera açılıyor…");
    g_app.capture = std::make_unique<CameraCapture>(g_app.window, g_app.cameras[index].symbolicLink);
    g_app.capture->Start();
}

void RefreshCameras() {
    if (g_app.capture) {
        g_app.capture->Stop();
        g_app.capture.reset();
    }
    g_app.cameras = EnumerateCameras();
    SendMessageW(g_app.cameraCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& camera : g_app.cameras) SendMessageW(g_app.cameraCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(camera.name.c_str()));
    if (g_app.cameras.empty()) {
        SetStatus(L"Bağlı kamera bulunamadı.");
        g_app.hasFrame = false;
        InvalidateRect(g_app.window, nullptr, FALSE);
        return;
    }
    SendMessageW(g_app.cameraCombo, CB_SETCURSEL, 0, 0);
    StartSelectedCamera();
}

bool EnsurePreviewBuffer(HDC referenceDc, int width, int height) {
    if (g_app.previewBufferDc && g_app.previewBufferBitmap &&
        g_app.previewBufferWidth == width && g_app.previewBufferHeight == height) {
        return true;
    }

    if (!g_app.previewBufferDc) g_app.previewBufferDc = CreateCompatibleDC(referenceDc);
    if (!g_app.previewBufferDc) return false;

    if (g_app.previewBufferBitmap) {
        SelectObject(g_app.previewBufferDc, g_app.previewBufferPreviousBitmap);
        DeleteObject(g_app.previewBufferBitmap);
        g_app.previewBufferBitmap = nullptr;
    }

    g_app.previewBufferBitmap = CreateCompatibleBitmap(referenceDc, width, height);
    if (!g_app.previewBufferBitmap) return false;
    g_app.previewBufferPreviousBitmap = static_cast<HBITMAP>(SelectObject(g_app.previewBufferDc, g_app.previewBufferBitmap));
    g_app.previewBufferWidth = width;
    g_app.previewBufferHeight = height;
    return true;
}

void PaintPreview(HWND window, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    RECT area{20, 74, client.right - 20, client.bottom - 62};
    const int boxWidth = area.right - area.left;
    const int boxHeight = area.bottom - area.top;
    if (boxWidth <= 0 || boxHeight <= 0 || !EnsurePreviewBuffer(dc, boxWidth, boxHeight)) return;

    RECT bufferArea{0, 0, boxWidth, boxHeight};
    HBRUSH background = CreateSolidBrush(RGB(24, 25, 28));
    FillRect(g_app.previewBufferDc, &bufferArea, background);
    DeleteObject(background);

    if (!g_app.hasFrame || g_app.displayedFrame.pixels.empty()) {
        SetBkMode(g_app.previewBufferDc, TRANSPARENT);
        SetTextColor(g_app.previewBufferDc, RGB(190, 194, 200));
        DrawTextW(g_app.previewBufferDc, L"Canlı görüntü burada görünecek", -1, &bufferArea, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        BitBlt(dc, area.left, area.top, boxWidth, boxHeight, g_app.previewBufferDc, 0, 0, SRCCOPY);
        return;
    }

    const double scale = std::min(static_cast<double>(boxWidth) / g_app.displayedFrame.width,
                                  static_cast<double>(boxHeight) / g_app.displayedFrame.height);
    const int width = static_cast<int>(g_app.displayedFrame.width * scale);
    const int height = static_cast<int>(g_app.displayedFrame.height * scale);
    const int x = (boxWidth - width) / 2;
    const int y = (boxHeight - height) / 2;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(g_app.displayedFrame.width);
    bitmapInfo.bmiHeader.biHeight = g_app.displayedFrame.topDown
        ? -static_cast<LONG>(g_app.displayedFrame.height)
        : static_cast<LONG>(g_app.displayedFrame.height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(g_app.previewBufferDc, HALFTONE);
    StretchDIBits(g_app.previewBufferDc, x, y, width, height, 0, 0,
                  static_cast<int>(g_app.displayedFrame.width), static_cast<int>(g_app.displayedFrame.height),
                  g_app.displayedFrame.pixels.data(), &bitmapInfo, DIB_RGB_COLORS, SRCCOPY);
    BitBlt(dc, area.left, area.top, boxWidth, boxHeight, g_app.previewBufferDc, 0, 0, SRCCOPY);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_app.window = window;
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND title = CreateWindowW(L"STATIC", L"SimpCam", WS_CHILD | WS_VISIBLE,
                                   20, 18, 180, 28, window, nullptr, nullptr, nullptr);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        g_app.cameraCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                            20, 44, 320, 240, window, reinterpret_cast<HMENU>(kCameraComboId), nullptr, nullptr);
        SendMessageW(g_app.cameraCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND refresh = CreateWindowW(L"BUTTON", L"Yenile", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     350, 44, 84, 27, window, reinterpret_cast<HMENU>(kRefreshButtonId), nullptr, nullptr);
        SendMessageW(refresh, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        g_app.statusLabel = CreateWindowW(L"STATIC", L"Kameralar aranıyor…", WS_CHILD | WS_VISIBLE,
                                          20, 0, 500, 24, window, nullptr, nullptr, nullptr);
        SendMessageW(g_app.statusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        // Durum metni istemci alanının altına yerleşecek.
        SetWindowPos(g_app.statusLabel, nullptr, 20, 0, 500, 24, SWP_NOZORDER);
        RefreshCameras();
        return 0;
    }
    case WM_SIZE:
        if (g_app.statusLabel) SetWindowPos(g_app.statusLabel, nullptr, 20, HIWORD(lParam) - 38, 500, 24, SWP_NOZORDER);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kCameraComboId && HIWORD(wParam) == CBN_SELCHANGE) StartSelectedCamera();
        else if (LOWORD(wParam) == kRefreshButtonId) RefreshCameras();
        return 0;
    case kFrameReadyMessage:
        if (g_app.capture) {
            g_app.capture->AcknowledgeFrameMessage();
            Frame frame;
            if (g_app.capture->CopyLatestFrame(frame)) {
                const bool firstFrame = !g_app.hasFrame;
                g_app.displayedFrame = std::move(frame);
                g_app.hasFrame = true;
                if (firstFrame) SetStatus(L"Canlı görüntü");
            } else {
                const auto error = g_app.capture->Error();
                if (!error.empty()) SetStatus(error);
            }
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintPreview(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (g_app.capture) {
            g_app.capture->Stop();
            g_app.capture.reset();
        }
        if (g_app.previewBufferDc) {
            if (g_app.previewBufferBitmap) {
                SelectObject(g_app.previewBufferDc, g_app.previewBufferPreviousBitmap);
                DeleteObject(g_app.previewBufferBitmap);
            }
            DeleteDC(g_app.previewBufferDc);
            g_app.previewBufferDc = nullptr;
            g_app.previewBufferBitmap = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitializeCom = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Windows bileşenleri başlatılamadı.", L"SimpCam", MB_OK | MB_ICONERROR);
        return 1;
    }
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Media Foundation başlatılamadı.", L"SimpCam", MB_OK | MB_ICONERROR);
        if (uninitializeCom) CoUninitialize();
        return 1;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(0, kWindowClass, L"SimpCam",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 560,
                                 nullptr, nullptr, instance, nullptr);
    if (!window) {
        MFShutdown();
        if (uninitializeCom) CoUninitialize();
        return 1;
    }
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    MFShutdown();
    if (uninitializeCom) CoUninitialize();
    return static_cast<int>(message.wParam);
}
