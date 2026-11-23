// The lobby screen: what the launcher shows between Start Online Game (online_tab.cpp) and the game, while the players
// gather. The host's player sees each player as they join (by their address) and starts the game with Start once at
// least one has; a joining player sees their place and waits for the host to start. Back, on either, leaves the lobby
// and tells the others. The session itself is the core's (core/session.cpp's lobby, netplay_core.h's lobby_*): this
// shows its status, polled at each UI update, and once it is Ready starts the game.
//
// Its layout follows RecompFrontend's prompt (recompui/src/composites/ui_prompt.cpp): a panel over the launcher, a
// title, the players, a line of status, and the buttons.

#include <cctype>
#include <functional>
#include <string>

#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_button.h"
#include "netplay.h"
#include "core/netplay_core.h"

namespace {
    using namespace recompui;

    std::string escaped(const std::string& text) {
        std::string out;
        for (char c : text) {
            switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '\n': out += "<br/>"; break;
            default: out += c;
            }
        }
        return out;
    }

    // The session's lines as the player reads them: without their "[netplay] " (gone already) and each a sentence.
    std::string sentences(const std::string& lines) {
        std::string out;
        bool line_start = true;
        for (char c : lines) {
            if (line_start && c != ' ') {
                c = char(toupper(static_cast<unsigned char>(c)));
                line_start = false;
            }
            if (c == '\n') {
                if (!out.empty() && out.back() != '.' && out.back() != ':') {
                    out += '.';
                }
                line_start = true;
            }
            out += c;
        }
        while (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        return out;
    }

    class LobbyPanel;

    struct {
        ContextId context = ContextId::null();
        LobbyPanel* panel = nullptr;
        std::function<void()> start_game;
        std::function<void()> back;
    } lobby;

    class LobbyPanel : public Element {
        Label* title;
        Label* detail;
        Label* rows[netplay::PORTS];
        Label* state;
        Button* start;
        Button* back;
        bool done = false;

    protected:
        std::string_view get_type_name() override { return "LobbyPanel"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Update) {
                refresh();
                if (!done) {
                    queue_update();
                }
            }
        }

    public:
        LobbyPanel(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Update)) {
            ContextId context = get_current_context();
            set_display(Display::Flex);
            set_flex_direction(FlexDirection::Column);
            set_width(100, Unit::Percent);
            set_max_width(700, Unit::Dp);
            set_height_auto();
            set_margin_auto();
            set_border_width(theme::border::width, Unit::Dp);
            set_border_radius(theme::border::radius_lg, Unit::Dp);
            set_border_color(theme::color::WhiteA20);
            set_background_color(theme::color::ModalOverlay);

            title = context.create_element<Label>(this, "", LabelStyle::Large);
            title->set_margin(24, Unit::Dp);
            detail = context.create_element<Label>(this, "", LabelStyle::Small);
            detail->set_margin(24, Unit::Dp);
            detail->set_margin_top(0);

            Element* list = context.create_element<Element>(this);
            list->set_display(Display::Flex);
            list->set_flex_direction(FlexDirection::Column);
            list->set_padding_left(24, Unit::Dp);
            list->set_padding_right(24, Unit::Dp);
            list->set_padding_bottom(16, Unit::Dp);
            for (int p = 0; p < netplay::PORTS; p++) {
                rows[p] = context.create_element<Label>(list, "", LabelStyle::Normal);
                rows[p]->set_margin_bottom(8, Unit::Dp);
            }

            state = context.create_element<Label>(this, "", LabelStyle::Small);
            state->set_margin(24, Unit::Dp);
            state->set_margin_top(0);

            Element* controls = context.create_element<Element>(this);
            controls->set_display(Display::Flex);
            controls->set_flex_direction(FlexDirection::Row);
            controls->set_as_navigation_container(NavigationType::Horizontal);
            controls->set_justify_content(JustifyContent::SpaceBetween);
            controls->set_align_items(AlignItems::Center);
            controls->set_padding(24, Unit::Dp);
            controls->set_border_top_width(theme::border::width, Unit::Dp);
            controls->set_border_top_color(theme::color::BorderSoft);

            start = context.create_element<Button>(controls, "Start", ButtonStyle::Success);
            start->set_min_width(185.0f, Unit::Dp);
            start->add_pressed_callback([]() { netplay::lobby_start(); });
            back = context.create_element<Button>(controls, "Back", ButtonStyle::Danger);
            back->set_min_width(185.0f, Unit::Dp);
            back->add_pressed_callback([this]() { leave(); });
            queue_update();
        }

        // A new lobby in the same panel.
        void reset(bool host) {
            done = false;
            start->set_display(host ? Display::Flex : Display::None);
            start->set_enabled(false);
            back->set_text("Back");
            get_current_context().set_autofocus_element(back);
            refresh();
            queue_update();
        }

        void leave() {
            done = true;
            netplay::lobby_close();
            hide_context(lobby.context);
            if (lobby.back) {
                lobby.back();
            }
        }

        void refresh() {
            if (done) {
                return;
            }
            netplay::LobbyStatus s = netplay::lobby_status();
            if (s.host) {
                title->set_text("Hosting online play");
                detail->set_text("Your friends join with your address. Start when everyone is here.");
            } else {
                title->set_text("Joining online play");
                detail->set_text(s.player > 0 ? "You're in. The host starts the game when everyone is here."
                                              : "Reaching the host...");
            }
            for (int p = 0; p < netplay::PORTS; p++) {
                std::string text = "Player " + std::to_string(p + 1) + ": ";
                if (s.host) {
                    if (p == 0) {
                        text += "you (host)";
                    } else if (p < int(s.players.size())) {
                        text += "joined, " + s.players[p];
                    } else {
                        text += "open";
                    }
                } else {
                    if (p == 0) {
                        text += "the host";
                    } else if (p == s.player) {
                        text += "you";
                    } else if (p < s.player) {
                        text += "joined";
                    } else {
                        text += "open";
                    }
                }
                rows[p]->set_text(text);
            }
            switch (s.phase) {
            case netplay::LobbyStatus::Waiting:
                if (s.host) {
                    int joined = int(s.players.size()) - 1;
                    state->set_text(joined == 0 ? "Waiting for players to join."
                                                : std::to_string(joined + 1) + " players are here.");
                    start->set_enabled(joined > 0);
                } else {
                    state->set_text(s.player > 0 ? "Waiting for the host to start." : "");
                }
                break;
            case netplay::LobbyStatus::Ready:
                state->set_text("Starting...");
                done = true;
                start->set_enabled(false);
                if (netplay::lobby_play()) {
                    hide_context(lobby.context);
                    if (lobby.start_game) {
                        lobby.start_game();
                    }
                }
                break;
            case netplay::LobbyStatus::Failed:
            case netplay::LobbyStatus::Closed:
                detail->set_text(s.phase == netplay::LobbyStatus::Closed ? "You left the lobby." : "The lobby closed.");
                state->set_text_unsafe(escaped(sentences(s.reason)));
                start->set_display(Display::None);
                break;
            }
        }
    };

    void build_context() {
        lobby.context = create_context();
        lobby.context.open();
        Element* window = lobby.context.create_element<Element>(lobby.context.get_root_element());
        window->set_display(Display::Flex);
        window->set_flex_direction(FlexDirection::Column);
        window->set_background_color(theme::color::Transparent);

        Element* overlay = lobby.context.create_element<Element>(window);
        overlay->set_background_color(theme::color::BGOverlay);
        overlay->set_position(Position::Absolute);
        overlay->set_top(0);
        overlay->set_right(0);
        overlay->set_bottom(0);
        overlay->set_left(0);

        Element* wrapper = lobby.context.create_element<Element>(window);
        wrapper->set_display(Display::Flex);
        wrapper->set_position(Position::Absolute);
        wrapper->set_top(0);
        wrapper->set_right(0);
        wrapper->set_bottom(0);
        wrapper->set_left(0);
        wrapper->set_align_items(AlignItems::Center);
        wrapper->set_justify_content(JustifyContent::Center);

        lobby.panel = lobby.context.create_element<LobbyPanel>(wrapper);
        lobby.context.close();
    }
}

bool mp_netplay::open_lobby(const netplay::SessionSettings& settings, std::function<void()> start_game,
                            std::function<void()> back) {
    if (!netplay::lobby_open(settings)) {
        std::string why = sentences(netplay::lobby_status().reason);
        recompui::open_info_prompt("Online Play", why.empty() ? "Online play couldn't start." : why, "OK", back,
                                   recompui::ButtonStyle::Primary);
        return false;
    }
    lobby.start_game = std::move(start_game);
    lobby.back = std::move(back);
    // The button that opened it runs in the settings' context: that closes while this one is built, as the prompt does.
    ContextId prev_context = try_close_current_context();
    if (lobby.context == ContextId::null()) {
        build_context();
    }
    lobby.context.open();
    lobby.panel->reset(settings.host);
    lobby.context.close();
    if (prev_context != ContextId::null()) {
        prev_context.open();
    }
    show_context(lobby.context, "");
    return true;
}
