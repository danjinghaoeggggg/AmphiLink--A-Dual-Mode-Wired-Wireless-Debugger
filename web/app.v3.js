(() => {
  'use strict';

  const iconPaths = {
    radio: '<path d="M4.9 19.1a10 10 0 0 1 0-14.2M7.8 16.2a6 6 0 0 1 0-8.4M16.2 7.8a6 6 0 0 1 0 8.4M19.1 4.9a10 10 0 0 1 0 14.2"/><circle cx="12" cy="12" r="2"/>',
    plus: '<path d="M5 12h14M12 5v14"/>',
    refresh: '<path d="M20 11a8.1 8.1 0 0 0-15.5-2M4 4v5h5M4 13a8.1 8.1 0 0 0 15.5 2M20 20v-5h-5"/>',
    inbox: '<path d="M4 4h16v13H4zM4 13h4l2 3h4l2-3h4"/>',
    radar: '<path d="M12 12 19 5M4.9 4.9a10 10 0 1 0 14.2 14.2M7.8 7.8a6 6 0 1 0 8.4 8.4"/><circle cx="12" cy="12" r="2"/>',
    x: '<path d="m6 6 12 12M18 6 6 18"/>',
    eye: '<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z"/><circle cx="12" cy="12" r="3"/>',
    eyeOff: '<path d="m3 3 18 18M10.6 10.6a2 2 0 0 0 2.8 2.8M9.9 4.2A10.8 10.8 0 0 1 12 4c6.5 0 10 8 10 8a18 18 0 0 1-2 3.1M6.6 6.6C3.6 8.5 2 12 2 12s3.5 8 10 8a9.8 9.8 0 0 0 4.1-.9"/>',
    wifi: '<path d="M5 12.6a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M2 9a14 14 0 0 1 20 0"/><circle cx="12" cy="20" r="1"/>',
    grip: '<circle cx="9" cy="5" r="1"/><circle cx="15" cy="5" r="1"/><circle cx="9" cy="12" r="1"/><circle cx="15" cy="12" r="1"/><circle cx="9" cy="19" r="1"/><circle cx="15" cy="19" r="1"/>',
    pencil: '<path d="M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L8 18l-4 1 1-4Z"/>',
    trash: '<path d="M3 6h18M8 6V4h8v2M19 6l-1 15H6L5 6M10 11v6M14 11v6"/>',
    up: '<path d="m18 15-6-6-6 6"/>',
    down: '<path d="m6 9 6 6 6-6"/>',
    lock: '<rect x="5" y="10" width="14" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/>',
    info: '<circle cx="12" cy="12" r="10"/><path d="M12 16v-4M12 8h.01"/>'
  };
  const icon = name => `<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${iconPaths[name] || ''}</svg>`;
  document.querySelectorAll('[data-icon]').forEach(el => { el.innerHTML = icon(el.dataset.icon); });

  const messages = {
    zh: { adminLogin:'管理登录', adminPassword:'管理密码', login:'登录', invalid_password:'密码错误', unauthorized:'请先登录', deviceStatus:'设备状态', deviceIdentity:'设备标识', deviceName:'设备名称', colorId:'颜色编号', identitySaved:'设备标识已保存', debugLink:'调试链路', serialLink:'无线串口', wirelessSerial:'无线串口', serialEnabled:'启用无线串口', serialClientConnected:'客户端已连接', baudrate:'波特率', dataBits:'数据位', parity:'校验位', parityNone:'无', parityEven:'偶校验', parityOdd:'奇校验', stopBits:'停止位', serialPort:'TCP 端口', rxPin:'RX 引脚', txPin:'TX 引脚', flowControl:'流控', serialSaved:'串口配置已保存', serialClientWarning:'串口客户端在线，保存配置会断开该客户端。继续吗？', savedNetworks:'已保存网络', addNetwork:'添加网络', nearbyNetworks:'附近网络', scan:'搜索', scanPrompt:'搜索附近可用的 Wi-Fi', password:'密码', passwordRule:'开放网络留空；加密网络需要 8–63 个字符', passwordKeepRule:'留空会保留当前密码；输入新密码可替换', openNetwork:'开放网络', cancel:'取消', save:'保存', connectNetwork:'连接网络', switchNotice:'切换网络会结束当前调试连接，页面可能暂时断开', connect:'连接', starting:'正在启动', connecting:'正在连接', sta:'已连接', ap:'配置热点', active:'使用中', inactive:'空闲', edit:'编辑', remove:'删除', showPassword:'显示密码', hidePassword:'隐藏密码', moveUp:'上移', moveDown:'下移', saved:'已保存', channel:'信道', open:'开放', wpa2:'WPA2', wpa3:'WPA3', wpa2_wpa3:'WPA2/WPA3', unsupported:'不支持', legacy_wpa:'旧版 WPA', legacyWarning:'该网络使用已淘汰的 WPA/TKIP 安全协议，容易受到攻击。仍要继续连接吗？', addTitle:'添加网络', editTitle:'编辑网络', savedOk:'网络列表已保存', removedOk:'网络已删除', scanStarted:'正在搜索附近网络', scanFailed:'搜索失败', connectStarted:'正在尝试连接，页面可能断开', connectSuccess:'连接成功，正在切换网络', connectFailed:'连接失败，已恢复原网络', confirmDelete:'确定删除这个已保存网络？', confirmSwitch:'当前存在调试连接。继续会断开它，是否切换？', credentialsLocked:'Wi-Fi 凭据已锁定', credentialsLockedDetail:'当前管理密码无法解密已保存的凭据。', resetNetworks:'清空并重新配置', confirmReset:'这会永久删除全部已保存的 Wi-Fi 凭据，是否继续？', resetOk:'Wi-Fi 凭据已清空', loadingError:'无法读取设备状态', invalid_identity:'名称需为 1–32 个可打印英文字符，编号需为 1–10', invalid_json:'提交的数据格式不正确', invalid_profile:'请检查 SSID 和密码长度', revision_conflict:'列表已被其他页面修改，请刷新后重试', profile_limit:'最多保存 10 个网络', dap_client_active:'请先关闭调试客户端再搜索', operation_busy:'另一项无线操作正在进行', credentials_locked:'Wi-Fi 凭据已锁定', confirmation_required:'需要确认清空操作', storage_error:'写入 NVS 失败', not_found:'目标不存在', web_assets_missing:'网页资源不可用' },
    en: { adminLogin:'Admin sign-in', adminPassword:'Admin password', login:'Sign in', invalid_password:'Incorrect password', unauthorized:'Sign in required', deviceStatus:'Device status', deviceIdentity:'Device identity', deviceName:'Device name', colorId:'Color ID', identitySaved:'Device identity saved', debugLink:'Debug link', serialLink:'Wireless serial', wirelessSerial:'Wireless serial', serialEnabled:'Enable wireless serial', serialClientConnected:'Client connected', baudrate:'Baud rate', dataBits:'Data bits', parity:'Parity', parityNone:'None', parityEven:'Even', parityOdd:'Odd', stopBits:'Stop bits', serialPort:'TCP port', rxPin:'RX pin', txPin:'TX pin', flowControl:'Flow control', serialSaved:'Serial configuration saved', serialClientWarning:'A serial client is connected. Saving will disconnect it. Continue?', savedNetworks:'Saved networks', addNetwork:'Add network', nearbyNetworks:'Nearby networks', scan:'Scan', scanPrompt:'Scan for nearby Wi-Fi networks', password:'Password', passwordRule:'Leave blank for open networks; secured networks require 8–63 characters', passwordKeepRule:'Leave blank to keep the current password, or enter a replacement', openNetwork:'Open network', cancel:'Cancel', save:'Save', connectNetwork:'Connect network', switchNotice:'Switching networks ends the current debug session and may disconnect this page', connect:'Connect', starting:'Starting', connecting:'Connecting', sta:'Connected', ap:'Setup access point', active:'Active', inactive:'Idle', edit:'Edit', remove:'Delete', showPassword:'Show password', hidePassword:'Hide password', moveUp:'Move up', moveDown:'Move down', saved:'Saved', channel:'Channel', open:'Open', wpa2:'WPA2', wpa3:'WPA3', wpa2_wpa3:'WPA2/WPA3', unsupported:'Unsupported', legacy_wpa:'Legacy WPA', legacyWarning:'This network uses the obsolete WPA/TKIP security protocol and is vulnerable to attack. Connect anyway?', addTitle:'Add network', editTitle:'Edit network', savedOk:'Network list saved', removedOk:'Network removed', scanStarted:'Scanning for nearby networks', scanFailed:'Scan failed', connectStarted:'Trying the network; this page may disconnect', connectSuccess:'Connected; switching networks', connectFailed:'Connection failed; previous network restored', confirmDelete:'Delete this saved network?', confirmSwitch:'A debug client is active. Continuing will disconnect it. Switch networks?', credentialsLocked:'Wi-Fi credentials are locked', credentialsLockedDetail:'The current admin password cannot decrypt the saved credentials.', resetNetworks:'Erase and reconfigure', confirmReset:'This permanently deletes every saved Wi-Fi credential. Continue?', resetOk:'Wi-Fi credentials erased', loadingError:'Unable to read device status', invalid_identity:'Use 1–32 printable ASCII characters and a color ID from 1 to 10', invalid_json:'The submitted data is invalid', invalid_profile:'Check the SSID and password length', revision_conflict:'Another page changed the list. Refresh and try again', profile_limit:'Up to 10 networks can be saved', dap_client_active:'Close the debug client before scanning', operation_busy:'Another wireless operation is running', credentials_locked:'Wi-Fi credentials are locked', confirmation_required:'Confirmation is required', storage_error:'Unable to write NVS', not_found:'Item not found', web_assets_missing:'Web assets unavailable' }
  };
  let language = localStorage.getItem('amphilink-language') || (navigator.language.startsWith('zh') ? 'zh' : 'en');
  let authenticated = false;
  let state = { status: {mode:'starting',ssid:'',ip:'',dap_client:false,serial_client:false}, serial: null, identity: {name:'',id:1}, networks: {revision:0,locked:false,items:[]}, scan:[] };
  let editingId = null;
  let editingHadPassword = false;
  let connectingRecord = null;
  let connectingProfile = null;
  let toastTimer;
  const $ = selector => document.querySelector(selector);
  const t = key => messages[language][key] || key;

  function setLoginVisible(visible) {
    $('#login-screen').hidden = !visible;
    document.body.classList.toggle('login-open', visible);
    if (visible) setTimeout(() => $('#login-password').focus(), 0);
  }

  function applyLanguage() {
    document.documentElement.lang = language === 'zh' ? 'zh-CN' : 'en';
    document.querySelectorAll('[data-i18n]').forEach(el => { el.textContent = t(el.dataset.i18n); });
    document.querySelectorAll('[data-lang]').forEach(el => el.classList.toggle('active', el.dataset.lang === language));
    renderAll();
  }
  function toast(message, error=false) {
    const el = $('#toast'); el.textContent = message; el.className = `toast show${error ? ' error' : ''}`;
    clearTimeout(toastTimer); toastTimer = setTimeout(() => { el.className = 'toast'; }, 4200);
  }
  async function api(path, options={}) {
    const response = await fetch(path, { ...options, credentials:'same-origin', headers: {'Content-Type':'application/json', ...(options.headers || {})} });
    const body = await response.json().catch(() => ({}));
    if (!response.ok) { const error = new Error(body.error?.code || 'storage_error'); error.code = body.error?.code; if (response.status === 401 && path !== '/api/v1/login') { authenticated=false; clearNetworkSecrets(); setLoginVisible(true); } throw error; }
    return body;
  }
  function renderStatus() {
    const status = state.status;
    $('#status-mode').textContent = t(status.mode);
    $('#status-ssid').textContent = status.ssid || '--'; $('#status-ip').textContent = status.ip || '--';
    $('#status-dap').textContent = t(status.dap_client ? 'active' : 'inactive');
    $('#status-serial').textContent = t(status.serial_client ? 'active' : 'inactive');
    $('#status-dot').className = `status-dot ${status.mode === 'sta' ? 'online' : status.mode === 'ap' ? 'ap' : ''}`;
  }
  function renderSerial() {
    const config = state.serial;
    if (!config) return;
    $('#serial-enabled').checked = Boolean(config.enabled);
    $('#serial-baudrate').value = String(config.baudrate);
    $('#serial-data-bits').value = String(config.data_bits);
    $('#serial-parity').value = config.parity;
    $('#serial-stop-bits').value = String(config.stop_bits);
    $('#serial-client-tag').hidden = !config.client;
  }
  function renderIdentity() {
    $('#identity-name').value = state.identity.name;
    const selected = document.querySelector(`input[name="identity-id"][value="${state.identity.id}"]`);
    if (selected) selected.checked = true;
  }
  const passwordDisplay = item => item.revealedPassword !== undefined ? (item.revealedPassword || t('open')) : (item.has_password ? '••••••••' : t('open'));
  function rowButton(iconName, label, className, action, disabled=false) {
    return `<button type="button" class="icon-button ${className}" data-action="${action}" aria-label="${label}" title="${label}" ${disabled?'disabled':''}>${icon(iconName)}</button>`;
  }
  function renderNetworks() {
    const list = $('#saved-list'); const items = state.networks.items;
    const locked=Boolean(state.networks.locked); $('#credentials-locked').hidden=!locked; $('#add-network').disabled=locked; list.hidden=locked;
    list.innerHTML = items.map((item,index) => `<div class="network-row" role="listitem" draggable="true" data-id="${item.id}">
      <button class="icon-button grip" type="button" aria-label="${t('moveUp')} / ${t('moveDown')}" title="${t('moveUp')} / ${t('moveDown')}">${icon('grip')}</button>
      <div class="network-name"><strong>${escapeHtml(item.ssid)}</strong>${item.connected?`<span class="connected-tag">${t('active')}</span>`:''}</div>
      <div class="password-cell"><span class="password-value">${escapeHtml(passwordDisplay(item))}</span>${rowButton(item.revealedPassword!==undefined?'eyeOff':'eye',t(item.revealedPassword!==undefined?'hidePassword':'showPassword'),'','reveal')}</div>
      <div class="row-actions">${rowButton('up',t('moveUp'),'move-up','up',index===0)}${rowButton('down',t('moveDown'),'move-down','down',index===items.length-1)}${rowButton('pencil',t('edit'),'','edit')}${rowButton('trash',t('remove'),'danger','delete')}</div>
    </div>`).join('');
  }
  function renderScan() {
    $('#scan-empty').hidden = state.scan.length !== 0;
    $('#scan-list').innerHTML = state.scan.map(item => `<div class="scan-item ${item.supported?'':'unsupported'}">
      <span class="signal ${item.rssi < -72?'weak':''}">${icon('wifi')}</span>
      <div class="scan-name"><strong>${escapeHtml(item.ssid)}</strong><span class="scan-meta"><span class="auth-tag">${t(item.auth)}</span>${item.saved?`<span class="saved-tag">${t('saved')}</span>`:''}<span>${item.rssi} dBm · ${t('channel')} ${item.channel}</span></span></div>
      <button class="command" type="button" data-connect="${encodeURIComponent(item.ssid)}" ${item.supported?'':'disabled'}>${t('connect')}</button>
    </div>`).join('');
  }
  function renderAll() { renderStatus(); renderSerial(); renderIdentity(); renderNetworks(); renderScan(); }
  function escapeHtml(value) { const div=document.createElement('div'); div.textContent=value; return div.innerHTML; }
  function clearNetworkSecrets() { state.networks.items.forEach(item=>delete item.revealedPassword); $('#profile-password').value=''; $('#connect-password').value=''; }
  async function refresh() {
    try { const [status,identity,networks,serial]=await Promise.all([api('/api/v1/status'),api('/api/v1/device'),api('/api/v1/networks'),api('/api/v1/serial')]); authenticated=true; setLoginVisible(false); clearNetworkSecrets(); state.status=status; state.identity=identity; state.networks=networks; state.serial=serial; renderAll(); }
    catch (error) { if (error.code === 'unauthorized') return; if (location.search.includes('demo=1')) loadDemo(); else toast(t(error.code || 'loadingError'),true); }
  }
  function loadDemo() { authenticated=true; setLoginVisible(false); state.status={mode:'sta',ssid:'Studio-2G',ip:'192.168.1.86',dap_client:false,serial_client:false}; state.serial={revision:1,enabled:true,port:4443,rx_pin:16,tx_pin:17,baudrate:115200,data_bits:8,parity:'none',stop_bits:1,flow_control:'none',client:false}; state.identity={name:'AmphiLink',id:6}; state.networks={revision:7,locked:false,items:[{id:1,ssid:'Studio-2G',has_password:true,connected:true},{id:2,ssid:'Lab-IoT',has_password:true,connected:false},{id:3,ssid:'Guest Bench',has_password:false,connected:false}]}; state.scan=[{ssid:'Studio-2G',rssi:-41,channel:6,auth:'wpa2_wpa3',saved:true,supported:true},{ssid:'Hardware-Lab',rssi:-58,channel:1,auth:'wpa2',saved:false,supported:true},{ssid:'Legacy-Device',rssi:-64,channel:11,auth:'legacy_wpa',saved:false,supported:true},{ssid:'Open-Test',rssi:-70,channel:11,auth:'open',saved:false,supported:true},{ssid:'Enterprise',rssi:-77,channel:1,auth:'unsupported',saved:false,supported:false}]; renderAll(); }
  async function submitSerial(event) { event.preventDefault(); $('#serial-error').textContent=''; if (state.serial?.client && !confirm(t('serialClientWarning'))) return; const payload={revision:state.serial.revision,enabled:$('#serial-enabled').checked,baudrate:Number($('#serial-baudrate').value),data_bits:Number($('#serial-data-bits').value),parity:$('#serial-parity').value,stop_bits:Number($('#serial-stop-bits').value)}; try { state.serial=await api('/api/v1/serial',{method:'PUT',body:JSON.stringify(payload)}); toast(t('serialSaved')); renderSerial(); } catch(e) { $('#serial-error').textContent=t(e.code || 'storage_error'); if(e.code==='revision_conflict') await refresh(); } }
  async function submitLogin(event) { event.preventDefault(); try { await api('/api/v1/login',{method:'POST',body:JSON.stringify({password:$('#login-password').value})}); authenticated=true; $('#login-password').value=''; setLoginVisible(false); await refresh(); } catch(e) { toast(t(e.code || 'loadingError'),true); } }
  async function submitIdentity(event) { event.preventDefault(); const selected=document.querySelector('input[name="identity-id"]:checked'); const payload={name:$('#identity-name').value,id:Number(selected?.value)}; try { state.identity=await api('/api/v1/device',{method:'PUT',body:JSON.stringify(payload)}); renderIdentity(); toast(t('identitySaved')); } catch(e){toast(t(e.code),true);} }
  async function saveOrder() { try { state.networks=await api('/api/v1/networks/order',{method:'PUT',body:JSON.stringify({revision:state.networks.revision,ids:state.networks.items.map(i=>i.id)})}); toast(t('savedOk')); renderNetworks(); } catch(e){toast(t(e.code),true);await refresh();} }
  function move(id,direction) { const index=state.networks.items.findIndex(i=>i.id===id), target=index+direction; if(target<0||target>=state.networks.items.length)return; [state.networks.items[index],state.networks.items[target]]=[state.networks.items[target],state.networks.items[index]]; renderNetworks(); saveOrder(); }
  function updateProfilePasswordState() { const open=$('#profile-open').checked,input=$('#profile-password'); input.disabled=open; input.required=!open&&(!editingId||!editingHadPassword); $('#profile-password-note').textContent=t(editingId&&editingHadPassword&&!open?'passwordKeepRule':'passwordRule'); if(open)input.value=''; }
  function openProfile(item=null) { editingId=item?.id||null; editingHadPassword=Boolean(item?.has_password); $('#profile-title').textContent=t(item?'editTitle':'addTitle'); $('#profile-ssid').value=item?.ssid||''; $('#profile-password').value=''; $('#profile-password').type='password'; $('#profile-open').checked=Boolean(item&&!item.has_password); updateProfilePasswordState(); $('#profile-dialog').showModal(); $('#profile-ssid').focus(); }
  async function submitProfile(event) { event.preventDefault(); const open=$('#profile-open').checked,password=$('#profile-password').value; const payload={revision:state.networks.revision,ssid:$('#profile-ssid').value}; if(!editingId||open||password)payload.password=open?'':password; try { clearNetworkSecrets(); state.networks=await api(editingId?`/api/v1/networks/${editingId}`:'/api/v1/networks',{method:editingId?'PUT':'POST',body:JSON.stringify(payload)}); $('#profile-dialog').close(); toast(t('savedOk')); renderNetworks(); } catch(e){toast(t(e.code),true);} }
  async function removeNetwork(id) { if(!confirm(t('confirmDelete')))return; try { state.networks=await api(`/api/v1/networks/${id}`,{method:'DELETE',body:JSON.stringify({revision:state.networks.revision})}); toast(t('removedOk')); renderNetworks(); } catch(e){toast(t(e.code),true);} }
  async function startScan() { const button=$('#scan-button'); button.classList.add('spin'); button.disabled=true; try { const operation=await api('/api/v1/scan',{method:'POST'}); toast(t('scanStarted')); for(let i=0;i<40;i++){await new Promise(r=>setTimeout(r,500));const result=await api(`/api/v1/scan/${operation.operation_id}`);if(result.state==='succeeded'){state.scan=result.items;renderScan();return;}if(result.state==='failed')throw new Error('scanFailed');}throw new Error('scanFailed'); } catch(e){toast(t(e.code||e.message||'scanFailed'),true);} finally{button.classList.remove('spin');button.disabled=false;} }
  function openConnect(record) { if(record.auth==='legacy_wpa'&&!confirm(t('legacyWarning')))return; connectingRecord=record; connectingProfile=state.networks.items.find(i=>i.ssid===record.ssid)||null; $('#connect-ssid').textContent=record.ssid; $('#connect-auth').textContent=t(record.auth); $('#connect-password').value=''; $('#connect-password').type='password'; $('#connect-password-label').hidden=Boolean(connectingProfile)||record.auth==='open'; $('#connect-dialog').showModal(); if(!connectingProfile&&record.auth!=='open')$('#connect-password').focus(); }
  async function submitConnect(event) { event.preventDefault(); if(state.status.dap_client&&!confirm(t('confirmSwitch')))return; const payload=connectingProfile?{profile_id:connectingProfile.id}:{ssid:connectingRecord.ssid,password:connectingRecord.auth==='open'?'':$('#connect-password').value}; try { const operation=await api('/api/v1/connect',{method:'POST',body:JSON.stringify(payload)}); $('#connect-password').value=''; $('#connect-dialog').close(); toast(t('connectStarted')); for(let i=0;i<30;i++){await new Promise(r=>setTimeout(r,500));const result=await api(`/api/v1/connect/${operation.operation_id}`);if(result.state==='succeeded'){toast(t('connectSuccess'));return;}if(result.state==='failed'){toast(t('connectFailed'),true);await refresh();return;}} } catch(e){ if(e instanceof TypeError) toast(t('connectStarted')); else toast(t(e.code),true); } }
  async function toggleNetworkReveal(item) { if(item.revealedPassword!==undefined){delete item.revealedPassword;renderNetworks();return;} try { const result=await api(`/api/v1/networks/${item.id}/password`); item.revealedPassword=result.password; renderNetworks(); } catch(e){toast(t(e.code),true);} }
  async function resetNetworks() { if(!confirm(t('confirmReset')))return; try { clearNetworkSecrets(); state.networks=await api('/api/v1/networks/reset',{method:'POST',body:JSON.stringify({confirm:true})}); toast(t('resetOk')); renderNetworks(); } catch(e){toast(t(e.code),true);} }
  function togglePassword(input,button) { const visible=input.type==='text'; input.type=visible?'password':'text'; button.innerHTML=icon(visible?'eye':'eyeOff'); }

  document.querySelectorAll('[data-lang]').forEach(button=>button.addEventListener('click',()=>{language=button.dataset.lang;localStorage.setItem('amphilink-language',language);applyLanguage();}));
  $('#login-form').addEventListener('submit',submitLogin);
  $('#login-eye').addEventListener('click',()=>togglePassword($('#login-password'),$('#login-eye')));
  $('#identity-form').addEventListener('submit',submitIdentity);
  $('#serial-form').addEventListener('submit',submitSerial);
  $('#add-network').addEventListener('click',()=>openProfile()); $('#profile-form').addEventListener('submit',submitProfile); $('#profile-cancel').addEventListener('click',()=>{$('#profile-password').value='';$('#profile-dialog').close();}); $('#profile-open').addEventListener('change',updateProfilePasswordState);
  $('#connect-form').addEventListener('submit',submitConnect); $('#connect-cancel').addEventListener('click',()=>{$('#connect-password').value='';$('#connect-dialog').close();}); $('#reset-networks').addEventListener('click',resetNetworks);
  document.querySelectorAll('.dialog-close').forEach(button=>button.addEventListener('click',()=>button.closest('dialog').close()));
  $('#dialog-eye').addEventListener('click',()=>togglePassword($('#profile-password'),$('#dialog-eye'))); $('#connect-eye').addEventListener('click',()=>togglePassword($('#connect-password'),$('#connect-eye'))); $('#scan-button').addEventListener('click',startScan);
  $('#saved-list').addEventListener('click',event=>{const button=event.target.closest('[data-action]');if(!button)return;const row=button.closest('[data-id]'),id=Number(row.dataset.id),item=state.networks.items.find(i=>i.id===id);if(button.dataset.action==='reveal')toggleNetworkReveal(item);if(button.dataset.action==='edit')openProfile(item);if(button.dataset.action==='delete')removeNetwork(id);if(button.dataset.action==='up')move(id,-1);if(button.dataset.action==='down')move(id,1);});
  $('#scan-list').addEventListener('click',event=>{const button=event.target.closest('[data-connect]');if(button){const ssid=decodeURIComponent(button.dataset.connect);openConnect(state.scan.find(i=>i.ssid===ssid));}});
  let draggedId=null; $('#saved-list').addEventListener('dragstart',event=>{const row=event.target.closest('[data-id]');if(!row)return;draggedId=Number(row.dataset.id);row.classList.add('dragging');}); $('#saved-list').addEventListener('dragover',event=>{event.preventDefault();event.target.closest('[data-id]')?.classList.add('drop-target');}); $('#saved-list').addEventListener('dragleave',event=>event.target.closest('[data-id]')?.classList.remove('drop-target')); $('#saved-list').addEventListener('drop',event=>{event.preventDefault();const row=event.target.closest('[data-id]');document.querySelectorAll('.drop-target,.dragging').forEach(el=>el.classList.remove('drop-target','dragging'));if(!row)return;const target=Number(row.dataset.id),from=state.networks.items.findIndex(i=>i.id===draggedId),to=state.networks.items.findIndex(i=>i.id===target);if(from>=0&&to>=0&&from!==to){const [item]=state.networks.items.splice(from,1);state.networks.items.splice(to,0,item);renderNetworks();saveOrder();}});
  $('#profile-dialog').addEventListener('close',()=>{$('#profile-password').value='';}); $('#connect-dialog').addEventListener('close',()=>{$('#connect-password').value='';connectingProfile=null;});
  window.addEventListener('pagehide',clearNetworkSecrets);
  applyLanguage(); setLoginVisible(true); refresh(); setInterval(async()=>{if(!authenticated)return;try{state.status=await api('/api/v1/status');if(state.serial) { state.serial.client=Boolean(state.status.serial_client); } renderStatus(); renderSerial();}catch(_){}},3000);
})();
