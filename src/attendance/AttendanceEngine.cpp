#include "skud/AttendanceEngine.h"
#include "skud/UserManager.h"
#include "skud/Util.h"
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>

namespace skud {
namespace {
std::string userStateKey(int id){return "U:"+std::to_string(id);}
std::string cardStateKey(const std::string& card){return "C:"+card;}
bool datePart(const std::string& timestamp,std::string& date){
    if(timestamp.size()<10||timestamp[4]!='-'||timestamp[7]!='-')return false;
    date=timestamp.substr(0,10);
    return true;
}
}

AttendanceEngine::AttendanceEngine(UserManager& users, FileStore& store, int repeat_seconds):users_(users),store_(store),repeat_seconds_(repeat_seconds){
    // v0.3.3 keeps attendance state per USER, not per physical card.  This is
    // required when one employee owns several cards: arrival with card A and
    // departure with card B must toggle the same presence state.
    for(auto&[persisted_key,p]:store_.loadCardStates()){
        State s;s.presence=p.state;s.last_read_text=p.last_read;s.last_read=parseTime(p.last_read,s.has_last);
        std::string key=persisted_key;
        std::string card;
        if(key.rfind("U:",0)==0){
            // already v0.3.3 format
        }else{
            if(key.rfind("C:",0)==0)card=key.substr(2);else card=key; // legacy file used raw card as key
            auto user=users_.byCard(card);
            key=user?userStateKey(user->id):cardStateKey(card);
        }
        auto it=states_.find(key);
        if(it==states_.end()||(!it->second.has_last&&s.has_last)||(it->second.has_last&&s.has_last&&s.last_read>it->second.last_read))states_[key]=s;
    }
    for(auto&a:store_.loadActivities())activities_[a.card]=a;
}
void AttendanceEngine::setNotifier(NotifyFn fn){std::lock_guard lk(mu_);notifier_=std::move(fn);}
std::chrono::system_clock::time_point AttendanceEngine::parseTime(const std::string&s,bool&ok){std::tm tm{};std::istringstream in(s);in>>std::get_time(&tm,"%Y-%m-%d %H:%M:%S");ok=!in.fail();if(!ok)return{};return std::chrono::system_clock::from_time_t(std::mktime(&tm));}
AttendanceEvent AttendanceEngine::onCardRead(const std::string&card,int node,const std::string&controller,const std::string&raw){
    return onControllerAccessEvent(card,node,controller,raw,util::nowLocal()).event;
}

AttendanceEngine::ControllerEventProcessResult AttendanceEngine::onControllerAccessEvent(const std::string&card,int node,const std::string&controller,const std::string&raw,const std::string&event_timestamp,bool allow_notification){
    ControllerEventProcessResult result;
    std::string ts=event_timestamp;
    bool event_time_ok=false;
    auto event_time=parseTime(ts,event_time_ok);
    if(!event_time_ok){ts=util::nowLocal();event_time=parseTime(ts,event_time_ok);}

    AttendanceEvent e;e.timestamp=ts;e.card=card;e.controller_node=node;e.controller_name=controller;e.raw_hex=raw;
    result.event=e;

    // 25H keeps the same oldest frame until 37H succeeds.  Check before any
    // attendance-state mutation so a restart between DB insert and FIFO delete
    // cannot turn the same physical access into a second arrival/departure.
    if(!raw.empty()&&store_.hasControllerEvent(node,raw,ts)){
        result.stored=true;result.duplicate=true;return result;
    }

    NotifyFn notify;
    {
        std::lock_guard lk(mu_);
        auto user=users_.byCard(card);
        if(user){e.user_id=user->id;e.user_name=user->last_name+" "+user->first_name;e.position=user->position;e.department=user->department;e.telegram_arrival=user->telegram_arrival;e.telegram_departure=user->telegram_departure;}else e.type=AttendanceEventType::UnknownCard;
        const std::string state_key=user?userStateKey(user->id):cardStateKey(card);
        auto& st=states_[state_key];

        // A calendar-day boundary is also an attendance-session boundary.
        // This is intentionally checked from the controller event timestamp,
        // not from server processing time: a 23:59:59 read followed by a
        // 00:00:01 read must become Departure(previous day) + Arrival(new day),
        // even though the two reads are inside the accidental-repeat window.
        std::string event_date,state_date;
        const bool have_event_date=datePart(ts,event_date);
        const bool have_state_date=datePart(st.last_read_text,state_date);
        const bool new_calendar_day=user&&have_event_date&&have_state_date&&event_date>state_date;
        if(new_calendar_day&&st.presence==PresenceState::Present){
            bool rollover_changed=false;
            if(!closePresentStateBeforeDateLocked(state_key,st,event_date,rollover_changed)){
                result.event=e;result.stored=false;return result;
            }
            // Persist the day-close before accepting the new-day event. If the
            // process stops here, restart recovery remains idempotent.
            if(rollover_changed)persistLocked();
        }

        bool accidental=false;
        if(!new_calendar_day&&st.has_last&&event_time_ok){
            const auto sec=std::chrono::duration_cast<std::chrono::seconds>(event_time-st.last_read).count();
            // Historical FIFO is drained much faster than real time.  Compare
            // controller event timestamps, not server processing speed.
            accidental=sec>=0&&sec<=repeat_seconds_;
        }

        PresenceState next_presence=st.presence;
        if(user&&user->enabled){
            if(accidental)e.type=AttendanceEventType::Accidental;
            else if(st.presence==PresenceState::Absent){next_presence=PresenceState::Present;e.type=AttendanceEventType::Arrival;}
            else{next_presence=PresenceState::Absent;e.type=AttendanceEventType::Departure;}
        }

        // Persist the attendance event before changing the durable presence
        // state.  ControllerManager will not issue 37H if this insert fails.
        if(!store_.appendEvent(e)){result.event=e;result.stored=false;return result;}

        st.presence=next_presence;st.last_read=event_time;st.has_last=event_time_ok;st.last_read_text=ts;
        auto& a=activities_[card];a.card=card;a.user_id=e.user_id;a.user_name=e.user_name;a.department=e.department;a.last_read=ts;a.controller_node=node;
        if(e.type==AttendanceEventType::Arrival)a.last_event="Приход";else if(e.type==AttendanceEventType::Departure)a.last_event="Уход";else if(e.type==AttendanceEventType::Accidental)a.last_event="Случайное повторное";else a.last_event="Неизвестная карта";
        persistLocked();notify=notifier_;
    }
    result.event=e;result.stored=true;
    if(allow_notification&&notify&&(e.type==AttendanceEventType::Arrival||e.type==AttendanceEventType::Departure))notify(e);
    return result;
}

void AttendanceEngine::recordRawControllerEvent(int node,const std::string&controller,const std::string&raw){
    (void)recordControllerRawEvent(node,controller,raw,util::nowLocal());
}

AttendanceEngine::ControllerEventProcessResult AttendanceEngine::recordControllerRawEvent(int node,const std::string&controller,const std::string&raw,const std::string&event_timestamp,const std::string&card){
    ControllerEventProcessResult result;
    AttendanceEvent e;e.timestamp=event_timestamp.empty()?util::nowLocal():event_timestamp;e.type=AttendanceEventType::RawControllerEvent;e.card=card;e.controller_node=node;e.controller_name=controller;e.raw_hex=raw;
    result.event=e;
    if(!raw.empty()&&store_.hasControllerEvent(node,raw,e.timestamp)){result.stored=true;result.duplicate=true;return result;}
    result.stored=store_.appendEvent(e);return result;
}
void AttendanceEngine::persistLocked(){std::map<std::string,PersistedCardState> p;for(auto&[key,s]:states_)p[key]={s.presence,s.last_read_text};store_.saveCardStates(p);std::vector<CardActivity>a;for(auto&[_,x]:activities_)a.push_back(x);store_.saveActivities(a);}
std::vector<CardActivity> AttendanceEngine::activities()const{std::lock_guard lk(mu_);std::vector<CardActivity>v;for(auto it=activities_.rbegin();it!=activities_.rend();++it)v.push_back(it->second);return v;}
std::vector<User> AttendanceEngine::presentUsers()const{
    std::lock_guard lk(mu_);std::vector<User>r;std::set<int> seen;
    for(const auto&[key,s]:states_){
        if(s.presence!=PresenceState::Present)continue;
        std::optional<User> u;
        if(key.rfind("U:",0)==0){try{u=users_.byId(std::stoi(key.substr(2)));}catch(...){}}
        else{auto card=key.rfind("C:",0)==0?key.substr(2):key;u=users_.byCard(card);}
        if(u&&seen.insert(u->id).second)r.push_back(*u);
    }
    return r;
}
std::vector<DailyAttendance> AttendanceEngine::todayAttendance()const{
    auto rows=store_.loadDailyAttendance(util::todayLocal());
    for(auto& row:rows){
        auto u=users_.byId(row.user_id);if(!u)continue;
        row.user_name=u->last_name+" "+u->first_name;if(!u->middle_name.empty())row.user_name+=" "+u->middle_name;row.position=u->position;row.department=u->department;
    }
    return rows;
}


bool AttendanceEngine::closePresentStateBeforeDateLocked(const std::string& key,State& st,const std::string& boundary_date,bool& changed){
    changed=false;
    if(st.presence!=PresenceState::Present)return true;

    std::string event_date;
    if(!datePart(st.last_read_text,event_date)){
        // A legacy/corrupt present state without a usable last-read time cannot
        // be assigned a truthful departure timestamp. Reset only the flag.
        st.presence=PresenceState::Absent;
        changed=true;
        return true;
    }
    if(event_date>=boundary_date)return true;

    std::optional<User> user;
    std::string legacy_card;
    if(key.rfind("U:",0)==0){
        try{user=users_.byId(std::stoi(key.substr(2)));}catch(...){}
    }else{
        legacy_card=key.rfind("C:",0)==0?key.substr(2):key;
        user=users_.byCard(legacy_card);
    }

    // Unknown cards never become Present in normal operation. If a legacy
    // state survived without a matching user, reset it without inventing an
    // attendance event for a non-existent employee.
    if(!user){st.presence=PresenceState::Absent;changed=true;return true;}

    AttendanceEvent e;
    e.timestamp=st.last_read_text; // last physical card use = departure
    e.type=AttendanceEventType::Departure;
    e.user_id=user->id;
    e.user_name=user->last_name+" "+user->first_name;
    if(!user->middle_name.empty())e.user_name+=" "+user->middle_name;
    e.position=user->position;
    e.department=user->department;
    e.telegram_arrival=user->telegram_arrival;
    e.telegram_departure=user->telegram_departure;
    e.controller_name="Автозакрытие дня 00:00";
    e.raw_hex="AUTO_MIDNIGHT_RESET|U:"+std::to_string(user->id)+"|D:"+event_date;

    // CardActivity is per physical card. Prefer a card read that is not newer
    // than this state timestamp; this avoids borrowing controller/card data
    // from a later day when old FIFO data is being processed.
    const CardActivity* latest=nullptr;
    for(const auto&[_,a]:activities_){
        if(a.user_id!=user->id||a.last_read.empty()||a.last_read>st.last_read_text)continue;
        if(!latest||a.last_read>latest->last_read)latest=&a;
    }
    if(latest){e.card=latest->card;e.controller_node=latest->controller_node;}
    else if(!legacy_card.empty())e.card=legacy_card;
    else if(!user->card.empty())e.card=user->card;

    // Crash-safe idempotence: if the synthetic departure was already written
    // but the state save did not finish, never append it a second time.
    bool already_stored=false;
    const auto prior=store_.loadAttendanceEventsByDate(event_date);
    for(const auto&x:prior){
        if(x.type==AttendanceEventType::Departure&&x.user_id==e.user_id&&
           x.timestamp==e.timestamp&&x.raw_hex==e.raw_hex){already_stored=true;break;}
    }
    if(!already_stored&&!store_.appendEvent(e))return false;

    st.presence=PresenceState::Absent;
    changed=true;
    for(auto&[_,a]:activities_){
        if(a.user_id==user->id){
            a.last_event="Уход (автосброс 00:00)";
            // Keep a.last_read: it is the real last physical card-use time.
        }
    }
    return true;
}

bool AttendanceEngine::processMidnightRollover(){
    const std::string today=util::todayLocal();
    bool all_ok=true;
    bool any_changed=false;
    std::lock_guard lk(mu_);

    for(auto&[key,st]:states_){
        bool changed=false;
        if(!closePresentStateBeforeDateLocked(key,st,today,changed))all_ok=false;
        any_changed=any_changed||changed;
    }

    if(any_changed)persistLocked();
    return all_ok;
}

bool AttendanceEngine::resetSiteActivity(){
    std::lock_guard lk(mu_);
    const auto old_states=states_;
    const auto old_activities=activities_;

    states_.clear();
    activities_.clear();

    if(!store_.saveCardStates({})){
        states_=old_states;
        activities_=old_activities;
        return false;
    }
    if(!store_.saveActivities({})){
        states_=old_states;
        activities_=old_activities;
        std::map<std::string,PersistedCardState> restored_states;
        for(const auto&[key,state]:states_)restored_states[key]={state.presence,state.last_read_text};
        std::vector<CardActivity> restored_activities;
        restored_activities.reserve(activities_.size());
        for(const auto&[_,activity]:activities_)restored_activities.push_back(activity);
        store_.saveCardStates(restored_states);
        store_.saveActivities(restored_activities);
        return false;
    }
    return true;
}
void AttendanceEngine::refreshUserMetadata(){
    std::lock_guard lk(mu_);
    for(auto&[card,a]:activities_){auto u=users_.byCard(card);if(u){a.user_id=u->id;a.user_name=u->last_name+" "+u->first_name;a.department=u->department;}else{a.user_id=0;a.user_name.clear();a.department.clear();}}

    // If a previously unknown card has just been attached to a user, migrate
    // its persisted state to that user's shared state key.
    std::vector<std::pair<std::string,std::string>> migrations;
    for(const auto&[key,_]:states_){
        if(key.rfind("U:",0)==0)continue;
        const auto card=key.rfind("C:",0)==0?key.substr(2):key;
        auto u=users_.byCard(card);if(u)migrations.push_back({key,userStateKey(u->id)});
    }
    for(const auto&[from,to]:migrations){
        auto src_it=states_.find(from);if(src_it==states_.end())continue;
        auto src=src_it->second;auto dst_it=states_.find(to);
        if(dst_it==states_.end()||(!dst_it->second.has_last&&src.has_last)||(dst_it->second.has_last&&src.has_last&&src.last_read>dst_it->second.last_read))states_[to]=src;
        states_.erase(from);
    }
    persistLocked();
}
}
