#include "CLI/interactor.h"
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

void check_preset(bool pretask, const json::value& value, const std::string& default_combo, bool accepted)
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
        require(saved->task.size() == 1 && saved->task.front().name == "original" && saved->pretask.empty(), "invalid presets must preserve the existing configuration");
    }
    std::filesystem::remove_all(root);
}
}

int main()
{
    for (bool pretask : { false, true }) {
        check_preset(pretask, json::object { { "combo", "control+e" } }, "Ctrl+E", true);
        check_preset(pretask, json::object { { "combo", "NoSuchKey" } }, "Ctrl+E", false);
        check_preset(pretask, json::value(), "NoSuchKey", false);
        check_preset(pretask, json::object { }, "NoSuchKey", false);
    }
    return failures == 0 ? 0 : 1;
}
