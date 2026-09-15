/* Dashboard i18n — zh / en */
(function (global) {
  const STORAGE_KEY = "dashboard-lang";

  const I18N = {
    zh: {
      "nav.ble": "BLE 配网",
      "nav.wifi": "网络配置记录",
      "nav.llm": "大模型配置",
      "nav.chat": "大模型对话历史",
      "nav.lang": "EN",
      "updated.connecting": "正在连接后端...",
      "updated.at": "更新于 {time}",
      "updated.backend_down": "后端不可用",
      "pill.backend": "后端",
      "pill.device_connected": "设备已连接",
      "pill.device_offline": "设备离线",
      "pill.push": "推送",
      "section.current": "当前显示",
      "gif.alt": "当前状态表情",
      "source.prefix": "来源：",
      "source.device": "设备",
      "text.wait_event": "等待事件",
      "text.no_extra": "暂无附加信息",
      "text.device_offline": "设备未连接后端",
      "meta.target": "推送目标",
      "meta.failures": "推送失败次数",
      "meta.router_wifi": "ESP 路由 WiFi",
      "meta.device": "设备状态",
      "meta.last_event": "最后事件",
      "meta.not_enabled": "未启用",
      "meta.connected": "已连接",
      "meta.disconnected": "未连接",
      "meta.not_detected": "未检测",
      "meta.ws_connected": "WebSocket 已连接",
      "meta.ws_disconnected": "WebSocket 未连接",
      "section.push_status": "推送状态（GIF / 表情）",
      "section.push_text": "推送文字（底部文案）",
      "hint.voice_only":
        "设备只回显 <b>VOICE</b> 来源。CURSOR/BOT 只会改顶部来源标签，底部字幕不会出现。",
      "hint.tts_voice":
        "带「本地」的走本机 Windows 语音，不联网。其余为微软 Edge TTS 在线合成。",
      "btn.send_status": "发送状态",
      "btn.send_text": "发送文字",
      "btn.send_speak": "发送并播报",
      "btn.mic_on": "麦克风开",
      "btn.mic_off": "麦克风关",
      "label.volume": "播报音量",
      "label.mic": "麦克风",
      "label.tts_voice": "语音音色",
      "placeholder.text": "例如：正在修改代码",
      "log.status_history": "状态历史",
      "log.speech": "语音识别",
      "log.backend": "后端日志",
      "log.items": "{n} 条",
      "log.empty_events": "暂无事件记录",
      "log.empty_asr": "等待设备收听并上传语音",
      "log.empty_backend": "等待 Python 后端输出",
      "log.no_extra_paren": "（无附加信息）",
      "log.no_speech": "（未识别到语音）",
      "role.user": "用户",
      "role.assistant": "助手",
      "link.online": "在线",
      "link.connected": "已连接",
      "link.offline": "离线",
      "link.checking": "检查中",
      "unknown": "未知",
      "status.idle": "空闲",
      "status.thinking": "思考中",
      "status.coding": "编码中",
      "status.reading": "读取中",
      "status.testing": "测试中",
      "status.waiting": "等待中",
      "status.done": "已完成",
      "status.error": "错误",
      "status.offline": "离线",
      "status.stale": "已过期",
      "status.unknown": "未知",
      "status.tool": "使用工具",
      "status.ear": "收听中",
      "status.speaking": "播报中",
      "quick.idle": "空闲",
      "quick.thinking": "思考中",
      "quick.coding": "编码中",
      "quick.testing": "测试中",
      "quick.waiting": "等待中",
      "quick.done": "已完成",
      "quick.error": "错误",
      "quick.modifying": "正在修改代码",
      "quick.wait_moment": "请稍等片刻",
      "quick.listening": "聆听中",
      "quick.replying": "回复中",
      "quick.greeting": "语音问候",
      "quick.clear": "清空文字",
      "quick.greeting_text": "你好，有什么可以帮你的？",
      "send.status_sending": "发送状态中...",
      "send.status_ok": "状态已发送",
      "send.status_debounced": "已忽略（与上次相同）",
      "send.text_sending": "发送文字中...",
      "send.speak_sending": "合成语音并发送中...",
      "send.text_ok": "文字已发送（底部文案）",
      "send.text_cleared": "已清空底部文案",
      "send.speak_ok": "文字已发送并开始播报",
      "send.speak_fail": "文字已发送，播报失败：{detail}",
      "modal.close": "关闭",
      "ble.title": "BLE 配网",
      "ble.subtitle": "BLE 配网（替代 SerialTest）",
      "ble.hint":
        "先在设备上长按 <b>BOOT 3 秒</b>（不要按 RST），屏幕出现蓝牙标志后再点「扫描」。<b>不会弹出</b> Windows 蓝牙配对框（这是 BLE GATT，不是经典蓝牙配对）。也不要在「系统设置 → 蓝牙」里找/配对 AgentDisplay；请只用本页扫描。本机蓝牙需保持开启；若扫不到请刷新页面（需后端已重启加载 BLE 接口）。",
      "ble.scan": "扫描 AgentDisplay",
      "ble.device_placeholder": "先扫描设备",
      "ble.apply": "写入并 APPLY",
      "ble.pass_placeholder": "WiFi 密码",
      "ble.host_placeholder": "后端 IP",
      "ble.port_placeholder": "端口",
      "ble.ip_placeholder": "设备静态 IP（可选）",
      "ble.mask_placeholder": "掩码（可选）",
      "ble.gw_placeholder": "网关（可选）",
      "ble.scanning": "扫描中（约6秒）…",
      "ble.not_found_option": "未找到 AgentDisplay",
      "ble.not_found": "未找到设备，请确认已长按 BOOT 进入配网",
      "ble.found": "找到 {n} 台，可填写 WiFi 后点击写入",
      "ble.applying": "连接并写入中…",
      "ble.need_ssid_host": "请填写 SSID 与后端 IP",
      "ble.ok": "配网成功：{ssid} -> {host}（{apply}）",
      "ble.no_logs": "暂无 BLE 日志",
      "ble.no_bleak": "后端缺少 bleak，请 pip install bleak",
      "wifi.modal_title": "ESP 网络配置",
      "wifi.title": "设备已存网络配置",
      "wifi.hint":
        "这里编辑的是 ESP NVS 里已经保存的配置（最多 5 条）。保存 / 删除 / 启用都会直接写到设备。切换或改当前生效项时设备会重连 WiFi。设备离线时请用「BLE 配网」。",
      "wifi.pull": "刷新设备列表",
      "wifi.new": "新增",
      "wifi.empty_open": "打开后将从设备读取",
      "wifi.save": "保存到设备",
      "wifi.ip_placeholder": "设备静态 IP（空=DHCP）",
      "wifi.empty_device": "设备上还没有网络配置",
      "wifi.current": "当前",
      "wifi.edit": "编辑",
      "wifi.use": "启用",
      "wifi.del": "删除",
      "wifi.dhcp": " · DHCP",
      "wifi.ip_prefix": " · IP ",
      "wifi.unnamed": "未命名",
      "wifi.saved": "已保存到设备",
      "wifi.added": "已新增到设备",
      "wifi.new_hint": "填写后点「保存到设备」新增一条（最多 5 条）",
      "wifi.editing": "正在编辑 #{idx}，改完点保存到设备",
      "wifi.activated": "已切换为 #{idx}，设备正在重连",
      "wifi.deleted": "已从设备删除",
      "wifi.confirm_delete": "确认从设备删除这条网络配置？",
      "wifi.pulling": "正在读取设备已存配置…",
      "wifi.pull_ok": "设备上有 {n} 条，当前生效 #{active}",
      "wifi.err_ssid_host": "SSID 与后端 IP 不能为空",
      "llm.title": "大模型配置",
      "llm.subtitle": "大模型配置（可存多条）",
      "llm.hint":
        "OpenAI 兼容 Chat Completions（HTTP 流式）。可保存最多 8 条，启用其中一条后立即生效。打开本窗口时页面轮询不会覆盖你正在编辑的内容。",
      "llm.reload": "刷新列表",
      "llm.new": "新增",
      "llm.empty_open": "打开后读取已保存配置",
      "llm.empty": "还没有 LLM 配置",
      "llm.name": "名称",
      "llm.base_url": "Base URL",
      "llm.api_key": "API Key",
      "llm.model": "模型名称",
      "llm.save": "保存",
      "llm.save_use": "保存并启用",
      "llm.name_ph": "例如 2api / 本地",
      "llm.base_ph": "https://api.example.com 或 .../v1",
      "llm.key_ph": "sk-...（留空或星号=不改原密钥）",
      "llm.model_ph": "模型名",
      "llm.saved": "已保存（未切换当前启用项）",
      "llm.saved_use": "已保存并启用",
      "llm.reloaded": "已重新读取",
      "llm.new_hint": "填写后点保存，会新增一条而不会覆盖已有配置",
      "llm.editing": "正在编辑「{name}」，改完点保存",
      "llm.activated": "已启用「{name}」",
      "llm.deleted": "已删除",
      "llm.confirm_delete": "确认删除这条 LLM 配置？",
      "llm.err_required": "Base URL 与模型名称不能为空",
      "tts.local_group": "本地系统（离线）",
      "tts.online_group": "微软在线",
      "chat.title": "大模型对话历史",
      "chat.subtitle": "落盘于项目 log/llm_chat.jsonl，支持分页与关键词查询",
      "chat.back": "← 返回控制台",
      "chat.filters_title": "历史记录筛选",
      "chat.filters_hint": "查找已保存的对话",
      "chat.keyword": "关键词",
      "chat.keyword_ph": "用户 / 助手 / 模型 / session",
      "chat.source": "来源",
      "chat.all": "全部",
      "chat.date_from": "起始日期",
      "chat.date_to": "结束日期",
      "chat.search": "查询",
      "chat.reset": "重置",
      "chat.total": "共 {n} 条",
      "chat.tokens_sum": "筛选结果 token 合计 {n}",
      "chat.page_of": "第 {page} / {pages} 页",
      "chat.prev": "上一页",
      "chat.next": "下一页",
      "chat.loading": "加载中...",
      "chat.no_records": "没有匹配的记录",
      "chat.load_fail": "加载失败：{detail}",
      "chat.empty_paren": "(空)",
    },
    en: {
      "nav.ble": "BLE Setup",
      "nav.wifi": "WiFi Profiles",
      "nav.llm": "LLM Config",
      "nav.chat": "Chat History",
      "nav.lang": "中文",
      "updated.connecting": "Connecting to backend...",
      "updated.at": "Updated at {time}",
      "updated.backend_down": "Backend unavailable",
      "pill.backend": "Backend",
      "pill.device_connected": "Device connected",
      "pill.device_offline": "Device offline",
      "pill.push": "Push",
      "section.current": "Current display",
      "gif.alt": "Current status emoji",
      "source.prefix": "Source: ",
      "source.device": "Device",
      "text.wait_event": "Waiting for events",
      "text.no_extra": "No extra info",
      "text.device_offline": "Device not connected",
      "meta.target": "Push target",
      "meta.failures": "Push failures",
      "meta.router_wifi": "ESP router WiFi",
      "meta.device": "Device status",
      "meta.last_event": "Last event",
      "meta.not_enabled": "Not enabled",
      "meta.connected": "Connected",
      "meta.disconnected": "Disconnected",
      "meta.not_detected": "Not detected",
      "meta.ws_connected": "WebSocket connected",
      "meta.ws_disconnected": "WebSocket disconnected",
      "section.push_status": "Push status (GIF / face)",
      "section.push_text": "Push caption (bottom text)",
      "hint.voice_only":
        "Only <b>VOICE</b> source shows on the device caption. CURSOR/BOT update the top source label only.",
      "hint.tts_voice":
        "Voices marked local use Windows SAPI offline. Others use Microsoft Edge TTS online.",
      "btn.send_status": "Send status",
      "btn.send_text": "Send text",
      "btn.send_speak": "Send & speak",
      "btn.mic_on": "Mic on",
      "btn.mic_off": "Mic off",
      "label.volume": "Volume",
      "label.mic": "Microphone",
      "label.tts_voice": "TTS voice",
      "placeholder.text": "e.g. Modifying code",
      "log.status_history": "Status history",
      "log.speech": "Speech recognition",
      "log.backend": "Backend logs",
      "log.items": "{n} items",
      "log.empty_events": "No events yet",
      "log.empty_asr": "Waiting for voice upload",
      "log.empty_backend": "Waiting for backend output",
      "log.no_extra_paren": "(no extra info)",
      "log.no_speech": "(no speech recognized)",
      "role.user": "User",
      "role.assistant": "Assistant",
      "link.online": "Online",
      "link.connected": "Connected",
      "link.offline": "Offline",
      "link.checking": "Checking",
      "unknown": "Unknown",
      "status.idle": "Idle",
      "status.thinking": "Thinking",
      "status.coding": "Coding",
      "status.reading": "Reading",
      "status.testing": "Testing",
      "status.waiting": "Waiting",
      "status.done": "Done",
      "status.error": "Error",
      "status.offline": "Offline",
      "status.stale": "Stale",
      "status.unknown": "Unknown",
      "status.tool": "Tool",
      "status.ear": "Listening",
      "status.speaking": "Speaking",
      "quick.idle": "Idle",
      "quick.thinking": "Thinking",
      "quick.coding": "Coding",
      "quick.testing": "Testing",
      "quick.waiting": "Waiting",
      "quick.done": "Done",
      "quick.error": "Error",
      "quick.modifying": "Modifying code",
      "quick.wait_moment": "Please wait",
      "quick.listening": "Listening",
      "quick.replying": "Replying",
      "quick.greeting": "Voice greeting",
      "quick.clear": "Clear text",
      "quick.greeting_text": "Hello, how can I help?",
      "send.status_sending": "Sending status...",
      "send.status_ok": "Status sent",
      "send.status_debounced": "Ignored (same as last)",
      "send.text_sending": "Sending text...",
      "send.speak_sending": "Synthesizing and sending...",
      "send.text_ok": "Text sent (caption)",
      "send.text_cleared": "Caption cleared",
      "send.speak_ok": "Text sent and playback started",
      "send.speak_fail": "Text sent, playback failed: {detail}",
      "modal.close": "Close",
      "ble.title": "BLE provisioning",
      "ble.subtitle": "BLE provisioning (replaces SerialTest)",
      "ble.hint":
        "Long-press <b>BOOT 3s</b> on the device (do not press RST) until the BLE icon appears, then click Scan. Windows will <b>not</b> show a classic Bluetooth pairing dialog (this is BLE GATT). Do not pair in system Bluetooth settings — use this page only. Keep Bluetooth enabled; refresh if scan fails (backend must expose BLE APIs).",
      "ble.scan": "Scan AgentDisplay",
      "ble.device_placeholder": "Scan devices first",
      "ble.apply": "Write & APPLY",
      "ble.pass_placeholder": "WiFi password",
      "ble.host_placeholder": "Backend IP",
      "ble.port_placeholder": "Port",
      "ble.ip_placeholder": "Device static IP (optional)",
      "ble.mask_placeholder": "Netmask (optional)",
      "ble.gw_placeholder": "Gateway (optional)",
      "ble.scanning": "Scanning (~6s)...",
      "ble.not_found_option": "AgentDisplay not found",
      "ble.not_found": "No device found — long-press BOOT to enter provisioning",
      "ble.found": "Found {n} device(s). Fill WiFi and write.",
      "ble.applying": "Connecting and writing...",
      "ble.need_ssid_host": "SSID and backend IP are required",
      "ble.ok": "Provisioned: {ssid} -> {host} ({apply})",
      "ble.no_logs": "No BLE logs",
      "ble.no_bleak": "bleak not installed — run pip install bleak",
      "wifi.modal_title": "ESP network config",
      "wifi.title": "Saved profiles on device",
      "wifi.hint":
        "Edits NVS profiles on the ESP (max 5). Save / delete / activate writes directly to the device. WiFi reconnects when switching profiles. Use BLE setup when the device is offline.",
      "wifi.pull": "Refresh from device",
      "wifi.new": "New",
      "wifi.empty_open": "Open to load from device",
      "wifi.save": "Save to device",
      "wifi.ip_placeholder": "Device static IP (empty = DHCP)",
      "wifi.empty_device": "No profiles on device",
      "wifi.current": "Active",
      "wifi.edit": "Edit",
      "wifi.use": "Activate",
      "wifi.del": "Delete",
      "wifi.dhcp": " · DHCP",
      "wifi.ip_prefix": " · IP ",
      "wifi.unnamed": "Unnamed",
      "wifi.saved": "Saved to device",
      "wifi.added": "Added to device",
      "wifi.new_hint": "Fill the form and save (max 5 profiles)",
      "wifi.editing": "Editing #{idx} — save when done",
      "wifi.activated": "Activated #{idx} — device reconnecting",
      "wifi.deleted": "Deleted from device",
      "wifi.confirm_delete": "Delete this profile from the device?",
      "wifi.pulling": "Reading profiles from device...",
      "wifi.pull_ok": "{n} profile(s) on device; active #{active}",
      "wifi.err_ssid_host": "SSID and backend IP are required",
      "llm.title": "LLM configuration",
      "llm.subtitle": "LLM profiles (up to 8)",
      "llm.hint":
        "OpenAI-compatible Chat Completions (HTTP streaming). Save up to 8 profiles; activating one takes effect immediately. Polling pauses overwriting while this dialog is open.",
      "llm.reload": "Reload",
      "llm.new": "New",
      "llm.empty_open": "Open to load saved profiles",
      "llm.empty": "No LLM profiles yet",
      "llm.name": "Name",
      "llm.base_url": "Base URL",
      "llm.api_key": "API Key",
      "llm.model": "Model",
      "llm.save": "Save",
      "llm.save_use": "Save & activate",
      "llm.name_ph": "e.g. 2api / local",
      "llm.base_ph": "https://api.example.com or .../v1",
      "llm.key_ph": "sk-... (blank = keep existing key)",
      "llm.model_ph": "Model name",
      "llm.saved": "Saved (not activated)",
      "llm.saved_use": "Saved and activated",
      "llm.reloaded": "Reloaded",
      "llm.new_hint": "Fill the form and save to add a new profile",
      "llm.editing": "Editing \"{name}\" — save when done",
      "llm.activated": "Activated \"{name}\"",
      "llm.deleted": "Deleted",
      "llm.confirm_delete": "Delete this LLM profile?",
      "llm.err_required": "Base URL and model are required",
      "tts.local_group": "Local (offline)",
      "tts.online_group": "Microsoft online",
      "chat.title": "LLM chat history",
      "chat.subtitle": "Stored in log/llm_chat.jsonl — paginated keyword search",
      "chat.back": "← Back to dashboard",
      "chat.filters_title": "History filters",
      "chat.filters_hint": "Search saved conversations",
      "chat.keyword": "Keyword",
      "chat.keyword_ph": "User / assistant / model / session",
      "chat.source": "Source",
      "chat.all": "All",
      "chat.date_from": "From date",
      "chat.date_to": "To date",
      "chat.search": "Search",
      "chat.reset": "Reset",
      "chat.total": "{n} records",
      "chat.tokens_sum": "Filtered tokens: {n}",
      "chat.page_of": "Page {page} / {pages}",
      "chat.prev": "Previous",
      "chat.next": "Next",
      "chat.loading": "Loading...",
      "chat.no_records": "No matching records",
      "chat.load_fail": "Load failed: {detail}",
      "chat.empty_paren": "(empty)",
    },
  };

  const STATUS_KEY = {
    IDLE: "status.idle",
    THINKING: "status.thinking",
    CODING: "status.coding",
    READING: "status.reading",
    TESTING: "status.testing",
    WAITING: "status.waiting",
    DONE: "status.done",
    ERROR: "status.error",
    OFFLINE: "status.offline",
    STALE: "status.stale",
    UNKNOWN: "status.unknown",
    TOOL: "status.tool",
    EAR: "status.ear",
    SPEAKING: "status.speaking",
  };

  const LINK_KEY = {
    ONLINE: "link.online",
    CONNECTED: "link.connected",
    OFFLINE: "link.offline",
    CHECKING: "link.checking",
    UNKNOWN: "unknown",
  };

  let lang =
    localStorage.getItem(STORAGE_KEY) ||
    ((navigator.language || "").toLowerCase().startsWith("zh") ? "zh" : "en");
  let onChange = null;

  function t(key, vars) {
    let s = (I18N[lang] && I18N[lang][key]) || (I18N.zh[key]) || key;
    if (vars) {
      Object.keys(vars).forEach((k) => {
        s = s.replace(new RegExp("\\{" + k + "\\}", "g"), vars[k]);
      });
    }
    return s;
  }

  function locale() {
    return lang === "zh" ? "zh-CN" : "en-US";
  }

  function statusText(code) {
    const key = STATUS_KEY[String(code || "").toUpperCase()];
    return key ? t(key) : String(code || t("unknown"));
  }

  function linkText(code) {
    const key = LINK_KEY[String(code || "").toUpperCase()];
    return key ? t(key) : String(code || t("unknown"));
  }

  function quickStatus() {
    return [
      { label: t("quick.idle"), status: "IDLE", source: "BOT" },
      { label: t("quick.thinking"), status: "THINKING", source: "CURSOR" },
      { label: t("quick.coding"), status: "CODING", source: "CURSOR" },
      { label: t("quick.testing"), status: "TESTING", source: "CURSOR" },
      { label: t("quick.waiting"), status: "WAITING", source: "BOT" },
      { label: t("quick.done"), status: "DONE", source: "BOT" },
      { label: t("quick.error"), status: "ERROR", source: "VOICE" },
    ];
  }

  function quickText() {
    return [
      {
        label: t("quick.modifying"),
        text: t("quick.modifying"),
        source: "VOICE",
        role: "assistant",
      },
      {
        label: t("quick.wait_moment"),
        text: t("quick.wait_moment"),
        source: "VOICE",
        role: "assistant",
      },
      {
        label: t("quick.listening"),
        text: t("quick.listening"),
        source: "VOICE",
        role: "assistant",
      },
      {
        label: t("quick.replying"),
        text: t("quick.replying"),
        source: "VOICE",
        role: "assistant",
      },
      {
        label: t("quick.greeting"),
        text: t("quick.greeting_text"),
        source: "VOICE",
        role: "assistant",
      },
      { label: t("quick.clear"), text: "", source: "VOICE" },
    ];
  }

  function applyStatic() {
    document.documentElement.lang = lang === "zh" ? "zh-CN" : "en";
    document.querySelectorAll("[data-i18n]").forEach((el) => {
      const key = el.getAttribute("data-i18n");
      if (!key) return;
      if (el.getAttribute("data-i18n-html") === "1") el.innerHTML = t(key);
      else el.textContent = t(key);
    });
    document.querySelectorAll("[data-i18n-placeholder]").forEach((el) => {
      const key = el.getAttribute("data-i18n-placeholder");
      if (key) el.placeholder = t(key);
    });
    document.querySelectorAll("[data-i18n-aria]").forEach((el) => {
      const key = el.getAttribute("data-i18n-aria");
      if (!key) return;
      if (el.tagName === "IMG") el.alt = t(key);
      else el.setAttribute("aria-label", t(key));
    });
    const sendStatus = document.getElementById("send-status");
    if (sendStatus) {
      Array.from(sendStatus.options).forEach((opt) => {
        const key = STATUS_KEY[opt.value];
        if (key) opt.textContent = t(key);
      });
    }
    const toggle = document.getElementById("lang-toggle");
    if (toggle) toggle.textContent = t("nav.lang");
  }

  function setLang(next) {
    lang = next === "en" ? "en" : "zh";
    localStorage.setItem(STORAGE_KEY, lang);
    applyStatic();
    if (onChange) onChange();
  }

  function toggleLang() {
    setLang(lang === "zh" ? "en" : "zh");
  }

  global.DashI18n = {
    t,
    locale,
    statusText,
    linkText,
    quickStatus,
    quickText,
    applyStatic,
    setLang,
    toggleLang,
    onLangChange(fn) {
      onChange = fn;
    },
    get lang() {
      return lang;
    },
  };

  applyStatic();
})(window);
