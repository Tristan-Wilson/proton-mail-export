// Copyright (c) 2023 Proton AG
//
// This file is part of Proton Export Tool.
//
// Proton Mail Bridge is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Proton Mail Bridge is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Proton Export Tool.  If not, see <https://www.gnu.org/licenses/>.

#include <atomic>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include <cxxopts.hpp>
#include <et.hpp>
#include <etconfig.hpp>
#include <etlog.hpp>
#include <etsession.hpp>
#include <etutil.hpp>

#include "operation.h"
#include "task_runner.hpp"
#include "tasks/backup_task.hpp"
#include "tasks/global_task.hpp"
#include "tasks/restore_task.hpp"
#include "tasks/session_task.hpp"
#include "tui_util.hpp"

#if defined(__APPLE__)
#include "macos.hpp"
#endif

constexpr int kNumInputRetries = 3;
constexpr const char* kReportTag = "cli";
static std::atomic_bool gShouldQuit = std::atomic_bool(false);
static std::atomic_bool gConnectionActive = std::atomic_bool(true);

inline uint64_t toMB(uint64_t value) {
    return value / 1024 / 1024;
}

class ReadInputException final : public etcpp::Exception {
public:
    explicit ReadInputException(std::string_view what) : etcpp::Exception(what) {}
};

template<class C>
inline auto readLine(C& stream, std::string_view label) {
    std::conditional_t<std::is_same_v<C, decltype(std::cin)>, std::string, std::wstring> result;
    std::cout << label << ": " << std::flush;
    std::getline(stream, result);
    if (stream.eof()) {
#if defined(_WIN32)
        // On win32 console, Ctrl+C will close the input stream before the signal
        // handler is run. To avoid always seeing the ReadInputException, just
        // throw cancel all the time.
        throw etcpp::CancelledException();
#else
        if (gShouldQuit) {
            throw etcpp::CancelledException();
        } else {
            throw ReadInputException(fmt::format("Failed read value for '{}'", label));
        }
#endif
    }

    return result;
}

std::string readText(std::string_view label) {
    for (int i = 0; i < kNumInputRetries; i++) {
        std::string result = readLine(std::cin, label);
        if (result.empty()) {
            std::cerr << "Value can't be empty" << std::endl;
            continue;
        }

        return result;
    }

    throw ReadInputException(fmt::format("Failed read value for '{}'", label));
}

#if defined(_WIN32)
struct Win32UTF16InputScope {
    int prevMode;
    Win32UTF16InputScope() { prevMode = _setmode(_fileno(stdin), _O_U16TEXT); }
    ~Win32UTF16InputScope() { _setmode(_fileno(stdin), prevMode); }
};

#endif

std::filesystem::path readPath(std::string_view label) {
#if defined(_WIN32)
    // We need to force the Win32 Console to read the input as Utf16, otherwise it will
    // ignore the remaing code points and we get garbage data.
    Win32UTF16InputScope modeScope;
#define stdindef std::wcin
#define mkpath(S) std::filesystem::path(S)
#else
#define stdindef std::cin
#define mkpath(S) std::filesystem::u8path(S)
#endif

    for (int i = 0; i < kNumInputRetries; i++) {
        auto result = readLine(stdindef, label);
        if (result.empty()) {
            std::cerr << "Value can't be empty" << std::endl;
            continue;
        }

        auto utf8path = mkpath(result);
        auto expandedPath = etcpp::expandCLIPath(utf8path);

        try {
            if (std::filesystem::exists(expandedPath) && !std::filesystem::is_directory(expandedPath)) {
                std::cerr << "Path is not a directory" << std::endl;
                continue;
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to check export utf8path:" << e.what() << std::endl;
            continue;
        }

        return expandedPath;
    }

    throw ReadInputException(fmt::format("Failed read value for '{}'", label));
#undef stdindef
#undef mkpath
}

std::string readSecret(std::string_view label) {
    struct PasswordScope {
        PasswordScope() { setStdinEcho(false); }

        ~PasswordScope() {
            setStdinEcho(true);
            std::cout << std::endl;
        }
    };

    PasswordScope pscope;

    for (int i = 0; i < kNumInputRetries; i++) {
        std::string result = readLine(std::cin, label);
        if (result.empty()) {
            std::cerr << "Value can't be empty" << std::endl;
            continue;
        }

        return result;
    }

    throw ReadInputException(fmt::format("Failed read value for '{}'", label));
}

bool readYesNo(std::string_view label) {
    for (int i = 0; i < kNumInputRetries; i++) {
        std::string result = readLine(std::cin, label);

        if (result.empty()) {
            std::cerr << "Value can't be empty" << std::endl;
            continue;
        }

        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });

        if (result == "y" || result == "yes") {
            return true;
        } else if (result == "n" || result == "no") {
            return false;
        } else {
            std::cerr << "Value must be one of: Y, y, Yes, yes, N, n, No, no" << std::endl;
        }
    }

    throw ReadInputException(fmt::format("Failed read value for '{}'", label));
}

std::string readOperation(std::string_view label) {
    for (int i = 0; i < kNumInputRetries; i++) {
        std::string result = readLine(std::cin, label);

        if (result.empty()) {
            std::cerr << "Value can't be empty" << std::endl;
            continue;
        }

        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });

        if (result == "b" || result == backupStr) {
            return "backup";
        }
        if (result == "r" || result == restoreStr) {
            return "restore";
        }

        std::cerr << "Value must be one of: b, B, Backup, backup, R, r, Restore, restore" << std::endl;
    }

    throw ReadInputException(fmt::format("Failed read value for '{}'", label));
}

void waitForEnter(std::string_view label) {
    std::string result;
    std::cout << label << ": " << std::flush;
    std::getline(std::cin, result);
}

template<class F>
std::string getCLIValue(cxxopts::ParseResult& parseResult, const char* argKey, std::optional<const char*> envVariable, F fallback) {
    static_assert(std::is_invocable_r_v<std::string, F>);

    std::string result;
    if (parseResult.count(argKey)) {
        result = parseResult[argKey].as<std::string>();
    }
    if (!result.empty()) {
        return result;
    }

    if (envVariable) {
        const auto envVar = std::getenv(*envVariable);
        if (envVar != nullptr && std::strlen(envVar) != 0) {
            return envVar;
        }
    }

    return fallback();
}

class SessionCallback final : public etcpp::SessionCallback {
public:
    void onNetworkLost() override { gConnectionActive.store(false); }
    void onNetworkRestored() override { gConnectionActive.store(true); }
};

class CLIAppState final : public TaskAppState {
public:
    bool shouldQuit() const override { return gShouldQuit.load(); }

    bool networkLost() const override { return !gConnectionActive.load(); }
};

std::filesystem::path getOutputPath() {
#if !defined(__APPLE__)
    std::filesystem::path execPath;
    try {
        execPath = etcpp::getExecutableDir();
    } catch (const std::exception& e) {
        std::cerr << "Failed to get executable directory: " << e.what() << std::endl;
        std::cerr << "Will user working directory instead" << std::endl;
    }

    return execPath;
#else
    return getMacOSDownloadsDir() / "proton-mail-export-cli";
#endif
}


std::string getExampleDir() {
#if defined(_WIN32)
    return "%USERPROFILE%\\Documents";
#else
    return "~/Documents";
#endif
}

std::filesystem::path getBackupPath(cxxopts::ParseResult const& argParseResult, std::string_view const email, bool& outPathCameFromArg,
                                    bool& usingDefaultBackupPath) {
    std::filesystem::path backupPath;
    outPathCameFromArg = false;
    bool promptEntry = false;
    if (argParseResult.count("dir")) {
        auto const argPath = argParseResult["dir"].as<std::string>();
        if (!argPath.empty()) {
            backupPath = etcpp::expandCLIPath(std::filesystem::u8path(argPath));
        }
        outPathCameFromArg = true;
    }

    std::filesystem::path const outputPath = getOutputPath();
    if (backupPath.empty()) {
        const auto defaultPath = getOutputPath() / email;
        std::cout << "\nBy default, the export will be made in:\n\n"
                  << defaultPath << "\n\nType 'Yes' to continue or 'No' to specify another path.\n"
                  << std::endl;

        promptEntry = !readYesNo("Do you wish to proceed?");
    }

    while (true) {
        if (promptEntry) {
            std::cout << "Please input desired export path. E.g.: " << getExampleDir() << std::endl;
            backupPath = readPath("Export Path");
            usingDefaultBackupPath = false;
        } else if (backupPath.empty()) {
            backupPath = outputPath;
        }

        if (backupPath.is_relative()) {
            backupPath = outputPath / backupPath;
        }

        try {
            std::filesystem::create_directories(backupPath);
            return backupPath;
        } catch (const std::exception& e) {
            etcpp::logError("Failed to create export directory '{}': {}", backupPath.u8string(), e.what());
            std::cerr << "Failed to create export directory '" << backupPath << "': " << e.what() << std::endl;
            if (outPathCameFromArg) {
                return {};
            }
        }
    }
}

std::filesystem::path getRestorePath(cxxopts::ParseResult const& argParseResult, bool& outPathCameFromArgOrEnv) {
    std::filesystem::path backupPath;
    outPathCameFromArgOrEnv = false;
    std::string argPath;
    if (argParseResult.count("dir")) {
        argPath = argParseResult["dir"].as<std::string>();
    }

    if (argPath.empty()) {
        const auto envVar = std::getenv("ET_DIR");
        if (envVar != nullptr && std::strlen(envVar) != 0) {
            argPath = std::string(envVar);
        }
    }

    if (!argPath.empty()) {
        backupPath = etcpp::expandCLIPath(std::filesystem::u8path(argPath));
        outPathCameFromArgOrEnv = true;
        return backupPath;
    }

    while (true) {
        std::cout << "Please specify the path of the backup folder. E.g.: " << getExampleDir() << std::endl;
        backupPath = readPath("Backup Path");

        if (backupPath.is_relative()) {
            backupPath = getOutputPath() / backupPath;
        }

        if (!std::filesystem::exists(backupPath)) {
            std::cerr << "The specified path does not exist" << std::endl;
            continue;
        }

        if (!std::filesystem::is_directory(backupPath)) {
            std::cerr << "The specified path is not a directory" << std::endl;
            continue;
        }

        return backupPath;
    }
}

std::optional<int> performLogin(etcpp::Session& session, cxxopts::ParseResult& argParseResult, CLIAppState& appState) {
    etcpp::Session::LoginState loginState = etcpp::Session::LoginState::LoggedOut;

    constexpr int kMaxNumLoginAttempts = 3;
    int numLoginAttempts = 0;

    std::string loginUsername;
    std::string loginPassword;

    while (loginState != etcpp::Session::LoginState::LoggedIn) {
        if (gShouldQuit) {
            return EXIT_SUCCESS;
        }

        if (numLoginAttempts >= kMaxNumLoginAttempts) {
            std::cerr << "Failed to login: Max attempts reached" << std::endl;
            return EXIT_FAILURE;
        }

        switch (loginState) {
        case etcpp::Session::LoginState::LoggedOut:
        {
            auto username = getCLIValue(argParseResult, "user", "ET_USER_EMAIL", []() { return readText("Username"); });
            if (gShouldQuit) {
                return EXIT_SUCCESS;
            }

            auto password = getCLIValue(argParseResult, "password", "ET_USER_PASSWORD", []() { return readSecret("Password"); });

            try {
                auto task = LoginSessionTask(session, "Logging In",
                                             [&](etcpp::Session& s) -> etcpp::Session::LoginState { return s.login(username.c_str(), password); });
                loginState = runTask(appState, task);
                loginUsername = std::move(username);
                loginPassword = std::move(password);
            } catch (const etcpp::SessionException& e) {
                std::cerr << "Failed to login: " << e.what() << std::endl;
                numLoginAttempts += 1;
                continue;
            }

            numLoginAttempts = 0;
            break;
        }
        case etcpp::Session::LoginState::AwaitingTOTP:
        {
            const auto totp =
                getCLIValue(argParseResult, "totp", "ET_TOTP_CODE", []() { return readSecret("Enter the code from your authenticator app"); });
            if (gShouldQuit) {
                return EXIT_SUCCESS;
            }
            try {
                auto task = LoginSessionTask(session, "Submitting 2FA Code",
                                             [&](etcpp::Session& s) -> etcpp::Session::LoginState { return s.loginTOTP(totp.c_str()); });
                loginState = runTask(appState, task);
            } catch (const etcpp::SessionException& e) {
                std::cerr << "Failed to submit 2FA code: " << e.what() << std::endl;
                numLoginAttempts += 1;
                continue;
            }

            numLoginAttempts = 0;
            break;
        }
        case etcpp::Session::LoginState::AwaitingHV:
        {
            const auto hvUrl = session.getHVSolveURL();

            std::cout << "\nHuman Verification requested. Please open the URL below in a "
                         "browser and"
                      << " press ENTER when the challenge has been completed.\n\n"
                      << hvUrl << '\n'
                      << std::endl;

            waitForEnter("Press ENTER to continue");
            if (gShouldQuit) {
                return EXIT_SUCCESS;
            }

            try {
                loginState = session.markHVSolved();
                // Auto-retry login with existing information if the HV was triggered during
                // login.
                if (loginState == etcpp::Session::LoginState::LoggedOut) {
                    auto task = LoginSessionTask(
                        session, "Retrying login after Human Verification request",
                        [&](etcpp::Session& s) -> etcpp::Session::LoginState { return s.login(loginUsername.c_str(), loginPassword); });
                    loginState = runTask(appState, task);
                }
                if (loginState == etcpp::Session::LoginState::AwaitingHV) {
                    numLoginAttempts += 1;
                    continue;
                }
            } catch (const etcpp::SessionException& e) {
                std::cerr << "Failed to login: " << e.what() << std::endl;
                numLoginAttempts += 1;
                continue;
            }

            numLoginAttempts = 0;
            break;
        }
        case etcpp::Session::LoginState::AwaitingMailboxPassword:
        {
            const auto mboxPassword =
                getCLIValue(argParseResult, "mbox-password", "ET_USER_MAILBOX_PASSWORD", []() { return readSecret("Mailbox Password"); });
            if (gShouldQuit) {
                return EXIT_SUCCESS;
            }

            try {
                loginState = session.loginMailboxPassword(mboxPassword);
            } catch (const etcpp::SessionException& e) {
                std::cerr << "Failed to set mailbox password: " << e.what() << std::endl;
                numLoginAttempts += 1;
                continue;
            }

            numLoginAttempts = 0;
            break;
        }
        default:
        {
            const auto msg = fmt::format("Encountered unexpected login state: {:x}", static_cast<uint32_t>(loginState));
            etcpp::GlobalScope::reportError(kReportTag, msg.c_str());
            std::cerr << msg << std::endl;
            return EXIT_FAILURE;
        }
        }
    }
    return std::nullopt;
}


int performBackup(etcpp::Session& session, cxxopts::ParseResult const& argParseResult, CLIAppState const& appState) {
    bool pathCameFromArgs = false;
    bool usingDefaultBackupPath = true;
    std::filesystem::path const backupPath = getBackupPath(argParseResult, session.getEmail(), pathCameFromArgs, usingDefaultBackupPath);
    if (backupPath.empty()) {
        return EXIT_FAILURE;
    }

    // Telemetry - we'd like to know whether the user overwrote the default export path
    session.setUsingDefaultExportPath(!pathCameFromArgs && usingDefaultBackupPath);

    std::filesystem::space_info spaceInfo{};
    try {
        spaceInfo = std::filesystem::space(backupPath);
    } catch (const std::exception& e) {
        etcpp::logError("Failed to get free space info: {}", e.what());
        std::cerr << "Failed to get free space info: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    std::unique_ptr<BackupTask> backupTask;
    try {
        backupTask = std::make_unique<BackupTask>(session, backupPath);
    } catch (const etcpp::SessionException& e) {
        etLogError("Failed to create export task: {}", e.what());
        std::cerr << "Failed to create export task: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    uint64_t expectedSpace = 0;
    try {
        expectedSpace = backupTask->getExpectedDiskUsage();
    } catch (const etcpp::BackupException& e) {
        std::cerr << "Could not get expected disk usage: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    if (expectedSpace > spaceInfo.available) {
        std::cout << "\nThis operation requires at least " << toMB(expectedSpace) << " MB of free space, but the destination volume only has "
                  << toMB(spaceInfo.available) << " MB available. " << std::endl
                  << "Type 'Yes' to continue or 'No' to abort in the prompt below.\n"
                  << std::endl;

        if (!readYesNo("Do you wish to proceed?")) {
            return EXIT_SUCCESS;
        }
    }

    std::cout << "Starting Export - Path=" << backupTask->getExportPath() << std::endl;
    try {
        runTaskWithProgress(appState, *backupTask);
    } catch (const etcpp::BackupException& e) {
        etcpp::logError("Failed to export : {}", e.what());
        std::cerr << "Failed to export: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Export Finished" << std::endl;
    return EXIT_SUCCESS;
}

void printRestoreStats(RestoreTask const& task) {
    std::cout << "Importable emails: " << task.getImportableCount() << std::endl;
    std::cout << "Successful imports: " << task.getImportedCount() << std::endl;
    std::cout << "Failed imports: " << task.getFailedCount() << std::endl;
    std::cout << "Skipped imports: " << task.getSkippedCount() << std::endl;
}

int performRestore(etcpp::Session& session, cxxopts::ParseResult const& argParseResult, CLIAppState const& appState) {
    std::filesystem::path backupPath;
    bool pathCameFromArgs = false;
    try {
        backupPath = getRestorePath(argParseResult, pathCameFromArgs);
    } catch (std::exception const& e) {
        etcpp::logError("Failed to access backup directory '{}': {}", backupPath.u8string(), e.what());
        std::cerr << "Failed to access backup directory '" << backupPath << "': " << e.what() << std::endl;
        if (pathCameFromArgs) {
            return EXIT_FAILURE;
        }
    }

    std::unique_ptr<RestoreTask> restoreTask;
    try {
        restoreTask = std::make_unique<RestoreTask>(session, backupPath);
    } catch (const etcpp::SessionException& e) {
        etLogError("Failed to create export task: {}", e.what());
        std::cerr << "Failed to create export task: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "Starting Restore - Path=" << restoreTask->getExportPath() << std::endl;

    try {
        runTaskWithProgress(appState, *restoreTask);
    } catch (const etcpp::RestoreException& e) {
        etcpp::logError("Failed to restore : {}", e.what());
        std::cerr << "Failed to restore: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Restore Finished" << std::endl;
    printRestoreStats(*restoreTask);
    return EXIT_SUCCESS;
}

int main(int argc, const char** argv) {
#if defined(_WIN32)
    // Ensure Win32 Console correctly processes utf8 characters.
    setlocale(LC_ALL, "en_US.UTF-8");
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    CLIAppState appState = CLIAppState();
    std::cout << "Proton Mail Export Tool (" << et::VERSION_STR << ") (c) Proton AG, Switzerland\n"
              << "This program is licensed under the GNU General Public License v3\n"
              << "Get support at https://proton.me/support/proton-mail-export-tool" << std::endl;
    std::filesystem::path outputPath = getOutputPath();

    if (!registerCtrlCSignalHandler([]() {
            if (!gShouldQuit) {
                std::cout << std::endl << "Received Ctrl+C, exiting as soon as possible" << std::endl;
                gShouldQuit.store(true);
#if !defined(_WIN32)
                // We need to reset the printing of chars by stdin here. As soon as we close stdin
                // to force the input reading to exit, we can't apply any more changes.
                setStdinEcho(true);
#endif
                fclose(stdin);
            }
        })) {
        std::cerr << "Failed to register signal handler";
        return EXIT_FAILURE;
    }

    try {
        auto logDir = outputPath / "logs";
        auto globalScope = etcpp::GlobalScope(logDir, []() {
            std::cerr << "\n\nThe application ran into an unrecoverable error, please consult the "
                         "log for more details."
                      << std::endl;
            exit(-1);
        });

        cxxopts::Options options("proton-mail-export-cli");

        options.add_options()("o,operation", "operation to perform, backup or restore (can also be set with env var ET_OPERATION)",
                              cxxopts::value<std::string>())("d,dir", "Backup/restore directory (can also be set with env var ET_DIR)",
                                                             cxxopts::value<std::string>())(
            "p,password", "User's password (can also be set with env var ET_USER_PASSWORD)", cxxopts::value<std::string>())(
            "m,mbox-password", "User's mailbox password when using 2 Password Mode (can also be set with env var ET_USER_MAILBOX_PASSWORD)",
            cxxopts::value<std::string>())("t,totp", "User's TOTP 2FA code (can also be set with env var ET_TOTP_CODE)",
                                           cxxopts::value<std::string>())(
            "u,user", "User's account/email (can also be set with env var ET_USER_EMAIL", cxxopts::value<std::string>())(
            "k, telemetry", "Disable anonymous telemetry statistics (can also be set with env var ET_TELEMETRY_OFF)", cxxopts::value<bool>())(
            "h,help", "Show help");

        auto argParseResult = options.parse(argc, argv);

        if (argParseResult.count("help")) {
            std::cout << options.help() << std::endl;
            return EXIT_SUCCESS;
        }

        try {
            std::cout << '\n';
            auto task = NewVersionCheckTask(globalScope, "Checking for new version");
            if (runTask(appState, task)) {
                std::cout << "A new version is available at: "
                             "https://proton.me/support/proton-mail-export-tool"
                          << std::endl;
            } else {
                std::cout << "The export tool is up to date" << std::endl;
            }
        } catch (const std::exception&) {
        }

        if (const auto& logPath = globalScope.getLogPath(); logPath) {
            std::cout << "\nSession Log: " << *logPath << '\n' << std::endl;
        }

        bool telemetryDisabled = argParseResult["telemetry"].as<bool>() || (std::getenv("ET_TELEMETRY_OFF") != nullptr);

        etcpp::Session session = etcpp::Session(et::DEFAULT_API_URL, telemetryDisabled, std::make_shared<SessionCallback>());

        // Unauth telemetry
        session.sendProcessStartTelemetry(argParseResult.count("operation") || (std::getenv("ET_OPERATION") != nullptr),
                                          argParseResult.count("dir") || (std::getenv("ET_DIR") != nullptr),
                                          argParseResult.count("password") || (std::getenv("ET_USER_PASSWORD") != nullptr),
                                          argParseResult.count("mbox-password") || (std::getenv("ET_USER_MAILBOX_PASSWORD") != nullptr),
                                          argParseResult.count("totp") || (std::getenv("ET_TOTP_CODE") != nullptr),
                                          argParseResult.count("user") || (std::getenv("ET_USER_EMAIL") != nullptr));


        std::optional<int> exitCode = performLogin(session, argParseResult, appState);
        if (exitCode.has_value()) {
            return *exitCode;
        }

        std::string operationStr =
            getCLIValue(argParseResult, "operation", "ET_OPERATION", [] { return readOperation("Operation ((B)ackup/(R)estore))"); });
        if (gShouldQuit) {
            return EXIT_SUCCESS;
        }
        EOperation const operation = stringToOperation(operationStr);
        if (EOperation::Unknown == operation) {
            std::cerr << "Could not determine operation to perform (" + operationStr + ")" << std::endl;
            return EXIT_FAILURE;
        }

        switch (operation) {
        case EOperation::Backup:
            return performBackup(session, argParseResult, appState);
            break;
        case EOperation::Restore:
            return performRestore(session, argParseResult, appState);
            break;
        default:
            throw etcpp::Exception("Could not determine operation to perform (" + operationStr + ")");
        }
    } catch (const etcpp::CancelledException&) {
        return EXIT_SUCCESS;
    } catch (const ReadInputException& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (const etcpp::KillSwitchException& e) {
        etcpp::logInfo("Kill switch enabled.");
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        const auto str = fmt::format("Encountered unexpected error: {}", e.what());
        etcpp::logError("Encountered unexpected error: {}", e.what());
        etcpp::GlobalScope::reportError(kReportTag, str.c_str());
        std::cerr << str << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}