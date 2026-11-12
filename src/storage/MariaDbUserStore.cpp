#include "skud/MariaDbUserStore.h"
#include "skud/Config.h"
#include "skud/Util.h"
#include <mariadb/mysql.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

namespace skud {
namespace {
bool validDbName(const std::string& s){return !s.empty()&&std::all_of(s.begin(),s.end(),[](unsigned char c){return std::isalnum(c)||c=='_';});}
std::string nullable(const char* p){return p?p:"";}
int toInt(const char* p,int d=0){try{return p?std::stoi(p):d;}catch(...){return d;}}
std::uint64_t toU64(const char* p,std::uint64_t d=0){try{return p?static_cast<std::uint64_t>(std::stoull(p)):d;}catch(...){return d;}}
std::string eventTypeText(AttendanceEventType t){
    if(t==AttendanceEventType::Arrival)return "arrival";
    if(t==AttendanceEventType::Departure)return "departure";
    if(t==AttendanceEventType::Accidental)return "accidental";
    if(t==AttendanceEventType::UnknownCard)return "unknown_card";
    return "raw";
}
AttendanceEventType eventTypeFromText(const std::string& s){
    if(s=="arrival")return AttendanceEventType::Arrival;
    if(s=="departure")return AttendanceEventType::Departure;
    if(s=="accidental")return AttendanceEventType::Accidental;
    if(s=="unknown_card")return AttendanceEventType::UnknownCard;
    return AttendanceEventType::RawControllerEvent;
}
}

MariaDbUserStore::MariaDbUserStore(const Config& cfg):cfg_(cfg){}
MariaDbUserStore::~MariaDbUserStore(){std::lock_guard lk(mu_);if(conn_)mysql_close(static_cast<MYSQL*>(conn_));}

bool MariaDbUserStore::connectLocked(std::string& error){
    if(conn_&&mysql_ping(static_cast<MYSQL*>(conn_))==0)return true;
    if(conn_){mysql_close(static_cast<MYSQL*>(conn_));conn_=nullptr;}
    const auto host=cfg_.get("database.host","127.0.0.1");
    const auto user=cfg_.get("database.user","monk_skud");
    const auto pass=cfg_.get("database.password","");
    const auto db=cfg_.get("database.name","monk_skud_unex");
    const auto port=static_cast<unsigned>(std::max(1,cfg_.getInt("database.port",3306)));
    if(!validDbName(db)){error="database.name должен содержать только A-Z, a-z, 0-9 и _";status_="ERROR: invalid database.name";return false;}
    MYSQL* c=mysql_init(nullptr);if(!c){error="mysql_init failed";status_="ERROR: mysql_init";return false;}
    unsigned timeout=5;mysql_options(c,MYSQL_OPT_CONNECT_TIMEOUT,&timeout);
    if(!mysql_real_connect(c,host.c_str(),user.c_str(),pass.c_str(),nullptr,port,nullptr,0)){
        error=mysql_error(c);status_="OFFLINE: "+error;mysql_close(c);return false;
    }
    mysql_set_character_set(c,"utf8mb4");
    if(cfg_.getBool("database.auto_create",true)){
        const std::string q="CREATE DATABASE IF NOT EXISTS `"+db+"` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci";
        if(mysql_query(c,q.c_str())!=0){error=mysql_error(c);status_="ERROR create database: "+error;mysql_close(c);return false;}
    }
    if(mysql_select_db(c,db.c_str())!=0){error=mysql_error(c);status_="ERROR select database: "+error;mysql_close(c);return false;}
    conn_=c;status_="ONLINE "+host+":"+std::to_string(port)+"/"+db;return true;
}

bool MariaDbUserStore::execLocked(const std::string& sql,std::string& error){
    if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);
    if(mysql_query(c,sql.c_str())==0)return true;error=mysql_error(c);status_="ERROR: "+error;return false;
}
std::string MariaDbUserStore::escLocked(const std::string& value)const{
    auto*c=static_cast<MYSQL*>(conn_);std::string out(value.size()*2+1,'\0');
    const auto n=mysql_real_escape_string(c,out.data(),value.data(),static_cast<unsigned long>(value.size()));out.resize(n);return out;
}

bool MariaDbUserStore::init(std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);
    const char* schema[]={
        "CREATE TABLE IF NOT EXISTS skud_users (id INT NOT NULL PRIMARY KEY, enabled TINYINT(1) NOT NULL DEFAULT 1,last_name VARCHAR(255) NOT NULL DEFAULT '', first_name VARCHAR(255) NOT NULL DEFAULT '', middle_name VARCHAR(255) NOT NULL DEFAULT '',department VARCHAR(255) NOT NULL DEFAULT '', position VARCHAR(255) NOT NULL DEFAULT '', pin_code VARCHAR(16) NOT NULL DEFAULT '',access_mode VARCHAR(32) NOT NULL DEFAULT 'card', controller_port INT NOT NULL DEFAULT 0,valid_from VARCHAR(32) NOT NULL DEFAULT '', valid_until VARCHAR(32) NOT NULL DEFAULT '',telegram_arrival TINYINT(1) NOT NULL DEFAULT 1, telegram_departure TINYINT(1) NOT NULL DEFAULT 1,updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        "CREATE TABLE IF NOT EXISTS skud_user_cards (user_id INT NOT NULL, card_order INT NOT NULL DEFAULT 0, card VARCHAR(32) NOT NULL, series INT UNSIGNED NOT NULL, number INT UNSIGNED NOT NULL,PRIMARY KEY(user_id,card_order), UNIQUE KEY uq_skud_card(card), KEY idx_skud_cards_user(user_id),CONSTRAINT fk_skud_cards_user FOREIGN KEY(user_id) REFERENCES skud_users(id) ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        "CREATE TABLE IF NOT EXISTS skud_departments (name VARCHAR(255) NOT NULL PRIMARY KEY, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        "CREATE TABLE IF NOT EXISTS skud_controllers (node INT NOT NULL PRIMARY KEY, name VARCHAR(255) NOT NULL DEFAULT '', model VARCHAR(255) NOT NULL DEFAULT 'UNEX 721', enabled TINYINT(1) NOT NULL DEFAULT 1, last_seen VARCHAR(32) NOT NULL DEFAULT '', last_raw_hex TEXT NOT NULL, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        "CREATE TABLE IF NOT EXISTS skud_attendance_events (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, source_key CHAR(64) NOT NULL, timestamp VARCHAR(32) NOT NULL, type VARCHAR(32) NOT NULL, card VARCHAR(32) NOT NULL DEFAULT '', user_id INT NOT NULL DEFAULT 0, user_name VARCHAR(765) NOT NULL DEFAULT '', department VARCHAR(255) NOT NULL DEFAULT '', controller_node INT NOT NULL DEFAULT 0, controller_name VARCHAR(255) NOT NULL DEFAULT '', raw_hex TEXT NOT NULL, UNIQUE KEY uq_skud_event_source(source_key), KEY idx_skud_event_time(timestamp), KEY idx_skud_event_user(user_id,timestamp)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        "CREATE TABLE IF NOT EXISTS skud_attendance_state (state_key VARCHAR(128) NOT NULL PRIMARY KEY, state VARCHAR(16) NOT NULL, last_read VARCHAR(32) NOT NULL DEFAULT '', updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        "CREATE TABLE IF NOT EXISTS skud_card_activity (card VARCHAR(32) NOT NULL PRIMARY KEY, user_id INT NOT NULL DEFAULT 0, user_name VARCHAR(765) NOT NULL DEFAULT '', department VARCHAR(255) NOT NULL DEFAULT '', last_read VARCHAR(32) NOT NULL DEFAULT '', last_event VARCHAR(64) NOT NULL DEFAULT '', controller_node INT NOT NULL DEFAULT 0, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        "CREATE TABLE IF NOT EXISTS skud_controller_cards (controller_node INT NOT NULL, card VARCHAR(32) NOT NULL, controller_name VARCHAR(255) NOT NULL DEFAULT '', first_seen VARCHAR(32) NOT NULL DEFAULT '', last_seen VARCHAR(32) NOT NULL DEFAULT '', read_count BIGINT UNSIGNED NOT NULL DEFAULT 0, last_raw_hex TEXT NOT NULL, PRIMARY KEY(controller_node,card), KEY idx_skud_controller_card(card)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        "CREATE TABLE IF NOT EXISTS skud_meta (meta_key VARCHAR(100) NOT NULL PRIMARY KEY, meta_value TEXT NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        "INSERT INTO skud_meta(meta_key,meta_value) VALUES('schema_version','2') ON DUPLICATE KEY UPDATE meta_value=VALUES(meta_value)"
    };
    for(const auto*q:schema)if(mysql_query(c,q)!=0){error=mysql_error(c);status_="ERROR schema: "+error;return false;}return true;
}

bool MariaDbUserStore::load(std::vector<User>& users,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);
    if(mysql_query(c,"SELECT id,enabled,last_name,first_name,middle_name,department,position,pin_code,access_mode,controller_port,valid_from,valid_until,telegram_arrival,telegram_departure FROM skud_users ORDER BY id")!=0){error=mysql_error(c);status_="ERROR load users: "+error;return false;}
    MYSQL_RES* res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}users.clear();std::map<int,std::size_t> index;MYSQL_ROW r;
    while((r=mysql_fetch_row(res))){User u;u.id=toInt(r[0]);u.enabled=toInt(r[1],1)!=0;u.last_name=nullable(r[2]);u.first_name=nullable(r[3]);u.middle_name=nullable(r[4]);u.department=nullable(r[5]);u.position=nullable(r[6]);u.pin_code=nullable(r[7]);u.access_mode=nullable(r[8]);u.controller_port=toInt(r[9]);u.valid_from=nullable(r[10]);u.valid_until=nullable(r[11]);u.telegram_arrival=toInt(r[12],1)!=0;u.telegram_departure=toInt(r[13],1)!=0;index[u.id]=users.size();users.push_back(std::move(u));}
    mysql_free_result(res);
    if(mysql_query(c,"SELECT user_id,card FROM skud_user_cards ORDER BY user_id,card_order")!=0){error=mysql_error(c);status_="ERROR load cards: "+error;return false;}res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}
    while((r=mysql_fetch_row(res))){const int id=toInt(r[0]);auto it=index.find(id);if(it!=index.end())users[it->second].cards.push_back(nullable(r[1]));}mysql_free_result(res);status_="ONLINE; users="+std::to_string(users.size());return true;
}

bool MariaDbUserStore::save(const std::vector<User>& users,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"START TRANSACTION")!=0){error=mysql_error(c);return false;}auto rollback=[&](){mysql_query(c,"ROLLBACK");};
    if(mysql_query(c,"DELETE FROM skud_user_cards")!=0||mysql_query(c,"DELETE FROM skud_users")!=0){error=mysql_error(c);rollback();return false;}
    for(const auto&u:users){std::ostringstream q;q<<"INSERT INTO skud_users(id,enabled,last_name,first_name,middle_name,department,position,pin_code,access_mode,controller_port,valid_from,valid_until,telegram_arrival,telegram_departure) VALUES("<<u.id<<','<<(u.enabled?1:0)<<",'"<<escLocked(u.last_name)<<"','"<<escLocked(u.first_name)<<"','"<<escLocked(u.middle_name)<<"','"<<escLocked(u.department)<<"','"<<escLocked(u.position)<<"','"<<escLocked(u.pin_code)<<"','"<<escLocked(u.access_mode)<<"',"<<u.controller_port<<",'"<<escLocked(u.valid_from)<<"','"<<escLocked(u.valid_until)<<"',"<<(u.telegram_arrival?1:0)<<','<<(u.telegram_departure?1:0)<<')';if(mysql_query(c,q.str().c_str())!=0){error=mysql_error(c);rollback();status_="ERROR save user: "+error;return false;}
        for(std::size_t i=0;i<u.cards.size();++i){std::uint16_t series=0,number=0;if(!util::parseCardId(u.cards[i],series,number,nullptr))continue;std::ostringstream qc;qc<<"INSERT INTO skud_user_cards(user_id,card_order,card,series,number) VALUES("<<u.id<<','<<i<<",'"<<escLocked(util::formatCardId(series,number))<<"',"<<series<<','<<number<<')';if(mysql_query(c,qc.str().c_str())!=0){error=mysql_error(c);rollback();status_="ERROR save card: "+error;return false;}}
    }
    if(mysql_query(c,"COMMIT")!=0){error=mysql_error(c);rollback();return false;}status_="ONLINE; users="+std::to_string(users.size());return true;
}

bool MariaDbUserStore::loadDepartments(std::vector<std::string>& out,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"SELECT name FROM skud_departments ORDER BY name")!=0){error=mysql_error(c);return false;}auto*res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}out.clear();MYSQL_ROW r;while((r=mysql_fetch_row(res)))out.push_back(nullable(r[0]));mysql_free_result(res);return true;
}
bool MariaDbUserStore::saveDepartments(const std::vector<std::string>& departments,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"START TRANSACTION")!=0){error=mysql_error(c);return false;}if(mysql_query(c,"DELETE FROM skud_departments")!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}for(const auto&name:departments){std::string q="INSERT INTO skud_departments(name) VALUES('"+escLocked(name)+"')";if(mysql_query(c,q.c_str())!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}}if(mysql_query(c,"COMMIT")!=0){error=mysql_error(c);return false;}return true;
}

bool MariaDbUserStore::loadControllers(std::vector<Controller>& out,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"SELECT node,name,model,enabled,last_seen,last_raw_hex FROM skud_controllers ORDER BY node")!=0){error=mysql_error(c);return false;}auto*res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}out.clear();MYSQL_ROW r;while((r=mysql_fetch_row(res))){Controller x;x.node=toInt(r[0]);x.name=nullable(r[1]);x.model=nullable(r[2]);x.enabled=toInt(r[3],1)!=0;x.last_seen=nullable(r[4]);x.last_raw_hex=nullable(r[5]);out.push_back(std::move(x));}mysql_free_result(res);return true;
}
bool MariaDbUserStore::saveControllers(const std::vector<Controller>& controllers,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"START TRANSACTION")!=0){error=mysql_error(c);return false;}if(mysql_query(c,"DELETE FROM skud_controllers")!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}for(const auto&x:controllers){std::ostringstream q;q<<"INSERT INTO skud_controllers(node,name,model,enabled,last_seen,last_raw_hex) VALUES("<<x.node<<",'"<<escLocked(x.name)<<"','"<<escLocked(x.model)<<"',"<<(x.enabled?1:0)<<",'"<<escLocked(x.last_seen)<<"','"<<escLocked(x.last_raw_hex)<<"')";if(mysql_query(c,q.str().c_str())!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}}if(mysql_query(c,"COMMIT")!=0){error=mysql_error(c);return false;}return true;
}

bool MariaDbUserStore::appendEvent(const AttendanceEvent& e,std::string& error,const std::string& source_salt){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);const auto type=eventTypeText(e.type);
    // Controller FIFO frames are stable across retries. Use a deterministic
    // source key so an event written to MariaDB but not yet deleted by 37H is
    // harmless when it is read again after a restart.
    std::string source;
    if(source_salt.empty()&&!e.raw_hex.empty()&&e.controller_node>0)
        source=util::sha256Hex("controller|"+std::to_string(e.controller_node)+"|"+e.raw_hex);
    else{
        const auto salt=source_salt.empty()?util::randomToken(12):source_salt;
        source=util::sha256Hex(e.timestamp+"|"+type+"|"+e.card+"|"+std::to_string(e.user_id)+"|"+std::to_string(e.controller_node)+"|"+e.raw_hex+"|"+salt);
    }
    std::ostringstream q;q<<"INSERT IGNORE INTO skud_attendance_events(source_key,timestamp,type,card,user_id,user_name,department,controller_node,controller_name,raw_hex) VALUES('"<<source<<"','"<<escLocked(e.timestamp)<<"','"<<type<<"','"<<escLocked(e.card)<<"',"<<e.user_id<<",'"<<escLocked(e.user_name)<<"','"<<escLocked(e.department)<<"',"<<e.controller_node<<",'"<<escLocked(e.controller_name)<<"','"<<escLocked(e.raw_hex)<<"')";if(mysql_query(c,q.str().c_str())!=0){error=mysql_error(c);return false;}return true;
}

bool MariaDbUserStore::hasControllerEvent(int controller_node,const std::string& raw_hex,bool& exists,std::string& error){
    exists=false;if(raw_hex.empty())return true;
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);
    std::ostringstream q;q<<"SELECT 1 FROM skud_attendance_events WHERE controller_node="<<controller_node<<" AND raw_hex='"<<escLocked(raw_hex)<<"' LIMIT 1";
    if(mysql_query(c,q.str().c_str())!=0){error=mysql_error(c);return false;}auto*res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}exists=mysql_num_rows(res)>0;mysql_free_result(res);return true;
}
bool MariaDbUserStore::loadEventsByDate(const std::string& date,std::vector<AttendanceEvent>& out,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);const auto prefix=escLocked(date)+"%";std::string q="SELECT timestamp,type,card,user_id,user_name,department,controller_node,controller_name,raw_hex FROM skud_attendance_events WHERE timestamp LIKE '"+prefix+"' ORDER BY timestamp,id";if(mysql_query(c,q.c_str())!=0){error=mysql_error(c);return false;}auto*res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}out.clear();MYSQL_ROW r;while((r=mysql_fetch_row(res))){AttendanceEvent e;e.timestamp=nullable(r[0]);e.type=eventTypeFromText(nullable(r[1]));e.card=nullable(r[2]);e.user_id=toInt(r[3]);e.user_name=nullable(r[4]);e.department=nullable(r[5]);e.controller_node=toInt(r[6]);e.controller_name=nullable(r[7]);e.raw_hex=nullable(r[8]);out.push_back(std::move(e));}mysql_free_result(res);return true;
}

bool MariaDbUserStore::loadAttendanceDates(std::vector<std::string>& out,std::string& error){
    std::lock_guard lk(mu_);
    if(!connectLocked(error))return false;
    auto*c=static_cast<MYSQL*>(conn_);
    const char*q="SELECT DISTINCT LEFT(timestamp,10) AS event_date FROM skud_attendance_events WHERE type IN ('arrival','departure') AND CHAR_LENGTH(timestamp)>=10 ORDER BY event_date";
    if(mysql_query(c,q)!=0){error=mysql_error(c);return false;}
    auto*res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}
    out.clear();MYSQL_ROW r;
    while((r=mysql_fetch_row(res))){
        const std::string date=nullable(r[0]);
        if(date.size()==10&&date[4]=='-'&&date[7]=='-')out.push_back(date);
    }
    mysql_free_result(res);
    return true;
}

bool MariaDbUserStore::loadCardStates(std::map<std::string,PersistedCardState>& out,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"SELECT state_key,state,last_read FROM skud_attendance_state ORDER BY state_key")!=0){error=mysql_error(c);return false;}auto*res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}out.clear();MYSQL_ROW r;while((r=mysql_fetch_row(res)))out[nullable(r[0])]={nullable(r[1])=="present"?PresenceState::Present:PresenceState::Absent,nullable(r[2])};mysql_free_result(res);return true;
}
bool MariaDbUserStore::saveCardStates(const std::map<std::string,PersistedCardState>& states,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"START TRANSACTION")!=0){error=mysql_error(c);return false;}if(mysql_query(c,"DELETE FROM skud_attendance_state")!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}for(const auto&[key,st]:states){std::string q="INSERT INTO skud_attendance_state(state_key,state,last_read) VALUES('"+escLocked(key)+"','"+(st.state==PresenceState::Present?"present":"absent")+"','"+escLocked(st.last_read)+"')";if(mysql_query(c,q.c_str())!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}}if(mysql_query(c,"COMMIT")!=0){error=mysql_error(c);return false;}return true;
}

bool MariaDbUserStore::loadActivities(std::vector<CardActivity>& out,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"SELECT card,user_id,user_name,department,last_read,last_event,controller_node FROM skud_card_activity ORDER BY last_read DESC,card")!=0){error=mysql_error(c);return false;}auto*res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}out.clear();MYSQL_ROW r;while((r=mysql_fetch_row(res))){CardActivity x;x.card=nullable(r[0]);x.user_id=toInt(r[1]);x.user_name=nullable(r[2]);x.department=nullable(r[3]);x.last_read=nullable(r[4]);x.last_event=nullable(r[5]);x.controller_node=toInt(r[6]);out.push_back(std::move(x));}mysql_free_result(res);return true;
}
bool MariaDbUserStore::saveActivities(const std::vector<CardActivity>& activities,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"START TRANSACTION")!=0){error=mysql_error(c);return false;}if(mysql_query(c,"DELETE FROM skud_card_activity")!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}for(const auto&x:activities){std::ostringstream q;q<<"INSERT INTO skud_card_activity(card,user_id,user_name,department,last_read,last_event,controller_node) VALUES('"<<escLocked(x.card)<<"',"<<x.user_id<<",'"<<escLocked(x.user_name)<<"','"<<escLocked(x.department)<<"','"<<escLocked(x.last_read)<<"','"<<escLocked(x.last_event)<<"',"<<x.controller_node<<')';if(mysql_query(c,q.str().c_str())!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}}if(mysql_query(c,"COMMIT")!=0){error=mysql_error(c);return false;}return true;
}

bool MariaDbUserStore::loadControllerCards(std::vector<ControllerCardRecord>& out,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"SELECT card,controller_node,controller_name,first_seen,last_seen,read_count,last_raw_hex FROM skud_controller_cards ORDER BY last_seen DESC,controller_node,card")!=0){error=mysql_error(c);return false;}auto*res=mysql_store_result(c);if(!res){error=mysql_error(c);return false;}out.clear();MYSQL_ROW r;while((r=mysql_fetch_row(res))){ControllerCardRecord x;x.card=nullable(r[0]);x.controller_node=toInt(r[1]);x.controller_name=nullable(r[2]);x.first_seen=nullable(r[3]);x.last_seen=nullable(r[4]);x.read_count=toU64(r[5]);x.last_raw_hex=nullable(r[6]);out.push_back(std::move(x));}mysql_free_result(res);return true;
}
bool MariaDbUserStore::saveControllerCards(const std::vector<ControllerCardRecord>& cards,std::string& error){
    std::lock_guard lk(mu_);if(!connectLocked(error))return false;auto*c=static_cast<MYSQL*>(conn_);if(mysql_query(c,"START TRANSACTION")!=0){error=mysql_error(c);return false;}if(mysql_query(c,"DELETE FROM skud_controller_cards")!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}for(const auto&x:cards){std::ostringstream q;q<<"INSERT INTO skud_controller_cards(controller_node,card,controller_name,first_seen,last_seen,read_count,last_raw_hex) VALUES("<<x.controller_node<<",'"<<escLocked(x.card)<<"','"<<escLocked(x.controller_name)<<"','"<<escLocked(x.first_seen)<<"','"<<escLocked(x.last_seen)<<"',"<<x.read_count<<",'"<<escLocked(x.last_raw_hex)<<"')";if(mysql_query(c,q.str().c_str())!=0){error=mysql_error(c);mysql_query(c,"ROLLBACK");return false;}}if(mysql_query(c,"COMMIT")!=0){error=mysql_error(c);return false;}return true;
}

std::string MariaDbUserStore::status()const{std::lock_guard lk(mu_);return status_;}
}
