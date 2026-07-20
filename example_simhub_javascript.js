// 1. Establish core state
let isGameRunning = $prop('DataCorePlugin.GameRunning');[span_0](start_span)[span_0](end_span)

// 2. Declare default standby states
let shiftLight = 0;[span_1](start_span)[span_1](end_span)
let engineLight = 1;[span_2](start_span)[span_2](end_span)
let speed = 0;[span_3](start_span)[span_3](end_span)
let tach = 0;[span_4](start_span)[span_4](end_span)
let temp = 0;[span_5](start_span)[span_5](end_span)
let fuel = 0;[span_6](start_span)[span_6](end_span)
let now = new Date();[span_7](start_span)[span_7](end_span)
let hours = now.getHours().toString().padStart(2, '0');[span_8](start_span)[span_8](end_span)
let minutes = now.getMinutes().toString().padStart(2, '0');[span_9](start_span)[span_9](end_span)
// Use SimHub's built-in time properties for the clock to avoid Date() errors
let lcd1 = "     " + hours + ":" + minutes;[span_10](start_span)[span_10](end_span)
let lcd2 = "";[span_11](start_span)[span_11](end_span)

// 3. Populate live telemetry
if (isGameRunning) {[span_12](start_span)[span_12](end_span)
    
    engineLight = $prop('EngineIgnitionOn') ? 1 : 0;[span_13](start_span)[span_13](end_span)
    speed = Math.round($prop('SpeedMph')) || 0;[span_14](start_span)[span_14](end_span)
    tach = Math.round($prop('Rpms')) || 0;[span_15](start_span)[span_15](end_span)
    temp = Math.round($prop('WaterTemperature')) || 0;[span_16](start_span)[span_16](end_span)
    fuel = Math.round($prop('DataCorePlugin.Computed.Fuel_Percent')) || 0;[span_17](start_span)[span_17](end_span)
    
    // Shift Light Logic
    if ($prop('CarSettings_RPMShiftLight2') == 1) {[span_18](start_span)[span_18](end_span)
        shiftLight = 2;[span_19](start_span)[span_19](end_span)
    } else if ($prop('CarSettings_RPMShiftLight1') == 1) {[span_20](start_span)[span_20](end_span)
        shiftLight = 1;[span_21](start_span)[span_21](end_span)
    }
    
    // LCD Line 1: Gear (Left) & FPS (Right)
    let drive = $prop('Gear') || 'N';[span_22](start_span)[span_22](end_span)
    let fps = Math.round($prop('AfterburnerPlugin.Framerate_FPS') || 0).toString();[span_23](start_span)[span_23](end_span)
    let topSpacer = 16 - drive.toString().length - fps.length;[span_24](start_span)[span_24](end_span)
    lcd1 = drive + " ".repeat(Math.max(0, topSpacer)) + fps;[span_25](start_span)[span_25](end_span)
    
    // LCD Line 2: Lap Time (Left) & Distance (Right)
    let distRaw = $prop('GameRawData.DistanceToFinish') || 0;[span_26](start_span)[span_26](end_span)
    let distFormatted = (distRaw / 1000).toFixed(1) + "k";[span_27](start_span)[span_27](end_span)
    
    // Safely get lap time as a string without forcing .toString('mm:ss')
    let lapTime = (distRaw <= 0) ?  $prop('LastLapTime').toString('mm\\:ss') : $prop('CurrentLapTime').toString('mm\\:ss');[span_28](start_span)[span_28](end_span)
    let cleanLap = (lapTime || "00:00").substring(0, 5).padEnd(5, ' ');[span_29](start_span)[span_29](end_span)
    
    let bottomSpacer = 16 - cleanLap.length - distFormatted.length;[span_30](start_span)[span_30](end_span)
    lcd2 = cleanLap + " ".repeat(Math.max(0, bottomSpacer)) + distFormatted;[span_31](start_span)[span_31](end_span)
}

// 4. Final safety pass (strip out semicolons to protect token formatting)
lcd1 = lcd1.replace(/;/g, '');
lcd2 = lcd2.replace(/;/g, '');

// 5. Construct modular semicolon-delimited key-value string
let output = "";
output += "S=" + shiftLight + ";";
output += "E=" + engineLight + ";";
output += "V=" + speed + ";";
output += "R=" + tach + ";";
output += "T=" + temp + ";";
output += "F=" + fuel + ";";
output += "L1=" + lcd1 + ";";
output += "L2=" + lcd2 + ";";

return output + "\n";
