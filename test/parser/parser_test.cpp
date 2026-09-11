#include "ProjectInterface/Parser.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace
{
int failures = 0;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        ++failures;
    }
}

template <typename T, typename Member>
std::vector<std::string> names(const std::vector<T>& values, Member member)
{
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(value.*member);
    }
    return result;
}

bool same_names(const std::vector<std::string>& actual, const std::vector<std::string>& expected, const std::string& message)
{
    bool equal = actual == expected;
    require(equal, message);
    if (!equal) {
        std::cerr << "  expected:";
        for (const auto& name : expected) {
            std::cerr << ' ' << name;
        }
        std::cerr << "\n  actual:  ";
        for (const auto& name : actual) {
            std::cerr << ' ' << name;
        }
        std::cerr << '\n';
    }
    return equal;
}
} // namespace

int main()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const std::filesystem::path fixture_dir = MAAPICLI_TEST_FIXTURE_DIR;
    {
        InterfaceData data;
        data.interface_version = 2;
        data.resource.emplace_back().name = "default-resource";
        data.controller.emplace_back().name = "default-controller";
        data.option["valid-option"].cases.emplace_back().name = "fast";
        data.pretask = std::vector<InterfaceData::Pretask> {
            InterfaceData::Pretask { .exec = "first-pretask", .name = "ordered-first", .option = { "valid-option" } }
        };

        Configuration config;
        config.resource = "default-resource";
        config.controller.name = "default-controller";
        config.pretask.emplace_back();
        config.pretask.front().name = "ordered-first";
        config.pretask.front().option = { Configuration::Option { .name = "stale-option" },
                                          Configuration::Option { .name = "valid-option", .value = "fast" } };

        require(!Parser::check_configuration(data, config), "an invalid pretask option should mark the configuration as changed");
        require(config.pretask.size() == 1, "an invalid pretask option should not remove the entire pretask");
        require(
            config.pretask.front().option.size() == 1 && config.pretask.front().option.front().name == "valid-option",
            "an invalid pretask option should be removed while valid options are retained");
    }

    auto interface = Parser::parse_interface(fixture_dir / "interface.json");
    require(interface.has_value(), "valid interface with imports should parse");
    if (interface) {
        same_names(
            names(interface->task, &InterfaceData::Task::name),
            { "main-task", "first-import-task", "second-import-task" },
            "tasks should append in main-first import order");

        same_names(
            names(interface->preset, &InterfaceData::Preset::name),
            { "main-preset", "first-preset", "second-preset" },
            "presets should append in main-first import order");

        same_names(
            names(interface->setting, &InterfaceData::Setting::name),
            { "main-setting", "first-setting", "second-setting" },
            "settings should append in main-first import order");

        same_names(
            interface->global_option,
            { "first-global", "shared-global", "first-only-global", "second-global", "second-only-global" },
            "global options should append and retain the first occurrence");

        require(
            interface->resource.size() == 1 && interface->resource.front().hash == "Expected-Resource-Hash",
            "resource hash should parse");

        same_names(
            names(interface->group, &InterfaceData::Group::name),
            { "main-group", "first-group", "first-only-group", "second-only-group" },
            "groups should append and retain the first occurrence");

        auto shared_option = interface->option.find("shared-option");
        require(
            shared_option != interface->option.end() && shared_option->second.cases.size() == 1
                && shared_option->second.cases.front().label == "second-import-label",
            "later imports should override options with the same name");
        require(interface->option.contains("first-import-option"), "main task should be able to reference an imported option");

        require(interface->pretask.has_value(), "merged pretask should be present");
        if (interface->pretask) {
            const auto* pretasks = std::get_if<std::vector<InterfaceData::Pretask>>(&*interface->pretask);
            require(pretasks != nullptr, "merged pretask should use the array variant");
            if (pretasks) {
                same_names(
                    names(*pretasks, &InterfaceData::Pretask::name),
                    { "main-first", "main-second", "first-single-object", "second-first", "second-second" },
                    "object and array pretasks should flatten in main-first import order");
            }
        }
    }

    require(
        !Parser::parse_interface(fixture_dir / "invalid_option.json").has_value(),
        "a missing merged option reference should be rejected");
    require(
        !Parser::parse_interface(fixture_dir / "invalid_group.json").has_value(),
        "a missing merged group reference should be rejected");

    if (failures != 0) {
        std::cerr << failures << " parser test assertion(s) failed\n";
        return 1;
    }

    std::cout << "MaaPiCli parser tests passed\n";
    return 0;
}
