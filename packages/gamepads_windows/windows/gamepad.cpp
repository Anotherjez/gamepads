#include <iostream>
#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <dbt.h>
#include <hidclass.h>
#pragma comment(lib, "winmm.lib")
#include <mmsystem.h>

#include <list>
#include <map>
#include <set>
#include <thread>
#include <chrono>
#include <algorithm>

// DirectInput
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

#include "gamepad.h"
#include "utils.h"

Gamepads gamepads;
// -------------------- DirectInput helpers --------------------
static IDirectInput8W* g_direct_input = nullptr;
// Serializes every WinMM joystick and DirectInput call. Neither API is safe to
// enter from several threads while Windows re-enumerates devices after a USB
// change; concurrent rescans and reader polls corrupted the process heap
// (0xC0000374 / 0xC0000005). Recursive because a rescan creates DI devices.
static std::recursive_mutex g_input_api_mutex;
using InputApiLock = std::lock_guard<std::recursive_mutex>;
// Timer used to debounce device-change rescans onto the window thread.
static constexpr UINT_PTR kRescanTimerId = 0x6A70;
static constexpr UINT kRescanDebounceMs = 150;
static inline DWORD di_axis_to_joy(LONG v) {
  // DI axes are typically -32768..32767; convert to 0..65535
  long shifted = static_cast<long>(v) + 32768L;
  if (shifted < 0)
    shifted = 0;
  if (shifted > 65535L)
    shifted = 65535L;
  return static_cast<DWORD>(shifted);
}

static bool EnsureDirectInput() {
  InputApiLock lock(g_input_api_mutex);
  if (g_direct_input)
    return true;
  HINSTANCE hInst = GetModuleHandleW(nullptr);
  HRESULT hr =
      DirectInput8Create(hInst, DIRECTINPUT_VERSION, IID_IDirectInput8W,
                         (void**)&g_direct_input, nullptr);
  if (FAILED(hr)) {
    std::cout << "DirectInput8Create failed: 0x" << std::hex << hr << std::dec
              << std::endl;
    return false;
  }
  return true;
}

struct DiFindContext {
  std::wstring target_name;
  IDirectInputDevice8W** out_device;
};

static BOOL CALLBACK EnumDevicesByNameCallback(const DIDEVICEINSTANCEW* inst,
                                               VOID* ctx) {
  auto* context = reinterpret_cast<DiFindContext*>(ctx);
  auto target = context->target_name;

  auto prod = std::wstring(inst->tszProductName);
  auto instname = std::wstring(inst->tszInstanceName);
  auto tolower_inplace = [](std::wstring& s) {
    for (auto& ch : s)
      ch = static_cast<wchar_t>(towlower(ch));
  };
  tolower_inplace(target);
  tolower_inplace(prod);
  tolower_inplace(instname);
  bool match = prod.find(target) != std::wstring::npos ||
               instname.find(target) != std::wstring::npos;
  if (match) {
    IDirectInputDevice8W* device = nullptr;
    if (SUCCEEDED(g_direct_input->CreateDevice(inst->guidInstance, &device,
                                               nullptr))) {
      *context->out_device = device;
      return DIENUM_STOP;
    }
  }
  return DIENUM_CONTINUE;
}

static IDirectInputDevice8W* CreateDIDeviceForName(const std::wstring& name) {
  if (!EnsureDirectInput())
    return nullptr;
  InputApiLock lock(g_input_api_mutex);
  IDirectInputDevice8W* device = nullptr;
  DiFindContext ctx;
  ctx.target_name = name;
  ctx.out_device = &device;
  g_direct_input->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevicesByNameCallback,
                              &ctx, DIEDFL_ATTACHEDONLY);
  if (!device) {
    // If not matched by name, just pick the first attached game controller as
    // a fallback to support wheels misreporting names across APIs.
    g_direct_input->EnumDevices(
        DI8DEVCLASS_GAMECTRL,
        [](const DIDEVICEINSTANCEW* inst, VOID* out) -> BOOL {
          auto** dev = reinterpret_cast<IDirectInputDevice8W**>(out);
          if (*dev == nullptr) {
            if (SUCCEEDED(g_direct_input->CreateDevice(inst->guidInstance, dev,
                                                       nullptr))) {
              return DIENUM_STOP;
            }
          }
          return DIENUM_CONTINUE;
        },
        &device, DIEDFL_ATTACHEDONLY);
  }
  if (device) {
    // Set data format and cooperative level.
    device->SetDataFormat(&c_dfDIJoystick2);
    device->SetCooperativeLevel(GetDesktopWindow(),
                                DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
  }
  return device;
}

// Polling interval to reduce CPU usage while reading gamepad state.
// 8 ms aligns with ~125 Hz update rate typical for many controllers.
static constexpr int kPollIntervalMs = 8;

std::list<Event> Gamepads::diff_states(Gamepad* gamepad,
                                       const JOYINFOEX& old,
                                       const JOYINFOEX& current) {
  std::time_t now = std::time(nullptr);
  int time = static_cast<int>(now);

  std::list<Event> events;
  if (old.dwXpos != current.dwXpos) {
    events.push_back(
        {time, "analog", "dwXpos", static_cast<int>(current.dwXpos)});
  }
  if (old.dwYpos != current.dwYpos) {
    events.push_back(
        {time, "analog", "dwYpos", static_cast<int>(current.dwYpos)});
  }
  if (old.dwZpos != current.dwZpos) {
    events.push_back(
        {time, "analog", "dwZpos", static_cast<int>(current.dwZpos)});
  }
  if (old.dwRpos != current.dwRpos) {
    events.push_back(
        {time, "analog", "dwRpos", static_cast<int>(current.dwRpos)});
  }
  if (old.dwUpos != current.dwUpos) {
    events.push_back(
        {time, "analog", "dwUpos", static_cast<int>(current.dwUpos)});
  }
  if (old.dwVpos != current.dwVpos) {
    events.push_back(
        {time, "analog", "dwVpos", static_cast<int>(current.dwVpos)});
  }
  if (old.dwPOV != current.dwPOV) {
    events.push_back({time, "analog", "pov", static_cast<int>(current.dwPOV)});
  }
  if (old.dwButtons != current.dwButtons) {
    // Scan up to 32 buttons (JOYINFOEX bitfield limit), independent of
    // WinMM-reported button count, since DirectInput may have more.
    for (int i = 0; i < 32; ++i) {
      bool was_pressed = old.dwButtons & (1 << i);
      bool is_pressed = current.dwButtons & (1 << i);
      if (was_pressed != is_pressed) {
        events.push_back(
            {time, "button", "button-" + std::to_string(i), is_pressed});
      }
    }
  }
  return events;
}

bool Gamepads::are_states_different(const JOYINFOEX& a, const JOYINFOEX& b) {
  return a.dwXpos != b.dwXpos || a.dwYpos != b.dwYpos || a.dwZpos != b.dwZpos ||
         a.dwRpos != b.dwRpos || a.dwUpos != b.dwUpos || a.dwVpos != b.dwVpos ||
         a.dwButtons != b.dwButtons || a.dwPOV != b.dwPOV;
}

void Gamepads::read_gamepad(std::shared_ptr<Gamepad> gamepad) {
  // If using DirectInput, set up DI polling state
  DIJOYSTATE2 di_state{};

  JOYINFOEX state;
  state.dwSize = sizeof(JOYINFOEX);
  state.dwFlags = JOY_RETURNALL;

  int joy_id = gamepad->joy_id;

  std::cout << "Listening to gamepad " << joy_id << std::endl;
  // Lower thread priority to minimize CPU impact under load.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  // Initial read to seed the state and avoid spurious diffs on first loop.
  if (!gamepad->use_directinput) {
    MMRESULT init_result;
    {
      InputApiLock api_lock(g_input_api_mutex);
      init_result = joyGetPosEx(joy_id, &state);
    }
    if (init_result != JOYERR_NOERROR) {
      std::cout << "Fail to initialize gamepad (WinMM) " << joy_id << std::endl;
      gamepad->alive = false;
      forget(gamepad);
      return;
    }
  } else {
    if (gamepad->di_device) {
      InputApiLock api_lock(g_input_api_mutex);
      gamepad->di_device->Acquire();
      HRESULT hr = gamepad->di_device->Poll();
      if (FAILED(hr)) {
        gamepad->di_device->Acquire();
      }
      if (SUCCEEDED(gamepad->di_device->GetDeviceState(sizeof(di_state),
                                                       &di_state))) {
        // Seed JOYINFOEX from DIJOYSTATE2 to reuse diff path
        state.dwXpos = di_axis_to_joy(di_state.lX);
        state.dwYpos = di_axis_to_joy(di_state.lY);
        state.dwZpos = di_axis_to_joy(di_state.lZ);
        state.dwRpos = di_axis_to_joy(di_state.lRz);  // wheels often use Rz
        state.dwUpos = di_axis_to_joy(di_state.lRx);
        state.dwVpos = di_axis_to_joy(di_state.lRy);
        // Buttons
        DWORD buttons = 0;
        for (int i = 0; i < 32; ++i) {
          if (di_state.rgbButtons[i] & 0x80)
            buttons |= (1u << i);
        }
        state.dwButtons = buttons;
        // Map POV[0] to 4 synthetic buttons (up/right/down/left) in high bits
        // when pressed; keep dwPOV too for analog pov angle if needed.
        state.dwPOV =
            (di_state.rgdwPOV[0] == 0xFFFFFFFF) ? 0xFFFF : di_state.rgdwPOV[0];
        if (di_state.rgdwPOV[0] != 0xFFFFFFFF) {
          DWORD angle = di_state.rgdwPOV[0] / 100;  // degrees
          auto set_btn = [&](int bit) { state.dwButtons |= (1u << bit); };
          // Use bits 28..31 for POV
          if (angle == 0 || angle == 315 || angle == 45)
            set_btn(28);  // up
          if (angle == 90 || angle == 45 || angle == 135)
            set_btn(29);  // right
          if (angle == 180 || angle == 135 || angle == 225)
            set_btn(30);  // down
          if (angle == 270 || angle == 225 || angle == 315)
            set_btn(31);  // left
        }
      }
    }
  }

  while (gamepad->alive.load() && !shutting_down.load(std::memory_order_relaxed)) {
    JOYINFOEX previous_state = state;
    bool ok = false;
    if (!gamepad->use_directinput) {
      InputApiLock api_lock(g_input_api_mutex);
      MMRESULT result = joyGetPosEx(joy_id, &state);
      ok = (result == JOYERR_NOERROR);
    } else if (gamepad->di_device) {
      InputApiLock api_lock(g_input_api_mutex);
      if (FAILED(gamepad->di_device->Poll())) {
        gamepad->di_device->Acquire();
      }
      if (SUCCEEDED(gamepad->di_device->GetDeviceState(sizeof(di_state),
                                                       &di_state))) {
        // Map DI to JOYINFOEX fields
        state.dwXpos = di_axis_to_joy(di_state.lX);
        state.dwYpos = di_axis_to_joy(di_state.lY);
        state.dwZpos = di_axis_to_joy(di_state.lZ);
        state.dwRpos = di_axis_to_joy(di_state.lRz);
        state.dwUpos = di_axis_to_joy(di_state.lRx);
        state.dwVpos = di_axis_to_joy(di_state.lRy);
        DWORD buttons = 0;
        for (int i = 0; i < 32; ++i) {
          if (di_state.rgbButtons[i] & 0x80)
            buttons |= (1u << i);
        }
        state.dwButtons = buttons;
        state.dwPOV =
            (di_state.rgdwPOV[0] == 0xFFFFFFFF) ? 0xFFFF : di_state.rgdwPOV[0];
        if (di_state.rgdwPOV[0] != 0xFFFFFFFF) {
          DWORD angle = di_state.rgdwPOV[0] / 100;  // degrees
          auto set_btn = [&](int bit) { state.dwButtons |= (1u << bit); };
          if (angle == 0 || angle == 315 || angle == 45)
            set_btn(28);
          if (angle == 90 || angle == 45 || angle == 135)
            set_btn(29);
          if (angle == 180 || angle == 135 || angle == 225)
            set_btn(30);
          if (angle == 270 || angle == 225 || angle == 315)
            set_btn(31);
        }
        ok = true;
      }
    }
    if (ok) {
      if (are_states_different(previous_state, state)) {
        std::list<Event> events =
            diff_states(gamepad.get(), previous_state, state);
        for (auto joy_event : events) {
          if (event_emitter.has_value()) {
            (*event_emitter)(gamepad.get(), joy_event);
          }
        }
      }
    } else {
      std::cout << "Fail to listen to gamepad " << joy_id << std::endl;
      gamepad->alive = false;
      forget(gamepad);
      break;
    }

    // Throttle polling to reduce CPU usage.
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  }
  // Cleanup DI device if was in use
  if (gamepad->di_device) {
    // Defensive cleanup in case the device was unplugged.
    {
      InputApiLock lock(g_input_api_mutex);
      HRESULT hr = gamepad->di_device->Unacquire();
      (void)hr;
      gamepad->di_device->Release();
      gamepad->di_device = nullptr;
    }
  }
}

void Gamepads::connect_gamepad(UINT joy_id, std::string name, int num_buttons) {
  auto gp = std::make_shared<Gamepad>();
  gp->joy_id = joy_id;
  gp->name = name;
  gp->num_buttons = num_buttons;
  gp->alive = true;
  // Try to bind a DirectInput device with matching product name to improve
  // support for devices like wheels.
  std::wstring wname = to_wstring_utf8(name);
  IDirectInputDevice8W* di_dev = CreateDIDeviceForName(wname);
  if (di_dev) {
    gp->di_device = di_dev;
    gp->use_directinput = true;
    // Try to query DI caps to adjust button count if possible.
    DIDEVCAPS caps;
    caps.dwSize = sizeof(DIDEVCAPS);
    HRESULT caps_result;
    {
      InputApiLock api_lock(g_input_api_mutex);
      caps_result = di_dev->GetCapabilities(&caps);
    }
    if (SUCCEEDED(caps_result)) {
      gp->num_buttons = std::min<int>(caps.dwButtons, 32);
      std::cout << "DI caps: buttons=" << gp->num_buttons
                << ", axes=" << caps.dwAxes << std::endl;
    }
    std::cout << "Using DirectInput for device " << joy_id << std::endl;
  } else {
    std::wcout << L"DirectInput not matched by name; product: " << wname
               << std::endl;
  }
  {
    std::lock_guard<std::mutex> lock(mtx);
    gamepads[joy_id] = gp;
  }
  std::thread read_thread([this, gp]() { read_gamepad(gp); });
  read_thread.detach();
}

void Gamepads::update_gamepads() {
  if (shutting_down.load(std::memory_order_relaxed))
    return;
  // Coalesce rapid calls; serialize the scan entire sequence.
  std::unique_lock<std::mutex> lk(update_mtx, std::try_to_lock);
  if (!lk.owns_lock()) {
    update_scheduled.store(true, std::memory_order_relaxed);
    return;
  }
  update_scheduled.store(false, std::memory_order_relaxed);
  std::cout << "Updating gamepads..." << std::endl;
  UINT max_joysticks;
  {
    InputApiLock api_lock(g_input_api_mutex);
    max_joysticks = joyGetNumDevs();
  }
  JOYCAPSW joy_caps;
  for (UINT joy_id = 0; joy_id < max_joysticks; ++joy_id) {
    if (shutting_down.load(std::memory_order_relaxed))
      break;
    MMRESULT result;
    {
      InputApiLock api_lock(g_input_api_mutex);
      result = joyGetDevCapsW(joy_id, &joy_caps, sizeof(JOYCAPSW));
    }
    if (result == JOYERR_NOERROR) {
      std::string name = to_string(joy_caps.szPname);
      int num_buttons = static_cast<int>(joy_caps.wNumButtons);
      bool need_connect = false;
      bool was_update = false;
      {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = gamepads.find(joy_id);
        if (it != gamepads.end()) {
          if (it->second->name != name) {
            std::cout << "Updated gamepad " << joy_id << std::endl;
            it->second->alive = false;
            gamepads.erase(it);
            need_connect = true;
            was_update = true;
          }
        } else {
          need_connect = true;
        }
      }
      if (need_connect) {
        if (!was_update)
          std::cout << "New gamepad connected " << joy_id << std::endl;
        connect_gamepad(joy_id, name, num_buttons);
      }
    }
  }
  // If updates piled up, run one more pass.
  if (!shutting_down.load(std::memory_order_relaxed) &&
      update_scheduled.exchange(false, std::memory_order_relaxed)) {
    lk.unlock();
    update_gamepads();
  }
}

void Gamepads::forget(const std::shared_ptr<Gamepad>& gamepad) {
  // A rescan may already have replaced this joy_id with a new reader; only
  // remove the entry this reader owns.
  std::lock_guard<std::mutex> lock(mtx);
  auto it = gamepads.find(gamepad->joy_id);
  if (it != gamepads.end() && it->second == gamepad)
    gamepads.erase(it);
}

std::set<std::wstring> connected_devices;

// Debounces device-change rescans onto the window thread: every HID change
// restarts the timer, and WM_TIMER runs a single rescan. Without a window
// (tests), falls back to the detached debounce.
static void schedule_rescan(HWND hwnd) {
  if (hwnd && SetTimer(hwnd, kRescanTimerId, kRescanDebounceMs, nullptr))
    return;
  gamepads.request_update();
}

std::optional<LRESULT> CALLBACK GamepadListenerProc(HWND hwnd,
                                                    UINT uMsg,
                                                    WPARAM wParam,
                                                    LPARAM lParam) {
  switch (uMsg) {
    case WM_TIMER: {
      if (wParam != kRescanTimerId)
        return std::nullopt;
      KillTimer(hwnd, kRescanTimerId);
      gamepads.update_gamepads();
      return 0;
    }
    case WM_DEVICECHANGE: {
      if (lParam != NULL) {
        PDEV_BROADCAST_HDR pHdr = (PDEV_BROADCAST_HDR)lParam;
        if (pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
          PDEV_BROADCAST_DEVICEINTERFACE pDevInterface =
              (PDEV_BROADCAST_DEVICEINTERFACE)pHdr;
          if (IsEqualGUID(pDevInterface->dbcc_classguid,
                          GUID_DEVINTERFACE_HID)) {
            std::wstring device_path = pDevInterface->dbcc_name;
            bool is_connected =
                connected_devices.find(device_path) != connected_devices.end();
            if (!is_connected && wParam == DBT_DEVICEARRIVAL) {
              connected_devices.insert(device_path);
              schedule_rescan(hwnd);
            } else if (is_connected && wParam == DBT_DEVICEREMOVECOMPLETE) {
              connected_devices.erase(device_path);
              schedule_rescan(hwnd);
            }
          }
        }
      }
      return 0;
    }
    case WM_DESTROY: {
      PostQuitMessage(0);
      return 0;
    }
  }
  return std::nullopt;
}

// Debounced request to update gamepads; schedules update on first call or
// coalesces multiple rapid requests to a single scan.
void Gamepads::request_update() {
  if (shutting_down.load(std::memory_order_relaxed))
    return;
  // Simple debounce: post a small delayed update on a separate thread, but
  // coalesce if many calls come in quickly.
  bool expected = false;
  if (update_scheduled.compare_exchange_strong(expected, true,
                                               std::memory_order_relaxed)) {
    std::thread([this]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      update_gamepads();
    }).detach();
  }
}

void Gamepads::shutdown() {
  // Mark shutdown first so any racing request_update/update_gamepads returns.
  bool already = shutting_down.exchange(true, std::memory_order_relaxed);
  (void)already;

  // Stop emitting events immediately to avoid calling into a disposed plugin.
  event_emitter.reset();

  // Mark all current gamepads as dead and clear the registry.
  {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& kv : gamepads) {
      if (kv.second)
        kv.second->alive.store(false, std::memory_order_relaxed);
    }
    gamepads.clear();
  }
}
