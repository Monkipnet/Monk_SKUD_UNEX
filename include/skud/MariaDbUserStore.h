#pragma once
#include "skud/Types.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace skud {
class Config;
// Shared MariaDB persistence implementation. The historical class name is kept
// for source compatibility with v0.3.6, but since v0.3.7 it stores all runtime
// SKUD data, not users only.
class MariaDbUserStore {
public:
    explicit MariaDbUserStore(const Config& cfg);
    ~MariaDbUserStore();
    bool init(std::string& error);

    bool load(std::vector<User>& users,std::string& error);
    bool save(const std::vector<User>& users,std::string& error);

    bool loadDepartments(std::vector<std::string>& departments,std::string& error);
    bool saveDepartments(const std::vector<std::string>& departments,std::string& error);

    bool loadControllers(std::vector<Controller>& controllers,std::string& error);
    bool saveControllers(const std::vector<Controller>& controllers,std::string& error);

    bool appendEvent(const AttendanceEvent& event,std::string& error,const std::string& source_salt={});
    bool hasControllerEvent(int controller_node,const std::string& raw_hex,bool& exists,std::string& error);
    bool loadEventsByDate(const std::string& date,std::vector<AttendanceEvent>& events,std::string& error);
    bool loadAttendanceDates(std::vector<std::string>& dates,std::string& error);

    bool loadCardStates(std::map<std::string,PersistedCardState>& states,std::string& error);
    bool saveCardStates(const std::map<std::string,PersistedCardState>& states,std::string& error);

    bool loadActivities(std::vector<CardActivity>& activities,std::string& error);
    bool saveActivities(const std::vector<CardActivity>& activities,std::string& error);

    bool loadControllerCards(std::vector<ControllerCardRecord>& cards,std::string& error);
    bool saveControllerCards(const std::vector<ControllerCardRecord>& cards,std::string& error);

    std::string status() const;
private:
    const Config& cfg_;
    mutable std::mutex mu_;
    void* conn_{nullptr};
    std::string status_{"DISABLED"};
    bool connectLocked(std::string& error);
    bool execLocked(const std::string& sql,std::string& error);
    std::string escLocked(const std::string& value) const;
};
}
