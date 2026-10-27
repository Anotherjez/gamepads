#include <windows.h>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <atomic>

// Forward declare to avoid pulling dinput headers here.
struct IDirectInputDevice8W;

struct Gamepad {
  UINT joy_id;
  std::string name;
  int num_buttons;
  std::atomic<bool> alive;
  // Optional DirectInput device for richer support (e.g., wheels like G923).
  IDirectInputDevice8W* di_device = nullptr;
  bool use_directinput = false;
};

struct Event {
  int time;
  std::string type;
  std::string key;
  int value;
};

class Gamepads {
 private:
  std::list<Event> diff_states(Gamepad* gamepad,
                               const JOYINFOEX& old,
                               const JOYINFOEX& current);
  bool are_states_different(const JOYINFOEX& a, const JOYINFOEX& b);
  void read_gamepad(std::shared_ptr<Gamepad> gamepad);
  void connect_gamepad(UINT joy_id, std::string name, const GUID& guid);
  // Removes gamepad's map entry only if a rescan has not replaced it.
  void forget(const std::shared_ptr<Gamepad>& gamepad);
  // Prevent overlapping update runs and coalesce rapid device-change events.
  std::atomic<bool> update_scheduled{false};
  std::atomic<bool> shutting_down{false};

 public:
  std::map<UINT, std::shared_ptr<Gamepad>> gamepads;
  std::mutex mtx;
  std::mutex update_mtx;
  std::optional<std::function<void(Gamepad* gamepad, const Event& event)>>
      event_emitter;
  void update_gamepads();
  void request_update();

  // Stops background polling threads and prevents further device updates.
  // Safe to call multiple times.
  void shutdown();
};

extern Gamepads gamepads;

std::optional<LRESULT> CALLBACK GamepadListenerProc(HWND hwnd,
                                                    UINT uMsg,
                                                    WPARAM wParam,
                                                    LPARAM lParam);