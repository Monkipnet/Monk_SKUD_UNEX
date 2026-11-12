
// Guaranteed visual feedback for every current and future button/link styled as a button.
function showButtonPress(target){
    const b=target&&target.closest?target.closest('button,.button'):null;
    if(!b||b.disabled||b.classList.contains('disabled'))return;
    clearTimeout(b._skudPressTimer);
    b.classList.remove('skud-pressed');
    void b.offsetWidth;
    b.classList.add('skud-pressed');
    b._skudPressTimer=setTimeout(()=>b.classList.remove('skud-pressed'),240);
}
document.addEventListener('pointerdown',e=>showButtonPress(e.target),true);
document.addEventListener('keydown',e=>{if(e.key==='Enter'||e.key===' ')showButtonPress(e.target)},true);

let USERS=[];
let CONTROLLERS=[];
let DEPARTMENTS=[];
let USERS_LOADED=false;
let REPORT_RANGES={};
let PROTOCOL_ENTRIES=[];
let PROTOCOL_LAST_ID=0;
let PROTOCOL_RUNNING=false;
let PROTOCOL_TIMER=null;
let SOYAL_RECORDS=[];
let SOYAL_IMPORT_META=null;
let SOYAL_ASSIGN_RECORD=null;
let SETTINGS_SECTION='general';

function enc(o){return new URLSearchParams(o)}
async function api(url,opt={}){let r=await fetch(url,opt);if(r.status===401){location='/login.html';throw new Error('auth');}return r}
function tab(id){document.querySelectorAll('.tab').forEach(x=>x.classList.add('hidden'));document.getElementById(id).classList.remove('hidden');if(id==='dashboard')loadTodayAttendance();if(id==='cards')loadCards();if(id==='users')loadUsers();if(id==='departments'){loadUsers().then(()=>loadDepartments());}if(id==='reports')loadReportSettings();if(id==='settings')settingsTab(SETTINGS_SECTION||'general');else stopProtocolLivePolling();}

function settingsTab(id){
    SETTINGS_SECTION=id||'general';
    document.querySelectorAll('#settings .settings-panel').forEach(x=>x.classList.add('hidden'));
    document.querySelectorAll('#settings .settings-subnav button').forEach(x=>x.classList.toggle('active',x.dataset.settingsTab===SETTINGS_SECTION));
    const panel=document.getElementById('settings-'+SETTINGS_SECTION);if(panel)panel.classList.remove('hidden');
    if(SETTINGS_SECTION==='controllers'){stopProtocolLivePolling();loadControllers();}
    else if(SETTINGS_SECTION==='live'){startProtocolLive();}
    else{stopProtocolLivePolling();loadSettings();}
}

function formatUptime(total){total=Math.max(0,Math.floor(Number(total)||0));const d=Math.floor(total/86400);total%=86400;const h=Math.floor(total/3600);total%=3600;const m=Math.floor(total/60);const sec=total%60;const clock=[h,m,sec].map(x=>String(x).padStart(2,'0')).join(':');return d>0?d+'д '+clock:clock;}
async function refreshStatus(){let r=await api('/api/status');let s=await r.json();if(window.presentCount)presentCount.textContent=s.present_count||0;if(window.registeredCount)registeredCount.textContent=s.registered_count||0;if(window.cpuLoad)cpuLoad.textContent=Number(s.cpu_percent||0).toFixed(1)+'%';if(window.ramLoad)ramLoad.textContent=Number(s.ram_percent||0).toFixed(1)+'%';if(window.ramDetail)ramDetail.textContent=(s.ram_used_mb||0)+' / '+(s.ram_total_mb||0)+' MB';if(window.uptimeValue)uptimeValue.textContent=formatUptime(s.uptime_seconds);}
function refreshHeaderClock(){
    const now=new Date();
    if(window.headerClockTime)headerClockTime.textContent=new Intl.DateTimeFormat('ru-RU',{hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false}).format(now);
    if(window.headerClockDate){const weekday=new Intl.DateTimeFormat('ru-RU',{weekday:'long'}).format(now);const date=new Intl.DateTimeFormat('ru-RU',{day:'2-digit',month:'2-digit',year:'numeric'}).format(now);headerClockDate.textContent=date+' · '+weekday;}
}
function timeOnly(value){if(!value)return '—';const s=String(value);return s.length>=19?s.slice(11,19):s;}
async function loadTodayAttendance(){
    let a=await (await api('/api/attendance/today')).json();
    if(!window.todayAttendanceBody)return;
    if(!a.length){todayAttendanceBody.innerHTML='<tr><td colspan="7" class="muted">Нет событий за сегодня</td></tr>';return;}
    todayAttendanceBody.innerHTML=a.map(x=>`<tr><td><b>${esc(x.user_name||('Пользователь №'+x.user_id))}</b></td><td>${esc(x.position||'—')}</td><td>${esc(x.department||'—')}</td><td>${esc(cardTextFromRaw(x.card||''))}</td><td>${timeOnly(x.arrival_time)}</td><td>${timeOnly(x.departure_time)}</td><td><span class="status-pill ${x.status==='at_work'?'status-present':'status-left'}">${x.status==='at_work'?'На работе':'Ушёл'}</span></td></tr>`).join('');
}

function userCardIds(u){
    if(u&&Array.isArray(u.cards)&&u.cards.length)return [...new Set(u.cards.map(x=>String(x||'').trim()).filter(Boolean))];
    return u&&u.card?[String(u.card)]:[];
}
function cardDisplay(u){const cards=userCardIds(u).map(cardTextFromRaw);return cards.length?cards.join(', '):'—';}
function userCardsHtml(u){const cards=userCardIds(u);if(!cards.length)return '—';return `<div class="user-cards-cell">${cards.map(c=>`<span class="user-card-chip">${esc(cardTextFromRaw(c))}</span>`).join('')}</div><small class="table-subtext">${cards.length} карт(а)</small>`;}
function cardTextFromRaw(card){const s=String(card||'').trim();let m=s.match(/^(\d+):(\d+)$/);if(m)return Number(m[1])+' / '+Number(m[2]);return s||'—';}
function accessModeText(mode){return ({card:'Только карта',card_or_pin:'Карта ИЛИ PIN',card_and_pin:'Карта + PIN'})[mode]||'Только карта';}
async function loadUsers(){USERS=await (await api('/api/users')).json();USERS_LOADED=true;usersBody.innerHTML=USERS.map(u=>`<tr><td>${u.id}</td><td>${esc(u.last_name+' '+u.first_name+' '+u.middle_name)}</td><td>${esc(u.department||'—')}</td><td>${esc(u.position)}</td><td>${userCardsHtml(u)}</td><td>${u.pin_code?'<span class="status-pill status-present">PIN задан</span>':'—'}<small class="table-subtext">${esc(accessModeText(u.access_mode))}</small></td><td>${Number.isInteger(Number(u.controller_port))?u.controller_port:'—'}</td><td><button class="mini" onclick="editUser(${u.id})">Изменить</button> <button class="mini danger" onclick="deleteUser(${u.id})">Удалить</button></td></tr>`).join('');}

function departmentOptions(selected=''){
    let items=[...DEPARTMENTS];
    if(selected&&!items.includes(selected))items.push(selected);
    items.sort((a,b)=>a.localeCompare(b,'ru'));
    return `<option value="">Без отдела</option>`+items.map(name=>`<option value="${attr(name)}"${name===selected?' selected':''}>${esc(name)}</option>`).join('');
}

async function loadDepartments(render=true){
    DEPARTMENTS=await (await api('/api/departments')).json();
    if(userDepartment)userDepartment.innerHTML=departmentOptions(userDepartment.value||'');
    if(!render||!document.getElementById('departmentsBody'))return;
    if(!USERS_LOADED){USERS=await (await api('/api/users')).json();USERS_LOADED=true;}
    departmentsBody.innerHTML=DEPARTMENTS.map((name,i)=>{
        const count=USERS.filter(u=>u.department===name).length;
        return `<tr><td><input id="department-${i}" value="${attr(name)}" maxlength="120"></td><td>${count}</td><td><button class="mini" onclick="saveDepartment('${js(name)}',${i})">Сохранить</button> <button class="mini danger" onclick="deleteDepartment('${js(name)}')">Удалить</button></td></tr>`;
    }).join('');
}

async function addDepartment(){
    const name=newDepartmentName.value.trim();
    if(!name)return;
    let r=await api('/api/departments/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({old_name:'',name})});
    let j=await r.json();
    if(!j.ok){alert('Ошибка: '+(j.error||'не удалось добавить отдел'));return;}
    newDepartmentName.value='';
    await loadDepartments();
}

async function saveDepartment(oldName,index){
    const name=document.getElementById('department-'+index).value.trim();
    if(!name)return alert('Название отдела не может быть пустым');
    let r=await api('/api/departments/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({old_name:oldName,name})});
    let j=await r.json();
    if(!j.ok){alert('Ошибка: '+(j.error||'не удалось сохранить отдел'));return;}
    await loadUsers();
    await loadDepartments();
    await loadCards();
}

async function deleteDepartment(name){
    if(!confirm('Удалить отдел «'+name+'»?'))return;
    let r=await api('/api/departments/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({name})});
    let j=await r.json();
    if(!j.ok){alert(j.error==='department is used by users'?'Нельзя удалить отдел: он назначен одному или нескольким пользователям. Сначала измените отдел у этих пользователей.':'Ошибка: '+(j.error||'не удалось удалить отдел'));return;}
    await loadDepartments();
}

async function loadCards(){
    await loadUsers();
    const [cardsResp,settingsResp]=await Promise.all([api('/api/cards/controller'),api('/api/cards/controller/settings')]);
    const a=await cardsResp.json(),settings=await settingsResp.json();
    if(window.cardAutoCreate)cardAutoCreate.checked=!!settings.auto_create_unknown;
    const unique=[...new Set(a.map(x=>x.card))],linked=[...new Set(a.filter(x=>x.linked).map(x=>x.card))],unlinked=unique.filter(c=>!linked.includes(c));
    if(window.cardsSummary)cardsSummary.textContent=`Уникальных карт: ${unique.length} · автоматически/вручную привязано: ${linked.length} · не привязано: ${unlinked.length}`;
    cardsBody.innerHTML=a.length?a.map(x=>{
        const status=x.linked?'<span class="status-pill status-present">Привязана</span><small class="table-subtext">совпадение по series:number</small>':'<span class="status-pill status-warn">Новая карта</span>';
        const user=x.linked?`<b>${esc(x.user_name||('Пользователь №'+x.user_id))}</b><small class="table-subtext">${esc(x.department||'Без отдела')}</small>`:'<span class="muted">Не привязана</span>';
        const actions=x.linked
            ?`<button class="mini" onclick="editUser(${x.user_id})">Открыть пользователя</button> <button class="mini danger" onclick="removeCard('${js(x.card)}')">Отвязать</button>`
            :`<button class="mini" onclick="openAssign('${js(x.card)}')">Привязать</button> <button class="mini" onclick="createCardUser('${js(x.card)}')">Создать пользователя</button>`;
        return `<tr><td><b>${esc(cardTextFromRaw(x.card))}</b><small class="read-card-badge">реальный 25H · RAW подтверждён</small></td><td>${x.controller_node||'—'}<small class="table-subtext">${esc(x.controller_name||'')}</small></td><td>${status}</td><td>${user}</td><td>${esc(x.first_seen||'—')}</td><td>${esc(x.last_seen||'—')}</td><td>${Number(x.read_count||0)}</td><td>${actions}</td></tr>`;
    }).join(''):'<tr><td colspan="8" class="muted">Каталог пуст. Приложите карты к контроллеру: реальные 25H-события будут добавляться сюда автоматически.</td></tr>';
}
async function saveCardImportSettings(){
    const auto=window.cardAutoCreate&&cardAutoCreate.checked?'1':'0';
    const r=await api('/api/cards/controller/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({auto_create_unknown:auto})});const j=await r.json();if(!r.ok||!j.ok)return alert('Не удалось сохранить режим');
    alert(auto==='1'?'Автосоздание включено. Новая неизвестная карта сразу станет отдельным пользователем.':'Автосоздание выключено. Новые карты будут ждать ручной привязки.');
}
async function createCardUser(card){
    const r=await api('/api/cards/create-user',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({card})});const j=await r.json();if(!r.ok||!j.ok)return alert('Ошибка: '+(j.error||'не удалось создать пользователя'));
    await loadUsers();await loadCards();editUser(j.id);
}
async function createAllCardUsers(){
    if(!confirm('Создать отдельного пользователя для каждой непривязанной карты из каталога? Имена-заглушки можно будет отредактировать позже.'))return;
    const r=await api('/api/cards/create-users',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:''});const j=await r.json();if(!r.ok||!j.ok)return alert('Не удалось создать пользователей');
    await loadUsers();await loadCards();alert(`Создано: ${j.created}; уже привязано: ${j.already_linked}; ошибок: ${j.failed}`);
}
async function clearControllerCardCatalog(){
    if(!confirm('Очистить только каталог считанных карт? Пользователи, их карты и журнал посещаемости останутся без изменений.'))return;
    const r=await api('/api/cards/controller/clear',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:''});const j=await r.json();if(!r.ok||!j.ok)return alert('Не удалось очистить каталог');await loadCards();
}
async function loadControllers(){
    CONTROLLERS=await (await api('/api/controllers')).json();
    if(window.controllersBody)controllersBody.innerHTML=CONTROLLERS.length?CONTROLLERS.map(c=>{
        const reported=Number(c.reported_node)>0?Number(c.reported_node):null;
        const idClass=reported===null?'muted':(reported===Number(c.node)?'ok':'bad');
        const idText=reported===null?'—':String(reported);
        return `<tr><td><b>${c.node}</b></td><td class="${idClass}"><b>${idText}</b><small class="table-subtext">${esc(c.id_status||'ID ещё не считан')}</small></td><td><input value="${attr(c.name)}" onchange="renameController(${c.node},this.value)"></td><td>${esc(c.model)}</td><td class="${c.online?'ok':'bad'}">${c.online?'ONLINE':'OFFLINE'}</td><td>${esc(c.last_seen||'')}</td><td><code>${esc(c.last_raw_hex||'')}</code></td><td><button class="mini primary" onclick="readControllerAttendance(${c.node})">Вычитать посещаемость</button><div class="controller-id-action"><input id="controllerNewNode-${c.node}" type="number" min="1" max="254" value="${c.node}" title="Новый Node ID"><button class="mini" onclick="setControllerNodeId(${c.node})">Изменить ID</button></div><button class="mini" onclick="disablePassAnyCards(${c.node})">Отключить пропуск любой карты</button></td></tr>`;
    }).join(''):'<tr><td colspan="8" class="muted">Контроллеры ещё не обнаружены</td></tr>';
    return CONTROLLERS;
}
async function refreshControllers(){
    const b=window.refreshControllersButton,m=window.controllersRefreshStatus;
    if(b)b.disabled=true;if(m)m.textContent='24H: чтение ID контроллеров...';
    try{
        await api('/api/controllers/refresh',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:''});
        await new Promise(resolve=>setTimeout(resolve,Math.max(700,450+(CONTROLLERS.length||1)*250)));
        await loadControllers();
        if(m)m.textContent='Обновлено с контроллеров: '+new Date().toLocaleTimeString('ru-RU',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
    }catch(e){if(m)m.textContent='Ошибка обновления';}
    finally{if(b)b.disabled=false;}
}

async function readControllerAttendance(node){
    const c=CONTROLLERS.find(x=>Number(x.node)===Number(node));const name=c?controllerDisplayName(c):('UNEX 721 #'+node);
    if(!confirm(`Контроллер ${node} — ${name}: вычитать всю накопленную посещаемость из FIFO?\n\nКаждая запись 25H будет сохранена с исходной датой/временем контроллера. Только после успешного сохранения будет отправлен 37H Delete Event.`))return;
    if(window.controllerActionResult){controllerActionResult.className='upload-summary muted';controllerActionResult.textContent=`Node ${node}: вычитывание FIFO посещаемости...`;}
    const r=await api('/api/controllers/read-attendance',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({controller_node:node})});
    let j={};try{j=await r.json();}catch{}
    if(!r.ok||!j.ok){if(window.controllerActionResult){controllerActionResult.className='upload-summary bad';controllerActionResult.textContent='Ошибка: '+(j.error||'не удалось запустить вычитывание');}return;}
    for(let i=0;i<3600;i++){
        await new Promise(resolve=>setTimeout(resolve,500));
        const sr=await api('/api/controllers/read-attendance/status?job_id='+encodeURIComponent(j.job_id));let st={};try{st=await sr.json();}catch{}
        if(!sr.ok)continue;
        const range=(st.first_event_time||st.last_event_time)?` · период ${st.first_event_time||'—'} — ${st.last_event_time||'—'}`:'';
        if(st.state==='completed'){
            if(window.controllerActionResult){controllerActionResult.className='upload-summary '+(st.ok?'ok':'bad');controllerActionResult.innerHTML=`<b>Node ${node}: ${st.ok?'вычитывание завершено':'ошибка'}</b><br>Событий: ${Number(st.read||0)} · сохранено: ${Number(st.stored||0)} · проходов: ${Number(st.access_events||0)} · прочих: ${Number(st.raw_events||0)} · дубликатов: ${Number(st.duplicates||0)}${esc(range)}<br>${esc(st.message||st.status||'')}`;}
            await loadControllers();return;
        }
        if(window.controllerActionResult)controllerActionResult.textContent=`Node ${node}: читаю FIFO... событий ${Number(st.read||0)}, сохранено ${Number(st.stored||0)}, проходов ${Number(st.access_events||0)}`;
    }
    if(window.controllerActionResult){controllerActionResult.className='upload-summary bad';controllerActionResult.textContent=`Node ${node}: тайм-аут ожидания. Проверьте LIVE протокол.`;}
}

async function setControllerNodeId(node){
    const input=document.getElementById('controllerNewNode-'+node);const newNode=Number(input&&input.value);
    if(!Number.isInteger(newNode)||newNode<1||newNode>254)return alert('Новый Node ID должен быть от 1 до 254');
    if(newNode===Number(node))return alert('Новый Node ID совпадает с текущим');
    const c=CONTROLLERS.find(x=>Number(x.node)===Number(node));const name=c?controllerDisplayName(c):('UNEX 721 #'+node);
    if(!confirm(`Контроллер ${node} — ${name}: изменить физический Node ID на ${newNode}?\n\nБудет выполнено: 24H проверка текущего ID → 80H Set Node ID → ACK с новым Reader ID → 24H проверка нового ID.\n\nНе отключайте питание во время операции.`))return;
    if(window.controllerActionResult){controllerActionResult.className='upload-summary muted';controllerActionResult.textContent=`Node ${node}: проверка ID и изменение на ${newNode}...`;}
    const r=await api('/api/controllers/set-node-id',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({controller_node:node,new_controller_node:newNode})});
    let j={};try{j=await r.json();}catch{}
    if(!r.ok||!j.ok){if(window.controllerActionResult){controllerActionResult.className='upload-summary bad';controllerActionResult.textContent='Ошибка: '+(j.error||'не удалось запустить изменение ID');}return;}
    for(let i=0;i<120;i++){
        await new Promise(resolve=>setTimeout(resolve,250));
        const sr=await api('/api/controllers/set-node-id/status?job_id='+encodeURIComponent(j.job_id));let st={};try{st=await sr.json();}catch{}
        if(!sr.ok)continue;
        if(st.state==='completed'){
            if(window.controllerActionResult){controllerActionResult.className='upload-summary '+(st.ok?'ok':'bad');controllerActionResult.innerHTML=`<b>Node ${node} → ${newNode}: ${st.ok?'готово':'ошибка'}</b><br>${esc(st.message||st.status||'')}`;}
            await loadControllers();return;
        }
        if(window.controllerActionResult)controllerActionResult.textContent=`Node ${node} → ${newNode}: ${st.state==='running'?'24H → 80H → ACK → 24H...':'в очереди COM-порта...'}`;
    }
    if(window.controllerActionResult){controllerActionResult.className='upload-summary bad';controllerActionResult.textContent=`Node ${node}: тайм-аут изменения ID. Проверьте LIVE протокол.`;}
}

async function disablePassAnyCards(node){
    const c=CONTROLLERS.find(x=>Number(x.node)===Number(node));const name=c?controllerDisplayName(c):('UNEX 721 #'+node);
    if(!confirm(`Контроллер ${node} — ${name}: отключить режим «пропуск любой карты»?\n\nБудет изменён только EEPROM 0x0016 bit 0x20. Пользовательская база не затрагивается.`))return;
    if(window.controllerActionResult){controllerActionResult.className='upload-summary muted';controllerActionResult.textContent=`Node ${node}: чтение EEPROM 0x0016...`;}
    const r=await api('/api/controllers/disable-pass-any',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({controller_node:node})});
    let j={};try{j=await r.json();}catch{}
    if(!r.ok||!j.ok){if(window.controllerActionResult){controllerActionResult.className='upload-summary bad';controllerActionResult.textContent='Ошибка: '+(j.error||'не удалось запустить операцию');}return;}
    const jobId=j.job_id;
    for(let i=0;i<120;i++){
        await new Promise(resolve=>setTimeout(resolve,250));
        const sr=await api('/api/controllers/disable-pass-any/status?job_id='+encodeURIComponent(jobId));let st={};try{st=await sr.json();}catch{}
        if(!sr.ok)continue;
        if(st.state==='completed'){
            if(window.controllerActionResult){controllerActionResult.className='upload-summary '+(st.ok?'ok':'bad');controllerActionResult.innerHTML=`<b>Node ${st.controller_node}: ${st.ok?'готово':'ошибка'}</b><br>${esc(st.message||st.status||'')}`;}
            return;
        }
        if(window.controllerActionResult)controllerActionResult.textContent=`Node ${node}: ${st.state==='running'?'12H → 20H → 12H, выполняется...':'в очереди COM-порта...'}`;
    }
    if(window.controllerActionResult){controllerActionResult.className='upload-summary bad';controllerActionResult.textContent=`Node ${node}: тайм-аут ожидания результата. Проверьте Live протокол.`;}
}

function userDisplayName(u){return [u.last_name,u.first_name,u.middle_name].filter(Boolean).join(' ')||('Пользователь №'+u.id);}
function controllerDisplayName(c){return c.name||('Контроллер '+c.node);}


function hex4(v){return Number(v).toString(16).toUpperCase().padStart(4,'0');}
function parseHex16(v){const s=String(v||'').trim();if(!/^[0-9a-fA-F]{1,4}$/.test(s))return NaN;return parseInt(s,16);}
async function openEepromSearch(series='',number=''){
    await loadControllers();
    eepromAllControllers.checked=true;
    eepromCardSeries.value=series||'';eepromCardNumber.value=number||'';eepromCompactAddresses.value='5,7';eepromStart.value='0000';eepromEnd.value='FFFF';eepromBlockSize.value='64';
    eepromControllersList.innerHTML=CONTROLLERS.length?CONTROLLERS.map(c=>`<label class="check selection-item"><input type="checkbox" class="eeprom-controller" value="${c.node}" checked> <span><b>${c.node} — ${esc(controllerDisplayName(c))}</b><small>${c.online?'ONLINE':'OFFLINE'} · ${esc(c.model||'UNEX 721')}</small></span></label>`).join(''):'<div class="muted">Контроллеры ещё не обнаружены</div>';
    eepromSearchResult.innerHTML='';toggleEepromControllers();updateEepromSearchSummary();eepromSearchDialog.showModal();
}
function toggleEepromControllers(){document.querySelectorAll('.eeprom-controller').forEach(x=>{x.disabled=eepromAllControllers.checked;if(eepromAllControllers.checked)x.checked=true;});updateEepromSearchSummary();}
function selectedEepromControllers(){return [...document.querySelectorAll('.eeprom-controller:checked')].map(x=>Number(x.value)).filter(Boolean);}
function updateEepromSearchSummary(){
    if(!window.eepromSearchSummary)return;const from=parseHex16(eepromStart.value),to=parseHex16(eepromEnd.value),block=Number(eepromBlockSize.value)||64;
    const controllers=eepromAllControllers.checked?CONTROLLERS.length:selectedEepromControllers().length;const bytes=Number.isFinite(from)&&Number.isFinite(to)&&to>=from?to-from+1:0;const blocks=bytes?Math.ceil(bytes/block):0;
    const compact=String(eepromCompactAddresses.value||'').trim();eepromSearchSummary.innerHTML=`Compact 87H: <b>${esc(compact||'не задан')}</b> · Диапазон: <b>${bytes?hex4(from)+'..'+hex4(to):'ошибка'}</b> · ${bytes} байт · <b>${blocks}</b> блок(ов) × <b>${controllers}</b> контроллер(ов) = <b>${blocks*controllers}</b> операций 12H.`;
}
document.addEventListener('input',e=>{if(e.target.matches&&e.target.matches('#eepromCardSeries,#eepromCardNumber,#eepromCompactAddresses,#eepromStart,#eepromEnd'))updateEepromSearchSummary();});
document.addEventListener('change',e=>{if(e.target.matches&&e.target.matches('.eeprom-controller,#eepromBlockSize'))updateEepromSearchSummary();});
function renderEepromSearchJob(job){
    const state=({queued:'В очереди',running:'Чтение EEPROM',completed:'Завершено'})[job.state]||job.state;
    const rows=(job.matches||[]).map(m=>{const c=CONTROLLERS.find(x=>x.node===m.controller_node);return `<tr><td>${m.controller_node} — ${esc(c?controllerDisplayName(c):'')}</td><td><code>0x${hex4(m.eeprom_address)}</code></td><td>${m.exact?'<span class="status-pill status-present">точное</span>':'<span class="status-pill">частичное</span>'}<small class="table-subtext">${esc(m.pattern)}</small></td><td><code>${esc(m.matched_hex)}</code></td><td><code class="eeprom-context">${esc(m.context_hex)}</code></td></tr>`;}).join('');
    const errs=(job.errors||[]).map(e=>`<div class="bad">Node ${e.controller_node}, EEPROM 0x${hex4(e.eeprom_address)}: ${esc(e.message)}</div>`).join('');
    const trunc=job.truncated?'<div class="protocol-warning">Результатов больше 500; список ограничен. Сузьте диапазон EEPROM.</div>':'';
    const probes=(job.compact_probes||[]).map(x=>`<div><code>${esc(x)}</code></div>`).join('');
    const probeBox=probes?`<div class="protocol-warning"><b>Compact 87H probes:</b>${probes}</div>`:'';
    eepromSearchResult.innerHTML=`<div class="upload-job-state"><b>${esc(state)}</b> · ${job.completed}/${job.total} блоков · совпадений ${(job.matches||[]).length} · ошибок ${job.failed}</div>${probeBox}${trunc}${rows?`<div class="upload-result-table eeprom-result-table"><table><thead><tr><th>Контроллер</th><th>EEPROM</th><th>Тип</th><th>Совпало</th><th>Контекст RAW</th></tr></thead><tbody>${rows}</tbody></table></div>`:(job.state==='completed'?'<div class="protocol-warning">Прямых совпадений известных представлений карты не найдено. Это тоже диагностический результат: карта может храниться в преобразованном формате или в другой области памяти.</div>':'')}${errs?`<div class="eeprom-errors">${errs}</div>`:''}`;
}
async function pollEepromSearch(jobId){for(let n=0;n<10000;n++){const r=await api('/api/controllers/eeprom-search/status?job_id='+encodeURIComponent(jobId));if(!r.ok)return;const job=await r.json();renderEepromSearchJob(job);if(job.state!=='queued'&&job.state!=='running')return;await new Promise(resolve=>setTimeout(resolve,700));}}
async function startEepromSearch(){
    const series=Number(eepromCardSeries.value),number=Number(eepromCardNumber.value),compact=String(eepromCompactAddresses.value||'').trim(),from=parseHex16(eepromStart.value),to=parseHex16(eepromEnd.value),block=Number(eepromBlockSize.value)||64,nodes=selectedEepromControllers();
    if(!Number.isInteger(series)||series<0||series>65535)return alert('Серия карты должна быть 0..65535');if(compact&&!/^\s*\d+(?:\s*,\s*\d+)*\s*$/.test(compact))return alert('Адреса compact 87H задайте числами, например 5,7');if(!Number.isInteger(number)||number<0||number>65535)return alert('Номер карты должен быть 0..65535');if(!Number.isFinite(from)||!Number.isFinite(to)||from<0||to>0xFFFF||from>to)return alert('Диапазон EEPROM должен быть HEX 0000..FFFF');
    if(!eepromAllControllers.checked&&!nodes.length)return alert('Выберите контроллер');if(eepromAllControllers.checked&&!CONTROLLERS.length)return alert('Нет обнаруженных контроллеров');
    const controllerCount=eepromAllControllers.checked?CONTROLLERS.length:nodes.length,ops=Math.ceil((to-from+1)/block)*controllerCount;if(ops>500&&!confirm(`Полное чтение потребует ${ops} операций 12H и может занять несколько минут. Продолжить?`))return;
    startEepromSearchButton.disabled=true;eepromSearchResult.innerHTML='<div class="muted">Создание задания безопасного чтения EEPROM...</div>';
    try{const r=await api('/api/controllers/eeprom-search',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({card_series:String(series),card_number:String(number),compact_addresses:compact,all_controllers:eepromAllControllers.checked?'1':'0',controller_nodes:nodes.join(','),start_address:String(from),end_address:String(to),block_size:String(block)})});const j=await r.json();if(!r.ok||!j.ok){eepromSearchResult.innerHTML='<div class="bad">Ошибка: '+esc(j.error||'не удалось начать поиск')+'</div>';return;}await pollEepromSearch(j.job_id);}finally{startEepromSearchButton.disabled=false;}
}

async function openUserRead(){
    if(!USERS_LOADED)await loadUsers();
    await loadControllers();
    readAllControllers.checked=true;
    readModeLocal.checked=true;
    readModeRange.checked=false;
    readIncludeEmpty.checked=false;
    readControllersList.innerHTML=CONTROLLERS.length?CONTROLLERS.map(c=>`<label class="check selection-item"><input type="checkbox" class="read-controller" value="${c.node}" checked> <span><b>${c.node} — ${esc(controllerDisplayName(c))}</b><small>${c.online?'ONLINE':'OFFLINE'} · ${esc(c.model||'UNEX 721')}</small></span></label>`).join(''):'<div class="muted">Контроллеры ещё не обнаружены</div>';
    readResult.innerHTML='';
    toggleReadControllers();
    updateReadMode();
    updateReadSummary();
    readUsersDialog.showModal();
}

function toggleReadControllers(){
    document.querySelectorAll('.read-controller').forEach(x=>{x.disabled=readAllControllers.checked;if(readAllControllers.checked)x.checked=true;});
    updateReadSummary();
}

function updateReadMode(){
    const range=readModeRange.checked;
    readRangeFields.classList.toggle('disabled-panel',!range);
    readRangeFrom.disabled=!range;
    readRangeTo.disabled=!range;
    updateReadSummary();
}

function selectedReadControllers(){return [...document.querySelectorAll('.read-controller:checked')].map(x=>Number(x.value)).filter(Boolean);}
function localReadAddresses(){return [...new Set(USERS.map(u=>Number(u.controller_port)||0).filter(x=>x>=0&&x<=1023))].sort((a,b)=>a-b);}

function updateReadSummary(){
    if(!window.readSelectionSummary)return;
    const controllerCount=readAllControllers.checked?CONTROLLERS.length:selectedReadControllers().length;
    let addressCount=0;
    let extra='';
    if(readModeLocal.checked){
        const a=localReadAddresses();addressCount=a.length;
        extra=a.length?`Адреса: ${a.join(', ')}`:'У пользователей не заданы адреса в контроллере.';
    }else{
        const from=Number(readRangeFrom.value)||0,to=Number(readRangeTo.value)||0;
        if(from>=0&&to>=from&&to<=1023)addressCount=to-from+1;
        extra=addressCount>1000?'Большой диапазон может читаться несколько минут и более.':'Диапазон читается последовательно командой 0x87.';
    }
    readSelectionSummary.innerHTML=`Будет прочитано: <b>${addressCount}</b> адрес(ов) × <b>${controllerCount}</b> контроллер(ов) = <b>${addressCount*controllerCount}</b> операций.<br><small>${esc(extra)}</small>`;
}
document.addEventListener('change',e=>{if(e.target.matches&&e.target.matches('.read-controller'))updateReadSummary();});

function readStatusText(status){return ({match:'Совпадает',diff:'Отличается',missing:'Нет в контроллере',unknown:'Нет в системе',unverified:'Не проверено',empty:'Пусто',error:'Ошибка'})[status]||status;}
function renderUserReadJob(job){
    const state=({queued:'В очереди',running:'Чтение',completed:'Завершено'})[job.state]||job.state;
    const rows=(job.results||[]).map(r=>{
        const c=CONTROLLERS.find(x=>x.node===r.controller_node);
        const cls=r.status==='match'?'ok':(r.status==='empty'||r.status==='unverified')?'muted':'bad';
        const detailLine=r.card_known===false
            ?`H/UNEX compact 8B · карта/PIN/режим не декодированы${r.raw_record_hex?' · RAW '+esc(r.raw_record_hex):''}`
            :r.details_known===false
            ?`PIN/режим не декодированы${r.raw_record_hex?' · RAW '+esc(r.raw_record_hex):''}`
            :`${r.controller_enabled?'Активен':'Отключен'} · ${r.pin_set?'PIN задан':'PIN нет'} · ${esc(accessModeText(r.access_mode))}`;
        const ctrlData=r.controller_card
            ?`${esc(cardTextFromRaw(r.controller_card))}<small class="table-subtext">${detailLine}</small>`
            :'—';
        const local=r.local_user_id?`${r.local_user_id} — ${esc(r.local_user_name||'')}`:'—';
        const action=(r.controller_enabled&&r.address>=0&&r.address<=1023)
            ?`<button class="mini danger" onclick="invalidateControllerSlot(${r.controller_node},${r.address})">Отключить слот</button>`
            :'—';
        return `<tr><td>${r.controller_node} — ${esc(c?controllerDisplayName(c):'')}</td><td>${r.address}</td><td>${ctrlData}</td><td>${local}</td><td class="${cls}"><b>${esc(readStatusText(r.status))}</b><small class="table-subtext">${esc(r.message||'')}</small></td><td>${action}</td></tr>`;
    }).join('');
    readResult.innerHTML=`<div class="upload-job-state"><b>${esc(state)}</b> · ${job.completed}/${job.total} · совпало ${job.matches} · отличается ${job.differences} · нет в контроллере ${job.missing} · неизвестных ${job.unknown} · не проверено ${job.unverified||0} · пустых ${job.empty} · ошибок ${job.failed}</div>${rows?`<div class="upload-result-table read-result-table"><table><thead><tr><th>Контроллер</th><th>Адрес</th><th>Данные контроллера</th><th>Пользователь в системе</th><th>Сравнение</th><th>Действие</th></tr></thead><tbody>${rows}</tbody></table></div>`:''}`;
}

async function invalidateControllerSlot(node,address){
    if(!confirm(`Отключить User Address ${address} на контроллере Node ${node}?\n\nБудет отправлен официальный 83H с Mode=0 (Invalid), остальные Site/Card/PIN/Zone сохранятся. После ACK выполняется обязательный 87H read-back.`))return;
    const r=await api('/api/controllers/invalidate-user-slot',{
        method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
        body:enc({controller_node:String(node),address:String(address)})
    });
    const j=await r.json();
    if(!r.ok||!j.ok)return alert('Ошибка: '+(j.error||'не удалось создать задание'));
    for(let n=0;n<180;n++){
        const sr=await api('/api/users/delete-selected/status?job_id='+encodeURIComponent(j.job_id));
        if(!sr.ok)break;
        const job=await sr.json();
        if(job.state!=='queued'&&job.state!=='running'){
            const x=(job.results||[])[0];
            alert(x?x.message:(job.success?'Слот отключён':'Операция завершилась с ошибкой'));
            return;
        }
        await new Promise(resolve=>setTimeout(resolve,500));
    }
    alert('Не получен финальный статус отключения слота');
}

async function pollUserRead(jobId){
    for(let n=0;n<7200;n++){
        const r=await api('/api/controllers/read-users/status?job_id='+encodeURIComponent(jobId));
        if(!r.ok)return;
        const job=await r.json();renderUserReadJob(job);
        if(job.state!=='queued'&&job.state!=='running')return;
        await new Promise(resolve=>setTimeout(resolve,700));
    }
}

async function startUserRead(){
    const nodes=selectedReadControllers();
    if(!readAllControllers.checked&&!nodes.length)return alert('Выберите хотя бы один контроллер');
    if(readAllControllers.checked&&!CONTROLLERS.length)return alert('Нет обнаруженных контроллеров');

    let addressMode='local',from='',to='',addressCount=localReadAddresses().length;
    if(readModeRange.checked){
        addressMode='range';from=Number(readRangeFrom.value)||0;to=Number(readRangeTo.value)||0;
        if(from<0||to>1023||from>to)return alert('Диапазон адресов AR-721H/727H должен быть в пределах 0..1023');
        addressCount=to-from+1;
    }else if(!addressCount)return alert('У пользователей в системе не задан ни один адрес в контроллере');

    const controllerCount=readAllControllers.checked?CONTROLLERS.length:nodes.length;
    if(addressCount*controllerCount>1000&&!confirm(`Будет выполнено ${addressCount*controllerCount} последовательных чтений 0x87. Это может занять длительное время. Продолжить?`))return;

    startReadButton.disabled=true;
    readResult.innerHTML='<div class="muted">Создание задания чтения...</div>';
    try{
        const r=await api('/api/controllers/read-users',{
            method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
            body:enc({
                all_controllers:readAllControllers.checked?'1':'0',
                controller_nodes:nodes.join(','),
                address_mode:addressMode,
                range_from:String(from),
                range_to:String(to),
                include_empty:readIncludeEmpty.checked?'1':'0'
            })
        });
        const j=await r.json();
        if(!r.ok||!j.ok){readResult.innerHTML='<div class="bad">Ошибка: '+esc(j.error||'не удалось создать задание')+'</div>';return;}
        await pollUserRead(j.job_id);
    }finally{startReadButton.disabled=false;}
}

async function openUserUpload(){
    if(!USERS_LOADED)await loadUsers();
    await loadControllers();
    uploadAllUsers.checked=false;uploadAllControllers.checked=true;
    uploadUsersList.innerHTML=USERS.length?USERS.map(u=>`<label class="check selection-item"><input type="checkbox" class="upload-user" value="${u.id}"> <span><b>${u.id} — ${esc(userDisplayName(u))}</b><small>${esc(u.department||'Без отдела')} · карта ${esc(cardDisplay(u))} · ${u.pin_code?'PIN задан · ':''}${esc(accessModeText(u.access_mode))} · адрес ${u.controller_port}</small></span></label>`).join(''):'<div class="muted">Пользователей нет</div>';
    uploadControllersList.innerHTML=CONTROLLERS.length?CONTROLLERS.map(c=>`<label class="check selection-item"><input type="checkbox" class="upload-controller" value="${c.node}" checked> <span><b>${c.node} — ${esc(controllerDisplayName(c))}</b><small>${c.online?'ONLINE':'OFFLINE'} · ${esc(c.model||'UNEX 721')}</small></span></label>`).join(''):'<div class="muted">Контроллеры ещё не обнаружены</div>';
    uploadResult.innerHTML='';
    toggleUploadSelection('users');toggleUploadSelection('controllers');updateUploadSummary();
    uploadDialog.showModal();
}

function toggleUploadSelection(kind){
    const all=kind==='users'?uploadAllUsers:uploadAllControllers;
    const selector=kind==='users'?'.upload-user':'.upload-controller';
    document.querySelectorAll(selector).forEach(x=>{x.disabled=all.checked;if(all.checked)x.checked=true;});
    updateUploadSummary();
}

function selectedUploadValues(selector){return [...document.querySelectorAll(selector+':checked')].map(x=>Number(x.value)).filter(Boolean);}
function updateUploadSummary(){
    if(!window.uploadSelectionSummary)return;
    const userCount=uploadAllUsers.checked?USERS.filter(u=>u.enabled!==false).length:selectedUploadValues('.upload-user').length;
    const controllerCount=uploadAllControllers.checked?CONTROLLERS.length:selectedUploadValues('.upload-controller').length;
    uploadSelectionSummary.textContent=uploadAllUsers.checked?`Полная синхронизация: отключение режима «любая карта», затем принудительная синхронизация всех 1024 адресов на ${controllerCount} контроллер(ах) и запись ${userCount} пользователей.`:`Будет подготовлено записей: ${userCount} × ${controllerCount} = ${userCount*controllerCount}`;
}
document.addEventListener('change',e=>{if(e.target.matches&&e.target.matches('.upload-user,.upload-controller'))updateUploadSummary();});

function uploadStatusText(status){return ({ok:'Записан и проверен',written_verified:'Записан и проверен',pass_any_disabled:'«Любая карта» отключена',pass_any_rewritten_disabled:'«Любая карта» подтверждённо выключена',pass_any_read_failed:'Ошибка чтения настройки',pass_any_write_failed:'Ошибка записи настройки',slot_cleared_verified:'Слот очищен и проверен',ok_unverified:'Записан — проверить картой',skipped:'Пропущен',blocked_protocol:'Заблокировано',error:'Ошибка'})[status]||status;}
function renderUserUploadJob(job){
    const state=({queued:'В очереди',running:'Выполняется',completed:'Завершено',blocked:'Аппаратная запись заблокирована'})[job.state]||job.state;
    const rows=(job.results||[]).map(r=>{const u=USERS.find(x=>x.id===r.user_id);const c=CONTROLLERS.find(x=>x.node===r.controller_node);const good=['ok','written_verified','pass_any_disabled','pass_any_rewritten_disabled','slot_cleared_verified','ok_unverified'].includes(r.status);const muted=['skipped','skipped_clear_failed'].includes(r.status);const cls=good?'ok':muted?'muted':'bad';const who=r.user_id===0?(String(r.status||'').startsWith('pass_any_')?'Настройка контроллера':'Очистка слота'):`${r.user_id} — ${esc(u?userDisplayName(u):'')}`;return `<tr><td>${who}</td><td>${r.controller_node} — ${esc(c?controllerDisplayName(c):'')}</td><td class="${cls}">${esc(uploadStatusText(r.status))}</td><td>${esc(r.message||'')}</td></tr>`;}).join('');
    uploadResult.innerHTML=`<div class="upload-job-state"><b>${esc(state)}</b> · ${job.completed}/${job.total} · успешно ${job.success} · пропущено ${job.skipped} · ошибок/блокировок ${job.failed}</div>${rows?`<div class="upload-result-table"><table><thead><tr><th>Пользователь</th><th>Контроллер</th><th>Результат</th><th>Комментарий</th></tr></thead><tbody>${rows}</tbody></table></div>`:''}`;
}

async function pollUserUpload(jobId){
    for(let n=0;n<900;n++){
        const r=await api('/api/controllers/upload-users/status?job_id='+encodeURIComponent(jobId));
        if(!r.ok)return;
        const job=await r.json();renderUserUploadJob(job);
        if(job.state!=='queued'&&job.state!=='running')return;
        await new Promise(resolve=>setTimeout(resolve,700));
    }
}

async function startUserUpload(){
    const userIds=selectedUploadValues('.upload-user');const nodes=selectedUploadValues('.upload-controller');
    if(!uploadAllUsers.checked&&!userIds.length)return alert('Выберите хотя бы одного пользователя');
    if(!uploadAllControllers.checked&&!nodes.length)return alert('Выберите хотя бы один контроллер');
    if(uploadAllUsers.checked&&!USERS.some(u=>u.enabled!==false))return alert('Нет активных пользователей для полной выгрузки');
    if(uploadAllControllers.checked&&!CONTROLLERS.length)return alert('Нет обнаруженных контроллеров');
    const fullSync=uploadAllUsers.checked;
    const warning=fullSync?'\n\nПОЛНАЯ СИНХРОНИЗАЦИЯ: сначала будет отключён глобальный режим «любая карта», затем каждый отсутствующий адрес 0..1023 будет принудительно записан нулями через 83H и проверен 87H. После этого будут записаны пользователи Monk SKUD. Операция может занять несколько минут.':'';
    if(!confirm(`Выгрузить ${fullSync?'всех':'выбранных'} пользователей в ${uploadAllControllers.checked?'все':'выбранные'} контроллеры?${warning}\n\nЗапись выполняется 83H, каждая запись проверяется контрольным 87H.`))return;
    startUploadButton.disabled=true;uploadResult.innerHTML='<div class="muted">Создание задания...</div>';
    try{
        const r=await api('/api/controllers/upload-users',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({all_users:uploadAllUsers.checked?'1':'0',user_ids:userIds.join(','),all_controllers:uploadAllControllers.checked?'1':'0',controller_nodes:nodes.join(',')})});
        const j=await r.json();
        if(!r.ok||!j.ok){uploadResult.innerHTML='<div class="bad">Ошибка: '+esc(j.error||'не удалось создать задание')+'</div>';return;}
        if(!j.protocol_ready)uploadResult.innerHTML='<div class="protocol-warning"><b>Аппаратная запись недоступна.</b><br>'+esc(j.protocol_message||'Протокол записи не готов')+'</div>';else uploadResult.innerHTML='<div class="upload-job-state"><b>UNEX H-series 83H / standard 0x7E активен.</b><br>'+esc(j.protocol_message||'')+'</div>';
        await pollUserUpload(j.job_id);
    }finally{startUploadButton.disabled=false;}
}

async function openUserDelete(preselectedId=0){
    if(!USERS_LOADED)await loadUsers();
    await loadControllers();
    deleteFromSystem.checked=true;
    deleteFromControllers.checked=true;
    deleteAllUsers.checked=false;
    deleteAllControllers.checked=true;
    deleteUsersList.innerHTML=USERS.length?USERS.map(u=>`<label class="check selection-item"><input type="checkbox" class="delete-user" value="${u.id}" ${preselectedId===u.id?'checked':''}> <span><b>${u.id} — ${esc(userDisplayName(u))}</b><small>${esc(u.department||'Без отдела')} · карта ${esc(cardDisplay(u))} · ${u.pin_code?'PIN задан · ':''}${esc(accessModeText(u.access_mode))} · адрес ${u.controller_port}</small></span></label>`).join(''):'<div class="muted">Пользователей нет</div>';
    deleteControllersList.innerHTML=CONTROLLERS.length?CONTROLLERS.map(c=>`<label class="check selection-item"><input type="checkbox" class="delete-controller" value="${c.node}" checked> <span><b>${c.node} — ${esc(controllerDisplayName(c))}</b><small>${c.online?'ONLINE':'OFFLINE'} · ${esc(c.model||'UNEX 721')}</small></span></label>`).join(''):'<div class="muted">Контроллеры ещё не обнаружены</div>';
    deleteResult.innerHTML='';
    toggleDeleteSelection('controllers');
    updateDeleteTargetUI();
    updateDeleteSummary();
    deleteUsersDialog.showModal();
}

function toggleDeleteSelection(kind){
    const all=kind==='users'?deleteAllUsers:deleteAllControllers;
    const selector=kind==='users'?'.delete-user':'.delete-controller';
    document.querySelectorAll(selector).forEach(x=>{x.disabled=all.checked;if(all.checked)x.checked=true;});
    updateDeleteSummary();
}

function updateDeleteTargetUI(){
    if(!window.deleteControllersPanel)return;
    const useControllers=deleteFromControllers.checked;
    deleteControllersPanel.classList.toggle('disabled-panel',!useControllers);
    deleteAllControllers.disabled=!useControllers;
    document.querySelectorAll('.delete-controller').forEach(x=>x.disabled=!useControllers||deleteAllControllers.checked);
    updateDeleteSummary();
}

function updateDeleteSummary(){
    if(!window.deleteSelectionSummary)return;
    const userCount=deleteAllUsers.checked?USERS.length:selectedUploadValues('.delete-user').length;
    const deleteSystem=deleteFromSystem.checked;
    const deleteControllers=deleteFromControllers.checked;
    const controllerCount=deleteControllers?(deleteAllControllers.checked?CONTROLLERS.length:selectedUploadValues('.delete-controller').length):0;
    let text=`Выбрано пользователей: ${userCount}. `;
    if(deleteSystem&&deleteControllers)text+=`Удаление из системы после подтверждения ${userCount} × ${controllerCount} = ${userCount*controllerCount} операций в контроллерах.`;
    else if(deleteControllers)text+=`Удаление только из контроллеров: ${userCount} × ${controllerCount} = ${userCount*controllerCount} операций.`;
    else if(deleteSystem)text+=`Удаление только из системы: ${userCount} пользователей.`;
    else text+='Не выбрано место удаления.';
    deleteSelectionSummary.textContent=text;
}

document.addEventListener('change',e=>{
    if(e.target.matches&&e.target.matches('.delete-user,.delete-controller'))updateDeleteSummary();
});

function deleteStatusText(status){return ({ok:'Удалён',error:'Ошибка'})[status]||status;}
function renderUserDeleteJob(job){
    const state=({queued:'В очереди',running:'Удаление из контроллеров',completed:'Завершено'})[job.state]||job.state;
    const rows=(job.results||[]).map(r=>{
        const u=USERS.find(x=>x.id===r.user_id);
        const c=CONTROLLERS.find(x=>x.node===r.controller_node);
        const cls=r.status==='ok'?'ok':'bad';
        return `<tr><td>${r.user_id} — ${esc(u?userDisplayName(u):'')}</td><td>${r.controller_node} — ${esc(c?controllerDisplayName(c):'')}</td><td class="${cls}">${esc(deleteStatusText(r.status))}</td><td>${esc(r.message||'')}</td></tr>`;
    }).join('');
    const local=job.delete_from_system?`<div class="delete-local-note ${job.local_retained?'bad':'ok'}">Из системы удалено: <b>${job.local_deleted}</b>. Оставлено из-за ошибок/неподтверждённого удаления в контроллерах: <b>${job.local_retained}</b>.</div>`:'';
    deleteResult.innerHTML=`<div class="upload-job-state"><b>${esc(state)}</b> · ${job.completed}/${job.total} · успешно ${job.success} · ошибок ${job.failed}</div>${local}${rows?`<div class="upload-result-table"><table><thead><tr><th>Пользователь</th><th>Контроллер</th><th>Результат</th><th>Комментарий</th></tr></thead><tbody>${rows}</tbody></table></div>`:''}`;
}

async function pollUserDelete(jobId){
    for(let n=0;n<180;n++){
        const r=await api('/api/users/delete-selected/status?job_id='+encodeURIComponent(jobId));
        if(!r.ok)return null;
        const job=await r.json();
        renderUserDeleteJob(job);
        if(job.state!=='queued'&&job.state!=='running')return job;
        await new Promise(resolve=>setTimeout(resolve,700));
    }
    return null;
}

async function startUserDelete(){
    const userIds=selectedUploadValues('.delete-user');
    const nodes=selectedUploadValues('.delete-controller');
    const deleteSystem=deleteFromSystem.checked;
    const deleteControllers=deleteFromControllers.checked;
    if(!deleteSystem&&!deleteControllers)return alert('Выберите: удалить из системы и/или из контроллеров');
    if(!deleteAllUsers.checked&&!userIds.length)return alert('Выберите хотя бы одного пользователя');
    if(deleteAllUsers.checked&&!USERS.length)return alert('Нет пользователей для удаления');
    if(deleteControllers&&!deleteAllControllers.checked&&!nodes.length)return alert('Выберите хотя бы один контроллер');
    if(deleteControllers&&deleteAllControllers.checked&&!CONTROLLERS.length)return alert('Нет обнаруженных контроллеров');

    const userCount=deleteAllUsers.checked?USERS.length:userIds.length;
    const target=deleteSystem&&deleteControllers?'из системы И контроллеров':deleteSystem?'из системы':'из контроллеров';
    const extra=deleteControllers?`\nКонтроллеры: ${deleteAllControllers.checked?'все':nodes.join(', ')}`:'';
    if(!confirm(`Удалить ${userCount} пользователь(ей) ${target}?${extra}\n\nОперация необратима.`))return;

    startDeleteButton.disabled=true;
    deleteResult.innerHTML='<div class="muted">Подготовка удаления...</div>';
    try{
        const r=await api('/api/users/delete-selected',{
            method:'POST',
            headers:{'Content-Type':'application/x-www-form-urlencoded'},
            body:enc({
                all_users:deleteAllUsers.checked?'1':'0',
                user_ids:userIds.join(','),
                delete_system:deleteSystem?'1':'0',
                delete_controllers:deleteControllers?'1':'0',
                all_controllers:deleteAllControllers.checked?'1':'0',
                controller_nodes:nodes.join(',')
            })
        });
        const j=await r.json();
        if(!r.ok||!j.ok){
            deleteResult.innerHTML='<div class="bad">Ошибка: '+esc(j.error||'не удалось начать удаление')+'</div>';
            return;
        }
        if(j.immediate){
            deleteResult.innerHTML=`<div class="upload-job-state ok"><b>Удаление завершено.</b> Из системы удалено: ${j.local_deleted}.</div>`;
        }else{
            await pollUserDelete(j.job_id);
        }
        await loadUsers();
        await loadDepartments(false);
        await loadCards();
        await loadTodayAttendance();
        refreshStatus();
    }finally{
        startDeleteButton.disabled=false;
    }
}


function renderUserCardRows(cards=[]){
    if(!window.userCardsList)return;
    userCardsList.innerHTML='';
    cards.forEach(card=>addUserCardRow(card));
    updateUserCardsEmpty();
}
function updateUserCardsEmpty(){
    if(!window.userCardsList)return;
    const has=userCardsList.querySelector('.user-card-row');
    const old=userCardsList.querySelector('.user-card-empty');if(old)old.remove();
    if(!has){const d=document.createElement('div');d.className='user-card-empty';d.textContent='Карты не назначены. Можно сохранить пользователя без карты или добавить одну/несколько карт.';userCardsList.appendChild(d);}
}
function addUserCardRow(card=''){
    if(!window.userCardsList)return;
    const empty=userCardsList.querySelector('.user-card-empty');if(empty)empty.remove();
    let series='',number='';const m=String(card||'').trim().match(/^(\d+):(\d+)$/);if(m){series=String(Number(m[1]));number=String(Number(m[2]));}
    const row=document.createElement('div');row.className='user-card-row';
    row.innerHTML=`<label><small>Серия</small><input data-card-series inputmode="numeric" maxlength="5" value="${attr(series)}" placeholder="112"></label><label><small>Номер карты</small><input data-card-number inputmode="numeric" maxlength="5" value="${attr(number)}" placeholder="53910"></label><button class="mini danger" type="button" onclick="removeUserCardRow(this)">Удалить</button>`;
    userCardsList.appendChild(row);
}
function removeUserCardRow(button){button.closest('.user-card-row')?.remove();updateUserCardsEmpty();}
function collectUserCards(){
    const cards=[];
    for(const row of userCardsList.querySelectorAll('.user-card-row')){
        const series=row.querySelector('[data-card-series]').value.trim(),number=row.querySelector('[data-card-number]').value.trim();
        if(!series&&!number)continue;
        if(!series||!number)return {ok:false,error:'Для каждой карты укажите одновременно серию и номер'};
        if(!/^\d{1,5}$/.test(series)||Number(series)>65535)return {ok:false,error:'Серия карты должна быть десятичным числом 0..65535'};
        if(!/^\d{1,5}$/.test(number)||Number(number)>65535)return {ok:false,error:'Номер карты должен быть десятичным числом 0..65535'};
        const card=Number(series)+':'+Number(number);if(!cards.includes(card))cards.push(card);
    }
    return {ok:true,cards};
}

async function editUser(id=0){
    if(!USERS_LOADED)await loadUsers();
    await loadDepartments(false);
    let u=USERS.find(x=>x.id===id)||{id:0,enabled:true,last_name:'',first_name:'',middle_name:'',department:'',position:'',cards:[],pin_code:'',access_mode:'card',controller_port:0};
    userDepartment.innerHTML=departmentOptions(u.department||'');
    for(let [k,v] of Object.entries(u)){let e=userForm.elements[k];if(!e||k==='cards')continue;if(e.type==='checkbox')e.checked=!!v;else e.value=v??'';}
    userDepartment.value=u.department||'';
    userForm.elements.pin_code.value=u.pin_code||'';
    userForm.elements.access_mode.value=u.access_mode||'card';
    renderUserCardRows(userCardIds(u));
    userDialog.showModal();
}

async function saveUser(){
    let f=new FormData(userForm),o=Object.fromEntries(f.entries());o.enabled=userForm.elements.enabled.checked?'1':'0';
    o.pin_code=(o.pin_code||'').trim();
    const cardResult=collectUserCards();if(!cardResult.ok)return alert(cardResult.error);
    o.cards=cardResult.cards.join(',');userForm.elements.cards.value=o.cards;
    if(o.pin_code&&(!/^\d{4}$/.test(o.pin_code)||Number(o.pin_code)<1))return alert('PIN должен содержать 4 цифры: 0001..9999');
    if((o.access_mode==='card_or_pin'||o.access_mode==='card_and_pin')&&!o.pin_code)return alert('Для выбранного режима доступа укажите PIN');
    let r=await api('/api/users/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc(o)});let j=await r.json();if(!r.ok||!j.ok)return alert('Ошибка: '+(j.error||'не удалось сохранить пользователя'));
    userDialog.close();await loadUsers();await loadDepartments(false);await loadCards();refreshStatus();
}
async function deleteUser(id){await openUserDelete(id);}
function openAssign(card,keepSoyal=false){
    if(!USERS.length)return alert('Сначала создайте пользователя');
    if(!keepSoyal)SOYAL_ASSIGN_RECORD=null;
    assignForm.card.value=card;assignUsers.innerHTML=USERS.map(u=>`<option value="${u.id}">${u.id} — ${esc(userDisplayName(u))} (${esc(u.department||'без отдела')}) · карт: ${userCardIds(u).length}</option>`).join('');assignDialog.showModal();
}
async function assignCard(){
    let o=Object.fromEntries(new FormData(assignForm)),r;
    if(SOYAL_ASSIGN_RECORD){
        const rec=SOYAL_ASSIGN_RECORD;r=await api('/api/import/soyal/apply-record',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc(soyalPayload(rec,'assign',o.user_id))});
    }else r=await api('/api/cards/assign',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc(o)});
    let j=await r.json();if(!r.ok||!j.ok)return alert('Ошибка: '+(j.error||'не удалось добавить карту'));
    SOYAL_ASSIGN_RECORD=null;assignDialog.close();await loadUsers();await loadCards();refreshSoyalStatuses();renderSoyalPreview();refreshStatus();
}
async function removeCard(card){if(!confirm('Отвязать только карту '+card+' от пользователя? Остальные карты пользователя останутся.'))return;let r=await api('/api/cards/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({card})});let j=await r.json();if(!r.ok||!j.ok)return alert('Не удалось отвязать карту');await loadUsers();await loadCards();refreshStatus();}
async function renameController(node,name){await api('/api/controllers/name',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({node,name})});}

function soyalPayload(rec,action,userId=0){
    return {action,user_id:userId||'',address:rec.address??0,card:rec.card||'',pin_code:rec.pin_code||'',full_name:rec.full_name||'',last_name:rec.last_name||'',first_name:rec.first_name||'',middle_name:rec.middle_name||'',department:rec.department||'',position:rec.position||'',source:rec.source||''};
}
function refreshSoyalStatuses(){
    if(!SOYAL_RECORDS.length)return;
    for(const rec of SOYAL_RECORDS){
        const owner=USERS.find(u=>userCardIds(u).includes(rec.card));
        const byAddr=USERS.filter(u=>Number(u.controller_port||0)===Number(rec.address));
        if(owner){rec.status='linked_card';rec.local_user_id=owner.id;rec.local_user_name=userDisplayName(owner);}
        else if(byAddr.length===1){rec.status='candidate_address';rec.local_user_id=byAddr[0].id;rec.local_user_name=userDisplayName(byAddr[0]);}
        else{rec.status='new';rec.local_user_id=0;rec.local_user_name='';}
    }
}
function soyalStatusHtml(rec){
    if(rec.status==='linked_card')return `<span class="status-pill status-present">Привязана</span><small class="table-subtext">${esc(rec.local_user_name||('Пользователь №'+rec.local_user_id))}</small>`;
    if(rec.status==='candidate_address')return `<span class="status-pill status-warn">Совпадает Address</span><small class="table-subtext">порт ${rec.address} → ${esc(rec.local_user_name||('№'+rec.local_user_id))}</small>`;
    return '<span class="status-pill">Новая</span><small class="table-subtext">совпадений нет</small>';
}
function renderSoyalPreview(){
    if(!window.soyalImportBody)return;
    if(!SOYAL_RECORDS.length){soyalImportBody.innerHTML='<tr><td colspan="7" class="muted">Нет предварительных данных</td></tr>';if(window.soyalAutoBtn)soyalAutoBtn.disabled=true;if(window.soyalCreateBtn)soyalCreateBtn.disabled=true;return;}
    const linked=SOYAL_RECORDS.filter(x=>x.status==='linked_card').length,candidates=SOYAL_RECORDS.filter(x=>x.status==='candidate_address').length,fresh=SOYAL_RECORDS.length-linked-candidates;
    if(window.soyalImportSummary)soyalImportSummary.innerHTML=`Формат: <b>${esc(SOYAL_IMPORT_META?.format||'SOYAL')}</b> · найдено записей: <b>${SOYAL_RECORDS.length}</b> · уже привязано: <b>${linked}</b> · совпадение по Address: <b>${candidates}</b> · новых: <b>${fresh}</b> · пустых слотов: <b>${SOYAL_IMPORT_META?.empty_slots||0}</b>`;
    soyalAutoBtn.disabled=false;soyalCreateBtn.disabled=false;
    soyalImportBody.innerHTML=SOYAL_RECORDS.map((rec,i)=>{
        const fio=rec.full_name||[rec.last_name,rec.first_name,rec.middle_name].filter(Boolean).join(' ')||'—';
        const info=`<b>${esc(fio)}</b>${rec.position?`<small class="table-subtext">${esc(rec.position)}</small>`:''}`;
        let actions='';
        if(rec.status==='linked_card')actions=`<button class="mini" onclick="editUser(${rec.local_user_id})">Открыть пользователя</button>`;
        else actions=`<button class="mini" onclick="applySoyalOne(${i},'auto')">Авто</button> <button class="mini" onclick="openSoyalAssign(${i})">Привязать</button> <button class="mini" onclick="createSoyalOne(${i})">Создать</button>`;
        return `<tr><td><b>${rec.address}</b></td><td class="soyal-card">${esc(cardTextFromRaw(rec.card))}</td><td>${rec.pin_code?esc(rec.pin_code):'—'}</td><td class="soyal-name">${info}</td><td>${esc(rec.department||'—')}</td><td>${soyalStatusHtml(rec)}</td><td><div class="soyal-actions">${actions}</div></td></tr>`;
    }).join('');
}
async function previewSoyal(file){
    if(!file)return;const name=(file.name||'').toLowerCase();const type=name.endsWith('.usr')?'usr':'txt';
    if(window.soyalImportSummary)soyalImportSummary.textContent='Чтение '+file.name+'...';
    const buf=await file.arrayBuffer();const r=await api('/api/import/soyal/preview?type='+type,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:buf});const j=await r.json();
    if(!r.ok||!j.ok){SOYAL_RECORDS=[];renderSoyalPreview();return alert('Ошибка SOYAL: '+(j.error||'не удалось разобрать файл'));}
    SOYAL_IMPORT_META=j;SOYAL_RECORDS=j.records||[];await loadUsers();refreshSoyalStatuses();renderSoyalPreview();
}
function clearSoyalPreview(){SOYAL_RECORDS=[];SOYAL_IMPORT_META=null;SOYAL_ASSIGN_RECORD=null;if(window.soyalImportFile)soyalImportFile.value='';if(window.soyalImportSummary)soyalImportSummary.textContent='Файл SOYAL ещё не выбран.';renderSoyalPreview();}
async function applySoyalRecord(index,action,userId=0){
    const rec=SOYAL_RECORDS[index];if(!rec)return null;
    const r=await api('/api/import/soyal/apply-record',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc(soyalPayload(rec,action,userId))});const j=await r.json();
    if(!r.ok||!j.ok)throw new Error(j.error||'ошибка импорта');return j;
}
async function applySoyalOne(index,action){
    try{const j=await applySoyalRecord(index,action);if(j.action==='no_match')alert('Автосопоставление не найдено. Используйте «Привязать» или «Создать».');await loadUsers();await loadDepartments(false);refreshSoyalStatuses();renderSoyalPreview();return j;}catch(e){alert('Ошибка: '+e.message);}
}
async function createSoyalOne(index){
    const j=await applySoyalOne(index,'create');if(j&&j.user_id)editUser(j.user_id);
}
async function openSoyalAssign(index){
    if(!USERS_LOADED)await loadUsers();const rec=SOYAL_RECORDS[index];if(!rec)return;SOYAL_ASSIGN_RECORD=rec;openAssign(rec.card,true);
}
async function applySoyalBulk(action){
    if(!SOYAL_RECORDS.length)return;const title=action==='auto'?'Автопривязать все записи SOYAL? Совпадение выполняется по карте, затем по уникальному Порту контроллера = Address.':'Создать отдельных пользователей для всех ещё непривязанных карт SOYAL?';if(!confirm(title))return;
    soyalAutoBtn.disabled=true;soyalCreateBtn.disabled=true;let done=0,changed=0,noMatch=0,failed=0;
    for(let i=0;i<SOYAL_RECORDS.length;i++){
        const rec=SOYAL_RECORDS[i];if(rec.status==='linked_card'){done++;continue;}
        try{const j=await applySoyalRecord(i,action);if(j.action==='no_match')noMatch++;else changed++;}catch(e){failed++;}
        done++;if(window.soyalImportSummary)soyalImportSummary.innerHTML=`Импорт SOYAL: <b>${done}/${SOYAL_RECORDS.length}</b> · изменено ${changed} · без совпадения ${noMatch} · ошибок ${failed}`;
    }
    await loadUsers();await loadDepartments(false);refreshSoyalStatuses();renderSoyalPreview();alert(`Готово. Изменено/создано: ${changed}; без автосовпадения: ${noMatch}; ошибок: ${failed}`);
}

async function importUsers(file){if(!file)return;let text=await file.text();let r=await api('/api/import/users',{method:'POST',headers:{'Content-Type':'text/csv'},body:text});let j=await r.json();alert(j.ok?'Импорт выполнен':'Ошибка: '+j.error);await loadUsers();await loadDepartments(false);}


async function loadReportSettings(resetRange=false){
    const r=await api('/api/reports/settings');
    const j=await r.json();
    REPORT_RANGES={today:j.today,week:j.week,month:j.month};
    const from=document.getElementById('reportFrom'),to=document.getElementById('reportTo');
    if(from&&to&&(resetRange||!from.value||!to.value)){from.value=j.today.from;to.value=j.today.to;}
    const exFrom=document.getElementById('extendedReportFrom'),exTo=document.getElementById('extendedReportTo');
    if(exFrom&&exTo&&(resetRange||!exFrom.value||!exTo.value)){exFrom.value=j.today.from;exTo.value=j.today.to;}
    await loadExtendedReportUsers();
    const sc=j.schedule||{};
    if(window.reportScheduleEnabled)reportScheduleEnabled.checked=!!sc.enabled;
    if(window.reportSchedulePeriod)reportSchedulePeriod.value=sc.period||'daily';
    if(window.reportScheduleTime)reportScheduleTime.value=sc.time||'18:00';
    if(window.reportScheduleWeekday)reportScheduleWeekday.value=String(sc.weekday||1);
    if(window.reportScheduleMonthDay)reportScheduleMonthDay.value=String(sc.month_day||1);
    updateReportScheduleFields();
    if(window.reportScheduleStatus){
        let text='Автоматические отчёты ещё не отправлялись.';
        let cls='report-schedule-status muted';
        if(sc.last_status==='ok'){
            text=`Последняя отправка: ${sc.last_sent_at||'—'} · период ${sc.last_period||'—'} · успешно`;
            cls='report-schedule-status ok';
        }else if(sc.last_status==='error'){
            text=`Последняя попытка завершилась ошибкой: ${sc.last_error||'неизвестная ошибка'}`;
            cls='report-schedule-status bad';
        }else if(sc.last_status==='running'){
            text='Автоматический отчёт сейчас формируется/отправляется.';
        }
        reportScheduleStatus.className=cls;reportScheduleStatus.textContent=text;
    }
}

async function applyReportPreset(kind){
    if(!REPORT_RANGES[kind])await loadReportSettings();
    const range=REPORT_RANGES[kind];if(!range)return;
    reportFrom.value=range.from;reportTo.value=range.to;
    reportActionMsg.textContent='';
}

function selectedReportRange(){
    const from=reportFrom.value,to=reportTo.value;
    if(!from||!to){alert('Укажите начальную и конечную дату отчёта');return null;}
    if(from>to){alert('Начальная дата не может быть больше конечной');return null;}
    return {from,to};
}

async function previewReport(){
    const range=selectedReportRange();if(!range)return;
    reportActionMsg.className='report-message muted';reportActionMsg.textContent='Формирование отчёта...';
    const r=await api('/api/reports/preview?'+new URLSearchParams(range));
    const j=await r.json();
    if(!r.ok||!j.ok){reportActionMsg.className='report-message bad';reportActionMsg.textContent='Ошибка: '+(j.error||'не удалось сформировать отчёт');return;}
    reportPreview.textContent=j.content||'';
    reportActionMsg.className='report-message ok';
    reportActionMsg.textContent=`Сформирован ${j.filename}: ${j.days} дн., пользователей ${j.users}, записей пользователь/день ${j.rows}.`;
}

function downloadReport(){
    const range=selectedReportRange();if(!range)return;
    location.href='/api/reports/download?'+new URLSearchParams(range);
}

async function sendReportTelegram(){
    const range=selectedReportRange();if(!range)return;
    if(!confirm(`Отправить TXT-отчёт за ${range.from} — ${range.to} в Telegram?`))return;
    reportActionMsg.className='report-message muted';reportActionMsg.textContent='Формирование и отправка файла в Telegram...';
    const r=await api('/api/reports/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc(range)});
    let j={};try{j=await r.json();}catch{}
    if(!r.ok||!j.ok){reportActionMsg.className='report-message bad';reportActionMsg.textContent='Ошибка Telegram: '+(j.error||'не удалось отправить файл');return;}
    reportActionMsg.className='report-message ok';reportActionMsg.textContent='Отчёт '+(j.filename||'')+' отправлен в Telegram.';
}

async function loadExtendedReportUsers(){
    const box=document.getElementById('extendedReportUsers');if(!box)return;
    if(!USERS_LOADED){USERS=await (await api('/api/users')).json();USERS_LOADED=true;}
    const ordered=[...USERS].sort((a,b)=>{
        const an=[a.last_name,a.first_name,a.middle_name].filter(Boolean).join(' ')||('Пользователь №'+a.id);
        const bn=[b.last_name,b.first_name,b.middle_name].filter(Boolean).join(' ')||('Пользователь №'+b.id);
        return an.localeCompare(bn,'ru')||Number(a.id)-Number(b.id);
    });
    box.innerHTML=ordered.map(u=>{
        const name=[u.last_name,u.first_name,u.middle_name].filter(Boolean).join(' ')||('Пользователь №'+u.id);
        return `<label class="check extended-user-item"><input type="checkbox" value="${Number(u.id)}"> <span><b>${esc(name)}</b><small>${esc(u.department||'Без отдела')}${u.position?' · '+esc(u.position):''}</small></span></label>`;
    }).join('');
    toggleExtendedReportUsers();
}

function toggleExtendedReportUsers(){
    const all=document.getElementById('extendedReportAllUsers');
    document.querySelectorAll('#extendedReportUsers input[type="checkbox"]').forEach(x=>x.disabled=!!(all&&all.checked));
}

async function applyExtendedReportPreset(kind){
    if(!REPORT_RANGES[kind])await loadReportSettings();
    const range=REPORT_RANGES[kind];if(!range)return;
    extendedReportFrom.value=range.from;extendedReportTo.value=range.to;
    if(window.extendedReportActionMsg)extendedReportActionMsg.textContent='';
}

function selectedExtendedReportRequest(){
    const from=extendedReportFrom.value,to=extendedReportTo.value;
    if(!from||!to){alert('Укажите начальную и конечную дату расширенного отчёта');return null;}
    if(from>to){alert('Начальная дата не может быть больше конечной');return null;}
    let users='';
    if(!extendedReportAllUsers.checked){
        const ids=[...document.querySelectorAll('#extendedReportUsers input[type="checkbox"]:checked')].map(x=>x.value);
        if(!ids.length){alert('Выберите хотя бы одного пользователя или включите «Все пользователи»');return null;}
        users=ids.join(',');
    }
    return {from,to,users};
}

async function previewExtendedReport(){
    const req=selectedExtendedReportRequest();if(!req)return;
    extendedReportActionMsg.className='report-message muted';extendedReportActionMsg.textContent='Формирование расширенного отчёта...';
    const r=await api('/api/reports/extended/preview?'+new URLSearchParams(req));let j={};try{j=await r.json();}catch{}
    if(!r.ok||!j.ok){extendedReportActionMsg.className='report-message bad';extendedReportActionMsg.textContent='Ошибка: '+(j.error||'не удалось сформировать расширенный отчёт');return;}
    extendedReportPreview.textContent=j.content||'';
    extendedReportActionMsg.className='report-message ok';
    extendedReportActionMsg.textContent=`Сформирован ${j.filename}: ${j.days} дн., выбрано пользователей ${j.users}, входов/выходов ${j.rows}.`;
}

function downloadExtendedReport(){
    const req=selectedExtendedReportRequest();if(!req)return;
    location.href='/api/reports/extended/download?'+new URLSearchParams(req);
}

async function sendExtendedReportTelegram(){
    const req=selectedExtendedReportRequest();if(!req)return;
    if(!confirm(`Отправить расширенный TXT-отчёт со всеми входами/выходами за ${req.from} — ${req.to} в Telegram?`))return;
    extendedReportActionMsg.className='report-message muted';extendedReportActionMsg.textContent='Формирование и отправка расширенного отчёта...';
    const r=await api('/api/reports/extended/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc(req)});let j={};try{j=await r.json();}catch{}
    if(!r.ok||!j.ok){extendedReportActionMsg.className='report-message bad';extendedReportActionMsg.textContent='Ошибка Telegram: '+(j.error||'не удалось отправить расширенный отчёт');return;}
    extendedReportActionMsg.className='report-message ok';extendedReportActionMsg.textContent='Расширенный отчёт '+(j.filename||'')+' отправлен в Telegram.';
}

function updateReportScheduleFields(){
    if(!window.reportSchedulePeriod)return;
    const enabled=reportScheduleEnabled.checked,period=reportSchedulePeriod.value;
    [reportSchedulePeriod,reportScheduleTime,reportScheduleWeekday,reportScheduleMonthDay].forEach(x=>x.disabled=!enabled);
    if(window.reportWeekdayLabel)reportWeekdayLabel.classList.toggle('hidden',period!=='weekly');
    if(window.reportMonthDayLabel)reportMonthDayLabel.classList.toggle('hidden',period!=='monthly');
}

async function saveReportSchedule(){
    const o={enabled:reportScheduleEnabled.checked?'1':'0',period:reportSchedulePeriod.value,time:reportScheduleTime.value||'18:00',weekday:reportScheduleWeekday.value||'1',month_day:reportScheduleMonthDay.value||'1'};
    const r=await api('/api/reports/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc(o)});let j={};try{j=await r.json();}catch{}
    if(!r.ok||!j.ok){reportScheduleStatus.className='report-schedule-status bad';reportScheduleStatus.textContent='Ошибка: '+(j.error||'не удалось сохранить расписание');return;}
    reportScheduleStatus.className='report-schedule-status ok';reportScheduleStatus.textContent='Расписание сохранено.';
    await loadReportSettings();
}

async function resetSiteActivity(){
    if(!confirm('Сбросить текущее состояние «На объекте»?\n\nБудут очищены только текущее присутствие и кэш последней активности карт. История посещаемости и отчёты останутся без изменений.'))return;
    const b=window.resetSiteActivityButton,m=window.siteActivityResetMsg;
    if(b)b.disabled=true;if(m)m.textContent='Сбрасываю текущее состояние...';
    try{
        const r=await api('/api/settings/reset-site-activity',{method:'POST'});let j={};try{j=await r.json();}catch{}
        if(!r.ok||!j.ok)throw new Error(j.error||('HTTP '+r.status));
        if(m)m.textContent='Активность на объекте сброшена. Сейчас на объекте: 0. Следующее считывание пользователя будет новым приходом.';
        await refreshStatus();
    }catch(e){if(m)m.textContent='Ошибка сброса: '+(e&&e.message?e.message:e);}
    finally{if(b)b.disabled=false;}
}

async function importSettings(file){if(!file)return;let text=await file.text();let r=await api('/api/import/settings',{method:'POST',headers:{'Content-Type':'text/plain'},body:text});let j=await r.json();alert(j.ok?'Настройки импортированы. Перезапустите службу.':'Ошибка импорта');}
async function loadSettings(){
    if(!window.settingsForm)return;
    try{const r=await api('/api/settings');const j=await r.json();
        settingsForm.username.value=j.username||'admin';settingsForm.port.value=j.port||'8080';settingsForm.serial_device.value=j.serial_device||'auto';settingsForm.bot_token.value=j.bot_token||'';settingsForm.chat_id.value=j.chat_id||'';
        settingsForm.telegram_enabled.checked=!!j.telegram_enabled;settingsForm.notify_arrival.checked=j.notify_arrival!==false;settingsForm.notify_departure.checked=j.notify_departure!==false;settingsForm.report_text_copy.checked=j.report_text_copy!==false;settingsForm.report_archive_enabled.checked=!!j.report_archive_enabled;settingsForm.report_archive_path.value=j.report_archive_path||'/srv/samba/UNEX';
    }catch(e){}
}
settingsForm.onsubmit=async e=>{e.preventDefault();let o=Object.fromEntries(new FormData(settingsForm));o.telegram_enabled=settingsForm.telegram_enabled.checked?'1':'0';o.notify_arrival=settingsForm.notify_arrival.checked?'1':'0';o.notify_departure=settingsForm.notify_departure.checked?'1':'0';o.report_text_copy=settingsForm.report_text_copy.checked?'1':'0';o.report_archive_enabled=settingsForm.report_archive_enabled.checked?'1':'0';let r=await api('/api/settings/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc(o)});let j=await r.json();settingsMsg.textContent=j.ok?'Сохранено. Telegram применён сразу; для изменения HTTP-порта/COM перезапустите службу.':'Ошибка';}
async function testTelegram(){let r=await api('/api/telegram/test',{method:'POST'}),j=await r.json();alert(j.ok?'Сообщение отправлено':'Ошибка: '+j.error)}

function protocolCommandName(cmd){return ({18:'12H Read EEPROM',24:'18H Status',32:'20H Write EEPROM',35:'23H Set Time',36:'24H Read RTC',37:'25H Get Event',55:'37H Delete Event',128:'80H Set Node ID',131:'83H Set User Data',132:'84H Stop Waiting',135:'87H Get User Data'})[Number(cmd)]||('0x'+Number(cmd<0?0:cmd).toString(16).toUpperCase().padStart(2,'0'));}
function protocolDirectionLabel(d){return ({TX:'TX →',RX:'← RX',EVENT:'CARD/EVENT',INFO:'INFO'})[d]||d;}
function protocolVisibleEntries(){const showPoll=window.protocolShowPoll&&protocolShowPoll.checked;const node=window.protocolNodeFilter?Number(protocolNodeFilter.value):0;return PROTOCOL_ENTRIES.filter(e=>(!node||e.node===node)&&(showPoll||e.command!==0x25||e.direction==='EVENT'));}
function renderProtocolLive(){
    if(!window.protocolLiveBody)return;const rows=protocolVisibleEntries().slice(-400);
    protocolLiveBody.innerHTML=rows.map(e=>{const cls=e.direction==='EVENT'?'protocol-event':e.direction==='TX'?'protocol-tx':e.direction==='RX'?'protocol-rx':'protocol-info';const info=[e.message,e.user_address>=0?'user='+e.user_address:'',e.card?'карта '+e.card:''].filter(Boolean).join(' · ');return `<tr class="${cls}"><td class="protocol-time">${esc(e.timestamp)}</td><td><b>${esc(protocolDirectionLabel(e.direction))}</b></td><td>${e.node||'—'}</td><td><code>${esc(protocolCommandName(e.command))}</code></td><td>${esc(e.protocol||'')}</td><td>${esc(info||'—')}</td><td><code class="protocol-raw">${esc(e.raw_hex||'')}</code></td></tr>`;}).join('')||'<tr><td colspan="7" class="muted">Пока нет записей. Приложите карту или выполните операцию с контроллером.</td></tr>';
    protocolLiveHint.textContent=`В памяти ${PROTOCOL_ENTRIES.length} записей · показано ${rows.length} · last id ${PROTOCOL_LAST_ID}`;
    if(protocolAutoScroll.checked){const box=document.querySelector('.protocol-live-table');if(box)box.scrollTop=box.scrollHeight;}
}
async function refreshProtocolLive(){
    if(!PROTOCOL_RUNNING)return;
    try{
        const r=await api('/api/protocol/live?after='+encodeURIComponent(PROTOCOL_LAST_ID)+'&limit=300');if(!r.ok)return;const j=await r.json();
        if(j.entries&&j.entries.length){
            PROTOCOL_ENTRIES.push(...j.entries);if(PROTOCOL_ENTRIES.length>1000)PROTOCOL_ENTRIES=PROTOCOL_ENTRIES.slice(-1000);PROTOCOL_LAST_ID=Math.max(PROTOCOL_LAST_ID,Number(j.last_id)||0);
            for(const e of j.entries){if(e.node&&!document.querySelector(`#protocolNodeFilter option[value="${e.node}"]`)){const o=document.createElement('option');o.value=e.node;o.textContent='Node '+e.node;protocolNodeFilter.appendChild(o);}}
            renderProtocolLive();
        }
    }catch(e){}
}
function startProtocolLive(){PROTOCOL_RUNNING=true;protocolLiveState.textContent='LIVE';protocolLiveDot.classList.remove('paused');protocolPauseButton.textContent='Пауза';refreshProtocolLive();if(!PROTOCOL_TIMER)PROTOCOL_TIMER=setInterval(refreshProtocolLive,500);}
function stopProtocolLivePolling(){if(PROTOCOL_TIMER){clearInterval(PROTOCOL_TIMER);PROTOCOL_TIMER=null;}}
function toggleProtocolLive(){PROTOCOL_RUNNING=!PROTOCOL_RUNNING;protocolLiveState.textContent=PROTOCOL_RUNNING?'LIVE':'ПАУЗА';protocolLiveDot.classList.toggle('paused',!PROTOCOL_RUNNING);protocolPauseButton.textContent=PROTOCOL_RUNNING?'Пауза':'Продолжить';if(PROTOCOL_RUNNING){refreshProtocolLive();if(!PROTOCOL_TIMER)PROTOCOL_TIMER=setInterval(refreshProtocolLive,500);}else stopProtocolLivePolling();}
async function clearProtocolLive(){await api('/api/protocol/live/clear',{method:'POST'});PROTOCOL_ENTRIES=[];PROTOCOL_LAST_ID=0;renderProtocolLive();}

async function logout(){await api('/api/logout');location='/login.html'}
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}function attr(s){return esc(s)}function js(s){return String(s).replace(/\\/g,'\\\\').replace(/'/g,"\\'")}

refreshHeaderClock();refreshStatus();loadTodayAttendance();loadUsers();loadDepartments(false);setInterval(refreshHeaderClock,1000);setInterval(refreshStatus,3000);setInterval(loadTodayAttendance,5000);
