#pragma once

#include <string>
#include <string_view>

namespace wolfie {

std::wstring toWide(std::string_view value);
std::string toUtf8(std::wstring_view value);
std::string formatDouble(double value, int decimals = 2);
std::wstring formatWideDouble(double value, int decimals = 2);

}  // namespace wolfie
