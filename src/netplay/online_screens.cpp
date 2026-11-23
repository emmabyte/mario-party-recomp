// The launcher's Online Play: three screens over the launcher, in RecompFrontend's own look (its theme, buttons and
// labels), laid out as the project's design for them has them.
// - Online Play: the player's name, then Host a Party, Join a Party and Back.
// - Join a Party: the host's address, Connect! and Back, and how connecting goes.
// - The party: a place for each of four players (its number, an initial, the name, HOST and YOU, the round trip to the
//   host, and whether they're ready), the address to share (the host's) or joined (a guest's), a note that characters
//   and rules are chosen in the game, and Leave, Ready? and, for the host, Let's Party!, which starts the game once
//   everyone, the host too, is ready.
// The session itself is the core's lobby (core/session.cpp; netplay_core.h's lobby_*), whose status this polls at each
// UI update. Once the lobby is Ready the game starts, with the function main.cpp gave open_online.

#include <array>
#include <cctype>
#include <chrono>
#include <functional>
#include <string>

#include "librecomp/config.hpp"
#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_button.h"
#include "elements/ui_text_input.h"
#include "netplay.h"
#include "core/netplay_core.h"

namespace {
    using namespace recompui;
    using Clock = std::chrono::steady_clock;

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

    // The session's lines as the player reads them: each a sentence.
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

    std::string trimmed(const std::string& s) {
        size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }

    // The player colours, by player: the theme's.
    const theme::color seat_colors[netplay::PORTS] = {
        theme::color::Primary, theme::color::Danger, theme::color::Success, theme::color::Warning
    };

    // A panel over the launcher, as RecompFrontend's prompt draws one.
    Element* panel(ContextId context, Element* parent, float max_width) {
        Element* p = context.create_element<Element>(parent);
        p->set_display(Display::None);
        p->set_flex_direction(FlexDirection::Column);
        p->set_width(100, Unit::Percent);
        p->set_max_width(max_width, Unit::Dp);
        p->set_height_auto();
        p->set_margin_auto();
        p->set_padding(24, Unit::Dp);
        p->set_gap(16, Unit::Dp);
        p->set_border_width(theme::border::width, Unit::Dp);
        p->set_border_radius(theme::border::radius_lg, Unit::Dp);
        p->set_border_color(theme::color::WhiteA20);
        p->set_background_color(theme::color::ModalOverlay);
        return p;
    }

    Element* row(ContextId context, Element* parent, JustifyContent justify = JustifyContent::FlexStart) {
        Element* r = context.create_element<Element>(parent);
        r->set_display(Display::Flex);
        r->set_flex_direction(FlexDirection::Row);
        r->set_align_items(AlignItems::Center);
        r->set_justify_content(justify);
        r->set_gap(12, Unit::Dp);
        r->set_as_navigation_container(NavigationType::Horizontal);
        return r;
    }

    // A menu's button: its title, and what it does in dimmer text at its right.
    Button* menu_button(ContextId context, Element* parent, const std::string& title, const std::string& what,
                        ButtonStyle style) {
        Button* b = context.create_element<Button>(parent, title, style);
        b->set_width(100, Unit::Percent);
        b->set_justify_content(JustifyContent::SpaceBetween);
        b->set_padding_left(24, Unit::Dp);
        b->set_padding_right(24, Unit::Dp);
        if (!what.empty()) {
            Label* l = context.create_element<Label>(b, what, LabelStyle::Small);
            l->set_color(theme::color::TextDim);
        }
        return b;
    }

    // One player's place in the party.
    struct SeatCard {
        Element* card;
        Label* number;
        Element* avatar;
        Label* initial;
        Label* name;
        Label* tags;
        Label* ping;
        Label* state;

        void build(ContextId context, Element* parent, int p) {
            card = context.create_element<Element>(parent);
            card->set_display(Display::Flex);
            card->set_flex_direction(FlexDirection::Column);
            card->set_align_items(AlignItems::Center);
            card->set_flex(1.0f, 1.0f, 0.0f);
            card->set_min_height(250, Unit::Dp);
            card->set_padding(12, Unit::Dp);
            card->set_gap(8, Unit::Dp);
            card->set_border_width(theme::border::width, Unit::Dp);
            card->set_border_radius(theme::border::radius_md, Unit::Dp);
            card->set_border_color(theme::color::Border);
            card->set_background_color(theme::color::Background2);

            number = context.create_element<Label>(card, "P" + std::to_string(p + 1), LabelStyle::Small);
            number->set_padding_left(12, Unit::Dp);
            number->set_padding_right(12, Unit::Dp);
            number->set_border_radius(12, Unit::Dp);
            number->set_background_color(seat_colors[p]);

            avatar = context.create_element<Element>(card);
            avatar->set_display(Display::Flex);
            avatar->set_align_items(AlignItems::Center);
            avatar->set_justify_content(JustifyContent::Center);
            avatar->set_width(72, Unit::Dp);
            avatar->set_height(72, Unit::Dp);
            avatar->set_border_radius(36, Unit::Dp);
            avatar->set_border_width(theme::border::width, Unit::Dp);
            avatar->set_border_color(theme::color::BorderHard);
            initial = context.create_element<Label>(avatar, "?", LabelStyle::Large);

            name = context.create_element<Label>(card, "", LabelStyle::Normal);
            tags = context.create_element<Label>(card, "", LabelStyle::Annotation);
            tags->set_color(theme::color::Warning);
            ping = context.create_element<Label>(card, "", LabelStyle::Small);
            state = context.create_element<Label>(card, "", LabelStyle::Small);
            state->set_margin_top_auto();
            state->set_padding_left(12, Unit::Dp);
            state->set_padding_right(12, Unit::Dp);
            state->set_border_radius(12, Unit::Dp);
            state->set_border_width(theme::border::width, Unit::Dp);
        }

        void show(const netplay::LobbySeat& seat, int p, bool you) {
            if (!seat.present) {
                card->set_opacity(0.5f);
                avatar->set_background_color(theme::color::Transparent);
                initial->set_text("?");
                name->set_text("Waiting...");
                tags->set_text("Open slot");
                tags->set_color(theme::color::TextDim);
                ping->set_text("");
                state->set_display(Display::None);
                return;
            }
            card->set_opacity(1.0f);
            avatar->set_background_color(seat_colors[p]);
            std::string shown = seat.name.empty() ? (you ? "You" : "Player " + std::to_string(p + 1)) : seat.name;
            initial->set_text(std::string(1, char(toupper(static_cast<unsigned char>(shown[0])))));
            name->set_text(shown);
            std::string t = p == 0 ? "HOST" : "";
            if (you) {
                t += t.empty() ? "YOU" : "  ·  YOU";
            }
            tags->set_text(t);
            tags->set_color(theme::color::Warning);
            if (p == 0) {
                ping->set_text("Hosting");
                ping->set_color(theme::color::TextDim);
            } else if (seat.ping_ms < 0) {
                ping->set_text("...");
                ping->set_color(theme::color::TextDim);
            } else {
                ping->set_text(std::to_string(seat.ping_ms) + " ms");
                ping->set_color(seat.ping_ms < 60 ? theme::color::Success
                                : seat.ping_ms < 120 ? theme::color::Warning : theme::color::Danger);
            }
            state->set_display(Display::Block);
            state->set_text(seat.ready ? "Ready!" : "Not ready");
            state->set_background_color(seat.ready ? theme::color::Success : theme::color::Background3);
            state->set_border_color(seat.ready ? theme::color::SuccessL : theme::color::Border);
        }
    };

    class OnlineScreens;
    struct {
        ContextId context = ContextId::null();
        OnlineScreens* screens = nullptr;
        recomp::config::Config* config = nullptr;
        std::function<bool()> can_start;
        std::function<void()> start_game;
    } online;

    std::string option(const std::string& id) {
        return online.config == nullptr ? std::string() : std::get<std::string>(online.config->get_option_value(id));
    }

    void set_option(const std::string& id, const std::string& value) {
        if (online.config != nullptr && option(id) != value) {
            online.config->set_option_value(id, value);
            online.config->save_config();
        }
    }

    class OnlineScreens : public Element {
        enum class Screen { Menu, Join, Party };
        Screen screen = Screen::Menu;
        bool connecting = false; // Join: a lobby is open, and the host hasn't given this copy a place yet
        bool in_party = false;   // Party: the lobby is open
        bool ready = false;
        bool started = false;
        std::array<bool, netplay::PORTS> was_present{};
        std::array<std::string, netplay::PORTS> was_named{};
        Clock::time_point toast_until{};
        bool toast_shown = false;

        Element* menu;
        TextInput* name_input;
        Button* host_button;

        Element* join;
        TextInput* address_input;
        Label* join_error;
        Button* connect_button;
        Label* join_state;

        Element* party;
        Label* party_title;
        Label* share_caption;
        Label* share_address;
        SeatCard seats[netplay::PORTS];
        Label* note;
        Label* hint;
        Button* ready_button;
        Button* start_button;
        Label* toast;

    protected:
        std::string_view get_type_name() override { return "OnlineScreens"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Update) {
                refresh();
                queue_update();
            }
        }

    public:
        OnlineScreens(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Update)) {
            ContextId context = get_current_context();
            set_display(Display::Flex);
            set_width(100, Unit::Percent);
            set_padding(24, Unit::Dp);

            // Online Play.
            menu = panel(context, this, 640);
            context.create_element<Label>(menu, "Online Play", LabelStyle::Large);
            Label* sub = context.create_element<Label>(menu, "Play with friends over the internet.", LabelStyle::Small);
            sub->set_color(theme::color::TextDim);
            context.create_element<Label>(menu, "Your name", LabelStyle::Small);
            name_input = context.create_element<TextInput>(menu);
            name_input->add_text_changed_callback([](const std::string& text) { set_option("name", trimmed(text)); });
            host_button = menu_button(context, menu, "Host a Party", "Open a lobby", ButtonStyle::Primary);
            host_button->add_pressed_callback([this]() { host(); });
            Button* join_button = menu_button(context, menu, "Join a Party", "Enter the host's address",
                                              ButtonStyle::Secondary);
            join_button->add_pressed_callback([this]() { show(Screen::Join); });
            Button* back = menu_button(context, menu, "Back", "", ButtonStyle::Tertiary);
            back->add_pressed_callback([]() { mp_netplay::close_online(); });

            // Join a Party.
            join = panel(context, this, 640);
            context.create_element<Label>(join, "Join a Party", LabelStyle::Large);
            sub = context.create_element<Label>(join, "Ask your host for their address.", LabelStyle::Small);
            sub->set_color(theme::color::TextDim);
            context.create_element<Label>(join, "Host's address", LabelStyle::Small);
            address_input = context.create_element<TextInput>(join);
            join_error = context.create_element<Label>(join, "", LabelStyle::Small);
            join_error->set_color(theme::color::Danger);
            Element* join_row = row(context, join);
            connect_button = context.create_element<Button>(join_row, "Connect!", ButtonStyle::Success);
            connect_button->add_pressed_callback([this]() { connect(); });
            Button* join_back = context.create_element<Button>(join_row, "Back", ButtonStyle::Tertiary);
            join_back->add_pressed_callback([this]() {
                if (connecting) {
                    netplay::lobby_close();
                    connecting = false;
                }
                show(Screen::Menu);
            });
            join_state = context.create_element<Label>(join, "", LabelStyle::Small);
            join_state->set_color(theme::color::TextDim);

            // The party.
            party = panel(context, this, 1200);
            Element* top = row(context, party, JustifyContent::SpaceBetween);
            party_title = context.create_element<Label>(top, "Your Party", LabelStyle::Large);
            Element* share = context.create_element<Element>(top);
            share->set_display(Display::Flex);
            share->set_flex_direction(FlexDirection::Column);
            share->set_padding(12, Unit::Dp);
            share->set_border_width(theme::border::width, Unit::Dp);
            share->set_border_radius(theme::border::radius_md, Unit::Dp);
            share->set_border_color(theme::color::Warning);
            share_caption = context.create_element<Label>(share, "", LabelStyle::Annotation);
            share_caption->set_color(theme::color::TextDim);
            share_address = context.create_element<Label>(share, "", LabelStyle::Normal);
            Element* slots = row(context, party);
            slots->set_align_items(AlignItems::Stretch);
            for (int p = 0; p < netplay::PORTS; p++) {
                seats[p].build(context, slots, p);
            }
            note = context.create_element<Label>(party, "", LabelStyle::Small);
            note->set_padding(12, Unit::Dp);
            note->set_border_width(theme::border::width, Unit::Dp);
            note->set_border_radius(theme::border::radius_md, Unit::Dp);
            note->set_border_color(theme::color::Border);
            Element* actions = row(context, party);
            Button* leave_button = context.create_element<Button>(actions, "Leave", ButtonStyle::Tertiary);
            leave_button->add_pressed_callback([this]() { leave(); });
            hint = context.create_element<Label>(actions, "", LabelStyle::Small);
            hint->set_margin_left_auto();
            hint->set_color(theme::color::TextDim);
            ready_button = context.create_element<Button>(actions, "Ready?", ButtonStyle::Warning);
            ready_button->add_pressed_callback([this]() {
                ready = !ready;
                netplay::lobby_set_ready(ready);
                ready_button->set_text(ready ? "Ready!" : "Ready?");
                ready_button->apply_button_style(ready ? ButtonStyle::Success : ButtonStyle::Warning);
            });
            start_button = context.create_element<Button>(actions, "Let's Party!", ButtonStyle::Success);
            start_button->add_pressed_callback([]() { netplay::lobby_start(); });
            toast = context.create_element<Label>(party, "", LabelStyle::Small);
            queue_update();
        }

        void open() {
            name_input->set_text(option("name"));
            address_input->set_text(option("address"));
            show(Screen::Menu);
        }

        void show(Screen s) {
            screen = s;
            menu->set_display(s == Screen::Menu ? Display::Flex : Display::None);
            join->set_display(s == Screen::Join ? Display::Flex : Display::None);
            party->set_display(s == Screen::Party ? Display::Flex : Display::None);
            ContextId context = get_current_context();
            if (s == Screen::Join) {
                join_error->set_text("");
                join_state->set_text("");
                connect_button->set_enabled(true);
                context.set_autofocus_element(address_input);
            } else if (s == Screen::Menu) {
                context.set_autofocus_element(host_button);
            } else {
                context.set_autofocus_element(ready_button);
            }
        }

        netplay::SessionSettings settings(bool as_host) {
            netplay::SessionSettings s;
            s.host = as_host;
            s.name = trimmed(name_input->get_text());
            if (as_host) {
                int port = atoi(option("port").c_str());
                s.port = uint16_t(port >= 1 && port <= 65535 ? port : 7845);
            } else {
                s.peer = trimmed(address_input->get_text());
            }
            return s;
        }

        bool open_lobby(const netplay::SessionSettings& s) {
            ready = false;
            started = false;
            was_present = {};
            was_named = {};
            ready_button->set_text("Ready?");
            ready_button->apply_button_style(ButtonStyle::Warning);
            return netplay::lobby_open(s);
        }

        void host() {
            if (!online.can_start()) {
                return; // it says why (no ROM)
            }
            if (!open_lobby(settings(true))) {
                show(Screen::Menu);
                open_info_prompt("Online Play", sentences(netplay::lobby_status().reason), "OK", []() {},
                                 ButtonStyle::Primary);
                return;
            }
            in_party = true;
            show(Screen::Party);
        }

        void connect() {
            netplay::SessionSettings s = settings(false);
            if (s.peer.empty()) {
                join_error->set_text("Type the host's address first, like 26.12.34.56.");
                return;
            }
            set_option("address", s.peer);
            if (!online.can_start()) {
                return;
            }
            if (!open_lobby(s)) {
                join_error->set_text(sentences(netplay::lobby_status().reason));
                return;
            }
            join_error->set_text("");
            join_state->set_text("Connecting to " + s.peer + "...");
            connect_button->set_enabled(false);
            connecting = true;
        }

        void leave() {
            netplay::lobby_close();
            in_party = false;
            show(Screen::Menu);
        }

        void say(const std::string& text) {
            toast->set_text(text);
            toast_until = Clock::now() + std::chrono::seconds(2);
            toast_shown = true;
        }

        void refresh() {
            if (toast_shown && Clock::now() > toast_until) {
                toast->set_text("");
                toast_shown = false;
            }
            if (!connecting && !in_party) {
                return;
            }
            netplay::LobbyStatus s = netplay::lobby_status();
            if (connecting) {
                if (s.phase == netplay::LobbyStatus::Failed || s.phase == netplay::LobbyStatus::Closed) {
                    connecting = false;
                    netplay::lobby_close();
                    join_state->set_text("");
                    join_error->set_text(sentences(s.reason));
                    connect_button->set_enabled(true);
                } else if (s.player > 0) {
                    connecting = false;
                    in_party = true;
                    show(Screen::Party);
                }
                return;
            }
            show_party(s);
        }

        void show_party(const netplay::LobbyStatus& s) {
            bool failed = s.phase == netplay::LobbyStatus::Failed || s.phase == netplay::LobbyStatus::Closed;
            if (s.host) {
                party_title->set_text("Your Party");
                share_caption->set_text("Share this address");
                std::string addresses;
                for (const std::string& a : s.share) {
                    addresses += (addresses.empty() ? "" : "\n") + a;
                }
                share_address->set_text_unsafe(escaped(addresses.empty() ? "your address, port " + option("port")
                                                                         : addresses));
                note->set_text_unsafe("<b>Characters and party rules are picked in-game.</b> Once everyone's ready, "
                                      "start the game and the usual setup screens take over.");
            } else {
                const std::string& host_name = s.seats[0].name;
                party_title->set_text(host_name.empty() ? "The Host's Party" : host_name + "'s Party");
                share_caption->set_text("Connected to");
                share_address->set_text(s.peer);
                note->set_text_unsafe("<b>Characters and party rules are picked in-game.</b> Ready up and the host "
                                      "will start the game.");
            }
            int present = 0;
            bool all_ready = true;
            for (int p = 0; p < netplay::PORTS; p++) {
                const netplay::LobbySeat& seat = s.seats[p];
                seats[p].show(seat, p, p == s.player);
                if (seat.present) {
                    present++;
                    all_ready = all_ready && seat.ready;
                }
                // Who came and went, as the cards change.
                if (seat.present && !was_present[p] && p != s.player && p != 0 && !failed) {
                    say((seat.name.empty() ? "Player " + std::to_string(p + 1) : seat.name) + " joined the party!");
                }
                if (!seat.present && was_present[p] && !failed) {
                    say((was_named[p].empty() ? "Player " + std::to_string(p + 1) : was_named[p]) + " left.");
                }
                was_present[p] = seat.present;
                was_named[p] = seat.name;
            }
            all_ready = all_ready && present >= 2;
            start_button->set_display(s.host && !failed ? Display::Flex : Display::None);
            ready_button->set_display(failed ? Display::None : Display::Flex);
            start_button->set_enabled(all_ready && s.phase == netplay::LobbyStatus::Waiting);
            if (failed) {
                hint->set_text_unsafe(escaped(sentences(s.reason)));
                hint->set_color(theme::color::Danger);
            } else if (s.host) {
                hint->set_text(all_ready ? "Everyone's ready!" : present < 2 ? "Need at least 2 players."
                                                                              : "Waiting for everyone...");
                hint->set_color(theme::color::TextDim);
            } else {
                hint->set_text(all_ready ? "Waiting for the host to start..." : "");
                hint->set_color(theme::color::TextDim);
            }
            if (s.phase == netplay::LobbyStatus::Ready && !started) {
                started = true;
                hint->set_text("Starting...");
                if (netplay::lobby_play()) {
                    in_party = false;
                    hide_context(online.context);
                    online.start_game();
                }
            }
        }
    };

    void build_context() {
        online.context = create_context();
        online.context.open();
        Element* window = online.context.create_element<Element>(online.context.get_root_element());
        window->set_display(Display::Flex);
        window->set_flex_direction(FlexDirection::Column);
        window->set_background_color(theme::color::Transparent);

        Element* overlay = online.context.create_element<Element>(window);
        overlay->set_background_color(theme::color::BGOverlay);
        overlay->set_position(Position::Absolute);
        overlay->set_top(0);
        overlay->set_right(0);
        overlay->set_bottom(0);
        overlay->set_left(0);

        Element* wrapper = online.context.create_element<Element>(window);
        wrapper->set_display(Display::Flex);
        wrapper->set_position(Position::Absolute);
        wrapper->set_top(0);
        wrapper->set_right(0);
        wrapper->set_bottom(0);
        wrapper->set_left(0);
        wrapper->set_align_items(AlignItems::Center);
        wrapper->set_justify_content(JustifyContent::Center);

        online.screens = online.context.create_element<OnlineScreens>(wrapper);
        online.context.close();
    }
}

void mp_netplay::set_online_config(recomp::config::Config* config) {
    online.config = config;
}

void mp_netplay::open_online(std::function<bool()> can_start, std::function<void()> start_game) {
    online.can_start = std::move(can_start);
    online.start_game = std::move(start_game);
    hide_all_contexts(); // the launcher, while these show
    // The launcher's option that opens it runs in the launcher's context: that closes while this one is built, as
    // RecompFrontend's prompt does.
    ContextId prev_context = try_close_current_context();
    if (online.context == ContextId::null()) {
        build_context();
    }
    online.context.open();
    online.screens->open();
    online.context.close();
    if (prev_context != ContextId::null()) {
        prev_context.open();
    }
    show_context(online.context, "");
}

void mp_netplay::close_online() {
    hide_context(online.context);
    show_context(get_launcher_context_id(), "");
}
