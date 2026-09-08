#include "skud/App.h"
#include "skud/AttendanceEngine.h"
#include "skud/Config.h"
#include "skud/ControllerManager.h"
#include "skud/DepartmentManager.h"
#include "skud/FileStore.h"
#include "skud/RuntimeBootstrap.h"
#include "skud/ReportManager.h"
#include "skud/TelegramNotifier.h"
#include "skud/UserManager.h"
#include "skud/Util.h"
#include "skud/WebServer.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace skud {
App::App(std::string root):root_(std::move(root)){}
App::~App(){stop();}
bool App::init(){
    std::string bootstrap_error;
    if(!ensureRuntimeLayout(root_,bootstrap_error)){
        std::cerr<<"Cannot initialize runtime directory "<<root_<<": "<<bootstrap_error<<"\n";
        return false;
    }
    cfg_=std::make_unique<Config>(root_+"/config/system.conf");
    if(!cfg_->load()){
        cfg_->set("server.port","8080");cfg_->set("serial.enabled","true");cfg_->set("serial.device","auto");cfg_->set("serial.baudrate","9600");cfg_->set("controllers.scan_from","1");cfg_->set("controllers.scan_to","16");cfg_->set("controllers.poll_interval_ms","200");cfg_->set("cards.auto_create_unknown","false");cfg_->set("attendance.accidental_repeat_seconds","5");cfg_->set("time_sync.enabled","true");cfg_->set("time_sync.interval_minutes","60");cfg_->set("time_sync.retry_seconds","60");cfg_->set("telegram.enabled","false");cfg_->set("telegram.bot_token","");cfg_->set("telegram.chat_id","");cfg_->set("telegram.notify_arrival","true");cfg_->set("telegram.notify_departure","true");cfg_->set("telegram.notify_recent_minutes","10");cfg_->set("telegram.report_text_copy","true");cfg_->set("telegram.retry_count","3");cfg_->set("reports.schedule.enabled","false");cfg_->set("reports.schedule.period","daily");cfg_->set("reports.schedule.time","18:00");cfg_->set("reports.schedule.weekday","1");cfg_->set("reports.schedule.month_day","1");cfg_->set("database.enabled","false");cfg_->set("database.host","127.0.0.1");cfg_->set("database.port","3306");cfg_->set("database.name","monk_skud_unex");cfg_->set("database.user","monk_skud");cfg_->set("database.password","");cfg_->set("database.auto_create","true");cfg_->set("database.migrate_users_csv","true");cfg_->set("database.migrate_runtime_csv","true");cfg_->set("database.remove_csv_after_migration","true");cfg_->set("auth.username","admin");auto salt=util::randomToken(16);cfg_->set("auth.salt",salt);cfg_->set("auth.password_hash",util::sha256Hex(salt+"admin"));cfg_->save();
    }
    if(cfg_->get("auth.salt").empty()||cfg_->get("auth.password_hash").empty()){auto salt=util::randomToken(16);cfg_->set("auth.salt",salt);cfg_->set("auth.password_hash",util::sha256Hex(salt+"admin"));cfg_->save();}
    // v0.3.19 one-time attendance policy migration: apply the requested 5-second
    // duplicate-card window to existing installations without forcing it again
    // after an administrator changes the value later.
    if(cfg_->get("attendance.policy_revision")!="0319"){
        cfg_->set("attendance.accidental_repeat_seconds","5");
        cfg_->set("attendance.policy_revision","0319");
        cfg_->save();
    }
    store_=std::make_unique<FileStore>(root_,cfg_.get());
    {std::string db_error;if(!store_->init(db_error)){std::cerr<<"Cannot initialize MariaDB runtime storage: "<<db_error<<"\n";return false;}}
    users_=std::make_unique<UserManager>(root_+"/config/users.csv",cfg_.get());
    if(!users_->load()){
        if(cfg_->getBool("database.enabled",false)){std::cerr<<"Cannot initialize MariaDB user storage: "<<users_->storageError()<<"\n";return false;}
        users_->save();
    }
    departments_=std::make_unique<DepartmentManager>(root_+"/config/departments.csv",cfg_.get());
    if(!departments_->load()){if(cfg_->getBool("database.enabled",false)){std::cerr<<"Cannot initialize MariaDB department storage: "<<departments_->storageError()<<"\n";return false;}departments_->save();}
    departments_->ensure(users_->usedDepartments());
    attendance_=std::make_unique<AttendanceEngine>(*users_,*store_,cfg_->getInt("attendance.accidental_repeat_seconds",5));
    telegram_=std::make_unique<TelegramNotifier>(*cfg_);attendance_->setNotifier([this](const AttendanceEvent&e){telegram_->enqueue(e);});
    reports_=std::make_unique<ReportManager>(*cfg_,*store_,*users_,*attendance_,*telegram_,root_);
    controllers_=std::make_unique<ControllerManager>(*cfg_,*attendance_,*users_,root_+"/config/controllers.csv");if(!controllers_->loadControllers()){if(cfg_->getBool("database.enabled",false)){std::cerr<<"Cannot initialize MariaDB controller storage\n";return false;}controllers_->saveControllers();}
    web_=std::make_unique<WebServer>(*cfg_,*users_,*departments_,*attendance_,*controllers_,*telegram_,*reports_,root_);
    return true;
}
bool App::run(){if(!cfg_&&!init())return false;attendance_->processMidnightRollover();telegram_->start();reports_->start();controllers_->start();if(!web_->start()){std::cerr<<"Cannot start web server on port "<<cfg_->getInt("server.port",8080)<<"\n";controllers_->stop();reports_->stop();telegram_->stop();return false;}running_=true;std::cout<<"Monk_SKUD_UNEX started. Web: http://0.0.0.0:"<<cfg_->getInt("server.port",8080)<<" default login admin/admin\n";while(running_){attendance_->processMidnightRollover();std::this_thread::sleep_for(std::chrono::seconds(1));}return true;}
void App::stop(){if(!running_&& !web_)return;running_=false;if(web_)web_->stop();if(reports_)reports_->stop();if(controllers_)controllers_->stop();if(telegram_)telegram_->stop();}
}
