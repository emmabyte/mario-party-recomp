// The launcher's Online Play: three screens over the launcher, in RecompFrontend's own look (its theme, buttons and
// labels), laid out as the project's design for them has them.
// - Online Play: the player's name, then Host a Party, Resume Party (when this copy saved one), Join a Party, Play on
//   a Server and Back.
// - Join a Party: the host's address, Connect! and Back, and how connecting goes.
// - Play on a Server: a netplay server's address (src/netplay/server, for players who can't reach each other) and its
//   password, Connect! (Resume Party too, when this copy saved one) and Back. The first player on the server hosts the
//   party and the others join it, so the party that follows is a host's or a guest's as the server says, through it.
//   Back from a party that ended into its lobby (returned) joins the same server again.
// - The party: a place for each of four players (its number, an initial, the name, HOST and YOU, the round trip to the
//   host, and whether they're ready), the address to share (the host's) or joined (a guest's), a note that characters
//   and rules are chosen in the game, and Leave, Ready? and, for the host, Let's Party!, which starts the game once
//   everyone, the host too, is ready.
// The session itself is the core's lobby (core/session.cpp; netplay_core.h's lobby_*), whose status this polls at each
// UI update. Once the lobby is Ready the game starts, with the function main.cpp gave open_online.

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
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
        enum class Screen { Menu, Join, Server, Party };
        Screen screen = Screen::Menu;
        bool connecting = false; // Join: a lobby is open, and the host hasn't given this copy a place yet
        bool server_joining = false; // Play on a Server: joining the server, before its lobby opens
        bool via_server = false;     // the lobby, or the join, is through a server
        bool server_resume = false;  // Play on a Server's Resume Party: if this copy hosts, it resumes
        bool in_party = false;   // Party: the lobby is open
        bool ready = false;
        bool started = false;
        std::array<bool, netplay::PORTS> was_present{};
        std::array<std::string, netplay::PORTS> was_named{};
        Clock::time_point toast_until{};
        bool toast_shown = false;

        Element* menu;
        Label* menu_note;
        TextInput* name_input;
        Button* host_button;
        Button* resume_button;
        bool resuming = false;

        Element* join;
        TextInput* address_input;
        Label* join_error;
        Button* connect_button;
        Label* join_state;

        Element* server;
        TextInput* server_input;
        TextInput* password_input;
        Label* server_error;
        Button* server_connect_button;
        Button* server_resume_button;
        Label* server_state;

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
            // Why the game came back here from a party, if it did (returned).
            menu_note = context.create_element<Label>(menu, "", LabelStyle::Small);
            menu_note->set_color(theme::color::Danger);
            context.create_element<Label>(menu, "Your name", LabelStyle::Small);
            name_input = context.create_element<TextInput>(menu);
            name_input->add_text_changed_callback([](const std::string& text) { set_option("name", trimmed(text)); });
            host_button = menu_button(context, menu, "Host a Party", "Open a lobby", ButtonStyle::Primary);
            host_button->add_pressed_callback([this]() { host(false); });
            resume_button = menu_button(context, menu, "Resume Party", "Pick up where you left off",
                                        ButtonStyle::Primary);
            resume_button->add_pressed_callback([this]() { host(true); });
            Button* join_button = menu_button(context, menu, "Join a Party", "Enter the host's address",
                                              ButtonStyle::Secondary);
            join_button->add_pressed_callback([this]() { show(Screen::Join); });
            Button* server_button = menu_button(context, menu, "Play on a Server", "Enter the server's address",
                                                ButtonStyle::Secondary);
            server_button->add_pressed_callback([this]() { show(Screen::Server); });
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

            // Play on a Server.
            server = panel(context, this, 640);
            context.create_element<Label>(server, "Play on a Server", LabelStyle::Large);
            sub = context.create_element<Label>(server, "No port to open: everyone joins the same server. The first "
                                                        "one there hosts the party.", LabelStyle::Small);
            sub->set_color(theme::color::TextDim);
            context.create_element<Label>(server, "Server's address", LabelStyle::Small);
            server_input = context.create_element<TextInput>(server);
            context.create_element<Label>(server, "Password (if the server has one)", LabelStyle::Small);
            password_input = context.create_element<TextInput>(server, false); // shown as dots
            server_error = context.create_element<Label>(server, "", LabelStyle::Small);
            server_error->set_color(theme::color::Danger);
            Element* server_row = row(context, server);
            server_connect_button = context.create_element<Button>(server_row, "Connect!", ButtonStyle::Success);
            server_connect_button->add_pressed_callback([this]() { join_server(false); });
            server_resume_button = context.create_element<Button>(server_row, "Resume Party", ButtonStyle::Primary);
            server_resume_button->add_pressed_callback([this]() { join_server(true); });
            Button* server_back = context.create_element<Button>(server_row, "Back", ButtonStyle::Tertiary);
            server_back->add_pressed_callback([this]() {
                if (server_joining) {
                    netplay::server_cancel();
                    server_joining = false;
                }
                if (connecting) {
                    netplay::lobby_close();
                    connecting = false;
                }
                show(Screen::Menu);
            });
            server_state = context.create_element<Label>(server, "", LabelStyle::Small);
            server_state->set_color(theme::color::TextDim);

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
            // Resume Party, when this copy has played a party online: the game as it was at the start of the
            // last scene everyone reached.
            uint64_t frame = 0;
            int party_players = 0;
            bool saved = netplay::party_save(frame, party_players);
            resume_button->set_display(saved ? Display::Flex : Display::None);
            server_resume_button->set_display(saved ? Display::Flex : Display::None);
            name_input->set_text(option("name"));
            address_input->set_text(option("address"));
            server_input->set_text(option("server"));
            password_input->set_text(option("server_password"));
            show(Screen::Menu);
        }

        void show(Screen s) {
            screen = s;
            menu->set_display(s == Screen::Menu ? Display::Flex : Display::None);
            join->set_display(s == Screen::Join ? Display::Flex : Display::None);
            server->set_display(s == Screen::Server ? Display::Flex : Display::None);
            party->set_display(s == Screen::Party ? Display::Flex : Display::None);
            ContextId context = get_current_context();
            if (s == Screen::Join) {
                join_error->set_text("");
                join_state->set_text("");
                connect_button->set_enabled(true);
                context.set_autofocus_element(address_input);
            } else if (s == Screen::Server) {
                server_error->set_text("");
                server_state->set_text("");
                server_connect_button->set_enabled(true);
                server_resume_button->set_enabled(true);
                context.set_autofocus_element(server_input);
            } else if (s == Screen::Menu) {
                context.set_autofocus_element(host_button);
            } else {
                context.set_autofocus_element(ready_button);
            }
        }

    public:
        // Back from a party that ended (mp_netplay::online_returned): why, on the menu, and the lobby again if the host
        // took everyone back to it: the host's resuming the party, a guest's joining the host's address again.
        void returned(const std::string& reason, const std::string& lobby) {
            show(Screen::Menu);
            menu_note->set_text(reason);
            if (!lobby.empty() && option("via") == "server") {
                // The same server again: the host's place is held for it a while, so it hosts again.
                show(Screen::Server);
                join_server(lobby == "host");
            } else if (lobby == "host") {
                host(true);
            } else if (lobby == "join") {
                show(Screen::Join);
                connect();
            }
        }

    private:
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
            via_server = s.via_server;
            ready = false;
            started = false;
            was_present = {};
            was_named = {};
            ready_button->set_text("Ready?");
            ready_button->apply_button_style(ButtonStyle::Warning);
            return netplay::lobby_open(s);
        }

        void host(bool resume) {
            if (!online.can_start()) {
                return; // it says why (no ROM)
            }
            netplay::SessionSettings s = settings(true);
            s.resume = resume;
            resuming = resume;
            set_option("via", "direct");
            if (!open_lobby(s)) {
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
            set_option("via", "direct");
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

        // Play on a Server's Connect! (or Resume Party: resume, if this copy turns out to host): joins the server, and
        // refresh opens the lobby once it has.
        void join_server(bool resume) {
            std::string address = trimmed(server_input->get_text());
            if (address.empty()) {
                server_error->set_text("Type the server's address first, like play.example.com or 203.0.113.5.");
                return;
            }
            set_option("server", address);
            set_option("server_password", password_input->get_text());
            set_option("via", "server");
            // This copy's token on servers, kept with the settings, so that a host whose game closed or crashed comes
            // back to its held place as the host (udp.h's relay_connect reads it from the environment).
            if (option("server_token").empty()) {
                std::random_device rd;
                char hex[24];
                snprintf(hex, sizeof(hex), "%08x%08x", unsigned(rd()), unsigned(rd()) | 1u);
                set_option("server_token", hex);
            }
#ifdef _WIN32
            _putenv_s("MP_NETPLAY_RELAY_TOKEN", option("server_token").c_str());
#else
            setenv("MP_NETPLAY_RELAY_TOKEN", option("server_token").c_str(), 1);
#endif
            if (!online.can_start()) {
                return;
            }
            server_resume = resume;
            server_error->set_text("");
            server_state->set_text("Connecting to " + address + "...");
            server_connect_button->set_enabled(false);
            server_resume_button->set_enabled(false);
            netplay::server_join(address, trimmed(name_input->get_text()), password_input->get_text());
            server_joining = true;
        }

        void server_failed(const std::string& why) {
            server_state->set_text("");
            server_error->set_text(why);
            server_connect_button->set_enabled(true);
            server_resume_button->set_enabled(true);
        }

        // Once the server has taken this copy: its lobby, the host's or a guest's.
        void server_joined(bool as_host) {
            netplay::SessionSettings s;
            s.host = as_host;
            s.via_server = true;
            s.peer = trimmed(server_input->get_text());
            s.name = trimmed(name_input->get_text());
            s.resume = as_host && server_resume;
            resuming = s.resume;
            if (!open_lobby(s)) {
                netplay::server_cancel();
                server_failed(sentences(netplay::lobby_status().reason));
                return;
            }
            if (as_host) {
                server_state->set_text("");
                in_party = true;
                show(Screen::Party);
            } else {
                server_state->set_text("On the server: waiting for the host...");
                connecting = true;
            }
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
            if (server_joining) {
                netplay::ServerJoin j = netplay::server_join_status();
                if (j.phase == netplay::ServerJoin::Failed) {
                    server_joining = false;
                    netplay::server_cancel();
                    server_failed(sentences(j.reason));
                } else if (j.phase == netplay::ServerJoin::Joined) {
                    server_joining = false;
                    server_joined(j.host);
                }
                return;
            }
            if (!connecting && !in_party) {
                return;
            }
            netplay::LobbyStatus s = netplay::lobby_status();
            if (connecting) {
                if (s.phase == netplay::LobbyStatus::Failed || s.phase == netplay::LobbyStatus::Closed) {
                    connecting = false;
                    netplay::lobby_close();
                    if (via_server) {
                        server_failed(sentences(s.reason));
                    } else {
                        join_state->set_text("");
                        join_error->set_text(sentences(s.reason));
                        connect_button->set_enabled(true);
                    }
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
                share_caption->set_text(via_server ? "Hosting on the server: friends join it" : "Share this address");
                std::string addresses;
                for (const std::string& a : s.share) {
                    addresses += (addresses.empty() ? "" : "\n") + a;
                }
                share_address->set_text_unsafe(escaped(addresses.empty() ? "your address, port " + option("port")
                                                                         : addresses));
                if (s.resuming) {
                    note->set_text_unsafe("<b>Resuming your last party</b> where its last scene started. Everyone who "
                                          "played in it joins again, each as the player they were, and the game picks up from there.");
                } else {
                    note->set_text_unsafe("<b>Characters and party rules are picked in-game.</b> Once everyone's "
                                          "ready, start the game and the usual setup screens take over.");
                }
            } else {
                const std::string& host_name = s.seats[0].name;
                party_title->set_text(host_name.empty() ? "The Host's Party" : host_name + "'s Party");
                share_caption->set_text(via_server ? "On the server" : "Connected to");
                share_address->set_text(s.peer);
                if (s.resuming) {
                    note->set_text_unsafe("<b>The host is resuming your last party.</b> Ready up and it picks up "
                                          "where its last scene started.");
                } else {
                    note->set_text_unsafe("<b>Characters and party rules are picked in-game.</b> Ready up and the "
                                          "host will start the game.");
                }
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

void mp_netplay::online_returned(const std::string& reason, const std::string& lobby) {
    // From the launcher's update, in its context, as open_online.
    ContextId prev_context = try_close_current_context();
    online.context.open();
    online.screens->returned(reason, lobby);
    online.context.close();
    if (prev_context != ContextId::null()) {
        prev_context.open();
    }
}

void mp_netplay::close_online() {
    hide_context(online.context);
    show_context(get_launcher_context_id(), "");
}
