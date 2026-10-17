#include "../../applications/common/zh_cn.hpp"
#include <cassert>

int main() {
    // Other applications retain their original UI until opting in.
    assert(mayo::uiText("Settings") == "Settings");
    mayo::setUiTranslator(forma::zhCN);
    assert(mayo::uiText("Settings") == "系统设置");
    assert(mayo::uiText("10,000 lines") == "10,000 行");
    assert(mayo::uiText("5 minutes") == "5 分钟");
    assert(mayo::uiText("Connected to “My WiFi”") == "已连接到“My WiFi”");
    assert(mayo::uiText("org.kde.KWin") == "org.kde.KWin");
    assert(mayo::uiText("--working-directory=/tmp") == "--working-directory=/tmp");
    assert(mayo::uiText("用户文件") == "用户文件");
    mayo::setUiTranslator(nullptr);
    assert(mayo::uiText("Settings") == "Settings");
}
