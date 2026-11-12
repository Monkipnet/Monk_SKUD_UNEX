#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace skud {
class Config;
class FileStore;
class UserManager;
class AttendanceEngine;
class TelegramNotifier;

struct ReportRange {
    std::string from;
    std::string to;
};

struct AttendanceReport {
    std::string from;
    std::string to;
    std::string filename;
    std::string path;
    std::string content;
    int days{};
    int rows{};
    int users{};
};

struct ReportSchedule {
    bool enabled{false};
    std::string period{"daily"}; // daily, weekly, monthly
    std::string time{"18:00"};
    int weekday{1}; // 1=Monday ... 7=Sunday
    int month_day{1}; // 1..28
    std::string last_sent_at;
    std::string last_period;
    std::string last_status;
    std::string last_error;
};

class ReportManager {
public:
    ReportManager(Config& cfg, FileStore& store, UserManager& users, AttendanceEngine& attendance, TelegramNotifier& telegram, std::string root);
    ~ReportManager();

    void start();
    void stop();

    ReportRange todayRange() const;
    ReportRange currentWeekRange() const;
    ReportRange currentMonthRange() const;

    bool build(const std::string& from, const std::string& to, AttendanceReport& report, std::string& error) const;
    bool sendToTelegram(const std::string& from, const std::string& to, AttendanceReport& report, std::string& error) const;

    // Detailed per-user report: every persisted arrival and departure in the period.
    // Empty user_ids means all current users.
    bool buildExtended(const std::string& from, const std::string& to, const std::vector<int>& user_ids,
                       AttendanceReport& report, std::string& error) const;
    bool sendExtendedToTelegram(const std::string& from, const std::string& to, const std::vector<int>& user_ids,
                                AttendanceReport& report, std::string& error) const;

    ReportSchedule schedule() const;
    bool saveSchedule(bool enabled, const std::string& period, const std::string& time,
                      int weekday, int month_day, std::string& error);

private:
    void schedulerLoop();
    bool syncArchive(std::string& error);
    bool scheduledRange(const std::string& period, ReportRange& range, std::string& error) const;
    bool isScheduleDue(const ReportSchedule& s, std::string& trigger_key) const;

    Config& cfg_;
    FileStore& store_;
    UserManager& users_;
    AttendanceEngine& attendance_;
    TelegramNotifier& telegram_;
    std::string root_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex schedule_mu_;
};
} // namespace skud
