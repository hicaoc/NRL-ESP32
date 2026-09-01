const translations = {
      en: {
        language: 'Language',
        wifiConfig: 'WiFi Config',
        nrlConfig: 'NRL Config',
        batteryConfig: 'Battery',
        serialGpsConfig: 'Serial / GPS Config',
        audioSettings: 'Audio Settings',
        firmwareUpdate: 'Firmware Update',
        homeTitle: 'Configuration',
        homeIntro: 'Choose the settings page to open.',
        backHome: 'Back to home',
        fmoConfig: 'FMO Config',
        configAp: 'Config AP',
        stationIp: 'Station IP',
        bootNotice: 'Hold BOOT for 5 seconds after startup to reset WiFi settings.',
        firmwareVersion: 'Firmware version',
        wifiHeadline: 'WiFi Config',
        wifiIntro: 'Set the WiFi network the device should join.',
        nrlHeadline: 'NRL Config',
        nrlIntro: 'Set server address and radio identity.',
        batteryHeadline: 'Battery',
        batteryIntro: 'Calibrate the on-board battery sense against a multimeter.',
        serialGpsHeadline: 'Serial / GPS Config',
        serialGpsIntro: 'Set UART parameters and GPS module power; live GPS status is shown below.',
        audioHeadline: 'Audio Settings',
        audioIntro: 'Set ES8311 output, volume, DRC, and EQ.',
        wifiAppliedHeadline: 'WiFi Saved',
        wifiAppliedIntro: 'WiFi settings were saved.',
        wifiErrorHeadline: 'WiFi Save Failed',
        wifiErrorIntro: 'Check the WiFi name and password.',
        nrlAppliedHeadline: 'NRL Saved',
        nrlAppliedIntro: 'NRL, server, and audio settings were saved.',
        nrlErrorHeadline: 'NRL Save Failed',
        nrlErrorIntro: 'Check server address, port, channel, callsign, SSID, and audio values.',
        network: 'Network',
        scanIdle: 'Click Scan to find nearby WiFi networks.',
        wifiSsid: 'WiFi SSID',
        wifiPassword: 'WiFi Password',
        wifiAddOrUpdate: 'Add / Update',
        wifiSavedNetworks: 'Saved WiFi Networks',
        wifiSavedHint: 'Up to 5 networks; the displayed order is the connection priority.',
        wifiDelete: 'Delete',
        wifiPriorityUp: 'Up',
        wifiPriorityDown: 'Down',
        wifiNoSaved: 'No saved WiFi networks.',
        dhcp: 'DHCP',
        useDhcp: 'Use DHCP for station IP',
        wifiIp: 'WiFi IP',
        wifiMask: 'WiFi Mask',
        wifiGateway: 'WiFi Gateway',
        wifiDns: 'WiFi DNS',
        nearbyWifi: 'Nearby WiFi',
        selectWifi: 'Select detected WiFi...',
        scan: 'Scan',
        server: 'Server',
        serverHost: 'Server Host / IP',
        serverHostHint: 'Pick from the list or type a custom hostname / IP address',
        serverPort: 'Server Port',
        currentNrlServer: 'Current NRL server',
        serverListLoading: 'Loading server list...',
        serverListLoaded: 'Loaded {count} servers',
        serverListCached: 'Loaded {count} cached servers from flash',
        serverListFailed: 'Could not load the server list; the current server is retained.',
        saveWifi: 'Save WiFi Config',
        radio: 'Radio',
        channel: 'Channel (0-7)',
        callsign: 'Callsign',
        callsignSsid: 'Callsign SSID',
        pttTimeout: 'PTT Timeout (seconds, 5-3600)',
        voicePayloadBytes: 'Voice Packet Size (bytes, 160-500)',
        tailSuppressMs: 'Tail Suppression (ms, 0-5000, 0=off)',
        hpDrive: 'ES8311 HP Drive',
        hpDriveText: 'Enable headphone output drive (REG13 HPSW)',
        audio: 'Audio',
        micVolume: 'Mic Volume (0-255)',
        micPcmGain: 'Mic PCM Gain (0.1-5.0x)',
        lineOutVolume: 'Line Out Volume (0-255)',
        volume: 'Volume',
        audioProcessing: 'Audio Processing',
        adc: 'ADC',
        dac: 'DAC',
        aec: 'AEC / AI',
        micHpfFilter: 'Mute Filter',
        micHpfFilterText: 'Software high-pass filter (~200Hz)',
        drc: 'DRC',
        eq: 'EQ',
        input: 'Input',
        aecLabel: 'Acoustic Echo Cancellation',
        aecText: 'Enable esp-sr echo cancellation on mic uplink',
        aecReferenceSource: 'AEC Reference Source',
        aecRefNetwork: 'Network playback',
        aecRefMic: 'Second microphone',
        aiNoiseLabel: 'AI Noise Reduction',
        aiNoiseText: 'Enable esp-sr NSNET2 noise suppression on mic uplink',
        dmic: 'DMIC',
        dmicText: 'REG14 bit6 select DMIC from MIC1P',
        micInput: 'MIC1P-MIC1N Input',
        micInputText: 'REG14 bit4 line input select',
        adcPgaGain: 'ADC PGA Gain (0-10)',
        adcRampRate: 'ADC VC Ramp Rate (0-15)',
        adcGainScale: 'ADC Gain Scale (0-7)',
        dmicLatchSense: 'DMIC Latch Sense',
        dmicLatchSenseText: 'REG15 bit0 latch on negative edge',
        adcSync: 'ADC Sync',
        adcSyncText: 'REG16 bit5 standard audio clock',
        adcInvert: 'ADC Invert',
        adcInvertText: 'REG16 bit4 ADC polarity inverted',
        adcRamClear: 'ADC RAM Clear',
        adcRamClearText: 'REG16 bit3 clear ADC RAM',
        alcAutomute: 'ALC / Automute',
        alcEnable: 'ALC Enable',
        alcEnableText: 'REG18 bit7 auto level control',
        adcAutomuteEnable: 'ADC Automute Enable',
        adcAutomuteEnableText: 'REG18 bit6 automute',
        alcWindowSize: 'ALC Window Size (0-15)',
        alcMaxLevel: 'ALC Max Level (0-15)',
        alcMinLevel: 'ALC Min Level (0-15)',
        adcAutomuteWindow: 'ADC Automute Window (0-15)',
        adcAutomuteNoiseGate: 'ADC Automute Noise Gate (0-15)',
        adcAutomuteVolume: 'ADC Automute Volume (0-7)',
        hpfEq: 'HPF / EQ',
        adcHpfStage1: 'ADC HPF Stage 1 (0-31)',
        adcHpfStage2: 'ADC HPF Stage 2 (0-31)',
        adcEqBypass: 'ADC EQ Bypass',
        adcEqBypassText: 'REG1C bit6 bypass ADC EQ',
        adcDynamicHpf: 'ADC Dynamic HPF',
        adcDynamicHpfText: 'REG1C bit5 dynamic HPF',
        adcEqCoefficients: 'ADC EQ Coefficients',
        adcEqCoefficientsText: '30-bit unsigned register values for B0/A1/A2/B1/B2',
        adceqB0: 'ADCEQ B0 (REG1D-20)',
        adceqA1: 'ADCEQ A1 (REG21-24)',
        adceqA2: 'ADCEQ A2 (REG25-28)',
        adceqB1: 'ADCEQ B1 (REG29-2C)',
        adceqB2: 'ADCEQ B2 (REG2D-30)',
        output: 'Output',
        drcEnable: 'ES8311 DRC Enable',
        drcEnableText: 'Enable DAC DRC (REG34 bit7)',
        drcWindowSize: 'DRC Window Size (0-15)',
        drcMaxLevel: 'DRC Max Level (0-15)',
        drcMinLevel: 'DRC Min Level (0-15)',
        dacRampRate: 'DAC Ramp Rate (0-15)',
        dacEqBypass: 'DAC EQ Bypass',
        dacEqBypassText: 'Bypass DAC EQ (REG37 bit3)',
        dacEqCoefficients: 'DAC EQ Coefficients',
        dacEqCoefficientsText: '30-bit unsigned register values for B0/B1/A1',
        daceqB0: 'DACEQ B0 (REG38-3B)',
        daceqB1: 'DACEQ B1 (REG3C-3F)',
        daceqA1: 'DACEQ A1 (REG40-43)',
        saveNrl: 'Save NRL Config',
        scanning: 'Scanning...',
        foundPrefix: 'Found ',
        foundSuffix: ' WiFi networks',
        noneFound: 'No WiFi networks found',
        scanFailed: 'Scan failed',
        saveItem: 'Save',
        audioResetDefaults: 'Restore Defaults',
        audioExpertMode: 'Expert Mode',
        saved: 'Saved',
        saveFailed: 'Save failed',
        sent: 'Sent',
        sendFailed: 'Send failed',
        mediaConfig: 'Media / Nanny',
        aprsConfig: 'APRS',
        signalingConfig: 'Signaling / CTCSS',
        serialPorts: 'Serial Ports',
        uart0Reserved: 'UART0 is reserved for system logs and serial AT commands.',
        uart1Sci: 'UART1 - SCI transparent serial',
        uart2Gps: 'UART2 - GPS NMEA',
        serialEnabled: 'Enable this serial port',
        serialRxPin: 'RX GPIO',
        serialTxPin: 'TX GPIO',
        serialBaud: 'Baud rate',
        serialDataBits: 'Data bits',
        serialParity: 'Parity (N/E/O)',
        serialStopBits: 'Stop bits',
        signalingHeadline: 'CW / MDC1200 / DTMF / CTCSS',
        signalingIntro: 'Configure CW and signaling decode sources and voice-tail transmit destinations.',
        ctcssRoutes: 'CTCSS / PL Receive',
        mdcRoutes: 'MDC1200 Routes',
        cwRoutes: 'CW / Morse Decode',
        cwDecodeMicText: 'Decode Morse audio received from microphone/radio',
        cwDecodeNrlText: 'Decode Morse audio in NRL downlink voice',
        dtmfRoutes: 'DTMF Routes',
        decodeMic: 'Decode from MIC',
        decodeNrl: 'Decode from NRL',
        ctcssDecodeMicText: 'Detect the standard CTCSS frequency from microphone/radio audio',
        ctcssDecodeNrlText: 'Detect the standard CTCSS frequency from NRL downlink audio',
        encodeNrl: 'Send to NRL',
        encodeSpeaker: 'Send to speaker',
        mdcDecodeMicText: 'Decode MDC1200 received from microphone/radio',
        mdcDecodeNrlText: 'Decode MDC1200 in network downlink audio',
        mdcEncodeNrlText: 'Append MDC1200 after local PTT release',
        mdcEncodeSpeakerText: 'Append MDC1200 after NRL voice ends',
        dtmfDecodeMicText: 'Decode DTMF received from microphone/radio',
        dtmfDecodeNrlText: 'Decode DTMF in network downlink audio',
        dtmfEncodeNrlText: 'Append DTMF after local PTT release',
        dtmfEncodeSpeakerText: 'Append DTMF after NRL voice ends',
        mdcOpcode: 'Opcode (hex)',
        mdcArgument: 'Argument (hex)',
        mdcUnitId: 'MDC ID (4 hex digits)',
        dtmfId: 'DTMF ID',
        aprsHeadline: 'APRS',
        aprsIntro: 'GPS beacons to APRS-IS and AFSK over the radio; stations heard are listed below.',
        aprsSwitches: 'APRS',
        aprsEnabled: 'APRS',
        aprsEnabledText: 'Master switch for the APRS transceiver',
        aprsNet: 'APRS-IS Network',
        aprsNetText: 'Send beacons to and hear stations from the APRS-IS server',
        aprsRfTx: 'RF Transmit (AFSK)',
        aprsRfTxText: 'Modulate beacons out through the speaker into the radio',
        aprsRfRx: 'RF Receive (AFSK)',
        aprsRfRxText: 'Demodulate APRS audio heard on the microphone/radio',
        aprsAuto: 'Auto interval',
        aprsAutoText: 'With a GPS fix, beacon faster while moving (30 s at 60+ km/h, after a 300 m jump, or 10 s after a significant turn); the interval below is the ceiling when parked',
        aprsFixed: 'Fixed beacon without GPS',
        aprsFixedText: 'When GPS has no fix, allow periodic beacons using the configured default position',
        gpsPower: 'GPS Power',
        gpsPowerText: 'Powers the GPS module; when off, APRS has no fix and uses the default position per the fixed-beacon rule',
        aprsNrlTx: 'NRL Network Transmit (AFSK)',
        aprsNrlTxText: 'Modulate beacons/messages out through the NRL network voice uplink',
        aprsNrlRx: 'NRL Network Receive (AFSK)',
        aprsNrlRxText: 'Demodulate APRS audio arriving over the NRL network voice downlink',
                aprsFwdRfIs: 'Gateway RF \u2192 APRS-IS',
                aprsFwdRfIsText: 'iGate upload: relay packets heard on the microphone/radio to APRS-IS',
                aprsFwdIsRf: 'Gateway APRS-IS \u2192 RF',
                aprsFwdIsRfText: 'Relay packets from APRS-IS out as AFSK through the radio (needs RF Transmit)',
                aprsFwdNrlIs: 'Gateway NRL \u2192 APRS-IS',
                aprsFwdNrlIsText: 'Relay packets heard on the NRL network downlink to APRS-IS',
                aprsFwdIsNrl: 'Gateway APRS-IS \u2192 NRL',
                aprsFwdIsNrlText: 'Relay packets from APRS-IS out as AFSK through the NRL network (needs NRL Transmit)',
                aprsFwdRfNrl: 'Gateway RF \u2192 NRL',
                aprsFwdRfNrlText: 'Relay packets heard on the microphone/radio out through the NRL network (needs NRL Transmit)',
                aprsFwdNrlRf: 'Gateway NRL \u2192 RF',
                aprsFwdNrlRfText: 'Relay packets heard on the NRL network out as AFSK through the radio (needs RF Transmit)',
        aprsMessage: 'Manual APRS Message',
        aprsMessageHint: 'Sent over APRS-IS when linked, and as AFSK on every enabled audio path.',
        aprsMsgDest: 'Addressee callsign (A-Z 0-9 -, e.g. BI4UMD-9)',
        aprsMsgText: 'Message text (up to 67 bytes)',
        aprsMsgSend: 'Send message',
        aprsMsgSent: 'Sent',
        gpsLive: 'GPS Live Information',
        gpsFixStatus: 'Fix status',
        gpsSatellites: 'Satellites',
        gpsSatellitesUsed: 'Satellites used (GGA)',
        gpsSatellitesVisible: 'Satellites visible (GSV)',
        gpsGsvUpdated: 'GSV updated',
        gpsSatelliteSignals: 'Satellite signals (constellation/PRN, elevation, azimuth, SNR)',
        gpsNoSatelliteSignals: 'No GSV satellite signal data',
        gpsLatitude: 'Latitude',
        gpsLongitude: 'Longitude',
        gpsSpeed: 'Speed',
        gpsCourse: 'Course',
        gpsAltitude: 'Altitude',
        gpsAccuracy: 'Fix quality / HDOP',
        gpsUpdated: 'NMEA updated',
        gpsFixed: 'Fixed',
        gpsAcquiring: 'Acquiring',
        aprsSettings: 'APRS Settings',
        aprsSettingsHint: 'Callsign comes from the Radio page; passcode is derived automatically.',
        aprsServer: 'APRS-IS Server',
        aprsPort: 'Port',
        aprsSsid: 'SSID (0-15)',
        aprsSymbol: 'Symbol (table+code)',
        aprsInterval: 'Beacon interval (s, 10-3600)',
        aprsPath: 'RF path',
        aprsLat: 'Default latitude (WGS-84 ddmm.mmmm or decimal degrees; N added automatically)',
        aprsLon: 'Default longitude (WGS-84 dddmm.mmmm or decimal degrees; E added automatically)',
        aprsComment: 'Beacon comment (up to 219 UTF-8 bytes)',
        aprsBeaconNow: 'Beacon now',
        aprsStations: 'Stations Heard',
        aprsStnCall: 'Callsign',
        aprsStnPos: 'Lat / Lon',
        aprsStnAlt: 'Alt',
        aprsStnDist: 'Distance',
        aprsStnSpeed: 'Speed',
        aprsStnCourse: 'Course',
        aprsStnAge: 'Heard',
        aprsStnComment: 'Comment',
        aprsStnEmpty: 'No stations heard yet.',
        mediaHeadline: 'Media / Nanny',
        mediaIntro: 'Configure playback target, nanny beacon, net radio, and SMB network share.',
        playback: 'Playback',
        musicTarget: 'Playback Target',
        musicTargetHint: 'One shared setting for everything the player outputs: music, nanny beacon, and net radio.',
        targetLocal: 'Local speaker',
        targetNet: 'NRL network',
        targetBoth: 'Local + network',
        espnowLabel: 'ESP-NOW Intercom',
        espnowText: 'Off-grid voice link between nearby devices',
        espnowRxLabel: 'ESP-NOW Receive',
        espnowRxText: 'Hear intercom voice even while TX stays off',
        espnowCodec: 'ESP-NOW Voice Codec (TX)',
        nannyBeacon: 'Nanny Beacon',
        nannyBeaconHint: 'Play a beacon file every N minutes through the configured target.',
        beaconPath: 'Beacon file path',
        beaconInterval: 'Interval (minutes, 1-1440)',
        beaconEnabledText: 'Beacon armed (uncheck and Save to disable)',
        netRadio: 'Net Radio',
        radioUrl: 'Stream URL (http:// or https://)',
        radioPlay: 'Play',
        radioStop: 'Stop',
        radioFavs: 'Favorite stations',
        radioFavName: 'Station name',
        radioFavUrl: 'Stream URL (http:// or https://)',
        radioFavAdd: 'Add favorite',
        radioFavDelete: 'Delete',
        radioFavEmpty: 'No favorites yet. Add a station below.',
        smbShare: 'Network Share (SMB)',
        smbServer: 'Server (NAS / PC)',
        smbShareName: 'Share name',
        smbUser: 'Username (empty = guest)',
        smbPassword: 'Password',
        smbClear: 'Clear',
        musicLibrary: 'Music Playback',
        musicPrev: 'Previous',
        musicStop: 'Stop',
        musicNext: 'Next',
        musicRepeatList: 'Repeat list',
        musicRepeatOne: 'Repeat one',
        musicUp: 'Up',
        musicRefresh: 'Rescan',
        musicLoading: 'Loading...',
        musicPagePrev: 'Previous page',
        musicPageNext: 'Next page',
        musicSourcesHint: 'Mounted SMB, TF/SD and USB sources appear here automatically. Tap a folder to browse and a track to play.',
        musicNoSources: 'No storage source is mounted.',
        musicNoEntries: 'No supported tracks or subfolders in this directory.',
        musicScanning: 'Scanning directory... Large SMB folders may take a while.',
        musicScanFailed: 'Directory scan timed out or failed. Tap Rescan to retry.',
        musicStopped: 'Stopped',
        musicActionFailed: 'Playback action failed',
        musicFavoriteNeedsSd: 'Favorites require a mounted TF/SD card',
        musicOutput: 'Music Output Device',
        outputSpk: 'Onboard speaker',
        outputBt: 'Bluetooth headset (A2DP)',
        voiceCodec: 'NRL Voice Codec (TX)',
        codecG711: 'G.711 8 kHz (compatible)',
        codecOpus: 'Opus 16 kHz wideband',
        voiceLink: 'Voice Link',
        pttMode: 'PTT Mode',
        pttModeNrl: 'NRL network PTT',
        pttModeEspnow: 'ESP-NOW PTT',
        pttModeFmo: 'FMO PTT',
        aiAssistant: 'AI Assistant (xiaozhi)',
        aiUrl: 'Server URL (ws:// or wss://)',
        aiToken: 'Access Token',
        aiEnabledText: 'Enable the assistant connection'
      },
      zh: {
        language: '语言',
        wifiConfig: 'WiFi配置',
        nrlConfig: 'NRL配置',
        batteryConfig: '电池',
        serialGpsConfig: '串口 / GPS 配置',
        audioSettings: '音频设置',
        firmwareUpdate: '固件升级',
        homeTitle: '配置导航',
        homeIntro: '选择要打开的管理页面。',
        backHome: '返回导航首页',
        fmoConfig: 'FMO配置',
        configAp: '配置热点',
        stationIp: '联网地址',
        bootNotice: '开机后按住 BOOT 5 秒可重置 WiFi 和服务器设置。',
        firmwareVersion: '固件版本',
        wifiHeadline: 'WiFi配置',
        wifiIntro: '设置设备要连接的 WiFi 网络。',
        nrlHeadline: 'NRL配置',
        nrlIntro: '设置服务器地址和电台身份。',
        batteryHeadline: '电池',
        batteryIntro: '使用万用表校准板载电池电压采样。',
        serialGpsHeadline: '串口 / GPS 配置',
        serialGpsIntro: '设置串口参数和 GPS 电源，下方显示 GPS 实时状态。',
        audioHeadline: '音频设置',
        audioIntro: '设置 ES8311 输出、音量、DRC 和 EQ。',
        wifiAppliedHeadline: 'WiFi已保存',
        wifiAppliedIntro: 'WiFi 设置已保存。',
        wifiErrorHeadline: 'WiFi保存失败',
        wifiErrorIntro: '请检查 WiFi 名称和密码。',
        nrlAppliedHeadline: 'NRL已保存',
        nrlAppliedIntro: 'NRL、服务器和音频设置已保存。',
        nrlErrorHeadline: 'NRL保存失败',
        nrlErrorIntro: '请检查服务器地址、端口、信道、呼号、SSID 和音频数值。',
        network: '网络',
        scanIdle: '点击扫描查找附近 WiFi。',
        wifiSsid: 'WiFi名称',
        wifiPassword: 'WiFi密码',
        wifiAddOrUpdate: '新增 / 更新',
        wifiSavedNetworks: '已保存的 WiFi 热点',
        wifiSavedHint: '最多保存 5 个热点，显示顺序就是连接优先级。',
        wifiDelete: '删除',
        wifiPriorityUp: '上移',
        wifiPriorityDown: '下移',
        wifiNoSaved: '尚未保存 WiFi 热点。',
        dhcp: 'DHCP',
        useDhcp: '联网地址使用 DHCP 自动获取',
        wifiIp: 'WiFi IP 地址',
        wifiMask: 'WiFi 子网掩码',
        wifiGateway: 'WiFi 网关',
        wifiDns: 'WiFi DNS',
        nearbyWifi: '附近WiFi',
        selectWifi: '选择扫描到的 WiFi...',
        scan: '扫描',
        server: '服务器',
        serverHost: '服务器地址 / IP',
        serverHostHint: '可从列表选择，也可直接输入自定义主机名 / IP 地址',
        serverPort: '服务器端口',
        currentNrlServer: '当前 NRL 服务器',
        serverListLoading: '正在加载服务器列表...',
        serverListLoaded: '已加载 {count} 个服务器',
        serverListCached: '已从 Flash 加载 {count} 个缓存服务器',
        serverListFailed: '服务器列表加载失败，已保留当前服务器。',
        saveWifi: '保存WiFi配置',
        radio: '电台',
        channel: '信道 (0-7)',
        callsign: '呼号',
        callsignSsid: '呼号SSID',
        pttTimeout: 'PTT 超时 (秒, 5-3600)',
        voicePayloadBytes: '语音包大小 (字节, 160-500)',
        tailSuppressMs: '尾音消除 (毫秒, 0-5000, 0=关闭)',
        hpDrive: 'ES8311耳机驱动',
        hpDriveText: '启用耳机输出驱动 (REG13 HPSW)',
        audio: '音频',
        micVolume: '麦克风音量 (0-255)',
        micPcmGain: '麦克风 PCM 增益 (0.1-5.0 倍)',
        lineOutVolume: '线路输出音量 (0-255)',
        volume: '音量',
        audioProcessing: '音频处理',
        adc: 'ADC 输入',
        dac: 'DAC 输出',
        aec: 'AEC / AI 降噪',
        micHpfFilter: '哑音过滤',
        micHpfFilterText: '软件高通滤波 (~200Hz)',
        drc: 'DRC 动态范围控制',
        eq: 'EQ 均衡',
        input: '输入',
        aecLabel: '声学回声消除',
        aecText: '启用 esp-sr 麦克风上行回声消除',
        aecReferenceSource: 'AEC 参照来源',
        aecRefNetwork: '网络下行语音',
        aecRefMic: '第二路麦克风',
        aiNoiseLabel: 'AI降噪',
        aiNoiseText: '启用 esp-sr NSNET2 麦克风上行降噪',
        dmic: '数字麦克风',
        dmicText: 'REG14 bit6 从 MIC1P 选择 DMIC',
        micInput: 'MIC1P-MIC1N 输入',
        micInputText: 'REG14 bit4 选择线路输入',
        adcPgaGain: 'ADC PGA 增益 (0-10)',
        adcRampRate: 'ADC 音量爬升速率 (0-15)',
        adcGainScale: 'ADC 增益缩放 (0-7)',
        dmicLatchSense: 'DMIC 锁存边沿',
        dmicLatchSenseText: 'REG15 bit0 负边沿锁存',
        adcSync: 'ADC 同步',
        adcSyncText: 'REG16 bit5 标准音频时钟',
        adcInvert: 'ADC 反相',
        adcInvertText: 'REG16 bit4 ADC 极性反相',
        adcRamClear: '清除 ADC RAM',
        adcRamClearText: 'REG16 bit3 清除 ADC RAM',
        alcAutomute: 'ALC / 自动静音',
        alcEnable: '启用 ALC',
        alcEnableText: 'REG18 bit7 自动电平控制',
        adcAutomuteEnable: '启用 ADC 自动静音',
        adcAutomuteEnableText: 'REG18 bit6 自动静音',
        alcWindowSize: 'ALC 窗口大小 (0-15)',
        alcMaxLevel: 'ALC 最大电平 (0-15)',
        alcMinLevel: 'ALC 最小电平 (0-15)',
        adcAutomuteWindow: 'ADC 自动静音窗口 (0-15)',
        adcAutomuteNoiseGate: 'ADC 自动静音噪声门限 (0-15)',
        adcAutomuteVolume: 'ADC 自动静音音量 (0-7)',
        hpfEq: '高通滤波 / EQ',
        adcHpfStage1: 'ADC 高通滤波 1 级 (0-31)',
        adcHpfStage2: 'ADC 高通滤波 2 级 (0-31)',
        adcEqBypass: '旁路 ADC EQ',
        adcEqBypassText: 'REG1C bit6 旁路 ADC EQ',
        adcDynamicHpf: 'ADC 动态高通滤波',
        adcDynamicHpfText: 'REG1C bit5 动态高通滤波',
        adcEqCoefficients: 'ADC EQ 系数',
        adcEqCoefficientsText: 'B0/A1/A2/B1/B2 的 30 位无符号寄存器值',
        adceqB0: 'ADCEQ B0 (REG1D-20)',
        adceqA1: 'ADCEQ A1 (REG21-24)',
        adceqA2: 'ADCEQ A2 (REG25-28)',
        adceqB1: 'ADCEQ B1 (REG29-2C)',
        adceqB2: 'ADCEQ B2 (REG2D-30)',
        output: '输出',
        drcEnable: '启用 ES8311 DRC',
        drcEnableText: '启用 DAC DRC (REG34 bit7)',
        drcWindowSize: 'DRC 窗口大小 (0-15)',
        drcMaxLevel: 'DRC 最大电平 (0-15)',
        drcMinLevel: 'DRC 最小电平 (0-15)',
        dacRampRate: 'DAC 爬升速率 (0-15)',
        dacEqBypass: '旁路 DAC EQ',
        dacEqBypassText: '旁路 DAC EQ (REG37 bit3)',
        dacEqCoefficients: 'DAC EQ 系数',
        dacEqCoefficientsText: 'B0/B1/A1 的 30 位无符号寄存器值',
        daceqB0: 'DACEQ B0 (REG38-3B)',
        daceqB1: 'DACEQ B1 (REG3C-3F)',
        daceqA1: 'DACEQ A1 (REG40-43)',
        saveNrl: '保存NRL配置',
        scanning: '正在扫描...',
        foundPrefix: '找到 ',
        foundSuffix: ' 个 WiFi 网络',
        noneFound: '没有找到 WiFi 网络',
        scanFailed: '扫描失败',
        saveItem: '保存',
        saved: '已保存',
        saveFailed: '保存失败',
        sent: '已发送',
        sendFailed: '发送失败',
        battery: '电池',
        batteryHint: '使用万用表校准板载电池电压采样。',
        batteryRaw: '原始读数 (mV)',
        batteryCalibrated: '校准后 (mV)',
        batteryActual: '万用表测量值 (mV) -- 点击校准按当前读数拟合系数',
        batteryCalibrate: '校准',
        batteryScale: '校准系数 (500-2000，1000 表示不修正)',
        mediaConfig: '媒体/保姆',
        aprsConfig: 'APRS',
        signalingConfig: '信令 / CTCSS',
        serialPorts: '串口配置',
        uart0Reserved: 'UART0 固定保留给系统日志和串口 AT 指令。',
        uart1Sci: 'UART1 - SCI 透明串口',
        uart2Gps: 'UART2 - GPS NMEA',
        serialEnabled: '启用此串口',
        serialRxPin: 'RX GPIO',
        serialTxPin: 'TX GPIO',
        serialBaud: '波特率',
        serialDataBits: '数据位',
        serialParity: '校验位 (N/E/O)',
        serialStopBits: '停止位',
        signalingHeadline: 'CW / MDC1200 / DTMF / CTCSS 信令',
        signalingIntro: '配置 CW 与其他信令的 MIC、NRL 音频解码来源，以及语音结束后的信令发送目标。',
        ctcssRoutes: 'CTCSS / 模拟亚音接收',
        mdcRoutes: 'MDC1200 收发',
        cwRoutes: 'CW / 莫尔斯解码',
        cwDecodeMicText: '解码麦克风或电台收到的 CW 莫尔斯语音',
        cwDecodeNrlText: '解码 NRL 网络下行语音中的 CW 莫尔斯信号',
        dtmfRoutes: 'DTMF 收发',
        decodeMic: '从 MIC 解码',
        decodeNrl: '从 NRL 解码',
        ctcssDecodeMicText: '识别麦克风或电台音频中的标准 CTCSS 亚音频率',
        ctcssDecodeNrlText: '识别 NRL 网络下行音频中的标准 CTCSS 亚音频率',
        encodeNrl: '发送到 NRL',
        encodeSpeaker: '发送到喇叭',
        mdcDecodeMicText: '解码麦克风或电台收到的 MDC1200',
        mdcDecodeNrlText: '解码 NRL 网络下行音频中的 MDC1200',
        mdcEncodeNrlText: '本地 PTT 松开后追加 MDC1200',
        mdcEncodeSpeakerText: 'NRL 网络语音结束后向喇叭追加 MDC1200',
        dtmfDecodeMicText: '解码麦克风或电台收到的 DTMF',
        dtmfDecodeNrlText: '解码 NRL 网络下行音频中的 DTMF',
        dtmfEncodeNrlText: '本地 PTT 松开后追加 DTMF',
        dtmfEncodeSpeakerText: 'NRL 网络语音结束后向喇叭追加 DTMF',
        mdcOpcode: '操作码（十六进制）',
        mdcArgument: '参数（十六进制）',
        mdcUnitId: 'MDC ID（4位十六进制）',
        dtmfId: 'DTMF ID',
        aprsHeadline: 'APRS 收发',
        aprsIntro: 'GPS 位置上报到 APRS-IS 服务器，或经电台 AFSK 收发；下方列出收到的台站。',
        aprsSwitches: 'APRS',
        aprsEnabled: 'APRS 总开关',
        aprsEnabledText: 'APRS 收发功能总开关',
        aprsNet: 'APRS-IS 网络',
        aprsNetText: '向 APRS-IS 服务器上报位置并接收周边台站',
        aprsRfTx: '射频发射 (AFSK)',
        aprsRfTxText: '信标经扬声器调制后由电台发射',
        aprsRfRx: '射频接收 (AFSK)',
        aprsRfRxText: '解调麦克风/电台收到的 APRS 音频',
        aprsAuto: '自动发送间隔',
        aprsAutoText: 'GPS 定位有效时按移动状态自动加快信标（60 km/h 以上 30 秒一次、位移超 300 米或明显转弯后最快 10 秒补发）；静止时使用下方设定的间隔',
        aprsFixed: '无 GPS 固定信标',
        aprsFixedText: 'GPS 没有定位时，允许使用下方默认坐标周期发送信标',
        gpsPower: 'GPS 电源',
        gpsPowerText: '关闭后 GPS 模块断电，APRS 无定位时按规则使用默认位置',
        aprsNrlTx: 'NRL 网络发送 (AFSK)',
        aprsNrlTxText: '将信标/消息调制成 AFSK 音频，经 NRL 网络语音上行发送',
        aprsNrlRx: 'NRL 网络接收 (AFSK)',
        aprsNrlRxText: '解调从 NRL 网络语音下行收到的 APRS 音频',
                aprsFwdRfIs: '网关 RF \u2192 APRS-IS',
                aprsFwdRfIsText: 'iGate 上行：把麦克风/电台收到的数据包转发到 APRS-IS',
                aprsFwdIsRf: '网关 APRS-IS \u2192 RF',
                aprsFwdIsRfText: '把 APRS-IS 的数据包以 AFSK 经电台发出（需开启 RF 发送）',
                aprsFwdNrlIs: '网关 NRL \u2192 APRS-IS',
                aprsFwdNrlIsText: '把 NRL 网络下行收到的数据包转发到 APRS-IS',
                aprsFwdIsNrl: '网关 APRS-IS \u2192 NRL',
                aprsFwdIsNrlText: '把 APRS-IS 的数据包以 AFSK 经 NRL 网络发出（需开启 NRL 发送）',
                aprsFwdRfNrl: '网关 RF \u2192 NRL',
                aprsFwdRfNrlText: '把麦克风/电台收到的数据包经 NRL 网络发出（需开启 NRL 发送）',
                aprsFwdNrlRf: '网关 NRL \u2192 RF',
                aprsFwdNrlRfText: '把 NRL 网络收到的数据包以 AFSK 经电台发出（需开启 RF 发送）',
        aprsMessage: '手工发送 APRS 消息',
        aprsMessageHint: '已连接时经 APRS-IS 发送，同时以 AFSK 音频经所有已启用的通道发送。',
        aprsMsgDest: '收信呼号（A-Z 0-9 -，例如 BI4UMD-9）',
        aprsMsgText: '消息内容（最多 67 字节）',
        aprsMsgSend: '发送消息',
        aprsMsgSent: '已发送',
        gpsLive: 'GPS 实时信息',
        gpsFixStatus: '定位状态',
        gpsSatellites: '卫星数量',
        gpsSatellitesUsed: '参与定位卫星数（GGA）',
        gpsSatellitesVisible: '可见卫星数（GSV）',
        gpsGsvUpdated: 'GSV 更新时间',
        gpsSatelliteSignals: '卫星信号（星座/编号、仰角、方位角、信噪比）',
        gpsNoSatelliteSignals: '暂无 GSV 卫星信号数据',
        gpsLatitude: '纬度',
        gpsLongitude: '经度',
        gpsSpeed: '速度',
        gpsCourse: '航向',
        gpsAltitude: '海拔',
        gpsAccuracy: '定位质量 / HDOP',
        gpsUpdated: 'NMEA 更新时间',
        gpsFixed: '已定位',
        gpsAcquiring: '定位中',
        aprsSettings: 'APRS 设置',
        aprsSettingsHint: '呼号使用"电台"页配置；APRS-IS 验证码自动计算。',
        aprsServer: 'APRS-IS 服务器',
        aprsPort: '端口',
        aprsSsid: 'SSID (0-15)',
        aprsSymbol: '符号 (表+代码)',
        aprsInterval: '信标间隔 (秒, 10-3600)',
        aprsPath: '射频路径',
        aprsLat: '默认纬度 (WGS-84 ddmm.mmmm 或十进制度，自动补N)',
        aprsLon: '默认经度 (WGS-84 dddmm.mmmm 或十进制度，自动补E)',
        aprsComment: '信标注释（最多 219 个 UTF-8 字节）',
        aprsBeaconNow: '立即发信标',
        aprsStations: '收到的台站',
        aprsStnCall: '呼号',
        aprsStnPos: '纬度/经度',
        aprsStnAlt: '高度',
        aprsStnDist: '距离',
        aprsStnSpeed: '速度',
        aprsStnCourse: '航向',
        aprsStnAge: '最后收到',
        aprsStnComment: '注释',
        aprsStnEmpty: '尚未收到台站。',
        mediaHeadline: '媒体/保姆',
        mediaIntro: '配置播放目标、保姆信标、网络收音机和 SMB 网络共享。',
        playback: '播放',
        musicTarget: '播放目标',
        musicTargetHint: '全局共用设置：音乐、保姆信标、网络收音机的播放都按它路由。',
        targetLocal: '本地扬声器',
        targetNet: 'NRL 网络',
        targetBoth: '本地 + 网络',
        espnowLabel: 'ESP-NOW 对讲',
        espnowText: '附近设备间脱网语音互通',
        espnowRxLabel: 'ESP-NOW 接收',
        espnowRxText: '发射关闭时也能听到对讲呼入',
        espnowCodec: 'ESP-NOW 语音编码 (发射)',
        nannyBeacon: '保姆信标',
        nannyBeaconHint: '每隔 N 分钟按播放目标播放一段信标音频。',
        beaconPath: '信标文件路径',
        beaconInterval: '间隔 (分钟, 1-1440)',
        beaconEnabledText: '信标已启用 (取消勾选并保存即停用)',
        netRadio: '网络收音机',
        radioUrl: '直播流地址 (http:// 或 https://)',
        radioPlay: '播放',
        radioStop: '停止',
        radioFavs: '收藏电台',
        radioFavName: '电台名称',
        radioFavUrl: '直播流地址 (http:// 或 https://)',
        radioFavAdd: '添加收藏',
        radioFavDelete: '删除',
        radioFavEmpty: '还没有收藏电台，在下方添加。',
        smbShare: '网络共享 (SMB)',
        smbServer: '服务器 (NAS / 电脑)',
        smbShareName: '共享名',
        smbUser: '用户名 (留空为来宾)',
        smbPassword: '密码',
        smbClear: '清除',
        musicLibrary: '音乐播放管理',
        musicPrev: '上一首',
        musicStop: '停止',
        musicNext: '下一首',
        musicRepeatList: '列表循环',
        musicRepeatOne: '单曲循环',
        musicUp: '返回上级',
        musicRefresh: '重新扫描',
        musicLoading: '正在载入...',
        musicPagePrev: '上一页',
        musicPageNext: '下一页',
        musicSourcesHint: '已挂载的 SMB、TF/SD 和 USB 会自动显示；点击文件夹进入，点击歌曲播放。',
        musicNoSources: '当前没有已挂载的存储来源。',
        musicNoEntries: '当前目录没有支持的歌曲或子目录。',
        musicScanning: '正在扫描目录，大型 SMB 目录可能需要一些时间…',
        musicScanFailed: '目录扫描超时或失败，请点击“重新扫描”重试。',
        musicStopped: '已停止',
        musicActionFailed: '播放操作失败',
        musicFavoriteNeedsSd: '收藏功能需要插入 TF/SD 卡',
        musicOutput: '音乐输出设备',
        outputSpk: '板载扬声器',
        outputBt: '蓝牙耳机 (A2DP)',
        voiceCodec: 'NRL 语音编码 (发送)',
        codecG711: 'G.711 8kHz (兼容)',
        codecOpus: 'Opus 16kHz 宽带',
        voiceLink: '语音链路',
        pttMode: 'PTT 模式',
        pttModeNrl: 'NRL 网络 PTT',
        pttModeEspnow: 'ESP-NOW PTT',
        pttModeFmo: 'FMO PTT',
        aiAssistant: 'AI 助手 (小智)',
        aiUrl: '服务器地址 (ws:// 或 wss://)',
        aiToken: '访问令牌 (Token)',
        aiEnabledText: '启用助手连接'
      }
};
Object.assign(translations.en, {sensorStatus: 'Sensors', radioConfig: 'RF Module'});
Object.assign(translations.zh, {sensorStatus: '传感器', radioConfig: '射频模块'});

    function currentLang() {
      const saved = localStorage.getItem('nrl_lang');
      if (saved === 'zh' || saved === 'en') return saved;
      return navigator.language && navigator.language.toLowerCase().startsWith('zh') ? 'zh' : 'en';
    }

    function t(key) {
      const lang = currentLang();
      return (translations[lang] && translations[lang][key]) || translations.en[key] || key;
    }

    function applyLanguage(lang) {
      localStorage.setItem('nrl_lang', lang);
      document.documentElement.lang = lang === 'zh' ? 'zh-CN' : 'en';
      document.querySelectorAll('input[name="lang"]').forEach((r) => {
        r.checked = (r.value === lang);
      });
      document.querySelectorAll('[data-i18n]').forEach((el) => {
        const key = el.getAttribute('data-i18n');
        if (translations[lang] && translations[lang][key]) {
          el.textContent = translations[lang][key];
        }
      });
      document.querySelectorAll('[data-i18n-title]').forEach((el) => {
        const key = el.getAttribute('data-i18n-title');
        if (translations[lang] && translations[lang][key]) {
          el.title = translations[lang][key];
        }
      });
    }

    function syncDhcpFields() {
      const dhcp = document.getElementById('wifi-dhcp-enabled');
      if (!dhcp) return;
      document.querySelectorAll('.wifi-static-field').forEach((input) => {
        input.disabled = dhcp.checked;
      });
    }

    function toggleAudioExpert(enabled) {
      document.querySelectorAll('[data-audio-expert]').forEach((panel) => {
        panel.hidden = !enabled;
      });
    }

    function clampEqValue(value) {
      const text = String(value).trim();
      const n = text.toLowerCase().startsWith('0x') ? Number.parseInt(text.slice(2), 16) : Number(text);
      if (!Number.isFinite(n)) return 0;
      return Math.max(0, Math.min(1073741823, Math.round(n)));
    }

    function formatEqHex(value) {
      return '0x' + clampEqValue(value).toString(16).toUpperCase().padStart(8, '0');
    }

    function syncEqValue(slider) {
      const box = slider.parentElement.querySelector('.eq-value');
      if (box) box.value = formatEqHex(slider.value);
    }

    function syncEqSlider(box) {
      const slider = box.parentElement.querySelector('.eq-slider');
      const value = clampEqValue(box.value);
      box.value = formatEqHex(value);
      if (slider) slider.value = value;
    }

    // POST a form as application/x-www-form-urlencoded and return the parsed
    // JSON reply: { ok: bool, fields: { name: stored_value, ... } }
    // Server-side parseFormBody in wifi_config_portal.cpp only handles
    // urlencoded; using FormData would send multipart/form-data and every
    // field would be silently ignored. The server echoes back the just-saved
    // value of every submitted field so the caller can refresh inputs with
    // on-device truth without a page reload.
    function postForm(form) {
      if (!form) return Promise.resolve({ ok: false });
      const body = new URLSearchParams();
      // FormData walks the form's successful controls (skips unchecked
      // checkboxes, etc.) -- exactly the set we want urlencoded.
      for (const [k, v] of new FormData(form)) {
        body.append(k, v);
      }
      return fetch(form.action || window.location.href, {
        method: form.method ? form.method.toUpperCase() : 'POST',
        body,
        cache: 'no-store',
        credentials: 'same-origin',
        referrerPolicy: 'no-referrer',
      }).then((r) => r.json().then((data) => Object.assign({ ok: r.ok }, data)))
        .catch(() => ({ ok: false }));
    }

    function applyEchoFields(fields) {
      if (!fields) return;
      Object.keys(fields).forEach((name) => {
        const value = fields[name];
        const selector = 'input[name="' + name + '"], select[name="' + name + '"]';
        document.querySelectorAll(selector).forEach((input) => {
          if (input.type === 'checkbox') {
            input.checked = (value === '1' || value === 'true' || value === 'on');
          } else if (input !== document.activeElement) {
            // Don't fight the user mid-edit. Active field keeps their text;
            // they'll see the canonical value on next interaction.
            input.value = value;
          }
          if (input.classList.contains('auto-slider')) {
            const box = input.parentElement.querySelector('.eq-value');
            if (box) box.value = input.value;
          } else if (input.classList.contains('eq-slider')) {
            syncEqValue(input);
          }
        });
      });
      syncDhcpFields();
    }

    function postAndApply(form) {
      return postForm(form).then((reply) => {
        if (reply && reply.fields) applyEchoFields(reply.fields);
        // /save_media replies carry the favorite-station list so the page
        // reflects adds/deletes/tunes without a reload.
        if (reply && reply.favs) renderRadioFavs(reply.favs, reply.fav_cur);
        return reply;
      });
    }

    // Favorite net-radio stations. The list is rendered client-side from
    // window.RADIO_FAVS (initial page data) and from the JSON echoed by every
    // /save_media POST; rows post their action straight back to /save_media.
    function renderRadioFavs(favs, current) {
      const list = document.getElementById('radio-fav-list');
      if (!list || !Array.isArray(favs)) return;
      list.innerHTML = '';
      if (!favs.length) {
        const empty = document.createElement('span');
        empty.className = 'hint';
        empty.setAttribute('data-i18n', 'radioFavEmpty');
        empty.textContent = t('radioFavEmpty');
        list.appendChild(empty);
        return;
      }
      favs.forEach((fav, idx) => {
        const row = document.createElement('div');
        row.className = 'fav-row' + (idx === current ? ' current' : '');
        const text = document.createElement('div');
        text.className = 'fav-text';
        const name = document.createElement('span');
        name.className = 'fav-name';
        name.textContent = (idx === current ? '▶ ' : '') + (fav.name || fav.url);
        const url = document.createElement('span');
        url.className = 'fav-url mono';
        url.textContent = fav.url;
        text.appendChild(name);
        text.appendChild(url);
        row.appendChild(text);
        const play = document.createElement('button');
        play.type = 'button';
        play.className = 'btn-small';
        play.textContent = t('radioPlay');
        play.setAttribute('data-i18n', 'radioPlay');
        play.addEventListener('click', () => radioFavAction(play, 'fav_play', idx));
        const del = document.createElement('button');
        del.type = 'button';
        del.className = 'secondary btn-small';
        del.textContent = t('radioFavDelete');
        del.setAttribute('data-i18n', 'radioFavDelete');
        del.addEventListener('click', () => radioFavAction(del, 'fav_del', idx));
        row.appendChild(play);
        row.appendChild(del);
        list.appendChild(row);
      });
    }

    function radioFavAction(button, action, index) {
      button.disabled = true;
      const body = new URLSearchParams();
      body.append(action, String(index));
      fetch('/save_media', {
        method: 'POST',
        body,
        cache: 'no-store',
        credentials: 'same-origin',
        referrerPolicy: 'no-referrer',
      }).then((r) => r.json().then((data) => Object.assign({ ok: r.ok }, data)))
        .catch(() => ({ ok: false }))
        .then((reply) => {
          button.disabled = false;
          if (reply && reply.fields) applyEchoFields(reply.fields);
          if (reply && reply.favs) renderRadioFavs(reply.favs, reply.fav_cur);
        });
    }

    function addRadioFav(button) {
      const form = button ? button.form : null;
      if (!form) return;
      const flag = document.createElement('input');
      flag.type = 'hidden';
      flag.name = 'fav_add';
      flag.value = '1';
      form.appendChild(flag);
      button.disabled = true;
      postAndApply(form).then((reply) => {
        flag.remove();
        button.disabled = false;
        flashButtonFeedback(button, reply && reply.ok);
        if (reply && reply.ok) {
          form.querySelectorAll('input[name="fav_name"], input[name="fav_url"]').forEach((input) => {
            input.value = '';
          });
        }
      });
    }

    let musicLibraryOffset = 0;
    let musicLibraryPageSize = 64;
    let musicLibraryTotal = 0;
    let musicLibraryBusy = false;
    let musicScanPollTimer = null;

    function scheduleMusicScanPoll() {
      if (musicScanPollTimer !== null) return;
      musicScanPollTimer = setTimeout(() => {
        musicScanPollTimer = null;
        if (document.getElementById('music-library-list')) {
          musicLibraryPost('snapshot', null, true);
        }
      }, 1000);
    }

    function musicLibraryPost(action, index, quiet) {
      if (musicLibraryBusy && action === 'status') return Promise.resolve(null);
      const body = new URLSearchParams();
      body.append('action', action);
      body.append('offset', String(musicLibraryOffset));
      if (index !== undefined && index !== null) body.append('index', String(index));
      if (!quiet) musicLibraryBusy = true;
      return fetch('/media/playlist', {
        method: 'POST',
        body,
        cache: 'no-store',
        credentials: 'same-origin',
        referrerPolicy: 'no-referrer',
      }).then((r) => r.json().then((data) => Object.assign({ ok: r.ok }, data)))
        .catch(() => ({ ok: false }))
        .then((reply) => {
          if (reply && reply.scanning) {
            musicLibraryBusy = true;
            scheduleMusicScanPoll();
          } else {
            musicLibraryBusy = false;
          }
          if (reply) renderMusicLibrary(reply, action !== 'status');
          return reply;
        });
    }

    function musicPathBasename(path) {
      if (!path) return '';
      const slash = path.lastIndexOf('/');
      return slash >= 0 ? path.slice(slash + 1) : path;
    }

    function updateMusicPlayerStatus(data) {
      const status = document.getElementById('music-player-status');
      if (status) {
        status.textContent = data.playing
          ? '\u25b6 ' + musicPathBasename(data.playing_path)
          : t('musicStopped');
      }
      const repeat = document.getElementById('music-repeat-button');
      if (repeat) {
        const key = Number(data.repeat) === 1 ? 'musicRepeatOne' : 'musicRepeatList';
        repeat.setAttribute('data-i18n', key);
        repeat.textContent = t(key);
      }
      const up = document.getElementById('music-up-button');
      if (up) up.disabled = !!data.root;
    }

    function renderMusicLibrary(data, includeEntries) {
      updateMusicPlayerStatus(data || {});
      if (!data || data.ok === false) {
        const status = document.getElementById('music-player-status');
        if (status) status.textContent = t('musicActionFailed');
        return;
      }
      const path = document.getElementById('music-browser-path');
      if (path) path.textContent = data.root ? '/' : (data.dir || '/');
      const list = document.getElementById('music-library-list');
      const scanning = !!data.scanning;
      if (scanning && (!Array.isArray(data.dirs) || !Array.isArray(data.tracks))) {
        if (list) {
          list.innerHTML = '';
          const loading = document.createElement('span');
          loading.className = 'hint';
          loading.textContent = t('musicScanning');
          list.appendChild(loading);
        }
        const pagination = document.getElementById('music-pagination');
        if (pagination) pagination.hidden = true;
        scheduleMusicScanPoll();
        return;
      }
      if (!scanning && data.scan_ok === false) {
        if (list) {
          list.innerHTML = '';
          const failed = document.createElement('span');
          failed.className = 'hint';
          failed.textContent = t('musicScanFailed');
          list.appendChild(failed);
        }
        return;
      }
      if (!includeEntries || !Array.isArray(data.dirs) || !Array.isArray(data.tracks)) return;

      musicLibraryOffset = Number(data.offset) || 0;
      musicLibraryPageSize = Number(data.page_size) || 64;
      musicLibraryTotal = Number(data.total) || 0;
      if (!list) return;
      list.innerHTML = '';
      data.dirs.forEach((dir) => {
        const row = document.createElement('div');
        row.className = 'music-entry music-dir';
        const open = document.createElement('button');
        open.type = 'button';
        open.className = 'music-entry-main secondary';
        open.textContent = '\ud83d\udcc1 ' + (dir.name || '(dir)');
        open.addEventListener('click', () => musicLibraryAction('enter', dir.index));
        row.appendChild(open);
        list.appendChild(row);
      });
      data.tracks.forEach((track) => {
        const row = document.createElement('div');
        row.className = 'music-entry' + (track.current ? ' current' : '');
        const play = document.createElement('button');
        play.type = 'button';
        play.className = 'music-entry-main secondary';
        play.textContent = (track.current ? '\u25b6 ' : '\u266b ') + track.name;
        play.addEventListener('click', () => musicLibraryAction('play', track.index));
        row.appendChild(play);
        if (data.favorite_supported) {
          const fav = document.createElement('button');
          fav.type = 'button';
          fav.className = 'music-favorite secondary';
          fav.textContent = track.favorite ? '\u2605' : '\u2606';
          fav.title = track.favorite ? t('radioFavDelete') : t('radioFavAdd');
          fav.addEventListener('click', () => musicLibraryAction('favorite', track.index));
          row.appendChild(fav);
        }
        list.appendChild(row);
      });
      if (!data.dirs.length && !data.tracks.length && !scanning) {
        const empty = document.createElement('span');
        empty.className = 'hint';
        empty.textContent = data.root ? t('musicNoSources') : t('musicNoEntries');
        list.appendChild(empty);
      }
      if (scanning) {
        const loading = document.createElement('span');
        loading.className = 'hint';
        loading.textContent = t('musicScanning') + ' (' + musicLibraryTotal + ')';
        list.appendChild(loading);
        scheduleMusicScanPoll();
      }

      const pagination = document.getElementById('music-pagination');
      if (pagination) {
        pagination.hidden = musicLibraryTotal <= musicLibraryPageSize;
        const buttons = pagination.querySelectorAll('button');
        if (buttons[0]) buttons[0].disabled = musicLibraryOffset === 0;
        if (buttons[1]) buttons[1].disabled = musicLibraryOffset + musicLibraryPageSize >= musicLibraryTotal;
      }
      const pageStatus = document.getElementById('music-page-status');
      if (pageStatus && musicLibraryTotal > 0) {
        const last = Math.min(musicLibraryOffset + musicLibraryPageSize, musicLibraryTotal);
        pageStatus.textContent = (musicLibraryOffset + 1) + '-' + last + ' / ' + musicLibraryTotal;
      }
    }

    function musicLibraryAction(action, index) {
      const navigation = action === 'enter' || action === 'up' || action === 'refresh';
      if (musicLibraryBusy && !navigation) return;
      if (action === 'enter' || action === 'up' || action === 'refresh') musicLibraryOffset = 0;
      musicLibraryPost(action, index, false);
    }

    function musicLibraryPage(direction) {
      const next = musicLibraryOffset + direction * musicLibraryPageSize;
      musicLibraryOffset = Math.max(0, Math.min(next,
        Math.max(0, musicLibraryTotal - musicLibraryPageSize)));
      musicLibraryPost('snapshot', null, false);
    }

    function flashButtonFeedback(button, ok, okKey, failKey) {
      if (!button) return;
      const orig = button.textContent;
      const origI18n = button.getAttribute('data-i18n');
      button.removeAttribute('data-i18n');
      button.textContent = ok ? t(okKey || 'saved') : t(failKey || 'saveFailed');
      button.disabled = true;
      setTimeout(() => {
        button.textContent = orig;
        if (origI18n) button.setAttribute('data-i18n', origI18n);
        button.disabled = false;
      }, 1200);
    }

    function submitFormFromButton(form) {
      const button = form.querySelector('button[type="submit"]');
      if (button) button.disabled = true;
      postAndApply(form).then((reply) => {
        flashButtonFeedback(button, reply && reply.ok);
        if (reply && reply.ok && form.dataset.reloadOnSave === '1') {
          setTimeout(() => window.location.reload(), 300);
        }
      });
    }

    function submitEqSlider(slider) {
      syncEqValue(slider);
      postAndApply(slider.form);
    }

    window.applyAdcEqPreset = function (button) {
      const form = button ? button.form : null;
      if (!form) return;
      form.querySelectorAll('input[type="hidden"][name]').forEach((preset) => {
        document.querySelectorAll('input.eq-slider[name="' + preset.name + '"]').forEach((slider) => {
          slider.value = preset.value;
          syncEqValue(slider);
        });
      });
      button.disabled = true;
      postAndApply(form).then((reply) => flashButtonFeedback(button, reply && reply.ok));
    };

    function syncAutoSliderValue(slider) {
      const box = slider.parentElement.querySelector('.eq-value');
      if (box) box.value = slider.value;
    }

    function submitAutoSlider(slider) {
      syncAutoSliderValue(slider);
      postAndApply(slider.form);
    }

    function submitSwitch(input) {
      postAndApply(input.form);
    }

    // Post a form plus one extra action flag (e.g. radio_play=1). Needed
    // because postForm serialises FormData without the clicked submit
    // button, so action buttons are type="button" with an onclick instead.
    function submitFormAction(button, name, okKey, failKey) {
      const form = button ? button.form : null;
      if (!form) return;
      const flag = document.createElement('input');
      flag.type = 'hidden';
      flag.name = name;
      flag.value = '1';
      form.appendChild(flag);
      button.disabled = true;
      postAndApply(form).then((reply) => {
        flag.remove();
        button.disabled = false;
        flashButtonFeedback(button, reply && reply.ok, okKey, failKey);
        if (reply && reply.ok && form.dataset.reloadOnSave === '1') {
          setTimeout(() => window.location.reload(), 300);
        }
      });
    }

    async function scanWifi() {
      const status = document.getElementById('scan-status');
      const select = document.getElementById('wifi-ssid-select');
      status.textContent = t('scanning');
      try {
        const res = await fetch('/scan', {cache: 'no-store'});
        if (!res.ok) throw new Error('scan failed');
        const items = await res.json();
        renderWifiOptions(select, items);
        status.textContent = items.length ? (t('foundPrefix') + items.length + t('foundSuffix')) : t('noneFound');
      } catch (e) {
        status.textContent = t('scanFailed');
      }
    }

    function renderWifiOptions(select, items) {
      let foundSelected = false;
      const selected = select ? select.value : '';
      if (select) {
        select.innerHTML = '';
        const placeholder = document.createElement('option');
        placeholder.value = '';
        placeholder.textContent = t('selectWifi');
        placeholder.setAttribute('data-i18n', 'selectWifi');
        select.appendChild(placeholder);
      }
      if (!Array.isArray(items)) {
        if (select) select.value = selected;
        return;
      }
      items.forEach((item) => {
        if (!item || !item.ssid) return;
        const label = item.label || item.ssid;
        if (select) {
          const opt = document.createElement('option');
          opt.value = item.ssid;
          opt.textContent = label;
          select.appendChild(opt);
          if (item.ssid === selected) foundSelected = true;
        }
      });
      if (select && selected && !foundSelected) {
        const opt = document.createElement('option');
        opt.value = selected;
        opt.textContent = selected;
        select.appendChild(opt);
      }
      if (select) select.value = selected;
    }

    function platformServerHost(value) {
      const host = String(value || '').trim();
      const withReportingPort = host.match(/^(.+):(\d+)$/);
      return withReportingPort ? withReportingPort[1] : host;
    }

    function setServerListStatus(key, count) {
      const status = document.getElementById('nrl-server-status');
      if (!status) return;
      status.removeAttribute('data-i18n');
      status.textContent = t(key).replace('{count}', String(count || 0));
    }

    function findNrlServerOption(host) {
      const list = document.getElementById('nrl-server-options');
      if (!list) return null;
      const value = String(host || '').trim();
      if (!value) return null;
      return Array.prototype.find.call(list.options, (opt) => opt.value === value) || null;
    }

    function syncNrlServerPort() {
      const host = document.getElementById('nrl-server-host');
      const port = document.getElementById('nrl-server-port');
      if (!host || !port) return;
      const selected = findNrlServerOption(host.value);
      if (selected && selected.dataset.port) port.value = selected.dataset.port;
      updateNrlServerCurrent();
    }

    function updateNrlServerCurrent() {
      const host = document.getElementById('nrl-server-host');
      const current = document.getElementById('nrl-server-current');
      if (!host || !current) return;
      const value = String(host.value || '').trim();
      if (!value) {
        current.textContent = t('currentNrlServer') + ': ---';
        return;
      }
      const option = findNrlServerOption(value);
      const name = option && option.dataset.name ? option.dataset.name : '';
      current.textContent = t('currentNrlServer') + ': ' +
                            (name ? name + ' (' + value + ')' : value);
    }

    function renderNrlServers(payload, currentHost, currentPort) {
        const list = document.getElementById('nrl-server-options');
        const servers = (payload && payload.data && Array.isArray(payload.data.items)
          ? payload.data.items
          : payload && Array.isArray(payload.data) ? payload.data : [])
          .filter((item) => item && !item.hidden && platformServerHost(item.host) &&
                            Number(item.port) > 0 && Number(item.port) <= 65535)
          .sort((a, b) => (Number(a.sort_order) || 0) - (Number(b.sort_order) || 0));
        if (!servers.length) throw new Error('empty server list');
        list.innerHTML = '';
        servers.forEach((server) => {
          const host = platformServerHost(server.host);
          const serverPort = String(server.port);
          const option = document.createElement('option');
          option.value = host;
          option.dataset.port = serverPort;
          option.dataset.name = String(server.name || '');
          option.label = String(server.name || host) + ' · ' + host + ':' + serverPort +
                         ' · ' + String(Number(server.online) || 0) + '/' +
                         String(Number(server.total) || 0);
          list.appendChild(option);
        });
        if (currentHost && !findNrlServerOption(currentHost)) {
          const option = document.createElement('option');
          option.value = currentHost;
          option.dataset.port = currentPort;
          option.label = currentHost + ':' + currentPort;
          list.insertBefore(option, list.firstChild);
        }
        updateNrlServerCurrent();
        if (nrlServerSuggestVisible()) renderNrlServerSuggest();
        return servers.length;
    }

    let nrlSuggestActive = -1;

    function nrlServerSuggestRows() {
      const suggest = document.getElementById('nrl-server-suggest');
      if (!suggest) return [];
      return Array.prototype.slice.call(suggest.children);
    }

    function nrlServerSuggestVisible() {
      const suggest = document.getElementById('nrl-server-suggest');
      return !!suggest && !suggest.hidden;
    }

    function setActiveNrlServerRow(index) {
      const rows = nrlServerSuggestRows();
      nrlSuggestActive = index;
      rows.forEach((row, i) => {
        row.classList.toggle('active', i === index);
        if (i === index && row.scrollIntoView) row.scrollIntoView({block: 'nearest'});
      });
    }

    function renderNrlServerSuggest() {
      const suggest = document.getElementById('nrl-server-suggest');
      const host = document.getElementById('nrl-server-host');
      if (!suggest || !host) return;
      const typed = String(host.value || '').trim().toLowerCase();
      const list = document.getElementById('nrl-server-options');
      const options = list ? Array.prototype.filter.call(list.options, (opt) =>
        !typed || opt.value.toLowerCase().includes(typed) ||
        String(opt.label || '').toLowerCase().includes(typed)) : [];
      suggest.innerHTML = '';
      options.forEach((opt) => {
        const row = document.createElement('div');
        row.className = 'nrl-server-row';
        row.setAttribute('role', 'option');
        row.dataset.value = opt.value;
        row.dataset.port = opt.dataset.port || '';
        row.textContent = opt.label || opt.value;
        suggest.appendChild(row);
      });
      nrlSuggestActive = -1;
    }

    function openNrlServerSuggest() {
      const suggest = document.getElementById('nrl-server-suggest');
      if (!suggest) return;
      renderNrlServerSuggest();
      suggest.hidden = false;
    }

    function closeNrlServerSuggest() {
      const suggest = document.getElementById('nrl-server-suggest');
      if (!suggest) return;
      suggest.hidden = true;
      nrlSuggestActive = -1;
    }

    function pickNrlServerRow(row) {
      const host = document.getElementById('nrl-server-host');
      if (!host || !row) return;
      host.value = row.dataset.value || '';
      if (row.dataset.port) {
        const port = document.getElementById('nrl-server-port');
        if (port) port.value = row.dataset.port;
      }
      closeNrlServerSuggest();
      updateNrlServerCurrent();
      if (host.form) submitFormFromButton(host.form);
    }

    async function loadNrlServers() {
      const host = document.getElementById('nrl-server-host');
      const port = document.getElementById('nrl-server-port');
      if (!host || !port) return;
      const currentHost = host.value;
      const currentPort = port.value;
      let lastAutoSaved = null;
      host.addEventListener('change', () => {
        syncNrlServerPort();
        const value = String(host.value || '').trim();
        // Auto-save only when an entry is picked from the server list; a
        // manually typed custom host/IP is saved via the Save button.
        if (value && findNrlServerOption(value) && value !== lastAutoSaved && host.form) {
          lastAutoSaved = value;
          submitFormFromButton(host.form);
        }
      });
      host.addEventListener('focus', openNrlServerSuggest);
      host.addEventListener('input', () => {
        renderNrlServerSuggest();
        const suggest = document.getElementById('nrl-server-suggest');
        if (suggest) suggest.hidden = false;
      });
      host.addEventListener('blur', () => setTimeout(closeNrlServerSuggest, 120));
      host.addEventListener('keydown', (event) => {
        if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
          event.preventDefault();
          if (!nrlServerSuggestVisible()) {
            openNrlServerSuggest();
            return;
          }
          const rows = nrlServerSuggestRows();
          if (!rows.length) return;
          const delta = event.key === 'ArrowDown' ? 1 : -1;
          setActiveNrlServerRow((nrlSuggestActive + delta + rows.length) % rows.length);
        } else if (event.key === 'Enter') {
          if (nrlServerSuggestVisible() && nrlSuggestActive >= 0) {
            event.preventDefault();
            pickNrlServerRow(nrlServerSuggestRows()[nrlSuggestActive]);
          }
        } else if (event.key === 'Escape') {
          closeNrlServerSuggest();
        }
      });
      const suggest = document.getElementById('nrl-server-suggest');
      if (suggest) {
        suggest.addEventListener('click', (event) => {
          const row = event.target.closest ? event.target.closest('.nrl-server-row') : null;
          if (row) pickNrlServerRow(row);
        });
        suggest.addEventListener('mouseover', (event) => {
          const row = event.target.closest ? event.target.closest('.nrl-server-row') : null;
          if (!row) return;
          const index = nrlServerSuggestRows().indexOf(row);
          if (index >= 0) setActiveNrlServerRow(index);
        });
      }
      const current = document.getElementById('nrl-server-current');
      if (current) {
        const openPicker = () => {
          host.focus();
          openNrlServerSuggest();
        };
        current.addEventListener('click', openPicker);
        current.addEventListener('keydown', (event) => {
          if (event.key === 'Enter' || event.key === ' ') openPicker();
        });
      }
      let cachedCount = 0;
      try {
        const cached = await fetch('/nrl/servers', {cache: 'no-store'});
        if (cached.ok) {
          cachedCount = renderNrlServers(await cached.json(), currentHost, currentPort);
          setServerListStatus('serverListCached', cachedCount);
        }
      } catch (_) {}
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 12000);
      try {
        // The platform endpoint does not expose browser CORS headers. Ask the
        // board to perform the HTTPS request against its current NRL hostname;
        // the response is atomically cached in LittleFS for LCD/Web reuse.
        const response = await fetch('/nrl/servers/refresh', {
          cache: 'no-store', credentials: 'same-origin', referrerPolicy: 'no-referrer',
          signal: controller.signal,
        });
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const payload = await response.json();
        const count = renderNrlServers(payload, currentHost, currentPort);
        setServerListStatus('serverListLoaded', count);
      } catch (error) {
        if (!cachedCount) setServerListStatus('serverListFailed', 0);
      } finally {
        clearTimeout(timer);
      }
    }

    function enforceUtf8ByteLimit(input) {
      const max = Number(input.dataset.maxUtf8Bytes) || 0;
      if (!max || typeof TextEncoder === 'undefined') return;
      const encoder = new TextEncoder();
      while (input.value && encoder.encode(input.value).length > max) {
        input.value = input.value.slice(0, -1);
      }
    }

    function initPortal() {
      applyLanguage(currentLang());
      syncDhcpFields();
      loadNrlServers();
      if (window.RADIO_FAVS) {
        renderRadioFavs(window.RADIO_FAVS, window.RADIO_FAV_CUR);
        applyLanguage(currentLang()); // translate the freshly rendered rows
      }
      if (document.getElementById('music-player-panel')) {
        musicLibraryPost('snapshot', null, false);
        setInterval(() => musicLibraryPost('status', null, true), 3000);
      }
      const expert = document.getElementById('audio-expert-mode');
      if (expert) {
        expert.checked = false;
        toggleAudioExpert(false);
      }
      document.querySelectorAll('[data-max-utf8-bytes]').forEach((input) => {
        enforceUtf8ByteLimit(input);
        input.addEventListener('input', () => enforceUtf8ByteLimit(input));
      });
      document.querySelectorAll('form').forEach((form) => {
        // Intercept explicit Save-button submits: send via AJAX so the page
        // never reloads, then reflect the server's echoed values back into the
        // inputs and flash the button.
        form.addEventListener('submit', (e) => {
          e.preventDefault();
          submitFormFromButton(form);
        });
      });
      document.querySelectorAll('input[name="lang"]').forEach((r) => {
        r.addEventListener('change', function () {
          if (this.checked) {
            applyLanguage(this.value);
            updateNrlServerCurrent();
          }
        });
      });
    }

    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', initPortal);
    } else {
      initPortal();
    }
