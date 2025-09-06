#include <Windows.h>
#include <string>
#include <vector>

std::string to_string(const std::wstring& wide_string) {
  if (wide_string.empty())
    return std::string();

  int requiredSize = WideCharToMultiByte(CP_UTF8, 0, wide_string.data(), -1,
                                         nullptr, 0, nullptr, nullptr);
  if (requiredSize == 0)
    return std::string();

  std::vector<char> buffer(requiredSize);
  WideCharToMultiByte(CP_UTF8, 0, wide_string.data(), -1, buffer.data(),
                      requiredSize, nullptr, nullptr);

  return std::string(buffer.begin(),
                     buffer.end() - 1);  // Remove the null terminator
}

std::wstring to_wstring_utf8(const std::string& utf8) {
  if (utf8.empty())
    return std::wstring();
  int required = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), -1, nullptr, 0);
  if (required <= 0)
    return std::wstring();
  std::vector<wchar_t> wbuf(required);
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), -1, wbuf.data(), required);
  return std::wstring(wbuf.begin(), wbuf.end() - 1);
}