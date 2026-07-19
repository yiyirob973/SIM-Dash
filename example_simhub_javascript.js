// 1. Establish core state
let isGameRunning = $prop('DataCorePlugin.GameRunning');

// 2. Declare default standby states
let shiftLight = 0;
let engineLight = 1;
let speed = 0;
let tach = 0;
let temp = 0;
let fuel = 0;
let now = new Date();
let hours = now.getHours().toString().padStart(2, '0');
let minutes = now.getMinutes().toString().padStart(2, '0');
// Use SimHub's built-in time properties for the clock to avoid Date() errors
let lcd1 = "     " + hours + ":" + minutes; 
let lcd2 = "";

// 3. Populate live telemetry
if (isGameRunning) {
    
    engineLight = $prop('EngineIgnitionOn') ? 1 : 0;
    speed = Math.round($prop('SpeedMph')) || 0;
    tach = Math.round($prop('Rpms')) || 0;
    temp = Math.round($prop('WaterTemperature')) || 0;
    fuel = Math.round($prop('DataCorePlugin.Computed.Fuel_Percent')) || 0;
    
    // Shift Light Logic
    if ($prop('CarSettings_RPMShiftLight2') == 1) {
        shiftLight = 2;
    } else if ($prop('CarSettings_RPMShiftLight1') == 1) {
        shiftLight = 1;
    }
    
    // LCD Line 1: Gear (Left) & FPS (Right)
    let drive = $prop('Gear') || 'N';
    let fps = Math.round($prop('AfterburnerPlugin.Framerate_FPS') || 0).toString();
    let topSpacer = 16 - drive.toString().length - fps.length;
    lcd1 = drive + " ".repeat(Math.max(0, topSpacer)) + fps;
    
    // LCD Line 2: Lap Time (Left) & Distance (Right)
    let distRaw = $prop('GameRawData.DistanceToFinish') || 0;
    let distFormatted = (distRaw / 1000).toFixed(1) + "k";
    
    // Safely get lap time as a string without forcing .toString('mm:ss')
    let lapTime = (distRaw <= 0) ?  $prop('LastLapTime').toString('mm\\:ss') : $prop('CurrentLapTime').toString('mm\\:ss');
    let cleanLap = (lapTime || "00:00").substring(0, 5).padEnd(5, ' ');
    
    let bottomSpacer = 16 - cleanLap.length - distFormatted.length;
    lcd2 = cleanLap + " ".repeat(Math.max(0, bottomSpacer)) + distFormatted;
}

// 4. Final safety pass
lcd1 = lcd1.replace(/,/g, '');
lcd2 = lcd2.replace(/,/g, '');

return shiftLight + "," + engineLight + "," + speed + "," + tach + "," + temp + "," + fuel + "," + lcd1 + "," + lcd2 + "\n";
