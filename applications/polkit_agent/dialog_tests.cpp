#include "dialog.hpp"
#include <cassert>
#include <string>
int main() {
    forma::auth::Dialog dialog;
    forma::auth::DialogState state;
    state.message = "An application wants to make changes.";
    state.identities = {"Admin"};
    dialog.update(state);
    const float normal = dialog.preferredHeight();
    dialog.arrange({0, 0, 300, normal});
    for (const auto& child : dialog.children()) {
        assert(child->frame().x >= 0 && child->frame().y >= 0);
        assert(child->frame().right() <= 300 && child->frame().bottom() <= normal);
    }
    auto input = std::dynamic_pointer_cast<mayo::TextFieldView>(dialog.children()[1]);
    assert(input);
    input->setText("fabricated-password");
    int replies = 0;
    dialog.onResponse = [&](std::string response) {
        assert(input->text().empty()); // clear UI before invoking any synchronous backend callback
        assert(response == "fabricated-password");
        ++replies;
    };
    dialog.submit();
    assert(replies == 1 && input->text().empty());
    state.waiting = false;
    dialog.update(state);
    dialog.submit();
    assert(replies == 1);
    state.message = std::string(2048, 'x');
    dialog.update(state);
    assert(dialog.preferredHeight() > normal); // long policy messages remain accessible in scroll content
    state.echo = true;
    state.waiting = true;
    dialog.update(state);
    input->setText("old otp");
    state.prompt = "Next challenge:";
    dialog.update(state);
    assert(input->text().empty());
    return 0;
}
