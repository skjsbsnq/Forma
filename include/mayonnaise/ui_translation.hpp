#pragma once
#include <string>
#include <string_view>

namespace mayo {
// Optional display-only translation. Register before constructing widgets; values,
// IDs, editable contents and application callbacks retain their original text.
using UiTranslator = std::string (*)(std::string_view);
inline UiTranslator uiTranslator = nullptr;
inline void setUiTranslator(UiTranslator translator) { uiTranslator = translator; }
inline std::string uiText(std::string_view text) {
    return uiTranslator ? uiTranslator(text) : std::string(text);
}
}  // namespace mayo
