#include "CLI/interactor.h"
#include "Impl/TerminalText.h"
#include "ProjectInterface/Parser.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace
{
int failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void check_preset(
    bool pretask,
    const json::value& value,
    const std::string& default_combo,
    bool accepted,
    const std::string& controller = "Adb")
{
    using namespace MAA_PROJECT_INTERFACE_NS;
    const auto root = std::filesystem::temp_directory_path()
                      / ("maapicli-hotkey-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "user/config");
    auto interface = *json::parse(R"({
        "interface_version": 2,
        "controller": [{"name":"controller", "type":"Adb"}],
        "resource": [{"name":"resource", "path":["resource"]}],
        "task": [{"name":"original", "entry":"Original"}, {"name":"task", "entry":"Main", "option":["shortcut"]}],
        "option": {"shortcut":{"type":"hotkey", "hotkeys":[{"name":"combo", "default":"Ctrl+E"}]}},
        "pretask": [{"name":"prepare", "exec":"unused", "option":["shortcut"]}],
        "preset": [{"name":"preset", "task":[{"name":"task"}]}]
    })");
    interface["option"]["shortcut"]["hotkeys"][0]["default"] = default_combo;
    interface["controller"][0]["type"] = controller;
    if (pretask) {
        interface["preset"][0]["task"] = json::array { json::object { { "name", "original" } } };
        interface["preset"][0]["pretask"] = json::array { json::object { { "name", "prepare" } } };
    }
    if (!value.is_null()) {
        interface["preset"][0][pretask ? "pretask" : "task"][0]["option"] = json::object { { "shortcut", value } };
    }
    {
        std::ofstream out(root / "interface.json");
        out << interface.dumps();
    }
    Configuration config;
    config.controller.name = "controller";
    config.resource = "resource";
    config.enabled_pretask = std::vector<std::string> { };
    config.task.emplace_back().name = "original";
    {
        std::ofstream out(root / "user/config/maa_pi_config.json");
        out << config.to_json().dumps();
    }
    std::istringstream input("7\n1\n8\n");
    std::ostringstream output;
    const auto old_input = std::cin.rdbuf(input.rdbuf());
    const auto old_output = std::cout.rdbuf(output.rdbuf());
    {
        Interactor interactor(root / "user");
        require(interactor.load(root), "hotkey fixture should load");
        require(interactor.interact(), "preset interaction should exit cleanly");
    }
    std::cin.rdbuf(old_input);
    std::cout.rdbuf(old_output);
    require((output.str().find("Applied preset:") != std::string::npos) == accepted, "preset acceptance should match hotkey validity");
    const auto saved = Parser::parse_config(root / "user/config/maa_pi_config.json");
    require(saved.has_value(), "configuration should remain readable");
    if (saved && accepted) {
        const auto& options = pretask ? saved->pretask.front().option : saved->task.front().option;
        require(options.size() == 1 && options.front().hotkeys.at("combo") == "Ctrl+E", "preset strings should be canonicalized");
    }
    if (saved && !accepted) {
        require(
            saved->task.size() == 1 && saved->task.front().name == "original" && saved->pretask.empty(),
            "invalid presets must preserve the existing configuration");
    }
    std::filesystem::remove_all(root);
}
}

int main()
{
    require(MaaPiCliTerminal::sanitize("Ctrl+\x1b[2JA") == "Ctrl+A", "CSI controls should be removed with their payload");
    require(MaaPiCliTerminal::sanitize("\x1b]52;c;secret\aA") == "A", "OSC clipboard commands should be removed");
    require(MaaPiCliTerminal::sanitize("\x1b]title\x1b\\A") == "A", "ST-terminated OSC should be removed");
    require(MaaPiCliTerminal::sanitize("A\n\tB\r\b") == "AB", "single-line hotkey text should not inject formatting");
    require(MaaPiCliTerminal::sanitize("快捷键") == "快捷键", "UTF-8 text should be preserved");
    require(
        MaaPiCliTerminal::sanitize("第一行\n\t\x1b[31m第二行\x1b[0m\r\b", true) == "第一行\n\t第二行",
        "multiline focus messages should retain line breaks and tabs while removing controls");
    for (unsigned int control = 0x80; control <= 0x9f; ++control) {
        const std::string encoded { static_cast<char>(0xc2), static_cast<char>(control) };
        require(MaaPiCliTerminal::sanitize(encoded).empty(), "all Unicode C1 controls should be removed from single-line fields");
        require(MaaPiCliTerminal::sanitize(encoded, true).empty(), "all Unicode C1 controls should be removed from multiline messages");
    }
    require(
        MaaPiCliTerminal::sanitize(
            "A\xc2\x9b"
            "2JB")
            == "AB",
        "Unicode CSI should be removed with its command payload");
    require(
        MaaPiCliTerminal::sanitize(
            "\xc2\x9d"
            "52;c;secret\xc2\x9c"
            "A")
            == "A",
        "Unicode OSC and ST should remove clipboard payloads");
    require(
        MaaPiCliTerminal::sanitize(
            "\x1b]title\xc2\x9c"
            "A")
            == "A",
        "ESC OSC should accept a Unicode ST terminator");
    require(
        MaaPiCliTerminal::sanitize(
            "\xc2\x9d"
            "title\x1b\\A")
            == "A",
        "Unicode OSC should accept an ESC ST terminator");
    require(
        MaaPiCliTerminal::sanitize(
            "A\xc2\x85"
            "B\n\tC",
            true)
            == "AB\n\tC",
        "C1 removal should retain allowed ASCII formatting");
    require(MaaPiCliTerminal::sanitize("界面🙂é") == "界面🙂é", "UTF-8 continuation bytes in the C1 range must remain intact");
    require(
        MaaPiCliTerminal::sanitize(
            "A\x9b"
            "B")
            == "AB",
        "standalone invalid C1 bytes should not reach the terminal");
    require(
        MaaPiCliTerminal::sanitize(
            "A\xe0\x82\x9b"
            "B")
            == "AB",
        "overlong UTF-8 controls should not reach the terminal");
    for (bool pretask : { false, true }) {
        check_preset(pretask, json::object { { "combo", "control+e" } }, "Ctrl+E", true);
        check_preset(pretask, json::object { { "combo", "NoSuchKey" } }, "Ctrl+E", false);
        check_preset(pretask, json::value(), "NoSuchKey", false);
        check_preset(pretask, json::object { }, "NoSuchKey", false);
        check_preset(pretask, json::value(), "Ctrl+E", true);
        check_preset(pretask, "Ctrl+E", "Ctrl+E", false);
        check_preset(pretask, json::object { { "combo", 42 } }, "Ctrl+E", false);
        check_preset(pretask, json::object { { "unknown", "Ctrl+E" } }, "Ctrl+E", false);
        check_preset(pretask, json::object { { "combo", "Insert" } }, "Ctrl+E", false, "MacOS");
    }
    return failures == 0 ? 0 : 1;
}
