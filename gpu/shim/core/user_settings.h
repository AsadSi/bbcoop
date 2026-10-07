// bbcoop: shadPS4's user manager reduced to bbport's single local user (user 1, the one
// runtime_services.c logs in). shadNet credentials come from the environment:
//   BB_SHADNET_NPID (default: BB_USER_NAME, else "Hunter") and BB_SHADNET_PASSWORD.
#pragma once
#include <array>
#include <cstdlib>
#include <string>
#include "common/types.h"
#include "core/emulator_settings.h"

struct User {
    s32 user_id = -1;
    std::string user_name;
    u32 user_color = 1;
    int player_index = 0;
    bool logged_in = false;
    std::string shadnet_npid;
    std::string shadnet_password;
    std::string shadnet_token;
    std::string shadnet_email;
    bool shadnet_enabled = false;
    std::string np_country = "us";
    std::string np_language = "en";
    u8 np_age = 30;
    std::string np_date_of_birth = "1994-01-01";
};

using LoggedInUsers = std::array<User*, 4>;

class UserManager {
public:
    UserManager() {
        auto env = [](const char* name, const char* fallback) {
            const char* value = std::getenv(name);
            return std::string(value && *value ? value : fallback);
        };
        user.user_id = 1;
        user.player_index = 1;
        user.logged_in = true;
        user.user_name = env("BB_USER_NAME", "Hunter");
        user.shadnet_npid = env("BB_SHADNET_NPID", user.user_name.c_str());
        user.shadnet_password = env("BB_SHADNET_PASSWORD", "");
        user.shadnet_enabled = EmulatorSettings.IsShadNetEnabled();
    }
    User* GetUserByID(s32 user_id) { return user_id == user.user_id ? &user : nullptr; }
    LoggedInUsers GetLoggedInUsers() const { return {const_cast<User*>(&user), nullptr, nullptr, nullptr}; }

private:
    User user;
};

class UserSettingsImpl {
public:
    static UserSettingsImpl* GetInstance() { static UserSettingsImpl s; return &s; }
    UserManager& GetUserManager() { return manager; }

private:
    UserManager manager;
};

#define UserSettings (*UserSettingsImpl::GetInstance())
#define UserManagement UserSettings.GetUserManager()
