#pragma once
#include "skud/Types.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace skud {
class Config;
class MariaDbUserStore;
class FileStore {
public:
    explicit FileStore(std::string root, Config* cfg=nullptr);
    ~FileStore();
    bool init(std::string& error);
    bool appendEvent(const AttendanceEvent& e);
    bool hasControllerEvent(int controller_node,const std::string& raw_hex,const std::string& event_timestamp={}) const;
    bool saveCardStates(const std::map<std::string,PersistedCardState>& states);
    std::map<std::string,PersistedCardState> loadCardStates() const;
    bool saveActivities(const std::vector<CardActivity>& activities);
    std::vector<CardActivity> loadActivities() const;
    // Full persisted event stream for one controller-reported calendar date.
    // Used by extended attendance reports to preserve every arrival/departure.
    std::vector<AttendanceEvent> loadAttendanceEventsByDate(const std::string& date) const;
    std::vector<DailyAttendance> loadDailyAttendance(const std::string& date) const;
    std::vector<std::string> attendanceDates() const;
    bool backupFile(const std::string& path) const;
    bool usingMariaDb() const;
    std::string storageError() const;
private:
    bool migrateLegacyRuntime(std::string& error);
    bool backupAndRemove(const std::string& path,const std::string& label,std::string& error) const;
    std::string root_;
    Config* cfg_{};
    std::unique_ptr<MariaDbUserStore> db_;
    mutable std::mutex storage_mu_;
    mutable std::string storage_error_;
    mutable std::mutex mu_;
};
}
