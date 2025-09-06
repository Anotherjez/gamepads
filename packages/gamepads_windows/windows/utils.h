#include <codecvt>
#include <locale>
#include <string>

std::string to_string(const std::wstring& wideString);
std::wstring to_wstring_utf8(const std::string& utf8);