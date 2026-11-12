#include "skud/WebServer.h"
#include "skud/AttendanceEngine.h"
#include "skud/Config.h"
#include "skud/ControllerManager.h"
#include "skud/DepartmentManager.h"
#include "skud/ReportManager.h"
#include "skud/SoyalImport.h"
#include "skud/TelegramNotifier.h"
#include "skud/UserManager.h"
#include "skud/Util.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace skud {
WebServer::WebServer(Config&c,UserManager&u,DepartmentManager&d,AttendanceEngine&a,ControllerManager&cm,TelegramNotifier&t,ReportManager&r,std::string root):cfg_(c),users_(u),departments_(d),attendance_(a),controllers_(cm),telegram_(t),reports_(r),root_(std::move(root)){}
WebServer::~WebServer(){stop();}
bool WebServer::start(){if(running_)return true;server_fd_=::socket(AF_INET,SOCK_STREAM,0);if(server_fd_<0)return false;int one=1;setsockopt(server_fd_,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=INADDR_ANY;addr.sin_port=htons(cfg_.getInt("server.port",8080));if(bind(server_fd_,(sockaddr*)&addr,sizeof(addr))<0||listen(server_fd_,32)<0){::close(server_fd_);server_fd_=-1;return false;}running_=true;thread_=std::thread(&WebServer::loop,this);return true;}
void WebServer::stop(){running_=false;if(server_fd_>=0){shutdown(server_fd_,SHUT_RDWR);::close(server_fd_);server_fd_=-1;}if(thread_.joinable())thread_.join();}
void WebServer::loop(){while(running_){int c=accept(server_fd_,nullptr,nullptr);if(c<0){if(!running_)break;continue;}std::thread(&WebServer::handleClient,this,c).detach();}}
static std::string lower(std::string s){for(char&c:s)c=std::tolower((unsigned char)c);return s;}
static std::vector<int> parseIntList(const std::string&s){std::vector<int> out;for(auto&part:util::split(s,',')){try{int v=std::stoi(util::trim(part));if(v>0&&std::find(out.begin(),out.end(),v)==out.end())out.push_back(v);}catch(...){}}return out;}
static bool parseCardList(std::string text,std::vector<std::string>&out,std::string&error){
    std::replace(text.begin(),text.end(),'|',',');
    for(const auto&part:util::split(text,',')){
        const auto value=util::trim(part);if(value.empty())continue;
        std::uint16_t series=0,number=0;
        if(!util::parseCardId(value,series,number,&error))return false;
        const auto canonical=util::formatCardId(series,number);
        if(std::find(out.begin(),out.end(),canonical)==out.end())out.push_back(canonical);
    }
    return true;
}

static std::string localUserName(const User&u){
    std::string n=u.last_name;
    if(!u.first_name.empty()){if(!n.empty())n+=' ';n+=u.first_name;}
    if(!u.middle_name.empty()){if(!n.empty())n+=' ';n+=u.middle_name;}
    return n.empty()?("Пользователь №"+std::to_string(u.id)):n;
}

static std::optional<User> uniqueUserByControllerPort(UserManager&users,int address){
    if(address<0||address>1023)return std::nullopt;std::optional<User> found;
    for(const auto&u:users.list())if(u.controller_port==address){if(found)return std::nullopt;found=u;}
    return found;
}

static void mergeSoyalFields(User&u,const SoyalImportRecord&r){
    if(u.last_name.empty())u.last_name=r.last_name;
    if(u.first_name.empty())u.first_name=r.first_name;
    if(u.middle_name.empty())u.middle_name=r.middle_name;
    if(u.department.empty())u.department=r.department;
    if(u.position.empty())u.position=r.position;
    if(u.pin_code.empty()&&!r.pin_code.empty())u.pin_code=r.pin_code;
    if(u.controller_port==0&&r.address>=0&&r.address<=16383)u.controller_port=r.address;
}

static SoyalImportRecord soyalRecordFromForm(const std::map<std::string,std::string>&f){
    SoyalImportRecord r;try{r.address=std::stoi(f.at("address"));}catch(...){r.address=0;}
    auto get=[&](const char*k)->std::string{auto it=f.find(k);return it==f.end()?std::string{}:it->second;};
    r.card=util::trim(get("card"));r.pin_code=util::trim(get("pin_code"));r.last_name=get("last_name");r.first_name=get("first_name");r.middle_name=get("middle_name");r.department=get("department");r.position=get("position");r.full_name=get("full_name");r.source=get("source");
    std::uint16_t series=0,number=0;if(!r.card.empty()&&util::parseCardId(r.card,series,number,nullptr)){r.card_series=series;r.card_number=number;r.card=util::formatCardId(series,number);}
    return r;
}
void WebServer::handleClient(int fd){std::string data;char buf[4096];while(data.find("\r\n\r\n")==std::string::npos&&data.size()<65536){auto n=recv(fd,buf,sizeof(buf),0);if(n<=0){close(fd);return;}data.append(buf,n);}auto hp=data.find("\r\n\r\n");std::string head=data.substr(0,hp),body=data.substr(hp+4);std::istringstream hs(head);std::string line;Req r;if(!std::getline(hs,line)){close(fd);return;}if(!line.empty()&&line.back()=='\r')line.pop_back();std::istringstream l1(line);std::string target,ver;l1>>r.method>>target>>ver;auto q=target.find('?');r.path=q==std::string::npos?target:target.substr(0,q);r.query=q==std::string::npos?"":target.substr(q+1);while(std::getline(hs,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();auto p=line.find(':');if(p!=std::string::npos)r.headers[lower(util::trim(line.substr(0,p)))]=util::trim(line.substr(p+1));}size_t cl=0;try{cl=std::stoul(r.headers["content-length"]);}catch(...){}while(body.size()<cl){auto n=recv(fd,buf,sizeof(buf),0);if(n<=0)break;body.append(buf,n);}r.body=body.substr(0,cl);auto res=route(r);std::ostringstream out;std::string reason=res.code==200?"OK":res.code==302?"Found":res.code==401?"Unauthorized":res.code==404?"Not Found":"Bad Request";out<<"HTTP/1.1 "<<res.code<<' '<<reason<<"\r\nContent-Type: "<<res.type<<"\r\nContent-Length: "<<res.body.size()<<"\r\nConnection: close\r\n";for(auto&h:res.headers)out<<h.first<<": "<<h.second<<"\r\n";out<<"\r\n"<<res.body;auto s=out.str();std::size_t sent=0;while(sent<s.size()){auto n=send(fd,s.data()+sent,s.size()-sent,MSG_NOSIGNAL);if(n<=0)break;sent+=static_cast<std::size_t>(n);}close(fd);}
std::string WebServer::cookie(const Req&r,const std::string&name)const{auto it=r.headers.find("cookie");if(it==r.headers.end())return{};for(auto&p:util::split(it->second,';')){auto x=p.find('=');if(x!=std::string::npos&&util::trim(p.substr(0,x))==name)return util::trim(p.substr(x+1));}return{};}
bool WebServer::authorized(const Req&r)const{auto sid=cookie(r,"SKUDSID");if(sid.empty())return false;std::lock_guard lk(sessions_mu_);return sessions_.count(sid)>0;}
WebServer::Res WebServer::file(const std::string&name,const std::string&type){const auto path=std::filesystem::path(root_)/"web"/name;std::ifstream f(path,std::ios::binary);if(!f){return{404,"text/plain; charset=utf-8","UI file not found: "+path.string()};}std::ostringstream o;o<<f.rdbuf();Res x{200,type,o.str()};x.headers.push_back({"Cache-Control","no-store, no-cache, must-revalidate, max-age=0"});x.headers.push_back({"Pragma","no-cache"});return x;}
WebServer::Res WebServer::jsonUsers(){
    auto v=users_.list();std::ostringstream o;o<<"[";bool first=true;
    for(const auto&u:v){
        if(!first)o<<',';first=false;
        o<<"{\"id\":"<<u.id<<",\"enabled\":"<<(u.enabled?"true":"false")
         <<",\"last_name\":\""<<util::jsonEscape(u.last_name)<<"\",\"first_name\":\""<<util::jsonEscape(u.first_name)<<"\",\"middle_name\":\""<<util::jsonEscape(u.middle_name)<<"\""
         <<",\"department\":\""<<util::jsonEscape(u.department)<<"\",\"position\":\""<<util::jsonEscape(u.position)<<"\""
         <<",\"card\":\""<<util::jsonEscape(u.card)<<"\",\"card_series\":\""<<util::jsonEscape(u.card_series)<<"\",\"card_number\":\""<<util::jsonEscape(u.card_number)<<"\""
         <<",\"cards\":[";
        bool first_card=true;for(const auto&card:u.cards){if(!first_card)o<<',';first_card=false;o<<"\""<<util::jsonEscape(card)<<"\"";}
        o<<"],\"pin_code\":\""<<util::jsonEscape(u.pin_code)<<"\",\"access_mode\":\""<<util::jsonEscape(u.access_mode)<<"\",\"controller_port\":"<<u.controller_port<<"}";
    }
    o<<"]";return{200,"application/json; charset=utf-8",o.str()};
}
WebServer::Res WebServer::jsonDepartments(){auto v=departments_.list();std::ostringstream o;o<<"[";bool first=true;for(const auto&name:v){if(!first)o<<',';first=false;o<<"\""<<util::jsonEscape(name)<<"\"";}o<<"]";return{200,"application/json; charset=utf-8",o.str()};}
WebServer::Res WebServer::jsonCards(){auto v=attendance_.activities();std::ostringstream o;o<<"[";bool first=true;for(auto&a:v){if(!first)o<<',';first=false;o<<"{\"card\":\""<<util::jsonEscape(a.card)<<"\",\"user_id\":"<<a.user_id<<",\"user_name\":\""<<util::jsonEscape(a.user_name)<<"\",\"department\":\""<<util::jsonEscape(a.department)<<"\",\"last_read\":\""<<a.last_read<<"\",\"last_event\":\""<<util::jsonEscape(a.last_event)<<"\",\"controller_node\":"<<a.controller_node<<"}";}o<<"]";return{200,"application/json; charset=utf-8",o.str()};}

WebServer::Res WebServer::jsonControllerCards(){
    auto cards=controllers_.controllerCards();std::ostringstream o;o<<"[";bool first=true;
    for(const auto&c:cards){
        if(!first)o<<',';first=false;auto user=users_.byCard(c.card);std::string name;
        if(user){name=user->last_name;if(!user->first_name.empty()){if(!name.empty())name+=' ';name+=user->first_name;}if(!user->middle_name.empty()){if(!name.empty())name+=' ';name+=user->middle_name;}if(name.empty())name="Пользователь №"+std::to_string(user->id);}
        o<<"{\"card\":\""<<util::jsonEscape(c.card)<<"\",\"controller_node\":"<<c.controller_node
         <<",\"controller_name\":\""<<util::jsonEscape(c.controller_name)<<"\",\"first_seen\":\""<<util::jsonEscape(c.first_seen)
         <<"\",\"last_seen\":\""<<util::jsonEscape(c.last_seen)<<"\",\"read_count\":"<<c.read_count
         <<",\"last_raw_hex\":\""<<util::jsonEscape(c.last_raw_hex)<<"\",\"linked\":"<<(user?"true":"false")
         <<",\"user_id\":"<<(user?user->id:0)<<",\"user_name\":\""<<util::jsonEscape(name)<<"\",\"department\":\""<<util::jsonEscape(user?user->department:std::string{})<<"\"}";
    }
    o<<"]";return{200,"application/json; charset=utf-8",o.str()};
}
WebServer::Res WebServer::jsonTodayAttendance(){
    auto v=attendance_.todayAttendance();
    std::ostringstream o;o<<"[";bool first=true;
    for(const auto&a:v){
        if(!first)o<<',';first=false;
        o<<"{\"user_id\":"<<a.user_id
         <<",\"user_name\":\""<<util::jsonEscape(a.user_name)<<"\""
         <<",\"position\":\""<<util::jsonEscape(a.position)<<"\""
         <<",\"department\":\""<<util::jsonEscape(a.department)<<"\""
         <<",\"card\":\""<<util::jsonEscape(a.card)<<"\""
         <<",\"arrival_time\":\""<<util::jsonEscape(a.arrival_time)<<"\""
         <<",\"departure_time\":\""<<util::jsonEscape(a.departure_time)<<"\""
         <<",\"status\":\""<<(a.presence==PresenceState::Present?"at_work":"left")<<"\""
         <<"}";
    }
    o<<"]";
    return{200,"application/json; charset=utf-8",o.str()};
}
WebServer::Res WebServer::jsonControllers(){auto v=controllers_.controllers();std::ostringstream o;o<<"[";bool first=true;for(auto&c:v){if(!first)o<<',';first=false;o<<"{\"node\":"<<c.node<<",\"reported_node\":"<<c.reported_node<<",\"name\":\""<<util::jsonEscape(c.name)<<"\",\"model\":\""<<util::jsonEscape(c.model)<<"\",\"online\":"<<(c.online?"true":"false")<<",\"last_seen\":\""<<c.last_seen<<"\",\"last_raw_hex\":\""<<util::jsonEscape(c.last_raw_hex)<<"\",\"id_status\":\""<<util::jsonEscape(c.id_status)<<"\"}";}o<<"]";return{200,"application/json; charset=utf-8",o.str()};}
WebServer::Res WebServer::jsonUserUploadJob(const ControllerUserUploadJob&job){
    std::ostringstream o;o<<"{\"id\":"<<job.id<<",\"created_at\":\""<<util::jsonEscape(job.created_at)<<"\",\"state\":\""<<util::jsonEscape(job.state)<<"\",\"total\":"<<job.total<<",\"completed\":"<<job.completed<<",\"success\":"<<job.success<<",\"failed\":"<<job.failed<<",\"skipped\":"<<job.skipped<<",\"full_sync\":"<<(job.full_sync?"true":"false")<<",\"results\":[";
    bool first=true;for(const auto&r:job.results){if(!first)o<<',';first=false;o<<"{\"user_id\":"<<r.user_id<<",\"controller_node\":"<<r.controller_node<<",\"status\":\""<<util::jsonEscape(r.status)<<"\",\"message\":\""<<util::jsonEscape(r.message)<<"\"}";}o<<"]}";
    return{200,"application/json; charset=utf-8",o.str()};
}
WebServer::Res WebServer::jsonControllerActionJob(const ControllerActionJob&job){
    std::ostringstream o;
    o<<"{\"id\":"<<job.id
     <<",\"created_at\":\""<<util::jsonEscape(job.created_at)<<"\""
     <<",\"state\":\""<<util::jsonEscape(job.state)<<"\""
     <<",\"action\":\""<<util::jsonEscape(job.action)<<"\""
     <<",\"controller_node\":"<<job.controller_node
     <<",\"new_controller_node\":"<<job.new_controller_node
     <<",\"ok\":"<<(job.ok?"true":"false")
     <<",\"status\":\""<<util::jsonEscape(job.status)<<"\""
     <<",\"message\":\""<<util::jsonEscape(job.message)<<"\"}";
    return{200,"application/json; charset=utf-8",o.str()};
}

WebServer::Res WebServer::jsonAttendanceReadJob(const ControllerAttendanceReadJob&job){
    std::ostringstream o;
    o<<"{\"id\":"<<job.id
     <<",\"created_at\":\""<<util::jsonEscape(job.created_at)<<"\""
     <<",\"state\":\""<<util::jsonEscape(job.state)<<"\""
     <<",\"controller_node\":"<<job.controller_node
     <<",\"ok\":"<<(job.ok?"true":"false")
     <<",\"status\":\""<<util::jsonEscape(job.status)<<"\""
     <<",\"message\":\""<<util::jsonEscape(job.message)<<"\""
     <<",\"read\":"<<job.read
     <<",\"stored\":"<<job.stored
     <<",\"access_events\":"<<job.access_events
     <<",\"raw_events\":"<<job.raw_events
     <<",\"duplicates\":"<<job.duplicates
     <<",\"failed\":"<<job.failed
     <<",\"first_event_time\":\""<<util::jsonEscape(job.first_event_time)<<"\""
     <<",\"last_event_time\":\""<<util::jsonEscape(job.last_event_time)<<"\"}";
    return{200,"application/json; charset=utf-8",o.str()};
}

WebServer::Res WebServer::jsonUserDeleteJob(const ControllerUserDeleteJob&job){
    std::ostringstream o;
    o<<"{\"id\":"<<job.id
     <<",\"created_at\":\""<<util::jsonEscape(job.created_at)<<"\""
     <<",\"state\":\""<<util::jsonEscape(job.state)<<"\""
     <<",\"delete_from_system\":"<<(job.delete_from_system?"true":"false")
     <<",\"total\":"<<job.total
     <<",\"completed\":"<<job.completed
     <<",\"success\":"<<job.success
     <<",\"failed\":"<<job.failed
     <<",\"local_deleted\":"<<job.local_deleted
     <<",\"local_retained\":"<<job.local_retained
     <<",\"results\":[";
    bool first=true;
    for(const auto&r:job.results){
        if(!first)o<<',';first=false;
        o<<"{\"user_id\":"<<r.user_id
         <<",\"controller_node\":"<<r.controller_node
         <<",\"status\":\""<<util::jsonEscape(r.status)<<"\""
         <<",\"message\":\""<<util::jsonEscape(r.message)<<"\"}";
    }
    o<<"]}";
    return{200,"application/json; charset=utf-8",o.str()};
}
WebServer::Res WebServer::jsonUserReadJob(const ControllerUserReadJob&job){
    std::ostringstream o;
    o<<"{\"id\":"<<job.id
     <<",\"created_at\":\""<<util::jsonEscape(job.created_at)<<"\""
     <<",\"state\":\""<<util::jsonEscape(job.state)<<"\""
     <<",\"total\":"<<job.total
     <<",\"completed\":"<<job.completed
     <<",\"matches\":"<<job.matches
     <<",\"differences\":"<<job.differences
     <<",\"missing\":"<<job.missing
     <<",\"unknown\":"<<job.unknown
     <<",\"unverified\":"<<job.unverified
     <<",\"empty\":"<<job.empty
     <<",\"failed\":"<<job.failed
     <<",\"results\":[";
    bool first=true;
    for(const auto&r:job.results){
        if(!first)o<<',';first=false;
        o<<"{\"controller_node\":"<<r.controller_node
         <<",\"address\":"<<r.address
         <<",\"local_user_id\":"<<r.local_user_id
         <<",\"local_user_name\":\""<<util::jsonEscape(r.local_user_name)<<"\""
         <<",\"controller_card\":\""<<util::jsonEscape(r.controller_card)<<"\""
         <<",\"controller_enabled\":"<<(r.controller_enabled?"true":"false")
         <<",\"pin_set\":"<<(r.pin_set?"true":"false")
         <<",\"access_mode\":\""<<util::jsonEscape(r.access_mode)<<"\""
         <<",\"card_known\":"<<(r.card_known?"true":"false")
         <<",\"details_known\":"<<(r.details_known?"true":"false")
         <<",\"raw_record_hex\":\""<<util::jsonEscape(r.raw_record_hex)<<"\""
         <<",\"status\":\""<<util::jsonEscape(r.status)<<"\""
         <<",\"message\":\""<<util::jsonEscape(r.message)<<"\"}";
    }
    o<<"]}";
    return{200,"application/json; charset=utf-8",o.str()};
}

WebServer::Res WebServer::jsonEepromSearchJob(const ControllerEepromSearchJob&job){
    std::ostringstream o;
    o<<"{\"id\":"<<job.id<<",\"created_at\":\""<<util::jsonEscape(job.created_at)<<"\",\"state\":\""<<util::jsonEscape(job.state)<<"\""
     <<",\"card_series\":"<<job.card_series<<",\"card_number\":"<<job.card_number<<",\"compact_user_addresses\":[";
    bool first_addr=true;for(int a:job.compact_user_addresses){if(!first_addr)o<<',';first_addr=false;o<<a;}o<<"]";
    o<<",\"compact_probes\":[";bool first_probe=true;for(const auto&n:job.compact_probes){if(!first_probe)o<<',';first_probe=false;o<<"\""<<util::jsonEscape(n)<<"\"";}o<<"]"
     <<",\"start_address\":"<<job.start_address<<",\"end_address\":"<<job.end_address<<",\"block_size\":"<<job.block_size
     <<",\"total\":"<<job.total<<",\"completed\":"<<job.completed<<",\"failed\":"<<job.failed<<",\"truncated\":"<<(job.truncated?"true":"false")<<",\"matches\":[";
    bool first=true;for(const auto&m:job.matches){if(!first)o<<',';first=false;o<<"{\"controller_node\":"<<m.controller_node<<",\"eeprom_address\":"<<m.eeprom_address<<",\"pattern\":\""<<util::jsonEscape(m.pattern)<<"\",\"exact\":"<<(m.exact?"true":"false")<<",\"matched_hex\":\""<<util::jsonEscape(m.matched_hex)<<"\",\"context_hex\":\""<<util::jsonEscape(m.context_hex)<<"\"}";}o<<"],\"errors\":[";
    first=true;for(const auto&e:job.errors){if(!first)o<<',';first=false;o<<"{\"controller_node\":"<<e.controller_node<<",\"eeprom_address\":"<<e.eeprom_address<<",\"message\":\""<<util::jsonEscape(e.message)<<"\"}";}o<<"]}";
    return{200,"application/json; charset=utf-8",o.str()};
}

WebServer::Res WebServer::jsonStatus(){auto p=attendance_.presentUsers();const auto registered=users_.list().size();auto m=system_metrics_.snapshot();std::ostringstream o;o<<"{\"serial_status\":\""<<controllers_.serialStatus()<<"\",\"serial_device\":\""<<util::jsonEscape(controllers_.serialDevice())<<"\",\"present_count\":"<<p.size()<<",\"registered_count\":"<<registered<<",\"cpu_percent\":"<<m.cpu_percent<<",\"ram_percent\":"<<m.ram_percent<<",\"ram_used_mb\":"<<m.ram_used_mb<<",\"ram_total_mb\":"<<m.ram_total_mb<<",\"uptime_seconds\":"<<m.uptime_seconds<<"}";return{200,"application/json; charset=utf-8",o.str()};}
WebServer::Res WebServer::jsonSettings(){
    std::ostringstream o;
    o<<"{\"username\":\""<<util::jsonEscape(cfg_.get("auth.username","admin"))<<"\""
     <<",\"port\":\""<<util::jsonEscape(cfg_.get("server.port","8080"))<<"\""
     <<",\"serial_device\":\""<<util::jsonEscape(cfg_.get("serial.device","auto"))<<"\""
     <<",\"telegram_enabled\":"<<(cfg_.getBool("telegram.enabled",false)?"true":"false")
     <<",\"bot_token\":\""<<util::jsonEscape(cfg_.get("telegram.bot_token"))<<"\""
     <<",\"chat_id\":\""<<util::jsonEscape(cfg_.get("telegram.chat_id"))<<"\""
     <<",\"notify_arrival\":"<<(cfg_.getBool("telegram.notify_arrival",true)?"true":"false")
     <<",\"notify_departure\":"<<(cfg_.getBool("telegram.notify_departure",true)?"true":"false")
     <<",\"report_text_copy\":"<<(cfg_.getBool("telegram.report_text_copy",true)?"true":"false")
     <<",\"report_archive_enabled\":"<<(cfg_.getBool("reports.archive.enabled",false)?"true":"false")
     <<",\"report_archive_path\":\""<<util::jsonEscape(cfg_.get("reports.archive.path","/srv/samba/UNEX"))<<"\""
     <<",\"monitor_path\":\"/monitor.html\",\"mag250_path\":\"/mag250/\"}";
    return{200,"application/json; charset=utf-8",o.str()};
}

WebServer::Res WebServer::jsonMonitor(){
    auto all=users_.list();
    auto present=attendance_.presentUsers();
    auto today=attendance_.todayAttendance();
    auto ctrls=controllers_.controllers();
    std::map<int,bool> present_ids;
    for(const auto&u:present)present_ids[u.id]=true;
    std::map<int,DailyAttendance> today_by_user;
    for(const auto&r:today)today_by_user[r.user_id]=r;

    struct Row{User user;std::string arrival;std::string departure;std::string status;};
    std::vector<Row> rows;
    // Keep the TV monitor count consistent with the main web UI: both count
    // every user record. Disabled users remain visible instead of silently
    // disappearing from the monitor; they get a dedicated neutral status.
    const int registered=static_cast<int>(all.size());
    int on_site=0;
    for(const auto&u:all){
        const bool here=u.enabled&&present_ids.count(u.id)>0;
        if(here)++on_site;
        Row row;row.user=u;
        auto it=today_by_user.find(u.id);
        if(it!=today_by_user.end()){row.arrival=it->second.arrival_time;row.departure=it->second.departure_time;}
        row.status=!u.enabled?"disabled":(here?"at_work":(it!=today_by_user.end()?"left":"absent"));
        rows.push_back(std::move(row));
    }
    // TV monitor ordering: people currently on site must always be visible first.
    // Keep a deterministic name order inside each status group so 3-second
    // refreshes do not make rows jump around on a television dashboard.
    std::sort(rows.begin(),rows.end(),[](const Row&a,const Row&b){
        auto rank=[](const std::string&s){return s=="at_work"?0:s=="left"?1:s=="absent"?2:3;};
        const int ar=rank(a.status),br=rank(b.status);if(ar!=br)return ar<br;
        if(a.user.last_name!=b.user.last_name)return a.user.last_name<b.user.last_name;
        if(a.user.first_name!=b.user.first_name)return a.user.first_name<b.user.first_name;
        if(a.user.middle_name!=b.user.middle_name)return a.user.middle_name<b.user.middle_name;
        return a.user.id<b.user.id;
    });
    int online=0,enabled_ctrl=0;for(const auto&c:ctrls)if(c.enabled){++enabled_ctrl;if(c.online)++online;}
    std::ostringstream o;
    o<<"{\"generated_at\":\""<<util::jsonEscape(util::nowLocal())<<"\",\"date\":\""<<util::jsonEscape(util::todayLocal())
     <<"\",\"registered_count\":"<<registered<<",\"present_count\":"<<on_site
     <<",\"controllers_online\":"<<online<<",\"controllers_total\":"<<enabled_ctrl<<",\"workers\":[";
    bool first=true;
    for(const auto&r:rows){
        if(!first)o<<',';first=false;
        std::string name=r.user.last_name;
        if(!r.user.first_name.empty()){if(!name.empty())name+=' ';name+=r.user.first_name;}
        if(!r.user.middle_name.empty()){if(!name.empty())name+=' ';name+=r.user.middle_name;}
        if(name.empty())name="Пользователь №"+std::to_string(r.user.id);
        o<<"{\"id\":"<<r.user.id<<",\"name\":\""<<util::jsonEscape(name)<<"\",\"position\":\""<<util::jsonEscape(r.user.position)
         <<"\",\"department\":\""<<util::jsonEscape(r.user.department)<<"\",\"arrival_time\":\""<<util::jsonEscape(r.arrival)
         <<"\",\"departure_time\":\""<<util::jsonEscape(r.departure)<<"\",\"status\":\""<<r.status<<"\"}";
    }
    o<<"]}";
    return{200,"application/json; charset=utf-8",o.str()};
}
WebServer::Res WebServer::jsonReportSettings(){
    const auto today=reports_.todayRange(),week=reports_.currentWeekRange(),month=reports_.currentMonthRange();
    const auto s=reports_.schedule();
    std::ostringstream o;
    o<<"{\"today\":{\"from\":\""<<today.from<<"\",\"to\":\""<<today.to<<"\"}"
     <<",\"week\":{\"from\":\""<<week.from<<"\",\"to\":\""<<week.to<<"\"}"
     <<",\"month\":{\"from\":\""<<month.from<<"\",\"to\":\""<<month.to<<"\"}"
     <<",\"schedule\":{\"enabled\":"<<(s.enabled?"true":"false")
     <<",\"period\":\""<<util::jsonEscape(s.period)<<"\""
     <<",\"time\":\""<<util::jsonEscape(s.time)<<"\""
     <<",\"weekday\":"<<s.weekday
     <<",\"month_day\":"<<s.month_day
     <<",\"last_sent_at\":\""<<util::jsonEscape(s.last_sent_at)<<"\""
     <<",\"last_period\":\""<<util::jsonEscape(s.last_period)<<"\""
     <<",\"last_status\":\""<<util::jsonEscape(s.last_status)<<"\""
     <<",\"last_error\":\""<<util::jsonEscape(s.last_error)<<"\"}}";
    return{200,"application/json; charset=utf-8",o.str()};
}
WebServer::Res WebServer::jsonProtocolTrace(std::uint64_t after_id,std::size_t limit){
    auto entries=controllers_.protocolTrace(after_id,limit);std::ostringstream o;o<<"{\"entries\":[";bool first=true;std::uint64_t last=after_id;
    for(const auto&e:entries){if(!first)o<<',';first=false;last=std::max(last,e.id);o<<"{\"id\":"<<e.id<<",\"timestamp\":\""<<util::jsonEscape(e.timestamp)<<"\",\"direction\":\""<<util::jsonEscape(e.direction)<<"\",\"node\":"<<e.node<<",\"command\":"<<e.command<<",\"protocol\":\""<<util::jsonEscape(e.protocol)<<"\",\"raw_hex\":\""<<util::jsonEscape(e.raw_hex)<<"\",\"message\":\""<<util::jsonEscape(e.message)<<"\",\"card\":\""<<util::jsonEscape(e.card)<<"\",\"user_address\":"<<e.user_address<<"}";}
    o<<"],\"last_id\":"<<last<<"}";return{200,"application/json; charset=utf-8",o.str()};
}
WebServer::Res WebServer::route(const Req&r){
    // Read-only TV/kiosk monitor is intentionally available without an admin session.
    // It exposes attendance status/name/position only; no cards, PINs or credentials.
    if(r.path=="/monitor"||r.path=="/monitor.html")return file("monitor.html","text/html; charset=utf-8");
    if(r.path=="/monitor.css")return file("monitor.css","text/css; charset=utf-8");
    if(r.path=="/monitor.js")return file("monitor.js","application/javascript; charset=utf-8");
    if(r.path=="/mag250"||r.path=="/mag250/"||r.path=="/mag250/index.html")return file("mag250.html","text/html; charset=utf-8");
    if(r.path=="/mag250.css")return file("mag250.css","text/css; charset=utf-8");
    if(r.path=="/mag250.js")return file("mag250.js","application/javascript; charset=utf-8");
    if(r.path=="/api/monitor"&&r.method=="GET")return jsonMonitor();
    if(r.path=="/api/login"&&r.method=="POST"){auto f=util::parseForm(r.body);auto salt=cfg_.get("auth.salt");auto hash=util::sha256Hex(salt+f["password"]);if(f["username"]==cfg_.get("auth.username","admin")&&util::constantTimeEqual(hash,cfg_.get("auth.password_hash"))){auto sid=util::randomToken();{std::lock_guard lk(sessions_mu_);sessions_[sid]=f["username"];}Res x{200,"application/json","{\"ok\":true}"};x.headers.push_back({"Set-Cookie","SKUDSID="+sid+"; Path=/; HttpOnly; SameSite=Strict"});return x;}return{401,"application/json","{\"ok\":false}"};}
    if(r.path=="/login.html")return file("login.html","text/html; charset=utf-8");
    if(r.path=="/style.css")return file("style.css","text/css; charset=utf-8");
    if(r.path=="/app.js")return file("app.js","application/javascript; charset=utf-8");
    if(r.path=="/"&&!authorized(r)){Res x; x.code=302;x.type="text/plain";x.headers.push_back({"Location","/login.html"});return x;}
    if(r.path=="/")return file("index.html","text/html; charset=utf-8");
    if(!authorized(r))return{401,"application/json","{\"error\":\"auth required\"}"};
    if(r.path=="/api/logout"){auto sid=cookie(r,"SKUDSID");{std::lock_guard lk(sessions_mu_);sessions_.erase(sid);}Res x{200,"application/json","{}"};x.headers.push_back({"Set-Cookie","SKUDSID=; Path=/; Max-Age=0"});return x;}
    if(r.path=="/api/status")return jsonStatus();
    if(r.path=="/api/users"&&r.method=="GET")return jsonUsers();
    if(r.path=="/api/departments"&&r.method=="GET")return jsonDepartments();
    if(r.path=="/api/cards/active")return jsonCards();
    if(r.path=="/api/cards/controller"&&r.method=="GET")return jsonControllerCards();
    if(r.path=="/api/cards/controller/settings"&&r.method=="GET"){return{200,"application/json",std::string("{\"auto_create_unknown\":")+(cfg_.getBool("cards.auto_create_unknown",false)?"true":"false")+"}"};}
    if(r.path=="/api/attendance/today"&&r.method=="GET")return jsonTodayAttendance();
    if(r.path=="/api/reports/settings"&&r.method=="GET")return jsonReportSettings();
    if(r.path=="/api/reports/settings"&&r.method=="POST"){
        auto f=util::parseForm(r.body);
        int weekday=1,month_day=1;try{weekday=std::stoi(f["weekday"]);}catch(...){}try{month_day=std::stoi(f["month_day"]);}catch(...){}
        std::string err;bool ok=reports_.saveSchedule(f["enabled"]=="1",f["period"],f["time"],weekday,month_day,err);
        return{ok?200:400,"application/json",ok?"{\"ok\":true}":("{\"ok\":false,\"error\":\""+util::jsonEscape(err)+"\"}")};
    }
    if(r.path=="/api/reports/extended/preview"&&r.method=="GET"){
        auto q=util::parseForm(r.query);AttendanceReport report;std::string err;auto ids=parseIntList(q["users"]);
        if(!reports_.buildExtended(q["from"],q["to"],ids,report,err))return{400,"application/json","{\"ok\":false,\"error\":\""+util::jsonEscape(err)+"\"}"};
        std::ostringstream o;o<<"{\"ok\":true,\"filename\":\""<<util::jsonEscape(report.filename)<<"\",\"days\":"<<report.days<<",\"users\":"<<report.users<<",\"rows\":"<<report.rows<<",\"content\":\""<<util::jsonEscape(report.content)<<"\"}";
        return{200,"application/json; charset=utf-8",o.str()};
    }
    if(r.path=="/api/reports/extended/download"&&r.method=="GET"){
        auto q=util::parseForm(r.query);AttendanceReport report;std::string err;auto ids=parseIntList(q["users"]);
        if(!reports_.buildExtended(q["from"],q["to"],ids,report,err))return{400,"text/plain; charset=utf-8",err};
        Res x{200,"text/plain; charset=utf-8",report.content};x.headers.push_back({"Content-Disposition","attachment; filename=\""+report.filename+"\""});return x;
    }
    if(r.path=="/api/reports/extended/send"&&r.method=="POST"){
        auto f=util::parseForm(r.body);AttendanceReport report;std::string err;auto ids=parseIntList(f["users"]);bool ok=reports_.sendExtendedToTelegram(f["from"],f["to"],ids,report,err);
        std::ostringstream o;o<<"{\"ok\":"<<(ok?"true":"false")<<",\"filename\":\""<<util::jsonEscape(report.filename)<<"\",\"error\":\""<<util::jsonEscape(err)<<"\"}";
        return{ok?200:400,"application/json; charset=utf-8",o.str()};
    }
    if(r.path=="/api/reports/preview"&&r.method=="GET"){
        auto q=util::parseForm(r.query);AttendanceReport report;std::string err;
        if(!reports_.build(q["from"],q["to"],report,err))return{400,"application/json","{\"ok\":false,\"error\":\""+util::jsonEscape(err)+"\"}"};
        std::ostringstream o;o<<"{\"ok\":true,\"filename\":\""<<util::jsonEscape(report.filename)<<"\",\"days\":"<<report.days<<",\"users\":"<<report.users<<",\"rows\":"<<report.rows<<",\"content\":\""<<util::jsonEscape(report.content)<<"\"}";
        return{200,"application/json; charset=utf-8",o.str()};
    }
    if(r.path=="/api/reports/download"&&r.method=="GET"){
        auto q=util::parseForm(r.query);AttendanceReport report;std::string err;
        if(!reports_.build(q["from"],q["to"],report,err))return{400,"text/plain; charset=utf-8",err};
        Res x{200,"text/plain; charset=utf-8",report.content};x.headers.push_back({"Content-Disposition","attachment; filename=\""+report.filename+"\""});return x;
    }
    if(r.path=="/api/reports/send"&&r.method=="POST"){
        auto f=util::parseForm(r.body);AttendanceReport report;std::string err;bool ok=reports_.sendToTelegram(f["from"],f["to"],report,err);
        std::ostringstream o;o<<"{\"ok\":"<<(ok?"true":"false")<<",\"filename\":\""<<util::jsonEscape(report.filename)<<"\",\"error\":\""<<util::jsonEscape(err)<<"\"}";
        return{ok?200:400,"application/json; charset=utf-8",o.str()};
    }
    if(r.path=="/api/controllers"&&r.method=="GET")return jsonControllers();
    if(r.path=="/api/controllers/refresh"&&r.method=="POST"){controllers_.requestControllerRefresh();return{200,"application/json","{\"ok\":true}"};}
    if(r.path=="/api/controllers/read-attendance"&&r.method=="POST"){
        auto f=util::parseForm(r.body);int node=0;try{node=std::stoi(f["controller_node"]);}catch(...){return{400,"application/json","{\"ok\":false,\"error\":\"invalid node\"}"};}
        auto allc=controllers_.controllers();auto it=std::find_if(allc.begin(),allc.end(),[&](const Controller&c){return c.node==node&&c.enabled;});
        if(it==allc.end())return{400,"application/json","{\"ok\":false,\"error\":\"controller not found or disabled\"}"};
        auto id=controllers_.queueAttendanceRead(node);std::ostringstream o;o<<"{\"ok\":true,\"job_id\":"<<id<<",\"controller_node\":"<<node<<"}";return{200,"application/json; charset=utf-8",o.str()};
    }
    if(r.path=="/api/controllers/read-attendance/status"&&r.method=="GET"){
        auto q=util::parseForm(r.query);std::uint64_t id=0;try{id=std::stoull(q["job_id"]);}catch(...){}
        auto job=controllers_.attendanceReadJob(id);if(!job)return{404,"application/json","{\"error\":\"attendance read job not found\"}"};return jsonAttendanceReadJob(*job);
    }
    if(r.path=="/api/controllers/set-node-id"&&r.method=="POST"){
        auto f=util::parseForm(r.body);int node=0,new_node=0;try{node=std::stoi(f["controller_node"]);new_node=std::stoi(f["new_controller_node"]);}catch(...){return{400,"application/json","{\"ok\":false,\"error\":\"invalid node id\"}"};}
        if(node<1||node>254||new_node<1||new_node>254)return{400,"application/json","{\"ok\":false,\"error\":\"Node ID must be 1..254\"}"};
        auto allc=controllers_.controllers();auto it=std::find_if(allc.begin(),allc.end(),[&](const Controller&c){return c.node==node&&c.enabled;});
        if(it==allc.end())return{400,"application/json","{\"ok\":false,\"error\":\"controller not found or disabled\"}"};
        if(std::any_of(allc.begin(),allc.end(),[&](const Controller&c){return c.node==new_node&&c.node!=node;}))return{409,"application/json","{\"ok\":false,\"error\":\"new Node ID already exists\"}"};
        auto id=controllers_.queueSetNodeId(node,new_node);std::ostringstream o;o<<"{\"ok\":true,\"job_id\":"<<id<<",\"controller_node\":"<<node<<",\"new_controller_node\":"<<new_node<<"}";return{200,"application/json; charset=utf-8",o.str()};
    }
    if(r.path=="/api/controllers/set-node-id/status"&&r.method=="GET"){auto q=util::parseForm(r.query);std::uint64_t id=0;try{id=std::stoull(q["job_id"]);}catch(...){}auto job=controllers_.controllerActionJob(id);if(!job)return{404,"application/json","{\"error\":\"controller action job not found\"}"};return jsonControllerActionJob(*job);}
    if(r.path=="/api/protocol/live"&&r.method=="GET"){auto q=util::parseForm(r.query);std::uint64_t after=0;std::size_t limit=250;try{if(!q["after"].empty())after=std::stoull(q["after"]);if(!q["limit"].empty())limit=static_cast<std::size_t>(std::stoul(q["limit"]));}catch(...){}return jsonProtocolTrace(after,limit);}
    if(r.path=="/api/protocol/live/clear"&&r.method=="POST"){controllers_.clearProtocolTrace();return{200,"application/json","{\"ok\":true}"};}
    if(r.path=="/api/controllers/eeprom-search"&&r.method=="POST"){
        auto f=util::parseForm(r.body);int series=-1,number=-1,start=0,end=0xFFFF,block=64;
        try{series=std::stoi(f["card_series"]);number=std::stoi(f["card_number"]);start=std::stoi(f["start_address"]);end=std::stoi(f["end_address"]);if(!f["block_size"].empty())block=std::stoi(f["block_size"]);}catch(...){return{400,"application/json","{\"ok\":false,\"error\":\"bad numeric parameters\"}"};}
        if(series<0||series>65535||number<0||number>65535)return{400,"application/json","{\"ok\":false,\"error\":\"card series/number must be 0..65535\"}"};
        if(start<0||end>0xFFFF||start>end)return{400,"application/json","{\"ok\":false,\"error\":\"EEPROM range must be 0000..FFFF\"}"};
        if(block<16||block>128)return{400,"application/json","{\"ok\":false,\"error\":\"block size must be 16..128\"}"};
        std::vector<int> selected_nodes;auto allc=controllers_.controllers();
        if(f["all_controllers"]=="1"){for(const auto&c:allc)if(c.enabled)selected_nodes.push_back(c.node);}else{for(int node:parseIntList(f["controller_nodes"])){auto it=std::find_if(allc.begin(),allc.end(),[&](const Controller&c){return c.node==node&&c.enabled;});if(it!=allc.end())selected_nodes.push_back(node);}}
        if(selected_nodes.empty())return{400,"application/json","{\"ok\":false,\"error\":\"no controllers selected\"}"};
        auto compact_addresses=parseIntList(f["compact_addresses"]);
        for(int a:compact_addresses)if(a<1||a>16383)return{400,"application/json","{\"ok\":false,\"error\":\"compact user addresses must be 1..16383\"}"};
        auto id=controllers_.queueEepromSearch(series,number,std::move(selected_nodes),start,end,block,std::move(compact_addresses));
        std::ostringstream o;o<<"{\"ok\":true,\"job_id\":"<<id<<"}";return{200,"application/json",o.str()};
    }
    if(r.path=="/api/controllers/eeprom-search/status"&&r.method=="GET"){
        auto q=util::parseForm(r.query);std::uint64_t id=0;try{id=std::stoull(q["job_id"]);}catch(...){}
        auto job=controllers_.eepromSearchJob(id);if(!job)return{404,"application/json","{\"error\":\"EEPROM search job not found\"}"};return jsonEepromSearchJob(*job);
    }
    if(r.path=="/api/controllers/read-users"&&r.method=="POST"){
        auto f=util::parseForm(r.body);
        std::vector<int> selected_nodes;
        auto allc=controllers_.controllers();
        if(f["all_controllers"]=="1"){
            for(const auto&c:allc)if(c.enabled)selected_nodes.push_back(c.node);
        }else{
            for(int node:parseIntList(f["controller_nodes"])){
                auto it=std::find_if(allc.begin(),allc.end(),[&](const Controller&c){return c.node==node&&c.enabled;});
                if(it!=allc.end())selected_nodes.push_back(node);
            }
        }
        if(selected_nodes.empty())return{400,"application/json","{\"ok\":false,\"error\":\"no controllers selected\"}"};

        auto local_users=users_.list();
        std::vector<int> addresses;
        const auto mode=f["address_mode"].empty()?"local":f["address_mode"];
        if(mode=="local"){
            for(const auto&u:local_users)if(u.controller_port>=0&&u.controller_port<=1023)addresses.push_back(u.controller_port);
        }else if(mode=="range"){
            int from=0,to=0;try{from=std::stoi(f["range_from"]);to=std::stoi(f["range_to"]);}catch(...){}
            if(from<0||to>1023||from>to)return{400,"application/json","{\"ok\":false,\"error\":\"range must be 0..1023 for AR-721H/727H\"}"};
            addresses.reserve(static_cast<std::size_t>(to-from+1));
            for(int a=from;a<=to;++a)addresses.push_back(a);
        }else{
            return{400,"application/json","{\"ok\":false,\"error\":\"bad address mode\"}"};
        }
        std::sort(addresses.begin(),addresses.end());
        addresses.erase(std::unique(addresses.begin(),addresses.end()),addresses.end());
        if(addresses.empty())return{400,"application/json","{\"ok\":false,\"error\":\"no user addresses to read\"}"};

        const bool include_empty=f["include_empty"]=="1";
        const auto count=addresses.size()*selected_nodes.size();
        auto id=controllers_.queueUserRead(std::move(local_users),std::move(selected_nodes),std::move(addresses),include_empty);
        std::ostringstream o;o<<"{\"ok\":true,\"job_id\":"<<id<<",\"total\":"<<count<<"}";
        return{200,"application/json",o.str()};
    }
    if(r.path=="/api/controllers/read-users/status"&&r.method=="GET"){
        auto q=util::parseForm(r.query);std::uint64_t id=0;try{id=std::stoull(q["job_id"]);}catch(...){}
        auto job=controllers_.userReadJob(id);
        if(!job)return{404,"application/json","{\"error\":\"read job not found\"}"};
        return jsonUserReadJob(*job);
    }

    if(r.path=="/api/controllers/invalidate-user-slot"&&r.method=="POST"){
        auto f=util::parseForm(r.body);
        int node=0,address=0;
        try{node=std::stoi(f["controller_node"]);address=std::stoi(f["address"]);}catch(...){}
        if(address<0||address>1023)return{400,"application/json","{\"ok\":false,\"error\":\"address must be 0..1023\"}"};
        auto allc=controllers_.controllers();
        auto it=std::find_if(allc.begin(),allc.end(),[&](const Controller&c){return c.node==node&&c.enabled;});
        if(it==allc.end())return{400,"application/json","{\"ok\":false,\"error\":\"controller not found or disabled\"}"};
        User slot;slot.id=address;slot.controller_port=address;slot.enabled=false;
        slot.last_name="Controller slot";slot.first_name=std::to_string(address);
        auto id=controllers_.queueUserDelete({slot},{node},false);
        std::ostringstream o;o<<"{\"ok\":true,\"job_id\":"<<id<<",\"address\":"<<address<<",\"controller_node\":"<<node<<"}";
        return{200,"application/json",o.str()};
    }

    if(r.path=="/api/controllers/disable-pass-any"&&r.method=="POST"){
        auto f=util::parseForm(r.body);int node=0;try{node=std::stoi(f["controller_node"]);}catch(...){}
        auto allc=controllers_.controllers();
        auto it=std::find_if(allc.begin(),allc.end(),[&](const Controller&c){return c.node==node&&c.enabled;});
        if(it==allc.end())return{400,"application/json","{\"ok\":false,\"error\":\"controller not found or disabled\"}"};
        auto id=controllers_.queueDisablePassAnyCards(node);
        std::ostringstream o;o<<"{\"ok\":true,\"job_id\":"<<id<<",\"controller_node\":"<<node<<"}";
        return{200,"application/json; charset=utf-8",o.str()};
    }
    if(r.path=="/api/controllers/disable-pass-any/status"&&r.method=="GET"){
        auto q=util::parseForm(r.query);std::uint64_t id=0;try{id=std::stoull(q["job_id"]);}catch(...){}
        auto job=controllers_.controllerActionJob(id);if(!job)return{404,"application/json","{\"error\":\"controller action job not found\"}"};
        return jsonControllerActionJob(*job);
    }

    if(r.path=="/api/controllers/upload-users"&&r.method=="POST"){
        auto f=util::parseForm(r.body);std::vector<User> selected_users;std::vector<int> selected_nodes;
        const auto all_users=f["all_users"]=="1";const auto all_controllers=f["all_controllers"]=="1";
        auto allu=users_.list();
        if(all_users){
            // Full synchronization rebuilds the controller from active local users only.
            // Disabled local users intentionally stay absent after verified 83H reconciliation.
            for(const auto&u:allu)if(u.enabled)selected_users.push_back(u);

            // Refuse full synchronization unless the local dataset is internally
            // safe to rebuild: every active user must
            // have a valid H-series address/card and addresses must be unique.
            std::map<int,int> address_owner;
            for(const auto&u:selected_users){
                const std::string card=!u.card.empty()?u.card:(!u.cards.empty()?u.cards.front():std::string{});
                if(u.controller_port<0||u.controller_port>1023){
                    return{400,"application/json","{\"ok\":false,\"error\":\"full sync blocked: active user "+std::to_string(u.id)+" has controller address outside 0..1023\"}"};
                }
                if(card.empty()){
                    return{400,"application/json","{\"ok\":false,\"error\":\"full sync blocked: active user "+std::to_string(u.id)+" has no card\"}"};
                }
                auto [it,inserted]=address_owner.emplace(u.controller_port,u.id);
                if(!inserted){
                    return{400,"application/json","{\"ok\":false,\"error\":\"full sync blocked: duplicate controller address "+std::to_string(u.controller_port)+" for users "+std::to_string(it->second)+" and "+std::to_string(u.id)+"\"}"};
                }
            }
        }else{auto ids=parseIntList(f["user_ids"]);for(int id:ids){auto u=users_.byId(id);if(u)selected_users.push_back(*u);}}
        auto allc=controllers_.controllers();
        if(all_controllers){for(const auto&c:allc)if(c.enabled)selected_nodes.push_back(c.node);}else{auto nodes=parseIntList(f["controller_nodes"]);for(int node:nodes){auto it=std::find_if(allc.begin(),allc.end(),[&](const Controller&c){return c.node==node&&c.enabled;});if(it!=allc.end())selected_nodes.push_back(node);}}
        if(selected_users.empty())return{400,"application/json","{\"ok\":false,\"error\":\"no users selected\"}"};
        if(selected_nodes.empty())return{400,"application/json","{\"ok\":false,\"error\":\"no controllers selected\"}"};
        auto id=controllers_.queueUserUpload(std::move(selected_users),std::move(selected_nodes),all_users);
        std::ostringstream o;o<<"{\"ok\":true,\"job_id\":"<<id<<",\"protocol_ready\":"<<(controllers_.userUploadProtocolReady()?"true":"false")<<",\"protocol_message\":\""<<util::jsonEscape(controllers_.userUploadProtocolMessage())<<"\"}";
        return{200,"application/json",o.str()};
    }
    if(r.path=="/api/controllers/upload-users/status"&&r.method=="GET"){
        auto q=util::parseForm(r.query);std::uint64_t id=0;try{id=std::stoull(q["job_id"]);}catch(...){}
        auto job=controllers_.userUploadJob(id);if(!job)return{404,"application/json","{\"error\":\"upload job not found\"}"};return jsonUserUploadJob(*job);
    }
    if(r.path=="/api/users/delete-selected"&&r.method=="POST"){
        auto f=util::parseForm(r.body);
        const bool delete_system=f["delete_system"]=="1";
        const bool delete_controllers=f["delete_controllers"]=="1";
        if(!delete_system&&!delete_controllers)
            return{400,"application/json","{\"ok\":false,\"error\":\"delete target not selected\"}"};

        std::vector<User> selected_users;
        const auto all_users=f["all_users"]=="1";
        if(all_users)selected_users=users_.list();
        else{
            for(int id:parseIntList(f["user_ids"])){
                auto u=users_.byId(id);
                if(u)selected_users.push_back(*u);
            }
        }
        if(selected_users.empty())
            return{400,"application/json","{\"ok\":false,\"error\":\"no users selected\"}"};

        if(!delete_controllers){
            std::vector<int> ids;ids.reserve(selected_users.size());
            for(const auto&u:selected_users)ids.push_back(u.id);
            const int removed=users_.eraseMany(ids);
            if(removed>0)attendance_.refreshUserMetadata();
            std::ostringstream o;o<<"{\"ok\":true,\"immediate\":true,\"local_deleted\":"<<removed<<"}";
            return{200,"application/json",o.str()};
        }

        std::vector<int> selected_nodes;
        const auto all_controllers=f["all_controllers"]=="1";
        auto allc=controllers_.controllers();
        if(all_controllers){
            for(const auto&c:allc)if(c.enabled)selected_nodes.push_back(c.node);
        }else{
            for(int node:parseIntList(f["controller_nodes"])){
                auto it=std::find_if(allc.begin(),allc.end(),[&](const Controller&c){return c.node==node&&c.enabled;});
                if(it!=allc.end())selected_nodes.push_back(node);
            }
        }
        if(selected_nodes.empty())
            return{400,"application/json","{\"ok\":false,\"error\":\"no controllers selected\"}"};

        auto id=controllers_.queueUserDelete(std::move(selected_users),std::move(selected_nodes),delete_system);
        std::ostringstream o;
        o<<"{\"ok\":true,\"immediate\":false,\"job_id\":"<<id
         <<",\"delete_from_system\":"<<(delete_system?"true":"false")<<"}";
        return{200,"application/json",o.str()};
    }
    if(r.path=="/api/users/delete-selected/status"&&r.method=="GET"){
        auto q=util::parseForm(r.query);std::uint64_t id=0;try{id=std::stoull(q["job_id"]);}catch(...){}
        auto job=controllers_.userDeleteJob(id);
        if(!job)return{404,"application/json","{\"error\":\"delete job not found\"}"};
        return jsonUserDeleteJob(*job);
    }
    if(r.path=="/api/users/save"&&r.method=="POST"){
        auto f=util::parseForm(r.body);User u;try{u.id=std::stoi(f["id"]);}catch(...){}
        u.enabled=f["enabled"]!="0";u.last_name=f["last_name"];u.first_name=f["first_name"];u.middle_name=f["middle_name"];u.department=f["department"];u.position=f["position"];
        u.pin_code=util::trim(f["pin_code"]);u.access_mode=f["access_mode"].empty()?"card":f["access_mode"];
        std::string validation_error;
        if(!f["cards"].empty()){
            if(!parseCardList(f["cards"],u.cards,validation_error))return{400,"application/json","{\"ok\":false,\"error\":\""+util::jsonEscape(validation_error)+"\"}"};
        }else{
            // Backward-compatible single-card form/API.
            u.card_series=util::trim(f["card_series"]);u.card_number=util::trim(f["card_number"]);
            std::uint16_t series=0,number=0;const bool any_card=!u.card_series.empty()||!u.card_number.empty();
            if(any_card){
                if(!util::parseCardParts(u.card_series,u.card_number,series,number,&validation_error))return{400,"application/json","{\"ok\":false,\"error\":\""+util::jsonEscape(validation_error)+"\"}"};
                u.card_series=util::formatCardSeries(series);u.card_number=std::to_string(number);u.card=util::formatCardId(series,number);u.cards.push_back(u.card);
            }
        }
        if(!u.pin_code.empty()){
            bool digits=u.pin_code.size()==4&&std::all_of(u.pin_code.begin(),u.pin_code.end(),[](unsigned char c){return std::isdigit(c);});
            int pv=0;try{pv=std::stoi(u.pin_code);}catch(...){}
            if(!digits||pv<1||pv>9999)return{400,"application/json","{\"ok\":false,\"error\":\"PIN должен состоять из 4 цифр и быть в диапазоне 0001..9999\"}"};
        }
        if(u.access_mode!="card"&&u.access_mode!="card_or_pin"&&u.access_mode!="card_and_pin")u.access_mode="card";
        if((u.access_mode=="card_or_pin"||u.access_mode=="card_and_pin")&&u.pin_code.empty())return{400,"application/json","{\"ok\":false,\"error\":\"Для режима с PIN необходимо указать PIN пользователя\"}"};
        try{u.controller_port=std::stoi(f["controller_port"]);}catch(...){u.controller_port=0;}if(u.controller_port<0)u.controller_port=0;if(u.controller_port>1023)u.controller_port=1023;
        if(u.id>0){auto old=users_.byId(u.id);if(old){u.valid_from=old->valid_from;u.valid_until=old->valid_until;u.telegram_arrival=old->telegram_arrival;u.telegram_departure=old->telegram_departure;}}
        auto saved=users_.upsert(u);if(!saved.department.empty())departments_.add(saved.department);attendance_.refreshUserMetadata();return{200,"application/json","{\"ok\":true,\"id\":"+std::to_string(saved.id)+"}"};
    }
    if(r.path=="/api/users/delete"&&r.method=="POST"){auto f=util::parseForm(r.body);bool ok=false;try{ok=users_.erase(std::stoi(f["id"]));}catch(...){}attendance_.refreshUserMetadata();return{200,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}"};}
    if(r.path=="/api/departments/save"&&r.method=="POST"){
        auto f=util::parseForm(r.body);
        auto old_name=util::trim(f["old_name"]),name=util::trim(f["name"]);
        if(name.empty())return{400,"application/json","{\"ok\":false,\"error\":\"empty department\"}"};
        bool ok=false;
        if(old_name.empty())ok=departments_.add(name);
        else if(old_name==name)ok=true;
        else{
            ok=departments_.rename(old_name,name);
            if(ok){users_.renameDepartment(old_name,name);attendance_.refreshUserMetadata();}
        }
        return{200,"application/json",ok?"{\"ok\":true}":"{\"ok\":false,\"error\":\"department already exists or was not found\"}"};
    }
    if(r.path=="/api/departments/delete"&&r.method=="POST"){
        auto f=util::parseForm(r.body);auto name=util::trim(f["name"]);
        if(users_.departmentInUse(name))return{200,"application/json","{\"ok\":false,\"error\":\"department is used by users\"}"};
        bool ok=departments_.erase(name);
        return{200,"application/json",ok?"{\"ok\":true}":"{\"ok\":false,\"error\":\"department not found\"}"};
    }
    if(r.path=="/api/cards/controller/settings"&&r.method=="POST"){
        auto f=util::parseForm(r.body);cfg_.set("cards.auto_create_unknown",f["auto_create_unknown"]=="1"?"true":"false");const bool ok=cfg_.save();return{ok?200:400,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}"};
    }
    if(r.path=="/api/cards/controller/clear"&&r.method=="POST"){controllers_.clearControllerCards();return{200,"application/json","{\"ok\":true}"};}
    if(r.path=="/api/cards/create-user"&&r.method=="POST"){
        auto f=util::parseForm(r.body);const auto card=util::trim(f["card"]);auto existing=users_.byCard(card);auto u=users_.ensureUserForCard(card);if(!u)return{400,"application/json","{\"ok\":false,\"error\":\"Некорректная карта\"}"};attendance_.refreshUserMetadata();std::ostringstream o;o<<"{\"ok\":true,\"id\":"<<u->id<<",\"created\":"<<(existing?"false":"true")<<"}";return{200,"application/json",o.str()};
    }
    if(r.path=="/api/cards/create-users"&&r.method=="POST"){
        auto records=controllers_.controllerCards();std::vector<std::string> seen;std::vector<int> ids;int created=0,already=0,failed=0;
        for(const auto&rec:records){if(std::find(seen.begin(),seen.end(),rec.card)!=seen.end())continue;seen.push_back(rec.card);if(auto old=users_.byCard(rec.card)){++already;continue;}auto u=users_.ensureUserForCard(rec.card);if(u){++created;ids.push_back(u->id);}else ++failed;}
        if(created>0)attendance_.refreshUserMetadata();std::ostringstream o;o<<"{\"ok\":true,\"created\":"<<created<<",\"already_linked\":"<<already<<",\"failed\":"<<failed<<",\"ids\":[";for(std::size_t i=0;i<ids.size();++i){if(i)o<<',';o<<ids[i];}o<<"]}";return{200,"application/json",o.str()};
    }
    if(r.path=="/api/cards/assign"&&r.method=="POST"){auto f=util::parseForm(r.body);bool ok=false;try{ok=users_.assignCard(std::stoi(f["user_id"]),f["card"]);}catch(...){}if(ok)attendance_.refreshUserMetadata();return{ok?200:400,"application/json",ok?"{\"ok\":true}":"{\"ok\":false,\"error\":\"Не удалось добавить карту пользователю\"}"};}
    if(r.path=="/api/cards/delete"&&r.method=="POST"){auto f=util::parseForm(r.body);bool ok=users_.removeCard(f["card"]);attendance_.refreshUserMetadata();return{200,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}"};}
    if(r.path=="/api/controllers/name"&&r.method=="POST"){auto f=util::parseForm(r.body);bool ok=false;try{ok=controllers_.renameController(std::stoi(f["node"]),f["name"]);}catch(...){}return{200,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}"};}
    if(r.path=="/api/telegram/test"&&r.method=="POST"){std::string err;bool ok=telegram_.sendTest(err);return{200,"application/json",ok?"{\"ok\":true}":("{\"ok\":false,\"error\":\""+util::jsonEscape(err)+"\"}")};}
    if(r.path=="/api/export/users"){Res x{200,"text/csv; charset=utf-8",users_.exportCsv()};x.headers.push_back({"Content-Disposition","attachment; filename=users.csv"});return x;}
    if(r.path=="/api/import/soyal/preview"&&r.method=="POST"){
        auto q=util::parseForm(r.query);const auto type=q["type"];SoyalImportResult parsed;
        if(type=="usr")parsed=SoyalImport::parseUsr(r.body);
        else if(type=="txt")parsed=SoyalImport::parseUserCardText(r.body);
        else if(!r.body.empty()&&r.body.size()%328==0)parsed=SoyalImport::parseUsr(r.body);
        else parsed=SoyalImport::parseUserCardText(r.body);
        if(!parsed.ok)return{400,"application/json; charset=utf-8","{\"ok\":false,\"error\":\""+util::jsonEscape(parsed.error)+"\"}"};
        std::ostringstream o;o<<"{\"ok\":true,\"format\":\""<<util::jsonEscape(parsed.format)<<"\",\"total_slots\":"<<parsed.total_slots<<",\"empty_slots\":"<<parsed.empty_slots<<",\"records\":[";bool first=true;
        for(const auto&x:parsed.records){
            if(!first)o<<',';first=false;std::optional<User> owner;if(!x.card.empty())owner=users_.byCard(x.card);auto byaddr=uniqueUserByControllerPort(users_,x.address);
            std::string status=owner?"linked_card":(byaddr?"candidate_address":"new");auto candidate=owner?owner:byaddr;
            o<<"{\"address\":"<<x.address<<",\"card\":\""<<util::jsonEscape(x.card)<<"\",\"card_series\":"<<x.card_series<<",\"card_number\":"<<x.card_number
             <<",\"pin_code\":\""<<util::jsonEscape(x.pin_code)<<"\",\"full_name\":\""<<util::jsonEscape(x.full_name)<<"\",\"last_name\":\""<<util::jsonEscape(x.last_name)<<"\",\"first_name\":\""<<util::jsonEscape(x.first_name)<<"\",\"middle_name\":\""<<util::jsonEscape(x.middle_name)<<"\",\"department\":\""<<util::jsonEscape(x.department)<<"\",\"position\":\""<<util::jsonEscape(x.position)<<"\",\"source\":\""<<util::jsonEscape(x.source)<<"\",\"status\":\""<<status<<"\",\"local_user_id\":"<<(candidate?candidate->id:0)<<",\"local_user_name\":\""<<util::jsonEscape(candidate?localUserName(*candidate):std::string{})<<"\"}";
        }
        o<<"]}";return{200,"application/json; charset=utf-8",o.str()};
    }
    if(r.path=="/api/import/soyal/apply-record"&&r.method=="POST"){
        auto f=util::parseForm(r.body);auto rec=soyalRecordFromForm(f);const auto action=f["action"];
        if(rec.card.empty())return{400,"application/json; charset=utf-8","{\"ok\":false,\"error\":\"В записи SOYAL нет карты\"}"};
        std::uint16_t cs=0,cn=0;std::string card_error;if(!util::parseCardId(rec.card,cs,cn,&card_error))return{400,"application/json; charset=utf-8","{\"ok\":false,\"error\":\""+util::jsonEscape(card_error)+"\"}"};
        std::optional<User> target;std::string result_action;
        if(action=="assign"){
            int id=0;try{id=std::stoi(f["user_id"]);}catch(...){}if(id<=0||!users_.assignCard(id,rec.card))return{400,"application/json; charset=utf-8","{\"ok\":false,\"error\":\"Не удалось привязать карту\"}"};
            target=users_.byId(id);result_action="assigned_manual";
        }else if(action=="create"){
            if(auto owner=users_.byCard(rec.card)){target=owner;result_action="already_linked";}
            else{User u;u.enabled=true;u.last_name=rec.last_name;u.first_name=rec.first_name;u.middle_name=rec.middle_name;u.department=rec.department;u.position=rec.position;u.pin_code=rec.pin_code;u.controller_port=(rec.address>=0&&rec.address<=16383)?rec.address:0;u.cards.push_back(rec.card);target=users_.upsert(std::move(u));result_action="created";}
        }else if(action=="auto"){
            if(auto owner=users_.byCard(rec.card)){target=owner;result_action="matched_card";}
            else if(auto byaddr=uniqueUserByControllerPort(users_,rec.address)){if(!users_.assignCard(byaddr->id,rec.card))return{400,"application/json; charset=utf-8","{\"ok\":false,\"error\":\"Не удалось привязать карту по адресу\"}"};target=users_.byId(byaddr->id);result_action="assigned_address";}
            else return{200,"application/json; charset=utf-8","{\"ok\":true,\"action\":\"no_match\",\"user_id\":0}"};
        }else return{400,"application/json; charset=utf-8","{\"ok\":false,\"error\":\"Неизвестное действие импорта\"}"};
        if(target){mergeSoyalFields(*target,rec);target=users_.upsert(*target);if(!target->department.empty())departments_.add(target->department);}
        attendance_.refreshUserMetadata();std::ostringstream o;o<<"{\"ok\":true,\"action\":\""<<result_action<<"\",\"user_id\":"<<(target?target->id:0)<<",\"user_name\":\""<<util::jsonEscape(target?localUserName(*target):std::string{})<<"\"}";return{200,"application/json; charset=utf-8",o.str()};
    }
    if(r.path=="/api/import/users"&&r.method=="POST"){std::string err;bool ok=users_.importCsv(r.body,err);if(ok)departments_.ensure(users_.usedDepartments());attendance_.refreshUserMetadata();return{200,"application/json",ok?"{\"ok\":true}":("{\"ok\":false,\"error\":\""+util::jsonEscape(err)+"\"}")};}
    if(r.path=="/api/export/settings"){Res x{200,"text/plain; charset=utf-8",cfg_.raw()};x.headers.push_back({"Content-Disposition","attachment; filename=system.conf"});return x;}
    if(r.path=="/api/import/settings"&&r.method=="POST"){bool ok=cfg_.replaceRaw(r.body);return{200,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}"};}
    if(r.path=="/api/settings/reset-site-activity"&&r.method=="POST"){
        const bool ok=attendance_.resetSiteActivity();
        if(!ok)return{500,"application/json; charset=utf-8","{\"ok\":false,\"error\":\"Не удалось очистить текущее состояние активности в хранилище\"}"};
        return{200,"application/json; charset=utf-8","{\"ok\":true,\"present_count\":0}"};
    }
    if(r.path=="/api/settings"&&r.method=="GET")return jsonSettings();
    if(r.path=="/api/settings/save"&&r.method=="POST"){auto f=util::parseForm(r.body);if(!f["username"].empty())cfg_.set("auth.username",f["username"]);if(!f["password"].empty()){auto salt=util::randomToken(16);cfg_.set("auth.salt",salt);cfg_.set("auth.password_hash",util::sha256Hex(salt+f["password"]));}if(!f["port"].empty())cfg_.set("server.port",f["port"]);if(!f["serial_device"].empty())cfg_.set("serial.device",f["serial_device"]);cfg_.set("telegram.enabled",f["telegram_enabled"]=="1"?"true":"false");cfg_.set("telegram.bot_token",f["bot_token"]);cfg_.set("telegram.chat_id",f["chat_id"]);cfg_.set("telegram.notify_arrival",f["notify_arrival"]=="1"?"true":"false");cfg_.set("telegram.notify_departure",f["notify_departure"]=="1"?"true":"false");cfg_.set("telegram.report_text_copy",f["report_text_copy"]=="1"?"true":"false");cfg_.set("reports.archive.enabled",f["report_archive_enabled"]=="1"?"true":"false");if(!f["report_archive_path"].empty())cfg_.set("reports.archive.path",f["report_archive_path"]);if(!cfg_.save())return{500,"application/json","{\"ok\":false,\"error\":\"save failed\"}"};return{200,"application/json","{\"ok\":true,\"restart_required\":true}"};}
    return{404,"application/json","{\"error\":\"not found\"}"};
}
}
