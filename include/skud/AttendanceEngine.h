#pragma once
#include "skud/FileStore.h"
#include "skud/Types.h"
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace skud {
class UserManager;
class AttendanceEngine {
public:
    using NotifyFn = std::function<void(const AttendanceEvent&)>;
    struct ControllerEventProcessResult {
        AttendanceEvent event;
        bool stored{false};      // true when already present or successfully persisted
        bool duplicate{false};   // same controller RAW frame was already stored
    };
    AttendanceEngine(UserManager& users, FileStore& store, int repeat_seconds);
    void setNotifier(NotifyFn fn);
    AttendanceEvent onCardRead(const std::string& card, int controller_node, const std::string& controller_name, const std::string& raw_hex="");
    ControllerEventProcessResult onControllerAccessEvent(const std::string& card,int controller_node,const std::string& controller_name,const std::string& raw_hex,const std::string& event_timestamp,bool allow_notification=true);
    void recordRawControllerEvent(int controller_node,const std::string& controller_name,const std::string& raw_hex);
    ControllerEventProcessResult recordControllerRawEvent(int controller_node,const std::string& controller_name,const std::string& raw_hex,const std::string& event_timestamp,const std::string& card="");
    std::vector<CardActivity> activities() const;
    std::vector<User> presentUsers() const;
    std::vector<DailyAttendance> todayAttendance() const;
    void refreshUserMetadata();
    bool resetSiteActivity();
    // At the first tick of a new local day, close every user still marked
    // present. The synthetic departure uses the last real card-read timestamp
    // from the previous day, then resets durable presence to Absent.
    bool processMidnightRollover();
private:
    struct State { PresenceState presence{PresenceState::Absent}; std::chrono::system_clock::time_point last_read{}; bool has_last{false}; std::string last_read_text; };
    static std::chrono::system_clock::time_point parseTime(const std::string& s, bool& ok);
    void persistLocked();
    UserManager& users_;
    FileStore& store_;
    int repeat_seconds_;
    mutable std::mutex mu_;
    std::map<std::string,State> states_;
    std::map<std::string,CardActivity> activities_;
    NotifyFn notifier_;
};
}
