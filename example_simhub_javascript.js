// 1. Establish core state
let isGameRunning = $prop('DataCorePlugin.GameRunning');

// 2. Declare default standby states
let shiftLight = 0;
let engineLight = 1;
let speed = 0;
let tach = 0;
let temp = 0;
let fuel = 0;
let boost = 0; // New data field
let now = new Date();
let hours = now.getHours().toString().padStart(2, '0');
let minutes = now.getMinutes().toString().padStart(2, '0');
let lcd1 = "     " + hours + ":" + minutes; 
let lcd2 = "";

// 3. Populate live telemetry
if (isGameRunning) {
    engineLight = $prop('EngineIgnitionOn') ? 1 : 0;
    speed = Math.round($prop('SpeedMph')) || 0;
    tach = Math.round($prop('Rpms')) || 0;
    temp = Math.round($prop('WaterTemperature')) || 0;
    fuel = Math.round($prop('DataCorePlugin.Computed.Fuel_Percent')) || 0;
    
    // Safely capture boost metrics (fallback to 0 if N/A or atmospheric)
    boost = Math.round($prop('TurboBoost')) || Math.round($prop('Boost')) || 0;
    if (boost < 0) boost = 0; // Prevent reverse needle bounce from vacuum pressures
    
    // Shift Light Logic
    if ($prop('CarSettings_RPMShiftLight2') == 1) {
        shiftLight = 2;
    } else if ($prop('CarSettings_RPMShiftLight1') == 1) {
        shiftLight = 1;
    }
    
    // LCD Line 1: Gear & FPS
    let drive = $prop('Gear') || 'N';
    let fps = Math.round($prop('AfterburnerPlugin.Framerate_FPS') || 0).toString();
    let topSpacer = 16 - drive.toString().length - fps.length;
    lcd1 = drive + " ".repeat(Math.max(0, topSpacer)) + fps;
    
    // LCD Line 2: Lap Time & Distance Remaining
    let distRaw = $prop('GameRawData.DistanceToFinish') || 0;
    let distFormatted = (distRaw / 1000).toFixed(1) + "k";
    let lapTime = (distRaw <= 0) ?  $prop('LastLapTime').toString('mm\\:ss') : $prop('CurrentLapTime').toString('mm\\:ss');
    let cleanLap = (lapTime || "00:00").substring(0, 5).padEnd(5, ' ');
    
    let bottomSpacer = 16 - cleanLap.length - distFormatted.length;
    lcd2 = cleanLap + " ".repeat(Math.max(0, bottomSpacer)) + distFormatted;
}

// 4. Final safety pass (strip out semicolons to preserve token processing boundaries)
lcd1 = lcd1.replace(/;/g, '');
lcd2 = lcd2.replace(/;/g, '');

// 5. Construct modular tokenized output
let output = "";
output += "S=" + shiftLight + ";";
output += "E=" + engineLight + ";";
output += "V=" + speed + ";";
output += "R=" + tach + ";";
output += "T=" + temp + ";";
output += "F=" + fuel + ";";
output += "B=" + boost + ";"; // Append boost data
output += "L1=" + lcd1 + ";";
output += "L2=" + lcd2 + ";";

return output + "\n";
