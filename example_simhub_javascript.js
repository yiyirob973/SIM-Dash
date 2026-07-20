// 1. Establish core state
const isGameRunning = $prop('DataCorePlugin.GameRunning');

// helpers
const get = (k) => $prop(k);
const roundProp = (k) => Math.round(get(k) || 0);
const safeStr = (v) => String(v == null ? '' : v);
const sanitizeLine = (s) => safeStr(s).replace(/;/g, '').substring(0, 16).padEnd(16, ' ');

// 2. Defaults / standby placeholders
let shiftLight = 0;
let engineLight = 0;
let speed = 0;
let tach = 0;
let temp = 0;
let fuel = 0;
let boost = 0;

let lcd1 = ''.padEnd(16, ' ');
let lcd2 = ''.padEnd(16, ' ');

// 3. Populate live telemetry or show standby clock when not running
if (isGameRunning) {
  // preserve original polarity as in source
  engineLight = get('EngineIgnitionOn') ? 0 : 1;
  speed = roundProp('SpeedMph');
  tach = roundProp('Rpms');
  temp = roundProp('WaterTemperature');
  fuel = roundProp('DataCorePlugin.Computed.Fuel_Percent');

  // boost fallback chain
  boost = roundProp('TurboBoost') || roundProp('Boost') || 0;
  if (boost < 0) boost = 0;

  // shift light
  if (get('CarSettings_RPMShiftLight2') == 1) shiftLight = 2;
  else if (get('CarSettings_RPMShiftLight1') == 1) shiftLight = 1;

  // LCD Line 1: Gear & FPS
  const drive = get('Gear') || 'N';
  const fps = roundProp('AfterburnerPlugin.Framerate_FPS').toString();
  const topSpacer = Math.max(0, 16 - String(drive).length - fps.length);
  lcd1 = `${drive}${' '.repeat(topSpacer)}${fps}`;

  // LCD Line 2: Lap Time & Distance Remaining
  const distRaw = get('GameRawData.DistanceToFinish') || 0;
  const distFormatted = `${(distRaw / 1000).toFixed(1)}k`;

  // Use only formatted lap time props
  const lapFormatted = get('CurrentLapTimeFormatted') || get('LastLapTimeFormatted') || '00:00';
  const cleanLap = String(lapFormatted).substring(0, 5).padEnd(5, ' ');

  const bottomSpacer = Math.max(0, 16 - cleanLap.length - distFormatted.length);
  lcd2 = `${cleanLap}${' '.repeat(bottomSpacer)}${distFormatted}`;
} else {
  // Only compute Date/standby when not running (micro-optimization)
  const now = new Date();
  const standby = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
  lcd1 = `     ${standby}`.substring(0, 16).padEnd(16, ' ');
  lcd2 = ''.padEnd(16, ' ');
}

// 4. Sanitize & enforce 16 chars
lcd1 = sanitizeLine(lcd1);
lcd2 = sanitizeLine(lcd2);

// 5. Construct modular tokenized output efficiently
const parts = [
  `S=${shiftLight}`,
  `E=${engineLight}`,
  `V=${speed}`,
  `R=${tach}`,
  `T=${temp}`,
  `F=${fuel}`,
  `B=${boost}`,
  `L1=${lcd1}`,
  `L2=${lcd2}`
];

return parts.join(';') + ';' + '\n';
