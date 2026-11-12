#include "skud/FileStore.h"
#include "skud/Config.h"
#include "skud/MariaDbUserStore.h"
#include "skud/Util.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace skud {
namespace {
std::string safe(std::string s){for(char&c:s)if(c==';'||c=='\n'||c=='\r')c=' ';return s;}
AttendanceEventType parseEventType(const std::string&s){if(s=="arrival")return AttendanceEventType::Arrival;if(s=="departure")return AttendanceEventType::Departure;if(s=="accidental")return AttendanceEventType::Accidental;if(s=="unknown_card")return AttendanceEventType::UnknownCard;return AttendanceEventType::RawControllerEvent;}
std::string stamp(){const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());std::tm tm{};localtime_r(&now,&tm);std::ostringstream o;o<<std::put_time(&tm,"%Y%m%d-%H%M%S");return o.str();}
}
FileStore::FileStore(std::string root,Config* cfg):root_(std::move(root)),cfg_(cfg){std::filesystem::create_directories(root_+"/data/events");std::filesystem::create_directories(root_+"/backup");}
FileStore::~FileStore()=default;
bool FileStore::usingMariaDb()const{return cfg_&&cfg_->getBool("database.enabled",false);}
std::string FileStore::storageError()const{std::lock_guard lk(storage_mu_);return storage_error_;}

bool FileStore::backupAndRemove(const std::string& path,const std::string& label,std::string& error)const{
    if(!std::filesystem::exists(path))return true;std::error_code ec;std::filesystem::create_directories(root_+"/backup",ec);if(ec){error=ec.message();return false;}
    const auto dst=std::filesystem::path(root_+"/backup")/(label+".pre-mariadb-"+stamp());std::filesystem::copy_file(path,dst,std::filesystem::copy_options::overwrite_existing,ec);if(ec){error="backup "+path+": "+ec.message();return false;}std::filesystem::remove(path,ec);if(ec){error="remove "+path+": "+ec.message();return false;}return true;
}

bool FileStore::init(std::string& error){
    error.clear();if(!usingMariaDb())return true;db_=std::make_unique<MariaDbUserStore>(*cfg_);if(!db_->init(error)){std::lock_guard lk(storage_mu_);storage_error_=error;return false;}
    if(cfg_->getBool("database.migrate_runtime_csv",true)&&!migrateLegacyRuntime(error)){std::lock_guard lk(storage_mu_);storage_error_=error;return false;}
    std::lock_guard lk(storage_mu_);storage_error_.clear();return true;
}

bool FileStore::migrateLegacyRuntime(std::string& error){
    // Events: INSERT IGNORE makes restart-safe migration possible if a prior run
    // imported rows but stopped before the old CSV was removed.
    const auto events_dir=std::filesystem::path(root_)/"data"/"events";
    if(std::filesystem::exists(events_dir))for(const auto&entry:std::filesystem::directory_iterator(events_dir)){
        if(!entry.is_regular_file()||entry.path().extension()!=".csv")continue;std::ifstream f(entry.path());std::string line;bool first=true;std::size_t line_no=0;
        while(std::getline(f,line)){++line_no;
            if(first){first=false;continue;}auto c=util::split(line,';');AttendanceEvent e;
            if(c.size()>=9){e.timestamp=c[0];e.type=parseEventType(c[1]);e.card=c[2];try{e.user_id=std::stoi(c[3]);e.controller_node=std::stoi(c[6]);}catch(...){continue;}e.user_name=c[4];e.department=c[5];e.controller_name=c[7];e.raw_hex=c[8];}
            else if(c.size()>=4&&entry.path().filename()=="undecoded_unex.csv"){e.timestamp=c[0];e.type=AttendanceEventType::RawControllerEvent;try{e.controller_node=std::stoi(c[1]);}catch(...){continue;}e.raw_hex=c[3];}
            else continue;
            if(!db_->appendEvent(e,error,entry.path().filename().string()+":"+std::to_string(line_no)))return false;
        }
        if(!backupAndRemove(entry.path().string(),"events-"+entry.path().stem().string()+".csv",error))return false;
    }

    const std::string state_path=root_+"/data/card_state.csv";if(std::filesystem::exists(state_path)){
        std::map<std::string,PersistedCardState> states;std::ifstream f(state_path);std::string l;bool first=true;while(std::getline(f,l)){if(first){first=false;continue;}auto c=util::split(l,';');if(c.size()>=3)states[c[0]]={c[1]=="present"?PresenceState::Present:PresenceState::Absent,c[2]};}
        std::map<std::string,PersistedCardState> existing;if(!db_->loadCardStates(existing,error))return false;
        for(const auto&[key,st]:states){auto it=existing.find(key);if(it==existing.end()||it->second.last_read<st.last_read)existing[key]=st;}
        if(!db_->saveCardStates(existing,error))return false;std::map<std::string,PersistedCardState> verify;if(!db_->loadCardStates(verify,error)||verify.size()!=existing.size()){error="attendance state migration verification failed";return false;}
        if(!backupAndRemove(state_path,"card_state.csv",error))return false;
    }

    const std::string activity_path=root_+"/data/active_cards.csv";if(std::filesystem::exists(activity_path)){
        std::vector<CardActivity> activities;std::ifstream f(activity_path);std::string l;bool first=true;while(std::getline(f,l)){if(first){first=false;continue;}auto c=util::split(l,';');if(c.size()<7)continue;CardActivity x;x.card=c[0];try{x.user_id=std::stoi(c[1]);x.controller_node=std::stoi(c[6]);}catch(...){}x.user_name=c[2];x.department=c[3];x.last_read=c[4];x.last_event=c[5];activities.push_back(std::move(x));}
        std::vector<CardActivity> existing;if(!db_->loadActivities(existing,error))return false;
        for(const auto&x:activities){auto it=std::find_if(existing.begin(),existing.end(),[&](const CardActivity&y){return y.card==x.card;});if(it==existing.end())existing.push_back(x);else if(it->last_read<x.last_read)*it=x;}
        if(!db_->saveActivities(existing,error))return false;std::vector<CardActivity> verify;if(!db_->loadActivities(verify,error)||verify.size()!=existing.size()){error="card activity migration verification failed";return false;}
        if(!backupAndRemove(activity_path,"active_cards.csv",error))return false;
    }
    return true;
}

bool FileStore::appendEvent(const AttendanceEvent&e){
    if(usingMariaDb()){if(!db_)return false;std::string err;const bool ok=db_->appendEvent(e,err);if(!ok){std::lock_guard lk(storage_mu_);storage_error_=err;}return ok;}
    // Historical 25H events must be stored under the date reported by the
    // controller, not under the day on which the server happened to read FIFO.
    std::string event_date=util::todayLocal();if(e.timestamp.size()>=10&&e.timestamp[4]=='-'&&e.timestamp[7]=='-')event_date=e.timestamp.substr(0,10);
    std::lock_guard lk(mu_);auto p=root_+"/data/events/"+event_date+".csv";bool exists=std::filesystem::exists(p);std::ofstream f(p,std::ios::app);if(!f)return false;if(!exists)f<<"timestamp;type;card;user_id;user_name;department;controller_node;controller_name;raw_hex\n";std::string t="raw";if(e.type==AttendanceEventType::Arrival)t="arrival";else if(e.type==AttendanceEventType::Departure)t="departure";else if(e.type==AttendanceEventType::Accidental)t="accidental";else if(e.type==AttendanceEventType::UnknownCard)t="unknown_card";f<<safe(e.timestamp)<<';'<<t<<';'<<safe(e.card)<<';'<<e.user_id<<';'<<safe(e.user_name)<<';'<<safe(e.department)<<';'<<e.controller_node<<';'<<safe(e.controller_name)<<';'<<safe(e.raw_hex)<<"\n";return true;
}

bool FileStore::hasControllerEvent(int controller_node,const std::string& raw_hex,const std::string& event_timestamp)const{
    if(raw_hex.empty())return false;
    if(usingMariaDb()){
        if(!db_)return false;std::string err;bool exists=false;
        if(!db_->hasControllerEvent(controller_node,raw_hex,exists,err)){std::lock_guard lk(storage_mu_);storage_error_=err;return false;}return exists;
    }
    std::string event_date=util::todayLocal();if(event_timestamp.size()>=10&&event_timestamp[4]=='-'&&event_timestamp[7]=='-')event_date=event_timestamp.substr(0,10);
    std::lock_guard lk(mu_);std::ifstream f(root_+"/data/events/"+event_date+".csv");std::string line;bool first=true;while(std::getline(f,line)){if(first){first=false;continue;}auto c=util::split(line,';');if(c.size()<9)continue;try{if(std::stoi(c[6])==controller_node&&c[8]==raw_hex)return true;}catch(...){}}
    return false;
}

bool FileStore::saveCardStates(const std::map<std::string,PersistedCardState>&s){
    if(usingMariaDb()){if(!db_)return false;std::string err;const bool ok=db_->saveCardStates(s,err);if(!ok){std::lock_guard lk(storage_mu_);storage_error_=err;}return ok;}
    std::lock_guard lk(mu_);auto p=root_+"/data/card_state.csv",tmp=p+".tmp";std::ofstream f(tmp);if(!f)return false;f<<"state_key;state;last_read\n";for(auto&[card,st]:s)f<<safe(card)<<';'<<(st.state==PresenceState::Present?"present":"absent")<<';'<<safe(st.last_read)<<"\n";f.close();std::error_code ec;std::filesystem::rename(tmp,p,ec);if(ec){std::filesystem::remove(p,ec);ec.clear();std::filesystem::rename(tmp,p,ec);}return !ec;
}
std::map<std::string,PersistedCardState> FileStore::loadCardStates()const{
    if(usingMariaDb()){std::map<std::string,PersistedCardState> out;if(!db_)return out;std::string err;if(!db_->loadCardStates(out,err)){std::lock_guard lk(storage_mu_);storage_error_=err;out.clear();}return out;}
    std::lock_guard lk(mu_);std::map<std::string,PersistedCardState>r;std::ifstream f(root_+"/data/card_state.csv");std::string l;bool first=true;while(std::getline(f,l)){if(first){first=false;continue;}auto c=util::split(l,';');if(c.size()>=3)r[c[0]]={c[1]=="present"?PresenceState::Present:PresenceState::Absent,c[2]};}return r;
}

bool FileStore::saveActivities(const std::vector<CardActivity>&a){
    if(usingMariaDb()){if(!db_)return false;std::string err;const bool ok=db_->saveActivities(a,err);if(!ok){std::lock_guard lk(storage_mu_);storage_error_=err;}return ok;}
    std::lock_guard lk(mu_);auto p=root_+"/data/active_cards.csv",tmp=p+".tmp";std::ofstream f(tmp);if(!f)return false;f<<"card;user_id;user_name;department;last_read;last_event;controller_node\n";for(auto&x:a)f<<safe(x.card)<<';'<<x.user_id<<';'<<safe(x.user_name)<<';'<<safe(x.department)<<';'<<safe(x.last_read)<<';'<<safe(x.last_event)<<';'<<x.controller_node<<"\n";f.close();std::error_code ec;std::filesystem::rename(tmp,p,ec);if(ec){std::filesystem::remove(p,ec);ec.clear();std::filesystem::rename(tmp,p,ec);}return !ec;
}
std::vector<CardActivity> FileStore::loadActivities()const{
    if(usingMariaDb()){std::vector<CardActivity> out;if(!db_)return out;std::string err;if(!db_->loadActivities(out,err)){std::lock_guard lk(storage_mu_);storage_error_=err;out.clear();}return out;}
    std::lock_guard lk(mu_);std::vector<CardActivity>r;std::ifstream f(root_+"/data/active_cards.csv");std::string l;bool first=true;while(std::getline(f,l)){if(first){first=false;continue;}auto c=util::split(l,';');if(c.size()>=7){CardActivity x;x.card=c[0];try{x.user_id=std::stoi(c[1]);x.controller_node=std::stoi(c[6]);}catch(...){}x.user_name=c[2];x.department=c[3];x.last_read=c[4];x.last_event=c[5];r.push_back(x);}}return r;
}

std::vector<AttendanceEvent> FileStore::loadAttendanceEventsByDate(const std::string&date)const{
    std::vector<AttendanceEvent> out;
    if(usingMariaDb()){
        if(!db_)return out;
        std::string err;
        if(!db_->loadEventsByDate(date,out,err)){
            std::lock_guard lk(storage_mu_);storage_error_=err;out.clear();
        }
        return out;
    }
    std::lock_guard lk(mu_);
    std::ifstream f(root_+"/data/events/"+date+".csv");
    std::string line;bool first=true;
    while(std::getline(f,line)){
        if(first){first=false;continue;}
        auto c=util::split(line,';');
        if(c.size()<9)continue;
        AttendanceEvent e;
        e.timestamp=c[0];e.type=parseEventType(c[1]);e.card=c[2];
        try{e.user_id=std::stoi(c[3]);e.controller_node=std::stoi(c[6]);}catch(...){continue;}
        e.user_name=c[4];e.department=c[5];e.controller_name=c[7];e.raw_hex=c[8];
        out.push_back(std::move(e));
    }
    std::sort(out.begin(),out.end(),[](const AttendanceEvent&a,const AttendanceEvent&b){return a.timestamp<b.timestamp;});
    return out;
}

std::vector<DailyAttendance> FileStore::loadDailyAttendance(const std::string&date)const{
    std::vector<AttendanceEvent> events;
    if(usingMariaDb()){if(!db_)return{};std::string err;if(!db_->loadEventsByDate(date,events,err)){std::lock_guard lk(storage_mu_);storage_error_=err;return{};}}
    else{
        std::lock_guard lk(mu_);std::ifstream f(root_+"/data/events/"+date+".csv");std::string line;bool first=true;while(std::getline(f,line)){if(first){first=false;continue;}auto c=util::split(line,';');if(c.size()<6|| (c[1]!="arrival"&&c[1]!="departure"))continue;AttendanceEvent e;e.timestamp=c[0];e.type=parseEventType(c[1]);e.card=c[2];try{e.user_id=std::stoi(c[3]);}catch(...){continue;}e.user_name=c[4];e.department=c[5];events.push_back(std::move(e));}
    }
    std::map<int,DailyAttendance> by_user;for(const auto&e:events){if(e.type!=AttendanceEventType::Arrival&&e.type!=AttendanceEventType::Departure)continue;if(e.user_id<=0)continue;auto&x=by_user[e.user_id];x.user_id=e.user_id;x.user_name=e.user_name;x.department=e.department;x.card=e.card;x.last_event_time=e.timestamp;if(e.type==AttendanceEventType::Arrival){if(x.arrival_time.empty())x.arrival_time=e.timestamp;/* If the employee returned after leaving, the earlier departure was only an intermediate exit. Keep one daily record and wait for the next departure to become the final departure. */x.departure_time.clear();x.presence=PresenceState::Present;}else{x.departure_time=e.timestamp;x.presence=PresenceState::Absent;}}
    std::vector<DailyAttendance> out;out.reserve(by_user.size());for(auto&[_,x]:by_user)out.push_back(std::move(x));std::sort(out.begin(),out.end(),[](const DailyAttendance&a,const DailyAttendance&b){if(a.presence!=b.presence)return a.presence==PresenceState::Present;if(a.arrival_time!=b.arrival_time){if(a.arrival_time.empty())return false;if(b.arrival_time.empty())return true;return a.arrival_time<b.arrival_time;}return a.user_name<b.user_name;});return out;
}

std::vector<std::string> FileStore::attendanceDates()const{
    std::vector<std::string> out;
    if(usingMariaDb()){
        if(!db_)return out;
        std::string err;
        if(!db_->loadAttendanceDates(out,err)){
            std::lock_guard lk(storage_mu_);
            storage_error_=err;
            out.clear();
        }
        return out;
    }
    std::lock_guard lk(mu_);
    const auto dir=std::filesystem::path(root_)/"data"/"events";
    std::error_code ec;
    if(!std::filesystem::exists(dir,ec))return out;
    for(const auto&entry:std::filesystem::directory_iterator(dir,ec)){
        if(ec)break;
        if(!entry.is_regular_file()||entry.path().extension()!=".csv")continue;
        const auto date=entry.path().stem().string();
        if(date.size()==10&&date[4]=='-'&&date[7]=='-')out.push_back(date);
    }
    std::sort(out.begin(),out.end());
    out.erase(std::unique(out.begin(),out.end()),out.end());
    return out;
}

bool FileStore::backupFile(const std::string&path)const{if(!std::filesystem::exists(path))return true;std::filesystem::create_directories(root_+"/backup");auto name=std::filesystem::path(path).filename().string();std::string s=util::nowLocal();for(char&c:s)if(c==':'||c==' ')c='_';std::error_code ec;std::filesystem::copy_file(path,root_+"/backup/"+s+"_"+name,std::filesystem::copy_options::overwrite_existing,ec);return !ec;}
}
