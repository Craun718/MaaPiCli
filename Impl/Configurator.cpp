#include "ProjectInterface/Configurator.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <ranges>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include "MaaUtils/SafeWindows.hpp"

#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "MaaFramework/Utility/MaaUtility.h"
#include "MaaUtils/Logger.h"
#include "MaaUtils/Platform.h"
#include "MaaUtils/ScopeLeave.hpp"
#include "MaaUtils/StringMisc.hpp"
#include "ProjectInterface/Parser.h"
#include "SecretStore.h"

MAA_PROJECT_INTERFACE_NS_BEGIN

namespace
{
#if defined(__APPLE__)
constexpr auto kConfigAclType = ACL_TYPE_EXTENDED;
#endif

#if defined(__APPLE__)
bool missing_acl_error(int error)
{
    if (error == EOPNOTSUPP || error == ENOTSUP) {
        return true;
    }
#ifdef ENOATTR
    if (error == ENOATTR) {
        return true;
    }
#endif
#ifdef __APPLE__
    // macOS reports an absent extended ACL as ENOENT even when the file exists.
    return error == ENOENT;
#else
    return false;
#endif
}
#endif

#if defined(__linux__)
constexpr char kPosixAclXattr[] = "system.posix_acl_access";

bool missing_xattr_error(int error)
{
    if (error == ENOTSUP || error == EOPNOTSUPP) {
        return true;
    }
#if defined(ENODATA)
    if (error == ENODATA) {
        return true;
    }
#endif
#if defined(ENOATTR) && ENOATTR != ENODATA
    if (error == ENOATTR) {
        return true;
    }
#endif
    return false;
}

bool read_configuration_acl(const std::filesystem::path& path, std::string& acl)
{
    const auto size = ::getxattr(path.c_str(), kPosixAclXattr, nullptr, 0);
    if (size < 0) {
        return missing_xattr_error(errno);
    }
    if (size == 0) {
        errno = EINVAL;
        return false;
    }

    acl.resize(static_cast<size_t>(size), '\0');
    const auto actual_size = ::getxattr(path.c_str(), kPosixAclXattr, acl.data(), acl.size());
    if (actual_size < 0) {
        return false;
    }
    if (actual_size != static_cast<ssize_t>(acl.size())) {
        errno = ERANGE;
        return false;
    }
    return true;
}
#endif

MaaWin32ScreencapMethod parse_win32_screencap_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaWin32ScreencapMethod> mapping = {
        { "GDI", MaaWin32ScreencapMethod_GDI },
        { "FramePool", MaaWin32ScreencapMethod_FramePool },
        { "DXGI_DesktopDup", MaaWin32ScreencapMethod_DXGI_DesktopDup },
        { "DXGI_DesktopDup_Window", MaaWin32ScreencapMethod_DXGI_DesktopDup_Window },
        { "PrintWindow", MaaWin32ScreencapMethod_PrintWindow },
        { "ScreenDC", MaaWin32ScreencapMethod_ScreenDC },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaWin32ScreencapMethod_None;
}

MaaWin32InputMethod parse_win32_input_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaWin32InputMethod> mapping = {
        { "Seize", MaaWin32InputMethod_Seize },
        { "SendMessage", MaaWin32InputMethod_SendMessage },
        { "PostMessage", MaaWin32InputMethod_PostMessage },
        { "LegacyEvent", MaaWin32InputMethod_LegacyEvent },
        { "PostThreadMessage", MaaWin32InputMethod_PostThreadMessage },
        { "SendMessageWithCursorPos", MaaWin32InputMethod_SendMessageWithCursorPos },
        { "PostMessageWithCursorPos", MaaWin32InputMethod_PostMessageWithCursorPos },
        { "SendMessageWithWindowPos", MaaWin32InputMethod_SendMessageWithWindowPos },
        { "PostMessageWithWindowPos", MaaWin32InputMethod_PostMessageWithWindowPos },
        { "Interception", MaaWin32InputMethod_Interception },
        { "AnchoredTouch", MaaWin32InputMethod_AnchoredTouch },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaWin32InputMethod_None;
}

MaaGamepadType parse_gamepad_type(const std::string& type)
{
    static const std::unordered_map<std::string, MaaGamepadType> mapping = {
        { "Xbox360", MaaGamepadType_Xbox360 },
        { "DualShock4", MaaGamepadType_DualShock4 },
        { "DS4", MaaGamepadType_DualShock4 },
    };

    if (auto it = mapping.find(type); it != mapping.end()) {
        return it->second;
    }
    return MaaGamepadType_Xbox360;
}

MaaMacOSScreencapMethod parse_macos_screencap_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaMacOSScreencapMethod> mapping = {
        { "ScreenCaptureKit", MaaMacOSScreencapMethod_ScreenCaptureKit },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaMacOSScreencapMethod_None;
}

MaaMacOSInputMethod parse_macos_input_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaMacOSInputMethod> mapping = {
        { "GlobalEvent", MaaMacOSInputMethod_GlobalEvent },
        { "PostToPid", MaaMacOSInputMethod_PostToPid },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaMacOSInputMethod_None;
}

MaaLinuxScreencapMethod parse_linux_screencap_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaLinuxScreencapMethod> mapping = {
        { "Wlr", MaaLinuxScreencapMethod_Wlr },
        { "PipeWire", MaaLinuxScreencapMethod_PipeWire },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaLinuxScreencapMethod_None;
}

MaaLinuxInputMethod parse_linux_input_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaLinuxInputMethod> mapping = {
        { "Wlr", MaaLinuxInputMethod_Wlr },
        { "UInput", MaaLinuxInputMethod_UInput },
        { "Libei", MaaLinuxInputMethod_Libei },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaLinuxInputMethod_None;
}

std::string pretask_identifier(const InterfaceData::Pretask& pretask)
{
    return pretask.name.empty() ? pretask.exec : pretask.name;
}

template <typename Transform>
bool transform_stored_passwords(
    const std::unordered_map<std::string, InterfaceData::Option>& options,
    Configuration& config,
    Transform transform)
{
    auto transform_option_list = [&](const std::string& scope, std::vector<Configuration::Option>& config_options) {
        for (auto& config_option : config_options) {
            auto option_iter = options.find(config_option.name);
            if (option_iter == options.end() || option_iter->second.type != InterfaceData::Option::Type::Input) {
                continue;
            }

            for (const auto& input_def : option_iter->second.inputs) {
                if (!input_def.password) {
                    continue;
                }

                auto value_iter = config_option.inputs.find(input_def.name);
                if (value_iter == config_option.inputs.end()) {
                    continue;
                }

                auto value = transform(scope + "/" + config_option.name + "/" + input_def.name, value_iter->second);
                if (value.empty() && !value_iter->second.empty()) {
                    return false;
                }
                value_iter->second = std::move(value);
            }
        }
        return true;
    };

    if (!transform_option_list("global", config.global_option) || !transform_option_list("resource", config.resource_option)
        || !transform_option_list("controller", config.controller_option)) {
        return false;
    }

    for (auto& task : config.task) {
        if (!transform_option_list("task/" + task.name, task.option)) {
            return false;
        }
    }

    for (auto& pretask : config.pretask) {
        if (!transform_option_list("pretask/" + pretask.name, pretask.option)) {
            return false;
        }
    }

    return true;
}

bool create_config_directory(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        LogError << "Failed to create configuration directory" << VAR(directory) << VAR(error.message());
        return false;
    }
    return true;
}

void remove_temporary_file(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
        LogError << "Failed to remove temporary configuration" << VAR(path) << VAR(error.message());
    }
}

class ConfigFileLock
{
public:
    explicit ConfigFileLock(const std::filesystem::path& user_dir)
    {
        lock_path_ = user_dir / "config" / ".maa_pi_config.lock";
        if (!create_config_directory(lock_path_.parent_path())) {
            return;
        }

#ifdef _WIN32
        handle_ = CreateFileW(
            lock_path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            LogError << "Failed to open configuration lock" << VAR(lock_path_) << VAR(GetLastError());
            return;
        }

        OVERLAPPED overlapped { };
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped)) {
            LogError << "Failed to lock configuration" << VAR(lock_path_) << VAR(GetLastError());
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        descriptor_ = ::open(lock_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (descriptor_ < 0) {
            LogError << "Failed to open configuration lock" << VAR(lock_path_) << VAR(errno);
            return;
        }

        if (::flock(descriptor_, LOCK_EX) != 0) {
            LogError << "Failed to lock configuration" << VAR(lock_path_) << VAR(errno);
            ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

    ~ConfigFileLock()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped { };
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
#endif
    }

    bool valid() const
    {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ >= 0;
#endif
    }

private:
    ConfigFileLock(const ConfigFileLock&) = delete;
    ConfigFileLock& operator=(const ConfigFileLock&) = delete;

    std::filesystem::path lock_path_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

class TemporaryConfigFile
{
public:
    TemporaryConfigFile(const std::filesystem::path& target_path, const std::filesystem::path& temporary_path, bool preserve_existing)
        : temporary_path_(temporary_path)
    {
        remove_temporary_file(temporary_path_);

#ifdef _WIN32
        SECURITY_ATTRIBUTES* security_attributes = nullptr;
        SECURITY_ATTRIBUTES attributes { };
        if (preserve_existing) {
            PSID owner = nullptr;
            PSID group = nullptr;
            PACL dacl = nullptr;
            PACL sacl = nullptr;
            PSECURITY_DESCRIPTOR security_descriptor = nullptr;
            const auto result = GetNamedSecurityInfoW(
                target_path.c_str(),
                SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                &owner,
                &group,
                &dacl,
                &sacl,
                &security_descriptor);
            if (result != ERROR_SUCCESS || security_descriptor == nullptr) {
                LogError << "Failed to read configuration security information" << VAR(target_path) << VAR(result);
                return;
            }
            OnScopeLeave([&]() { LocalFree(security_descriptor); });

            attributes.nLength = sizeof(attributes);
            attributes.lpSecurityDescriptor = security_descriptor;
            security_attributes = &attributes;
        }

        handle_ = CreateFileW(temporary_path_.c_str(), GENERIC_WRITE, 0, security_attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            LogError << "Failed to create temporary configuration" << VAR(temporary_path_) << VAR(GetLastError());
        }
#else
        struct stat target_status { };
        if (::stat(target_path.c_str(), &target_status) == 0) {
            target_mode_ = target_status.st_mode & 07777;
        }
        else if (errno != ENOENT) {
            LogError << "Failed to inspect existing configuration" << VAR(target_path) << VAR(errno);
            return;
        }

#if defined(__APPLE__)
        errno = 0;
        existing_acl_ = ::acl_get_file(target_path.c_str(), kConfigAclType);
        if (existing_acl_ == nullptr && !missing_acl_error(errno)) {
            LogError << "Failed to read configuration ACL" << VAR(target_path) << VAR(errno);
            return;
        }
#elif defined(__linux__)
        if (!read_configuration_acl(target_path, existing_acl_)) {
            LogError << "Failed to read configuration ACL" << VAR(target_path) << VAR(errno);
            return;
        }
#endif

        descriptor_ = ::open(temporary_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (descriptor_ < 0) {
            LogError << "Failed to create temporary configuration" << VAR(temporary_path_) << VAR(errno);
        }
#endif
    }

    bool valid() const
    {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ >= 0;
#endif
    }

    bool write(std::string_view content)
    {
        size_t offset = 0;
        while (offset < content.size()) {
#ifdef _WIN32
            const auto chunk_size = static_cast<DWORD>(std::min<size_t>(content.size() - offset, 64 * 1024));
            DWORD written = 0;
            if (!WriteFile(handle_, content.data() + offset, chunk_size, &written, nullptr) || written == 0) {
                LogError << "Failed to write temporary configuration" << VAR(temporary_path_) << VAR(GetLastError());
                return false;
            }
#else
            const ssize_t written = ::write(descriptor_, content.data() + offset, content.size() - offset);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LogError << "Failed to write temporary configuration" << VAR(temporary_path_) << VAR(errno);
                return false;
            }
            if (written == 0) {
                LogError << "Failed to write temporary configuration" << VAR(temporary_path_);
                return false;
            }
#endif
            offset += static_cast<size_t>(written);
        }
        return true;
    }

    bool finish()
    {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) {
            LogError << "Failed to flush temporary configuration" << VAR(temporary_path_) << VAR(GetLastError());
            return false;
        }
        if (!CloseHandle(handle_)) {
            handle_ = INVALID_HANDLE_VALUE;
            LogError << "Failed to close temporary configuration" << VAR(temporary_path_) << VAR(GetLastError());
            return false;
        }
        handle_ = INVALID_HANDLE_VALUE;
#else
        if (::fchmod(descriptor_, target_mode_) != 0) {
            LogError << "Failed to preserve configuration permissions" << VAR(temporary_path_) << VAR(errno);
            return false;
        }
#if defined(__APPLE__)
        if (existing_acl_ != nullptr && ::acl_set_fd(descriptor_, existing_acl_) != 0) {
            LogError << "Failed to preserve configuration ACL" << VAR(temporary_path_) << VAR(errno);
            return false;
        }
#elif defined(__linux__)
        if (!existing_acl_.empty() && ::fsetxattr(descriptor_, kPosixAclXattr, existing_acl_.data(), existing_acl_.size(), 0) != 0) {
            LogError << "Failed to preserve configuration ACL" << VAR(temporary_path_) << VAR(errno);
            return false;
        }
#endif
        if (::fsync(descriptor_) != 0) {
            LogError << "Failed to flush temporary configuration" << VAR(temporary_path_) << VAR(errno);
            return false;
        }
        if (::close(descriptor_) != 0) {
            descriptor_ = -1;
            LogError << "Failed to close temporary configuration" << VAR(temporary_path_) << VAR(errno);
            return false;
        }
        descriptor_ = -1;
#endif
        return true;
    }

    ~TemporaryConfigFile()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
#if defined(__APPLE__)
        if (existing_acl_ != nullptr) {
            ::acl_free(existing_acl_);
            existing_acl_ = nullptr;
        }
#endif
#endif
        remove_temporary_file(temporary_path_);
    }

private:
    std::filesystem::path temporary_path_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
    mode_t target_mode_ = 0600;
#if defined(__APPLE__)
    acl_t existing_acl_ = nullptr;
#elif defined(__linux__)
    std::string existing_acl_;
#endif
#endif
};

bool write_configuration(const Configuration& config, const std::filesystem::path& config_path)
{
    std::error_code error;
    auto link_status = std::filesystem::symlink_status(config_path, error);
    if (error) {
        LogError << "Failed to inspect configuration path" << VAR(config_path) << VAR(error.message());
        return false;
    }

    auto target_path = config_path;
    if (std::filesystem::is_symlink(link_status)) {
        target_path = std::filesystem::weakly_canonical(config_path, error);
        if (error) {
            LogError << "Failed to resolve configuration symlink" << VAR(config_path) << VAR(error.message());
            return false;
        }
    }

    const auto target_status = std::filesystem::status(target_path, error);
    if (error) {
        LogError << "Failed to inspect configuration target" << VAR(target_path) << VAR(error.message());
        return false;
    }
    if (std::filesystem::exists(target_status) && !std::filesystem::is_regular_file(target_status)) {
        LogError << "Configuration target is not a regular file" << VAR(target_path);
        return false;
    }

    auto temporary_path = target_path;
    temporary_path += ".tmp";

    std::ostringstream stream;
    stream << config.to_json();
    if (!stream.good()) {
        LogError << "Failed to serialize configuration" << VAR(temporary_path);
        return false;
    }
    const auto serialized = stream.str();

    TemporaryConfigFile temporary_file(target_path, temporary_path, std::filesystem::exists(target_status));
    if (!temporary_file.valid() || !temporary_file.write(serialized) || !temporary_file.finish()) {
        return false;
    }

    if (!Parser::parse_config(temporary_path)) {
        LogError << "failed to validate" << VAR(temporary_path);
        return false;
    }

#ifdef _WIN32
    if (!MoveFileExW(temporary_path.c_str(), target_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LogError << "failed to replace" << VAR(temporary_path) << VAR(target_path) << VAR(GetLastError());
        return false;
    }
#else
    std::filesystem::rename(temporary_path, target_path, error);
    if (error) {
        LogError << "failed to replace" << VAR(temporary_path) << VAR(target_path) << VAR(error.message());
        return false;
    }
#endif

    return true;
}

void merge_local_changes(const Configuration& baseline, const Configuration& local, Configuration& persisted)
{
    auto merge_field = [&]<typename Field>(Field Configuration::* field) {
        if (local.*field != baseline.*field) {
            persisted.*field = local.*field;
        }
    };

    merge_field(&Configuration::controller);
    merge_field(&Configuration::adb);
    merge_field(&Configuration::win32);
    merge_field(&Configuration::macos);
    merge_field(&Configuration::playcover);
    merge_field(&Configuration::gamepad);
    merge_field(&Configuration::lnx);
    merge_field(&Configuration::resource);
    merge_field(&Configuration::task);
    merge_field(&Configuration::global_option);
    merge_field(&Configuration::resource_option);
    merge_field(&Configuration::controller_option);
    merge_field(&Configuration::pretask);

    // Welcome snapshots have their own locked update path and are not ordinary
    // configuration edits.
}

} // namespace

bool Configurator::load(const std::filesystem::path& resource_dir, const std::filesystem::path& user_dir)
{
    LogFunc << VAR(resource_dir) << VAR(user_dir);

    auto data_opt = Parser::parse_interface(resource_dir / kInterfaceFilename);
    if (!data_opt) {
        LogError << "Failed to parse interface.json";
        return false;
    }
    data_ = *std::move(data_opt);
    if (data_.resource.empty()) {
        LogError << "Resource is empty";
        return false;
    }

    if (auto cfg_opt = Parser::parse_config(user_dir / kConfigPath)) {
        config_ = *std::move(cfg_opt);
        first_time_use_ = false;
        if (!transform_stored_passwords(data_.option, config_, SecretStore::decrypt)) {
            LogError << "Failed to decrypt stored password inputs";
            return false;
        }
    }
    else {
        first_time_use_ = true;
    }

    resource_dir_ = resource_dir;
    loaded_config_ = config_;

    // 加载翻译文件
    load_translations();

    return true;
}

bool Configurator::check_configuration()
{
    LogFunc;

    if (first_time_use_) {
        return true;
    }

    return Parser::check_configuration(data_, config_);
}

bool Configurator::save(const std::filesystem::path& user_dir)
{
    LogInfo << VAR(user_dir);

    ConfigFileLock lock(user_dir);
    if (!lock.valid()) {
        return false;
    }

    const auto config_path = user_dir / kConfigPath;
    auto persisted_config = config_;
    std::error_code error;
    if (std::filesystem::exists(config_path, error)) {
        auto latest_config = Parser::parse_config(config_path);
        if (!latest_config) {
            LogError << "Failed to reload configuration" << VAR(config_path);
            return false;
        }
        persisted_config = *std::move(latest_config);
        if (!transform_stored_passwords(data_.option, persisted_config, SecretStore::decrypt)) {
            LogError << "Failed to decrypt persisted password inputs";
            return false;
        }
    }
    else if (error) {
        LogError << "Failed to inspect configuration" << VAR(config_path) << VAR(error.message());
        return false;
    }

    merge_local_changes(loaded_config_, config_, persisted_config);

    auto stored_config = persisted_config;
    if (!transform_stored_passwords(data_.option, stored_config, SecretStore::encrypt)) {
        LogError << "Refusing to save configuration with an encryption failure";
        return false;
    }

    if (!write_configuration(stored_config, config_path)) {
        return false;
    }

    config_ = std::move(persisted_config);
    loaded_config_ = config_;
    return true;
}

std::optional<bool> Configurator::update_welcome_snapshots(
    const std::filesystem::path& user_dir,
    std::vector<std::string> declared,
    std::vector<std::string> resolved)
{
    const auto config_path = user_dir / kConfigPath;
    std::error_code error;
    const auto config_exists = std::filesystem::exists(config_path, error);
    if (error) {
        LogError << "Failed to inspect configuration" << VAR(config_path) << VAR(error.message());
        return std::nullopt;
    }
    if (!config_exists) {
        LogError << "Cannot update welcome snapshots before a configuration exists" << VAR(config_path);
        return std::nullopt;
    }

    ConfigFileLock lock(user_dir);
    if (!lock.valid()) {
        return std::nullopt;
    }

    auto persisted_config = Parser::parse_config(config_path);
    if (!persisted_config) {
        LogError << "Failed to reload configuration for welcome snapshots" << VAR(config_path);
        return std::nullopt;
    }

    auto latest_config = *std::move(persisted_config);
    if (!transform_stored_passwords(data_.option, latest_config, SecretStore::decrypt)) {
        LogError << "Failed to decrypt persisted password inputs for welcome snapshots";
        return std::nullopt;
    }

    if (latest_config.last_welcome == declared && latest_config.last_resolved_welcome == resolved) {
        auto& config = configuration();
        config.last_welcome = latest_config.last_welcome;
        config.last_resolved_welcome = latest_config.last_resolved_welcome;
        loaded_config_.last_welcome = config.last_welcome;
        loaded_config_.last_resolved_welcome = config.last_resolved_welcome;
        return false;
    }

    latest_config.last_welcome = std::move(declared);
    latest_config.last_resolved_welcome = std::move(resolved);

    auto stored_config = latest_config;
    if (!transform_stored_passwords(data_.option, stored_config, SecretStore::encrypt)) {
        LogError << "Refusing to save welcome snapshots with an encryption failure";
        return std::nullopt;
    }

    if (!write_configuration(stored_config, config_path)) {
        return std::nullopt;
    }

    auto& config = configuration();
    config.last_welcome = latest_config.last_welcome;
    config.last_resolved_welcome = latest_config.last_resolved_welcome;
    loaded_config_.last_welcome = config.last_welcome;
    loaded_config_.last_resolved_welcome = config.last_resolved_welcome;
    return true;
}

std::optional<RuntimeParam> Configurator::generate_runtime() const
{
    RuntimeParam runtime;

    auto resource_iter = std::ranges::find_if(data_.resource, [&](const auto& resource) { return resource.name == config_.resource; });

    if (resource_iter == data_.resource.end()) {
        LogWarn << "Resource not found";
        return std::nullopt;
    }

    for (const auto& path_string : resource_iter->path) {
        // v2: relative path from interface.json directory
        runtime.resource_path.emplace_back(resource_dir_ / MaaNS::path(path_string));
    }
    if (runtime.resource_path.empty()) {
        LogWarn << "No resource to load";
        return std::nullopt;
    }
    runtime.primary_resource_count = runtime.resource_path.size();
    runtime.resource_hash = resource_iter->hash;

    // Find current controller for attach_resource_path

    auto controller_iter =
        std::ranges::find_if(data_.controller, [&](const auto& controller) { return controller.name == config_.controller.name; });
    if (controller_iter == data_.controller.end()) {
        LogWarn << "Controller not found" << VAR(config_.controller.name);
        return std::nullopt;
    }
    auto& controller = *controller_iter;

    // Append attach_resource_path after resource.path
    for (const auto& attach_path : controller.attach_resource_path) {
        runtime.resource_path.emplace_back(resource_dir_ / MaaNS::path(attach_path));
    }

    for (const auto& config_task : config_.task) {
        auto data_task_iter = std::ranges::find_if(data_.task, [&](const auto& data_task) { return data_task.name == config_task.name; });
        if (data_task_iter != data_.task.end() && !is_task_applicable(*data_task_iter)) {
            continue;
        }

        auto task_opt = generate_runtime_task(config_task);
        if (!task_opt) {
            LogWarn << "failed to generate runtime, ignore" << VAR(config_task.name);
            continue;
        }
        runtime.task.emplace_back(*std::move(task_opt));
    }
    if (runtime.task.empty()) {
        LogWarn << "No task to run";
        return std::nullopt;
    }

    switch (controller.type) {
    case InterfaceData::Controller::Type::Adb: {
        RuntimeParam::AdbParam adb;

        adb.name = config_.adb.name;
        adb.adb_path = config_.adb.adb_path;
        adb.address = config_.adb.address;
        adb.agent_path = MaaNS::path_to_utf8_string(resource_dir_ / "MaaAgentBinary");

        runtime.controller_param = std::move(adb);
    } break;

    case InterfaceData::Controller::Type::Win32: {
        RuntimeParam::Win32Param win32;

        win32.hwnd = config_.win32.hwnd;

        // v2: parse from config, use default if not specified or invalid
        if (!controller.win32.screencap.empty()) {
            win32.screencap = parse_win32_screencap_method(controller.win32.screencap);
        }
        if (win32.screencap == MaaWin32ScreencapMethod_None) {
            win32.screencap = MaaWin32ScreencapMethod_DXGI_DesktopDup;
        }

        if (!controller.win32.mouse.empty()) {
            win32.mouse = parse_win32_input_method(controller.win32.mouse);
        }
        if (win32.mouse == MaaWin32InputMethod_None) {
            win32.mouse = MaaWin32InputMethod_Seize;
        }

        if (!controller.win32.keyboard.empty()) {
            win32.keyboard = parse_win32_input_method(controller.win32.keyboard);
        }
        if (win32.keyboard == MaaWin32InputMethod_None) {
            win32.keyboard = MaaWin32InputMethod_Seize;
        }

        runtime.controller_param = std::move(win32);
    } break;

    case InterfaceData::Controller::Type::MacOS: {
        RuntimeParam::MacOSParam macos;

        macos.window_id = config_.macos.window_id;

        // v2: parse from config, use default if not specified or invalid
        if (!controller.macos.screencap.empty()) {
            macos.screencap = parse_macos_screencap_method(controller.macos.screencap);
        }
        if (macos.screencap == MaaMacOSScreencapMethod_None) {
            macos.screencap = MaaMacOSScreencapMethod_ScreenCaptureKit;
        }

        if (!controller.macos.input.empty()) {
            macos.input = parse_macos_input_method(controller.macos.input);
        }
        if (macos.input == MaaMacOSInputMethod_None) {
            macos.input = MaaMacOSInputMethod_GlobalEvent;
        }

        runtime.controller_param = std::move(macos);
    } break;

    case InterfaceData::Controller::Type::PlayCover: {
        RuntimeParam::PlayCoverParam playcover;

        playcover.address = config_.playcover.address;
        playcover.uuid = config_.playcover.uuid.empty() ? "maa.playcover" : config_.playcover.uuid;

        if (playcover.address.empty()) {
            LogError << "PlayCover address is empty";
            return std::nullopt;
        }

        runtime.controller_param = std::move(playcover);
    } break;

    case InterfaceData::Controller::Type::Gamepad: {
        RuntimeParam::GamepadParam gamepad;

        gamepad.hwnd = config_.gamepad.hwnd;
        gamepad.gamepad_type =
            parse_gamepad_type(config_.gamepad.gamepad_type.empty() ? controller.gamepad.gamepad_type : config_.gamepad.gamepad_type);

        if (!controller.gamepad.screencap.empty()) {
            gamepad.screencap = parse_win32_screencap_method(controller.gamepad.screencap);
        }
        if (gamepad.screencap == MaaWin32ScreencapMethod_None && gamepad.hwnd != nullptr) {
            gamepad.screencap = MaaWin32ScreencapMethod_DXGI_DesktopDup;
        }

        runtime.controller_param = std::move(gamepad);
    } break;

    case InterfaceData::Controller::Type::Linux: {
        RuntimeParam::LinuxParam lnx;

        lnx.use_win32_vk_code = controller.lnx.use_win32_vk_code;
        lnx.pipewire_source = controller.lnx.pipewire_source.empty() ? "Gamescope" : controller.lnx.pipewire_source;

        if (!controller.lnx.screencap.empty()) {
            lnx.screencap = parse_linux_screencap_method(controller.lnx.screencap);
        }
        if (lnx.screencap == MaaLinuxScreencapMethod_None) {
            lnx.screencap = MaaLinuxScreencapMethod_Wlr;
        }

        if (!controller.lnx.input.empty()) {
            lnx.input = parse_linux_input_method(controller.lnx.input);
        }
        if (lnx.input == MaaLinuxInputMethod_None) {
            lnx.input = MaaLinuxInputMethod_Wlr;
        }

        lnx.wlr_socket_path = config_.lnx.wlr_socket_path;
        lnx.uinput_screen_width = config_.lnx.uinput_screen_width;
        lnx.uinput_screen_height = config_.lnx.uinput_screen_height;
        lnx.eis_socket_path = config_.lnx.eis_socket_path;

        runtime.controller_param = std::move(lnx);
    } break;

    default: {
        LogError << "Unknown controller type" << VAR(controller.type);
        return std::nullopt;
    }
    }

    // 设置分辨率配置
    runtime.display_config.short_side = controller.display_short_side;
    runtime.display_config.long_side = controller.display_long_side;
    runtime.display_config.expand = controller.display_expand;
    runtime.display_config.raw = controller.display_raw;

    for (const auto& pretask_config : Parser::flatten_pretask(data_.pretask)) {
        if (!pretask_config.resource.empty()
            && std::ranges::find(pretask_config.resource, config_.resource) == pretask_config.resource.end()) {
            continue;
        }
        if (!pretask_config.controller.empty()
            && std::ranges::find(pretask_config.controller, controller.name) == pretask_config.controller.end()) {
            continue;
        }

        RuntimeParam::Pretask runtime_pretask;
        runtime_pretask.name = pretask_identifier(pretask_config);
        runtime_pretask.exec = MaaNS::path(pretask_config.exec);
        runtime_pretask.args = pretask_config.args;
        runtime_pretask.cwd = resource_dir_;

        if (!pretask_config.option.empty()) {
            auto config_pretask_iter = std::ranges::find_if(config_.pretask, [&](const auto& config_pretask) {
                return config_pretask.name == runtime_pretask.name;
            });
            if (config_pretask_iter == config_.pretask.end()) {
                LogError << "Pretask config not found" << VAR(runtime_pretask.name);
                return std::nullopt;
            }

            json::object options;
            std::unordered_set<std::string> expanding;
            for (const auto& option_name : pretask_config.option) {
                if (!append_pretask_option(option_name, *config_pretask_iter, options, expanding)) {
                    LogError << "Failed to generate pretask option" << VAR(runtime_pretask.name) << VAR(option_name);
                    return std::nullopt;
                }
            }
            runtime_pretask.args.emplace_back(options.dumps());
        }

        runtime.pretask.emplace_back(std::move(runtime_pretask));
    }

    std::vector<InterfaceData::Agent> agents = std::visit(
        [](auto&& arg) -> std::vector<InterfaceData::Agent> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, InterfaceData::Agent>) {
                return { arg };
            }
            else {
                return arg;
            }
        },
        data_.agent);

    // v2.5.0: prepare PI_* env vars
    std::unordered_map<std::string, std::string> pi_env;
    pi_env["PI_INTERFACE_VERSION"] = "v2.6.0";
    pi_env["PI_CLIENT_NAME"] = "MaaPiCli";
    pi_env["PI_CLIENT_VERSION"] = MaaVersion();
    pi_env["PI_CLIENT_LANGUAGE"] = detect_system_language();
    pi_env["PI_CLIENT_MAAFW_VERSION"] = MaaVersion();
    if (!data_.version.empty()) {
        pi_env["PI_VERSION"] = data_.version;
    }

    // PI_CONTROLLER: serialize selected controller with i18n resolved
    {
        json::value ctrl_json = controller.to_json();
        if (ctrl_json.contains("label")) {
            ctrl_json["label"] = translate(controller.label);
        }
        if (ctrl_json.contains("description")) {
            ctrl_json["description"] = translate(controller.description);
        }
        pi_env["PI_CONTROLLER"] = ctrl_json.dumps();
    }

    // PI_RESOURCE: serialize selected resource with i18n resolved
    {
        json::value res_json = resource_iter->to_json();
        if (res_json.contains("label")) {
            res_json["label"] = translate(resource_iter->label);
        }
        if (res_json.contains("description")) {
            res_json["description"] = translate(resource_iter->description);
        }
        pi_env["PI_RESOURCE"] = res_json.dumps();
    }

    for (const auto& agent_config : agents) {
        if (agent_config.child_exec.empty()) {
            continue;
        }

        RuntimeParam::Agent agent;
        agent.child_exec = MaaNS::path(agent_config.child_exec);
        agent.child_args = agent_config.child_args;
        agent.identifier = agent_config.identifier;
        agent.cwd = resource_dir_;
        agent.env_vars = pi_env;

        runtime.agent.emplace_back(std::move(agent));
    }

    return runtime;
}

bool Configurator::is_option_applicable(const InterfaceData::Option& opt) const
{
    if (!opt.controller.empty() && std::ranges::find(opt.controller, config_.controller.name) == opt.controller.end()) {
        return false;
    }
    if (!opt.resource.empty() && std::ranges::find(opt.resource, config_.resource) == opt.resource.end()) {
        return false;
    }
    return true;
}

bool Configurator::is_task_applicable(const InterfaceData::Task& task) const
{
    if (!task.controller.empty() && std::ranges::find(task.controller, config_.controller.name) == task.controller.end()) {
        return false;
    }
    if (!task.resource.empty() && std::ranges::find(task.resource, config_.resource) == task.resource.end()) {
        return false;
    }
    return true;
}

void Configurator::merge_option_overrides(RuntimeParam::Task& runtime_task, const std::vector<Configuration::Option>& config_options) const
{
    for (const auto& config_option : config_options) {
        auto data_option_iter = data_.option.find(config_option.name);
        if (data_option_iter == data_.option.end()) {
            LogWarn << "option not found" << VAR(config_option.name);
            continue;
        }
        const auto& data_option = data_option_iter->second;

        if (!is_option_applicable(data_option)) {
            continue;
        }

        switch (data_option.type) {
        case InterfaceData::Option::Type::Select:
        case InterfaceData::Option::Type::Switch: {
            auto data_case_iter =
                std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == config_option.value; });
            if (data_case_iter == data_option.cases.end()) {
                LogWarn << "case not found" << VAR(config_option.value);
                continue;
            }
            runtime_task.pipeline_override.emplace(data_case_iter->pipeline_override);
        } break;

        case InterfaceData::Option::Type::Checkbox: {
            for (const auto& data_case : data_option.cases) {
                if (std::ranges::find(config_option.values, data_case.name) != config_option.values.end()) {
                    runtime_task.pipeline_override.emplace(data_case.pipeline_override);
                }
            }
        } break;

        case InterfaceData::Option::Type::Input: {
            std::string override_str = data_option.pipeline_override.dumps();
            for (const auto& input_def : data_option.inputs) {
                std::string placeholder = "{" + input_def.name + "}";
                std::string value;
                if (auto it = config_option.inputs.find(input_def.name); it != config_option.inputs.end()) {
                    value = it->second;
                }
                else {
                    value = input_def.default_;
                }

                switch (input_def.pipeline_type) {
                case InterfaceData::Option::Input::PipelineType::String:
                    override_str = MaaNS::string_replace_all(override_str, "\"" + placeholder + "\"", "\"" + value + "\"");
                    override_str = MaaNS::string_replace_all(override_str, placeholder, value);
                    break;
                case InterfaceData::Option::Input::PipelineType::Int:
                case InterfaceData::Option::Input::PipelineType::Bool:
                    override_str = MaaNS::string_replace_all(override_str, "\"" + placeholder + "\"", value);
                    override_str = MaaNS::string_replace_all(override_str, placeholder, value);
                    break;
                }
            }
            if (auto parsed = json::parse(override_str)) {
                runtime_task.pipeline_override.emplace(parsed->as_object());
            }
            else {
                LogWarn << "Failed to parse pipeline override JSON for input option" << VAR(config_option.name);
            }
        } break;
        }
    }
}

std::optional<RuntimeParam::Task> Configurator::generate_runtime_task(const Configuration::Task& config_task) const
{
    auto data_iter = std::ranges::find_if(data_.task, [&](const auto& data_task) { return data_task.name == config_task.name; });
    if (data_iter == data_.task.end()) {
        LogWarn << "task not found" << VAR(config_task.name);
        return std::nullopt;
    }
    const auto& data_task = *data_iter;

    RuntimeParam::Task runtime_task { .name = data_task.name,
                                      .entry = data_task.entry,
                                      .pipeline_override = json::array { data_task.pipeline_override } };

    // v2.3.0: merge in priority order (later overrides earlier)
    // 1. global_option (lowest priority)
    merge_option_overrides(runtime_task, config_.global_option);
    // 2. resource.option
    merge_option_overrides(runtime_task, config_.resource_option);
    // 3. controller.option
    merge_option_overrides(runtime_task, config_.controller_option);
    // 4. task.option (highest priority)
    merge_option_overrides(runtime_task, config_task.option);

    return runtime_task;
}

bool Configurator::append_pretask_option(
    const std::string& option_name,
    const Configuration::Pretask& config_pretask,
    json::object& options,
    std::unordered_set<std::string>& expanding) const
{
    if (!expanding.emplace(option_name).second) {
        LogError << "Recursive pretask option reference" << VAR(option_name);
        return false;
    }
    OnScopeLeave([&]() { expanding.erase(option_name); });

    auto data_option_iter = data_.option.find(option_name);
    if (data_option_iter == data_.option.end()) {
        LogError << "Pretask option not found" << VAR(option_name);
        return false;
    }
    const auto& data_option = data_option_iter->second;
    if (!is_option_applicable(data_option)) {
        return true;
    }

    auto config_option_iter = std::ranges::find_if(config_pretask.option, [&](const auto& option) { return option.name == option_name; });
    if (config_option_iter == config_pretask.option.end()) {
        LogError << "Pretask option config not found" << VAR(option_name);
        return false;
    }
    const auto& config_option = *config_option_iter;

    switch (data_option.type) {
    case InterfaceData::Option::Type::Select:
    case InterfaceData::Option::Type::Switch: {
        auto data_case_iter =
            std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == config_option.value; });
        if (data_case_iter == data_option.cases.end()) {
            LogError << "Pretask option case not found" << VAR(option_name) << VAR(config_option.value);
            return false;
        }

        options[option_name] = config_option.value;
        for (const auto& sub_option_name : data_case_iter->option) {
            if (!append_pretask_option(sub_option_name, config_pretask, options, expanding)) {
                return false;
            }
        }
    } break;

    case InterfaceData::Option::Type::Checkbox: {
        json::array checked_values;
        for (const auto& value : config_option.values) {
            auto data_case_iter = std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == value; });
            if (data_case_iter == data_option.cases.end()) {
                LogError << "Pretask option case not found" << VAR(option_name) << VAR(value);
                return false;
            }
            checked_values.emplace_back(value);
        }
        options[option_name] = std::move(checked_values);

        for (const auto& value : config_option.values) {
            auto data_case_iter = std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == value; });
            if (data_case_iter == data_option.cases.end()) {
                return false;
            }
            for (const auto& sub_option_name : data_case_iter->option) {
                if (!append_pretask_option(sub_option_name, config_pretask, options, expanding)) {
                    return false;
                }
            }
        }
    } break;

    case InterfaceData::Option::Type::Input: {
        json::object input_values;
        for (const auto& input_def : data_option.inputs) {
            std::string value = input_def.default_;
            if (auto value_iter = config_option.inputs.find(input_def.name); value_iter != config_option.inputs.end()) {
                value = value_iter->second;
            }
            input_values[input_def.name] = std::move(value);
        }
        options[option_name] = std::move(input_values);
    } break;
    }

    return true;
}

std::string Configurator::detect_system_language() const
{
    // 尝试获取系统语言
    std::string lang;

#ifdef _WIN32
    // Windows: 使用 GetUserDefaultLocaleName
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
        lang = MaaNS::from_u16(locale_name);
    }
#else
    // Unix-like: 使用环境变量
    if (const char* lc_all = std::getenv("LC_ALL"); lc_all && *lc_all) {
        lang = lc_all;
    }
    else if (const char* lc_messages = std::getenv("LC_MESSAGES"); lc_messages && *lc_messages) {
        lang = lc_messages;
    }
    else if (const char* language = std::getenv("LANG"); language && *language) {
        lang = language;
    }
#endif

    // 转换为小写并替换 - 为 _
    std::transform(lang.begin(), lang.end(), lang.begin(), [](unsigned char c) { return std::tolower(c); });
    std::replace(lang.begin(), lang.end(), '-', '_');

    // 提取语言代码 (例如 zh_cn.utf-8 -> zh_cn)
    if (auto pos = lang.find('.'); pos != std::string::npos) {
        lang = lang.substr(0, pos);
    }

    return lang;
}

void Configurator::load_translations()
{
    if (data_.languages.empty()) {
        return;
    }

    std::string sys_lang = detect_system_language();
    LogInfo << "System language:" << sys_lang;

    // 查找匹配的语言文件
    std::string translation_file;

    // 1. 精确匹配 (例如 zh_cn)
    if (auto it = data_.languages.find(sys_lang); it != data_.languages.end()) {
        translation_file = it->second;
    }
    // 2. 语言前缀匹配 (例如 zh_cn 匹配 zh)
    else if (auto pos = sys_lang.find('_'); pos != std::string::npos) {
        std::string lang_prefix = sys_lang.substr(0, pos);
        if (auto prefix_it = data_.languages.find(lang_prefix); prefix_it != data_.languages.end()) {
            translation_file = prefix_it->second;
        }
    }
    // 3. 尝试匹配任何以系统语言开头的语言（仅当 sys_lang 非空时）
    // 注意：starts_with("") 会匹配任何字符串，所以必须检查 sys_lang 是否为空
    if (translation_file.empty() && sys_lang.size() >= 2) {
        std::string lang_prefix = sys_lang.substr(0, 2);
        for (const auto& [lang_code, file] : data_.languages) {
            if (lang_code.starts_with(lang_prefix)) {
                translation_file = file;
                break;
            }
        }
    }

    if (translation_file.empty()) {
        LogInfo << "No matching translation file found for language:" << sys_lang;
        return;
    }

    // 加载翻译文件
    auto translation_path = resource_dir_ / translation_file;
    LogInfo << "Loading translation file:" << translation_path;

    std::ifstream ifs(translation_path);
    if (!ifs.is_open()) {
        LogWarn << "Failed to open translation file:" << translation_path;
        return;
    }

    std::stringstream buffer;
    buffer << ifs.rdbuf();
    auto json_opt = json::parse(buffer.str());
    if (!json_opt || !json_opt->is_object()) {
        LogWarn << "Failed to parse translation file:" << translation_path;
        return;
    }

    for (const auto& [key, value] : json_opt->as_object()) {
        if (value.is_string()) {
            translations_[key] = value.as_string();
        }
    }

    LogInfo << "Loaded" << translations_.size() << "translations";
}

std::string Configurator::translate(const std::string& text) const
{
    if (text.empty()) {
        return text;
    }

    // 如果文本以 $ 开头，则从翻译表中查找
    if (text[0] == '$') {
        std::string key = text.substr(1); // 去掉 $ 前缀
        if (auto it = translations_.find(key); it != translations_.end()) {
            return it->second;
        }
        // 如果没找到翻译，返回去掉 $ 的原文
        return key;
    }

    return text;
}

MAA_PROJECT_INTERFACE_NS_END
