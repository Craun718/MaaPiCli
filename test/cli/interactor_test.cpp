#include "CLI/interactor.h"

#include "ProjectInterface/Parser.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
int failures = 0;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path unique_temp_directory()
{
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto path = std::filesystem::temp_directory_path() / ("maapicli-eof-" + unique);
    std::filesystem::remove_all(path);
    return path;
}

class StreamRedirector
{
public:
    StreamRedirector()
        : old_input_(std::cin.rdbuf(nullptr))
        , old_output_(std::cout.rdbuf(nullptr))
    {
        std::cin.rdbuf(input_stream_.rdbuf());
        std::cout.rdbuf(output_stream_.rdbuf());
    }

    ~StreamRedirector()
    {
        std::cin.rdbuf(old_input_);
        std::cout.rdbuf(old_output_);
    }

    const std::stringstream& output() const { return output_stream_; }

private:
    std::streambuf* old_input_;
    std::streambuf* old_output_;
    std::stringstream input_stream_;
    std::stringstream output_stream_;
};
}

int main()
{
    const std::filesystem::path fixture_dir = MAAPICLI_TEST_FIXTURE_DIR;
    const auto interface = MAA_PROJECT_INTERFACE_NS::Parser::parse_interface(fixture_dir / "interface.json");
    require(interface.has_value() && interface->controller.size() == 2, "the EOF fixture should contain two controllers");

    const auto user_dir = unique_temp_directory();

    {
        StreamRedirector redirector;

        Interactor interactor(user_dir);
        require(interactor.load(fixture_dir), "the EOF interface fixture should load");
        const bool completed = interactor.interact();
        if (!completed) {
            std::cerr << redirector.output().str();
        }
        require(completed, "EOF during first-time setup should exit successfully");
    }

    require(
        !std::filesystem::exists(user_dir / "config/maa_pi_config.json"),
        "EOF during first-time setup should not create a configuration");

    std::filesystem::remove_all(user_dir);

    if (failures != 0) {
        std::cerr << failures << " interactor test assertion(s) failed\n";
        return 1;
    }

    std::cout << "MaaPiCli interactor tests passed\n";
    return 0;
}
