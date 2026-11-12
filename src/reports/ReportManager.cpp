#include "skud/ReportManager.h"
#include "skud/Config.h"
#include "skud/AttendanceEngine.h"
#include "skud/FileStore.h"
#include "skud/TelegramNotifier.h"
#include "skud/UserManager.h"
#include "skud/Util.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace skud {
namespace {

bool parseDate(const std::string& text, std::tm& out, std::time_t& stamp) {
    if (text.size() != 10) return false;
    std::tm tm{};
    std::istringstream in(text);
    in >> std::get_time(&tm, "%Y-%m-%d");
    if (in.fail()) return false;
    tm.tm_hour = 12; // Noon avoids edge cases around timezone/DST boundaries.
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    auto t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) return false;
    std::tm check{};
    localtime_r(&t, &check);
    std::ostringstream normalized;
    normalized << std::put_time(&check, "%Y-%m-%d");
    if (normalized.str() != text) return false;
    out = check;
    stamp = t;
    return true;
}

std::string formatDate(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d");
    return out.str();
}

std::time_t shiftDays(std::time_t t, int days) {
    std::tm tm{};
    localtime_r(&t, &tm);
    tm.tm_mday += days;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::string timeOnly(const std::string& timestamp) {
    if (timestamp.size() >= 19) return timestamp.substr(11, 8);
    return timestamp.empty() ? "—" : timestamp;
}


std::string displayCard(const std::string& card) {
    std::uint16_t series = 0, number = 0;
    if (util::parseCardId(card, series, number, nullptr))
        return std::to_string(series) + " / " + std::to_string(number);
    return card.empty() ? "—" : card;
}

std::string userName(const User& u) {
    std::string name = u.last_name;
    if (!u.first_name.empty()) name += (name.empty() ? "" : " ") + u.first_name;
    if (!u.middle_name.empty()) name += (name.empty() ? "" : " ") + u.middle_name;
    return name.empty() ? ("Пользователь №" + std::to_string(u.id)) : name;
}

std::string htmlEscape(const std::string& value) {
    std::string out; out.reserve(value.size() + 16);
    for (char c : value) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '\"') out += "&quot;";
        else out += c;
    }
    return out;
}

std::string compactWhitespace(std::string value) {
    for (char& c : value) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    std::string out; out.reserve(value.size());
    bool previous_space = false;
    for (unsigned char c : value) {
        if (c == ' ') {
            if (!previous_space) out.push_back(' ');
            previous_space = true;
        } else {
            out.push_back(static_cast<char>(c));
            previous_space = false;
        }
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::size_t utf8Length(const std::string& value) {
    std::size_t count = 0;
    for (unsigned char c : value) if ((c & 0xC0) != 0x80) ++count;
    return count;
}

std::string telegramPad(const std::string& raw, std::size_t width) {
    std::string value = compactWhitespace(raw.empty() ? "—" : raw);
    const auto length = utf8Length(value);
    if (length < width) value.append(width - length, ' ');
    return value;
}

bool validTimeOfDay(const std::string& value, int& hour, int& minute) {
    if (value.size() != 5 || value[2] != ':') return false;
    try {
        hour = std::stoi(value.substr(0, 2));
        minute = std::stoi(value.substr(3, 2));
    } catch (...) { return false; }
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

std::string reportMonthFolder(const std::string& date) {
    static const char* kMonths[] = {
        "", "Январь", "Февраль", "Март", "Апрель", "Май", "Июнь",
        "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"
    };
    if (date.size() < 7) return "Неизвестный-месяц";
    int month = 0;
    try { month = std::stoi(date.substr(5, 2)); } catch (...) { month = 0; }
    if (month < 1 || month > 12) return "Неизвестный-месяц";
    std::ostringstream out;
    out << std::setw(2) << std::setfill('0') << month << "-" << kMonths[month];
    return out.str();
}

bool saveDailyArchiveFile(const std::string& archive_root, const std::string& date,
                          const std::string& content, std::string& error) {
    if (archive_root.empty()) {
        error = "Не задан путь папки архива дневных отчетов";
        return false;
    }
    if (date.size() != 10) {
        error = "Неверная дата для архива отчета: " + date;
        return false;
    }

    std::error_code ec;
    const auto dir = std::filesystem::path(archive_root) / date.substr(0, 4) / reportMonthFolder(date);
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "Не удалось создать папку архива " + dir.string() + ": " + ec.message();
        return false;
    }

    const auto path = dir / (date + ".txt");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Не удалось открыть файл архива: " + path.string();
        return false;
    }
    out << content;
    if (!out) {
        error = "Ошибка записи файла архива: " + path.string();
        return false;
    }
    return true;
}

} // namespace

ReportManager::ReportManager(Config& cfg, FileStore& store, UserManager& users, AttendanceEngine& attendance,
                             TelegramNotifier& telegram, std::string root)
    : cfg_(cfg), store_(store), users_(users), attendance_(attendance), telegram_(telegram), root_(std::move(root)) {
    std::filesystem::create_directories(std::filesystem::path(root_) / "data" / "reports");
}

ReportManager::~ReportManager() { stop(); }

void ReportManager::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&ReportManager::schedulerLoop, this);
}

void ReportManager::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

ReportRange ReportManager::todayRange() const {
    const auto today = util::todayLocal();
    return {today, today};
}

ReportRange ReportManager::currentWeekRange() const {
    std::tm tm{};
    std::time_t t{};
    if (!parseDate(util::todayLocal(), tm, t)) return todayRange();
    const int monday_offset = (tm.tm_wday + 6) % 7; // Sunday=0 -> 6, Monday=1 -> 0.
    return {formatDate(shiftDays(t, -monday_offset)), formatDate(t)};
}

ReportRange ReportManager::currentMonthRange() const {
    std::tm tm{};
    std::time_t t{};
    if (!parseDate(util::todayLocal(), tm, t)) return todayRange();
    tm.tm_mday = 1;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    const auto first = std::mktime(&tm);
    return {formatDate(first), formatDate(t)};
}

bool ReportManager::build(const std::string& from, const std::string& to,
                          AttendanceReport& report, std::string& error) const {
    std::tm from_tm{}, to_tm{};
    std::time_t from_t{}, to_t{};
    if (!parseDate(from, from_tm, from_t) || !parseDate(to, to_tm, to_t)) {
        error = "Неверная дата. Используйте формат YYYY-MM-DD";
        return false;
    }
    if (from_t > to_t) {
        error = "Начальная дата больше конечной";
        return false;
    }

    int days = 0;
    for (auto t = from_t; t <= to_t; t = shiftDays(t, 1)) {
        ++days;
        if (days > 3660) {
            error = "Диапазон отчета слишком большой (максимум 3660 дней)";
            return false;
        }
    }

    std::ostringstream body;
    body << "Monk SKUD UNEX\n";
    body << "ОТЧЕТ ПО ПОСЕЩАЕМОСТИ\n";
    body << "Период: " << from << " — " << to << "\n";
    body << "Сформирован: " << util::nowLocal() << "\n\n";

    std::set<int> unique_users;
    int row_count = 0;
    std::ostringstream details;

    for (auto t = from_t; t <= to_t; t = shiftDays(t, 1)) {
        const auto date = formatDate(t);
        auto rows = store_.loadDailyAttendance(date);
        details << "============================================================\n";
        details << "Дата: " << date << "\n";
        details << "============================================================\n";
        if (rows.empty()) {
            details << "Событий посещаемости нет.\n\n";
            continue;
        }

        int n = 0;
        for (auto& row : rows) {
            ++n;
            ++row_count;
            unique_users.insert(row.user_id);

            // Keep historical values recorded with the event. Use current user metadata
            // only as a fallback for older/partial event files.
            if (auto u = users_.byId(row.user_id)) {
                if (row.user_name.empty()) row.user_name = userName(*u);
                row.position = u->position;
                if (row.department.empty()) row.department = u->department;
                if (row.card.empty()) row.card = u->card;
            }

            details << n << ". " << (row.user_name.empty() ? ("Пользователь №" + std::to_string(row.user_id)) : row.user_name) << "\n";
            details << "   Должность: " << (row.position.empty() ? "—" : row.position) << "\n";
            details << "   Отдел: " << (row.department.empty() ? "—" : row.department) << "\n";
            details << "   Карта: " << displayCard(row.card) << "\n";
            details << "   Первый приход: " << timeOnly(row.arrival_time) << "\n";
            details << "   Последний уход: " << timeOnly(row.departure_time) << "\n";
            details << "   Состояние на конец дня: "
                    << (row.presence == PresenceState::Present ? "На работе" : "Ушел") << "\n\n";
        }
    }

    body << "Дней в периоде: " << days << "\n";
    body << "Пользователей с событиями: " << unique_users.size() << "\n";
    body << "Записей пользователь/день: " << row_count << "\n\n";
    body << details.str();

    const auto filename = "attendance_" + from + "_" + to + ".txt";
    const auto path = (std::filesystem::path(root_) / "data" / "reports" / filename).string();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "Не удалось создать файл отчета: " + path;
        return false;
    }
    f << body.str();
    if (!f) {
        error = "Ошибка записи файла отчета";
        return false;
    }

    report.from = from;
    report.to = to;
    report.filename = filename;
    report.path = path;
    report.content = body.str();
    report.days = days;
    report.rows = row_count;
    report.users = static_cast<int>(unique_users.size());
    error.clear();
    return true;
}

bool ReportManager::buildExtended(const std::string& from, const std::string& to,
                                  const std::vector<int>& user_ids,
                                  AttendanceReport& report, std::string& error) const {
    std::tm from_tm{}, to_tm{};
    std::time_t from_t{}, to_t{};
    if (!parseDate(from, from_tm, from_t) || !parseDate(to, to_tm, to_t)) {
        error = "Неверная дата. Используйте формат YYYY-MM-DD";
        return false;
    }
    if (from_t > to_t) {
        error = "Начальная дата больше конечной";
        return false;
    }

    int days = 0;
    for (auto t = from_t; t <= to_t; t = shiftDays(t, 1)) {
        ++days;
        if (days > 3660) {
            error = "Диапазон отчета слишком большой (максимум 3660 дней)";
            return false;
        }
    }

    const auto all_users = users_.list();
    std::map<int, User> users_by_id;
    for (const auto& u : all_users) users_by_id[u.id] = u;

    std::vector<User> selected_users;
    if (user_ids.empty()) {
        selected_users = all_users;
    } else {
        std::set<int> seen;
        for (int id : user_ids) {
            if (id <= 0 || !seen.insert(id).second) continue;
            auto it = users_by_id.find(id);
            if (it == users_by_id.end()) {
                error = "Пользователь №" + std::to_string(id) + " не найден";
                return false;
            }
            selected_users.push_back(it->second);
        }
        if (selected_users.empty()) {
            error = "Не выбран ни один пользователь";
            return false;
        }
    }

    std::sort(selected_users.begin(), selected_users.end(), [](const User& a, const User& b) {
        const auto an = userName(a), bn = userName(b);
        if (an != bn) return an < bn;
        return a.id < b.id;
    });

    std::set<int> selected_ids;
    for (const auto& u : selected_users) selected_ids.insert(u.id);
    std::map<int, std::vector<AttendanceEvent>> events_by_user;
    int total_events = 0;
    int arrivals = 0;
    int departures = 0;

    for (auto t = from_t; t <= to_t; t = shiftDays(t, 1)) {
        const auto date = formatDate(t);
        const auto events = store_.loadAttendanceEventsByDate(date);
        for (const auto& e : events) {
            if (e.type != AttendanceEventType::Arrival && e.type != AttendanceEventType::Departure) continue;
            if (e.user_id <= 0 || !selected_ids.count(e.user_id)) continue;
            events_by_user[e.user_id].push_back(e);
            ++total_events;
            if (e.type == AttendanceEventType::Arrival) ++arrivals;
            else ++departures;
        }
    }

    std::ostringstream body;
    body << "Monk SKUD UNEX\n";
    body << "РАСШИРЕННЫЙ ОТЧЕТ ПО ПОЛЬЗОВАТЕЛЯМ\n";
    body << "Все входы и выходы за указанный период\n";
    body << "Период: " << from << " — " << to << "\n";
    body << "Сформирован: " << util::nowLocal() << "\n\n";
    body << "Выбрано пользователей: " << selected_users.size() << "\n";
    body << "Всего событий: " << total_events << " (входов: " << arrivals << ", выходов: " << departures << ")\n\n";

    int users_with_events = 0;
    for (const auto& u : selected_users) {
        auto& events = events_by_user[u.id];
        std::sort(events.begin(), events.end(), [](const AttendanceEvent& a, const AttendanceEvent& b) {
            if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
            return static_cast<int>(a.type) < static_cast<int>(b.type);
        });
        if (!events.empty()) ++users_with_events;

        body << "======================================================================\n";
        body << userName(u) << " (ID " << u.id << ")\n";
        body << "Должность: " << (u.position.empty() ? "—" : u.position) << "\n";
        body << "Отдел: " << (u.department.empty() ? "—" : u.department) << "\n";
        body << "======================================================================\n";

        if (events.empty()) {
            body << "За выбранный период входов и выходов нет.\n\n";
            continue;
        }

        int user_arrivals = 0, user_departures = 0, n = 0;
        for (const auto& e : events) {
            ++n;
            const bool arrival = e.type == AttendanceEventType::Arrival;
            if (arrival) ++user_arrivals; else ++user_departures;
            body << std::setw(4) << n << ". "
                 << (e.timestamp.empty() ? "—" : e.timestamp)
                 << "  " << (arrival ? "ВХОД " : "ВЫХОД")
                 << "  Карта: " << displayCard(e.card)
                 << "  Контроллер: ";
            if (e.controller_node > 0) body << e.controller_node; else body << "—";
            if (!e.controller_name.empty()) body << " (" << e.controller_name << ")";
            body << "\n";
        }
        body << "Итого по пользователю: входов " << user_arrivals
             << ", выходов " << user_departures
             << ", событий " << events.size() << "\n\n";
    }

    body << "----------------------------------------------------------------------\n";
    body << "Пользователей с событиями: " << users_with_events << " из " << selected_users.size() << "\n";
    body << "Всего входов: " << arrivals << "\n";
    body << "Всего выходов: " << departures << "\n";
    body << "Всего событий: " << total_events << "\n";

    std::string selector = user_ids.empty() ? "all" : ("users" + std::to_string(selected_users.size()));
    const auto filename = "attendance_extended_" + from + "_" + to + "_" + selector + ".txt";
    const auto path = (std::filesystem::path(root_) / "data" / "reports" / filename).string();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "Не удалось создать файл отчета: " + path;
        return false;
    }
    f << body.str();
    if (!f) {
        error = "Ошибка записи файла отчета";
        return false;
    }

    report.from = from;
    report.to = to;
    report.filename = filename;
    report.path = path;
    report.content = body.str();
    report.days = days;
    report.rows = total_events;
    report.users = static_cast<int>(selected_users.size());
    error.clear();
    return true;
}

bool ReportManager::sendExtendedToTelegram(const std::string& from, const std::string& to,
                                           const std::vector<int>& user_ids,
                                           AttendanceReport& report, std::string& error) const {
    if (!buildExtended(from, to, user_ids, report, error)) return false;
    std::ostringstream caption;
    caption << "Monk SKUD UNEX — расширенный отчет\n"
            << "Все входы/выходы: " << from << " — " << to
            << "\nПользователей: " << report.users << ", событий: " << report.rows;
    const int retries = std::max(1, cfg_.getInt("telegram.retry_count", 3));
    for (int i = 0; i < retries; ++i) {
        if (telegram_.sendDocument(report.path, caption.str(), error)) return true;
        if (i + 1 < retries) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return false;
}

bool ReportManager::sendToTelegram(const std::string& from, const std::string& to,
                                   AttendanceReport& report, std::string& error) const {
    if (!build(from, to, report, error)) return false;
    std::ostringstream caption;
    caption << "Monk SKUD UNEX — отчет посещаемости\nПериод: " << from << " — " << to;

    const int retries = std::max(1, cfg_.getInt("telegram.retry_count", 3));
    bool document_sent = false;
    for (int i = 0; i < retries; ++i) {
        if (telegram_.sendDocument(report.path, caption.str(), error)) { document_sent = true; break; }
        if (i + 1 < retries) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!document_sent) return false;

    const bool text_copy = cfg_.getBool("telegram.report_text_copy", true);
    const bool archive_enabled = cfg_.getBool("reports.archive.enabled", false);
    const std::string archive_root = cfg_.get("reports.archive.path", "/srv/samba/UNEX");
    if (!text_copy && !archive_enabled) return true;

    const auto all_users = users_.list();
    const int total_users = static_cast<int>(all_users.size());
    int enabled_users = 0;
    for (const auto& u : all_users) if (u.enabled) ++enabled_users;
    int on_site = 0;
    for (const auto& u : attendance_.presentUsers()) if (u.enabled) ++on_site;

    std::tm from_tm{}, to_tm{};
    std::time_t from_t{}, to_t{};
    if (!parseDate(from, from_tm, from_t) || !parseDate(to, to_tm, to_t)) {
        error = "TXT отправлен, но не удалось подготовить таблицу отчета";
        return false;
    }

    std::vector<std::string> messages;
    if (text_copy) {
        std::ostringstream summary;
        summary << "<b>📋 ОТЧЕТ ПО ПОСЕЩАЕМОСТИ</b>\n"
                << "Период: <b>" << htmlEscape(from) << " — " << htmlEscape(to) << "</b>\n"
                << "👥 Всего работников: <b>" << total_users << "</b>\n"
                << "🏢 Сейчас на объекте: <b>" << on_site << "</b>";
        messages.push_back(summary.str());
    }

    // Telegram and local archive use the same calculated widths and the same
    // plain-text rows, so the saved daily report mirrors the table sent to chat.
    constexpr std::size_t kTelegramChunkLimit = 3300;
    for (auto t = from_t; t <= to_t; t = shiftDays(t, 1)) {
        const auto date = formatDate(t);
        auto rows = store_.loadDailyAttendance(date);
        const std::string day_header = "<b>" + htmlEscape(date) + "</b>\n";

        std::ostringstream archive_day;
        archive_day << "Monk SKUD UNEX\n"
                    << "ОТЧЕТ ПО ПОСЕЩАЕМОСТИ\n"
                    << "Дата: " << date << "\n"
                    << "Сформирован: " << util::nowLocal() << "\n"
                    << "Всего работников: " << total_users << "\n"
                    << "Сейчас на объекте: " << on_site << "\n\n";

        if (rows.empty()) {
            if (text_copy) messages.push_back(day_header + "<i>Событий посещаемости нет.</i>");
            archive_day << "Событий посещаемости нет.\n";
            if (archive_enabled && !saveDailyArchiveFile(archive_root, date, archive_day.str(), error)) {
                error = "Telegram-отчет отправлен, но архив не сохранен: " + error;
                return false;
            }
            continue;
        }

        std::size_t name_width = utf8Length("Сотрудник");
        std::size_t position_width = utf8Length("Должность");
        std::size_t arrival_width = utf8Length("Приход");
        std::size_t departure_width = utf8Length("Уход");
        std::size_t state_width = utf8Length("Состояние");

        for (auto& row : rows) {
            if (auto u = users_.byId(row.user_id)) {
                if (row.user_name.empty()) row.user_name = userName(*u);
                if (row.position.empty()) row.position = u->position;
                if (row.department.empty()) row.department = u->department;
            }
            const std::string name = row.user_name.empty()
                ? ("Пользователь №" + std::to_string(row.user_id)) : row.user_name;
            const std::string position = row.position.empty() ? "—" : row.position;
            const std::string arrival = timeOnly(row.arrival_time);
            const std::string departure = timeOnly(row.departure_time);
            const std::string state = row.presence == PresenceState::Present ? "На работе" : "Ушел";
            name_width = std::max(name_width, utf8Length(compactWhitespace(name)));
            position_width = std::max(position_width, utf8Length(compactWhitespace(position)));
            arrival_width = std::max(arrival_width, utf8Length(arrival));
            departure_width = std::max(departure_width, utf8Length(departure));
            state_width = std::max(state_width, utf8Length(state));
        }

        const std::string table_header =
            "Ст │ " + telegramPad("Сотрудник", name_width) + " │ " +
            telegramPad("Должность", position_width) + " │ " +
            telegramPad("Приход", arrival_width) + " │ " +
            telegramPad("Уход", departure_width) + " │ " +
            telegramPad("Состояние", state_width) + "\n";

        archive_day << table_header;
        const std::string block_prefix = day_header + "<pre>" + htmlEscape(table_header);
        std::string chunk = block_prefix;
        for (const auto& row : rows) {
            const bool present = row.presence == PresenceState::Present;
            const std::string mark = present ? "🟢" : "🔴";
            const std::string state = present ? "На работе" : "Ушел";
            const std::string name = row.user_name.empty()
                ? ("Пользователь №" + std::to_string(row.user_id)) : row.user_name;
            const std::string position = row.position.empty() ? "—" : row.position;

            const std::string plain_line =
                mark + " │ " + telegramPad(name, name_width) + " │ " +
                telegramPad(position, position_width) + " │ " +
                telegramPad(timeOnly(row.arrival_time), arrival_width) + " │ " +
                telegramPad(timeOnly(row.departure_time), departure_width) + " │ " +
                telegramPad(state, state_width) + "\n";
            archive_day << plain_line;

            if (text_copy) {
                const std::string row_text = htmlEscape(plain_line);
                if (chunk.size() + row_text.size() + 7 > kTelegramChunkLimit &&
                    chunk.size() > block_prefix.size()) {
                    chunk += "</pre>";
                    messages.push_back(chunk);
                    chunk = block_prefix;
                }
                chunk += row_text;
            }
        }

        if (text_copy) {
            chunk += "</pre>";
            messages.push_back(chunk);
        }
        if (archive_enabled && !saveDailyArchiveFile(archive_root, date, archive_day.str(), error)) {
            error = "Telegram-отчет отправлен, но архив не сохранен: " + error;
            return false;
        }
    }

    if (text_copy) {
        for (const auto& message : messages) {
            bool sent = false;
            for (int i = 0; i < retries; ++i) {
                if (telegram_.sendHtml(message, error)) { sent = true; break; }
                if (i + 1 < retries) std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            if (!sent) {
                error = "TXT-файл отправлен, но текстовая таблица Telegram не отправлена: " + error;
                return false;
            }
        }
    }
    error.clear();
    return true;
}

ReportSchedule ReportManager::schedule() const {
    ReportSchedule s;
    s.enabled = cfg_.getBool("reports.schedule.enabled", false);
    s.period = cfg_.get("reports.schedule.period", "daily");
    if (s.period != "daily" && s.period != "weekly" && s.period != "monthly") s.period = "daily";
    s.time = cfg_.get("reports.schedule.time", "18:00");
    int h = 0, m = 0;
    if (!validTimeOfDay(s.time, h, m)) s.time = "18:00";
    s.weekday = std::clamp(cfg_.getInt("reports.schedule.weekday", 1), 1, 7);
    s.month_day = std::clamp(cfg_.getInt("reports.schedule.month_day", 1), 1, 28);
    s.last_sent_at = cfg_.get("reports.schedule.last_sent_at");
    s.last_period = cfg_.get("reports.schedule.last_period");
    s.last_status = cfg_.get("reports.schedule.last_status");
    s.last_error = cfg_.get("reports.schedule.last_error");
    return s;
}

bool ReportManager::saveSchedule(bool enabled, const std::string& period, const std::string& time,
                                 int weekday, int month_day, std::string& error) {
    if (period != "daily" && period != "weekly" && period != "monthly") {
        error = "Неизвестный тип расписания";
        return false;
    }
    int hour = 0, minute = 0;
    if (!validTimeOfDay(time, hour, minute)) {
        error = "Время должно быть в формате HH:MM";
        return false;
    }
    if (weekday < 1 || weekday > 7) {
        error = "День недели должен быть от 1 до 7";
        return false;
    }
    if (month_day < 1 || month_day > 28) {
        error = "День месяца должен быть от 1 до 28";
        return false;
    }

    std::lock_guard lk(schedule_mu_);
    cfg_.set("reports.schedule.enabled", enabled ? "true" : "false");
    cfg_.set("reports.schedule.period", period);
    cfg_.set("reports.schedule.time", time);
    cfg_.set("reports.schedule.weekday", std::to_string(weekday));
    cfg_.set("reports.schedule.month_day", std::to_string(month_day));
    // Changing schedule deliberately permits a fresh run for the new settings.
    cfg_.set("reports.schedule.last_attempt_key", "");
    cfg_.set("reports.schedule.last_sent_key", "");
    if (!cfg_.save()) {
        error = "Не удалось сохранить system.conf";
        return false;
    }
    error.clear();
    return true;
}

bool ReportManager::scheduledRange(const std::string& period, ReportRange& range, std::string& error) const {
    std::tm today_tm{};
    std::time_t today_t{};
    if (!parseDate(util::todayLocal(), today_tm, today_t)) {
        error = "Не удалось определить текущую дату";
        return false;
    }

    if (period == "daily") {
        const auto yesterday = formatDate(shiftDays(today_t, -1));
        range = {yesterday, yesterday};
        return true;
    }
    if (period == "weekly") {
        const int monday_offset = (today_tm.tm_wday + 6) % 7;
        const auto current_monday = shiftDays(today_t, -monday_offset);
        range.from = formatDate(shiftDays(current_monday, -7));
        range.to = formatDate(shiftDays(current_monday, -1));
        return true;
    }
    if (period == "monthly") {
        std::tm first_tm = today_tm;
        first_tm.tm_mday = 1;
        first_tm.tm_hour = 12;
        first_tm.tm_isdst = -1;
        const auto current_first = std::mktime(&first_tm);
        const auto previous_last = shiftDays(current_first, -1);
        std::tm prev_tm{};
        localtime_r(&previous_last, &prev_tm);
        prev_tm.tm_mday = 1;
        prev_tm.tm_hour = 12;
        prev_tm.tm_isdst = -1;
        const auto previous_first = std::mktime(&prev_tm);
        range = {formatDate(previous_first), formatDate(previous_last)};
        return true;
    }
    error = "Неизвестный период расписания";
    return false;
}

bool ReportManager::isScheduleDue(const ReportSchedule& s, std::string& trigger_key) const {
    if (!s.enabled) return false;
    int scheduled_hour = 0, scheduled_minute = 0;
    if (!validTimeOfDay(s.time, scheduled_hour, scheduled_minute)) return false;

    const auto now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    const int now_minutes = tm.tm_hour * 60 + tm.tm_min;
    const int scheduled_minutes = scheduled_hour * 60 + scheduled_minute;
    if (now_minutes < scheduled_minutes) return false;

    const int weekday = tm.tm_wday == 0 ? 7 : tm.tm_wday;
    if (s.period == "weekly" && weekday != s.weekday) return false;
    if (s.period == "monthly" && tm.tm_mday != s.month_day) return false;

    std::ostringstream date;
    date << std::put_time(&tm, "%Y-%m-%d");
    trigger_key = s.period + "|" + date.str() + "|" + s.time;
    return true;
}

void ReportManager::schedulerLoop() {
    while (running_) {
        const auto s = schedule();
        std::string trigger_key;
        if (isScheduleDue(s, trigger_key)) {
            const auto last_attempt = cfg_.get("reports.schedule.last_attempt_key");
            if (last_attempt != trigger_key) {
                cfg_.set("reports.schedule.last_attempt_key", trigger_key);
                cfg_.set("reports.schedule.last_status", "running");
                cfg_.set("reports.schedule.last_error", "");
                cfg_.save();

                ReportRange range;
                AttendanceReport report;
                std::string error;
                bool ok = scheduledRange(s.period, range, error);
                if (ok) ok = sendToTelegram(range.from, range.to, report, error);

                if (ok) {
                    cfg_.set("reports.schedule.last_sent_key", trigger_key);
                    cfg_.set("reports.schedule.last_sent_at", util::nowLocal());
                    cfg_.set("reports.schedule.last_period", report.from + " — " + report.to);
                    cfg_.set("reports.schedule.last_status", "ok");
                    cfg_.set("reports.schedule.last_error", "");
                } else {
                    cfg_.set("reports.schedule.last_status", "error");
                    cfg_.set("reports.schedule.last_error", error);
                }
                cfg_.save();
            }
        }
        for (int i = 0; i < 20 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace skud
